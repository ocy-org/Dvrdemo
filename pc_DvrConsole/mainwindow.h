#pragma once
#include <QMainWindow>
#include <QComboBox>
#include <QSpinBox>
#include <QPushButton>
#include <QLabel>
#include <QTabWidget>
#include <QTextBrowser>
#include <QJsonObject>
// 职责定位：
//     1) DeviceClient   —— TCP 协议链路（DV 帧收发，控制命令 + 状态/事件下行），端口9001
//     2) VideoWorker    —— RTSP 拉流 + YUV 解码线程，端口8554
//     3) DeviceDiscoverer —— UDP 广播搜索设备（顶栏「搜索设备」按钮的后端，当前预留）
//     4）YuvGLWidget —— 视频显示窗口，接收解码线程吐出的 YUV 帧并 OpenGL 渲染
class DeviceClient;     // TCP 协议客户端
class VideoWorker;      // RTSP 拉流 + 解码线程
class DeviceDiscoverer; // UDP 设备搜索器
class YuvGLWidget;      // YUV OpenGL 视频显示控件

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

protected:
    void closeEvent(QCloseEvent* e) override;// 关闭窗口事件

private:
    void buildUi();
    void wireSignals();// 把控件 clicked、设备信号、解码线程信号连到槽/lambda
    void loadSettings();// 从 QSettings 读取上次保存的 IP 历史与端口
    void rememberIp(const QString& ip);// 把本次使用的 IP 顶置进下拉历史（去重）
    void appendLog(const QString& text, const QString& kind = {});//写日志
    void setConnected(bool on);// 切换「连接/断开」按钮可用性与状态栏连接标签
    void applyRecordState(bool on, const QString& src);  // 录像开(on)/关(!on)
    void onEvent(const QJsonObject& o);                  // 事件入口：解析一个下行事件 JSON（含 _t 类型），分发到对应处理；真机与回放共用。
    QString currentIp() const;// 取 IP 下拉框当前文本

    DeviceClient*     dev_ = nullptr;// TCP 协议客户端：收发 DV 帧
    VideoWorker*      worker_ = nullptr;// RTSP 拉流+解码线程对象
    DeviceDiscoverer* discoverer_ = nullptr;// UDP 搜索器（当前按钮预留未启用）
    YuvGLWidget*      video_ = nullptr;// 视频显示控件（接收解码帧）

    QComboBox*   ipBox_ = nullptr; // 设备 IP（可编辑 + 历史下拉）
    QSpinBox*    portProto_ = nullptr;// 协议(TCP控制)端口，默认 9001
    QSpinBox*    portRtsp_ = nullptr; // RTSP(视频)端口，默认 8554
    QPushButton* btnSearch_ = nullptr;// 搜索设备（待对接 DeviceDiscoverer）
    QPushButton* btnConnect_ = nullptr; // 连接设备
    QPushButton* btnDisconnect_ = nullptr;// 断开设备

    QPushButton* btnRecOn_ = nullptr;// 开始录像(ACC_ON)
    QPushButton* btnRecOff_ = nullptr; // 停止录像(ACC_OFF)
    QPushButton* btnLock_ = nullptr;// 锁定当前录像段

    QTextBrowser* logView_ = nullptr;// 彩色事件日志区
    QLabel* lblVideo_ = nullptr;// 状态栏左：视频流 fps/kbps
    QLabel* lblConn_ = nullptr;// 状态栏右：连接状态文字

    bool cmdAcked_    = true;    // 最近一条命令是否收到应答（3秒超时用）
    bool pingProbed_ = false;   // PING探针只发一次
};
