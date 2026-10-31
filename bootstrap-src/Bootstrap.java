package com.twinvm.m3;

import java.lang.reflect.Constructor;
import java.lang.reflect.Method;
import android.app.Activity;
import android.content.Context;
import android.os.Handler;

public class Bootstrap {
    public static void main(String[] args) throws Exception {
        String cls = args.length > 0 ? args[0] : "com.twinvm.demo.MainActivity";
        System.out.println("M3-Bootstrap: 目标 Activity = " + cls);

        /* M3-c ② ★★★：framework.jar 进 classloader——BinderInternal/
         * Instrumentation 一众 framework 类的加载根（08:32 案的根治方向）。
         * dex 元数据 lazy mmap——加载不等于初始化，<clinit> 仍按需跑。 */
        ClassLoader fwLoader = null;
        try {
            fwLoader = new dalvik.system.DexClassLoader(
                    "/system/framework/framework.jar",
                    null,
                    null,
                    Bootstrap.class.getClassLoader());
            Thread.currentThread().setContextClassLoader(fwLoader);
            Class<?> bi = Class.forName("com.android.internal.os.BinderInternal", true, fwLoader);
            System.out.println("M3-fw: framework.jar 挂载 ✓ BinderInternal 可加载（fwLoader）");
            Class<?> bi2 = Class.forName("com.android.internal.os.BinderInternal", true,
                    Bootstrap.class.getClassLoader());
            System.out.println("M3-fw: BCP 里 BinderInternal 也可加载 ✓（framework 在 BCP 实锤）");
        } catch (Throwable t) {
            System.out.println("M3-fw: framework 挂载失败 = " + t);
            Throwable c2 = t.getCause() != null ? t.getCause() : t;
            System.out.println("M3-fw-因: " + c2.getClass().getName() + ": " + c2.getMessage());
        }

        /* v56 ★：Build 烙印侦查——12:52 案（ActivityThread.<clinit>:404
         * → Build NoClassDef）。首个引爆点定位+cause 链。 */
        try {
            Class<?> bc = Class.forName("android.os.Build", true,
                    Bootstrap.class.getClassLoader());
            System.out.println("M3-fw: Build 预初始化 OK（无烙印）");
        } catch (Throwable t) {
            System.out.println("M3-fw: Build 预初始化 = " + t);
            Throwable c5 = t.getCause() != null ? t.getCause() : t;
            while (c5.getCause() != null && c5.getCause() != c5) c5 = c5.getCause();
            System.out.println("M3-fw: Build-因 = " + c5.getClass().getName()
                    + ": " + c5.getMessage());
            StackTraceElement[] st5 = c5.getStackTrace();
            for (int i = 0; i < Math.min(6, st5.length); i++)
                System.out.println("  at " + st5[i]);
        }
        /* v90: AIC.clinit:119 -> Build.IS_AGING_VERSION NoSuchFieldError
         * ROM neibu version mismatch. Diagnostic: enumerate Build fields. */
        try {
            Class<?> bc2 = Class.forName("android.os.Build", false,
                    Bootstrap.class.getClassLoader());
            try {
                java.lang.reflect.Field ag = bc2.getField("IS_AGING_VERSION");
                System.out.println("M3-fw: IS_AGING_VERSION exist = " + ag.get(null));
            } catch (NoSuchFieldException nsf) {
                System.out.println("M3-fw: IS_AGING_VERSION MISSING");
                java.lang.reflect.Field[] all = bc2.getDeclaredFields();
                System.out.println("M3-fw: Build total fields = " + all.length);
                for (java.lang.reflect.Field fx : all) {
                    int mod = fx.getModifiers();
                    if (java.lang.reflect.Modifier.isStatic(mod)
                            && fx.getType() == boolean.class) {
                        try { System.out.println("M3-fw: boolField " + fx.getName() + " = " + fx.get(null)); }
                        catch (Throwable ig) { System.out.println("M3-fw: boolField " + fx.getName() + " = <unreadable>"); }
                    }
                }
            }
        } catch (Throwable t90o) {
            System.out.println("M3-fw: v90 fail = " + t90o);
        }
        /* M3-c ① ★：Process 烙印根治尝试——受控预初始化（在一切 framework
         * 触碰前引爆 oplus 链，失败也在这里记录；若 ART 允许二次初始化
         * （boot loader 的 error 态清除——实测见真章））。 */
        try {
            Class<?> pc = Class.forName("android.os.Process", true,
                    Bootstrap.class.getClassLoader());
            System.out.println("M3-fw: Process 预初始化 OK（烙印不存在或已清）");
        } catch (Throwable t) {
            System.out.println("M3-fw: Process 预初始化 = " + t.getClass().getSimpleName());
        }
        /* v17 ★★★：整个流程挪进 HandlerThread（自带 Looper）——
         * 14:38 案根因：Bootstrap 主线程无 Looper →
         * HostCallbacks 构造炸 → host=null → mFragmentManager null。
         * 真机的 ActivityThread.main 第一件事就是 Looper.prepareMainLooper()
         * ——我们在等价位置补上。 */
        final String fCls = cls;
        final Object done = new Object();
        final java.util.concurrent.atomic.AtomicReference<Throwable> err =
                new java.util.concurrent.atomic.AtomicReference<Throwable>(null);
        Thread th = new Thread(new Runnable() {
            @Override public void run() {
                try {
                    /* v22 ★：去掉 Looper.prepare（v18 引入的"真机等价物"
                     * 是唯一剩余炸点：prepare→MessageQueue 构造→
                     * initIsProcessAllowedToUseConcurrent→Process.clinit→oplus SEGV/EIIE。
                     * 19:36 案定罪：manualAttach 排在 prepare 之后=从未执行。
                     * M3-b 只跑生命周期（无 UI 消息循环）——不需要 Looper。
                     * 真需要时 M3-c 换用户态 Looper。） */
                    /* v53 ★：恢复 Looper.prepare——v22 删除是为绕 stub Process 的
                     * Stub! 炸（myProcessName）——core-patch v2（真 Process）后此路已通。
                     * mHost 需要 Handler，Handler 需要 Looper。 */
                    android.os.Looper.prepare();
                    System.out.println("M3-flow: Looper.prepare ✓（v53——Process 已真）");
                    runFlow(fCls);
                    /* v34：loop() 也删——15:23 案：loop 对 null looper 抛
                     * RuntimeException（收尾噪音）+ stub 环境里一切 Looper
                     * 触碰都不可信 */
                } catch (Throwable t) {
                    err.set(t);
                }
                synchronized (done) { done.notifyAll(); }
            }
        }, "tvm-main");
        /* v19 ★：oplus RefClass 第一现场侦查（先于 Looper.prepare 触碰）：
         * 1) RefClass 静态块是否炸
         * 2) RefClass.get(ZygoteProcessExtPlugin.class) 返回什么 */
        try {
            Class<?> rc = Class.forName("com.oplus.reflect.RefClass");
            System.out.println("M3-ref: RefClass 装载 OK=" + rc);
            try {
                Class<?> ep = Class.forName("android.os.ZygoteProcessExtPlugin");
                System.out.println("M3-ref: ExtPlugin 装载 OK=" + ep);
                java.lang.reflect.Method gm2 = null;
                for (java.lang.reflect.Method mm : rc.getDeclaredMethods())
                    if (mm.getName().equals("get") && mm.getParameterCount() == 1) { gm2 = mm; break; }
                if (gm2 != null) {
                    gm2.setAccessible(true);
                    Object r = gm2.invoke(null, new Object[]{ ep });
                    System.out.println("M3-ref: RefClass.get(ExtPlugin) = " + r);
                }
            } catch (Throwable epFail) {
                System.out.println("M3-ref: ExtPlugin/调用 异常 = " + epFail);
                Throwable c3 = epFail.getCause() != null ? epFail.getCause() : epFail;
                System.out.println("M3-ref-因: " + c3.getClass().getName() + ": " + c3.getMessage());
                StackTraceElement[] s3 = c3.getStackTrace();
                for (int i = 0; i < Math.min(5, s3.length); i++)
                    System.out.println("  at " + s3[i]);
            }
        } catch (Throwable rcFail) {
            System.out.println("M3-ref: RefClass 装载异常 = " + rcFail);
        }
        /* v20：侦查在 main（Thread/Looper 之前）——v19 的设计失误：
         runFlow 在 Looper.prepare 之后才执行，而炸点恰是 prepare 本身
         → v18/v19 的打点从未打出（"九版空转"实为"输出被 EIIE 挡在 prepare"） */
                th.start();
        synchronized (done) {
            try { done.wait(120000); } catch (InterruptedException ignored) { }
        }
        Throwable t2 = err.get();
        if (t2 != null) {
            System.out.println("M3-Bootstrap: 流程异常 = " + t2);
            Throwable c2 = t2.getCause() != null ? t2.getCause() : t2;
            StackTraceElement[] s2 = c2.getStackTrace();
            for (int i = 0; i < Math.min(5, s2.length); i++)
                System.out.println("  at " + s2[i]);
        }
        /* v44：热路径状态位收尾报告（JIT 吃不掉字段写） */
        try {
            System.out.println("M3-trace(" + sTrace.size() + "): " + sTrace);
        } catch (Throwable ignore) { }
        System.out.println("M3-Bootstrap: 完（looper 线程流程收尾）");
    }

