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
    /* v1.2.111：FGS 竞态防线——startForeground 成功前禁 stopService */
    static volatile boolean sForeground = false;
    static volatile boolean sStopRequested = false;

    @Override
    public void onCreate() {
        super.onCreate();
        try {
            NotificationManager nm = (NotificationManager) getSystemService(NOTIFICATION_SERVICE);
            if (nm != null && nm.getNotificationChannel(CH) == null) {
                nm.createNotificationChannel(new NotificationChannel(
                        CH, "TwinVM 运行", NotificationManager.IMPORTANCE_LOW));
            }
            /* v1.2.110 ★★★：onCreate 就 startForeground（不等 onStartCommand）。
             * v1.2.111：成功后置 sForeground；若此刻已有人请求停（秒败场景），
             * 自行 stopSelf——绝不给系统留"未兑现"账。 */
            startForeground(2001, buildNote());
            sForeground = true;
            if (sStopRequested) stopSelf();
        } catch (Throwable ignore) { }
    }

    @Override
    public int onStartCommand(Intent i, int flags, int startId) {
        /* v1.2.110：try 包裹（通知限制/权限异常不能炸服务——炸了就是
         * ForegroundServiceDidNotStartInTimeException 连坐闪退） */
        try {
            startForeground(2001, buildNote());
            sForeground = true;
            if (sStopRequested) stopSelf();
        } catch (Throwable ignore) { }
        return START_NOT_STICKY;
    }

    private Notification buildNote() {
        return new Notification.Builder(this, CH)
                .setSmallIcon(android.R.drawable.ic_media_play)
                .setContentTitle("TwinVM 虚拟机运行中")
                .setContentText("guest 执行期间保持活跃（防系统冻结）")
                .setOngoing(true)
                .build();
    }

    @Override
    public IBinder onBind(Intent i) { return null; }
}
