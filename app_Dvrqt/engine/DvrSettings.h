#ifndef DVRSETTINGS_H
#define DVRSETTINGS_H

// =====================================================================
// 第4周：运行期配置持久化（JSON）
// 与 Config.h 的区别：Config.h 是编译期常量（改了要重编），本类是运行期
// 可写配置（QML开关→立即落盘→断电/重启不丢），存于App私有目录
// =====================================================================
#include <QJsonObject>
#include <QJsonDocument>
#include <QFile>
#include <QString>

class DvrSettings {
public:
    explicit DvrSettings(const QString& path) : path_(path) { load(); }

    bool autoRecord() const { return j_[QStringLiteral("autoRecord")].toBool(true); } // 默认开
    void setAutoRecord(bool b) {
        j_[QStringLiteral("autoRecord")] = b;
        save();
    }

private:
    void load() {
        QFile f(path_);
        if (f.open(QIODevice::ReadOnly))
            j_ = QJsonDocument::fromJson(f.readAll()).object();
    }
    void save() const {
        QFile f(path_);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            f.write(QJsonDocument(j_).toJson());
    }
    QString path_;
    QJsonObject j_;
};

#endif // DVRSETTINGS_H
