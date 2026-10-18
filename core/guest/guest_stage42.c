/* TwinVM Stage 4.2 guest —— 真 linker 接管 + dalvikvm 引导。
 * VM 侧（vm_backend_sigsys execve → vm_loader_exec_dyn）已就绪：
 * execve(动态 PIE) → VM 映射 dalvikvm + linker64 + 构造 auxv →
 * linker64 接管：装载 DT_NEEDED 全链（走 ROM VFS 的 openat/pread64/mmap
 * 虚拟 fd 路径）→ 跳 dalvikvm main。
 * 本 guest 只负责：打印 banner → execve dalvikvm（带 -cp hello.dex）。 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("== TwinVM Stage 4.2: 真 linker 接管 + dalvikvm 引导 ==\n");
    char* a[] = {(char*)"dalvikvm", (char*)"-cp", (char*)"/rom/hello.dex",
                 (char*)"Hello", NULL};
    /* 真机 layout 的 boot classpath。v0.7.37 三点：
     *  · 顺序对齐设备 /apex/com.android.art/etc/classpaths/bootclasspath.pb
     *    （core-oj, core-libart, okhttp, bouncycastle, apache-xml；service-art 不在 boot 里）；
     *  · 必须同时给 DEX2OATBOOTCLASSPATH，且按 ART 规则它是 BOOTCLASSPATH 的**前缀**
     *    （libartbase 里有 "DEX2OATBOOTCLASSPATH must be a prefix of BOOTCLASSPATH"）；
     *  · 这些 jar 与 /apex/com.android.art/etc/* 必须真的进 ROM —— android/build.sh
     *    第 2 步原来只 cp 了 fixture 的 system/，apex/ 被丢掉，真机就报
     *    file_utils.cc:609 Check failed: ... BOOTCLASSPATH ... must not be empty。 */
    char* e[] = {
        (char*)"BOOTCLASSPATH=/apex/com.android.art/javalib/core-oj.jar:"
               "/apex/com.android.art/javalib/core-libart.jar:"
               "/apex/com.android.i18n/javalib/core-icu4j.jar:"
               "/apex/com.android.art/javalib/okhttp.jar:"
               "/apex/com.android.art/javalib/bouncycastle.jar:"
               "/apex/com.android.art/javalib/apache-xml.jar",
        /* v0.7.39 关键修正：DEX2OATBOOTCLASSPATH 必须是 BOOTCLASSPATH 的**真前缀**，
         * 前缀之后剩下的那些 jar 才是 ART 眼里的 "mainline framework library"。
         * 上一版我把两者设成一样 → libartbase 里
         *   GetFirstMainlineFrameworkLibraryFilename() 找不到 mainline →
         *   file_utils.cc:609 Check failed: !location.empty()
         *   "No mainline framework library found"（真机 2026-10-14 06:55 日志实锤）。
         * 真机约定（v1.2.50 六件形态）：DEX2OAT=core-oj:core-libart:core-icu4j，BOOT=core-oj:core-libart:core-icu4j:okhttp:bouncycastle:apache-xml（init.environ.rc 定义；art APEX 的 bootclasspath.pb 只有五件是 APEX 内部清单，不含 i18n 的 core-icu4j）
         * （与设备 /apex/com.android.art/etc/classpaths/bootclasspath.pb 一致）。 */
        (char*)"DEX2OATBOOTCLASSPATH=/apex/com.android.art/javalib/core-oj.jar:"
               "/apex/com.android.art/javalib/core-libart.jar:"
               "/apex/com.android.i18n/javalib/core-icu4j.jar",
        (char*)"ANDROID_ROOT=/system",
        (char*)"ANDROID_DATA=/data",
        (char*)"ANDROID_ASSETS=/system/app",
        (char*)"ANDROID_STORAGE=/storage",
        (char*)"LD_LIBRARY_PATH=/system/lib64",
        (char*)"PATH=/system/bin:/system/xbin",
        (char*)"TMPDIR=/data/local/tmp",
        NULL };
    /* v0.7.38：先交给 /bin/probe（动态 PIE，和 dalvikvm 同款装载路径）——
     * 它把"execve 之后我看得见的环境/ROM 路径"打进日志，再原样把 argv/env
     * 透传给 dalvikvm。用来定位 ART 那句
     *   file_utils.cc:609 Check failed: ... BOOTCLASSPATH ... must not be empty
     * 到底是环境没到、还是 android_root 推算失败。确认后可把下面这行换回
     * "/system/bin/dalvikvm"（probe 就可以不进 ROM 了）。 */
    printf("[guest] execve(/bin/probe -> /system/bin/dalvikvm)...\n");
    execve("/bin/probe", a, e);
    printf("[guest] !! execve 失败\n");
    _exit(98);
}
