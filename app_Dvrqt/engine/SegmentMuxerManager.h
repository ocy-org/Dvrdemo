#ifndef SEGMENTMUXERMANAGER_H
#define SEGMENTMUXERMANAGER_H

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <atomic>
#include <memory>
#include <functional>
#include <QString>
#include <QFile>
#include "SegmentMuxer.h"
#include "StorageManager.h"
#include "Config.h"

/** 编码器永不停，只在IDR帧边界切换Muxer（"秒切"）。
 * 切换触发源（都要求isKey才真正切）：
 * ① 周期切换：segDur >= 60s
 * ② 紧急锁档：requestLock() 标记 → 切段后文件进 event/ 并置只读 */
class SegmentMuxerManager {
public:
    using FinishedCb = std::function<void(const QString& file, qint64 durUs, qint64 bytes)>;
    using LockedCb   = std::function<void(const QString& file, qint64 durUs, qint64 bytes, float g)>;

    SegmentMuxerManager(const StorageManager& s, FinishedCb cb, LockedCb lcb)
        : storage_(s), cb_(std::move(cb)), lcb_(std::move(lcb)) {}

    void attach(AMediaCodec* codec) { codec_ = codec; }
    QString currentFilePath() const { return current_ ? current_->filePath() : QString(); }
    void openFirstSegment() { openSegment(); }

    // 紧急锁档 —— 下一个IDR切段后，文件移入event/并置只读
    void requestLock(float g) {
        lastG_ = g;
        pendingLock_ = true;
        pendingRotate_ = true;
    }

    void write(const uint8_t* data, const AMediaCodecBufferInfo& info) {
        if (!current_ || !current_->isStarted()) return;
        if (segStartUs_ < 0) segStartUs_ = info.presentationTimeUs;
        lastPtsUs_ = info.presentationTimeUs;
        const bool isKey = (info.flags & AMEDIACODEC_BUFFER_FLAG_KEY_FRAME) != 0;
        const qint64 segDur = lastPtsUs_ - segStartUs_;
        const bool due = segDur >= Config::SEGMENT_DURATION_US;
        if (isKey && (due || pendingRotate_.exchange(false))) {
            finishCurrent(segDur);
            openSegment();
            segStartUs_ = lastPtsUs_;     // 新段起点 = 当前IDR帧
        }
        current_->write(data, info);
    }

    void release() {
        pendingLock_ = false;      // 停机时未及切换的锁档请求作废
        pendingRotate_ = false;
        if (!current_) return;
        const QString f = current_->filePath();
        const qint64 durUs = (segStartUs_ >= 0 && lastPtsUs_ > segStartUs_)
                             ? (lastPtsUs_ - segStartUs_) : 0;
        const qint64 bytes = current_->stop();
        current_.reset();
        if (bytes > 0 && durUs > 0) {
            cb_(f, durUs, bytes);
            storage_.enforceQuota();
        } else {
            QFile::remove(f);
        }
    }

private:
    void openSegment() {
        auto m = std::make_unique<SegmentMuxer>();
        if (!m->open(storage_.newNormalFile())) return;
        AMediaFormat* fmt = AMediaCodec_getOutputFormat(codec_);
        bool ok = m->start(fmt);
        AMediaFormat_delete(fmt);
        if (!ok) return;
        current_ = std::move(m);
    }

    void finishCurrent(qint64 durUs) {
        const QString f = current_->filePath();
        const qint64 bytes = current_->stop();

        if (pendingLock_.exchange(false)) {       // 锁档：先搬走，再配额
            const QString ev = storage_.moveToEvent(f);
            if (!ev.isEmpty()) {
                storage_.enforceQuota();          // 只清normal/，event/幸存
                if (lcb_) lcb_(ev, durUs, bytes, lastG_.load());
                return;
            }
            // 移动失败(极端) → 降级为普通段，数据不丢
        }
        storage_.enforceQuota();
        cb_(f, durUs, bytes);
    }

    const StorageManager& storage_;
    FinishedCb cb_;
    LockedCb lcb_;
    AMediaCodec* codec_ = nullptr;
    std::unique_ptr<SegmentMuxer> current_;
    std::atomic<bool> pendingRotate_{false};
    std::atomic<bool> pendingLock_{false};
    std::atomic<float> lastG_{0.0f};
    qint64 segStartUs_ = -1;
    qint64 lastPtsUs_ = -1;
};

#endif // SEGMENTMUXERMANAGER_H
