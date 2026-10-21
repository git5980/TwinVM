package dev.twinbox.vm;

import android.app.Activity;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * TwinVM Stage 1 —— 用户态虚拟机自检台。
 * 纯平台 widget（零依赖，宽视口触屏友好：竖排大按钮 + 等宽输出）。
 */
public class MainActivity extends Activity implements View.OnClickListener {

    private TextView mOut;
    private Handler mUi = new Handler(Looper.getMainLooper());

    /* v1.2.51 ★ 日志目录改公共 Documents/TwinVM（TA 要求：文件管理器直接可见，
     * 不用钻 Android/data/dev.twinbox.vm/files）。targetSdk=27 旧存储模型，
     * WRITE_EXTERNAL_STORAGE 运行时授权后可直写公共目录；
     * 授权被拒/目录建失败 → 回退 App 私有目录（日志绝不能丢）。 */
    private static final String TVM_DOCS_SUBDIR = "TwinVM";

    private File logsDir() {
        try {
            File docs = Environment.getExternalStoragePublicDirectory(
                    Environment.DIRECTORY_DOCUMENTS);
            if (docs != null) {
                File d = new File(docs, TVM_DOCS_SUBDIR);
                if ((d.exists() || d.mkdirs()) && d.canWrite()) return d;
            }
        } catch (Throwable ignore) { }
        File priv = getExternalFilesDir(null);
        return priv != null ? priv : getFilesDir();
    }

    private boolean hasStoragePermission() {
        return checkSelfPermission("android.permission.WRITE_EXTERNAL_STORAGE")
                == PackageManager.PERMISSION_GRANTED;
    }

    private android.os.PowerManager.WakeLock mRunLock;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        /* v1.2.69 ★★★ 五版灵异终局定罪：Android 16 cgroup freezer。
         * 病理：无 WakeLock 的 App 在"输出停止"后数秒被系统判定空闲 →
         * 进程整体冻结（guest+tracer+主线程全部挂起，无声无息）→
         * stress 后 [beat]/[chk]/watch 全停（v1.2.66~68 三版取证实锤：
         * 无条件心跳都会停=线程不在执行）；TA 手动停止→解冻→导出日志成功。
         * 治：运行期间持 PARTIAL_WAKE_LOCK（CPU 保持调度），停止时释放。 */
        android.os.PowerManager pm = (android.os.PowerManager)
                getSystemService(POWER_SERVICE);
        if (pm != null) {
            mRunLock = pm.newWakeLock(
                    android.os.PowerManager.PARTIAL_WAKE_LOCK, "TwinVM:run");
            mRunLock.setReferenceCounted(false);
        }

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(48, 72, 48, 48);
        root.setBackgroundColor(0xFF101418);

        TextView title = new TextView(this);
        title.setText("TwinVM · 用户态虚拟机 Stage 2");
        title.setTextColor(0xFFE8EAED);
        title.setTextSize(22);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        root.addView(title);

        TextView sub = new TextView(this);
        sub.setText("Stage 4.2：真 linker 接管——linker64 拉起 dalvikvm（ART 引导第一跳）");
        sub.setTextColor(0xFF9AA0A6);
        sub.setTextSize(13);
        sub.setPadding(0, 12, 0, 24);
        root.addView(sub);

        Button run = makeButton("运行 Stage 4.2 自检（dalvikvm 引导）");
        run.setTag("run");
        root.addView(run);

        /* v1.2.96 Stage 5-M1：容器 A（hello.dex）+ 容器 B（world.dex） */
        Button runB = makeButton("运行容器B（world.dex 容器）");
        runB.setTag("runB");
        root.addView(runB);

        Button probe = makeButton("引擎能力探测");
        probe.setTag("probe");
        LinearLayout.LayoutParams lp =
                new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                        LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.topMargin = 16;
        root.addView(probe, lp);

        mOut = new TextView(this);
        mOut.setTypeface(Typeface.MONOSPACE);
        mOut.setTextSize(12);
        mOut.setTextColor(0xFF7EE1B4);
        mOut.setPadding(8, 24, 8, 8);
        mOut.setText("就绪。点「运行 Stage 1 自检」：\n"
                + "从 ROM 装载 guest 二进制到本进程 fork 出的槽位，\n"
                + "在其自己的用户态内核下原生执行（同 VMOS 免 root 机制）。");

