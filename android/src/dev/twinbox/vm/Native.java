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

    /** v1.2.101 M1 并发：独立进程跑容器 B（与 A 并发，结果见 twinvm-live-b.log）。 */
    public static native String stage0Parallel(String romPath, String liveLog);
}
