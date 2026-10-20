#include "mainwindow.h"
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QStatusBar>
#include <QCloseEvent>
#include <QSettings>
#include <QTime>
#include <QTimer>
#include <QStringList>
#include <QJsonDocument>

#include "client/DeviceClient.h"
#include "client/VideoWorker.h"
#include "client/DeviceDiscoverer.h"
#include "client/yuvglwidget.h"

// 状态值分类：1=录像中 0=未录像 -1=未识别（从问候帧命令表看，车机状态可能是IDLE）
static int classifyState(const QString& st) {
    const QStringList ON  = {"ACC_ON","ON","REC","RECORDING","START","1","TRUE"};
    const QStringList OFF = {"ACC_OFF","OFF","IDLE","STOP","STOPPED","STANDBY","0","FALSE"};
    for (const auto& s : ON)  if (st.compare(s, Qt::CaseInsensitive) == 0) return 1;
    for (const auto& s : OFF) if (st.compare(s, Qt::CaseInsensitive) == 0) return 0;
    return -1;
}

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("DVR 上位机");
    resize(1100, 640);

    dev_        = new DeviceClient(this);
    worker_     = new VideoWorker(this);
    discoverer_ = new DeviceDiscoverer(this);

    buildUi();
    wireSignals();
    loadSettings();
    setConnected(false);

    video_->setVisible(false);        // ★初始不显示——等状态确认ACC_ON才出现
    video_->attach(worker_->hub());   // 信箱对接：解码线程→显示控件
}

// ============================ 界面搭建 ============================
void MainWindow::buildUi() {
    // ---- 顶栏 ----
    auto* top = new QWidget;
    auto* tb = new QHBoxLayout(top);
    tb->setContentsMargins(8, 4, 8, 4);
    tb->addWidget(new QLabel("设备IP"));
    ipBox_ = new QComboBox; ipBox_->setEditable(true); ipBox_->setMinimumWidth(160);
    tb->addWidget(ipBox_);
    tb->addWidget(new QLabel("协议口"));
    portProto_ = new QSpinBox; portProto_->setRange(1, 65535); portProto_->setValue(9001);
    tb->addWidget(portProto_);
    tb->addWidget(new QLabel("RTSP口"));
    portRtsp_ = new QSpinBox; portRtsp_->setRange(1, 65535); portRtsp_->setValue(8554);
    tb->addWidget(portRtsp_);
    btnSearch_     = new QPushButton("搜索设备");
    btnConnect_    = new QPushButton("连接");
    btnDisconnect_ = new QPushButton("断开");
    tb->addWidget(btnSearch_);
    tb->addWidget(btnConnect_);
    tb->addWidget(btnDisconnect_);
    tb->addStretch();

    // ---- 中央：左视频 + 右页签 ----
    video_ = new YuvGLWidget;
    video_->setMinimumSize(400, 300);

    auto* tabs = new QTabWidget;
    logView_ = new QTextBrowser;
    tabs->addTab(logView_, "事件日志");

    auto* ctrlPage = new QWidget;
    auto* cv = new QVBoxLayout(ctrlPage);
    auto* row1 = new QHBoxLayout;
    btnRecOn_  = new QPushButton("开始录像(ACC_ON)");  btnRecOn_->setCheckable(true);
    btnRecOff_ = new QPushButton("停止(ACC_OFF)");     btnRecOff_->setCheckable(true);
    btnLock_   = new QPushButton("锁定当前段");
    row1->addWidget(btnRecOn_); row1->addWidget(btnRecOff_); row1->addWidget(btnLock_);
    cv->addLayout(row1);

    cv->addStretch();
    tabs->addTab(ctrlPage, "控制/回放");

    // ★顶栏必须进布局，否则不显示（上次的坑）
    auto* central = new QWidget;
    auto* outer = new QVBoxLayout(central);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    outer->addWidget(top);
    auto* cl = new QHBoxLayout;
    cl->addWidget(video_, 3);
    cl->addWidget(tabs, 2);
    outer->addLayout(cl);
    setCentralWidget(central);

    // ---- 状态栏 ----
    lblVideo_ = new QLabel("无视频流");
    lblConn_  = new QLabel("未连接");
    statusBar()->addWidget(lblVideo_);
    statusBar()->addPermanentWidget(lblConn_);
}

