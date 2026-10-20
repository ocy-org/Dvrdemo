#ifndef DVRPROTOSERVER_H
#define DVRPROTOSERVER_H

// =====================================================================
// DvrProtoServer.h  —— 车机二进制协议服务器（端口9001，控制面）
// CMD_G=0x06(测试钩子:注入G事件) EVT_ALARM=0x84(告警推送)
// 特性：CRC校验、失步重同步、事件推送、ACC熄火消抖、心跳超时清理
// =====================================================================

#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>
#include <QDateTime>
#include <QNetworkInterface>
#include <functional>
#include "StorageManager.h"

class DvrProtoServer : public QTcpServer {
public:
    enum Type : quint8 {
        CMD_PING   = 0x01,
        CMD_ACC_ON = 0x02,
        CMD_ACC_OFF= 0x03,
        CMD_LOCK   = 0x04,
        CMD_STATUS = 0x05,
        CMD_G      = 0x06,   // 模拟G事件（真实G-sensor由本机产生）
        EVT_STATE   = 0x81,
        EVT_SEGMENT = 0x82,
        EVT_ERROR   = 0x83,
        EVT_ALARM   = 0x84,  // 碰撞/急刹告警
        ACK = 0xE0,
        NAK = 0xE1,
    };
    using StateFn = std::function<QString()>;
    using ControlFn = std::function<void()>;
    using GFn = std::function<void(float)>;   // 

    explicit DvrProtoServer(const StorageManager& storage) : storage_(storage) {}

    void setStateProvider(StateFn fn) { stateFn_ = std::move(fn); }
    void setControlHandlers(ControlFn accOn, ControlFn accOff, ControlFn lock) {
        accOn_ = std::move(accOn); accOff_ = std::move(accOff); lock_ = std::move(lock);
    }
    void setGEventHandler(GFn fn) { gEvent_ = std::move(fn); }   // 

    QString url() const { return QString("proto://%1:%2").arg(ip_).arg(serverPort()); }

    bool start(quint16 port = 9001) {
        accOffTimer_ = new QTimer(this);
        accOffTimer_->setSingleShot(true);
        connect(accOffTimer_, &QTimer::timeout, this, [this]{ if (accOff_) accOff_(); });

        sweepTimer_ = new QTimer(this);
        sweepTimer_->setInterval(10 * 1000);
        connect(sweepTimer_, &QTimer::timeout, this, [this]{ reapIdle(); });
        sweepTimer_->start();

        connect(this, &QTcpServer::newConnection, this, [this]{ acceptClients(); });
        if (!listen(QHostAddress::AnyIPv4, port)) {
            qWarning() << "[proto] listen FAILED:" << errorString();
            return false;
        }
        ip_ = firstLanIp();
        qWarning() << "[proto] listening on" << ip_ << port;
        return true;
    }

    // ---- 事件推送 ----
    void pushState(const QString& s) { broadcast(EVT_STATE, {{"state", s}}); }
    void pushSegment(const QString& n, qint64 durUs, qint64 bytes) {
        broadcast(EVT_SEGMENT, {{"name", n}, {"durUs", double(durUs)}, {"bytes", double(bytes)}});
    }
    void pushError(const QString& m) { broadcast(EVT_ERROR, {{"msg", m}}); }
    void pushAlarm(float g) { broadcast(EVT_ALARM, {{"g", double(g)}}); }   // 

private:
    struct Client { QByteArray buf; qint64 lastSeenMs = 0; };
    static constexpr qint64 IDLE_TIMEOUT_MS = 30 * 1000;

    QHash<QTcpSocket*, Client> clients_;
    const StorageManager& storage_;
    StateFn stateFn_;
    ControlFn accOn_, accOff_, lock_;
    GFn gEvent_;                                   // 
    QTimer* accOffTimer_ = nullptr;
    QTimer* sweepTimer_ = nullptr;
    QString ip_;
    quint8 txSeq_ = 0;

    // ---------- 连接 ----------
    void acceptClients() {
        while (QTcpSocket* s = nextPendingConnection()) {
            s->setSocketOption(QAbstractSocket::LowDelayOption, 1);
            clients_.insert(s, Client{{}, nowMs()});
            connect(s, &QTcpSocket::readyRead, this, [this, s]{ onReadyRead(s); });
            connect(s, &QTcpSocket::disconnected, this, [this, s]{
                clients_.remove(s); s->deleteLater();
            });
            sendFrame(s, ACK, QJsonObject{
                {"hello", "dvr-proto-v1"},
                {"cmds", "PING|ACC_ON|ACC_OFF|LOCK|STATUS|G"},
                {"state", currentState()}});
        }
    }

    void onReadyRead(QTcpSocket* s) {
        if (!clients_.contains(s)) return;
        Client& c = clients_[s];
        c.buf += s->readAll();
        if (c.buf.size() > 65536) { s->disconnectFromHost(); return; }
        parseLoop(s, c);
    }