    static void runFlow(String cls) throws Exception {
        System.out.println("M3-flow: 进入 runFlow（looper 线程就绪）");
        /* v18：普通 Thread + 手动 Looper.prepare（HandlerThread 类链绕开） */
        try {
            ClassLoader cl = Bootstrap.class.getClassLoader();
        System.out.println("M3-CL: " + cl);
        while (cl != null) {
            System.out.println("M3-CL: 层级 " + cl.getClass().getName()
                + " -> " + cl);
            cl = cl.getParent();
        }
        /* 列 DexPathList 的 dexElements（反射）——看 apk 段在不在 */
        try {
            ClassLoader mine = Bootstrap.class.getClassLoader();
            java.lang.reflect.Field fPath = mine.getClass().getSuperclass()
                    .getDeclaredField("pathList");
            fPath.setAccessible(true);
            Object pathList = fPath.get(mine);
            System.out.println("M3-pathList: " + pathList);
            java.lang.reflect.Field fEls = pathList.getClass()
                    .getDeclaredField("dexElements");
            fEls.setAccessible(true);
            Object[] els = (Object[]) fEls.get(pathList);
            System.out.println("M3-elements: " + (els == null ? -1 : els.length));
            for (int i = 0; els != null && i < els.length; i++) {
                System.out.println("M3-elem[" + i + "]: " + els[i]);
            }
        } catch (Throwable t) {
            System.out.println("M3-pathList 反射失败: " + t);
        }
        /* v3 侦查：直接用 ZipFile 列 apk 的 entries（绕过 ART 的 DexFile） */
        try {
            java.util.zip.ZipFile zf = new java.util.zip.ZipFile(
                    "/data/app/TwinVM-M2-DemoApp.apk");
            java.util.Enumeration<? extends java.util.zip.ZipEntry> en = zf.entries();
            System.out.println("M3-zip: entry 列表：");
            while (en.hasMoreElements()) {
                java.util.zip.ZipEntry e2 = en.nextElement();
                System.out.println("M3-zip: " + e2.getName()
                        + " size=" + e2.getSize() + " method=" + e2.getMethod());
            }
            java.util.zip.ZipEntry dx = zf.getEntry("classes.dex");
            System.out.println("M3-zip: classes.dex = " + dx);
            if (dx != null) {
                java.io.InputStream in = zf.getInputStream(dx);
                byte[] head = new byte[8];
                int rn = in.read(head);
                in.close();
                System.out.println("M3-zip: dex 头 " + rn + "B: "
                        + Integer.toHexString((head[0] & 0xFF) << 24 | (head[1] & 0xFF) << 16
                        | (head[2] & 0xFF) << 8 | (head[3] & 0xFF))
                        + " (dex=0x78563412)");
            }
            zf.close();
        } catch (Throwable t) {
            System.out.println("M3-zip: ZipFile 打开/读取失败 = " + t);
        }
        /* v4 侦查：直接构造 DexFile（绕过 PathClassLoader 的元素状态） */
        try {
            java.lang.reflect.Constructor<?> dc =
                    Class.forName("dalvik.system.DexFile").getDeclaredConstructor(
                            new Class[]{java.io.File.class});
            dc.setAccessible(true);
            Object df = dc.newInstance(new Object[]{
                    new java.io.File("/data/app/TwinVM-M2-DemoApp.apk")});
            System.out.println("M3-dexfile: 直接构造 OK = " + df);
            java.lang.reflect.Method lm = df.getClass().getMethod(
                    "loadClass", new Class[]{String.class, ClassLoader.class});
            Class<?> dc2 = (Class<?>) lm.invoke(df, new Object[]{
                    "com.twinvm.demo.MainActivity",
                    Bootstrap.class.getClassLoader()});
            System.out.println("M3-dexfile: loadClass = " + dc2);
            /* v5：mCookie 透视（cookie[0]=oat，后续=dex 句柄——长度=数量） */
            try {
                java.lang.reflect.Field fc = Class.forName("dalvik.system.DexFile")
                        .getDeclaredField("mCookie");
                fc.setAccessible(true);
                Object cookie = fc.get(df);
                System.out.println("M3-cookie: " + cookie);
                if (cookie instanceof Object[]) {
                    Object[] carr = (Object[]) cookie;
                    System.out.println("M3-cookie: len=" + carr.length
                            + " (oat+ndex)——第0=" + carr[0]);
                }
            } catch (Throwable t2) {
                System.out.println("M3-cookie: 反射失败 = " + t2);
            }
        } catch (Throwable t) {
            System.out.println("M3-dexfile: 直接 DexFile 失败 = " + t);
            Throwable cc = t.getCause() != null ? t.getCause() : t;
            System.out.println("M3-dexfile: 因 = " + cc.getClass().getName()
                    + ": " + cc.getMessage());
        }
        try {
            Class<?> ac = Class.forName(cls);
            System.out.println("M3-Bootstrap: 类装载 OK：" + ac.getName());
            Object act;
            boolean attached = false;
            try {
                /* v8 ★★★ M3-b 核心：模拟 ActivityThread.performLaunchActivity
                 * 的 attach 段——真机上这一步由 system_server 协商后调用：
                 * activity.attach(context, activityThread, instr, token,
                 *                ident, application, intent, activityInfo,
                 *                title, parent, id, config)
                 * 我们提供最小 Context（StubContext 系）——字段不再 null，
                 * super 链就能走通。 */
                Constructor<?> ctor = ac.getDeclaredConstructor();
                ctor.setAccessible(true);
                act = ctor.newInstance();
                System.out.println("M3-Bootstrap: 实例化 OK（无参构造）");
            } catch (Throwable ctorFail) {
                /* v6 ★：构造器炸（stub 雷或 super 链）→ Unsafe.allocateInstance
                 * 裸分配实例（跳过一切构造器）——生命周期方法反射照调。 */
                System.out.println("M3-Bootstrap: 构造失败（" + ctorFail.getClass().getSimpleName()
                        + "）→ Unsafe.allocateInstance 裸分配");
                /* sun.misc 不在 android.jar——纯反射拿（ROM 的 core-oj 有） */
                Class<?> uc = Class.forName("sun.misc.Unsafe");
                java.lang.reflect.Field uf = uc.getDeclaredField("theUnsafe");
                uf.setAccessible(true);
                Object U = uf.get(null);
                java.lang.reflect.Method am = uc.getMethod("allocateInstance",
                        new Class[]{Class.class});
                act = am.invoke(U, new Object[]{ac});
                System.out.println("M3-Bootstrap: 裸实例化 OK：" + act.getClass().getName());
            }
            /* v16 ★：mFragments 预填（FragmentController）——attach 内部
             * mFragments.attachHost(fragment) on null（14:14 案）。
             * FragmentController 可 new（有公开构造收 HostCallbacks）——
             * 反射找无参或造最小参。 */
            try {
                Class<?> fc = Class.forName("android.app.FragmentController");
                java.lang.reflect.Constructor<?> fcC = null;
                for (java.lang.reflect.Constructor<?> cc3 : fc.getDeclaredConstructors())
                    if (cc3.getParameterCount() == 1) { fcC = cc3; break; }
                if (fcC != null) {
                    fcC.setAccessible(true);
                    Class<?> hb = fcC.getParameterTypes()[0];   /* FragmentHostCallbacks */
                    Object host = null;
                    try {
                        java.lang.reflect.Constructor<?> hbC = null;
                        for (java.lang.reflect.Constructor<?> c4 : hb.getDeclaredConstructors())
                            if (c4.getParameterCount() >= 4) { hbC = c4; break; }
                        if (hbC != null) {
                            hbC.setAccessible(true);
                            Class<?>[] hps = hbC.getParameterTypes();
                            Object[] hargs = new Object[hps.length];
                            for (int hi = 0; hi < hps.length; hi++) {
                                Class<?> hp = hps[hi];
                                if (hp == android.app.Activity.class || hp.isInstance(act)) hargs[hi] = act;
                                else if (hp == boolean.class) hargs[hi] = Boolean.FALSE;
                                else if (hp == int.class) hargs[hi] = Integer.valueOf(0);
                                else if (hp == android.os.Handler.class) {
                                    hargs[hi] = new android.os.Handler();
                                } else {
                                    try {
                                        java.lang.reflect.Constructor<?> hc2 = hp.getDeclaredConstructor();
                                        hc2.setAccessible(true);
                                        hargs[hi] = hc2.newInstance();
                                    } catch (Throwable e5) { hargs[hi] = null; }
                                }
                            }
                            host = hbC.newInstance(hargs);
                        }
                    } catch (Throwable hbFail) {
                        System.out.println("M3-frag: HostCallbacks 构造失败 = " + hbFail);
                    }
                    Object fctrl = fcC.newInstance(new Object[]{ host });
                    java.lang.reflect.Field ff = null;
                    for (Class<?> k = ac; ff == null && k != null; k = k.getSuperclass())
                        try { ff = k.getDeclaredField("mFragments"); } catch (Throwable ignore) { }
                    if (ff != null) {
                        ff.setAccessible(true);
                        ff.set(act, fctrl);
                        System.out.println("M3-frag: mFragments 预填 OK（host=" + (host != null) + "）");
                    }
                }
            } catch (Throwable fragOuter) {
                System.out.println("M3-frag: 预填失败 = " + fragOuter);
            }
            /* v14 ★：OnePlus 定制 mActivityExt 预填——attach 内部会调
             * IActivityExt.attach(context, componentName)（12:38 案三帧实锤），
             * 反射预填动态代理（接口可代理）后再 attach。字段名多试几个。 */
            try {
                for (String fn : new String[]{"mActivityExt", "mExt", "mActivityExtImpl"}) {
                    try {
                        java.lang.reflect.Field fex = null;
                        for (Class<?> k = ac; fex == null && k != null; k = k.getSuperclass())
                            try { fex = k.getDeclaredField(fn); } catch (Throwable ignore) { }
                        if (fex != null) {
                            fex.setAccessible(true);
                            Class<?> extCls = fex.getType();
                            Object extProxy = java.lang.reflect.Proxy.newProxyInstance(
                                extCls.getClassLoader(), new Class[]{ extCls },
                                new java.lang.reflect.InvocationHandler() {
                                    @Override public Object invoke(Object pr, java.lang.reflect.Method m3,
                                            Object[] ar) throws Throwable {
                                        Class<?> rt = m3.getReturnType();
                                        if (rt == boolean.class) return Boolean.FALSE;
                                        if (rt == int.class) return Integer.valueOf(0);
                                        return null;
                                    }
                                });
                            fex.set(act, extProxy);
                            System.out.println("M3-ext: " + fn + " 预填代理 OK");
                            break;
                        }
                    } catch (Throwable fexFail) {
                        System.out.println("M3-ext: " + fn + " 试填失败 = " + fexFail);
                    }
                }
            } catch (Throwable extOuter) { }
            /* v9 ★：attach 汇合点统一调（裸分配分支也 attach——v8 结构 bug：
             * attach 只在构造成功分支，TA 的日志走裸分配分支 → attach 没跑） */
            try {
                /* v21 ★★★：手工填字段版 attach——绕开 Activity.attach 整条链
                 *（attach 内部触 Process.clinit → oplus RefClass → JIT SEGV
                 * 17:15 案双线定罪）。attach 本质=批量填字段，我们直接填：
                 * mActivityInfo/mFragments/mApplication/mInstrumentation… */
                manualAttach(act);
                attached = true;
                System.out.println("M3-Bootstrap: attach 完成（最小 Context 注入）");
            } catch (Throwable atFail) {
                System.out.println("M3-Bootstrap: attach 异常（继续裸跑）: "
                        + atFail);
                Throwable ac2 = atFail.getCause() != null ? atFail.getCause() : atFail;
                System.out.println("M3-attach-因: " + ac2.getClass().getName()
                        + ": " + ac2.getMessage());
                StackTraceElement[] as = ac2.getStackTrace();
                for (int i3 = 0; i3 < Math.min(3, as.length); i3++)
                    System.out.println("  at " + as[i3]);
            }
            System.out.println("M3-Bootstrap: attach 态=" + attached);
            int okSegs = 0;
            String[] segs = {"onCreate", "onStart", "onResume", "onPause", "onStop"};
            for (String seg : segs) {
                try {
                    if (seg.equals("onCreate"))
                        call(act, "onCreate", new Class[]{android.os.Bundle.class}, new Object[]{null});
                    else
                        call(act, seg, null, null);
                    System.out.println("M3-Bootstrap: " + seg + " 完成");
                    okSegs++;
                } catch (Throwable segT) {
                    Throwable cc = segT.getCause() != null ? segT.getCause() : segT;
                    System.out.println("M3-Bootstrap: " + seg + " 段异常（继续）: "
                            + cc.getClass().getName() + ": " + cc.getMessage()
                            + " ← " + seg);
                    StackTraceElement[] ss = cc.getStackTrace();
                    for (int i2 = 0; i2 < Math.min(3, ss.length); i2++)
                        System.out.println("  at " + ss[i2]);
                }
            }
            System.out.println("M3-Bootstrap: 生命周期 " + okSegs + "/5 段完成");
            /* M3-a 判据：≥2 段完成即达标（生命周期调用链打通——
             * UI 构建的 stub 阻断属于 M3-b 真 framework 的靶区） */
            if (okSegs >= 2) {
                System.out.println("M3-a-OK（" + okSegs + "/5 段，生命周期链路打通）");
                System.out.println("M3-NEXT: UI 构建需真 framework（M3-b）");
            } else {
                System.out.println("M3-NEXT-CASE");
            }
        } catch (Throwable t) {
            System.out.println("M3-Bootstrap: 异常 = " + t);
            Throwable c = t.getCause() != null ? t.getCause() : t;
            System.out.println("M3 异常类型: " + c.getClass().getName());
            System.out.println("M3 异常消息: " + c.getMessage());
            StackTraceElement[] st = c.getStackTrace();
            for (int i = 0; i < Math.min(3, st.length); i++)
                System.out.println("  at " + st[i]);
            System.out.println("M3-NEXT-CASE");
        }
        }   /* v18 的 try 闭合（吞 Process.clinit 类的 EIIE——oplus ZygoteProcess 扩展的坑绕开尝试） */
        finally {
            System.out.println("M3-flow: runFlow 退出");
        }
    }

