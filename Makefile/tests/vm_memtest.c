/* TwinVM 回归测试：guest 野指针防护 + 控制台 NUL 卫生（v0.7.22）
 *
 * 复现 2026-10-13 16:25 真机日志的两个致命点（都不需要 seccomp，普通命令行即可跑）：
 *   1) writev 的 iov 域非法（guest 野指针）→ 旧版 in_process 直接 memcpy → handler 内
 *      SIGSEGV → 槽位被杀，日志"writev 返回后无下文"；
 *   2) 被 tee 的 logd 负载（大片 0x00）进控制台 → JNI 侧 NewStringUTF 遇到第一个 NUL
 *      就截断 → 连退出码/致命信号尾巴一起丢。
 *
 * 断言：野生指针只返回错误、绝不崩；控制台字节流里不允许出现 NUL；
 *       writev 判决表在坏 iov / 坏 iov 表下都要打完诊断并正常返回。
 * 编译：gcc -O2 -Iinclude -o build/vm_memtest tests/vm_memtest.c src/vm_core.c src/vm_rom.c
 *      （只需 vm_core；不需要 backend/loader/seccomp）
 */
#define _GNU_SOURCE
#include "vm_core.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_fail;
static void ck(int ok, const char* what) {
    printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
    if (!ok) g_fail++;
}

/* 把控制台重定向到管道，收集这段输出（并顺带确认里面没有 NUL） */
static size_t capture_console(const char* data, size_t n, char* out, size_t cap, int* has_nul) {
    int pfd[2];
    if (pipe(pfd) != 0) return 0;
    int saved = vm_console_fd;
    vm_console_fd = pfd[1];
    vm_console_write(data, n);
    close(pfd[1]);
    vm_console_fd = saved;
    size_t len = 0;
    ssize_t r;
    *has_nul = 0;
    while ((r = read(pfd[0], out + len, cap - 1 - len)) > 0) {
        for (ssize_t i = 0; i < r; i++) if (out[len + (size_t)i] == 0) *has_nul = 1;
        len += (size_t)r;
        if (len >= cap - 1) break;
    }
    close(pfd[0]);
    out[len] = 0;
    return len;
}

