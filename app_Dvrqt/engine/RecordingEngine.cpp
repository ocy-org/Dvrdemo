#if defined(Q_OS_ANDROID)
#include <QJniObject>
#include <QGuiApplication>
#endif

#include "RecordingEngine.h"
#include "MediaLibrary.h"                       // ★ 优化篇
#include <QGuiApplication>
#include <QPermissions>
#include <QStandardPaths>
#include <QFileInfo>
#include <android/log.h>
#include <QDebug>

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "DVR", __VA_ARGS__)

RecordingEngine::RecordingEngine(QObject* parent) : QObject(parent) {
    const QString root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/DVR";
    storage_ = std::make_unique<StorageManager>(root);
    LOG("storage root: %s", root.toUtf8().constData());

#ifdef Q_OS_ANDROID
    QJniObject::callStaticMethod<void>(
        "org/qtproject/example/Dvrqt/WifiLockHelper",
        "acquire", "(Landroid/content/Context;)V",
        QNativeInterface::QAndroidApplication::context());
#endif

    tcp_ = std::make_unique<DvrTcpServer>(*storage_);
    tcp_->setStateProvider([this]{ return state_; });
    tcp_->setActions([this]{ start(); }, [this]{ stop(); });
    if (tcp_->start(9000)) {
        tcpUrl_ = tcp_->url();
        LOG("tcp server: %s", tcpUrl_.toUtf8().constData());
    }

    proto_ = std::make_unique<DvrProtoServer>(*storage_);
    proto_->setStateProvider([this]{ return state_; });
    proto_->setControlHandlers([this]{ start(); }, [this]{ stop(); },
                               [this]{ lockCurrentSegment(); });
    proto_->setGEventHandler([this](float g){ onGEvent(g); });
    connect(this, &RecordingEngine::statusChanged, proto_.get(),
            [this](const QString& s){ proto_->pushState(s); });
    connect(this, &RecordingEngine::segmentFinished, proto_.get(),
            [this](const QString& n, qint64 d, qint64 b){ proto_->pushSegment(n, d, b); });
    connect(this, &RecordingEngine::errorOccurred, proto_.get(),
            [this](const QString& m){ proto_->pushError(m); });
    if (proto_->start(9001)) {
        protoUrl_ = proto_->url();
        LOG("proto server: %s", protoUrl_.toUtf8().constData());
    }

    rtsp_ = std::make_unique<RtspServer>();
    rtsp_->setIdrRequester([this]{
        if (state_ == "DRIVING") encoder_.requestSyncFrame();
    });
    if (rtsp_->start(8554)) {
        rtspUrl_ = rtsp_->url();
        LOG("rtsp server: %s", rtspUrl_.toUtf8().constData());
    }

    // ---- ★ 优化篇：媒体库（相册列表 + 缩略图工作线程） ----
    mediaLibrary_ = std::make_unique<MediaLibrary>(storage_.get(), this);
    mediaLibrary_->setCurrentFileProvider([this]{
        // 主线程访问muxerMgr_（refresh只在主线程跑），无竞态
        return muxerMgr_ ? muxerMgr_->currentFilePath() : QString();
    });
    connect(this, &RecordingEngine::segmentFinished, mediaLibrary_.get(),
            [this](const QString&, qint64, qint64){ mediaLibrary_->refresh(); }); // ★ 每收好一段→列表自动刷新

    gsensor_ = std::make_unique<GSensor>();
    gsensor_->start(2.0f, [this](float g) {
        QMetaObject::invokeMethod(this, [this, g]{ onGEvent(g); },
                                  Qt::QueuedConnection);
    });
    LOG("gsensor: threshold 2.0g armed");
}

RecordingEngine::~RecordingEngine() {
    if (gsensor_) gsensor_->stop();
    stop();
}

void RecordingEngine::setState(const QString& s) {
    state_ = s;
    emit statusChanged(s);
}

