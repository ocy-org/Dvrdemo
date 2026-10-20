#ifndef SEGMENTMUXER_H
#define SEGMENTMUXER_H
#include <media/NdkMediaMuxer.h>
#include <media/NdkMediaCodec.h>     // AMediaCodecBufferInfo
#include <media/NdkMediaFormat.h>
#include <QString>
#include <cstdio>
#include <unistd.h>                  // fileno

/** 一个分段的封装器。
 *  ⚠️ NDK差异：AMediaMuxer 用 fd 而非路径构造。
 *  ⚠️ 释放顺序必须：先 AMediaMuxer_delete，再 fclose，反了会写坏文件。 */
class SegmentMuxer {
public:
    bool open(const QString& path) {
        path_ = path.toStdString();
        file_ = fopen(path_.c_str(), "wb");
        if (!file_) return false;
        muxer_ = AMediaMuxer_new(fileno(file_), AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4);
        return muxer_ != nullptr;
    }

    bool start(AMediaFormat* fmt) {              // fmt必须含csd-0/csd-1(SPS/PPS)
        if (!muxer_) return false;
        track_ = AMediaMuxer_addTrack(muxer_, fmt);
        if (track_ < 0) return false;
        started_ = (AMediaMuxer_start(muxer_) == AMEDIA_OK);
        return started_;
    }

    bool    isStarted() const { return started_; }
    QString filePath()  const { return QString::fromStdString(path_); }

    void write(const uint8_t* data, const AMediaCodecBufferInfo& info) {
        if (!started_) return;
        if (startPtsUs_ < 0) startPtsUs_ = info.presentationTimeUs;
        AMediaCodecBufferInfo rel = info;        // PTS相对化：每段从0开始
        rel.presentationTimeUs = info.presentationTimeUs - startPtsUs_;
        AMediaMuxer_writeSampleData(muxer_, track_, data + info.offset, &rel);
    }

    qint64 stop() {
        if (muxer_) {
            if (started_) AMediaMuxer_stop(muxer_);
            AMediaMuxer_delete(muxer_);          // ① 先删muxer
            muxer_ = nullptr; started_ = false;
        }
        qint64 size = 0;
        if (file_) {                             // ② 再关文件
            fflush(file_);
            size = ftell(file_);
            fclose(file_); file_ = nullptr;
        }
        return size;
    }

private:
    std::string    path_;
    FILE*          file_    = nullptr;
    AMediaMuxer*   muxer_   = nullptr;
    ssize_t        track_   = -1;
    bool           started_ = false;
    int64_t        startPtsUs_ = -1;
};

#endif // SEGMENTMUXER_H
