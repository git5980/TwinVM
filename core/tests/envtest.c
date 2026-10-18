/* TwinVM 回归测试：exec_dyn 的 argv/envp 栈构造（v0.7.30）
 *
 * 真机症状（2026-10-13 21:22 日志）：
 *   dalvikvm file_utils.cc:609 Check failed: !location.empty()
 *   BOOTCLASSPATH and DEX2OATBOOTCLASSPATH must not be empty
 * 反汇编定位：CHECK 在 art::GetDefaultBootImageLocation() 里 —— 它用 **boot classpath
 * 第一项 jar 的目录**推出默认 boot image 位置；boot classpath 为空 → location 为空 →
 * FATAL。而 guest（guest_stage42.c）明明把 BOOTCLASSPATH 放进了 envp，
 * 所以要么 envp 没进新栈，要么字符串被写坏。
 *
 * 本测试直接调 vm_loader_exec_dyn（纯用户态 mmap/memcpy，不需要 seccomp/真机），
 * 构造出栈以后按 ABI 解析边界并逐串校验：
 *   [sp]=argc argv[] NULL envp[] NULL auxv[]，字符串在栈上
 * 断言：argc/envc 正确、每条 argv/envp 字符串**逐字节完好**、所有指针都在栈区间内。
 */
#define _GNU_SOURCE
#include "vm_core.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

static int g_fail;
static void ck(int ok, const char* what) {
    printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
    if (!ok) g_fail++;
}

