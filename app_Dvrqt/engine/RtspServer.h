#ifndef RTSPSERVER_H
#define RTSPSERVER_H

// =====================================================================
// RtspServer.h -- RTSP 直播预览服务器（端口8554，实时数据面）
// 协议：RTSP over TCP(RFC2326) + interleaved RTP(RFC3550) + H264打包
// (RFC6184 Single-NAL/FU-A)。ffplay/VLC 直接可播。
// 省略RTCP发送方报告、无鉴权、
// 仅TCP交织传输。分流规则：首字节'$'=交织二进制帧，字母=RTSP文本信令。
// =====================================================================

#include <QTcpServer>
#include <QTcpSocket>
#include <QHash>
#include <QVector>
#include <QByteArray>
#include <QRandomGenerator>
#include <QMetaObject>
#include <QDateTime>
#include <QNetworkInterface>
#include <QDebug>
#include <functional>

class RtspServer : public QTcpServer {
public:
    using IdrFn = std::function<void()>;

    void setIdrRequester(IdrFn fn) { idrFn_ = std::move(fn); }
    QString url() const { return QString("rtsp://%1:8554/live").arg(ip_); }

    bool start(quint16 port = 8554) {
        connect(this, &QTcpServer::newConnection, this, [this]{ acceptClients(); });
        if (!listen(QHostAddress::AnyIPv4, port)) {
            qWarning() << "[rtsp] listen FAILED:" << errorString();
            return false;
        }
        ip_ = firstLanIp();
        qWarning() << "[rtsp] listening on" << ip_ << port;
        return true;
    }

    // ---- 供 drainLoop（工作线程）调用：拷贝后队列投递回属主线程再发包 ----
    // （QTcpSocket 只能在属主线程读写；QByteArray隐式共享，拷贝近零成本）
    void setCodecConfig(const uint8_t* d, int n) {          // SPS/PPS → 进SDP
        QByteArray ba(reinterpret_cast<const char*>(d), n);
        QMetaObject::invokeMethod(this, [this, ba]{ codecCfg_ = ba; },
                                  Qt::QueuedConnection);
    }
    void pushFrame(const uint8_t* d, int n, qint64 ptsUs, bool key) {
        QByteArray ba(reinterpret_cast<const char*>(d), n);
        QMetaObject::invokeMethod(this,
                [this, ba, ptsUs, key]{ streamFrame(ba, ptsUs, key); },
                Qt::QueuedConnection);
    }

private:
    struct Session {
        QByteArray buf;
        QString sessionId;
        bool playing = false;
        bool waitingIdr = true;       // 新观众必须从IDR入流，否则花屏
        quint16 seq = 0;
        quint32 ssrc = 0;
        int dropStreak = 0;
    };
    static constexpr int MAX_PAYLOAD = 1400;   // RTP载荷上限（FU-A分片阈值）

    QHash<QTcpSocket*, Session> sessions_;
    QByteArray codecCfg_;                     // Annex-B 格式 SPS+PPS
    IdrFn idrFn_;
    QString ip_;

    // ---------- 连接 ----------
    void acceptClients() {
        while (QTcpSocket* s = nextPendingConnection()) {
            s->setSocketOption(QAbstractSocket::LowDelayOption, 1);
            Session& se = sessions_[s];
            se.ssrc = QRandomGenerator::global()->generate();
            se.seq  = quint16(QRandomGenerator::global()->generate());
            se.sessionId = QString::number(QDateTime::currentMSecsSinceEpoch());
            connect(s, &QTcpSocket::readyRead, this, [this, s]{ onReadyRead(s); });
            connect(s, &QTcpSocket::disconnected, this,
                    [this, s]{ sessions_.remove(s); s->deleteLater(); });
            qWarning() << "[rtsp] client" << s->peerAddress().toString() << "in";
        }
    }

    // ---------- 接收：先分流 $二进制 / 文本信令 ----------
    void onReadyRead(QTcpSocket* s) {
        QByteArray& b = sessions_[s].buf;
        b += s->readAll();
        if (b.size() > (1 << 20)) { s->disconnectFromHost(); return; }
        forever {
            if (b.isEmpty()) return;
            if (uchar(b[0]) == '$') {                 // 交织二进制帧（客户端RTCP等）
                if (b.size() < 4) return;
                const int len = (uchar(b[2]) << 8) | uchar(b[3]);
                if (b.size() < 4 + len) return;
                b.remove(0, 4 + len);                 // 接收端RR我们不需要，丢弃
                continue;
            }
            const int idx = b.indexOf("\r\n\r\n");    // 文本信令：空行结尾
            if (idx < 0) return;
            const QByteArray msg = b.left(idx);
            b.remove(0, idx + 4);
            handleMessage(s, msg);
            if (!sessions_.contains(s)) return;       // 可能已被TEARDOWN清理
        }
    }

