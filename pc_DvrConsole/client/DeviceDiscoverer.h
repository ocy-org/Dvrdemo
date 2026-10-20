#ifndef DEVICEDISCOVERER_H
#define DEVICEDISCOVERER_H
// =====================================================================
// UDP广播设备发现（PC端）
// 协议约定（设备端预留实现，见文末说明）：
//   PC  → 255.255.255.255:9002 及各网卡定向广播，载荷 "DVR_DISCOVER\n"
//   设备 → 按收包源地址原路单播回复一行JSON：
//          {"dvr":1,"name":"行车记录仪","proto":9001,"rtsp":8554}
// 安全原则：设备IP以回包源地址为准，不信任载荷内容
// =====================================================================
#include <QObject>
#include <QUdpSocket>
#include <QTimer>
#include <QSet>
#include <QJsonObject>
#include <QJsonDocument>
#include <QNetworkInterface>

class DeviceDiscoverer : public QObject {
    Q_OBJECT
public:
    static constexpr quint16 kPort = 9002;
    explicit DeviceDiscoverer(QObject* parent = nullptr) : QObject(parent) {}

    void discover(int timeoutMs = 1500) {
        if (!rx_.bind(QHostAddress::AnyIPv4)) { emit finished(0); return; }
        found_.clear();
        connect(&rx_, &QUdpSocket::readyRead, this, &DeviceDiscoverer::onReadyRead,
                Qt::UniqueConnection);                     // 重复点击不叠加连接

        const QByteArray req = "DVR_DISCOVER\n";
        rx_.writeDatagram(req, QHostAddress::Broadcast, kPort);   // 255.255.255.255
        for (const QNetworkInterface& ifa : QNetworkInterface::allInterfaces()) {
            const auto fl = ifa.flags();
            if (!(fl & QNetworkInterface::CanBroadcast) || (fl & QNetworkInterface::IsLoopBack))
                continue;
            for (const QNetworkAddressEntry& e : ifa.addressEntries()) {
                if (e.ip().protocol() != QAbstractSocket::IPv4Protocol) continue;
                if (!e.broadcast().isNull())
                    rx_.writeDatagram(req, e.broadcast(), kPort); // 子网定向广播
            }
        }
        QTimer::singleShot(timeoutMs, this, [this]{
            rx_.close();
            emit finished(int(found_.size()));
        });
    }

signals:
    void deviceFound(const QJsonObject& info);
    void finished(int count);

private slots:
    void onReadyRead() {
        while (rx_.hasPendingDatagrams()) {
            QByteArray d; d.resize(int(rx_.pendingDatagramSize()));
            QHostAddress from; quint16 port;
            rx_.readDatagram(d.data(), d.size(), &from, &port);
            const auto doc = QJsonDocument::fromJson(d);
            if (!doc.isObject() || !doc.object().contains("dvr")) continue;
            QJsonObject o = doc.object();
            QString ip = from.toString();
            if (ip.startsWith("::ffff:", Qt::CaseInsensitive))
                ip = ip.mid(7);              // 去掉 ::ffff: 前缀，剩纯IPv4
            o["ip"] = ip;                 // ★以源地址为准
            if (!found_.contains(o["ip"].toString())) {
                found_.insert(o["ip"].toString());     // 去重
                emit deviceFound(o);
            }
        }
    }

private:
    QUdpSocket rx_;
    QSet<QString> found_;
};
#endif // DEVICEDISCOVERER_H