    /** v8：模拟 ActivityThread.performLaunchActivity 的 attach 段
     * （真 framework 的 Activity.onCreate 全家不再 null 崩）。 */
    static {
        /* v12 ★：native 桥预载——framework 的 native 方法兜底
         *（isLoggable 等，UnsatisfiedLink 案的根治）。 */
        try {
            System.loadLibrary("tvmbridge");
            System.out.println("M3-bridge: libtvmbridge.so 装载 OK");
        } catch (Throwable t) {
            System.out.println("M3-bridge: 装载失败 = " + t);
        }
    }

    static Object sTheActivityThread;    /* v15：systemMain 的单例 */
    static Object sSystemContext;          /* 真 ContextImpl */

    /** v15 ★★★ 系统性适配：不再手工 new ActivityThread——
     * 跑 framework 自己的 systemMain()（系统进程入口，设计上
     * 不需要 system_server 协商），拿真 ActivityThread + 真
     * ContextImpl（ext/Instrumentation/LoadedApk 全初始化）。 */
    /** v21：手工填 attach 的五字段（不再调 Activity.attach——它碰 Process） */
    static void manualAttach(Object act) throws Exception {
        Class<?> ac = act.getClass();
        /* v28 ★★★：填 mBase（attachBaseContext 等价物——不调真 attach，
         * 反射直填 mBase 字段=MiniContext）。onResume 的
         * getSystemServiceName(ContentWrapper:979)、onPause 的
         * getContentCaptureManager(1399) 全走 mBase。 */
        try {
            java.lang.reflect.Method ab = null;
            for (Class<?> k = ac; ab == null && k != null; k = k.getSuperclass())
                try { ab = k.getDeclaredMethod("attachBaseContext", Context.class); } catch (Throwable ignore) { }
            if (ab != null) {
                ab.setAccessible(true);
                MiniContext mc = new MiniContext();
                ab.invoke(act, mc);
                System.out.println("M3-man: mBase = MiniContext（attachBaseContext ✓）");
                /* v93 ★（21:51 案）：v92 的 getInstance(mc) 也 InvocationTarget
                 * （AM 构造:533 Handler(getMainLooper) → :536 initialFocusAppearance
                 * → getResources 或 :537 tryConnect 的 binder 链）——构造整条都是雷。
                 * 终解：【Unsafe 直造】跳过构造——壳 + 全字段手填 + sInstance
                 * 静态槽直写（AOSP :229 `private static AccessibilityManager
                 * sInstance;`，:501/:514 的 if(null) 判断被我们预占）。
                 * View 构造时 getInstance → sInstance 非空 → 直接返回壳 →
                 * isEnabled 读 mIsEnabled=true → || 短路 → AIC 永不被触达。 */
                try {
                    Class<?> amC0 = Class.forName("android.view.accessibility.AccessibilityManager");
                    Class<?> uc2 = Class.forName("sun.misc.Unsafe");
                    java.lang.reflect.Field uf2 = null;
                    for (java.lang.reflect.Field fx : uc2.getDeclaredFields())
                        if (fx.getType() == uc2) { uf2 = fx; break; }
                    uf2.setAccessible(true);
                    Object U2 = uf2.get(null);
                    Object inst = uc2.getMethod("allocateInstance", Class.class).invoke(U2, amC0);
                    /* 逐字段手填（名字/类型全部 AOSP 源码核实）：
                     * mLock:232 Object、mUserId:238 int、mHandler:241 final Handler
                     * （Unsafe 可写 final）、mCallback:243、mIsEnabled:246 boolean */
                    java.lang.reflect.Field fl;
                    java.lang.reflect.Method off = uc2.getMethod("objectFieldOffset", java.lang.reflect.Field.class);
                    java.lang.reflect.Method pu = uc2.getMethod("putObject", Object.class, long.class, Object.class);
                    java.lang.reflect.Method pb = uc2.getMethod("putBoolean", Object.class, long.class, boolean.class);
                    java.lang.reflect.Method pi = uc2.getMethod("putInt", Object.class, long.class, int.class);
                    fl = amC0.getDeclaredField("mLock");      fl.setAccessible(true);
                    pu.invoke(U2, inst, off.invoke(U2, fl), new Object());
                    fl = amC0.getDeclaredField("mUserId");    fl.setAccessible(true);
                    pi.invoke(U2, inst, off.invoke(U2, fl), 0);
                    fl = amC0.getDeclaredField("mHandler");    fl.setAccessible(true);
                    pu.invoke(U2, inst, off.invoke(U2, fl),
                            new android.os.Handler(android.os.Looper.myLooper()));
                    fl = amC0.getDeclaredField("mCallback");   fl.setAccessible(true);
                    pu.invoke(U2, inst, off.invoke(U2, fl), null);
                    fl = amC0.getDeclaredField("mIsEnabled"); fl.setAccessible(true);
                    pb.invoke(U2, inst, off.invoke(U2, fl), true);
                    /* sInstance 静态槽直写（:229） */
                    fl = amC0.getDeclaredField("sInstance");  fl.setAccessible(true);
                    fl.set(null, inst);
                    System.out.println("M3-man: v93 AM 壳就位+短路 ✓");
                } catch (Throwable t93) {
                    System.out.println("M3-man: v93 失败 = " + t93);
                }
                /* v94b ★（23:22 案）：PhoneWindow 构造的 Settings.Global.getInt
                 * → getString:18791 → ContentResolver.getUserId() on null
                 * （resolver 壳的 mContext 填充失败——隐藏字段反射 InvocationTarget）
                 * 构造路线放弃 → Unsafe 直造 PhoneWindow 壳（跳构造：
                 * Settings/LayoutInflater/ProxyOnBack 全跳）+ 三字段手填：
                 *   mLayoutInflater（PhoneLayoutInflater——MiniContext 服务）
                 *   mDecor / mContentParent（PhoneWindow:223/:231 核实）*/
                try {
                    Class<?> pwC = Class.forName("com.android.internal.policy.PhoneWindow");
                    Class<?> uc3 = Class.forName("sun.misc.Unsafe");
                    java.lang.reflect.Field uf3 = null;
                    for (java.lang.reflect.Field fx : uc3.getDeclaredFields())
                        if (fx.getType() == uc3) { uf3 = fx; break; }
                    uf3.setAccessible(true);
                    Object U3 = uf3.get(null);
                    Object pw = uc3.getMethod("allocateInstance", Class.class).invoke(U3, pwC);
                    /* Window 基类字段：mContext（构造里唯一的必填） */
                    Class<?> wC = Class.forName("android.view.Window");
                    java.lang.reflect.Field fmc = wC.getDeclaredField("mContext");
                    fmc.setAccessible(true); fmc.set(pw, act);   /* v18：Activity（有 Theme）*/
                    /* PhoneWindow.mLayoutInflater */
                    Object li = null;
                    try {
                        li = Class.forName("com.android.internal.policy.PhoneLayoutInflater")
                                .getConstructor(Context.class).newInstance((Context) act);
                    } catch (Throwable tli) {
                        System.out.println("M3-man: v94b PhoneLayoutInflater = " + tli);
                    }
                    java.lang.reflect.Field fli = pwC.getDeclaredField("mLayoutInflater");
                    fli.setAccessible(true); fli.set(pw, li);
                    /* DecorView：Unsafe 壳（构造 :293 有 AnimationUtils 资源链
                     * ——AM 值链对 interpolator 资源不完全——跳构造最稳）*/
                    Class<?> dvC = Class.forName("com.android.internal.policy.DecorView");
                    Object dv = uc3.getMethod("allocateInstance", Class.class).invoke(U3, dvC);
                    /* mContentParent：FrameLayout 直造（公有类） */
                    android.widget.FrameLayout fpl = new android.widget.FrameLayout((Context) act);
                    /* 字段写入（AOSP :223/:231 核实：mDecor/mContentParent） */
                    java.lang.reflect.Field fd = pwC.getDeclaredField("mDecor");
                    fd.setAccessible(true); fd.set(pw, dv);
                    java.lang.reflect.Field fc = pwC.getDeclaredField("mContentParent");
                    fc.setAccessible(true); fc.set(pw, fpl);
                    /* Activity.mWindow 写入 */
                    java.lang.reflect.Field fw = act.getClass().getSuperclass()
                            .getDeclaredField("mWindow");
                    fw.setAccessible(true); fw.set(act, pw);
                    System.out.println("M3-man: v94b PhoneWindow 壳链 ✓（decor+content+inflater 就位）");
                } catch (Throwable t94) {
                    System.out.println("M3-man: v94 失败 = " + t94);
                    java.io.StringWriter sw = new java.io.StringWriter();
                    t94.printStackTrace(new java.io.PrintWriter(sw));
                    System.out.println("M3-man: v94 栈 = " + sw);
                }
            }
        } catch (Throwable t) {
            System.out.println("M3-man: attachBaseContext 失败 = " + t);
        }
        /* v35 ★★★：ServiceManager 去雷——onPause/onStop 的
         * getService2 NPE（rawGetService:430）：反射预填
         * ServiceManager.sServiceManager = IServiceManager Proxy
         * （getService2 返 null → rawGetService 判 null 返 null
         *   → getService 返 null → 调用方各自判空自退） */
        try {
            Class<?> smc = Class.forName("android.os.ServiceManager");
            java.lang.reflect.Field smf = null;
            for (java.lang.reflect.Field f2 : smc.getDeclaredFields()) {
                String fn = f2.getName();
                if (fn.contains("ServiceManager") || fn.equals("sServiceManager")) {
                    Class<?> ft = f2.getType();
                    if (ft.getName().contains("IServiceManager") || ft.isInterface()) { smf = f2; break; }
                }
            }
            /* 兜底：任意 IServiceManager 类型字段 */
            if (smf == null) {
                for (java.lang.reflect.Field f2 : smc.getDeclaredFields()) {
                    if (f2.getType().getName().contains("IServiceManager")) { smf = f2; break; }
                }
            }
            if (smf != null) {
                smf.setAccessible(true);
                final java.lang.reflect.Method mGetService2;
                Class<?> ism = smf.getType();
                java.lang.reflect.Method gs2 = null;
                try { gs2 = ism.getMethod("getService2", String.class); } catch (Throwable ignore) { }
                final java.lang.reflect.Method gs2f = gs2;
                Object proxy = java.lang.reflect.Proxy.newProxyInstance(
                        Thread.currentThread().getContextClassLoader(),
                        new Class<?>[]{ ism },
                        new java.lang.reflect.InvocationHandler() {
                            @Override public Object invoke(Object p, java.lang.reflect.Method m, Object[] a) {
                                /* v44 ★：JIT 打点对抗——println 在热路径会被 JIT 吃掉。
                                 * 状态位写 static（JIT 不优化字段写）+ 收尾统一报告。 */
                                sDebug.set(m.getName() + "→" + m.getReturnType().getSimpleName());
                                System.out.println("M3-ism: call " + m.getName() + " ret=" + m.getReturnType().getName());   /* v38 打点 */
                                Object rr;
                                /* v88 ★★（06:17 案）：getService2(name) 按【服务名】分流。
                                 * AOSP 机制：ActivityTaskManager.getService()
                                 *   → ServiceManager.getService("activity_task")
                                 *   → rawGetService → getService2(name)
                                 *       .getServiceWithMetadata().service   ← 链上解引用
                                 *   → IActivityTaskManager.Stub.asInterface(binder)
                                 *   → binder 壳 queryLocalInterface 返 null → 自动出 Proxy
                                 * 此前 deepStub 只按返回类型（Service）——不分名字，
                                 * metadata 链返回的壳缺 binder 字段 → ATM 单例 NPE。
                                 * 修：name∈{activity_task, activity} 时返回带
                                 * ServiceWithMetadata(内含 Binder 壳) 的 Service——
                                 * Binder 壳 Unsafe 造（transact 全走桥桩不 deref）。 */
                                boolean isAtm = a != null && a.length == 1 && a[0] instanceof String
                                        && ("activity_task".equals(a[0]) || "activity".equals(a[0]));
                                if (isAtm && "getService2".equals(m.getName())) {
                                    try {
                                        Class<?> svc = m.getReturnType();
                                        Class<?> uc2 = Class.forName("sun.misc.Unsafe");
                                        java.lang.reflect.Field uf2 = null;
                                        for (java.lang.reflect.Field fx : uc2.getDeclaredFields())
                                            if (fx.getType() == uc2) { uf2 = fx; break; }
                                        uf2.setAccessible(true);
                                        Object U2 = uf2.get(null);
                                        rr = uc2.getMethod("allocateInstance", Class.class).invoke(U2, svc);
                                        /* Service._value → ServiceWithMetadata{service: Binder 壳} */
                                        try {
                                            Class<?> swmC = Class.forName("android.os.ServiceWithMetadata");
                                            Object swm = uc2.getMethod("allocateInstance", Class.class).invoke(U2, swmC);
                                            Object binder = uc2.getMethod("allocateInstance", Class.class).invoke(U2,
                                                    Class.forName("android.os.Binder"));
                                            java.lang.reflect.Field svf = null;
                                            for (java.lang.reflect.Field fx : swmC.getDeclaredFields())
                                                if (!java.lang.reflect.Modifier.isStatic(fx.getModifiers())
                                                        && fx.getType().getName().equals("android.os.IBinder"))
                                                    { svf = fx; break; }
                                            /* Binder 类直接找（字段名零猜测：遍历类型匹配） */
                                            if (svf == null)
                                                for (java.lang.reflect.Field fx : swmC.getDeclaredFields())
                                                    if (!java.lang.reflect.Modifier.isStatic(fx.getModifiers())
                                                            && fx.getType().getName().endsWith("Binder"))
                                                        { svf = fx; break; }
                                            if (svf != null) {
                                                svf.setAccessible(true);
                                                svf.set(swm, binder);
                                            }
                                            java.lang.reflect.Field vf2 = null;
                                            for (java.lang.reflect.Field fx : svc.getDeclaredFields())
                                                if (!java.lang.reflect.Modifier.isStatic(fx.getModifiers())
                                                        && fx.getType() == Object.class)
                                                    { vf2 = fx; break; }
                                            if (vf2 != null) {
                                                vf2.setAccessible(true);
                                                vf2.set(rr, swm);
                                            }
                                            sTrace.add("A0:ATM壳✓:" + a[0]);
                                        } catch (Throwable t88) {
                                            sTrace.add("A0:ATM壳败:" + t88);
                                        }
                                    } catch (Throwable t88o) {
                                        rr = null;
                                    }
                                } else {
                                try {
                                    rr = deepStub(m.getReturnType());
                                    sTrace.add("A1:返=" + (rr == null ? "null" : rr.getClass().getName()));
                                } catch (Throwable deepT) {
                                    sTrace.add("A1:抛=" + deepT);
                                    rr = null;
                                }
                                }   /* v88 else 闭合 */
                                if ("getService2".equals(m.getName()) && rr == null) {
                                    /* v42：Service 类直接 Unsafe 造（绕 deepStub） */
                                    try {
                                        Class<?> svc = m.getReturnType();
                                        Class<?> uc = Class.forName("sun.misc.Unsafe");
                                        java.lang.reflect.Field uf = null;
                                        for (java.lang.reflect.Field f2 : uc.getDeclaredFields())
                                            if (f2.getType() == uc) { uf = f2; break; }
                                        uf.setAccessible(true);
                                        Object U = uf.get(null);
                                        rr = uc.getMethod("allocateInstance", Class.class).invoke(U, svc);
                                        sTrace.add("A2:直造=" + (rr != null));
                                        /* v46 ★★：直造分支的字段续命（20:56 案：
                                         * deepStub 类分支在 guest 里走不通返回 null，
                                         * 直造接住了 Service——字段填充也搬到这里） */
                                        try {
                                            for (java.lang.reflect.Field vf : svc.getDeclaredFields()) {
                                                int vm = vf.getModifiers();
                                                if (java.lang.reflect.Modifier.isStatic(vm)
                                                        || java.lang.reflect.Modifier.isFinal(vm)) continue;
                                                Class<?> vt = vf.getType();
                                                if (vt.isPrimitive() || vt == String.class) continue;
                                                try {
                                                    vf.setAccessible(true);
                                                    Class<?> uc2 = Class.forName("sun.misc.Unsafe");
                                                    java.lang.reflect.Field uf2 = null;
                                                    for (java.lang.reflect.Field f2 : uc2.getDeclaredFields())
                                                        if (f2.getType() == uc2) { uf2 = f2; break; }
                                                    if (uf2 != null) {
                                                        uf2.setAccessible(true);
                                                        Object U2 = uf2.get(null);
                                                        Object sub = uc2.getMethod("allocateInstance", Class.class)
                                                                .invoke(U2, vt);
                                                        if (sub != null) {
                                                            vf.set(rr, sub);
                                                            sTrace.add("v46填:" + vf.getName() + "=" + vt.getSimpleName());
                                                        }
                                                    }
                                                } catch (Throwable f46) {
                                                    sTrace.add("v46字段失败:" + vf.getName() + ":" + f46);
                                                }
                                            }
                                            sTrace.add("A3:v46字段已填:" + svc.getName());
                                        } catch (Throwable t46) {
                                            sTrace.add("v46整段失败:" + t46);
                                        }
                                    } catch (Throwable t5) {
                                        System.out.println("M3-ism: v42 直造失败 = " + t5);
                                    }
                                }
                                return rr;
                            }
                        });
                smf.set(null, proxy);
                System.out.println("M3-man: sServiceManager = IServiceManager Proxy ✓");
                /* v89 ★★（07:44 案）：ATM 单例预填——绕开 AIDL Proxy 的 Parcel 链。
                 * 07:44 现场取证：A0:ATM壳✓ 后 asInterface 出真 AIDL Proxy →
                 *   Proxy 方法体 Parcel.obtain() → new Parcel() → nativeCreate
                 *   → UnsatisfiedLinkError。
                 * 根修（纯 Java 零新 native）：把 ATM.IActivityTaskManagerSingleton
                 * 的 Singleton.mInstance 直接预填 deepStub Proxy——
                 *   getService() 拿到我们的 Proxy → getActivityClientController()
                 *   → deepStub(IActivityClientController) → Proxy——
                 *   全程无 Parcel、无 transact。
                 * AOSP 证据：ATM.java:153 字段名 IActivityTaskManagerSingleton；
                 *   Singleton.java:24 字段名 mInstance（源码拉取核实，非猜测）。 */
                try {
                    Class<?> atmC = Class.forName("android.app.ActivityTaskManager");
                    java.lang.reflect.Field sgnF = null;
                    for (java.lang.reflect.Field fx : atmC.getDeclaredFields())
                        if (java.lang.reflect.Modifier.isStatic(fx.getModifiers())
                                && fx.getType().getName().equals("android.util.Singleton"))
                            { sgnF = fx; break; }
                    if (sgnF != null) {
                        sgnF.setAccessible(true);
                        Object singleton = sgnF.get(null);
                        Class<?> sgC = Class.forName("android.util.Singleton");
                        java.lang.reflect.Field miF = sgC.getDeclaredField("mInstance");
                        miF.setAccessible(true);
                        Class<?> itf = Class.forName("android.app.IActivityTaskManager");
                        Object atmProxy = java.lang.reflect.Proxy.newProxyInstance(
                                itf.getClassLoader(), new Class<?>[]{ itf },
                                new java.lang.reflect.InvocationHandler() {
                                    @Override public Object invoke(Object p2, java.lang.reflect.Method m2, Object[] a2) {
                                        if (m2.getName().equals("toString")) return "TVM-ATM";
                                        if (m2.getName().equals("hashCode")) return Integer.valueOf(0);
                                        if (m2.getName().equals("equals")) return Boolean.FALSE;
                                        return deepStub(m2.getReturnType());
                                    }
                                });
                        miF.set(singleton, atmProxy);
                        System.out.println("M3-man: v89 ATM 单例预填 ✓");
                    } else {
                        System.out.println("M3-man: v89 Singleton 字段未找到");
                    }
                } catch (Throwable t89) {
                    System.out.println("M3-man: v89 ATM 预填失败 = " + t89);
                }
            } else {
                System.out.println("M3-man: sServiceManager 字段未找到（名字猜错）");
            }
        } catch (Throwable t) {
            System.out.println("M3-man: ServiceManager 去雷失败 = " + t);
        }
        /* v91 ★（12:15 案）：View.setFlags:18848 → AM.isEnabled:659 →
         * hasAnyDirectConnection:670 → AIC.<clinit>:119 → Build.IS_AGING_VERSION
         * NoSuchFieldError（ROM 版本错配：core-patch 无此字段——无法加字段）。
         * 正解=短路 AOSP:618-621：return mIsEnabled || hasAnyDirectConnection()...
         * ——把【所有已存在的 AM 实例】的 mIsEnabled Unsafe 强写 true。
         * AM 实例是 AccessibilityManager.getInstance(cache per-context)——
         * demo 的 View 拿的是 MiniContext 链的实例——在 onCreate 前批量修。 */
        try {
            Class<?> amC = Class.forName("android.view.accessibility.AccessibilityManager");
            /* 反射找静态实例缓存（sInstance/sInstances——名字不重要，遍历找） */
            for (java.lang.reflect.Field fx : amC.getDeclaredFields()) {
                if (!java.lang.reflect.Modifier.isStatic(fx.getModifiers())) continue;
                Class<?> ft = fx.getType();
                if (ft == amC || ft.getName().equals("android.util.ArrayMap")
                        || ft.getName().equals("java.util.HashMap")
                        || ft.getName().equals("android.util.SparseArray")) {
                    fx.setAccessible(true);
                    Object cache = fx.get(null);
                    if (cache == null) continue;
                    System.out.println("M3-fw: v91 AM缓存字段 " + fx.getName()
                            + " (" + ft.getSimpleName() + ")");
                    /* SparseArray/HashMap 里的 AM 实例全修 */
                    try {
                        if (cache instanceof java.util.Map) {
                            for (Object v : ((java.util.Map<?, ?>) cache).values()) fixAmEnable(v);
                        } else if (cache.getClass().getName().endsWith("SparseArray")) {
                            /* SparseArray 无公共 values——反射走 */
                            java.lang.reflect.Method gsv = cache.getClass().getMethod("get", int.class);
                            java.lang.reflect.Method gsz = cache.getClass().getMethod("size");
                            int n = (Integer) gsz.invoke(cache);
                            for (int i = 0; i < n; i++) {
                                Object k = cache.getClass().getMethod("keyAt", int.class).invoke(cache, i);
                                Object v = gsv.invoke(cache, k);
                                fixAmEnable(v);
                            }
                        } else if (amC.isInstance(cache)) {
                            fixAmEnable(cache);
                        }
                    } catch (Throwable ig) { }
                }
            }
            /* v91b：主动预创建单例（getInstance）——View 构造时直接命中缓存 */
            try {
                Class<?> amC0 = Class.forName("android.view.accessibility.AccessibilityManager");
                java.lang.reflect.Method gi = amC0.getMethod("getInstance", Class.forName("android.content.Context"));
                Object inst = gi.invoke(null, new Object[]{ null });
                fixAmEnable(inst);
                System.out.println("M3-fw: v91b AM 预创建短路 ✓");
            } catch (Throwable t91b) {
                System.out.println("M3-fw: v91b 失败 = " + t91b);
            }
            System.out.println("M3-fw: v91 AM mIsEnabled 扫描完成");
        } catch (Throwable t91) {
            System.out.println("M3-fw: v91 失败 = " + t91);
        }
        /* v33 ★★★：Unsafe.allocateInstance 预填【构造麻烦的 framework 对象】——
         * 不跑构造不跑静态块，只占位非 null：
         * mContentCaptureManager（onResume/onPause 的 getContentCaptureManager:1399
         *   → 不再走静态方法 → 不再碰 Process 烙印）
         * mAutofillManager（同类炸点预防） */
        try {
            Class<?> uc = Class.forName("sun.misc.Unsafe");
            java.lang.reflect.Field uf = null;
            for (java.lang.reflect.Field f2 : uc.getDeclaredFields())
                if (f2.getType() == Class.forName("sun.misc.Unsafe")) { uf = f2; break; }
            if (uf != null) {
                uf.setAccessible(true);
                Object U = uf.get(null);
                java.lang.reflect.Method alloc = uc.getMethod("allocateInstance", Class.class);
                String[] lazyFields = { "mContentCaptureManager", "mAutofillManager" };
                for (String lf : lazyFields) {
                    try {
                        java.lang.reflect.Field f3 = null;
                        for (Class<?> k = ac; f3 == null && k != null; k = k.getSuperclass())
                            try { f3 = k.getDeclaredField(lf); } catch (Throwable ignore) { }
                        if (f3 == null) { System.out.println("M3-man: " + lf + " 字段无（跳过）"); continue; }
                        f3.setAccessible(true);
                        Object o2 = alloc.invoke(U, f3.getType());
                        f3.set(act, o2);
                        System.out.println("M3-man: " + lf + " = Unsafe 占位实例 ✓");
                    } catch (Throwable t3) {
                        System.out.println("M3-man: " + lf + " Unsafe 失败 = " + t3);
                    }
                }
            } else {
                System.out.println("M3-man: Unsafe 字段没找到");
            }
        } catch (Throwable uFail) {
            System.out.println("M3-man: Unsafe 获取失败 = " + uFail);
        }
        String[][] fills = {
            {"mActivityInfo", "android.content.pm.ActivityInfo"},
            {"mApplication", "android.app.Application"},
            {"mInstrumentation", "android.app.Instrumentation"},  /* M3-c③：fw loader 版见下 */
            /* v24：生命周期段内 NPE 字段大而全（23:14 日志：
             * monitor-enter null=被 synchronized 的字段 null；
             * onStop 的 ActivityTransitionState null） */
            {"mActivityTransitionState", "android.app.ActivityTransitionState"},
        };
        /* v24：非构造型字段——直接 new/常量填充 */
        try {
            java.lang.reflect.Field f = null;
            for (Class<?> k = ac; f == null && k != null; k = k.getSuperclass())
                try { f = k.getDeclaredField("mManagedCursor"); } catch (Throwable ignore) { }
            if (f != null) {
                f.setAccessible(true);
                f.set(act, new java.util.ArrayList<android.database.Cursor>());
                System.out.println("M3-man: mManagedCursor = ArrayList");
            }
        } catch (Throwable t) {
            System.out.println("M3-man: mManagedCursor 失败 = " + t);
        }
        try {
            java.lang.reflect.Field f = null;
            for (Class<?> k = ac; f == null && k != null; k = k.getSuperclass())
                try { f = k.getDeclaredField("mUiThread"); } catch (Throwable ignore) { }
            if (f != null) {
                f.setAccessible(true);
                f.set(act, Thread.currentThread());
                System.out.println("M3-man: mUiThread = currentThread");
            }
        } catch (Throwable t) {
            System.out.println("M3-man: mUiThread 失败 = " + t);
        }
        /* v25 ★：monitor-enter 定罪——collectActivityLifecycleCallbacks:1824
         * synchronized(mActivityLifecycleCallbacks)——字段 null。填 ArrayList */
        try {
            java.lang.reflect.Field f = null;
            for (Class<?> k = ac; f == null && k != null; k = k.getSuperclass())
                try { f = k.getDeclaredField("mActivityLifecycleCallbacks"); } catch (Throwable ignore) { }
            if (f == null) {
                System.out.println("M3-man: mActivityLifecycleCallbacks 字段未找到（API 差异）");
            } else {
                f.setAccessible(true);
                f.set(act, new java.util.ArrayList<Object>());
                System.out.println("M3-man: mActivityLifecycleCallbacks = ArrayList");
            }
        } catch (Throwable t) {
            System.out.println("M3-man: mActivityLifecycleCallbacks 失败 = " + t);
        }
        try {
            /* mAllLoaderManagers：ArrayMap（API 34 类型） */
            java.lang.reflect.Field f = null;
            for (Class<?> k = ac; f == null && k != null; k = k.getSuperclass())
                try { f = k.getDeclaredField("mAllLoaderManagers"); } catch (Throwable ignore) { }
            if (f != null) {
                f.setAccessible(true);
                Class<?> vt2 = f.getType();
                Object v2 = vt2.newInstance();   /* ArrayMap 有无参构造 */
                f.set(act, v2);
                System.out.println("M3-man: mAllLoaderManagers = " + vt2.getSimpleName());
            }
        } catch (Throwable t) {
            System.out.println("M3-man: mAllLoaderManagers 失败 = " + t);
        }
        for (String[] fi : fills) {
            try {
                java.lang.reflect.Field f = null;
                for (Class<?> k = ac; f == null && k != null; k = k.getSuperclass())
                    try { f = k.getDeclaredField(fi[0]); } catch (Throwable ignore) { }
                if (f == null) continue;
                f.setAccessible(true);
                Class<?> vt = f.getType();
                Object v = null;
                if (!vt.isPrimitive() && vt.getName().equals(fi[1])) {
                    try {
                        java.lang.reflect.Constructor<?> c2 = vt.getDeclaredConstructor();
                        c2.setAccessible(true);
                        v = c2.newInstance();
                    } catch (Throwable ig) {
                        v = null;
                        System.out.println("M3-man: " + fi[0] + " 构造失败因 = " + ig);
                        Throwable cc = ig.getCause() != null ? ig.getCause() : ig;
                        System.out.println("M3-man: " + fi[0] + " 因 = "
                                + cc.getClass().getName() + ": " + cc.getMessage());
                        StackTraceElement[] st = cc.getStackTrace();
                        for (int si = 0; si < Math.min(4, st.length); si++)
                            System.out.println("  at " + st[si]);
                    }
                } else if (!vt.isPrimitive()) {
                    v = null;
                }
                f.set(act, v);
                System.out.println("M3-man: " + fi[0] + " = "
                        + (v != null ? v.getClass().getSimpleName() : "null"));
            } catch (Throwable t) {
                System.out.println("M3-man: " + fi[0] + " 失败 = " + t);
            }
        }
        /* mFragments（v16 已验证的 FragmentController 构造） */
        try {
            Class<?> fc = Class.forName("android.app.FragmentController");
            java.lang.reflect.Constructor<?> fcC = null;
            for (java.lang.reflect.Constructor<?> cc3 : fc.getDeclaredConstructors())
                if (cc3.getParameterCount() == 1) { fcC = cc3; break; }
            if (fcC != null) {
                fcC.setAccessible(true);
                Object host = null;
                try {
                    /* v23：FragmentHostCallback 是 abstract——用 bootstrap 内
                     * 预编的最小子类 MyHost（构造签名 act/ctx/handler/anim）*/
                    java.lang.reflect.Constructor<?> mhC =
                            MyHost.class.getConstructor(Activity.class, Context.class,
                                                        Handler.class, int.class);
                    host = mhC.newInstance(new Object[]{ (Activity) act, null,
                            new android.os.Handler(android.os.Looper.myLooper()),
                            Integer.valueOf(0) });
                    try { host.getClass().getMethod("bind", Activity.class).invoke(host, act);
                          System.out.println("M3-man: MyHost.bind(act) ✓"); } catch (Throwable b) { }
                } catch (Throwable hbFail) {
                    System.out.println("M3-man: MyHost = " + hbFail);
                }
                Object fctrl = fcC.newInstance(new Object[]{ host });
                java.lang.reflect.Field ff = null;
                for (Class<?> k = ac; ff == null && k != null; k = k.getSuperclass())
                    try { ff = k.getDeclaredField("mFragments"); } catch (Throwable ignore) { }
                if (ff != null) {
                    ff.setAccessible(true);
                    ff.set(act, fctrl);
                    System.out.println("M3-man: mFragments = FragmentController(host="
                            + (host != null) + ")");
                    /* v52 ★★★：attachHost——Activity.attach 欠的供血。
                     * 01:12 案：onCreate 的 dispatchCreate → moveToState:1626
                     * "No activity"——FragmentManagerImpl.mHost 为 null（真机由
                     * Activity.attach 的 mFragments.attachHost(null) 设上，我们
                     * 跳过了 attach）。FragmentController.attachHost(Fragment)
                     * → mHost.mFragmentManager.attachController(mHost, mHost, parent) */
                    try {
                        java.lang.reflect.Method ah = null;
                        for (java.lang.reflect.Method m2 : fctrl.getClass().getMethods())
                            if (m2.getName().equals("attachHost")) { ah = m2; break; }
                        if (ah != null) {
                            ah.invoke(fctrl, new Object[ah.getParameterCount()]);
                            sTrace.add("v52:attachHost ✓");
                            System.out.println("M3-man: v52 attachHost ✓（mHost 供血）");
                        } else {
                            sTrace.add("v52:attachHost 方法未找到");
                            System.out.println("M3-man: v52 attachHost ✗（无此方法）");
                        }
                    } catch (Throwable t52) {
                        sTrace.add("v52:attachHost 异常=" + t52);
                        System.out.println("M3-man: v52 attachHost 异常 = " + t52);
                    }
                }
            }
        } catch (Throwable fragFail) {
            System.out.println("M3-man: mFragments 失败 = " + fragFail);
        }
    }