int main(void) {
    /* v0.7.41 回归：msync 必须成功返回（真机 ART 拿到 ENOSYS 会无限重试 → UI 假死）。
     * 号码：arm64 227 / x86_64 26。 */
    {
        VmCpu* mc = vm_machine_setup(NULL, (int)getpid(), 1);
#if defined(__x86_64__)
        const long NR_msync = 26;
#else
        const long NR_msync = 227;
#endif
        long r = vm_handle_syscall(mc, NR_msync, 0, 0, 0, 0, 0, 0);
        ck(r == 0, "msync 返回 0（不能是 ENOSYS，否则 ART 死循环）");
    }

    printf("== TwinVM v0.7.22 回归: 野指针防护 + 控制台 NUL 卫生 ==\n");

    /* 一台空机器（只有内存桥/VFS 需要，不装过滤器、不跳 guest） */
    VmRom rom;
    memset(&rom, 0, sizeof(rom));
    VmCpu* c = vm_machine_setup(&rom, (int)getpid(), 1);

    printf("-- 1. vm_mem_read/write 对非法地址必须是 -EFAULT，不是 SIGSEGV\n");
    char small[32];
    ck(vm_mem_read(&c->mem, small, (uintptr_t)0x10, 16) == -1, "读 0x10（未映射）→ -1");
    ck(vm_mem_read(&c->mem, small, (uintptr_t)0xDEADBEEF0000ULL, 16) == -1, "读野地址 0xDEADBEEF0000 → -1");
    ck(vm_mem_write(&c->mem, (uintptr_t)0x10, small, 16) == -1, "写 0x10 → -1");
    /* 只读页（本进程 .text 附近）：写入应失败 */
    ck(vm_mem_write(&c->mem, (uintptr_t)(uintptr_t)&main, "x", 1) == -1, "写只读 .text → -1");

    printf("-- 2. 正常访存仍然可用（读写回环）\n");
    char src[16]; memset(src, 0x5A, sizeof(src));
    char dst[16]; memset(dst, 0, sizeof(dst));
    ck(vm_mem_read(&c->mem, dst, (uintptr_t)src, sizeof(src)) == 0 && memcmp(dst, src, 16) == 0,
       "同进程读到自身栈缓冲");
    memset(dst, 0xA5, sizeof(dst));
    ck(vm_mem_write(&c->mem, (uintptr_t)src, dst, sizeof(dst)) == 0 && memcmp(src, dst, 16) == 0,
       "同进程写自身栈缓冲");

    printf("-- 3. 控制台必须丢弃 NUL / 折掉控制字符（否则 JNI NewStringUTF 截断日志）\n");
    {
        char out[256]; int nul = 0;
        /* 注意 "\x01C" 会被当成一个十六进制转义（0x1C），故用相邻字面量断开 */
        static const char in[] = "A\0" "B\x01" "C\tD\nE";
        size_t n = capture_console(in, sizeof(in) - 1, out, sizeof(out), &nul);
        ck(!nul && n == 8 && strcmp(out, "AB.C\tD\nE") == 0, "NUL 丢弃 + 0x01→'.'，后续内容保留");
        /* 模拟 logd struct log_msg：一堆 0 里夹着文本 */
        char blob[64]; memset(blob, 0, sizeof(blob));
        memcpy(blob + 8, "logd text", 9);
        n = capture_console(blob, sizeof(blob), out, sizeof(out), &nul);
        ck(!nul && strstr(out, "logd text") != NULL, "logd 负载（含 NUL 填充）仍能看到正文、无 NUL");
    }

    printf("-- 4. writev：坏 iov 表 / 坏 iov 域 都不能崩，且要打完诊断\n");
    {
        char out[1024]; int nul = 0;
        /* 4a：iov 表本身就是野指针 */
        int pfd[2]; pipe(pfd);
        int saved = vm_console_fd; vm_console_fd = pfd[1];
        #if defined(__x86_64__)
        long WR_NR = 20;
#else
        long WR_NR = 66;
#endif
        long r = vm_handle_syscall(c, WR_NR, 83, (long)0x10 /*坏表*/, 2, 0, 0, 0);
        close(pfd[1]); vm_console_fd = saved;
        size_t len = read(pfd[0], out, sizeof(out) - 1); close(pfd[0]);
        out[len > 0 ? len : 0] = 0;
        for (size_t i = 0; i < len; i++) if (out[i] == 0) nul = 1;
        ck(r >= 0 && !nul && strstr(out, "[bad iov #0") != NULL,
           "野 iov 表 → 返回正常 + [bad iov #0] 诊断（旧版这里是 SIGSEGV）");

        /* 4b：iov 表合法，但 iov[0].base 指向未映射区（作者怀疑的真凶） */
        struct { uintptr_t base; size_t len; } iov[2];
        iov[0].base = (uintptr_t)0x10; iov[0].len = 64;              /* 野指针 */
        iov[1].base = (uintptr_t)"TAIL-TEXT"; iov[1].len = 9;        /* 正常 */
        pipe(pfd); saved = vm_console_fd; vm_console_fd = pfd[1];
        r = vm_handle_syscall(c, WR_NR, 83, (long)iov, 2, 0, 0, 0);
        close(pfd[1]); vm_console_fd = saved;
        len = read(pfd[0], out, sizeof(out) - 1); close(pfd[0]);
        out[len > 0 ? len : 0] = 0;
        nul = 0; for (size_t i = 0; i < len; i++) if (out[i] == 0) nul = 1;
        ck(r == 73 && !nul && strstr(out, "[iov#0 域非法") != NULL && strstr(out, "TAIL-TEXT") != NULL,
           "坏 iov 域 → 跳过该项 + [iov#0 域非法]，后续 iov 照常 tee，返回累计长度");
    }


    printf("-- 5. getdents64：整条 dirent 一次性写回（v0.7.22 改单次 vm_mem_write）\n");
    {
#if defined(__x86_64__)
        const long NR_getdents64 = 217;
#else
        const long NR_getdents64 = 61;
#endif
        static VmRomInode ino[3];
        VmRom r2; memset(&r2, 0, sizeof(r2));
        r2.inodes = ino; r2.inode_count = 3;
        strcpy(ino[0].name, "/rom/hello.txt"); ino[0].size = 5; ino[0].mode = 0100644;
        strcpy(ino[1].name, "/rom/poem.txt");  ino[1].size = 5; ino[1].mode = 0100644;
        strcpy(ino[2].name, "/bin/guest");     ino[2].size = 5; ino[2].mode = 0100755;
        VmCpu* c2 = vm_machine_setup(&r2, (int)getpid(), 1);
        int vfd = vm_vfs_open(c2->vfs, "/rom");          /* 目录 fd（VM 区间） */
        ck(vfd >= VM_FD_BASE, "openat(\"/rom\") 得到目录虚拟 fd");
        unsigned char dbuf[512]; memset(dbuf, 0xCC, sizeof(dbuf));
        long n = vm_handle_syscall(c2, NR_getdents64, vfd, (long)dbuf, sizeof(dbuf), 0, 0, 0);
        /* 逐条解析：ino8 + off8 + reclen2 + type1 + name */
        size_t off = 0; int names = 0, bad = 0;
        while (off + 19 <= (size_t)n) {
            uint16_t rl; memcpy(&rl, dbuf + off + 16, 2);
            if (rl == 0 || off + rl > (size_t)n) { bad = 1; break; }
            const char* nm = (const char*)(dbuf + off + 19);
            if (dbuf[off + 18] != 8) bad = 1;                     /* 都是普通文件 */
            if (strcmp(nm, "hello.txt") == 0 || strcmp(nm, "poem.txt") == 0) names++;
            else bad = 1;
            off += rl;
        }
        ck(n > 0 && !bad && names == 2 && off == (size_t)n,
           "两条 dirent 布局正确（reclen 对齐、type=DT_REG、名称无 \"/bin\" 越界）");
    }

    printf("\n%s\n", g_fail ? "FAIL" : "PASS: 野指针不再崩槽位；控制台无 NUL（日志不会再被截断）");
    return g_fail ? 1 : 0;
}
