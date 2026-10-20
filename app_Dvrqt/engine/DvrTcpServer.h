#ifndef DVRTCPSERVER_H
#define DVRTCPSERVER_H

// =====================================================================
// 第2周：TCP 命令服务器（文本行JSON控制 + 原始字节流文件下载）
// 客户端命令（一行一条，'\n' 结尾）：
//   STATUS              → {"ok":true,"state":"IDLE",...}
//   LIST [dir]          → {"ok":true,"files":[{"spec":"normal/x.mp4",...},...]}
//   GET dir/name.mp4    → {"ok":true,"size":N} + N字节原始数据 + {"done":true,...}
//   DEL dir/name.mp4    → {"ok":true}
//   START / STOP        → {"ok":true,"state":"..."}
// 仅限局域网/USB调试使用（无鉴权）
// =====================================================================

#include <QTcpServer>
#include <QTcpSocket>
#include <QHash>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>
#include <QNetworkInterface>
#include <functional>
#include "StorageManager.h"

class DvrTcpServer : public QTcpServer {
public:
    using StateFn  = std::function<QString()>;
    using ActionFn = std::function<void()>;

    explicit DvrTcpServer(const StorageManager& storage) : storage_(storage) {}

    void setStateProvider(StateFn fn) { stateFn_ = std::move(fn); }
    void setActions(ActionFn start, ActionFn stop) {
        startFn_ = std::move(start);
        stopFn_  = std::move(stop);
    }

    QString url() const { return QString("tcp://%1:%2").arg(ip_).arg(serverPort()); }

    bool start(quint16 port = 9000) {
        connect(this, &QTcpServer::newConnection, this, [this]{ acceptClients(); });
        if (!listen(QHostAddress::AnyIPv4, port)) {
            qWarning() << "[tcp] listen FAILED:" << errorString();
            return false;
        }
        ip_ = firstLanIp();
        qWarning() << "[tcp] listening on" << ip_ << port;
        return true;
    }

private:
    // ---------- 每连接状态（QFile不可拷贝，用指针+cleanup统一释放） ----------
    struct Client {
        QFile*  file      = nullptr;   // 下载中的文件（非空 = 正在发送）
        qint64  remaining = 0;
        qint64  sent      = 0;
        bool    sending   = false;     // true = 数据信道占用中
    };

    // ---------- 连接管理 ----------
    void acceptClients() {
        while (QTcpSocket* s = nextPendingConnection()) {
            s->setSocketOption(QAbstractSocket::LowDelayOption, 1);  // 命令低延迟(禁Nagle)
            clients_.insert(s, new Client);
            connect(s, &QTcpSocket::readyRead,    this, [this, s]{ onReadyRead(s); });
            connect(s, &QTcpSocket::bytesWritten, this, [this, s](qint64){ pumpIfSending(s); });
            connect(s, &QTcpSocket::disconnected, this, [this, s]{ cleanup(s); });
            sendJson(s, QJsonObject{
                {"welcome", "DVR week2"},
                {"cmds", "STATUS | LIST [dir] | GET dir/name | DEL dir/name | START | STOP"}});
        }
    }

    void cleanup(QTcpSocket* s) {
        if (Client* c = clients_.take(s)) {
            delete c->file;
            delete c;
        }
        s->deleteLater();
    }

    // ---------- 命令接收：readLine按'\n'切消息，粘包/拆包由QIODevice缓冲自动处理 ----------
    void onReadyRead(QTcpSocket* s) {
        Client* c = clients_.value(s, nullptr);
        if (!c || c->sending) return;                 // 发送数据中，不解析新命令
        while (s->canReadLine()) {
            const QByteArray line = s->readLine().trimmed();
            if (line.size() > 4096) { s->disconnectFromHost(); return; }  // 超长行防滥用
            if (!line.isEmpty()) handleLine(s, line);
            c = clients_.value(s, nullptr);
            if (!c) return;                           // 连接已被清理
            if (c->sending) return;                   // GET 刚启动了文件发送
        }
    }

    void handleLine(QTcpSocket* s, const QByteArray& line) {
        const int sp = line.indexOf(' ');
        const QByteArray cmd = (sp < 0) ? line : line.left(sp);
        const QByteArray arg = (sp < 0) ? QByteArray() : line.mid(sp + 1);

        if (cmd == "STATUS") {
            sendJson(s, jsonStatus());
        } else if (cmd == "LIST") {
            sendJson(s, jsonList(arg.isEmpty() ? QStringLiteral("normal")
                                               : QString::fromUtf8(arg)));
        } else if (cmd == "GET") {
            startSend(s, QString::fromUtf8(arg));
        } else if (cmd == "DEL") {
            handleDelete(s, arg);
        } else if (cmd == "START") {
            if (startFn_) startFn_();
            sendJson(s, QJsonObject{{"ok", true}, {"state", currentState()}});
        } else if (cmd == "STOP") {
            if (stopFn_) stopFn_();
            sendJson(s, QJsonObject{{"ok", true}, {"state", currentState()}});
        } else {
            sendJson(s, QJsonObject{{"ok", false}, {"err", "unknown command"}});
        }
    }