// ============================ 信号接线 ============================
void MainWindow::wireSignals() {
    // ---- 连接管理 ----
    connect(btnConnect_, &QPushButton::clicked, this, [this]{
        const QString ip = currentIp();
        if (ip.isEmpty()) { appendLog("请先填写设备IP", "err"); return; }
        rememberIp(ip);
        appendLog(QString("[连接] %1:%2 ...").arg(ip).arg(portProto_->value()));
        dev_->connectTo(ip, quint16(portProto_->value()));
    });
    connect(btnDisconnect_, &QPushButton::clicked, this, [this]{
        dev_->disconnectFromDevice();
        appendLog("[手动断开]", "warn");
    });
    connect(dev_, &DeviceClient::connected, this, [this]{
        appendLog("[已连接] 等待车机问候帧…", "ok");
        setConnected(true);
    });
    connect(dev_, &DeviceClient::disconnected, this, [this]{
        if (dev_->willReconnect()) appendLog("[断开] 将自动重连（手动断开除外）", "warn");
        else                       appendLog("[断开]（手动，不重连）", "warn");
        setConnected(false);
        if (video_->isVisible()) applyRecordState(false, "连接断开");
    });

    // ---- 探针：非JSON字节原样上屏（DV帧头只出现一次）----
    connect(dev_, &DeviceClient::rawLineReceived, this, [this](const QByteArray& raw){
        appendLog(QString("[原始] %1 | hex:%2")
                  .arg(QString::fromUtf8(raw), QString::fromLatin1(raw.toHex())), "warn");
    });
    connect(dev_, &DeviceClient::eventReceived, this, [this](const QJsonObject& o){ onEvent(o); });
    connect(dev_, &DeviceClient::crcLearned, this, [this](const QString& d){
            appendLog("[协议自检] CRC算法=" + d, d.contains("✓") ? "ok" : "err");
        });

    // ---- ACC控制：乐观UI + 3秒无应答提示 ----
    connect(btnRecOn_, &QPushButton::clicked, this, [this]{
        if (!dev_->isConnected()) { appendLog("未连接", "err"); btnRecOn_->setChecked(false); return; }
        dev_->sendCommand("ACC_ON");
        appendLog(">> ACC_ON", "tx");
        cmdAcked_ = false;
        QTimer::singleShot(3000, this, [this]{ if (!cmdAcked_)
            appendLog("⚠ ACC_ON 3秒无应答——发送格式可能不符（需文档/抓包）", "err"); });
        applyRecordState(true, "本地");
    });
    connect(btnRecOff_, &QPushButton::clicked, this, [this]{
        if (!dev_->isConnected()) { appendLog("未连接", "err"); btnRecOff_->setChecked(false); return; }
        dev_->sendCommand("ACC_OFF");
        appendLog(">> ACC_OFF", "tx");
        cmdAcked_ = false;
        QTimer::singleShot(3000, this, [this]{ if (!cmdAcked_)
            appendLog("⚠ ACC_OFF 3秒无应答——发送格式可能不符（需文档/抓包）", "err"); });
        applyRecordState(false, "本地");
    });
    connect(btnLock_, &QPushButton::clicked, this, [this]{
        if (!dev_->isConnected()) { appendLog("未连接", "err"); return; }
        dev_->sendCommand("LOCK");          // 原"LCK"改为命令表实测值
        appendLog(">> LOCK", "tx");
        cmdAcked_ = false;
        QTimer::singleShot(3000, this, [this]{ if (!cmdAcked_)
            appendLog("⚠ LCK 3秒无应答——发送格式可能不符（需文档/抓包）", "err"); });
    });

    // ---- 解码线程 → 状态栏 ----
    connect(worker_, &VideoWorker::stats, this, [this](double fps, double kbps){
        lblVideo_->setText(QString("%1 fps | %2 kbps").arg(fps, 0, 'f', 1).arg(kbps, 0, 'f', 0));
    });
    connect(worker_, &VideoWorker::errorOccurred, this, [this](const QString& msg){
        appendLog("[流错误] " + msg, "err");
    });

    // ---- 搜索设备（UDP广播发现，后端=已有DeviceDiscoverer，端口9002）----
    connect(btnSearch_, &QPushButton::clicked, this, [this]{
        appendLog(">> 搜索设备（UDP广播 :9002，1.5s窗口）…");
        discoverer_->discover();                       // 接口：discover(timeoutMs=1500)
    });
    connect(discoverer_, &DeviceDiscoverer::deviceFound, this, [this](const QJsonObject& info){
        const QString ip = info.value("ip").toString(); // 该文件已把源地址注入"ip"键
        const QString nm = info.value("name").toString();
        if (ip.isEmpty()) return;
        if (ipBox_->findText(ip) < 0) ipBox_->addItem(ip); // 历史下拉去重后追加
        ipBox_->setEditText(ip);                        // ★自动填入IP编辑框，直接可点连接
        appendLog(QString("[发现] %1 → %2 已填入IP框")
                  .arg(nm.isEmpty() ? "DVR设备" : nm, ip), "ok");
    });
    connect(discoverer_, &DeviceDiscoverer::finished, this, [this](int n){  // ★信号名是finished
        appendLog(n ? QString("[发现] 完成，共 %1 台").arg(n)
                    : "[发现] 未发现设备（确认与本机同网段；PC防火墙放行UDP）",
                  n ? "ok" : "warn");
    });

}

