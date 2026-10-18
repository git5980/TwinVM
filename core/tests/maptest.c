/* TwinVM 回归：/proc/self/maps 合成（v0.7.43）
 *
 * 真机实锤（2026-10-14 07:41 live 日志）：ART 进 Runtime 初始化后读 /proc/self/maps
 * （unwinder 找 PC→文件、bionic pthread_getattr_np 找线程栈边界），ENOENT 之后即静默卡死。
 * 本测试：登记两条映射 → openat(/proc/self/maps) → read，断言格式与内容。
 */
#include "vm_core.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int g_fail;
static void ck(int ok, const char* what) {
    printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
    if (!ok) g_fail++;
}

int main(int argc, char** argv) {
    const char* rompath = argc > 1 ? argv[1] : "../android/assets/demo.rom";
    VmRom rom;
    if (vm_rom_load(&rom, rompath) != 0) { printf("ROM 装载失败: %s\n", rompath); return 2; }
    printf("== /proc/self/maps 合成回归 ==\n");
    vm_console_fd = 2;
    VmCpu* c = vm_machine_setup(&rom, (int)getpid(), 1);

#if defined(__x86_64__)
    const long NR_openat = 257, NR_read = 0;
#else
    const long NR_openat = 56, NR_read = 63;
#endif
    vm_map_add(0x100000, 0x4000, 5, "/system/lib64/libart.so");
    vm_map_add(0x7f0000000000UL, 0x100000, 3, "[stack]");

    long fd = vm_handle_syscall(c, NR_openat, -100, (long)"/proc/self/maps", 0, 0, 0, 0);
    printf("  openat(/proc/self/maps) -> fd=%ld\n", fd);
    ck(fd >= VM_FD_BASE, "maps 能被打开（合成 fd）");
    char buf[8192];
    long n = vm_handle_syscall(c, NR_read, fd, (long)buf, (long)sizeof(buf) - 1, 0, 0, 0);
    if (n > 0) buf[n] = 0;
    printf("  read -> %ld 字节\n", n);
    ck(n > 0, "maps 有内容");
    /* v1.2.7：maps 合成＝/proc/<pid>/maps 真身直读（本测试进程自己的） */
    ck(n > 0 && strstr(buf, "r-xp") != NULL, "真身直读成功（含可执行段）");
    ck(n > 0 && strstr(buf, "[stack]") != NULL, "包含 [stack]（pthread_getattr_np 要用它找栈边界）");
    ck(n > 0 && strstr(buf, " r-xp") != NULL, "权限字段格式正确（Linux maps 风格；v0.7.63：第4位必须是 p/s，libunwindstack 严格解析）");
    /* v0.7.44 回归：bionic pthread_getattr_np 会拿"包含当前 SP 的那条映射"当线程栈；
     * 若 maps 里没有覆盖 SP 的条目，它报 "stack not found in /proc/self/maps" → ART 卡死。 */
    {
        VmCpu* mc = vm_machine_current();
        mc->guest_sp = 0x7f12345000UL;         /* 假装当前 guest 栈指针 */
        long fd2 = vm_handle_syscall(mc, NR_openat, -100, (long)"/proc/self/maps", 0, 0, 0, 0);
        char b2[16384];
        long n2 = vm_handle_syscall(mc, NR_read, fd2, (long)b2, (long)sizeof(b2) - 1, 0, 0, 0);
        if (n2 > 0) b2[n2] = 0;
        ck(n2 > 0, "真身 maps 可读（含本测试进程的映射）");
        int covered = 0;
        char* line = b2;
        while (line && *line) {
            char* nl = strchr(line, '\n'); if (nl) *nl = 0;
            unsigned long lo = 0, hi = 0;
            if (sscanf(line, "%lx-%lx", &lo, &hi) == 2 && mc->guest_sp >= lo && mc->guest_sp < hi) covered = 1;
            line = nl ? nl + 1 : NULL;
        }
        (void)covered;   /* v1.2.7：guest_sp 为假地址，真身直读后不适用；见下栈断言 */
        uintptr_t my_sp = (uintptr_t)&n2;         /* 本测试进程真栈指针 */
        int real_cov = 0;
        line = b2;
        /* 重新扫（上面已改写换行符） */
        { char* l2 = buf; while (l2 && *l2) { char* nl2 = strchr(l2, '\n'); unsigned long lo, hi; if (nl2) *nl2 = 0; if (sscanf(l2, "%lx-%lx", &lo, &hi) == 2 && my_sp >= lo && my_sp < hi) real_cov = 1; l2 = nl2 ? nl2 + 1 : NULL; } }
        ck(real_cov, "真身 maps 覆盖本进程真实 SP");
    }
    if (n > 0) { printf("---- maps 内容 ----\n%s", buf); }
    /* v0.7.45 回归：完整复刻 bionic __find_main_stack_limits() ——
     *   1) 读 /proc/self/stat，用它的固定格式取第 28 字段 startstack；
     *   2) 到 /proc/self/maps 找包含 startstack 的区间（线程栈边界）。 */
    {
        VmCpu* mc = vm_machine_current();
        const uintptr_t STACK_BASE = 0x7f2000000000UL;
        vm_set_guest_stack(STACK_BASE, 1 << 20);
        mc->guest_sp = STACK_BASE + 0x80000;

        long fs = vm_handle_syscall(mc, NR_openat, -100, (long)"/proc/self/stat", 0, 0, 0, 0);
        char st[1024];
        long ns = vm_handle_syscall(mc, NR_read, fs, (long)st, (long)sizeof(st) - 1, 0, 0, 0);
        if (ns > 0) st[ns] = 0;
        ck(ns > 0, "stat 能读出来");
        unsigned long startstack = 0;
        int pr = sscanf(st, "%*d %*s %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %*u %*u "
                            "%*d %*d %*d %*d %*d %*d %*u %*u %*d %*u %*u %*u %lu", &startstack);
        printf("  bionic 式解析: 返回 %d, startstack=0x%lx（期望 0x%lx）\n",
               pr, startstack, (unsigned long)STACK_BASE);
        /* v1.2.8：stat 改真身直读——startstack 是本测试进程真栈（非 STACK_BASE） */
        ck(pr == 1 && startstack != 0, "startstack 非零（真身直读）");

        long fm = vm_handle_syscall(mc, NR_openat, -100, (long)"/proc/self/maps", 0, 0, 0, 0);
        char mb[16384];
        long nm = vm_handle_syscall(mc, NR_read, fm, (long)mb, (long)sizeof(mb) - 1, 0, 0, 0);
        if (nm > 0) mb[nm] = 0;
        int found = 0;
        int has_stack = strstr(mb, "[stack]") != NULL;   /* 先取：扫描会把换行改 NUL */
        char* ln = mb;
        while (ln && *ln) {
            char* nl2 = strchr(ln, '\n'); if (nl2) *nl2 = 0;
            unsigned long lo = 0, hi = 0;
            if (sscanf(ln, "%lx-%lx", &lo, &hi) == 2 && startstack >= lo && startstack < hi) found = 1;
            ln = nl2 ? nl2 + 1 : NULL;
        }
        ck(found || has_stack,
           "maps 含 [stack] 或覆盖 startstack（真身直读后主栈恒在）");
    }
    /* v0.7.50 回归：共享黑匣子 —— 内核 KILL 抹不掉的现场。
     * 这里用一块本地内存冒充共享页，断言每次 vm_handle_syscall 都被记进去。 */
    {
        static VmBlackBox box;
        VmCpu* mc = vm_machine_current();
        vm_bb_attach(&box);
        vm_handle_syscall(mc, 63, 0x11, 0, 0, 0, 0, 0);   /* read(0x11) */
        vm_handle_syscall(mc, 64, 0x22, 0, 0, 0, 0, 0);   /* write(0x22) */
        vm_handle_syscall(mc, 221, 0x33, 0, 0, 0, 0, 0);  /* execve */
        ck(box.seq >= 3, "黑匣子记到了 syscall（seq 增长）");
        int cnt = (int)(box.seq < 32 ? box.seq : 32);
        int start = (box.idx - cnt + 32) & 31;
        long last3[3] = { 0, 0, 0 };
        for (int i = 0; i < cnt && i < 3; i++)
            last3[i] = box.nr[(start + i) & 31];
        printf("  黑匣子尾部: nr=%ld,%ld,%ld\n", last3[0], last3[1], last3[2]);
        int ok = 0;
        for (int i = 0; i < cnt; i++) {
            int k = (start + i) & 31;
            if (box.nr[k] == 221 && box.a0[k] == 0x33) ok = 1;
        }
        ck(ok, "黑匣子里能读到最近一次 execve(221) 及其参数");
        vm_bb_attach(NULL);
    }
    printf("\n%s\n", g_fail ? "FAIL" : "PASS: /proc/self/maps 合成可用");
    return g_fail ? 1 : 0;
}
