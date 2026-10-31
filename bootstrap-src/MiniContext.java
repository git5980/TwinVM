package com.twinvm.m3;

import android.content.Context;
import android.content.ContextWrapper;

/** v13 ★：最小 Context——覆写 attach 链高频方法（base=null 转调全拦）。
 *  每个新炸点 = 一个新覆写（Java 层桥，比 native 桥好维护）。 */
public class MiniContext extends ContextWrapper {
    public MiniContext() { super(null); }

    /* API36 覆写（占位接口描述符与真 framework 一致——运行时互认） */
    public void setAutofillClient(android.view.autofill.AutofillManager.AutofillClient client) { }
    public android.view.autofill.AutofillManager.AutofillClient getAutofillClient() { return null; }

    /* AutofillClient 在 API 34 位于 android.view.autofill.AutofillManager 内部吗——
     * 用字符串签名不可行（覆写需编译期类型）。改为【反射排除】：不覆写，
     * 由 attach 参数组装处跳过（MiniContext 装载后若无此方法，
     * attach 内部调用走 ContextWrapper.mBase(null) 仍会炸——
     * 因此这里改为【反射查方法存在才覆写】不可行。
     * 务实：API34 的 Context.setAutofillClient 参数类型就是
     * AutofillManager.AutofillClient——报错说明 API 34 无此类。
     * 查：AutofillClient 是 API 35+ 顶层？先去掉，用反射处理。 */    @Override public Context getApplicationContext() { return this; }
    /* v94 ★（当前阶段终版）：PhoneWindow 构造链需要的服务
     * ——AOSP PhoneWindow:399-406 逐行核实：
     *   mLayoutInflater = LayoutInflater.from(context) → LAYOUT_INFLATER
     *     （from 返 null 直接 AssertionError——必须给真）
     *   mWindowManager = getSystemService(WINDOW_SERVICE)（只存字段）
     *   Settings.Global.getInt(getContentResolver()...) → 非空壳 */
    @Override public Object getSystemService(String name) {
        if ("layout_inflater".equals(name)) {
            try {
                return Class.forName("com.android.internal.policy.PhoneLayoutInflater")
                        .getConstructor(Context.class).newInstance(this);
            } catch (Throwable t) {
                System.out.println("M3-ctx: PhoneLayoutInflater 失败 = " + t);
                return null;
            }
        }
        if ("window".equals(name)) {
            try {
                Class<?> wmC = Class.forName("android.view.WindowManager");
                return java.lang.reflect.Proxy.newProxyInstance(
                        wmC.getClassLoader(), new Class<?>[]{ wmC },
                        new java.lang.reflect.InvocationHandler() {
                            @Override public Object invoke(Object p, java.lang.reflect.Method m, Object[] a) {
                                String n = m.getName();
                                Class<?> rt = m.getReturnType();
                                if (rt == boolean.class) return Boolean.FALSE;
                                if (rt == int.class) return Integer.valueOf(0);
                                if (rt == long.class) return Long.valueOf(0);
                                return null;
                            }
                        });
            } catch (Throwable t) { return null; }
        }
        return null;
    }
    /* v94 修正：API34 ContentResolver 无抽象方法（全 final 化——javap 实证），
     * 子类化不成立 → Unsafe 壳 + 关键字段直填（mContext/mPackageName/mUserId=0/
     * mTargetSdkVersion）。Settings.Global.getInt 链：NameValueCache →
     * ATM 单例（v89 预填 Proxy）→ IContentProvider Proxy（deepStub）→
     * query 返 Proxy Cursor → moveToNext=false → getString 返 null →
     * getInt 返 default ✓ 全程不碰 binder.transact/Parcel（bridge v14b 撤销区） */
    private android.content.ContentResolver mResolver;
    @Override public android.content.ContentResolver getContentResolver() {
        if (mResolver == null) {
            try {
                Class<?> uc = Class.forName("sun.misc.Unsafe");
                java.lang.reflect.Field uf = null;
                for (java.lang.reflect.Field f : uc.getDeclaredFields())
                    if (f.getType() == uc) { uf = f; break; }
                uf.setAccessible(true);
                Object U = uf.get(null);
                Object r = uc.getMethod("allocateInstance", Class.class).invoke(U,
                        Class.forName("android.content.ContentResolver"));
                java.lang.reflect.Method off = uc.getMethod("objectFieldOffset", java.lang.reflect.Field.class);
                java.lang.reflect.Method po = uc.getMethod("putObject", Object.class, long.class, Object.class);
                java.lang.reflect.Method pi = uc.getMethod("putInt", Object.class, long.class, int.class);
                Class<?> crC = Class.forName("android.content.ContentResolver");
                try { po.invoke(U, r, off.invoke(U, gf(crC, "mContext")), this); } catch (Throwable ig) { }
                try { po.invoke(U, r, off.invoke(U, gf(crC, "mPackageName")), "com.twinvm.demo"); } catch (Throwable ig) { }
                try { pi.invoke(U, r, off.invoke(U, gf(crC, "mUserId")), 0); } catch (Throwable ig) { }
                try { pi.invoke(U, r, off.invoke(U, gf(crC, "mTargetSdkVersion")), 36); } catch (Throwable ig) { }
                mResolver = (android.content.ContentResolver) r;
            } catch (Throwable t) {
                System.out.println("M3-ctx: v94 resolver 壳失败 = " + t);
            }
        }
        return mResolver;
    }
    /* v28 ★：onResume 的 ContextWrapper:979 炸点——
     * getSystemServiceName(Clazz)——直接返回 null（无服务） */
    @Override public String getSystemServiceName(java.lang.Class<?> serviceClass) { return null; }
    /* v58 ★：View.<init>:5929 → ViewConfiguration.getDisplayDensity:1481 →
     * getResources().getDisplayMetrics() on null（setContentView 的 UI 链）。
     * 手工构造真 Resources：AssetManager(公有构造) + DisplayMetrics + Configuration */
    private android.content.res.Resources mRes;
    @Override public android.content.res.Resources getResources() {
        if (mRes != null) return mRes;
        /* v76 ★★ 极简终局：方法体只此一路（旧 100 行链全删——JIT 无从抽风） */
        try {
            Class<?> uc = Class.forName("sun.misc.Unsafe");
            java.lang.reflect.Field uf = null;
            for (java.lang.reflect.Field f : uc.getDeclaredFields())
                if (f.getType() == uc) { uf = f; break; }
            uf.setAccessible(true);
            Object U = uf.get(null);
            mRes = (android.content.res.Resources)
                    uc.getMethod("allocateInstance", Class.class).invoke(U, RuntimeResources.class);
            /* v83 ★★ 主题链根修（20:48 TA 指令：找资料一次性实现——
             * Robolectric Shadow 思路：真 framework 类 + Unsafe 填字段 + bridge native 桩）
             * 链：ContextThemeWrapper.getTheme:210 → Resources.newTheme:2195 [final]
             *   → mResourcesImpl.newThemeImpl():1412 → new ThemeImpl():1441
             *   → mAssets.createTheme() → nativeThemeCreate [bridge v13]
             *   → sThemeRegistry.registerNativeAllocation [真 libcore + freeFunc 桩] */
            try {
                java.lang.reflect.Method al = uc.getMethod("allocateInstance", Class.class);
                java.lang.reflect.Method off = uc.getMethod("objectFieldOffset", java.lang.reflect.Field.class);
                java.lang.reflect.Method putObj = uc.getMethod("putObject", Object.class, long.class, Object.class);
                java.lang.reflect.Method putLong = uc.getMethod("putLong", Object.class, long.class, long.class);
                /* AssetManager 壳：v84 通用壳工厂（mOpenRefs[AtomicInteger] 等
                 * 全字段自动填）→ 再覆盖 mObject=2（nativeTheme* 走 bridge 桩不 deref） */
                Class<?> amC = Class.forName("android.content.res.AssetManager");
                Object am = al.invoke(U, amC);
                fillShell(U, am);
                putLong.invoke(U, am, off.invoke(U, gf(amC, "mObject")), 2L);
                /* ResourcesImpl 壳：fillShell 全填 + 手动接 am */
                Class<?> implC = Class.forName("android.content.res.ResourcesImpl");
                Object impl = al.invoke(U, implC);
                fillShell(U, impl);
                putObj.invoke(U, impl, off.invoke(U, gf(implC, "mAssets")), am);
                /* Resources.mResourcesImpl = 壳；mClassLoader 也补 */
                putObj.invoke(U, mRes, off.invoke(U, gf(android.content.res.Resources.class, "mResourcesImpl")), impl);
                putObj.invoke(U, mRes, off.invoke(U, gf(android.content.res.Resources.class, "mClassLoader")),
                        ClassLoader.getSystemClassLoader());
                /* v85：RuntimeResources 自己也过工厂——
                 * Resources.mThemeRefs[ArrayList]（newTheme:2196 synchronized 对象）
                 * 等 Resources 层字段全填——v84 只造了 impl/am 两层壳 */
                fillShell(U, mRes);
                System.out.println("M3-ctx: v87 资源链 ✓（接口字段动态代理）");
            } catch (Throwable t) {
                System.out.println("M3-ctx: v83 资源链失败 = " + t);
            }
        } catch (Throwable t) {
            System.out.println("M3-ctx: v76 构造失败 = " + t);
        }
        return mRes;
    }
    @Override public android.content.res.AssetManager getAssets() {
        try { return getResources() != null ? getResources().getAssets() : null; }
        catch (Throwable ignore) { return null; }
    }
    /* v78：AccessibilityManager:537 → checkCallingOrSelfPermission
     * AOSP 语义：无权限 = PERMISSION_DENIED = -1（走"无服务"分支） */
    /* v80：AccessibilityManager:545 else 分支——getUserId()
     * （v79 裁决：覆写已接住 DENIED → 走 else → getUserId on null）
     * AOSP ContextWrapper:1177：return mBase.getUserId() */
    public int getUserId() { return 0; }   /* API36 方法——编译34 去 Override，运行时虚表覆写 */
    /* v83：Unsafe 字段偏移辅助（局部方法 Java8 不支持——提为 static） */
    static java.lang.reflect.Field gf(Class<?> dc, String n) throws Exception {
        java.lang.reflect.Field f = dc.getDeclaredField(n); f.setAccessible(true); return f;
    }