    void handleDelete(QTcpSocket* s, const QByteArray& arg) {
        QString dir, name;
        if (!parseSpec(QString::fromUtf8(arg), &dir, &name)) {
            sendJson(s, QJsonObject{{"ok", false}, {"err", "bad spec"}});
            return;
        }
        const bool ok = storage_.removeFile(dir, name);
        sendJson(s, QJsonObject{
            {"ok", ok},
            {"err", ok ? QString() : QStringLiteral("not found")}});
    }

    // ---------- 文件发送（背压泵） ----------
    void startSend(QTcpSocket* s, const QString& spec) {
        QString dir, name;
        if (!parseSpec(spec, &dir, &name)) {
            sendJson(s, QJsonObject{{"ok", false}, {"err", "bad spec"}});
            return;
        }
        Client* c = clients_.value(s, nullptr);
        if (!c) return;
        const QString path = storage_.pathFor(dir, name);   // canonical校验，防目录穿越
        QFile* f = new QFile(path);
        if (path.isEmpty() || !f->open(QIODevice::ReadOnly)) {
            delete f;
            sendJson(s, QJsonObject{{"ok", false}, {"err", "not found"}});
            return;
        }
        c->file      = f;
        c->remaining = f->size();
        c->sent      = 0;
        c->sending   = true;
        // ① 长度声明：接收方按 size 精确读取原始字节
        sendJson(s, QJsonObject{{"ok", true}, {"name", name}, {"size", double(c->remaining)}});
        pump(s);                                            // ② 立即开始填充发送缓冲
    }

    // 背压泵：发送缓冲 <2MB 才继续读盘；填满则退出，等 bytesWritten 再次驱动
    void pump(QTcpSocket* s) {
        Client* c = clients_.value(s, nullptr);
        if (!c || !c->sending || !c->file) return;
        while (c->file->isOpen() && s->bytesToWrite() < (2 << 20)) {
            const QByteArray chunk = c->file->read(256 * 1024);
            if (chunk.isEmpty()) {                          // EOF → 传输完成
                delete c->file;
                c->file    = nullptr;
                c->sending = false;
                sendJson(s, QJsonObject{{"done", true}, {"bytes", double(c->sent)}});
                return;
            }
            s->write(chunk);
            c->sent += chunk.size();
        }
    }

    void pumpIfSending(QTcpSocket* s) { pump(s); }

    // ---------- 规格解析与安全校验 ----------
    // spec 形如 "normal/xxx.mp4"；文件名白名单 [A-Za-z0-9_.-]，防路径穿越
    static bool parseSpec(const QString& spec, QString* dir, QString* name) {
        static const QRegularExpression safe(QStringLiteral("^[A-Za-z0-9_.\\-]+$"));
        const QStringList p = spec.split('/');
        if (p.size() != 2) return false;
        if (p[0] != "normal" && p[0] != "event") return false;
        if (!safe.match(p[1]).hasMatch()) return false;
        *dir = p[0];
        *name = p[1];
        return true;
    }

    // ---------- JSON构造与工具 ----------
    static void sendJson(QTcpSocket* s, const QJsonObject& o) {
        s->write(QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n');
    }

    QString currentState() const { return stateFn_ ? stateFn_() : QString(); }

    QJsonObject jsonStatus() const {
        return QJsonObject{
            {"state",       currentState()},
            {"freeMB",      double(storage_.freeMb())},
            {"normalCount", double(storage_.listFiles("normal").size())},
            {"eventCount",  double(storage_.listFiles("event").size())}};
    }

    QJsonObject jsonList(const QString& dir) const {
        QJsonArray arr;
        if (dir == "normal" || dir == "event") {
            for (const QFileInfo& fi : storage_.listFiles(dir)) {
                arr.append(QJsonObject{
                    {"name",  fi.fileName()},
                    {"spec",  dir + "/" + fi.fileName()},
                    {"size",  double(fi.size())},
                    {"mtime", fi.lastModified().toString(Qt::ISODate)}});
            }
        }
        return QJsonObject{{"dir", dir}, {"count", double(arr.size())}, {"files", arr}};
    }

    static QString firstLanIp() {
        QString fallback;
        const auto ifaces = QNetworkInterface::allInterfaces();
        for (const QNetworkInterface& i : ifaces) {
            const auto fl = i.flags();
            if (!fl.testFlag(QNetworkInterface::IsUp) || fl.testFlag(QNetworkInterface::IsLoopBack)) continue;
            const QString name = i.name();
            if (name.startsWith("rmnet") || name.startsWith("ccmni") || name.startsWith("tun")
                || name.startsWith("tap") || name.startsWith("p2p") || name.startsWith("swlan")) continue;
            for (const QNetworkAddressEntry& e : i.addressEntries())
                if (e.ip().protocol() == QAbstractSocket::IPv4Protocol) {
                    if (name.startsWith("wlan") || name.startsWith("eth"))
                        return e.ip().toString();
                    if (fallback.isEmpty()) fallback = e.ip().toString();
                }
        }
        return fallback.isEmpty() ? QStringLiteral("0.0.0.0") : fallback;
    }

    // ---------- 成员 ----------
    QHash<QTcpSocket*, Client*> clients_;
    const StorageManager& storage_;
    StateFn  stateFn_;
    ActionFn startFn_;
    ActionFn stopFn_;
    QString  ip_;
};

#endif // DVRTCPSERVER_H
