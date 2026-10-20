#ifndef RECORDINGENGINE_H
#define RECORDINGENGINE_H
=====================================================================
// RecordingEngine：行车记录仪的「后端总控」(C++/Qt)
//   职责：把摄像头采集、编码、分段封装、TCP/私有协议服务、RTSP 服务、
//         碰撞(G传感器)告警、媒体库相册等所有子模块聚合到一个对象里，
//         再把这套能力「翻译」成 QML 能直接用的接口（属性/方法/信号）。
//   定位：它是前后端分离里的「后端」——不含任何界面，只暴露能力。
//         QML 通过 main.cpp 注入的全局名(如 RecEngine)访问本对象。
// =====================================================================
#include <QObject>
#include <QString>
#include <atomic>
#include <memory>
#include <thread>
#include "StorageManager.h"
#include "VideoEncoder.h"
#include "CameraController.h"
#include "SegmentMuxerManager.h"
#include "DvrTcpServer.h"
#include "DvrProtoServer.h"
#include "RtspServer.h"
#include "GSensor.h"

class MediaLibrary;                         // 前置声明

class RecordingEngine : public QObject {
    Q_OBJECT
    // ------- 暴露给 QML 的属性(READ=读函数, NOTIFY=变更信号) -------
    // statusText：当前工作状态文本(如 IDLE/RECORDING)，绑定它可做状态灯/文字。
    //   NOTIFY statusChanged → 后端一 setState 就 emit，前端 Label 自动刷新(响应式)。
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged)
    // 下面三个 URL 是「常量属性」(CONSTANT)：构造时拼好、运行期不变，
    //   QML 读一次即定型，不需要 NOTIFY，也不会反复触发绑定重算。
    Q_PROPERTY(QString tcpUrl   READ tcpUrl   CONSTANT)// TCP 服务地址
    Q_PROPERTY(QString protoUrl READ protoUrl CONSTANT)// 私有协议服务地址
    Q_PROPERTY(QString rtspUrl  READ rtspUrl  CONSTANT)// RTSP 流地址(播放器拉流用)
    // lastAlarm：最近一次碰撞告警文本，NOTIFY → 前端可弹窗/红字提示。
    Q_PROPERTY(QString lastAlarm READ lastAlarm NOTIFY lastAlarmChanged)
    // 把「媒体库」也作为嵌套后端对象暴露给 QML。
    //   返回 QObject* 是为了让 QML 通过属性访问它的子属性/方法(二层分离)：
    //   QML 里写 RecEngine.mediaLibrary.photos / .deleteVideo(...) 即可直接用相册能力。
    Q_PROPERTY(QObject* mediaLibrary READ mediaLibraryObj CONSTANT)
public:
    explicit RecordingEngine(QObject* parent = nullptr);
    ~RecordingEngine() override;

    QString statusText() const { return state_; }
    QString tcpUrl()   const { return tcpUrl_; }
    QString protoUrl() const { return protoUrl_; }
    QString rtspUrl()  const { return rtspUrl_; }
    QString lastAlarm() const { return lastAlarm_; }
    QObject* mediaLibraryObj() const ;  // ★

		// ------- Q_INVOKABLE：标记为「QML 可直接调用的方法」(前端→后端 主动发命令) -------
    Q_INVOKABLE void start();// 开始录像(启动采集→编码→分段→各服务)
    Q_INVOKABLE void stop();// 停止录像(停采集、封最后一段、关服务)
    Q_INVOKABLE void lockCurrentSegment();// 手动锁定当前段：把正在写的普通段升级为

signals:
    void statusChanged(const QString& s);
    void segmentFinished(const QString& name, qint64 durUs, qint64 bytes);
    void errorOccurred(const QString& msg);
    void lastAlarmChanged();

private:
    void onCameraReady(const QString& err);
    void drainLoop();
    void setState(const QString& s);
    void onGEvent(float g);
    void makeThumb(const QString& video);

    std::unique_ptr<StorageManager> storage_;
    std::unique_ptr<DvrTcpServer>   tcp_;
    std::unique_ptr<DvrProtoServer> proto_;
    std::unique_ptr<RtspServer>     rtsp_;
    std::unique_ptr<GSensor>        gsensor_;
    std::unique_ptr<MediaLibrary>   mediaLibrary_;   // ★ 优化篇
    VideoEncoder encoder_;
    CameraController camera_;
    std::unique_ptr<SegmentMuxerManager> muxerMgr_;
    std::atomic<bool> running_{false};
    std::thread drain_;
    QString tcpUrl_;
    QString protoUrl_;
    QString rtspUrl_;
    QString lastAlarm_;
    QString state_ = "IDLE";
};

#endif // RECORDINGENGINE_H