// ============ 事件分发：平替 car_sim.py 的 rx 打印 + UI联动 ============
void MainWindow::onEvent(const QJsonObject& o) {
    const quint8  t    = quint8(o.value("_t").toInt());
    const int     seq  = o.value("_seq").toInt();
    const QString full = QJsonDocument(o).toJson(QJsonDocument::Compact);
    cmdAcked_ = true;   // 有下行帧=链路活着（喂3秒超时检查）

    switch (t) {
    case DeviceClient::ACK: {                                // 0xE0：问候帧/命令应答
        const QString st = o.value("state").toString();
        if (o.contains("cmds"))
            appendLog(QString("<< [ACK] 问候 proto=%1 命令表%2 状态=%3")
                      .arg(o.value("hello").toString("(?)"),
                           o.value("cmds").toString(), st), "ok");
        else
            appendLog(QString("<< [ACK] seq=%1 %2").arg(seq).arg(full), "tx");
        if (!st.isEmpty()) {
            const int c = classifyState(st);
            if (c >= 0) applyRecordState(c == 1, "设备");
            else        appendLog("[状态] 值未识别：" + st, "warn");
        }
        return;
    }
    case DeviceClient::NAK:                                  // 0xE1：错误应答
        appendLog(QString("<< [NAK] seq=%1 %2").arg(seq).arg(full), "err");
        return;
    case DeviceClient::EVT_STATE: {                          // 0x81：状态推送
        const QString st = o.value("state").toString(
                           o.value("status").toString(
                           o.value("rec").toString()));
        appendLog(QString("<< [EVT_STATE] seq=%1 %2").arg(seq).arg(full), "ok");
        const int c = classifyState(st);
        if (c >= 0) applyRecordState(c == 1, "设备");
        else if (!st.isEmpty()) appendLog("[状态] 值未识别：" + st, "warn");
        return;
    }
    case DeviceClient::EVT_SEGMENT:                          // 0x82：durUs→秒（同Python）
        appendLog(QString("<< [EVT_SEGMENT] seq=%1 时长=%2s %3")
                  .arg(seq).arg(o.value("durUs").toDouble() / 1e6, 0, 'f', 1).arg(full), "ok");
        return;
    case DeviceClient::EVT_ALARM:
        appendLog(QString("<< [EVT_ALARM] seq=%1 %2").arg(seq).arg(full), "warn");
        return;
    case DeviceClient::EVT_ERROR:
        appendLog(QString("<< [EVT_ERROR] seq=%1 %2").arg(seq).arg(full), "err");
        return;
    default:
        appendLog(QString("<< [%1] seq=%2 %3")
                  .arg(o.value("_tname").toString()).arg(seq).arg(full));
        return;
    }
}