    // ---------- RTSP 信令（文本，形似HTTP） ----------
    void handleMessage(QTcpSocket* s, const QByteArray& msg) {
        const QList<QByteArray> lines = msg.split('\n');
        if (lines.isEmpty()) return;
        const QByteArray first = lines.first().trimmed();
        const int sp1 = first.indexOf(' ');
        if (sp1 < 0) return;
        const QString method = QString::fromLatin1(first.left(sp1));

        QString cseq = "0", transport, sessionHdr;
        for (int i = 1; i < lines.size(); ++i) {
            const QByteArray ln = lines[i].trimmed();
            const int c = ln.indexOf(':');
            if (c <= 0) continue;
            const QString k = QString::fromLatin1(ln.left(c).trimmed()).toLower();
            const QString v = QString::fromLatin1(ln.mid(c + 1).trimmed());
            if      (k == "cseq")      cseq = v;
            else if (k == "transport") transport = v;
            else if (k == "session")   sessionHdr = v;
        }
        qWarning() << "[rtsp]" << method << "cseq=" << cseq;

        if (method == "OPTIONS") {
            respond(s, cseq, "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN\r\n");
        } else if (method == "DESCRIBE") {
            respond(s, cseq, QString("Content-Base: %1/\r\n").arg(url()),
                    sdp(), "application/sdp");
        } else if (method == "SETUP") {
            if (!transport.contains("RTP/AVP/TCP") || !transport.contains("interleaved")) {
                respondErr(s, cseq, 461, "Unsupported transport (use -rtsp_transport tcp)");
                return;
            }
            respond(s, cseq, QString("Session: %1\r\n"
                                     "Transport: RTP/AVP/TCP;interleaved=0-1\r\n")
                             .arg(sessions_[s].sessionId));
        } else if (method == "PLAY") {
            Session& se = sessions_[s];
            se.playing = true;
            se.waitingIdr = true;                 // 从下一个IDR开始发
            if (idrFn_) idrFn_();                 // 让编码器立刻出IDR，亚秒入画
            respond(s, cseq, QString("Session: %1\r\n").arg(se.sessionId));
            qWarning() << "[rtsp] PLAY, waiting IDR...";
        } else if (method == "TEARDOWN") {
            sessions_[s].playing = false;
            respond(s, cseq, QString());
            s->disconnectFromHost();
        } else {                                  // GET_PARAMETER保活等 → 200
            respond(s, cseq, QString());
        }
    }

    // ---------- SDP（DESCRIBE应答体：告诉播放器这是什么流） ----------
    QByteArray sdp() const {
        QString fmtp = QStringLiteral("packetization-mode=1");
        QByteArray spsB64, ppsB64;
        int pr = 0x42, co = 0xC0, lv = 0x1F;
        for (const QByteArray& nal : splitNals(codecCfg_)) {
            const quint8 t = uchar(nal[0]) & 0x1F;
            if (t == 7 && spsB64.isEmpty()) {             // SPS
                spsB64 = nal.toBase64();
                if (nal.size() >= 4) { pr = uchar(nal[1]); co = uchar(nal[2]); lv = uchar(nal[3]); }
            }
            if (t == 8 && ppsB64.isEmpty()) ppsB64 = nal.toBase64();   // PPS
        }
        if (!spsB64.isEmpty() && !ppsB64.isEmpty())
            fmtp += QString(";profile-level-id=%1%2%3;sprop-parameter-sets=%4,%5")
                        .arg(pr, 2, 16, QChar('0')).arg(co, 2, 16, QChar('0'))
                        .arg(lv, 2, 16, QChar('0'))
                        .arg(QString::fromLatin1(spsB64), QString::fromLatin1(ppsB64));
        return QString("v=0\r\n"
                       "o=- 0 0 IN IP4 0.0.0.0\r\n"
                       "s=DVR Live\r\n"
                       "t=0 0\r\n"
                       "m=video 0 RTP/AVP 96\r\n"
                       "a=rtpmap:96 H264/90000\r\n"
                       "a=fmtp:96 %1\r\n"
                       "a=control:trackID=0\r\n").arg(fmtp).toUtf8();
    }

