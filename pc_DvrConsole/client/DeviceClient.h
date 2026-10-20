#pragma once
#include <QTcpSocket>
#include <QTimer>
#include <QJsonObject>
#include <QJsonDocument>

// =====================================================================
// 设备协议交互层
// 作用：与车载/终端设备建立 TCP 长连接，按自定义二进制协议【主动下发命令】
//       并【被动解析设备回帧】，把每一帧解出的 JSON 载荷以信号形式传给 UI。
// 帧结构（大端序）：
// 帧  = "DV" + ver(0x01) + type + len(2B大端) + seq(1B) + payload + CRC16(2B大端)
// 即：魔数2 + 头部(ver+type+len2+seq=5) + 载荷len + CRC2  => 总长 = 9 + len
// CRC = CCITT-FALSE算法(init=0xFFFF, poly=0x1021, 不反射, xor=0)，范围=ver起至载荷末
// 上行帧：PING=0x01  ACC_ON=0x02  ACC_OFF=0x03  LOCK=0x04  STATUS=0x05  G=0x06
// 下行帧：EVT_STATE=0x81  EVT_SEGMENT=0x82  EVT_ERROR=0x83  EVT_ALARM=0x84  ACK=0xE0  NAK=0xE1
// 运行行为：
//   1) seq 序号从 1 开始自增（按字节回绕）；
//   2) 每次连接建立后自动发一帧 STATUS 拉取一次设备状态；
//   3) 连接存续期间每 10 秒发一帧 PING 维持心跳；
//   4) 接收侧用"滑动找魔数 + 按长度截帧 + CRC 校验"组帧，CRC 不符整帧丢弃。
// =====================================================================
class DeviceClient : public QObject {
    Q_OBJECT
public:
    //向APP发送的协议命令 / 设备回帧的事件类型（type 字节的全集）
    // 0x01~0x06 为 APP 主动下发的命令；0x81 以上高位为设备上报的事件；0xE0/0xE1 为应答
    enum Type : quint8 {
        CMD_PING = 0x01, CMD_ACC_ON = 0x02, CMD_ACC_OFF = 0x03,
        CMD_LOCK = 0x04, CMD_STATUS = 0x05, CMD_G = 0x06,
        EVT_STATE = 0x81, EVT_SEGMENT = 0x82, EVT_ERROR = 0x83, EVT_ALARM = 0x84,
        ACK = 0xE0, NAK = 0xE1
    };
    //APP端返回的信息：二进制转字符串
    static QString typeName(quint8 t) {
        switch (t) {
        case CMD_PING: return "PING";       case CMD_ACC_ON: return "ACC_ON";
        case CMD_ACC_OFF: return "ACC_OFF"; case CMD_LOCK: return "LOCK";
        case CMD_STATUS: return "STATUS";   case CMD_G: return "G";
        case EVT_STATE: return "EVT_STATE"; case EVT_SEGMENT: return "EVT_SEGMENT";
        case EVT_ERROR: return "EVT_ERROR"; case EVT_ALARM: return "EVT_ALARM";
        case ACK: return "ACK";             case NAK: return "NAK";
        default: return QString("0x%1").arg(t, 2, 16, QChar('0'));
        }
    }

    explicit DeviceClient(QObject* parent = nullptr) : QObject(parent) {
        // —— 自动重连定时器：单次触发(2 秒) ——
        // 断线/出错后启动，到点尝试一次重连；连上后在 connected 槽里 stop。
        reconnectTimer_.setInterval(2000);
        reconnectTimer_.setSingleShot(true);
        connect(&reconnectTimer_, &QTimer::timeout, this, &DeviceClient::tryReconnect);
        //心跳定时器：连接存续期间每 10 秒触发一次，发 PING 保活
        heartTimer_.setInterval(10000);
        connect(&heartTimer_, &QTimer::timeout, this, [this]{
            if (isConnected()) sendFrame(CMD_PING);//10秒一次心跳
        });
        // —— socket 连上：复位会话状态并拉起会话级动作 ——
        connect(&sock_, &QTcpSocket::connected, this, [this]{
            reconnectTimer_.stop();
            buf_.clear();
            seq_ = 0;
            heartTimer_.start();//启动心跳定时器
            emit connected();// 发送"已连接"事件到主框架
            sendFrame(CMD_STATUS);//连接之后，获取一次状态
        });
        // —— socket 断开：停心跳并（在期望保持连接时）安排重连 ——
        connect(&sock_, &QTcpSocket::disconnected, this, [this]{
            heartTimer_.stop();//关闭心跳定时器
            emit disconnected();// 发送"断开连接"事件到主框架
            if (!host_.isEmpty()) reconnectTimer_.start();
        });
        // —— socket 出错：与断开同处理，安排重连 ——
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
        connect(&sock_, &QAbstractSocket::errorOccurred, this, [this](QAbstractSocket::SocketError){
            if (!host_.isEmpty()) reconnectTimer_.start(); });
#else
        connect(&sock_, qOverload<QAbstractSocket::SocketError>(&QAbstractSocket::error),
                this, [this](QAbstractSocket::SocketError){ if (!host_.isEmpty()) reconnectTimer_.start(); });
#endif
        // —— socket 有可读数据：累加到缓冲区后尝试组帧解析 ——
        connect(&sock_, &QTcpSocket::readyRead, this, &DeviceClient::onReadyRead);
    }

