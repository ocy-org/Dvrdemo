// client/ScenarioReplayer.h —— 场景脚本回放器
// 脚本格式与第5周一致："秒 TAB命令 参数"，如:
//   0 STATUS / 1 ACC_ON / 6 G 2.5 / 20 STATUS / 26 LOCK / 30 ACC_OFF
#pragma once
#include <QObject>
#include <QTimer>
#include <QVector>
#include <QFile>
#include "DeviceClient.h"

class ScenarioReplayer : public QObject {
    Q_OBJECT
public:
    struct Stats { int total = 0, ackOk = 0, alarms = 0, segments = 0; };
    bool load(const QString& path) {
        QFile f(path); if (!f.open(QIODevice::ReadOnly)) return false;
        steps_.clear();
        while (!f.atEnd()) {
            const auto parts = f.readLine().trimmed().split(' ');
            if (parts.size() >= 2)
                steps_.push_back({parts[0].toInt(), parts[1],
                                  parts.size() > 2 ? parts[2] : QString()});
        }
        return !steps_.isEmpty();
    }
    void run(DeviceClient* dev) {
        dev_ = dev; st_ = {};
        connect(dev_, &DeviceClient::eventReceived, this, [this](const QJsonObject& o){
            const QString evt = o["evt"].toString();
            if (evt == "ACK")            ++st_.ackOk;      // 字段名按你的
            else if (evt == "EVT_ALARM") ++st_.alarms;    // DvrProtoServer实际
            else if (evt == "EVT_SEGMENT") ++st_.segments; // 输出微调
        });
        int maxT = 0;
        for (const auto& s : steps_) {
            maxT = qMax(maxT, s.tSec);
            const QString cmd = s.cmd, arg = s.arg;
            QTimer::singleShot(s.tSec * 1000, this, [this, s, cmd, arg]{
                dev_->sendCommand(cmd, arg);
                emit log(QString("[%1s] >> %2 %3").arg(s.tSec).arg(cmd, arg));
            });
        }
        QTimer::singleShot((maxT + 5) * 1000, this, [this]{  // 缓冲5s收尾报统计
            disconnect(dev_, nullptr, this, nullptr);
            emit finished(st_);                          // ↔ 期望EVT_ALARM=2
        });                                              //    EVT_SEGMENT=4
    }
signals:
    void log(const QString& line);
    void finished(const Stats& s);
private:
    struct Step { int tSec; QString cmd; QString arg; };
    QVector<Step> steps_;
    DeviceClient* dev_ = nullptr;
    Stats st_;
};