    // ---------- 推流：IDR门控 + 背压丢帧 ----------
    void streamFrame(const QByteArray& frame, qint64 ptsUs, bool key) {
        if (sessions_.isEmpty()) return;
        const quint32 ts = quint32(ptsUs * 9 / 100);      // μs → 90kHz时钟
        for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
            Session& se = it.value();
            if (!se.playing) continue;
            if (se.waitingIdr) {                          // P帧无参考帧无法解
                if (!key) continue;                       // → 丢到IDR为止
                se.waitingIdr = false;
                qWarning() << "[rtsp] session enters at IDR";
            }
            QTcpSocket* sock = it.key();
            if (sock->bytesToWrite() > 4 * 1024 * 1024) { // 直播宁可丢、不可堆
                if (++se.dropStreak == 1 || se.dropStreak % 90 == 0)
                    qWarning() << "[rtsp] client slow, dropping";
                continue;
            }
            se.dropStreak = 0;
            writeFrame(sock, se, frame, ts);
        }
    }

    void writeFrame(QTcpSocket* sock, Session& se,
                    const QByteArray& frame, quint32 ts) {
        const QVector<QByteArray> nals = splitNals(frame);
        for (int i = 0; i < nals.size(); ++i) {
            const QByteArray& nal = nals[i];
            const bool lastNal = (i == nals.size() - 1);
            if (nal.size() <= MAX_PAYLOAD)
                sendRtp(sock, se, nal.constData(), nal.size(), ts, lastNal);
            else
                sendFuA(sock, se, nal, ts, lastNal);
        }
    }

    // Annex-B(00 00 01/00 00 00 01分隔) → NAL列表（去起始码与定界0）
    static QVector<QByteArray> splitNals(const QByteArray& annexB) {
        QVector<QByteArray> out;
        const char* p = annexB.constData();
        const int n = annexB.size();
        QVector<int> sc;
        for (int i = 0; i + 2 < n; ++i)
            if (uchar(p[i]) == 0 && uchar(p[i+1]) == 0 && uchar(p[i+2]) == 1)
                sc.append(i);
        for (int k = 0; k < sc.size(); ++k) {
            const int b = sc[k] + 3;
            int e = (k + 1 < sc.size()) ? sc[k+1] : n;
            while (e > b && uchar(p[e-1]) == 0) --e;      // 尾部0属下一帧定界
            if (e > b) out.append(annexB.mid(b, e - b));
        }
        return out;
    }

    // 单NAL包（小NAL）
    void sendRtp(QTcpSocket* sock, Session& se, const char* pay, int len,
                 quint32 ts, bool marker) {
        QByteArray rtp;
        rtp.reserve(12 + len);
        rtp += char(0x80);                                // V=2
        rtp += char((marker ? 0x80 : 0x00) | 96);         // M + PT=96
        rtp += char(se.seq >> 8); rtp += char(se.seq & 0xFF); ++se.seq;
        rtp += char(ts >> 24); rtp += char(ts >> 16);
        rtp += char(ts >> 8);  rtp += char(ts);
        rtp += char(se.ssrc >> 24); rtp += char(se.ssrc >> 16);
        rtp += char(se.ssrc >> 8);  rtp += char(se.ssrc);
        rtp.append(pay, len);
        QByteArray pkt;
        pkt.reserve(rtp.size() + 4);
        pkt += '$'; pkt += char(0);                       // channel 0 = RTP
        pkt += char(rtp.size() >> 8); pkt += char(rtp.size() & 0xFF);
        pkt += rtp;
        sock->write(pkt);
    }

    // FU-A分片（大NAL：一个I帧常见100KB+）
    void sendFuA(QTcpSocket* sock, Session& se, const QByteArray& nal,
                 quint32 ts, bool lastNalOfFrame) {
        const quint8 nh    = uchar(nal[0]);
        const char  ind    = char((nh & 0xE0) | 28);      // 保留F/NRI + type=28
        const quint8 ftype = nh & 0x1F;
        const QByteArray body = nal.mid(1);
        int off = 0;
        bool first = true;
        while (off < body.size()) {
            const int chunk = qMin(MAX_PAYLOAD - 2, body.size() - off);
            const bool last = (off + chunk >= body.size());
            QByteArray pay;
            pay += ind;
            pay += char((first ? 0x80 : 0) | (last ? 0x40 : 0) | ftype); // S|E|type
            pay.append(body.constData() + off, chunk);
            sendRtp(sock, se, pay.constData(), pay.size(), ts,
                    last && lastNalOfFrame);              // M=1标记帧尾
            off += chunk;
            first = false;
        }
    }

    // ---------- 应答与工具 ----------
    static void respond(QTcpSocket* s, const QString& cseq, const QString& extra,
                        const QByteArray& body = {}, const char* ctype = nullptr) {
        QByteArray r = "RTSP/1.0 200 OK\r\nCSeq: " + cseq.toUtf8() + "\r\n"
                     + extra.toUtf8();
        if (!body.isEmpty()) {
            r += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
            if (ctype) r += "Content-Type: " + QByteArray(ctype) + "\r\n";
        }
        r += "\r\n";
        r += body;
        s->write(r);
    }
    static void respondErr(QTcpSocket* s, const QString& cseq, int code,
                           const QString& why) {
        s->write("RTSP/1.0 " + QByteArray::number(code) + " " + why.toUtf8()
                 + "\r\nCSeq: " + cseq.toUtf8() + "\r\n\r\n");
    }

    static QString firstLanIp() {   // 与 DvrProtoServer.h 完全相同
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

#endif // RTSPSERVER_H
