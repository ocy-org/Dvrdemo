import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia

ApplicationWindow {
    id: root
    width: 360; height: 640; visible: true
    title: "DVR v5.3"
    color: "#101418"

    // ==================== 全局状态 ====================
    property int pageIdx: 0                    // 0=实时  1=回放
    property var lib: RecEngine.mediaLibrary   // C++媒体库对象

    function addLog(line) {
        var ts = Qt.formatDateTime(new Date(), "hh:mm:ss")
        log.text = ts + " " + line + "\n" + log.text
        var lines = log.text.split("\n")
        if (lines.length > 60) log.text = lines.slice(0, 60).join("\n")
    }
    function openPlayer(url) {
        player.source = url
        playerPopup.open()
        player.play()
    }

    // ==================== 播放器弹窗（全屏） ====================
    Popup {
        id: playerPopup
        parent: Overlay.overlay
        width: root.width; height: root.height
        padding: 0; modal: true
        background: Rectangle { color: "black" }
        onClosed: { player.stop(); player.source = "" }

        VideoOutput { id: vOut; anchors.fill: parent }
        MediaPlayer {
            id: player
            videoOutput: vOut
            audioOutput: aOut
            onMediaStatusChanged: if (mediaStatus === MediaPlayer.EndOfMedia)
                                      playerPopup.close()
        }
        AudioOutput { id: aOut; muted: true }          // 记录仪无音轨，静音兜底

        Button {
            text: "✕ 关闭"
            anchors.top: parent.top; anchors.right: parent.right
            anchors.margins: 10
            onClicked: playerPopup.close()
        }
        RowLayout {
            anchors.left: parent.left; anchors.right: parent.right
            anchors.bottom: parent.bottom; anchors.margins: 12
            Button {
                Layout.preferredWidth: 72
                text: player.playbackState === MediaPlayer.PlayingState ? "暂停" : "播放"
                onClicked: player.playbackState === MediaPlayer.PlayingState
                           ? player.pause() : player.play()
            }
            Slider {
                Layout.fillWidth: true
                from: 0; to: Math.max(player.duration, 1)
                value: player.position
                onMoved: player.setPosition(value)
            }
            Label {
                color: "white"
                text: Qt.formatTime(new Date(player.position), "mm:ss")
                      + " / " + Qt.formatTime(new Date(player.duration), "mm:ss")
            }
        }
    }

    // ==================== 看图弹窗（显示所点的图+日期） ====================
    Popup {
        id: photoViewer
        parent: Overlay.overlay
        width: root.width; height: root.height
        padding: 0; modal: true
        background: Rectangle { color: "black" }

        property string currentUrl: ""         // 点击瞬间写入，不再固定取第一张
        property string currentDate: ""

        Image {
            anchors.fill: parent
            fillMode: Image.PreserveAspectFit
            asynchronous: true
            source: photoViewer.currentUrl
        }
        Label {
            anchors.top: parent.top; anchors.horizontalCenter: parent.horizontalCenter
            topPadding: 10
            text: photoViewer.currentDate
            color: "#c8d2dc"; font.pixelSize: 14
        }
        Label {
            text: "点击任意处关闭"; color: "#888"
            anchors.bottom: parent.bottom
            anchors.horizontalCenter: parent.horizontalCenter
            bottomPadding: 12
        }
        MouseArea { anchors.fill: parent; onClicked: photoViewer.close() }
    }

    // ==================== 删除确认弹窗 ====================
    Dialog {
        id: confirmDel
        property string kind: ""      // normal | event | photo
        property string name: ""
        property bool paired: false   // 图片是否有配对视频（删了会被自动重建）
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        width: Math.min(root.width - 60, 340)

        Label {
            width: parent.width; wrapMode: Text.Wrap
            color: "#c8d2dc"; font.pixelSize: 13
            text: confirmDel.kind === "photo"
                  ? (confirmDel.paired
                     ? "该图片是视频封面，删除后刷新时会自动重建，确定删除？"
                     : "确定删除图片 " + confirmDel.name + " ？")
                  : (confirmDel.kind === "event"
                     ? "确定删除【事件视频】及其封面？\n" + confirmDel.name
                     : "确定删除该视频及其封面？\n" + confirmDel.name)
        }
        footer: DialogButtonBox {
            Button {
                text: "取消"
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            }
            Button {
                text: "删除"
                DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            }
        }

        onAccepted: {
            var ok
            if (kind === "photo") ok = root.lib.deletePhoto(name)
            else                  ok = root.lib.deleteVideo(kind, name)
            addLog("[del] " + (ok ? "OK " : "FAIL ") + name)
        }
    }

    // ==================== 主布局：左Tab菜单 + 右页面 ====================
    RowLayout {
        anchors.fill: parent; anchors.margins: 12; spacing: 12

        // ---- 左栏：Tab 菜单（自绘选中态，非按钮） ----
        ColumnLayout {
            Layout.preferredWidth: root.width > 500 ? 128 : 100
            Layout.fillHeight: true
            spacing: 6

            Repeater {
                model: ["实时", "回放"]
                delegate: Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 56
                    radius: 10
                    color: root.pageIdx === index ? "#243040" : "#161c23"
                    border.color: root.pageIdx === index ? "#ff2d78" : "#232b34"
                    border.width: 1

                    Rectangle {                      // 选中态：左侧高亮指示条
                        width: 3; radius: 1.5
                        height: parent.height * 0.55
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        color: "#ff2d78"
                        visible: root.pageIdx === index
                    }
                    Column {
                        anchors.centerIn: parent; spacing: 3
                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: index === 0 ? "◉" : "▶"
                            color: root.pageIdx === index ? "#ff2d78" : "#6d7a88"
                            font.pixelSize: 15
                        }
                        Text {
                            anchors.horizontalCenter: parent.horizontalCenter
                            text: modelData
                            color: root.pageIdx === index ? "#ffffff" : "#8a96a3"
                            font.pixelSize: 14
                            font.bold: root.pageIdx === index
                        }
                    }
                    TapHandler {
                        onTapped: {
                            root.pageIdx = index
                            if (index === 1) root.lib.refresh()   // 进相册自动刷新
                        }
                    }
                }
            }

            Item { Layout.fillHeight: true }
        }

        // ---- 右栏：两个页面 ----
        StackLayout {
            Layout.fillWidth: true; Layout.fillHeight: true
            currentIndex: root.pageIdx

            // ========== 页0：实时（录像控制，延续原逻辑） ==========
            ColumnLayout {
                spacing: 8

                RowLayout {
                    Layout.fillWidth: true; spacing: 8
                    Button {
                        text: "开始录像"
                        enabled: RecEngine.statusText === "IDLE"
                        onClicked: RecEngine.start()
                    }
                    Button {
                        text: "停止录像"
                        enabled: RecEngine.statusText !== "IDLE"
                        onClicked: RecEngine.stop()
                    }
                    Button {
                        text: "锁定当前段"
                        enabled: RecEngine.statusText === "DRIVING"
                        onClicked: RecEngine.lockCurrentSegment()
                    }
                }

                Label {
                    text: RecEngine.statusText
                    color: RecEngine.statusText === "DRIVING" ? "#ff6b6b" : "#9fe870"
                    font.pixelSize: 20; font.bold: true
                }

                Label { text: "直播: " + (RecEngine.rtspUrl || "启动失败")
                        color: "#9fd7ff"; font.pixelSize: 12
                        elide: Text.ElideMiddle; Layout.fillWidth: true }
                Label { text: "TCP: " + (RecEngine.tcpUrl || "启动失败")
                        color: "#7fd4ff"; font.pixelSize: 12
                        elide: Text.ElideMiddle; Layout.fillWidth: true }
                Label { text: "车机: " + (RecEngine.protoUrl || "启动失败")
                        color: "#ffd479"; font.pixelSize: 12
                        elide: Text.ElideMiddle; Layout.fillWidth: true }
                Label { text: RecEngine.lastAlarm ? "最近事件: " + RecEngine.lastAlarm
                                                  : "G-sensor 待命"
                        color: "#ffb86c"; font.pixelSize: 13; font.bold: true
                        Layout.fillWidth: true }

                ScrollView {
                    Layout.fillWidth: true; Layout.fillHeight: true
                    TextArea {
                        id: log
                        readOnly: true; wrapMode: TextArea.Wrap
                        color: "#9fe870"; font.family: "monospace"; font.pixelSize: 12
                        text: "DVR ready.\n"
                    }
                }
            }

            // ========== 页1：回放相册（三类列表 + 删除） ==========
            ColumnLayout {
                spacing: 6

                TabBar {
                    id: albBar; Layout.fillWidth: true
                    TabButton { text: "日常视频" }
                    TabButton { text: "事件视频" }
                    TabButton { text: "图片" }
                }
                RowLayout {
                    Button { text: "刷新"; onClicked: root.lib.refresh() }
                    Label {
                        color: "#5a6470"; font.pixelSize: 12
                        text: albBar.currentIndex === 2
                              ? root.lib.photos.length + " 张"
                              : (albBar.currentIndex === 0
                                 ? root.lib.normalVideos.length + " 段"
                                 : root.lib.eventVideos.length + " 段")
                    }
                }

                StackLayout {
                    currentIndex: albBar.currentIndex
                    Layout.fillWidth: true; Layout.fillHeight: true

                    VideoGrid { model: root.lib.normalVideos }
                    VideoGrid { model: root.lib.eventVideos; eventMode: true }

                    // —— 图片网格 ——
                    GridView {
                        id: photoGrid; clip: true
                        cellWidth: Math.floor(width / Math.max(1, Math.floor(width/110)))
                        cellHeight: cellWidth + 18          // 图下留日期行
                        model: root.lib.photos
                        delegate: Item {
                            width: photoGrid.cellWidth; height: photoGrid.cellHeight
                            ColumnLayout {
                                anchors.fill: parent; anchors.margins: 4; spacing: 2
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: parent.width   // 方形封面
                                    color: "#1c232b"; border.color: "#2c3642"
                                    Image {
                                        anchors.fill: parent; anchors.margins: 2
                                        source: modelData.url
                                        fillMode: Image.PreserveAspectCrop
                                        asynchronous: true
                                    }
                                    MouseArea {
                                        anchors.fill: parent
                                        onClicked: {
                                            photoViewer.currentUrl = modelData.url
                                            photoViewer.currentDate = modelData.date
                                            photoViewer.open()
                                        }
                                    }
                                    AbstractButton {          // 删除按钮(声明在后=盖在上层)
                                        width: 24; height: 24
                                        anchors.top: parent.top; anchors.right: parent.right
                                        anchors.margins: 3
                                        background: Rectangle { radius: 12; color: "#b3101418" }
                                        contentItem: Text {
                                            text: "✕"; color: "#ff6b6b"; font.pixelSize: 12
                                            horizontalAlignment: Text.AlignHCenter
                                            verticalAlignment: Text.AlignVCenter
                                        }
                                        onClicked: {
                                            confirmDel.kind = "photo"
                                            confirmDel.name = modelData.name
                                            confirmDel.paired = modelData.paired
                                            confirmDel.open()
                                        }
                                    }
                                }
                                Label { Layout.fillWidth: true
                                        text: modelData.date
                                        color: "#c8d2dc"; font.pixelSize: 10
                                        elide: Text.ElideRight }
                            }
                        }
                    }
                }
            }
        }
    }

    // ==================== 视频网格组件（日常/事件复用；eventMode=红框） ====================
    component VideoGrid: GridView {
        id: vg
        property bool eventMode: false
        readonly property string kind: eventMode ? "event" : "normal"
        clip: true
        cellWidth: Math.floor(width / Math.max(1, Math.floor(width/150)))
        cellHeight: cellWidth * 9 / 16 + 34
        delegate: Item {
            width: GridView.view.cellWidth
            height: GridView.view.cellHeight
            ColumnLayout {
                anchors.fill: parent; anchors.margins: 4; spacing: 2
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: parent.width * 9 / 16 - 8
                    color: "#1c232b"
                    border.color: eventMode ? "#ff6b6b" : "#2c3642"; border.width: 1
                    Image {
                        anchors.fill: parent; anchors.margins: 1
                        visible: modelData.thumb !== ""
                        source: modelData.thumb
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                    }
                    Label {
                        anchors.centerIn: parent; visible: modelData.thumb === ""
                        text: "封面生成中…"; color: "#5a6470"; font.pixelSize: 10
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: root.openPlayer(modelData.url)
                    }
                    AbstractButton {              // 删除按钮(声明在后=盖在上层)
                        width: 24; height: 24
                        anchors.top: parent.top; anchors.right: parent.right
                        anchors.margins: 3
                        background: Rectangle { radius: 12; color: "#b3101418" }
                        contentItem: Text {
                            text: "✕"; color: "#ff6b6b"; font.pixelSize: 12
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        onClicked: {
                            confirmDel.kind = vg.kind
                            confirmDel.name = modelData.name
                            confirmDel.open()
                        }
                    }
                }
                Label { Layout.fillWidth: true; text: modelData.date
                        color: "#c8d2dc"; font.pixelSize: 10; elide: Text.ElideRight }
                Label { Layout.fillWidth: true; text: modelData.size
                        color: "#5a6470"; font.pixelSize: 9 }
            }
        }
    }

    // ==================== C++信号 → 日志 ====================
    Connections {
        target: RecEngine
        function onStatusChanged(s) { addLog("[state] " + s) }
        function onSegmentFinished(name, durUs, bytes) {
            addLog("[seg] " + name + " " + (durUs/1e6).toFixed(0) + "s "
                   + (bytes/1048576).toFixed(1) + "MB")
        }
        function onErrorOccurred(msg) { addLog("[ERR] " + msg) }
        function onLastAlarmChanged() { addLog("[alarm] " + RecEngine.lastAlarm) }
    }
}