        ScrollView sc = new ScrollView(this);
        sc.addView(mOut);
        root.addView(sc, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));

        setContentView(root);
    }

    private static volatile int sTickerGen = 0;   /* v1.2.97：ticker 代际 */

    private Button makeButton(String text) {
        Button b = new Button(this);
        b.setText(text);
        b.setTextSize(15);
        b.setGravity(Gravity.CENTER);
        b.setOnClickListener(this);
        return b;
    }

    /** v0.8.9：把当前进度推送到通知栏（id 固定，持续更新）。 */
    private void notifyProgress(int sec, String text) {
        try {
            android.app.NotificationManager nm = (android.app.NotificationManager)
                    getSystemService(NOTIFICATION_SERVICE);
            if (nm == null) return;
            if (android.os.Build.VERSION.SDK_INT >= 26) {
                android.app.NotificationChannel ch = new android.app.NotificationChannel(
                        "twinvm", "TwinVM 运行进度",
                        android.app.NotificationManager.IMPORTANCE_LOW);
                ch.setShowBadge(false);
                nm.createNotificationChannel(ch);
            }
            android.app.Notification.Builder b = (android.os.Build.VERSION.SDK_INT >= 26)
                    ? new android.app.Notification.Builder(this, "twinvm")
                    : new android.app.Notification.Builder(this);
            b.setContentTitle("TwinVM 运行中 · " + sec + " 秒")
             .setContentText(text == null || text.length() == 0 ? "等待输出…" : text)
             .setSmallIcon(android.R.drawable.stat_sys_download)
             .setOngoing(true);
            if (text != null && text.length() > 0) {
                b.setStyle(new android.app.Notification.BigTextStyle().bigText(text));
            }
            nm.notify(1001, b.build());
        } catch (Throwable ignore) { }
    }

    /** v0.8.9：读文件尾部（用于实时显示 live 日志的最后若干字）。 */
    private static String readTail(File f, int maxChars) {
        java.io.RandomAccessFile raf = null;
        try {
            if (!f.exists() || f.length() == 0) return null;
            raf = new java.io.RandomAccessFile(f, "r");
            long len = raf.length();
            int take = (int) Math.min(len, maxChars);
            raf.seek(len - take);
            byte[] buf = new byte[take];
            raf.readFully(buf);
            String s = new String(buf, "UTF-8");
            int cut = s.indexOf('\n');
            if (cut > 0 && take < len) s = s.substring(cut + 1);
            return s;
        } catch (Throwable t) {
            return null;
        } finally {
            try { if (raf != null) raf.close(); } catch (Throwable ignore) { }
        }
    }

    @Override
    public void onClick(final View v) {
        final boolean isRun = "run".equals(v.getTag());
        final boolean isRunB = "runB".equals(v.getTag());
        /* v1.2.96：容器 B 换 dex+独立日志（串行多容器第一步） */
        if (isRunB) Native.setDex("/rom/world.dex");
        else if (isRun) Native.setDex("/rom/hello.dex");
        /* v0.8.9 ★ 实时进度：不再只显示"运行中"干等。
         * 每 400ms 读 live 日志尾部并刷新界面，同时显示已运行秒数与日志大小。 */
        if (mRunLock != null) mRunLock.acquire(10 * 60 * 1000L);   /* v1.2.69：防 CPU 睡眠 */
        /* v1.2.70 ★：前台服务才是 freezer 豁免正解（WakeLock 不阻止 cgroup 冻结） */
        try {
            android.content.Intent si = new android.content.Intent(this, RunService.class);
            if (android.os.Build.VERSION.SDK_INT >= 26) startForegroundService(si);
            else startService(si);
        } catch (Throwable ignore) { }
        mOut.setText("\u25b6 启动中…\n（正在解析 ROM / 安装拦截器）");
        /* v1.2.51：授权跟手势当场弹（不藏菜单）——首次点"运行"请求存储权限 */
        if (!hasStoragePermission()) {
            requestPermissions(new String[]{"android.permission.WRITE_EXTERNAL_STORAGE"}, 100);
        }
        final File liveFile = new File(logsDir(),
                isRunB ? "twinvm-live-b.log" : "twinvm-live.log");
        final long t0 = System.currentTimeMillis();
        /* v1.2.97 ★ 修复计时器交替闪烁：旧 ticker 未停，新 ticker 又起
         * ——两个 400ms 循环刷同一个 mOut。静态标志：起 ticker 前先让
         * 上一代退役（计数器代际制）。 */
        final int gen = ++sTickerGen;
        final boolean[] running = new boolean[] { true };
        Thread ticker = new Thread(new Runnable() {
            @Override
            public void run() {
                while (running[0] && gen == sTickerGen) {
                    try { Thread.sleep(400); } catch (InterruptedException e) { break; }
                    long sec = (System.currentTimeMillis() - t0) / 1000;
                    String tail = readTail(liveFile, 1600);
                    /* v1.2.97：终点线检测——日志出现 -pass in 后 15 秒视为
                     * 已完成，ticker 提前收摊（结果页由 stage0 返回后翻）。 */
                    if (tail != null && (tail.contains("-pass in") || tail.contains("槽位退出"))
                            && sec > 8) {
                        running[0] = false;
                        break;
                    }
                    final String show = "\u23f1 已运行 " + sec + " 秒 ｜ 日志 "
                            + (liveFile.length() / 1024) + " KB\n"
                            + "\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\n"
                            + (tail == null || tail.length() == 0
                               ? "（等待引擎输出…）" : tail);
                    mUi.post(new Runnable() {
                        @Override public void run() { mOut.setText(show); }
                    });
                    /* v0.8.9：通知栏同步（切后台也能看到进度；用通知免权限） */
                    try {
                        String[] lines = (tail == null ? "" : tail).split("\\n");
                        String last = "";
                        for (int i = lines.length - 1; i >= 0; i--) {
                            String t = lines[i].trim();
                            if (t.length() > 0 && !t.startsWith("[TwinVM] [trace]")) { last = t; break; }
                        }
                        notifyProgress((int) sec, last);
                    } catch (Throwable ignore) { }
                }
            }
        });
        ticker.setDaemon(true);
        ticker.start();
        new Thread(new Runnable() {
            @Override
            public void run() {
                String result;
                try {
                    /* v0.7.40：把 live 日志路径也交给 native —— 卡死时也能留现场 */
                    /* v1.2.97 ★ Bug 修复：容器B（isRunB）此前错走 probe()
                     * （三元只判 isRun）——点了 B 只跑了引擎探测，dex 根本没跑。
                     * 现在 A/B 都走 stage0，且 B 用自己的日志文件。 */
                    result = (isRun || isRunB)
                            ? Native.stage0(romPath(), liveFile.getAbsolutePath())
                            : Native.probe();
                } catch (Throwable t) {
                    result = "异常: " + t;
                }
                running[0] = false;
                if (mRunLock != null) mRunLock.release();   /* v1.2.69：收场释放 */
                try { stopService(new android.content.Intent(MainActivity.this, RunService.class)); } catch (Throwable ignore) { }
                try {
                    android.app.NotificationManager nm = (android.app.NotificationManager)
                            getSystemService(NOTIFICATION_SERVICE);
                    if (nm != null) nm.cancel(1001);
                } catch (Throwable ignore) { }
                final String r = result;
                /* 4.2：日志落盘——完整自检输出存文件（免截图/免 OCR 丢字）。
                 * v1.2.51：位置改 Documents/TwinVM（公共目录，文件管理器直接可见）；
                 * 权限被拒时自动回退 App 私有目录。文件名 twinvm-时间戳.log */
                String saved = null;
                try {
                    File dir = logsDir();
                    String stamp = new java.text.SimpleDateFormat(
                            "yyyyMMdd-HHmmss", java.util.Locale.US)
                            .format(new java.util.Date());
                    File logf = new File(dir, "twinvm-" + stamp + ".log");
                    FileOutputStream fo = new FileOutputStream(logf);
                    fo.write((r + "\n").getBytes("UTF-8"));
                    fo.close();
                    saved = logf.getAbsolutePath();
                } catch (Throwable ignore) { }
                final String savedPath = saved;
                mUi.post(new Runnable() {
                    @Override
                    public void run() {
                        mOut.setText(r);
                        if (savedPath != null)
                            mOut.append("\n\n[日志已保存] " + savedPath);
                        android.widget.Toast.makeText(MainActivity.this,
                                savedPath != null ? "日志已存: " + savedPath : "日志保存失败",
                                android.widget.Toast.LENGTH_LONG).show();
                    }
                });
            }
        }, "twinvm-selftest").start();
    }

    /** assets/demo.rom → filesDir（native 侧从文件 mmap 装载）。 */
    private String romPath() throws IOException {
        File f = new File(getFilesDir(), "demo.rom");
        InputStream in = getAssets().open("demo.rom");
        OutputStream o = new FileOutputStream(f);
        byte[] b = new byte[8192];
        int n;
        while ((n = in.read(b)) > 0) o.write(b, 0, n);
        o.close();
        in.close();
        return f.getAbsolutePath();
    }
}