// ============================ 录像状态：单一真相源 ============================
void MainWindow::applyRecordState(bool on, const QString& src) {
    if (on) {
        const QString url = QString("rtsp://%1:%2/live").arg(currentIp()).arg(portRtsp_->value());
        if (worker_->isRunning()) worker_->stopStream();   // 已在跑就别让GUI白等3秒
        worker_->startStream(url);
        video_->setVisible(true);
        lblVideo_->setText("连接中…");
        appendLog(QString("[%1] ACC_ON → 拉流 %2").arg(src, url), "ok");
    } else {
        worker_->stopStream();
        video_->setVisible(false);
        lblVideo_->setText("无视频流");
        appendLog(QString("[%1] ACC_OFF → 视频关闭").arg(src), "warn");
    }
    btnRecOn_->setChecked(on);    // ★按钮开/关效果与视频状态同步
    btnRecOff_->setChecked(!on);
}

// ============================ 日志 / 状态 / 持久化 ============================
void MainWindow::appendLog(const QString& text, const QString& kind) {
    QString c = "#555555";
    if      (kind == "ok")   c = "#2e7d32";
    else if (kind == "err")  c = "#c62828";
    else if (kind == "warn") c = "#e65100";
    else if (kind == "tx")   c = "#1565c0";
    logView_->append(QString("<span style='color:%1'>%2</span>")
                     .arg(c, (QTime::currentTime().toString("hh:mm:ss ") + text).toHtmlEscaped()));
}

void MainWindow::setConnected(bool on) {
    btnConnect_->setEnabled(!on);
    btnDisconnect_->setEnabled(on);
    lblConn_->setText(on ? "已连接" : "未连接");
    lblConn_->setStyleSheet(QString("color:%1;font-weight:bold;")
                            .arg(on ? "#2e7d32" : "#888888"));
}

QString MainWindow::currentIp() const { return ipBox_->currentText().trimmed(); }

void MainWindow::rememberIp(const QString& ip) {
    const int i = ipBox_->findText(ip);
    if (i >= 0) ipBox_->removeItem(i);
    ipBox_->insertItem(0, ip);
    ipBox_->setCurrentIndex(0);
}

void MainWindow::loadSettings() {
    QSettings s("DvrProject", "DvrConsole");
    ipBox_->addItems(s.value("ipHistory").toStringList());
    portProto_->setValue(s.value("protoPort", 9001).toInt());
    portRtsp_->setValue(s.value("rtspPort", 8554).toInt());
}

// 关闭窗口事件：退出前把 IP 历史、端口等写回 QSettings 持久化，并主动断开/停流收尾。
void MainWindow::closeEvent(QCloseEvent* e) {
    QSettings s("DvrProject", "DvrConsole");
    QStringList ips;
    for (int i = 0; i < ipBox_->count(); ++i) ips << ipBox_->itemText(i);
    s.setValue("ipHistory", ips);
    s.setValue("protoPort", portProto_->value());
    s.setValue("rtspPort",  portRtsp_->value());
    dev_->disconnectFromDevice();
    worker_->stopStream();
    e->accept();
}