    /* v44：JIT 对抗——热路径状态位 */
    static final java.util.List<String> sTrace = java.util.Collections.synchronizedList(new java.util.ArrayList<String>());
    static final ThreadLocal<String> sDebug = new ThreadLocal<String>() {
        @Override protected String initialValue() { return ""; }
    };

    /** v91：AM 实例 mIsEnabled 强写 true（短路 isEnabled 的 AIC 调用） */
    static void fixAmEnable(Object am) {
        if (am == null) return;
        try {
            Class<?> amC2 = am.getClass();
            java.lang.reflect.Field f = null;
            for (Class<?> c = amC2; c != null && c != Object.class; c = c.getSuperclass()) {
                try { f = c.getDeclaredField("mIsEnabled"); break; } catch (Throwable ig) { }
            }
            if (f != null) {
                f.setAccessible(true);
                /* boolean 非静态——直接反射写（static final 才需要 Unsafe） */
                f.setBoolean(am, true);
                System.out.println("M3-fw: v91 AM 实例已短路（mIsEnabled=true）");
            }
        } catch (Throwable t) {
            System.out.println("M3-fw: v91 fixAmEnable 失败 = " + t);
        }
    }

    /** v48 ★极简重写：直线代码零嵌套——verify:none+JIT 环境的行为可靠性优先 */
    static Object deepStub(Class<?> t) {
        sTrace.add("ds:" + (t == null ? "null" : t.getName()));
        if (t == null || t == void.class) return null;
        if (t == boolean.class) return Boolean.FALSE;
        if (t == int.class) return Integer.valueOf(0);
        if (t == long.class) return Long.valueOf(0);
        if (t == String.class) return "stub";
        if (t.isInterface()) {
            sTrace.add("B1:iface:" + t.getName());
            return java.lang.reflect.Proxy.newProxyInstance(
                    Thread.currentThread().getContextClassLoader(),
                    new Class<?>[]{ t },
                    new java.lang.reflect.InvocationHandler() {
                        @Override public Object invoke(Object p, java.lang.reflect.Method m, Object[] a) {
                            if (m.getName().equals("toString")) return "TVM";
                            if (m.getName().equals("hashCode")) return Integer.valueOf(0);
                            if (m.getName().equals("equals")) return Boolean.FALSE;
                            return deepStub(m.getReturnType());
                        }
                    });
        }
        /* 类：Unsafe 直造 + 字段续命（一层）——全部直线，无嵌套 try */
        sTrace.add("B2:class:" + t.getName());
        try {
            Class<?> uc = Class.forName("sun.misc.Unsafe");
            java.lang.reflect.Field uf = null;
            for (java.lang.reflect.Field f2 : uc.getDeclaredFields())
                if (f2.getType() == uc) { uf = f2; break; }
            if (uf == null) { sTrace.add("B2:无theUnsafe"); return null; }
            uf.setAccessible(true);
            Object U = uf.get(null);
            Object o = uc.getMethod("allocateInstance", Class.class).invoke(U, t);
            sTrace.add("B3:alloc=" + (o != null));
            if (o != null) {
                for (java.lang.reflect.Field vf : t.getDeclaredFields()) {
                    int vm = vf.getModifiers();
                    if (java.lang.reflect.Modifier.isStatic(vm)
                            || java.lang.reflect.Modifier.isFinal(vm)) continue;
                    Class<?> vt = vf.getType();
                    if (vt.isPrimitive() || vt == String.class) continue;
                    sTrace.add("B4:字段 " + vf.getName() + ":" + vt.getSimpleName());
                    try {
                        vf.setAccessible(true);
                        Object sub = null;
                        if (vt.isInterface()) {
                            sub = java.lang.reflect.Proxy.newProxyInstance(
                                    Thread.currentThread().getContextClassLoader(),
                                    new Class<?>[]{ vt },
                                    new java.lang.reflect.InvocationHandler() {
                                        @Override public Object invoke(Object p2, java.lang.reflect.Method m2, Object[] a2) {
                                            if (m2.getName().equals("toString")) return "TVM";
                                            return null;
                                        }
                                    });
                        } else {
                            /* v49 ★：壳字段（_value:Object 型）——用宿主方法的
                             * 返回类型推断真身（Service.getServiceWithMetadata→
                             * ServiceWithMetadata）。挑【非 java.lang 的对象返回类型】。
                             */
                            Class<?> want = vt;
                            if (vt == Object.class) {
                                /* v50：壳字段推断——跳过接口（v49 案：
                                 * 推断成 IBinder 接口→allocateInstance 炸），
                                 * 只要【具体类】（ServiceWithMetadata 会中） */
                                for (java.lang.reflect.Method hm : t.getDeclaredMethods()) {
                                    Class<?> hrt = hm.getReturnType();
                                    if (hrt == Object.class || hrt.isPrimitive()) continue;
                                    if (hrt.getName().startsWith("java.lang")) continue;
                                    if (hrt == t) continue;
                                    if (hrt.isInterface()) continue;   /* v50 ★ */
                                    if (hrt.isArray()) continue;
                                    want = hrt;
                                    sTrace.add("B4:推断 " + vf.getName() + "→" + hrt.getName());
                                    break;
                                }
                            }
                            sub = uc.getMethod("allocateInstance", Class.class).invoke(U, want);
                        }
                        if (sub != null) vf.set(o, sub);
                    } catch (Throwable f48) {
                        sTrace.add("B4:字段失败 " + vf.getName() + ":" + f48);
                    }
                }
                sTrace.add("B5:完成:" + t.getName());
            }
            return o;
        } catch (Throwable t48) {
            sTrace.add("B6:整段抛=" + t48);
            return null;
        }
    }