    /* v84 ★★ 通用壳工厂（22:07 案：AssetManager.mOpenRefs[AtomicInteger] on null
     * ——逐字段补不胜补。反射枚举全部实例字段按类型自动填默认值，
     * 此后 ResourcesImpl/PhoneWindow/一切壳类共用） */
    static void fillShell(Object U, Object o) {
        try {
            Class<?> uc = Class.forName("sun.misc.Unsafe");
            java.lang.reflect.Method al = uc.getMethod("allocateInstance", Class.class);
            java.lang.reflect.Method off = uc.getMethod("objectFieldOffset", java.lang.reflect.Field.class);
            java.lang.reflect.Method putObj = uc.getMethod("putObject", Object.class, long.class, Object.class);
            java.lang.reflect.Method putInt = uc.getMethod("putInt", Object.class, long.class, int.class);
            java.lang.reflect.Method putLong = uc.getMethod("putLong", Object.class, long.class, long.class);
            java.lang.reflect.Method putByte = uc.getMethod("putByte", Object.class, long.class, byte.class);
            for (Class<?> c = o.getClass(); c != null && c != Object.class; c = c.getSuperclass()) {
                for (java.lang.reflect.Field f : c.getDeclaredFields()) {
                    if (java.lang.reflect.Modifier.isStatic(f.getModifiers())) continue;
                    f.setAccessible(true);
                    long fo = (Long) off.invoke(U, f);
                    Class<?> t = f.getType();
                    if (t == java.util.concurrent.atomic.AtomicInteger.class)
                        putObj.invoke(U, o, fo, new java.util.concurrent.atomic.AtomicInteger(1));
                    else if (t == java.util.concurrent.atomic.AtomicLong.class)
                        putObj.invoke(U, o, fo, new java.util.concurrent.atomic.AtomicLong(1));
                    else if (t == java.util.concurrent.atomic.AtomicBoolean.class)
                        putObj.invoke(U, o, fo, new java.util.concurrent.atomic.AtomicBoolean(true));
                    else if (t == String.class)
                        putObj.invoke(U, o, fo, "");
                    else if (t.isArray())
                        putObj.invoke(U, o, fo, java.lang.reflect.Array.newInstance(t.getComponentType(), 0));
                    else if (t == long.class)
                        putLong.invoke(U, o, fo, 1L);
                    else if (t == int.class || t == short.class || t == char.class || t == float.class)
                        putInt.invoke(U, o, fo, 1);   /* mNumRefs=1 / density 兜底 */
                    else if (t == boolean.class || t == byte.class)
                        putByte.invoke(U, o, fo, (byte) 1);   /* mOpen=true */
                    else if (t.isInterface()) {
                        /* v87 ★：接口字段 → 直通动态代理（23:15 案：
                         * ColorOS IResourcesImplExt.replaceTypedArray on null——
                         * 厂商扩展接口无构造器，Proxy 实现：
                         * replaceXxx(单参)→原样返回（跳过换肤=合法降级，
                         * 换肤框架架构佐证：资源拦截点直通即不换肤）
                         * 基本类型→0/false，其余→null）
                         * 方法名/签名【零猜测】——Proxy 拦截时从运行时
                         * Method 对象取，不用提前知道厂商接口长啥样 */
                        Object v = java.lang.reflect.Proxy.newProxyInstance(
                                t.getClassLoader(), new Class<?>[]{ t },
                                new java.lang.reflect.InvocationHandler() {
                                    @Override public Object invoke(Object proxy, java.lang.reflect.Method m, Object[] a) {
                                        String n = m.getName();
                                        Class<?> rt = m.getReturnType();
                                        if (n.startsWith("replace") && a != null && a.length == 1
                                                && rt.isInstance(a[0])) return a[0];
                                        if (rt == boolean.class) return Boolean.FALSE;
                                        if (rt == int.class) return Integer.valueOf(0);
                                        if (rt == long.class) return Long.valueOf(0);
                                        return null;
                                    }
                                });
                        putObj.invoke(U, o, fo, v);
                    }
                    else {
                        /* 引用类型：无参构造优先；失败重试 (int) 构造
                         * （v86：Pools$SynchronizedPool 只有 SynchronizedPool(int)——
                         * 22:30 案 TypedArray.obtain:55 池 on null 就是它） */
                        try {
                            Object v = t.getDeclaredConstructor().newInstance();
                            putObj.invoke(U, o, fo, v);
                        } catch (Throwable first) {
                            try {
                                Object v = t.getDeclaredConstructor(int.class).newInstance(5);   /* AOSP :156 原文 5 */
                                putObj.invoke(U, o, fo, v);
                            } catch (Throwable ignore) { /* 两种都炸——留 null */ }
                        }
                    }
                }
            }
        } catch (Throwable t) {
            System.out.println("M3-ctx: fillShell = " + t);
        }
    }