    // 发起连接：记录目标地址端口并立即尝试连接（host_ 非空 = 进入"期望保持连接"状态）
    void connectTo(const QString& host, quint16 port) {
        host_ = host;
        port_ = port;
        tryReconnect();
    }
    // 主动断开：清空 host_（使重连逻辑失效）并中止套接字，避免断开后被自动重连
    void disconnectFromDevice() {
        host_.clear();
        sock_.abort();
    }
    // 是否处于"期望保持连接"状态（host_ 非空即会自动重连）
    bool willReconnect() const { return !host_.isEmpty(); }
    // 当前链路是否真正处于已连接状态
    bool isConnected() const { return sock_.state() == QAbstractSocket::ConnectedState; }

    // 按协议把 type + 可选 JSON 载荷组装成完整帧并发送。
    // 组装顺序：body = ver + type + len(2B大端) + seq + payload，
    //         对 body 求 CRC16，再 frame = "DV" + body + CRC(2B大端)。
    void sendFrame(quint8 type, const QJsonObject& payload = {}) {
        if (!isConnected()) return;
        QByteArray pay;
        if (!payload.isEmpty())
            pay = QJsonDocument(payload).toJson(QJsonDocument::Compact);
        const quint8 s = ++seq_;                         // seq 从 1 自增
        QByteArray body;
        body += char(0x01);     // ver = 0x01
        body += char(type);     // type 命令/事件字节
        body += char((pay.size() >> 8) & 0xFF);// len 高字节（大端）
        body += char(pay.size() & 0xFF);// len 低字节
        body += char(s);        // seq
        body += pay;            // payload（可为空）
        const quint16 c = crc16(body);  // CRC 计算范围 = body（ver..载荷末）
        QByteArray frame;
        frame += "DV";
        frame += body;
        frame += char((c >> 8) & 0xFF); // CRC 高字节（大端）
        frame += char(c & 0xFF);        // CRC 低字节
        sock_.write(frame); sock_.flush();// 写入缓存并立即刷新，降低时延
    }
    // 这是面向 UI 的"字符串命令"入口，内部翻译为 sendFrame(type, payload)，主要由MainWindow调用
    void sendCommand(const QString& cmd, const QString& arg = {}) {
        quint8 t = 0; QJsonObject o;
        if      (cmd == "PING")    t = CMD_PING;
        else if (cmd == "ACC_ON")  t = CMD_ACC_ON;
        else if (cmd == "ACC_OFF") t = CMD_ACC_OFF;
        else if (cmd == "LOCK")    t = CMD_LOCK;
        else if (cmd == "STATUS")  t = CMD_STATUS;
        else if (cmd == "G")     { t = CMD_G; o["g"] = arg.toDouble(); }
        else { emit crcLearned("未知命令名：" + cmd); return; }
        sendFrame(t, o);
    }

signals:
    void connected();
    void disconnected();
    void eventReceived(const QJsonObject& o);      // 载荷JSON + 注入 _t/_seq/_tname
    void rawLineReceived(const QByteArray& line);  // CRC错误等异常留痕
    void crcLearned(const QString& desc);          // 提示通道

private slots:
    // 尝试重连：仅在尚未连接时 abort 残留状态并重新发起 connectToHost
    void tryReconnect() {
        if (sock_.state() != QAbstractSocket::ConnectedState) {
            sock_.abort(); sock_.connectToHost(host_, port_);
        }
    }
    // 可读数据到达：把新字节追加进缓冲 buf_，再交给 processBuffer 组帧
    // （TCP 是字节流，一帧可能被拆包/粘包，必须先缓冲再解析）
    void onReadyRead() { buf_ += sock_.readAll(); processBuffer(); }

private:
    // ---- CRC16-CCITT-FALSE ----
    static quint16 crc16(const QByteArray& d) {
        quint16 crc = 0xFFFF;
        for (char b : d) {
            crc = quint16(crc ^ quint16(quint8(b) << 8));
            for (int i = 0; i < 8; ++i)
                crc = (crc & 0x8000) ? quint16(quint16(crc << 1) ^ 0x1021) : quint16(crc << 1);
        }
        return crc;
    }

