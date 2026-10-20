package org.qtproject.example.Dvrqt;

import android.content.Context;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.util.Log;

public class WifiLockHelper {
    private static final String TAG = "DVR";   // same tag as C++ LOG
    private static WifiManager.WifiLock sLock;//WiFi高性能锁
    private static WifiManager.MulticastLock sMcastLock; // 放行广播/组播包

    @SuppressWarnings("deprecation")
    public static void acquire(Context ctx) {
        Log.d(TAG, "WifiLock: acquire() entered");          // probe1: JNI bridge alive
        
        if (sLock != null) { Log.d(TAG, "WifiLock: already held"); return; }
        try {
            WifiManager wm = (WifiManager) ctx.getApplicationContext()
                    .getSystemService(Context.WIFI_SERVICE);
            if (wm == null) { Log.e(TAG, "WifiLock: WifiManager null"); return; }
            int mode = Build.VERSION.SDK_INT >= 29
                    ? WifiManager.WIFI_MODE_FULL_LOW_LATENCY
                    : WifiManager.WIFI_MODE_FULL_HIGH_PERF;
            sLock = wm.createWifiLock(mode, "DvrWifiLock");
            sLock.setReferenceCounted(false);
            sLock.acquire();
            Log.d(TAG, "WifiLock: ACQUIRED ok");            // probe2: success
        } catch (Exception e) {
            Log.e(TAG, "WifiLock: FAILED", e);              // probe3: failure reason
        }
        
        // ----------多播锁（服务UDP发现 :9002，收广播包的前提） ----------
        // WiFi STA模式下安卓驱动默认丢弃广播/组播帧省电；不持此锁则
        // PC的"DVR_DISCOVER"广播根本到不了DiscoverResponder的socket。
        if (sMcastLock != null) {
            Log.d(TAG, "MulticastLock: already held");
        } else {
            try {
                WifiManager wm = (WifiManager) ctx.getApplicationContext()
                        .getSystemService(Context.WIFI_SERVICE);
                if (wm == null) {
                    Log.e(TAG, "MulticastLock: WifiManager null");
                } else {
                    sMcastLock = wm.createMulticastLock("DvrMulticastLock");
                    sMcastLock.setReferenceCounted(false);
                    sMcastLock.acquire();
                    Log.d(TAG, "MulticastLock: ACQUIRED ok"); // probe2: success
                }
            } catch (Exception e) {
                Log.e(TAG, "MulticastLock: FAILED", e); // probe3: failure reason
            }
        }
    }
}