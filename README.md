# Dvrdemo
Dvrdemo是一个基于QT框架实现安卓端行车记录仪APP与PC控制端的协同系统。核心功能包括：局域网UDP广播设备发现、TCP私有协议命令交互（ACC_ON/ACC_OFF/LOCK）、RTSP视频推流与拉流、OpenGL加速YUV渲染、硬件编码录像、碰撞检测触发事件录像及分段落盘。
## 效果图
### 安卓APP客户端
<img width="2280" height="1080" alt="image" src="https://github.com/user-attachments/assets/83af164d-998a-49ae-93db-4122cca007f6" />
<img width="2280" height="1080" alt="image" src="https://github.com/user-attachments/assets/5b47263d-59a9-4802-896e-02dbbe6632bd" />
<img width="2280" height="1080" alt="image" src="https://github.com/user-attachments/assets/fd00b1d2-3e63-446f-8fba-315e390d5d3a" />
### PC控制端
<img width="1095" height="670" alt="image" src="https://github.com/user-attachments/assets/2680f846-fae1-4aff-828e-ffb0b990a5f9" />
<img width="1105" height="675" alt="image" src="https://github.com/user-attachments/assets/46c1c9d2-d6ed-46b8-832c-61464edbd0a2" />
<img width="1099" height="676" alt="image" src="https://github.com/user-attachments/assets/507b49e5-eeea-4043-a53b-88a4c87d89f4" />
## 功能操作说明
### 1、设备互通
（1）同时打开APP端和PC端，两个设备（安卓手机和PC端）在连接同一个局域网 的情况下，点击PC端的【搜索设备】按钮，设备端的IP地址自动填写到设备IP列表空中，此时点击【连接】按钮即可完成设备互通；（见效果图3）

（2）同时打开APP端和PC端，两个设备（安卓手机和PC端）网络可达的情况下，查看APP端”实时“菜单中的提醒信息，把获取到的IP地址填写到PC端的IP地址栏，此时点击【连接】按钮即可完成设备互通；

### 2、开始录像
（1）APP端，在”实时“菜单中，点击【开始录像】按钮，即可开始录像，每隔60秒生成一个日常视频，在录像期间，如果强力甩动（模拟碰撞效果），达到预警阈值时，自动生成事件视频；（见效果图1）

（2）PC端连接成功后，首先观察APP端的”实时“菜单中有提示信息“车机:proto://xx.xx.xx.xx:9001”，然后在PC端进入”控制/回放“菜单页面（见效果图4），点击【开始录像(ACC_ON)】按钮，APP端即会触发【开始录像】功能，这是PC端通过两端定义的私有车机协议实现的后台服务功能，在录像期间，如果强力甩动（模拟碰撞效果），达到预警阈值时，自动生成事件视频。

### 3、停止录像
APP端，在”实时“菜单中，点击【开始录像】按钮之后，会显示出【停止录像】和【锁定当前段】按钮。

（1）在”实时“菜单中，点击【停止录像】按钮，即可关闭录像，并且生成录像到日常视频中（不需要满足60秒的条件）。

（2）PC端连接成功后，首先观察APP端的”实时“菜单中有提示信息“车机:proto://xx.xx.xx.xx:9001”，然后在PC端进入”控制/回放“菜单页面（见效果图4），点击【停止(ACC_OFF)】按钮，APP端即会触发【停止录像】功能，并且生成录像到日常视频中（不需要满足60秒的条件）。

### 4、锁定当前段
在开始录像的情况下，点击APP端和PC端的【锁定当前段】按钮都会生成一个事件视频。

### 5、RTSP推流
 PC端连接成功后，首先观察APP端的”实时“菜单中有提示信息“直播:rtsp://xx.xx.xx.xxx:8554/live”，，然后在PC端进入”控制/回放“菜单页面（见效果图4），点击【开始录像】按钮，此时在做成有一个显示RTSP推流的视频窗口（见效果图5）。

## 环境编译方法
PC控制端，本人是在window QT MinGW下编译的，方便通用移植。
APP客户端，本人是在ubuntu安装的QT andriod环境下编译的。

## 篇外
详细介绍可以查看
https://blog.csdn.net/liangyuna8787/article/details/168342822?spm=1001.2014.3001.5502
