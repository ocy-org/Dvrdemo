#ifndef GSENSOR_H
#define GSENSOR_H

// =====================================================================
// G-sensor（加速度传感器）碰撞/急刹检测
// 物理量：手机任意姿态下重力方向未知，只有"模长"可用——
//   |a| = sqrt(x^2+y^2+z^2)，静止时约等于1g(9.81 m/s^2)
// 事件指标：dev = | |a|/9.81 - 1 |，即偏离静止重力多少g
// 抗误触发：连续3个采样(50Hz下60ms)超阈值才算事件；触发后5秒冷却
// 线程模型：独占线程跑 ALooper 轮询(NDK的事件循环)；回调由调用方投递主线程
// NDK兼容：
//   - pollAll已在新NDK标记unavailable → 用pollOnce(轮询模式下行为一致)
//   - 队列ident是调用者自选整数(1/2/3/-2为框架保留，旧LOOPER_ID_USER已删) → 自定义常量
// =====================================================================

#include <android/sensor.h>
#include <android/looper.h>
#include <atomic>
#include <thread>
#include <chrono>
#include <cmath>
#include <functional>

class GSensor {
public:
    using Cb = std::function<void(float devG)>;   // devG = 事件强度(偏差g数)

    bool start(float thresholdG, Cb cb) {
        thresholdG_ = thresholdG;
        cb_ = std::move(cb);
        running_ = true;
        thread_ = std::thread([this]{ loop(); });
        return true;
    }

    void stop() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
    }

private:
    // ★ 修复：自选队列ident。pollOnce唤醒时原样返回此值；
    // 避开框架保留的1/2/3和-2，也避开poll自身的返回值(-1~-4)
    static constexpr int SENSOR_LOOPER_ID = 100;

    void loop() {
        // ALooper：NDK版的"每线程一个事件循环"(类似Qt的事件循环但更底层)
        ALooper* looper = ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
        ASensorManager* mgr =
#if __ANDROID_API__ >= 26
            ASensorManager_getInstanceForPackage("org.qtproject.example.Dvrqt");
#else
            ASensorManager_getInstance();
#endif
        if (!looper || !mgr) return;

        ASensorEventQueue* q = ASensorManager_createEventQueue(
                mgr, looper, SENSOR_LOOPER_ID, nullptr, nullptr);   // ★ 修复1
        const ASensor* acc =
                ASensorManager_getDefaultSensor(mgr, ASENSOR_TYPE_ACCELEROMETER);
        if (!q || !acc) {
            if (q) ASensorManager_destroyEventQueue(mgr, q);
            return;
        }
        ASensorEventQueue_enableSensor(q, acc);
        ASensorEventQueue_setEventRate(q, acc, 20000);      // 20000us=20ms=50Hz

        int aboveCnt = 0;
        while (running_) {
            // pollAll→pollOnce：差异仅在回调处理数量；非回调轮询模式下行为一致
            const int id = ALooper_pollOnce(500, nullptr, nullptr, nullptr);
            if (!running_) break;
            if (id != SENSOR_LOOPER_ID) continue;           // ★ 修复2：超时(-3)等→查退出标志

            ASensorEvent e;
            while (ASensorEventQueue_getEvents(q, &e, 1) > 0) {
                const float gx = e.acceleration.x;          // 单位 m/s^2，含重力
                const float gy = e.acceleration.y;
                const float gz = e.acceleration.z;
                const float dev = std::fabs(
                        std::sqrt(gx*gx + gy*gy + gz*gz) / 9.81f - 1.0f);

                const auto now = std::chrono::steady_clock::now();
                if (now < cooldownUntil_) continue;         // 冷却期：只吞不判

                if (dev > thresholdG_) {
                    if (++aboveCnt >= 3) {                  // 3连击→真事件
                        aboveCnt = 0;
                        cooldownUntil_ = now + std::chrono::milliseconds(5000);
                        if (cb_) cb_(dev);                  // 调用方负责跨线程投递
                    }
                } else {
                    aboveCnt = 0;                           // 连击被打断→重新计数
                }
            }
        }
        ASensorEventQueue_disableSensor(q, acc);
#if __ANDROID_API__ >= 29
        ASensorManager_destroyEventQueue(mgr, q);   // 同时释放队列持有的looper引用
#else
        ALooper_release(looper);                    // 老API无destroy接口，兜底释放
#endif
    }

    float thresholdG_ = 2.0f;
    Cb cb_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::chrono::steady_clock::time_point cooldownUntil_{};
};

#endif // GSENSOR_H