void RecordingEngine::start() {
    qWarning() << "[dvr] >>> start() entered";
    if (state_ != "IDLE") return;

    if (qGuiApp->checkPermission(QCameraPermission{}) != Qt::PermissionStatus::Granted) {
        qGuiApp->requestPermission(QCameraPermission{}, this, [this] { start(); });
        return;
    }
    setState("STARTING");

    ANativeWindow* win = nullptr;
    if (!encoder_.create(&win)) {
        setState("IDLE");
        emit errorOccurred("encoder create failed");
        return;
    }
    camera_.open(win, [this](const QString& err) { onCameraReady(err); });
}

void RecordingEngine::onCameraReady(const QString& err) {
    if (state_ != "STARTING") {
        QMetaObject::invokeMethod(this, [this] { camera_.close(); encoder_.release(); },
                                  Qt::QueuedConnection);
        return;
    }
    if (!err.isEmpty()) {
        QMetaObject::invokeMethod(this, [this] { encoder_.release(); },
                                  Qt::QueuedConnection);
        setState("IDLE");
        emit errorOccurred(err);
        return;
    }
    muxerMgr_ = std::make_unique<SegmentMuxerManager>(*storage_,
            [this](const QString& f, qint64 dur, qint64 bytes) {
                emit segmentFinished(f, dur, bytes);
            },
            [this](const QString& f, qint64 dur, qint64 bytes, float g) {
                QMetaObject::invokeMethod(this, [this, f, dur, bytes, g] {
                    makeThumb(f);
                    emit segmentFinished(f, dur, bytes);
                }, Qt::QueuedConnection);
            });
    muxerMgr_->attach(encoder_.get());
    running_ = true;
    drain_ = std::thread(&RecordingEngine::drainLoop, this);
    setState("DRIVING");
}

void RecordingEngine::drainLoop() {
    AMediaCodec* codec = encoder_.get();
    bool firstFormatHandled = false;
    while (running_) {
        AMediaCodecBufferInfo info{};
        ssize_t idx = AMediaCodec_dequeueOutputBuffer(codec, &info, 10000);
        if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            if (!firstFormatHandled) {
                muxerMgr_->openFirstSegment();
                firstFormatHandled = true;
            }
        } else if (idx == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
        } else if (idx >= 0) {
            size_t outSize = 0;
            uint8_t* data = AMediaCodec_getOutputBuffer(codec, idx, &outSize);
            if (data && info.size > 0) {
                if (info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG) {
                    rtsp_->setCodecConfig(data, int(info.size));
                } else {
                    muxerMgr_->write(data, info);
                    rtsp_->pushFrame(data, int(info.size), info.presentationTimeUs,
                            (info.flags & AMEDIACODEC_BUFFER_FLAG_KEY_FRAME) != 0);
                }
            }
            AMediaCodec_releaseOutputBuffer(codec, idx, false);
        }
    }
}

void RecordingEngine::stop() {
    if (state_ == "IDLE") return;
    running_ = false;
    if (drain_.joinable()) drain_.join();
    camera_.close();
    if (muxerMgr_) muxerMgr_->release();
    encoder_.release();
    muxerMgr_.reset();
    setState("IDLE");
}

void RecordingEngine::onGEvent(float g) {
    if (state_ != "DRIVING" || !muxerMgr_) return;
    LOG("G-EVENT dev=%.2fg", (double)g);
    lastAlarm_ = QString("G %1g").arg(g, 0, 'f', 2);
    emit lastAlarmChanged();
    proto_->pushAlarm(g);
    encoder_.requestSyncFrame();
    muxerMgr_->requestLock(g);
}

void RecordingEngine::lockCurrentSegment() {
    if (state_ != "DRIVING" || !muxerMgr_) return;
    encoder_.requestSyncFrame();
    muxerMgr_->requestLock(0.0f);
}

// ★ 优化篇：缩略图生成转交媒体库工作线程（主线程不再被JNI阻塞）
void RecordingEngine::makeThumb(const QString& video) {
    if (mediaLibrary_) mediaLibrary_->requestThumb(video);
}

QObject* RecordingEngine::mediaLibraryObj() const { return mediaLibrary_.get(); }
