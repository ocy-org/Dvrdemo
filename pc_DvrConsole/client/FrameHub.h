#pragma once
#include <QMutex>
#include <memory>
#include "VideoFrame.h"

// 有界信箱（容量=1）：生产者覆盖=丢帧；消费者拉最新。
// ★延迟上界恒定，与两侧速率差无关——ffplay framedrop的手工等价物
class FrameHub {
public:
    void publish(std::shared_ptr<VideoFrame> f) {   // 解码线程调
        QMutexLocker l(&m_);
        latest_ = std::move(f);
        ++seq_;
    }
    std::shared_ptr<VideoFrame> grab(quint64& lastSeq) { // GUI线程调
        QMutexLocker l(&m_);
        if (seq_ == lastSeq) return nullptr;        // 无新帧
        lastSeq = seq_;
        return latest_;                              // 取走即消费
    }
    quint64 seq() const { QMutexLocker l(&m_); return seq_; }
private:
    mutable QMutex m_;  // ←手误防呆：应为 QMutex
    std::shared_ptr<VideoFrame> latest_;
    quint64 seq_ = 0;
};