    static void attachActivity(Object act) throws Exception {
        Class<?> acCls = act.getClass();
        /* 1. systemMain（一次性） */
        if (sTheActivityThread == null) {
            try {
                Class<?> at = Class.forName("android.app.ActivityThread");
                java.lang.reflect.Method sm = null;
                for (java.lang.reflect.Method mm : at.getDeclaredMethods())
                    if (mm.getName().equals("systemMain")
                            && mm.getParameterCount() == 0) { sm = mm; break; }
                if (sm == null) throw new NoSuchMethodException("systemMain()");
                sm.setAccessible(true);
                sTheActivityThread = sm.invoke(null, new Object[0]);
                System.out.println("M3-at: systemMain() OK = " + sTheActivityThread);
            } catch (Throwable smFail) {
                System.out.println("M3-at: systemMain 异常 = " + smFail);
                Throwable cc = smFail.getCause() != null ? smFail.getCause() : smFail;
                System.out.println("M3-at-因: " + cc.getClass().getName() + ": " + cc.getMessage());
                StackTraceElement[] ss = cc.getStackTrace();
                for (int i = 0; i < Math.min(4, ss.length); i++)
                    System.out.println("  at " + ss[i]);
            }
            if (sTheActivityThread == null) {
                /* 保底：currentActivityThread()（systemMain 部分成功时） */
                try {
                    Class<?> at = Class.forName("android.app.ActivityThread");
                    java.lang.reflect.Method ct = at.getMethod("currentActivityThread");
                    sTheActivityThread = ct.invoke(null);
                    System.out.println("M3-at: currentActivityThread() = " + sTheActivityThread);
                } catch (Throwable t) { }
            }
        }
        /* 2. systemContext（ActivityThread 上的 getSystemContext） */
        if (sSystemContext == null && sTheActivityThread != null) {
            try {
                java.lang.reflect.Method gs = sTheActivityThread.getClass()
                        .getMethod("getSystemContext");
                sSystemContext = gs.invoke(sTheActivityThread);
                System.out.println("M3-at: getSystemContext() = " + sSystemContext);
            } catch (Throwable gsFail) {
                System.out.println("M3-at: getSystemContext 异常 = " + gsFail);
            }
        }
        /* 3. 找 attach（≥8 参实例方法） */
        java.lang.reflect.Method m = null;
        for (Class<?> k = acCls; m == null && k != null; k = k.getSuperclass())
            for (java.lang.reflect.Method mm : k.getDeclaredMethods())
                if (mm.getName().equals("attach")
                        && !java.lang.reflect.Modifier.isStatic(mm.getModifiers())
                        && mm.getParameterCount() >= 8)
                    { m = mm; break; }
        if (m == null) throw new NoSuchMethodException("attach 未找到（≥8参）");
        System.out.println("M3-attach: 签名 " + m.getParameterCount() + " 参");
        m.setAccessible(true);
        /* 4. 参数组装：Context 用【真 systemContext】（没有再 MiniContext）；
         * ActivityThread 用【真单例】 */
        Class<?>[] pts = m.getParameterTypes();
        Object[] args = new Object[pts.length];
        for (int i = 0; i < pts.length; i++) {
            Class<?> p = pts[i];
            String pn = p.getName();
            if (pn.equals("android.content.Context")) {
                args[i] = (sSystemContext != null) ? sSystemContext : newMiniContext();
            } else if (pn.equals("android.app.ActivityThread")) {
                args[i] = sTheActivityThread;   /* ★ 真单例（手工 new 的没了） */
            } else if (!p.isPrimitive()) {
                try {
                    java.lang.reflect.Constructor<?> pc = p.getDeclaredConstructor();
                    pc.setAccessible(true);
                    args[i] = pc.newInstance();
                } catch (Throwable noCtor) {
                    args[i] = null;
                }
            } else if (p == boolean.class) {
                args[i] = Boolean.FALSE;
            } else if (p == int.class) {
                args[i] = Integer.valueOf(0);
            } else if (p == long.class) {
                args[i] = Long.valueOf(0);
            } else if (p == char.class) {
                args[i] = Character.valueOf('\0');
            } else {
                args[i] = null;
            }
        }
        m.invoke(act, args);
    }

