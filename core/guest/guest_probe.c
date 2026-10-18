/* TwinVM /bin/probe —— execve 之后"我看到的世界"诊断（v0.7.38）
 *
 * 为什么有它：真机 ART 报
 *   file_utils.cc:609 Check failed: !location.empty()
 *   BOOTCLASSPATH and DEX2OATBOOTCLASSPATH must not be empty
 * 反汇编已确定这句 CHECK 来自 ParsedOptions::DoParse 里
 *   GetDefaultBootImageLocation(GetAndroidRoot(), ...) —— 也就是
 *   **android_root 字符串是空的**（ANDROID_ROOT 环境变量没被 ART 看到，
 *   或者 root 目录推算失败）。而 guest 明明把 envp 传了，
 *   引擎侧（读 guest argv/envp → vm_loader 造新栈）本地单元测试也全绿。
 * 唯一没验证过的就是"在真机 VM 里，动态映像（linker64 接管的那种）到底看不看得见环境"。
 *
 * 本探针是**动态 PIE**（和自己的 /system/bin/dalvikvm 同款装载路径），做四件事：
 *   1) 打印 argc/argv；
 *   2) 打印完整 environ（重点 BOOTCLASSPATH / DEX2OATBOOTCLASSPATH / ANDROID_ROOT / ANDROID_DATA）；
 *   3) 逐个 open() ART 要用的 ROM 路径，打印拿到的 fd（≥10000 就是 ROM 虚拟 fd）；
 *   4) 用**裸 svc 221**（不是 execveat，引擎只实现 execve）继续 exec dalvikvm，
 *      argv/env 原样透传 —— 引导流程不受影响。
 */
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

extern char** environ;

static void w(const char* s) {
    size_t n = strlen(s);
    while (n) {
        ssize_t k = write(1, s, n);
        if (k <= 0) break;
        s += k; n -= (size_t)k;
    }
}
static void wn(long v) {
    char t[24]; int i = 0; int neg = v < 0;
    unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
    if (!u) { w("0"); return; }
    while (u && i < 23) { t[i++] = (char)('0' + u % 10); u /= 10; }
    if (neg) w("-");
    while (i) { char c = t[--i]; write(1, &c, 1); }
}
static void wl(const char* s) { w(s); w("\n"); }

#if defined(__aarch64__)
static long raw_execve(const char* p, char* const a[], char* const e[]) {
    register long x8 __asm__("x8") = 221;              /* arm64 execve */
    register long x0 __asm__("x0") = (long)p;
    register long x1 __asm__("x1") = (long)a;
    register long x2 __asm__("x2") = (long)e;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
    return x0;
}
#else
static long raw_execve(const char* p, char* const a[], char* const e[]) {
    (void)p; (void)a; (void)e; return -38;
}
#endif

static const char* kPaths[] = {
    "/apex/com.android.art/javalib/core-oj.jar",
    "/apex/com.android.art/javalib/core-libart.jar",
    "/apex/com.android.art/javalib/boot.art",
    "/apex/com.android.art/javalib/boot.oat",
    "/apex/com.android.art/javalib/boot.vdex",
    "/apex/com.android.art/etc/classpaths/bootclasspath.pb",
    "/system/framework/arm64/boot.art",
    "/system/framework/arm64/boot.oat",
    "/system/framework/arm64/boot.vdex",
    "/system/lib64/libart.so",
    "/system/bin/dalvikvm",
    NULL
};

int main(int argc, char** argv, char** envp) {
    (void)envp;
    wl("== [probe] execve 之后我看到的世界 ==");
    w("[probe] argc="); wn(argc); w(" argv:");
    for (int i = 0; i < argc; i++) { w(" ["); w(argv[i] ? argv[i] : "(null)"); w("]"); }
    wl("");
    int n = 0;
    const char* keys[4] = { "BOOTCLASSPATH", "DEX2OATBOOTCLASSPATH", "ANDROID_ROOT", "ANDROID_DATA" };
    w("[probe] environ 条数=");
    if (environ) { while (environ[n]) n++; }
    wn(n); wl("");
    for (int i = 0; environ && environ[i]; i++) {
        for (int k = 0; k < 4; k++) {
            size_t l = strlen(keys[k]);
            if (strncmp(environ[i], keys[k], l) == 0 && environ[i][l] == '=') {
                w("[probe]   "); wl(environ[i][0] ? environ[i] : "");
            }
        }
    }
    w("[probe] 关键路径 open 结果（fd≥10000 = ROM 虚拟 fd）：");
    wl("");
    for (int i = 0; kPaths[i]; i++) {
        int fd = open(kPaths[i], O_RDONLY);
        w("[probe]   "); w(kPaths[i]); w(" -> fd="); wn(fd);
        if (fd >= 0) close(fd);
        wl("");
    }
    wl("[probe] 继续 exec /system/bin/dalvikvm（argv/env 原样透传）...");
    long r = raw_execve("/system/bin/dalvikvm", argv, environ);
    w("[probe] !! raw execve 返回 ");
    wn(r); wl("（失败）");
    return 99;
}
