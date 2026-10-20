#ifndef VIDEOENCODER_H
#define VIDEOENCODER_H

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <android/native_window.h>
#include "Config.h"


// =====================================================================
// VideoEncoder.h -- 视频编码器
// =====================================================================

class VideoEncoder {
public:
    /** 创建编码器并返回输入Surface（Surface输入=零拷贝，相机帧直达VPU硬件） */
    bool create(ANativeWindow** outWindow) {
        AMediaFormat* fmt = AMediaFormat_new();
        AMediaFormat_setString(fmt, AMEDIAFORMAT_KEY_MIME, "video/avc");
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_WIDTH,        Config::W);
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT,       Config::H);
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_BIT_RATE,     Config::BITRATE);
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_FRAME_RATE,   Config::FPS);
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, Config::I_FRAME);
        // 0x7F000789 = COLOR_FormatSurface。
        // NDK头文件未定义此常量，取Java层CodecCapabilities的同值魔数：
        AMediaFormat_setInt32(fmt, AMEDIAFORMAT_KEY_COLOR_FORMAT, 2130708361);

        codec_ = AMediaCodec_createEncoderByType("video/avc");
        bool ok = codec_ &&
            AMediaCodec_configure(codec_, fmt, nullptr, nullptr,
                                  AMEDIACODEC_CONFIGURE_FLAG_ENCODE) == AMEDIA_OK &&
            AMediaCodec_createInputSurface(codec_, outWindow) == AMEDIA_OK;
        AMediaFormat_delete(fmt);
        if (!ok) return false;

        window_ = *outWindow;
        ANativeWindow_acquire(window_);          // 持引用，防相机还在写时被提前销毁
        return AMediaCodec_start(codec_) == AMEDIA_OK;
    }

    AMediaCodec* get() const { return codec_; }

    /** 主动请求一个IDR帧（NDK键名为字符串"request-sync"） */
    void requestSyncFrame() {
        if (!codec_) return;
        AMediaFormat* p = AMediaFormat_new();
        AMediaFormat_setInt32(p, "request-sync", 0);
        AMediaCodec_setParameters(codec_, p);
        AMediaFormat_delete(p);
    }

    void release() {
        if (codec_)  { AMediaCodec_stop(codec_); AMediaCodec_delete(codec_); codec_ = nullptr; }
        if (window_) { ANativeWindow_release(window_); window_ = nullptr; }
    }

private:
    AMediaCodec*   codec_  = nullptr;
    ANativeWindow* window_ = nullptr;
};

#endif // VIDEOENCODER_H
