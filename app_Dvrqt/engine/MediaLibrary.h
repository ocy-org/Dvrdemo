#ifndef MEDIALIBRARY_H
#define MEDIALIBRARY_H

// =====================================================================
// 优化篇：回放/相册 —— 为QML提供三类媒体列表
//   normalVideos / eventVideos: 已封存mp4（file://URL、封面、大小、时间）
//   photos: thumb/ 全部jpg
// 缩略图策略：无配对jpg的mp4自动排队生成（含历史旧文件），后台线程完成
// 线程模型：列表扫描在主线程(QML调refresh)；缩略图生成在专属工作线程
//   (QJniEnvironment把工作线程挂到JVM——跨线程只传QString，纪律同第3周)
// =====================================================================

#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <QUrl>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QMutex>
#include <QWaitCondition>
#include <QJniEnvironment>
#include <QJniObject>
#include <QMetaObject>
#include <functional>
#include <thread>
#include "StorageManager.h"

class MediaLibrary : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList normalVideos READ normalVideos NOTIFY changed)
    Q_PROPERTY(QVariantList eventVideos  READ eventVideos  NOTIFY changed)
    Q_PROPERTY(QVariantList photos       READ photos       NOTIFY changed)
public:
    explicit MediaLibrary(const StorageManager* storage, QObject* parent = nullptr)
        : QObject(parent), storage_(storage) {
        worker_ = std::thread([this]{ workerLoop(); });
    }
    ~MediaLibrary() override {
        { QMutexLocker l(&m_); quit_ = true; }
        cv_.wakeAll();
        if (worker_.joinable()) worker_.join();
    }

    using FileFn = std::function<QString()>;
    // 注入"当前正在写的段"查询：列表必须排除它（moov未封口，播必失败）
    void setCurrentFileProvider(FileFn fn) { currentFn_ = std::move(fn); }

    Q_INVOKABLE void refresh() {
        const QString cur = currentFn_ ? currentFn_() : QString();
        normal_ = scanVideos(storage_->normalDir(), cur);
        event_  = scanVideos(storage_->eventDir(),  QString());
        photos_ = scanPhotos();
        emit changed();
    }

    Q_INVOKABLE void requestThumb(const QString& video) { enqueueThumb(video); }
		// ---- ★ 删除（QML入口；统一走"删→refresh→changed→绑定重算"） ----
    Q_INVOKABLE bool deleteVideo(const QString& kind, const QString& name) {
        const bool ok = storage_->deleteVideo(kind, name);
        refresh();
        return ok;
    }
    Q_INVOKABLE bool deletePhoto(const QString& name) {
        const bool ok = storage_->deletePhoto(name);
        refresh();
        return ok;
    }
    QVariantList normalVideos() const { return normal_; }
    QVariantList eventVideos()  const { return event_; }
    QVariantList photos()       const { return photos_; }

signals:
    void changed();

private:
    // ---- 列表扫描（主线程） ----
    QVariantList scanVideos(const QString& dirPath, const QString& exclude) {
        QVariantList out;
        const QDir d(dirPath);
        auto files = d.entryInfoList({"*.mp4"}, QDir::Files,
                                     QDir::Name | QDir::Reversed);   // 新的在前
        for (const QFileInfo& fi : files) {
            if (fi.absoluteFilePath() == exclude) continue;          // 正在写的段
            QVariantMap m;
            m["name"] = fi.fileName();
            m["url"]  = QUrl::fromLocalFile(fi.absoluteFilePath()).toString();
            const QString jpg = storage_->thumbDir() + "/"
                              + fi.completeBaseName() + ".jpg";
            m["thumb"] = QFile::exists(jpg)
                         ? QUrl::fromLocalFile(jpg).toString() : QString();
            if (!QFile::exists(jpg)) enqueueThumb(fi.absoluteFilePath()); // 自动补齐
            m["size"] = QString::number(fi.size() / 1048576.0, 'f', 1) + " MB";
            m["date"] = prettyDate(fi.fileName());
            out.append(m);
        }
        return out;
    }

    QVariantList scanPhotos() {
        QVariantList out;
        QDir d(storage_->thumbDir());
        auto files = d.entryInfoList({"*.jpg"}, QDir::Files,
                                     QDir::Name | QDir::Reversed);
        for (const QFileInfo& fi : files) {
            const QString base = fi.completeBaseName();
            QVariantMap m;
            m["name"] = fi.fileName();
            m["url"]  = QUrl::fromLocalFile(fi.absoluteFilePath()).toString();
            m["date"] = prettyDate(fi.fileName());
            m["paired"] = QFile::exists(storage_->normalDir() + "/" + base + ".mp4")
                       || QFile::exists(storage_->eventDir()  + "/" + base + ".mp4");
            out.append(m);
        }
        return out;
    }

    // 20261015_124538302_front.mp4 → "2026-10-15 12:45:38"
    static QString prettyDate(const QString& fname) {
        const QString stem = QFileInfo(fname).completeBaseName();
        if (stem.size() < 15) return fname;
        const QString d = stem.left(8), t = stem.mid(9, 6);
        return QString("%1-%2-%3 %4:%5:%6").arg(d.mid(0,4), d.mid(4,2), d.mid(6,2),
                                                t.mid(0,2), t.mid(2,2), t.mid(4,2));
    }

    // ---- 缩略图工作线程 ----
    void enqueueThumb(const QString& video) {
        { QMutexLocker l(&m_);
          if (jobs_.contains(video)) return;      // 去重
          jobs_.append(video); }
        cv_.wakeAll();
    }

    void workerLoop() {
        QJniEnvironment env;                      // 把本线程挂到Java VM（JNI前提）
        for (;;) {
            QString job;
            { QMutexLocker l(&m_);
              while (!quit_ && jobs_.isEmpty()) cv_.wait(&m_);
              if (quit_) return;
              job = jobs_.takeFirst(); }
            const QString jpg = storage_->thumbDir() + "/"
                              + QFileInfo(job).completeBaseName() + ".jpg";
            if (QFile::exists(jpg)) continue;     // 双检：可能已被别的周期生成
            const jboolean ok = QJniObject::callStaticMethod<jboolean>(
                "org/qtproject/example/Dvrqt/ThumbHelper",
                "make", "(Ljava/lang/String;Ljava/lang/String;J)Z",
                QJniObject::fromString(job).object(),
                QJniObject::fromString(jpg).object(), (jlong)1000000);
            if (ok) notifyRefresh();              // 成功才刷新UI（失败不刷防循环）
        }
    }

    void notifyRefresh() {                        // 工作线程 → 主线程重扫列表
        QMetaObject::invokeMethod(this, [this]{ refresh(); }, Qt::QueuedConnection);
    }

    const StorageManager* storage_;
    FileFn currentFn_;
    QVariantList normal_, event_, photos_;
    std::thread worker_;
    QMutex m_;
    QWaitCondition cv_;
    QStringList jobs_;
    bool quit_ = false;
};

#endif // MEDIALIBRARY_H
