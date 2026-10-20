#pragma once
// =====================================================================
// UDP发现应答器 —— 与PC端DeviceDiscoverer.h协议对齐：
//   收 "DVR_DISCOVER"(:9002) → 向【收包源IP:源端口】原路单播回JSON
//   {"dvr":1,"name":"行车记录仪","proto":9001,"rtsp":8554}
// 纯被动、随APP常驻：与录像状态无关，IDLE时也要能被搜到。
// 刻意不带Q_OBJECT：lambda接收QUdpSocket信号即可，纯头文件，工程零配置。
// =====================================================================
#include <QUdpSocket>
#include <QJsonObject>
#include <QJsonDocument>
#include <QHostAddress>

class DiscoverResponder : public QObject {
public:
    static constexpr quint16 kPort = 9002;   // ★与PC端一致；不冲突9000/9001/8554
    DiscoverResponder(const QString& name, quint16 protoPort, quint16 rtspPort,
                      QObject* parent = nullptr) : QObject(parent) {
        if (!udp_.bind(QHostAddress::AnyIPv4, kPort, QUdpSocket::ReuseAddressHint)) {
            qWarning() << "[discover] UDP" << kPort << "bind失败:" << udp_.errorString();
            return;
        }
        reply_ = QJsonDocument(QJsonObject{          // 应答体固定 → 预生成
            {"dvr", 1}, {"name", name}, {"proto", protoPort}, {"rtsp", rtspPort}
        }).toJson(QJsonDocument::Compact);
        QObject::connect(&udp_, &QUdpSocket::readyRead, this, [this]{
            while (udp_.hasPendingDatagrams()) {
                QByteArray d; d.resize(int(udp_.pendingDatagramSize()));
                QHostAddress from; quint16 port;
                udp_.readDatagram(d.data(), d.size(), &from, &port);
                if (QString::fromUtf8(d).trimmed() != "DVR_DISCOVER") continue;
                const qint64 n = udp_.writeDatagram(reply_, from, port);
                if (n < 0) qWarning() << "[discover]" << from.toString() << port << "发送失败:" << udp_.errorString();
                else       qWarning() << "[discover]" << from.toString() << port << "已回" << n << "字节";

            }
        });
        qWarning() << "[discover] 应答器就绪 :9002";
    }
private:
    QUdpSocket udp_;
    QByteArray reply_;
};