    /* v83：getTheme → null（AOSP ContextThemeWrapper.initializeTheme:211-213
     * 的 if (theme != null) 天然跳过 setTo——v82 空壳退役；Activity 自己的
     * mTheme 由 :210 newTheme() 真链供给（v83 impl 壳） */
    @Override public android.content.res.Resources.Theme getTheme() { return null; }

    /* v81：AccessibilityManager:572 → Handler(context.getMainLooper())
     * （v58 旧链曾覆写、v76 极简化时丢失——补回） */
    @Override public android.os.Looper getMainLooper() { return android.os.Looper.myLooper(); }

    @Override public int checkCallingOrSelfPermission(String permission) {
        System.out.println("M3-ctx: v79 checkCallingOrSelfPermission 接住 ✓（DENIED）");
        return -1;
    }
    @Override public int checkSelfPermission(String permission) { return -1; }

    /* v55 ★：onCreate 案——ContextWrapper.mBase(null) 的连锁补断 */
    @Override public android.content.pm.ApplicationInfo getApplicationInfo() {
        try {
            Class<?> t = android.content.pm.ApplicationInfo.class;
            Class<?> uc = Class.forName("sun.misc.Unsafe");
            java.lang.reflect.Field uf = null;
            for (java.lang.reflect.Field f : uc.getDeclaredFields())
                if (f.getType() == uc) { uf = f; break; }
            if (uf != null) {
                uf.setAccessible(true);
                Object U = uf.get(null);
                return (android.content.pm.ApplicationInfo)
                        uc.getMethod("allocateInstance", Class.class).invoke(U, t);
            }
        } catch (Throwable ignore) { }
        return null;
    }


