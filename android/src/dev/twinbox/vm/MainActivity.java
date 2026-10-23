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
import android.widget.Toast;

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

    private TextView mOut;              /* 单栏模式 = A 的显示（兼容旧路径） */
    private TextView mOutB;             /* v1.2.104：双栏模式的 B 栏 */
    private LinearLayout mOutRow;       /* v1.2.104：单/双栏容器 */
    private ScrollView mScrollerA, mScrollerB;
    private boolean mDualMode = false;  /* B 启动后切双栏 */
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
        /* v1.2.110 ★：冷启动先清 RunService 遗留账——上个进程被杀时
         * service 若有未兑现的 startForegroundService，系统会在本进程
         * attach 后连坐清算（ForegroundServiceDidNotStartInTime）。
         * stopService 触发 system_server 关闭旧 record。 */
        try { stopService(new android.content.Intent(this, RunService.class)); }
        catch (Throwable ignore) { }

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
        sub.setText("Stage 5：App 容器化——一个 App 里的独立虚拟手机（每容器独立 ART）");
        sub.setTextColor(0xFF9AA0A6);
        sub.setTextSize(13);
        sub.setPadding(0, 12, 0, 24);
        root.addView(sub);

        /* v1.2.98 Stage 5：Stage 4.2 已全线贯通（stress-all-pass 复现稳定）
         * ——自检使命完成，主界面精简为纯容器操作。
         * 容器A（hello.dex·stress 体检）+ 容器B（world.dex·独立体检）。 */
        Button run = makeButton("▶ 运行容器A（hello.dex）");
        run.setTag("run");
        root.addView(run);

        Button runB = makeButton("▶ 运行容器B（world.dex）");
        runB.setTag("runB");
        root.addView(runB);

        /* 探测降级为小字（不占主操作流） */
        /* v1.2.108 M2：容器C——真 APK 类路径（/data/app/SampleApp.apk） */
        Button runC = makeButton("▶ 运行容器C（SampleApp.apk · M2）");
        runC.setTag("runC");
        root.addView(runC);

        /* v1.2.109 M2：运行期安装——选任意 APK 装进容器 ROM */
        Button install = makeButton("⬇ 安装 APK 进容器（选文件）");
        install.setTag("install");
        root.addView(install);

        Button probe = makeButton("引擎能力探测（诊断用）");
        probe.setTag("probe");
        probe.setTextSize(12);
        LinearLayout.LayoutParams lp =
                new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                        LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.topMargin = 24;
        root.addView(probe, lp);

        mOut = new TextView(this);
        mOut.setTypeface(Typeface.MONOSPACE);
        mOut.setTextSize(12);
        mOut.setTextColor(0xFF7EE1B4);
        mOut.setPadding(8, 24, 8, 8);
        mOut.setText("就绪。点上方按钮启动容器：\n"
                + "每容器 = 独立槽位进程 + 独立 ART + 独立日志\n"
                + "A: twinvm-live.log · B: twinvm-live-b.log");

        /* v1.2.104 ★ 双栏显示：A 栏先独立占满；B 启动时切成水平双栏
         *（单容器=单栏、双容器=双栏——TA 需求）。 */
        mOutRow = new LinearLayout(this);
        mOutRow.setOrientation(LinearLayout.HORIZONTAL);
        mScrollerA = new ScrollView(this);
        mScrollerA.addView(mOut);
        mOutRow.addView(mScrollerA, new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.MATCH_PARENT, 1f));
        root.addView(mOutRow, new LinearLayout.LayoutParams(
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
        final boolean isRunC = "runC".equals(v.getTag());
        /* v1.2.109 M2：安装——拉起系统文件选择器（结果在 onActivityResult） */
        if ("install".equals(v.getTag())) {
            try {
                android.content.Intent it = new android.content.Intent(
                        android.content.Intent.ACTION_OPEN_DOCUMENT);
                it.addCategory(android.content.Intent.CATEGORY_OPENABLE);
                it.setType("application/vnd.android.package-archive");
                startActivityForResult(it, 2001);
            } catch (Throwable t) {
                /* 部分文件管理器不认 mime——退化为任意类型 */
                try {
                    android.content.Intent it2 = new android.content.Intent(
                            android.content.Intent.ACTION_OPEN_DOCUMENT);
                    it2.addCategory(android.content.Intent.CATEGORY_OPENABLE);
                    it2.setType("*/*");
                    startActivityForResult(it2, 2001);
                } catch (Throwable t2) {
                    Toast.makeText(this, "文件选择器不可用: " + t2,
                            Toast.LENGTH_LONG).show();
                }
            }
            return;
        }
        /* v1.2.96：容器 B 换 dex+独立日志（串行多容器第一步）。
         * v1.2.108 M2：容器 C = 真 APK 类路径（/data/app/SampleApp.apk）。 */
        if (isRunB) Native.setDex("/rom/world.dex");
        else if (isRunC) { Native.setDex("/data/app/SampleApp.apk");
                           Native.setMainClass("com.twinvm.sample.Main"); }
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
        if (isRunC) {
            /* M2 容器C：串行（与 A 一样的 UI 路径，双栏只留给并发 B） */
            mOut.setText("\u25b6 容器C 启动…（APK 类路径 /data/app/SampleApp.apk）\n");
        } else if (!isRunB) {
            mOut.setText("\u25b6 启动中…\n（正在解析 ROM / 安装拦截器）");
        } else {
            /* v1.2.104 ★ 切双栏：B 栏创建并接管 B 的显示；A 栏原样继续。 */
            if (mOutB == null) {
                mOutB = new TextView(this);
                mOutB.setTypeface(Typeface.MONOSPACE);
                mOutB.setTextSize(10);
                mOutB.setTextColor(0xFFE8B47E);          /* B 栏橙色调区分 */
                mOutB.setPadding(8, 24, 8, 8);
                mOutB.setText("[容器B] world.dex · 启动中…\n");
                mScrollerB = new ScrollView(this);
                mScrollerB.addView(mOutB);
                mOutRow.addView(mScrollerB, new LinearLayout.LayoutParams(
                        0, LinearLayout.LayoutParams.MATCH_PARENT, 1f));
                mDualMode = true;
                /* A 栏字号缩小以适配双栏 */
                mOut.setTextSize(10);
            }
            mOutB.append("[容器B] 已请求启动（独立进程）…\n");
        }
        /* v1.2.51：授权跟手势当场弹（不藏菜单）——首次点"运行"请求存储权限 */
        if (!hasStoragePermission()) {
            requestPermissions(new String[]{"android.permission.WRITE_EXTERNAL_STORAGE"}, 100);
        }
        final File liveFile = new File(logsDir(),
                isRunC ? "twinvm-live-c.log"
              : isRunB ? "twinvm-live-b.log"
              :          "twinvm-live.log");
        final long t0 = System.currentTimeMillis();
        /* v1.2.97 ★ 修复计时器交替闪烁：旧 ticker 未停，新 ticker 又起
         * ——两个 400ms 循环刷同一个 mOut。静态标志：起 ticker 前先让
         * 上一代退役（计数器代际制）。 */
        /* v1.2.105 ★ 修复"A 栏被 B 停摆"：v1.2.97 的代际制在 B 点击时也
         * ++sTickerGen——把 A 正在跑的 ticker 挤掉（A 停在最后一帧）。
         * 修正：代际只由【A/探测的 ticker】维护——B 不启 ticker 也不该
         * 动计数器。用独立的 boolean 守卫替代全局递增语义。 */
        final int gen;
        if (isRunB) {
            gen = sTickerGen;          /* B：读当前代（自己不启 ticker） */
        } else {
            gen = ++sTickerGen;        /* A/探测：新代 */
        }
        final boolean[] running = new boolean[] { !isRunB };
        if (isRunB) {
            /* v1.2.105 ★ bwatch 误判门闩：启动 1.5s 后读到的可能是【上一轮
             * 的旧日志】（含旧"槽位退出"）→ 误判 B 完成一去不返（B 栏冻住
             * 的真凶）。门闩：记录点击时刻的文件 mtime，只有 mtime 变了
             * （本轮 O_TRUNC 重写发生）才开始判终点；没变就显示"等待启动"。 */
            final long clickMtime = liveFile.exists() ? liveFile.lastModified() : 0L;
            Thread bwatch = new Thread(new Runnable() {
                @Override public void run() {
                    try { Thread.sleep(1500); } catch (InterruptedException ignored) { }
                    for (int i = 0; i < 400; i++) {   /* 最多等 ~13 分钟 */
                        try { Thread.sleep(2000); } catch (InterruptedException ignored) { }
                        /* 门闩检查：本轮的 O_TRUNC 还没发生 → 还在启动期 */
                        long nowM = liveFile.exists() ? liveFile.lastModified() : 0L;
                        if (nowM <= clickMtime) {
                            final int waited = i;
                            mUi.post(new Runnable() {
                                @Override public void run() {
                                    if (mOutB != null) mOutB.setText(
                                        "[容器B] 等待本轮启动…（" + (waited * 2) + "s）\n");
                                }
                            });
                            continue;
                        }
                        String tail = readTail(liveFile, 400);
                        if (tail != null && (tail.contains("-pass in") || tail.contains("槽位退出"))) {
                            final String t = tail.length() > 300 ? tail.substring(tail.length()-300) : tail;
                            mUi.post(new Runnable() {
                                @Override public void run() {
                                    if (mOutB != null) mOutB.append("\n[容器B 完成]\n" + t + "\n");
                                }
                            });
                            break;
                        }
                        /* v1.2.104：B 栏实时尾随（每 2s 刷 3 行——轻量心跳） */
                        if (tail != null && mOutB != null) {
                            final String t2 = tail.length() > 400 ? tail.substring(tail.length()-400) : tail;
                            mUi.post(new Runnable() {
                                @Override public void run() {
                                    if (mOutB != null) {
                                        mOutB.setText("[容器B] live-b 尾随…\n" + t2);
                                        mScrollerB.fullScroll(ScrollView.FOCUS_DOWN);
                                    }
                                }
                            });
                        }
                    }
                }
            });
            bwatch.setDaemon(true);
            bwatch.start();
        }
        Thread ticker = new Thread(new Runnable() {
            @Override
            public void run() {
                while (running[0] && gen == sTickerGen) {
                    try { Thread.sleep(400); } catch (InterruptedException e) { break; }
                    long sec = (System.currentTimeMillis() - t0) / 1000;
                    String tail = readTail(liveFile, 1600);
                    /* v1.2.97：终点线检测——日志出现 -pass in 后 15 秒视为
                     * 已完成，ticker 提前收摊（结果页由 stage0 返回后翻）。 */
                    if (tail != null && (tail.contains("-pass in")
                            || tail.contains("SampleApp-all-ok") || tail.contains("槽位退出"))
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
                    /* v1.2.101 ★ M1 并发：A 走 stage0（宿主进程内，串行语义）；
                     * B 走 stage0Parallel——fork 独立 b-runner 进程，与 A 真并发。
                     * （A 保持原样：并发时 A 先点，B 再点即双活。） */
                    if (isRunB) {
                        result = Native.stage0Parallel(romPath(), liveFile.getAbsolutePath());
                    } else if (isRun || isRunC) {
                        result = Native.stage0(romPath(), liveFile.getAbsolutePath());
                    } else {
                        result = Native.probe();
                    }
                } catch (Throwable t) {
                    result = "异常: " + t;
                }
                if (isRunB) {
                    /* v1.2.102 ★ B 并发旁路：秒回的启动确认不触发任何收尾——
                     * 不停 RunService（A 可能还在跑）、不 release 锁、不翻页、
                     * 不弹"日志已保存"（那是 A 的终点仪式）。B 的结果走
                     * live-b.log 监听提示。 */
                    return;
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
    /* v1.2.109 M2：文件选择器回调——拷贝 + 调 native 安装 */
    @Override
    protected void onActivityResult(int requestCode, int resultCode,
                                    android.content.Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != 2001 || resultCode != RESULT_OK || data == null
                || data.getData() == null) return;
        final android.net.Uri uri = data.getData();
        mOut.setText("⬇ 安装中…（ROM 增量重写，约 1~2 秒）\n" + uri.getLastPathSegment());
        new Thread(new Runnable() {
            @Override public void run() {
                String msg;
                try {
                    /* 1. 拷到 filesDir/pending.apk（ContentResolver 流） */
                    File pending = new File(getFilesDir(), "pending.apk");
                    java.io.InputStream in = getContentResolver().openInputStream(uri);
                    java.io.OutputStream out = new java.io.FileOutputStream(pending);
                    byte[] b = new byte[8192];
                    int n;
                    while ((n = in.read(b)) > 0) out.write(b, 0, n);
                    out.close(); in.close();
                    /* 2. 确保 ROM 已落地 + native 安装 */
                    final String rom = romPath();
                    final boolean ok = Native.installApk(pending.getAbsolutePath(), rom);
                    pending.delete();
                    msg = ok ? "✅ 已安装进容器（/data/app/install-*.apk）\n"
                              + "容器C 跑它需入口类名——先用 SampleApp 验证链路，"
                              + "装好的 APK 将在 M3（ActivityThread）启用。\n"
                              + "ROM 大小: " + new File(rom).length() / 1024 / 1024 + " MB"
                            : "❌ 安装失败（看日志）";
                } catch (Throwable t) {
                    msg = "❌ 安装异常: " + t;
                }
                final String m = msg;
                mUi.post(new Runnable() {
                    @Override public void run() { mOut.setText(m); }
                });
            }
        }, "apk-installer").start();
    }

    private String romPath() throws IOException {
        File f = new File(getFilesDir(), "demo.rom");
        /* v1.2.109 ★ M2：只在首次落地（不再每次覆盖）——运行期安装进
         * ROM 的 APK 才能留存。清 App 数据即恢复出厂。 */
        if (!f.exists() || f.length() == 0) {
            InputStream in = getAssets().open("demo.rom");
            OutputStream o = new FileOutputStream(f);
            byte[] b = new byte[8192];
            int n;
            while ((n = in.read(b)) > 0) o.write(b, 0, n);
            o.close();
            in.close();
        }
        return f.getAbsolutePath();
    }
}
