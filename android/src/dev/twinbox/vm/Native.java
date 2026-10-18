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
}
