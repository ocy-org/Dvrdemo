#ifndef CAMERACONTROLLER_H
#define CAMERACONTROLLER_H
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCaptureRequest.h>
#include <android/native_window.h>
#include <functional>
#include <QString>
#include <QDebug>
/** NDK camera2封装。NDK 与 Java 不同，openCamera/建会话/下发请求均为同步直接调用：
 * open() → openCamera 返回 OK 即已打开 → createSession() → 立即 setRepeatingRequest
 * → readyCb("")。onReady/onActive/onClosed 仅为状态通知，不承载流程推进。
 */
class CameraController {
public:
    using ReadyCb = std::function<void(const QString& err)>;

    ~CameraController() { close(); }

    void open(ANativeWindow* target, ReadyCb cb) {
        readyCb_ = std::move(cb);
        target_ = target;
        manager_ = ACameraManager_create();
        std::string backId;
        if (!findBackCamera(&backId)) {
            qWarning() << "[camera] findBackCamera FAILED";
            notifyReady("no back camera"); return;
        }
        qWarning() << "[camera] openCamera id=" << backId.c_str();
        deviceCb_.context = this;
        deviceCb_.onDisconnected = &CameraController::onDisconnected;
        deviceCb_.onError = &CameraController::onDeviceError;
        camera_status_t st = ACameraManager_openCamera(manager_, backId.c_str(), &deviceCb_, &device_);
        qWarning() << "[camera] openCamera returned st=" << static_cast<int>(st);
        if (st != ACAMERA_OK)  { notifyReady("openCamera failed"); return; }
        // ★ NDK 与 Java 不同：openCamera 同步返回，返回 OK 即设备已打开，
        //   没有 onOpened 回调，直接建会话
        createSession();
    }


    void close() {
        if (session_)   { ACameraCaptureSession_close(session_);          session_   = nullptr; }
        if (request_)   { ACaptureRequest_free(request_);                 request_   = nullptr; }
        if (outTarget_) { ACameraOutputTarget_free(outTarget_);           outTarget_ = nullptr; }
        if (output_)    { ACaptureSessionOutput_free(output_);            output_    = nullptr; }
        if (container_) { ACaptureSessionOutputContainer_free(container_);container_  = nullptr; }
        if (device_)    { ACameraDevice_close(device_);                   device_    = nullptr; }
        if (manager_)   { ACameraManager_delete(manager_);                manager_   = nullptr; }
        started_ = false;
        notified_ = false;
        target_ = nullptr;   
        readyCb_ = nullptr; 
    }

private:
    // ---------- 相机线程回调（静态转发） ----------
//    static void onOpened(void* ctx, ACameraDevice* d) {
//        qWarning() << "[camera] onOpened entered";
//        auto self = static_cast<CameraController*>(ctx);
//        self->device_ = d;                    // 保险：确保成员已赋值
//        self->createSession();
//    }
    static void onDisconnected(void* ctx, ACameraDevice*) {
        qWarning() << "[camera] onDisconnected entered";
        static_cast<CameraController*>(ctx)->notifyReady("camera disconnected");
    }
    static void onDeviceError(void* ctx, ACameraDevice*, int err) {
        qWarning() << "[camera] onError entered code=" << err;
        static_cast<CameraController*>(ctx)->notifyReady("camera error " + QString::number(err));
    }
    static void onSessionClosed(void*, ACameraCaptureSession*) {}
    static void onSessionActive(void*, ACameraCaptureSession*) {qWarning() << "[camera] onSessionActive (info)";}
    static void onSessionReady(void* ctx, ACameraCaptureSession*) {
        qWarning() << "[camera] onSessionReady entered";
//        auto self = static_cast<CameraController*>(ctx);
//        if (self->started_) return;
//        self->started_ = true;
//        self->startRepeating();
//        self->notifyReady("");
    }

