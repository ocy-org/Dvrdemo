#ifndef STORAGEMANAGER_H
#define STORAGEMANAGER_H

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileInfoList>
#include <QStorageInfo>
#include <QDateTime>
#include <algorithm>
#include "Config.h"

class StorageManager {
public:
    explicit StorageManager(const QString& rootPath)
        : rootPath_(rootPath), normal_(rootPath + "/normal"), eventPath_(rootPath + "/event") {
        normal_.mkpath(".");
        QDir().mkpath(eventPath_);
    }

    QString newNormalFile() const {
        return normal_.filePath(
                QDateTime::currentDateTime().toString("yyyyMMdd_HHmmsszzz") + "_front.mp4");
    }

    void enforceQuota() const {
        while (freeMb() < Config::KEEP_FREE_MB) {
            auto files = listNormalFiles();
            if (files.isEmpty()) break;
            QFile::remove(files.first().absoluteFilePath());
        }
    }

    QFileInfoList listNormalFiles() const {
        auto files = normal_.entryInfoList({"*.mp4"}, QDir::Files);
        std::sort(files.begin(), files.end(), [](const QFileInfo& a, const QFileInfo& b){
            return a.lastModified() < b.lastModified();
        });
        return files;
    }

    qint64 freeMb() const {
        return QStorageInfo(rootPath_).bytesAvailable() / 1048576;
    }

    // =================事件锁档与缩略图 =================

    // 锁档 = normal/ 移入 event/ + 置只读。
    // 循环覆盖(enforceQuota)只清 normal/，锁档文件因此幸存——"锁"的本质。
    // 返回 event/ 下新路径；失败返回空串（调用方降级为普通段，数据不丢）
    QString moveToEvent(const QString& normalFile) const {
        const QString dst = eventPath_ + "/" + QFileInfo(normalFile).fileName();
        if (!QFile::rename(normalFile, dst)) return QString();
        QFile::setPermissions(dst, QFile::permissions(dst)
                & ~(QFile::WriteOwner | QFile::WriteUser
                  | QFile::WriteGroup | QFile::WriteOther));
        return dst;
    }

    QString eventDir() const { return eventPath_; }
    QString normalDir() const { return normal_.absolutePath(); } 
    QString thumbDir() const { QDir().mkpath(rootPath_ + "/thumb"); return rootPath_ + "/thumb"; }

    // ================= TCP/HTTP共用 =================
    // event/ 目录天然被覆盖：LIST event / GET event/xxx 直接可用（第2周成果）
    QString pathFor(const QString& dir, const QString& name) const {
        QDir d = (dir == "normal") ? normal_ : QDir(eventPath_);
        const QString root = QFileInfo(d.absolutePath()).canonicalFilePath();
        const QString canon = QFileInfo(d.filePath(name)).canonicalFilePath();
        return (canon.startsWith(root + "/")) ? canon : QString();
    }

    QFileInfoList listFiles(const QString& dir) const {
        QDir d = (dir == "normal") ? normal_ : QDir(eventPath_);
        auto files = d.entryInfoList({"*.mp4"}, QDir::Files);
        std::sort(files.begin(), files.end(), [](const QFileInfo& a, const QFileInfo& b){
            return a.lastModified() < b.lastModified();
        });
        return files;
    }

    bool removeFile(const QString& dir, const QString& name) const {
        const QString p = pathFor(dir, name);
        return !p.isEmpty() && QFile::remove(p);
    }

		// ================= 删除（相册） =================
    // 删除视频(dir="normal"/"event")，连带删除配对缩略图
    bool deleteVideo(const QString& dir, const QString& name) const {
        const QString p = pathFor(dir, name);
        if (p.isEmpty()) return false;
        if (dir == "event")     // 解锁=锁语义的反向操作(恢复moveToEvent剥夺的写权限)。
            QFile::setPermissions(p, QFile::permissions(p)   // 注: POSIX的unlink只看
                    | QFile::WriteOwner | QFile::WriteUser   // 父目录写权限,这里显式
                    | QFile::WriteGroup | QFile::WriteOther);// 解锁是为语义对称/跨平台
        const bool ok = QFile::remove(p);
        if (ok)  // 配对封面一并清理;直接拼rootPath_而非thumbDir(),避免误建目录
            QFile::remove(rootPath_ + "/thumb/"
                          + QFileInfo(name).completeBaseName() + ".jpg");
        return ok;
    }

    // 删除图片(仅限thumb/下纯jpg文件名,防路径穿越)
    bool deletePhoto(const QString& name) const {
        if (name.contains('/') || name.contains('\\')) return false;
        if (!name.endsWith(QStringLiteral(".jpg"))) return false;
        return QFile::remove(rootPath_ + "/thumb/" + name);
    }
private:
    QString rootPath_, eventPath_;
    QDir normal_;
};

#endif // STORAGEMANAGER_H