    static Object newMiniContext() throws Exception {
        return Class.forName("com.twinvm.m3.MiniContext")
                .getDeclaredConstructor().newInstance();
    }

    static Object newContextProxy(Class<?> ctxCls) throws Exception {
        Class<?> cw = Class.forName("android.content.ContextWrapper");
        /* 通过子类化字节码？无 javassist——用 java.lang.reflect.Proxy 不行。
         * 方案：直接实例化 ContextWrapper（构造 null base），高频方法
         * 我们在 Bootstrap 里无法覆写……换个思路：
         * Context 的【抽象方法】有限（getAssets/getResources/getPackageManager/
         * getSystemService/getApplicationContext/getTheme/…）——
         * 用【运行时子类】不可行，那就【挑最朴素的可行件】：
         *   android.content.ContextWrapper 本身 new（base=null）——
         *   attach 存引用 OK；后续调法炸时三帧指路（逐案覆写）。 */
        java.lang.reflect.Constructor<?> c = cw.getDeclaredConstructor(
                new Class[]{ Class.forName("android.content.Context") });
        c.setAccessible(true);
        return c.newInstance(new Object[]{ null });
    }

    static void call(Object target, String name, Class<?>[] types, Object[] vals) throws Exception {
        Method m = find(target.getClass(), name, types);
        if (m == null) {
            System.out.println("M3-Bootstrap: " + name + " 未找到（跳过）");
            return;
        }
        m.setAccessible(true);
        m.invoke(target, vals);
    }

    static Method find(Class<?> c, String name, Class<?>[] types) {
        for (Class<?> k = c; k != null; k = k.getSuperclass()) {
            try { return k.getDeclaredMethod(name, types == null ? new Class[0] : types); }
            catch (NoSuchMethodException ignore) { }
        }
        return null;
    }
}