    // ---------- 帧解析：长度定界 + CRC校验 + 失步重同步 ----------
    void parseLoop(QTcpSocket* s, Client& c) {
        forever {
            if (c.buf.size() < 7) return;
            if (uchar(c.buf[0]) != 0x44 || uchar(c.buf[1]) != 0x56 || uchar(c.buf[2]) != 1) {
                c.buf.remove(0, 1); continue;
            }
            const int len = qFromBigEndian<quint16>(c.buf.constData() + 4);
            if (len > 4096) { c.buf.remove(0, 1); continue; }
            const int total = 9 + len;
            if (c.buf.size() < total) return;
            const quint16 crcGot = qFromBigEndian<quint16>(c.buf.constData() + 7 + len);
            if (crc16(c.buf.constData() + 2, 5 + len) != crcGot) {
                sendFrame(s, NAK, {{"err", "bad crc"}});
                c.buf.remove(0, total); continue;
            }
            const quint8 type = uchar(c.buf[3]);
            const quint8 seq  = uchar(c.buf[6]);
            const QByteArray payload = c.buf.mid(7, len);
            c.buf.remove(0, total);
            c.lastSeenMs = nowMs();
            handleFrame(s, type, seq, payload);
        }
    }

    void handleFrame(QTcpSocket* s, quint8 type, quint8 seq, const QByteArray& payload) {
        switch (type) {
        case CMD_PING: sendFrame(s, ACK, {{"for", "PING"}, {"seq", seq}}); break;
        case CMD_ACC_ON:
            accOffTimer_->stop();
            if (accOn_) accOn_();
            sendFrame(s, ACK, {{"for", "ACC_ON"}, {"seq", seq}});
            break;
        case CMD_ACC_OFF:
            accOffTimer_->start(2000);
            sendFrame(s, ACK, {{"for", "ACC_OFF"}, {"seq", seq}, {"stopDelayMs", 2000}});
            break;
        case CMD_LOCK:
            if (lock_) lock_();
            sendFrame(s, ACK, {{"for", "LOCK"}, {"seq", seq}});
            break;
        case CMD_STATUS: sendFrame(s, ACK, statusJson()); break;
        case CMD_G: {                                   // 测试注入
            const QJsonObject p = QJsonDocument::fromJson(payload).object();
            const float g = float(p.value(QStringLiteral("g")).toDouble());
            if (gEvent_) gEvent_(g);                    // 与真实G-sensor同一处理通道
            sendFrame(s, ACK, {{"for", "G"}, {"seq", seq}, {"g", double(g)}});
            break;
        }
        default:
            sendFrame(s, NAK, {{"err", "unknown type"}, {"type", double(type)}});
        }
    }

    // ---------- 发送 ----------
    void sendFrame(QTcpSocket* s, quint8 type, const QJsonObject& payload) {
        s->write(buildFrame(type, QJsonDocument(payload).toJson(QJsonDocument::Compact)));
    }

    void broadcast(quint8 type, const QJsonObject& payload) {
        if (clients_.isEmpty()) return;
        const QByteArray f = buildFrame(type, QJsonDocument(payload).toJson(QJsonDocument::Compact));
        for (QTcpSocket* s : clients_.keys()) s->write(f);
    }

    QByteArray buildFrame(quint8 type, const QByteArray& p) {
        QByteArray f;
        f += char(0x44); f += char(0x56); f += char(1); f += char(type);
        const char lenBe[2] = { char((p.size() >> 8) & 0xFF), char(p.size() & 0xFF) };
        f.append(lenBe, 2);
        f += char(txSeq_++);
        f += p;
        const quint16 crc = crc16(f.constData() + 2, f.size() - 2);
        const char crcBe[2] = { char(crc >> 8), char(crc & 0xFF) };
        f.append(crcBe, 2);
        return f;
    }

    // ---------- 心跳超时（三步走修复版 + 驱逐日志） ----------
    void reapIdle() {
        const qint64 now = nowMs();
        QList<QTcpSocket*> dead;
        for (auto it = clients_.cbegin(); it != clients_.cend(); ++it)
            if (now - it->lastSeenMs > IDLE_TIMEOUT_MS) {
                qWarning() << "[proto] evict idle client"
                           << it.key()->peerAddress().toString();
                dead.append(it.key());
            }
        for (QTcpSocket* s : dead) {
            if (!clients_.contains(s)) continue;
            clients_.remove(s);
            s->disconnectFromHost();
            s->deleteLater();
        }
    }

    // ---------- 工具 ----------
    QJsonObject statusJson() const {
        return {{"state", currentState()},
                {"freeMB", double(storage_.freeMb())},
                {"normalCount", double(storage_.listFiles("normal").size())},
                {"eventCount", double(storage_.listFiles("event").size())}};
    }
    QString currentState() const { return stateFn_ ? stateFn_() : QString(); }
    static qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }

    static quint16 crc16(const char* d, int n) {
        quint32 crc = 0xFFFF;
        for (int i = 0; i < n; ++i) {
            crc ^= quint32(uchar(d[i])) << 8;
            for (int b = 0; b < 8; ++b)
                crc = (crc & 0x8000) ? ((crc << 1) ^ 0x1021) & 0xFFFF : (crc << 1) & 0xFFFF;
        }
        return quint16(crc);
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
};

#endif // DVRPROTOSERVER_H