    @Override public String getPackageName() { return "com.twinvm.demo"; }
    @Override public String getOpPackageName() { return "com.twinvm.demo"; }
    @Override public int checkPermission(String perm, int pid, int uid) { return -1; }   /* v78: 语义统一 DENIED */
    @Override public android.content.pm.PackageManager getPackageManager() { return null; }
    @Override public java.io.File getFilesDir() { return new java.io.File("/data/local/tmp"); }
    @Override public ClassLoader getClassLoader() { return MiniContext.class.getClassLoader(); }
    @Override public Context createConfigurationContext(android.content.res.Configuration c) { return this; }
    @Override public Context createDisplayContext(android.view.Display d) { return this; }
    @Override public Context createPackageContext(String pkg, int flags) { return this; }
    @Override public android.content.Intent registerReceiver(android.content.BroadcastReceiver r, android.content.IntentFilter f) { return null; }
    @Override public void unregisterReceiver(android.content.BroadcastReceiver r) { }
    @Override public void startActivity(android.content.Intent i) { }
    @Override public void startActivity(android.content.Intent i, android.os.Bundle b) { }
    @Override public android.content.SharedPreferences getSharedPreferences(String n, int m) {
        return new android.content.SharedPreferences() {
            @Override public java.util.Map<String, ?> getAll() { return new java.util.HashMap<String, Object>(); }
            @Override public String getString(String k, String d) { return d; }
            @Override public java.util.Set<String> getStringSet(String k, java.util.Set<String> d) { return d; }
            @Override public int getInt(String k, int d) { return d; }
            @Override public long getLong(String k, long d) { return d; }
            @Override public float getFloat(String k, float d) { return d; }
            @Override public boolean getBoolean(String k, boolean d) { return d; }
            @Override public boolean contains(String k) { return false; }
            @Override public android.content.SharedPreferences.Editor edit() { return new android.content.SharedPreferences.Editor() {
                @Override public android.content.SharedPreferences.Editor putString(String k, String v) { return this; }
                @Override public android.content.SharedPreferences.Editor putStringSet(String k, java.util.Set<String> v) { return this; }
                @Override public android.content.SharedPreferences.Editor putInt(String k, int v) { return this; }
                @Override public android.content.SharedPreferences.Editor putLong(String k, long v) { return this; }
                @Override public android.content.SharedPreferences.Editor putFloat(String k, float v) { return this; }
                @Override public android.content.SharedPreferences.Editor putBoolean(String k, boolean v) { return this; }
                @Override public android.content.SharedPreferences.Editor remove(String k) { return this; }
                @Override public android.content.SharedPreferences.Editor clear() { return this; }
                @Override public void apply() { }
                @Override public boolean commit() { return true; }
            }; }
            @Override public void registerOnSharedPreferenceChangeListener(android.content.SharedPreferences.OnSharedPreferenceChangeListener l) { }
            @Override public void unregisterOnSharedPreferenceChangeListener(android.content.SharedPreferences.OnSharedPreferenceChangeListener l) { }
        };
    }
}

    /* v75 ★：代理 Resources——字段读链在 guest 不可靠（isLayoutSizeAtLeast
     * 报 null 但 impl.cfg 非空——读取路径错位）。覆写读方法直供值。 */

