package dev.twinbox.vm;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.os.IBinder;

/**
 * v1.2.70 ★★★ RunService：运行期前台服务（免冻结豁免）。
 * 病理（v1.2.66~69 四版取证实锤 + v1.2.69 WakeLock 无效实锤）：
 * stage0 跑 35s+ 期间，App 无前台服务 → ColorOS/A16 判定"空闲" →
 * cgroup freezer 冻结整个进程（guest+tracer+主线程无声挂起）——
 * PARTIAL_WAKE_LOCK 只防 CPU 睡眠，**不阻止 cgroup 冻结**。
 * 前台服务持有期间：App 保持"前台"身份，freezer 不碰。
 */
public class RunService extends Service {
    private static final String CH = "twinvm_run";

    @Override
    public void onCreate() {
        super.onCreate();
        NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
        if (nm != null && nm.getNotificationChannel(CH) == null) {
            nm.createNotificationChannel(new NotificationChannel(
                    CH, "TwinVM 运行", NotificationManager.IMPORTANCE_LOW));
        }
    }

    @Override
    public int onStartCommand(Intent i, int flags, int startId) {
        Notification n = new Notification.Builder(this, CH)
                .setSmallIcon(android.R.drawable.ic_media_play)
                .setContentTitle("TwinVM 虚拟机运行中")
                .setContentText("guest 执行期间保持活跃（防系统冻结）")
                .setOngoing(true)
                .build();
        startForeground(2001, n);
        return START_NOT_STICKY;
    }

    @Override
    public IBinder onBind(Intent i) { return null; }
}