int main(int argc, char** argv) {
    const char* rompath = argc > 1 ? argv[1] : "android/assets/demo.rom";
    VmRom rom;
    int rc = vm_rom_load(&rom, rompath);
    if (rc != 0) { printf("ROM 装载失败 %d: %s\n", rc, rompath); return 2; }

    const VmRomInode* exe = vm_rom_find(&rom, "/system/bin/dalvikvm");
    const VmRomInode* ld  = vm_rom_find(&rom, "/system/bin/linker64");
    printf("== exec_dyn argv/envp 栈构造回归（%s）==\n", rompath);
    if (!exe || !ld) { printf("ROM 缺 dalvikvm 或 linker64（%p %p）\n", (void*)exe, (void*)ld); return 2; }

    char* av[] = { (char*)"dalvikvm", (char*)"-cp", (char*)"/rom/hello.dex",
                   (char*)"Hello", NULL };
    char* ev[] = {
        (char*)"BOOTCLASSPATH=/apex/com.android.art/javalib/core-oj.jar:"
               "/apex/com.android.art/javalib/core-libart.jar:"
               "/apex/com.android.art/javalib/bouncycastle.jar:"
               "/apex/com.android.art/javalib/okhttp.jar:"
               "/apex/com.android.art/javalib/apache-xml.jar:"
               "/apex/com.android.art/javalib/service-art.jar",
        (char*)"DEX2OATBOOTCLASSPATH=/apex/com.android.art/javalib/core-oj.jar:"
               "/apex/com.android.art/javalib/core-libart.jar",
        (char*)"ANDROID_DATA=/data",
        (char*)"ANDROID_ROOT=/system",
        NULL };
    (void)ev;
    /* 可选：argv[2] = 额外塞几条长环境变量，用来暴露"字符串区没预留"的越界写。
     * 真机 dalvikvm 的 BOOTCLASSPATH 266B；这里放大到 ~1.5KB 就必然越过旧代码的渣余量。 */
    static char clutter[8][512];
    int extra = argc > 2 ? atoi(argv[2]) : 0;
    char* evbig[64];
    int eb = 0;
    for (int i = 0; i < 8 && eb < extra; i++) {
        int n = snprintf(clutter[i], sizeof(clutter[i]), "VMBIG%d=", i);
        for (; n < 500; n++) clutter[i][n] = (char)('a' + (n + i) % 26);
        clutter[i][n] = 0;
        evbig[eb++] = clutter[i];
    }
    for (int i = 0; ev[i]; i++) evbig[eb++] = ev[i];
    evbig[eb] = NULL;
    char** ev_use = eb > 4 ? evbig : ev;
    int na = 0, ne = 0;
    while (av[na]) na++;
    while (ev_use[ne]) ne++;

    vm_console_fd = 1;
    vm_machine_setup(&rom, (int)getpid(), 1);
    VmLaunch L;
    int r = vm_loader_exec_dyn(exe->map, exe->size, ld->map, ld->size,
                               "/system/bin/dalvikvm", av, ev_use, &L);
    printf("  exec_dyn=%d sp=0x%lx entry=%p\n", r, (unsigned long)L.sp, (void*)L.entry);
    if (r != 0) { ck(0, "vm_loader_exec_dyn 返回 0"); return 1; }

    uint64_t* sp = (uint64_t*)L.sp;
    uint64_t stk_lo = (uintptr_t)sp & ~(uintptr_t)0xFFFFF;   /* 1MB 栈底（近似） */
    ck(sp[0] == (uint64_t)na, "栈顶 argc == 传给 exec 的 argv 个数");
    int ok_av = 1, ok_ev = 1;
    for (int i = 0; i < na; i++) {
        const char* got = (const char*)sp[1 + i];
        if (strcmp(got, av[i]) != 0) { printf("    argv[%d]: 期望 %s 实际 %s\n", i, av[i], got); ok_av = 0; }
    }
    ck(ok_av, "argv[] 每一条都完好");
    ck(sp[1 + na] == 0, "argv 以 NULL 结尾");
    uint64_t* envp = &sp[2 + na];
    int got_ne = 0;
    while (envp[got_ne] && got_ne < 64) got_ne++;
    uint64_t* ax = envp + got_ne + 1;   /* auxv 起点（envp NULL 之后） */
    if (got_ne != ne) printf("    期望 envc=%d 实际 %d\n", ne, got_ne);
    for (int i = 0; i < ne && i < got_ne; i++) {
        const char* got = (const char*)envp[i];
        if (strcmp(got, ev_use[i]) != 0) {
            printf("    envp[%d]: 期望 %s\n              实际 %s\n", i, ev_use[i], got ? got : "(null)");
            ok_ev = 0;
        }
    }
    ck(got_ne == ne, "envp 条数与传入一致");
    ck(ok_ev, "envp 每一条都完好（BOOTCLASSPATH 不能为空/被写坏）");
    ck(envp[got_ne] == 0, "envp 以 NULL 结尾");

    /* 关键：boot classpath 是否可读且非空（ART file_utils.cc:609 的触发条件） */
    const char* bcp = NULL;
    for (int i = 0; i < got_ne; i++)
        if (strncmp((const char*)envp[i], "BOOTCLASSPATH=", 14) == 0) bcp = (const char*)envp[i] + 14;
    ck(bcp && bcp[0] != 0, "BOOTCLASSPATH 在新栈里非空（正是 ART 那个 CHECK 的条件）");
    if (bcp) printf("    BOOTCLASSPATH 长度 = %zu\n", strlen(bcp));

    /* 所有字符串（含结尾 NUL）必须整条落在**真实的栈映射** [lo,hi) 内 ——
     * v0.7.30 修的正是"字符串区没预留、写穿栈顶/写进相邻映射"这个 bug。 */
    uintptr_t lo = 0, hi = 0;
    FILE* m = fopen("/proc/self/maps", "r");
    char line[512];
    while (m && fgets(line, sizeof(line), m)) {
        unsigned long a, b;
        if (sscanf(line, "%lx-%lx", &a, &b) == 2 && (uintptr_t)sp >= a && (uintptr_t)sp < b) {
            lo = a; hi = b; break;
        }
    }
    if (m) fclose(m);
    printf("    栈映射 = [0x%lx, 0x%lx) 共 %lu KB（sp=0x%lx）\n",
           (unsigned long)lo, (unsigned long)hi, (unsigned long)((hi - lo) / 1024),
           (unsigned long)(uintptr_t)sp);
    int in_range = (lo != 0);
    for (int i = 0; i < na; i++) {
        uintptr_t p = (uintptr_t)sp[1 + i];
        if (p < lo || p + strlen((const char*)p) + 1 > hi) in_range = 0;
    }
    for (int i = 0; i < got_ne; i++) {
        uintptr_t p = (uintptr_t)envp[i];
        if (p < lo || p + strlen((const char*)p) + 1 > hi) in_range = 0;
    }
    ck(in_range, "每条 argv/envp 字符串整条都在栈映射内（不越界写穿）");

    /* AT_RANDOM / AT_EXECFN / AT_PLATFORM 这三个 auxv 指向的内容必须还在映射内且非空 */
    int aux_ok = 1;
    for (int i = 0; ; i += 2) {
        uint64_t t = ax[i];
        if (t == 0) break;                       /* AT_NULL */
        if (t == 25 /*AT_RANDOM*/ || t == 31 /*AT_EXECFN*/ || t == 15 /*AT_PLATFORM*/) {
            uintptr_t p = (uintptr_t)ax[i + 1];
            if (p < lo || p + 16 > hi) aux_ok = 0;
        }
    }
    ck(aux_ok, "AT_RANDOM/AT_EXECFN/AT_PLATFORM 指针与内容都在映射内（没被字符串覆盖写飞）");

    printf("\n%s\n", g_fail ? "FAIL" : "PASS: exec_dyn 的 argv/envp 栈构造正确，BOOTCLASSPATH 能到达 guest");
    return g_fail ? 1 : 0;
}