    // ---- 接收解析----
    // 在字节流缓冲上循环提取完整帧：
    //   1) 滑动窗口丢弃"非 DV 魔数"前导字节，对齐帧头；
    //   2) 头不足 7 字节则等待续包；按 len 字段算出整帧总长 total=9+len；
    //   3) 总长不足=半帧，等待续包；len>4096 视为脏数据，滑过 1 字节继续找魔数；
    //   4) 对 ver..载荷末这段 body 校验 CRC，不符则丢整帧并留痕；
    //   5) 取出 type/seq/载荷，解析载荷 JSON，注入元信息后上抛 eventReceived。
    void processBuffer() {
        while (true) {
            // 对齐帧头：只要开头两字节不是 'D''V'，就丢弃首字节继续右滑
            while (buf_.size() >= 2 && !(buf_.at(0) == 'D' && buf_.at(1) == 'V'))
                buf_.remove(0, 1);                       // 滑过非魔数字节
            // 连基本头(2+ver+type+len2+seq)都不够→等续包
            if (buf_.size() < 7) return;
            const int ln = (quint8(buf_.at(4)) << 8) | quint8(buf_.at(5));// 读 len（大端，buf[4..5]）
            // 异常长度→判为脏数据，滑 1 字节重找魔数
            if (ln > 4096) { buf_.remove(0, 1); continue; }
            const int total = 9 + ln;// 整帧总长 = 魔数2 + 头5 + 载荷len + CRC2
            if (buf_.size() < total) return;             // 半帧→等续包

            const QByteArray body = buf_.mid(2, 5 + ln); // ver..载荷 = buf[2:7+ln]
            const quint16 wc = quint16((quint8(buf_.at(7 + ln)) << 8) | quint8(buf_.at(8 + ln)));
            if (crc16(body) != wc) {                     // CRC错→整帧丢弃（同Python）
                emit rawLineReceived("[CRC错误，丢帧] " + buf_.left(total).toHex());
                buf_.remove(0, total);// 本帧消费完毕，移出缓冲
                continue;
            }
            const quint8 type = quint8(buf_.at(3));// type = buf[3]
            const quint8 seq  = quint8(buf_.at(6));// type = buf[6]
            const QByteArray pay = buf_.mid(7, ln);// 载荷 = buf[7 .. 7+ln)
            buf_.remove(0, total);// 本帧消费完毕，移出缓冲

            QJsonObject o;
            if (ln > 0) {
                QJsonParseError e{};
                o = QJsonDocument::fromJson(pay, &e).object();
                if (e.error != QJsonParseError::NoError) { // 载荷非法 JSON→丢弃并留痕（继续找下一帧）
                    emit rawLineReceived("[载荷非JSON] " + pay.toHex());
                    continue;
                }
            }
            // 注入帧元信息，便于 UI 不必解析原始帧即可识别类型/序号
            o["_t"] = type; o["_seq"] = seq; o["_tname"] = typeName(type);
            emit eventReceived(o);
        }
    }

    QTcpSocket sock_;
    QTimer reconnectTimer_, heartTimer_;
    QByteArray buf_;// 接收粘包/拆包缓冲（字节流组帧用）
    QString host_; quint16 port_ = 0;// 目标地址端口；host_ 非空=期望保持连接
    quint8 seq_ = 0;// 发送序号，发帧前 ++ 后使用（从 1 起）
};