class RuntimeResources extends android.content.res.Resources {
        private android.content.res.Configuration mCfg;
        private android.util.DisplayMetrics mDm;
        RuntimeResources() {
        super(null, null, null);   /* 编译需要——运行时 Unsafe 跳过（走不到雷区） */
    }
        @Override public android.content.res.Configuration getConfiguration() {
            /* v77：Unsafe 跳构造器→实例字段 null——懒初始化 */
            if (mCfg == null) {
                mCfg = new android.content.res.Configuration();
                mCfg.screenLayout = android.content.res.Configuration.SCREENLAYOUT_SIZE_XLARGE
                        | android.content.res.Configuration.SCREENLAYOUT_LONG_YES;
            }
            return mCfg;
        }
        @Override public android.util.DisplayMetrics getDisplayMetrics() {
            if (mDm == null) mDm = new android.util.DisplayMetrics();   /* v77 懒初始化 */
            mDm.setToDefaults();
            return mDm;
        }
        @Override public int getDimensionPixelSize(int id) { return 8; }
        @Override public int getInteger(int id) { return 1; }
        @Override public boolean getBoolean(int id) { return false; }
        @Override public int getColor(int id) { return 0xFF888888; }
        @Override public String getString(int id) { return ""; }
        @Override public float getDimension(int id) { return 8f; }
        @Override public void getValue(int id, android.util.TypedValue v, boolean r) {
            v.type = 0x10; v.data = 8; v.assetCookie = 1; v.density = 0;
        }
    }
