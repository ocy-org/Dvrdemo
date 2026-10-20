#ifndef CONFIG_H
#define CONFIG_H
#include <QtGlobal>

struct Config {
    static constexpr int     W          = 1920;
    static constexpr int     H          = 1080;
    static constexpr int     FPS        = 30;
    static constexpr int     BITRATE    = 8'000'000;
    static constexpr int     I_FRAME    = 1;              // 1秒一个IDR
    static constexpr qint64  SEGMENT_DURATION_US = 60'000'000LL; // 60秒/段
    static constexpr qint64  KEEP_FREE_MB = 500;          // 剩余空间水位线
    static constexpr float G_THRESHOLD_G = 2.0f;   // G事件阈值(偏离1g的g数)
};
#endif // CONFIG_H
