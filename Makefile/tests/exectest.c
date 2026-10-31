/* TwinVM 回归测试：**引擎 execve 通路**的 argv/envp 端到端（v0.7.38）
 *
 * 为什么要它：真机 ART 报
 *   file_utils.cc:609 Check failed: !location.empty()
 *   BOOTCLASSPATH and DEX2OATBOOTCLASSPATH must not be empty
 * 而反汇编 ART 的 libart.so 可见 ParsedOptions::DoParse 就是
 *   boot_class_path = getenv("BOOTCLASSPATH");  (空/缺失 → 直接致命)
 * 客端二进制(/bin/guest)确实把 BOOTCLASSPATH 放进了 envp（我们反汇编核过：
 *   envp 数组 = [BOOTCLASSPATH=..., ANDROID_DATA=/data, ANDROID_ROOT=/system, NULL]），
 * vm_loader 的栈构造也测过没问题（tests/envtest.c）。
 * 唯一没测过的一环就是**引擎自己**：从 guest 内存里读 argv/envp（vm_mem_read 循环）。
 * 本测试用白盒办法把它测掉：直接构造一个假 guest 现场（ucontext + siginfo），
 * 调 vm_syscall handler 的 execve 分支，然后按 ABI 解析引擎产出的新栈。
 *
 * 编译：见 Makefile 的 test-mem（本文件 include 了 vm_backend_sigsys.c 以拿 static handler）
 */
#define _GNU_SOURCE
/* 宿主是 glibc：引擎的 TLS 交换是给 bionic 用的（换 tpidr_el0），在 glibc 进程里
 * 会直接把 libc 的 TLS 换掉 → 下一次 printf/errno 就崩。测试里把它置成 no-op。 */
#define VM_TLS_GET(p) ((void)(p), 0)
#define VM_TLS_SET(v) ((void)(v))
#include "../src/vm_backend_sigsys.c"      /* static vm_sigsys_handler */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <ucontext.h>

static int g_fail;
static void ck(int ok, const char* what) {
    printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
    if (!ok) g_fail++;
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char* rompath = argc > 1 ? argv[1] : "../android/assets/demo.rom";
    VmRom rom;
    if (vm_rom_load(&rom, rompath) != 0) { printf("ROM 装载失败: %s\n", rompath); return 2; }
    printf("== 引擎 execve 通路 argv/envp 回归（%s）==\n", rompath);

    vm_console_fd = 2;
    vm_machine_setup(&rom, (int)getpid(), 1);

    /* 假 guest 现场：路径 + argv + envp（都是本进程内的“guest”内存） */
    static char path[] = "/system/bin/dalvikvm";
    static char s_bcp[] =
        "BOOTCLASSPATH=/apex/com.android.art/javalib/core-oj.jar:"
        "/apex/com.android.art/javalib/core-libart.jar:"
        "/apex/com.android.art/javalib/okhttp.jar:"
        "/apex/com.android.art/javalib/bouncycastle.jar:"
        "/apex/com.android.art/javalib/apache-xml.jar";
    static char s_data[] = "ANDROID_DATA=/data";
    static char s_root[] = "ANDROID_ROOT=/system";
    static char s_dex2[] = "DEX2OATBOOTCLASSPATH=/apex/com.android.art/javalib/core-oj.jar";
    static char s_a0[] = "dalvikvm", s_a1[] = "-cp",
                 s_a2[] = "/rom/hello.dex", s_a3[] = "Hello";
    static char* av[] = { s_a0, s_a1, s_a2, s_a3, NULL };
    static char* ev[] = { s_bcp, s_dex2, s_data, s_root, NULL };

    ucontext_t uc; memset(&uc, 0, sizeof(uc));
    siginfo_t si;  memset(&si, 0, sizeof(si));
    uintptr_t fake_pc = 0x25d78c;
    si.si_signo = SIGSYS;
    si.si_code = SYS_SECCOMP;
    si.si_syscall = 221;                  /* arm64 execve */
#ifdef si_call_addr
    si.si_call_addr = (void*)fake_pc;
#endif
    uc.uc_mcontext.pc = fake_pc;          /* 与 si_call_addr 一致 → 不判为迟到投递 */
#if defined(__x86_64__)
    uc.uc_mcontext.gregs[REG_R8]  = 59;
    uc.uc_mcontext.gregs[REG_RDI] = (greg_t)(uintptr_t)path;
    uc.uc_mcontext.gregs[REG_RSI] = (greg_t)(uintptr_t)av;
    uc.uc_mcontext.gregs[REG_RDX] = (greg_t)(uintptr_t)ev;
#else
    uc.uc_mcontext.regs[8] = 221;
    uc.uc_mcontext.regs[0] = (uint64_t)(uintptr_t)path;
    uc.uc_mcontext.regs[1] = (uint64_t)(uintptr_t)av;
    uc.uc_mcontext.regs[2] = (uint64_t)(uintptr_t)ev;
#endif

    vm_sigsys_handler(SIGSYS, &si, &uc);

    /* execve 成功语义 = 上下文被改写成新映像入口/新栈 */
#if defined(__x86_64__)
    uintptr_t pc = (uintptr_t)uc.uc_mcontext.gregs[REG_RIP];
    uintptr_t sp = (uintptr_t)uc.uc_mcontext.gregs[REG_RSP];
#else
    uintptr_t pc = (uintptr_t)uc.uc_mcontext.pc;
    uintptr_t sp = (uintptr_t)uc.uc_mcontext.sp;
#endif
    printf("  handler 返回后: pc=0x%lx sp=0x%lx（原 pc=0x%lx）\n",
           (unsigned long)pc, (unsigned long)sp, (unsigned long)fake_pc);
    ck(pc != fake_pc, "execve 分支真的接管并改写了 PC（跳到新映像）");
    ck(sp != 0, "新栈指针有效");

    uint64_t* s = (uint64_t*)sp;
    int na = 0, ne = 0;
    while (av[na]) na++;
    while (ev[ne]) ne++;
    ck(s[0] == (uint64_t)na, "新栈 argc == guest 传进来的 argv 个数");
    int ok_av = 1;
    for (int i = 0; i < na; i++)
        if (strcmp((const char*)s[1 + i], av[i]) != 0) ok_av = 0;
    ck(ok_av, "新栈 argv[] 每条一致（引擎确实把 guest 的 argv 读出来了）");
    uint64_t* ep = &s[2 + na];
    int gn = 0;
    while (ep[gn] && gn < 64) gn++;
    printf("  envc: 期望 %d 实际 %d\n", ne, gn);
    for (int i = 0; i < gn; i++) printf("    envp[%d] = %s\n", i, (const char*)ep[i]);
    ck(gn == ne, "新栈 envp 条数与 guest 传进来的完全一致");
    int ok_ev = 1;
    if (gn == ne)
        for (int i = 0; i < ne; i++)
            if (strcmp((const char*)ep[i], ev[i]) != 0) ok_ev = 0;
    ck(ok_ev, "新栈 envp[] 每条一致 —— BOOTCLASSPATH 必须原样到达 guest");
    const char* bcp = NULL;
    for (int i = 0; i < gn; i++)
        if (strncmp((const char*)ep[i], "BOOTCLASSPATH=", 14) == 0) bcp = (const char*)ep[i] + 14;
    ck(bcp && bcp[0], "BOOTCLASSPATH 在新栈里非空（= ART 那个 CHECK 的通过条件）");

    printf("\n%s\n", g_fail ? "FAIL" : "PASS: 引擎 execve 通路把 argv/envp 完整送到了新映像");
    return g_fail ? 1 : 0;
}