    // ---------- 内部步骤 ----------
    bool findBackCamera(std::string* outId) {
        ACameraIdList* ids = nullptr;
        if (ACameraManager_getCameraIdList(manager_, &ids) != ACAMERA_OK) return false;
        bool found = false;
        for (int i = 0; i < ids->numCameras && !found; ++i) {
            ACameraMetadata* md = nullptr;
            if (ACameraManager_getCameraCharacteristics(manager_, ids->cameraIds[i], &md) != ACAMERA_OK)
                continue;
            ACameraMetadata_const_entry e{};
            if (ACameraMetadata_getConstEntry(md, ACAMERA_LENS_FACING, &e) == ACAMERA_OK
                && e.count > 0 && e.data.u8[0] == ACAMERA_LENS_FACING_BACK) {
                *outId = ids->cameraIds[i];
                found = true;
            }
            ACameraMetadata_free(md);
        }
        ACameraManager_deleteCameraIdList(ids);
        return found;
    }

    void createSession() {
        qWarning() << "[camera] createSession begin target=" << (void*)target_;
        ACaptureSessionOutput* out = nullptr;
        camera_status_t st = ACaptureSessionOutput_create(target_, &out);
        qWarning() << "[camera] output_create st=" << static_cast<int>(st);
        if (st != ACAMERA_OK) { notifyReady("output create failed"); return; }
        output_ = out;
        ACaptureSessionOutputContainer_create(&container_);
        ACaptureSessionOutputContainer_add(container_, out);
        sessionCb_.context = this;
        sessionCb_.onClosed = &CameraController::onSessionClosed;
        sessionCb_.onReady  = &CameraController::onSessionReady;
        sessionCb_.onActive = &CameraController::onSessionActive;
        st = ACameraDevice_createCaptureSession(device_, container_, &sessionCb_, &session_);
        qWarning() << "[camera] createCaptureSession st=" << static_cast<int>(st);
        if (st != ACAMERA_OK) { notifyReady("createCaptureSession failed"); return; }
        qWarning() << "[camera] session ok, waiting onSessionReady";
        // ★ 不等 onSessionReady（无请求提交前不保证触发），立即下发 repeating request；
        //   流尚未配置完成时框架会自动排队（官方示例 camera/basic 的固定模式）
        startRepeating();
        notifyReady("");
    }


    void startRepeating() {
        qWarning() << "[camera] startRepeating begin";
        camera_status_t st = ACameraDevice_createCaptureRequest(device_, TEMPLATE_RECORD, &request_);
        if (st != ACAMERA_OK || !request_) {
            qWarning() << "[camera] createRequest FAILED st=" << static_cast<int>(st);
            notifyReady("createRequest failed"); return;
        }
        ACameraOutputTarget* t = nullptr;
        st = ACameraOutputTarget_create(target_, &t);
        if (st != ACAMERA_OK || !t) {
            qWarning() << "[camera] outputTarget FAILED st=" << static_cast<int>(st);
            notifyReady("outputTarget failed"); return;
        }
        outTarget_ = t;
        ACaptureRequest_addTarget(request_, t);
        st = ACameraCaptureSession_setRepeatingRequest(session_, nullptr, 1, &request_, nullptr);
        qWarning() << "[camera] setRepeating st=" << static_cast<int>(st);
        if (st != ACAMERA_OK) notifyReady("setRepeating failed");
    }

    void notifyReady(const QString& err) {
        if (notified_) return;
        notified_ = true;
        if (readyCb_) readyCb_(err);
    }

    ANativeWindow*                   target_    = nullptr;
    ACameraManager*                  manager_   = nullptr;
    ACameraDevice*                   device_    = nullptr;
    ACameraCaptureSession*           session_   = nullptr;
    ACaptureRequest*                 request_   = nullptr;
    ACameraOutputTarget*             outTarget_ = nullptr;
    ACaptureSessionOutput*           output_    = nullptr;
    ACaptureSessionOutputContainer*  container_ = nullptr;
    ACameraDevice_StateCallbacks         deviceCb_{};
    ACameraCaptureSession_stateCallbacks sessionCb_{};
    ReadyCb readyCb_;
    bool    started_  = false;
    bool    notified_ = false;
};

#endif // CAMERACONTROLLER_H
