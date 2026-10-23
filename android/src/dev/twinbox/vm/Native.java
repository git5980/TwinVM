package dev.twinbox.vm;

/** TwinVM 原生引擎桥（libtwinvm.so）。 */
public final class Native {
    static {
        System.loadLibrary("twinvm");
    }

    private Native() {}

    /** 完整跑一遍 Stage 0 用户态虚拟机，返回控制台输出全文。 */
    public static native String stage0(String romPath, String liveLog);

    /** 引擎能力探测（seccomp 可用性等）。 */
    public static native String probe();

    /** Stage 5-M1：选择容器要跑的 dex（ROM 内绝对路径，如 /rom/world.dex）。 */
    public static native void setDex(String dexPath);

    /** v1.2.108 M2：指定 APK/dex 的入口主类（如 com.twinvm.sample.Main）。 */
    public static native void setMainClass(String mainClass);

    /** v1.2.111 M2：运行期安装 APK 进容器 ROM（返回安装后路径，null=失败）。 */
    public static native String installApk(String apkPath, String romPath);

    /** v1.2.111：ROM 全量 CRC 验证（后台自愈）。 */
    public static native boolean verifyRom(String romPath);

    /** v1.2.113 M2：列出容器已装 APK（/data/app/ 下，返回 ROM 内路径）。 */
    public static native String[] listInstalledApks(String romPath);

    /** v1.2.113 M2：从 ROM 提取文件到宿主（dex 扫描用）。 */
    public static native boolean extractFile(String romPath, String innerPath, String outPath);

    /** v1.2.101 M1 并发：独立进程跑容器 B（与 A 并发，结果见 twinvm-live-b.log）。 */
    public static native String stage0Parallel(String romPath, String liveLog);
}
