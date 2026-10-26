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

    private TextView mOut;
    /* v1.2.113 M2：容器C 选择态（null=未选，需弹列表） */
    private String mCRunApk;
    private String mCRunClass;
    private Button mRunCButton;              /* 单栏模式 = A 的显示（兼容旧路径） */
    private TextView mOutB;             /* v1.2.104：双栏模式的 B 栏 */
    private LinearLayout mOutRow;       /* v1.2.104：单/双栏容器 */
    private ScrollView mScrollerA, mScrollerB;
    private boolean mDualMode = false;  /* B 启动后切双栏 */
    private volatile boolean mInstalling = false;   /* v1.2.120：安装防重入 */
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
        /* v1.2.111：后台全量 CRC 自愈——损坏的 ROM 自动重提 assets
         * （v1.2.109 installApk 错位损坏的 ROM 由轻体检+此全量双保险覆盖） */
        new Thread(new Runnable() {
            @Override public void run() {
                try {
                    File rom = new File(romPath());
                    if (rom.exists() && rom.length() > 0
                            && !Native.verifyRom(rom.getAbsolutePath())) {
                        rom.delete();
                        romPath();
                    }
                } catch (Throwable ignore) { }
            }
        }, "rom-selfheal").start();

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
        title.setText("TwinVM · 用户态虚拟机");
        title.setTextColor(0xFFE8EAED);
        title.setTextSize(22);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        root.addView(title);

        TextView sub = new TextView(this);
        sub.setText("Stage 5：App 容器化 · M2 已验收 · M3 进行中\n每容器 = 独立进程 + 独立 ART");
        sub.setTextColor(0xFF9AA0A6);
        sub.setTextSize(13);
        sub.setPadding(0, 12, 0, 24);
        root.addView(sub);

        /* v1.2.98 Stage 5：Stage 4.2 已全线贯通（stress-all-pass 复现稳定）
         * ——自检使命完成，主界面精简为纯容器操作。
         * 容器A（hello.dex·stress 体检）+ 容器B（world.dex·独立体检）。 */
        Button run = makeButton("▶ 运行容器A（hello.dex）", true);
        run.setTag("run");
        root.addView(run, btnLp());
        root.addView(spacer(12));

        Button runB = makeButton("▶ 运行容器B（world.dex）", true);
        runB.setTag("runB");
        root.addView(runB, btnLp());
        root.addView(spacer(12));

        /* 探测降级为小字（不占主操作流） */
        /* v1.2.113 M2：容器C——已装应用列表（点开选 APK 运行） */
        Button runC = makeButton("📦 容器C：已装应用（点开选 APK 运行）", true);
        runC.setTag("runC");
        mRunCButton = runC;
        root.addView(runC, btnLp());
        root.addView(spacer(12));

        /* v1.2.109 M2：运行期安装——选任意 APK 装进容器 ROM */
        Button install = makeButton("⬇ 安装 APK 进容器（选文件）");
        install.setTag("install");
        root.addView(install, btnLp());

        Button probe = makeButton("引擎能力探测（诊断用）");
        probe.setTag("probe");
        probe.setTextSize(12);
        root.addView(probe, btnLp());
        root.addView(spacer(16));

        mOut = new TextView(this);
        mOut.setTypeface(Typeface.MONOSPACE);
        mOut.setTextSize(12);
        mOut.setTextColor(0xFF7EE1B4);
        /* v1.2.120：输出区圆角卡片（代码块感） */
        android.graphics.drawable.GradientDrawable ogd =
                new android.graphics.drawable.GradientDrawable();
        ogd.setCornerRadius(16f);
        ogd.setColor(0xFF0B0E12);
        ogd.setStroke(1, 0xFF22272E);
        mOut.setBackground(ogd);
        mOut.setPadding(20, 24, 20, 16);
        mOut.setText("就绪。点上方按钮启动容器：\n"
                + "每容器 = 独立槽位进程 + 独立 ART + 独立日志\n"
                + "A: twinvm-live.log · B: twinvm-live-b.log");

        /* v1.2.104 ★ 双栏显示：A 栏先独立占满；B 启动时切成水平双栏
         *（单容器=单栏、双容器=双栏——TA 需求）。 */
        mOutRow = new LinearLayout(this);
        mOutRow.setOrientation(LinearLayout.HORIZONTAL);
        mScrollerA = new ScrollView(this);
        mScrollerA.setFillViewport(true);
        mScrollerA.addView(mOut);
        mOutRow.addView(mScrollerA, new LinearLayout.LayoutParams(
                0, LinearLayout.LayoutParams.MATCH_PARENT, 1f));
        root.addView(mOutRow, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));

        setContentView(root);
    }

    private static volatile int sTickerGen = 0;   /* v1.2.97：ticker 代际 */

    /** v1.2.120：按钮视觉分级——primary（容器A/B/C 绿边）secondary（工具灰） */
    private Button makeButton(String text) { return makeButton(text, false); }

    private Button makeButton(String text, boolean primary) {
        Button b = new Button(this);
        b.setText(text);
        b.setTextSize(15);
        b.setGravity(Gravity.CENTER);
        b.setAllCaps(false);
        b.setPadding(32, 28, 32, 28);
        /* 无背景描边卡（GradientDrawable 零依赖圆角） */
        android.graphics.drawable.GradientDrawable gd =
                new android.graphics.drawable.GradientDrawable();
        gd.setCornerRadius(primary ? 24f : 18f);
        if (primary) {
            gd.setStroke(3, 0xFF7EE1B4);                 /* 主操作：绿边 */
            gd.setColor(0xFF16211C);
        } else {
            gd.setStroke(2, 0xFF3A4048);                 /* 工具操作：灰边 */
            gd.setColor(0xFF181C22);
        }
        b.setBackground(gd);
        b.setTextColor(primary ? 0xFF7EE1B4 : 0xFFB8BDC4);
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
        /* v1.2.113 M2：容器C → 已装应用列表（未选时拦截弹列表） */
        if (isRunC && mCRunApk == null) { showApkList(); return; }
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
         * v1.2.113 M2：容器 C = 应用列表 → 点选 APK → 运行。 */
        if (isRunB) Native.setDex("/rom/world.dex");
        else if (isRunC) {
            if (mCRunApk == null || mCRunClass == null || mCRunClass.length() == 0) {
                /* v1.2.114：配置不全不跑（mCRunClass 空会 CNFE 文件名推断类） */
                showApkList();
                return;
            }
            Native.setDex(mCRunApk);
            Native.setMainClass(mCRunClass);
        }
        else if (isRun) Native.setDex("/rom/hello.dex");
        /* v0.8.9 ★ 实时进度：不再只显示"运行中"干等。
         * 每 400ms 读 live 日志尾部并刷新界面，同时显示已运行秒数与日志大小。 */
        if (mRunLock != null) mRunLock.acquire(10 * 60 * 1000L);   /* v1.2.69：防 CPU 睡眠 */
        /* v1.2.70 ★：前台服务才是 freezer 豁免正解（WakeLock 不阻止 cgroup 冻结）
         * v1.2.111：拉起前重置标志——服务成功 startForeground 后才允许 stopService
         * （16:01 闪退真因：ROM 秒败→线程秒收尾→stopService 撞上 4ms 前的
         * startForegroundService 未兑现 → ForegroundServiceDidNotStartInTime） */
        RunService.sForeground = false;
        RunService.sStopRequested = false;
        try {
            android.content.Intent si = new android.content.Intent(this, RunService.class);
            if (android.os.Build.VERSION.SDK_INT >= 26) startForegroundService(si);
            else startService(si);
        } catch (Throwable ignore) { }
        if (isRunC) {
            /* M2 容器C：串行（与 A 一样的 UI 路径，双栏只留给并发 B） */
            mOut.setText("\u25b6 容器C 启动…（" + mCRunApk + "）\n");
        } else if (!isRunB) {
            mOut.setText("\u25b6 启动中…\n（正在解析 ROM / 安装拦截器）");
        } else {
            /* v1.2.104 ★ 切双栏：B 栏创建并接管 B 的显示；A 栏原样继续。 */
            if (mOutB == null) {
                mOutB = new TextView(this);
                mOutB.setTypeface(Typeface.MONOSPACE);
                mOutB.setTextSize(10);
                mOutB.setTextColor(0xFFE8B47E);          /* B 栏橙色调区分 */
                /* v1.2.120：与 A 栏同款圆角卡片（对称） */
                android.graphics.drawable.GradientDrawable bgd =
                        new android.graphics.drawable.GradientDrawable();
                bgd.setCornerRadius(16f);
                bgd.setColor(0xFF120D08);
                bgd.setStroke(1, 0xFF2E2418);
                mOutB.setBackground(bgd);
                mOutB.setPadding(20, 24, 20, 16);
                mOutB.setText("[容器B] world.dex · 启动中…\n");
                mScrollerB = new ScrollView(this);
                mScrollerB.setFillViewport(true);
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
                        if (!running[0]) break;      /* v1.2.120：主线程已收摊 */
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
                        @Override public void run() {
                            mOut.setText(show);
                            /* v1.2.120：自动滚底——长日志最新行始终可见 */
                            if (mScrollerA != null) mScrollerA.fullScroll(ScrollView.FOCUS_DOWN);
                        }
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
                /* v1.2.112：终点自动存档——live 日志下轮会被 O_TRUNC 冲掉，
                 * 先在 Documents/TwinVM/archive/ 留带时间戳的快照（TA 的
                 * "保存到 document 文件夹"） */
                try {
                    File src = liveFile;
                    if (src.exists() && src.length() > 0) {
                        File adir = new File(src.getParentFile(), "archive");
                        if (adir.exists() || adir.mkdirs()) {
                            java.text.SimpleDateFormat sf2 = new java.text.SimpleDateFormat(
                                    "-yyyyMMdd-HHmmss");
                            File dst = new File(adir, src.getName().replace(".log", "")
                                    + sf2.format(new java.util.Date()) + ".log");
                            java.io.FileInputStream fin = new java.io.FileInputStream(src);
                            java.io.FileOutputStream fout = new java.io.FileOutputStream(dst);
                            byte[] bb = new byte[8192];
                            int nn;
                            while ((nn = fin.read(bb)) > 0) fout.write(bb, 0, nn);
                            fout.close(); fin.close();
                            final String done = "📁 已存档: " + dst.getName();
                            mUi.post(new Runnable() {
                                @Override public void run() {
                                    mOut.append("\n" + done + "\n");
                                }
                            });
                        }
                    }
                } catch (Throwable ignore) { }
                RunService.sStopRequested = true;
                if (RunService.sForeground) {
                    try { stopService(new android.content.Intent(MainActivity.this, RunService.class)); } catch (Throwable ignore) { }
                }   /* 未前台的服务由 RunService 在 startForeground 后自行 stopSelf */
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
        if (mInstalling) {                     /* v1.2.120：防重入（连点双装） */
            Toast.makeText(this, "上一个安装还在进行…", Toast.LENGTH_SHORT).show();
            return;
        }
        mInstalling = true;
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
                    final String installed = Native.installApk(
                            pending.getAbsolutePath(), rom);
                    pending.delete();
                    msg = installed != null
                        ? "✅ 已安装进容器：" + installed + "\n"
                          + "（CLI 直跑 M3 启用；先跑容器C 验证链路）\n"
                          + "ROM 大小: " + new File(rom).length() / 1024 / 1024 + " MB"
                        : "❌ 安装失败（引擎层日志 installApk fail）";
                } catch (Throwable t) {
                    msg = "❌ 安装异常: " + t;
                }
                mInstalling = false;            /* v1.2.120：防重入解除 */
                final String m = msg;
                mUi.post(new Runnable() {
                    @Override public void run() { mOut.setText(m); }
                });
            }
        }, "apk-installer").start();
    }

    /* ═══════ v1.2.113 M2：容器C 应用列表 ═══════ */

    private static int le32(byte[] b, int o) {
        return (b[o] & 0xFF) | (b[o + 1] & 0xFF) << 8
                | (b[o + 2] & 0xFF) << 16 | (b[o + 3] & 0xFF) << 24;
    }

    /** v1.2.119：dex 扫描返回多候选（List：精确 Main 优先，Activity 次之，
     * 最多 3 个——列表点击后弹入口选择：M2 直跑 or M3 引导） */
    private java.util.ArrayList<String> suggestClasses(byte[] dex) {
        java.util.ArrayList<String> out = new java.util.ArrayList<String>();
        try {
            if (dex.length < 0x40) return out;
            int ssz = le32(dex, 0x38), soff = le32(dex, 0x3C);
            if (ssz <= 0 || ssz > 200000 || soff <= 0 || soff + 4L * ssz > dex.length)
                return out;
            for (int i = 0; i < ssz && out.size() < 3; i++) {
                int ido = le32(dex, soff + i * 4);
                if (ido < 0 || ido + 8 > dex.length) continue;
                int p = ido;
                while (p < dex.length && (dex[p] & 0x80) != 0) p++;  /* uleb128 size */
                if (p >= dex.length) continue;
                p++;
                int e2 = p;
                StringBuilder sb = new StringBuilder();
                while (e2 < dex.length && dex[e2] != 0 && sb.length() < 300) {
                    sb.append((char) (dex[e2] & 0xFF));
                    e2++;
                }
                String str = sb.toString();
                if (str.startsWith("L") && str.endsWith(";")) {
                    if (str.indexOf('$') >= 0) continue;   /* 内部类排除 */
                    String cls = str.substring(1, str.length() - 1).replace('/', '.');
                    if (str.endsWith("/Main;")) {        /* 精确 Main 首位 */
                        out.add(0, cls);
                    } else if (str.indexOf("Activity") > 0 && !out.contains(cls)) {
                        out.add(cls);                     /* Activity 次之 */
                    }
                }
            }
        } catch (Throwable t) { }
        return out;
    }

    /** 从（已提取的）APK 文件读 classes.dex → 扫入口候选 */
    private java.util.ArrayList<String> suggestFromApk(File apk) {
        java.util.ArrayList<String> out = new java.util.ArrayList<String>();
        try {
            java.util.zip.ZipFile zf = new java.util.zip.ZipFile(apk);
            java.util.zip.ZipEntry e = zf.getEntry("classes.dex");
            if (e == null) { zf.close(); return out; }
            java.io.ByteArrayOutputStream bo = new java.io.ByteArrayOutputStream();
            java.io.InputStream in = zf.getInputStream(e);
            byte[] b = new byte[8192];
            int n;
            while ((n = in.read(b)) > 0) bo.write(b, 0, n);
            in.close(); zf.close();
            out = suggestClasses(bo.toByteArray());
        } catch (Throwable t) { }
        return out;
    }

    private void showApkList() {
        mOut.setText("\u25b6 正在读取容器已装应用…\n");
        new Thread(new Runnable() {
            @Override public void run() {
                try {
                    final String rom = romPath();
                    final String[] apks = Native.listInstalledApks(rom);
                    if (apks == null || apks.length == 0) {
                        mUi.post(new Runnable() { @Override public void run() {
                            mOut.setText("容器里还没有应用\n（用 [\u2b07 安装 APK 进容器] 装一个）\n");
                        } });
                        return;
                    }
                    final String[] labels = new String[apks.length];
                    final java.util.ArrayList<String>[] suggests =
                            new java.util.ArrayList[apks.length];
                    for (int i = 0; i < apks.length; i++) {
                        String name = apks[i].substring(apks[i].lastIndexOf('/') + 1);
                        boolean preinstalled = name.startsWith("SampleApp")
                                || name.startsWith("TwinVM-M2-DemoApp");
                        java.util.ArrayList<String> sugg;
                        if ("/data/app/SampleApp.apk".equals(apks[i])) {
                            sugg = new java.util.ArrayList<String>();
                            sugg.add("com.twinvm.sample.Main");   /* 内置映射 */
                        } else {
                            File tmp = new File(getCacheDir(), "scan.apk");
                            sugg = Native.extractFile(rom, apks[i], tmp.getAbsolutePath())
                                    ? suggestFromApk(tmp)
                                    : new java.util.ArrayList<String>();
                            tmp.delete();
                        }
                        suggests[i] = sugg;
                        StringBuilder sb2 = new StringBuilder();
                        for (String sg : sugg) sb2.append(sb2.length() > 0 ? " | " : "").append(sg);
                        labels[i] = (preinstalled ? "[预装] " : "") + name
                                + "\n    " + (sb2.length() > 0 ? sb2 : "（入口未知）");
                    }
                    mUi.post(new Runnable() { @Override public void run() {
                        new android.app.AlertDialog.Builder(MainActivity.this)
                            .setTitle("容器已装应用（" + apks.length + " 个 · 点选运行）")
                            .setItems(labels, new android.content.DialogInterface
                                    .OnClickListener() {
                                @Override public void onClick(
                                        android.content.DialogInterface d, int which) {
                                    pickApk(apks[which], suggests[which]);
                                }
                            })
                            .setNegativeButton("取消", null)
                            .show();
                    } });
                } catch (final Throwable t) {
                    mUi.post(new Runnable() { @Override public void run() {
                        mOut.setText("读取失败: " + t + "\n");
                    } });
                }
            }
        }, "apk-lister").start();
    }

    private void pickApk(final String romInnerPath,
                        final java.util.ArrayList<String> sugg) {
        mCRunApk = romInnerPath;
        if (sugg == null || sugg.isEmpty()) {
            /* 无候选 → 输入框 */
            promptClassInput(romInnerPath, null);
            return;
        }
        if (sugg.size() == 1) {
            mCRunClass = sugg.get(0);
            mRunCButton.performClick();
            return;
        }
        /* v1.2.119：多候选 → 弹入口选择（标注模式：CLI 直跑 / M3 引导） */
        final String[] items = new String[sugg.size()];
        for (int i = 0; i < sugg.size(); i++) {
            String c = sugg.get(i);
            items[i] = c + (c.contains("Activity") ? "\n    （M3：Activity 引导）"
                                                  : "\n    （CLI 直跑）");
        }
        new android.app.AlertDialog.Builder(this)
            .setTitle("选择入口：" + romInnerPath.substring(romInnerPath.lastIndexOf('/') + 1))
            .setItems(items, new android.content.DialogInterface.OnClickListener() {
                @Override public void onClick(android.content.DialogInterface d, int which) {
                    mCRunClass = sugg.get(which);
                    mRunCButton.performClick();
                }
            })
            .setNegativeButton("取消", new android.content.DialogInterface
                    .OnClickListener() {
                @Override public void onClick(android.content.DialogInterface d, int w) {
                    mCRunApk = null; mCRunClass = null;
                }
            })
            .setOnCancelListener(new android.content.DialogInterface.OnCancelListener() {
                @Override public void onCancel(android.content.DialogInterface d) {
                    mCRunApk = null; mCRunClass = null;
                }
            })
            .show();
    }

    private void promptClassInput(final String romInnerPath, final String prefill) {
        mCRunApk = romInnerPath;
        final android.widget.EditText et = new android.widget.EditText(this);
        et.setText(prefill != null ? prefill : "");
        et.setHint("如 com.example.Main（含 main(String[]) 的类）");
        new android.app.AlertDialog.Builder(this)
            .setTitle("入口主类：" + romInnerPath.substring(romInnerPath.lastIndexOf('/') + 1))
            .setMessage("该 APK 的入口是 Activity（运行界面需 M3 阶段）。\n"
                + "可输入含 main(String[]) 的工具类名试跑：")
            .setView(et)
            .setPositiveButton("运行", new android.content.DialogInterface
                    .OnClickListener() {
                @Override public void onClick(android.content.DialogInterface d, int w) {
                    String c = et.getText().toString().trim();
                    if (c.length() == 0) { mCRunApk = null; return; }
                    mCRunClass = c;
                    mRunCButton.performClick();
                }
            })
            .setNegativeButton("取消", new android.content.DialogInterface
                    .OnClickListener() {
                @Override public void onClick(android.content.DialogInterface d, int w) {
                    mCRunApk = null; mCRunClass = null;
                }
            })
            .setOnCancelListener(new android.content.DialogInterface
                    .OnCancelListener() {
                @Override public void onCancel(android.content.DialogInterface d) {
                    mCRunApk = null; mCRunClass = null;   /* 点外面也清（防残留直跑） */
                }
            })
            .show();
    }

    /** v1.2.111：ROM 轻体检（头部 magic + 总长 + 末 inode 越界检查）。
     * v1.2.109 的 installApk 曾把表写错位——这种损坏靠末 inode 越界检出。 */
    private boolean romLooksHealthy(File f) {
        try {
            java.io.RandomAccessFile raf = new java.io.RandomAccessFile(f, "r");
            try {
                byte[] h = new byte[64];
                if (raf.read(h) != 64) return false;
                if (h[0] != 'T' || h[1] != 'V' || h[2] != 'M' || h[3] != 'R'
                        || h[4] != 'O' || h[5] != 'M' || h[6] != '1') return false;
                long count = (h[8] & 0xFFL) | (h[9] & 0xFFL) << 8
                        | (h[10] & 0xFFL) << 16 | (h[11] & 0xFFL) << 24;
                long doff = 0, dsize = 0;
                for (int i = 0; i < 8; i++) {
                    doff |= (h[16 + i] & 0xFFL) << (8 * i);
                    dsize |= (h[24 + i] & 0xFFL) << (8 * i);
                }
                if (count <= 0 || count > 4096) return false;
                if (doff + dsize != f.length()) return false;
                /* 末 inode 越界检查（garbage 表项的 off/size 巨大——必炸） */
                byte[] ino = new byte[292];
                raf.seek(64 + (count - 1) * 292);
                if (raf.read(ino) != 292) return false;
                long foff = 0, fsz = 0;
                for (int i = 0; i < 8; i++) {
                    foff |= (ino[276 - 256 + i] & 0xFFL) << (8 * i);   /* ino[20..27] */
                    fsz |= (ino[268 - 256 + i] & 0xFFL) << (8 * i);    /* ino[12..19] */
                }
                return foff + fsz <= f.length();
            } finally { raf.close(); }
        } catch (Throwable t) { return false; }
    }

    /** v1.2.120：统一按钮布局参数 + 间隔 */
    private LinearLayout.LayoutParams btnLp() {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.topMargin = 8;
        return lp;
    }

    private View spacer(int h) {
        View v = new View(this);
        v.setLayoutParams(new LinearLayout.LayoutParams(1, h));
        return v;
    }

    private synchronized String romPath() throws IOException {
        /* v1.2.120：synchronized——rom-selfheal 线程与 onClick 线程
         * 冷启动并发首启时的双写竞态（双击场景） */
        File f = new File(getFilesDir(), "demo.rom");
        /* v1.2.149 ★★★【ROM 版本指纹】：filesDir 的 ROM 与 assets 的
         * 不是同一个（APK 升级不替换 filesDir）——10:07 案实锤：
         * TA 装了 v148 但跑的还是 v145 的 bootstrap（旧 ROM 持续服役）。
         * 修：落地时写 md5 指纹；启动时 assets 指纹不一致 → 重新落地。
         * （运行期安装的 APK 会丢——恢复出厂语义，可接受：引擎升级优先） */
        File fp = new File(getFilesDir(), "demo.rom.fp");
        try {
            java.security.MessageDigest md =
                    java.security.MessageDigest.getInstance("MD5");
            java.io.InputStream ain = getAssets().open("demo.rom");
            byte[] ab = new byte[65536];
            int an;
            while ((an = ain.read(ab)) > 0) md.update(ab, 0, an);
            ain.close();
            StringBuilder asb = new StringBuilder();
            for (byte b3 : md.digest()) asb.append(String.format("%02x", b3));
            String want = asb.toString();
            String have = null;
            if (fp.exists()) {
                java.io.BufferedReader br = new java.io.BufferedReader(
                        new java.io.FileReader(fp));
                have = br.readLine();
                br.close();
            }
            if (f.exists() && !want.equals(have)) {
                f.delete();               /* 引擎已升级——换新 ROM */
            }
            if (!f.exists() || f.length() == 0) {
                /* 落地 + 写指纹 */
                InputStream in = getAssets().open("demo.rom");
                OutputStream o = new FileOutputStream(f);
                byte[] b = new byte[8192];
                int n;
                while ((n = in.read(b)) > 0) o.write(b, 0, n);
                o.close(); in.close();
                java.io.FileWriter fw2 = new java.io.FileWriter(fp);
                fw2.write(want);
                fw2.close();
            }
            return f.getAbsolutePath();
        } catch (Throwable t) {
            /* 指纹机制任何异常 → 退回旧逻辑（下方） */
        }
        /* v1.2.109 ★ M2：只在首次落地（不再每次覆盖）——运行期安装进
         * ROM 的 APK 才能留存。清 App 数据即恢复出厂。
         * v1.2.111：轻体检不过 → 当损坏重提（自愈第一步，后台还有全量 CRC） */
        if (f.exists() && f.length() > 0 && !romLooksHealthy(f)) {
            f.delete();
        }
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
