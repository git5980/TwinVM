/* TwinVM Stage 0 guest 演示体 —— freestanding，无 libc，裸系统调用。
 * 它在"虚拟机"里的每一次 openat/read/write/exit_group 都会被
 * 用户态内核截获服务：从 ROM 读文件、被控制台桥输出、宿主文件系统完全不可见。 */
#if defined(__x86_64__)
static long _sys(long n, long a, long b, long c, long d) {
    long ret;
    register long r10 __asm__("r10") = d;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10)
                     : "rcx", "r11", "memory");
    return ret;
}
#define NR_openat     257
#define NR_read         0
#define NR_write        1
#define NR_exit_group 231
#elif defined(__aarch64__)
static long _sys(long n, long a, long b, long c, long d) {
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x8), "r"(x1), "r"(x2), "r"(x3)
                     : "memory", "cc");
    return x0;
}
#define NR_openat      56
#define NR_read        63
#define NR_write       64
#define NR_exit_group  94
#else
#error "guest 支持 x86_64/aarch64"
#endif

#define writelit(s) _sys(NR_write, 1, (long)(s), sizeof(s) - 1, 0)

void _start(void) {
    static char buf[256];

    long fd = _sys(NR_openat, -100 /*AT_FDCWD*/, (long)"/rom/hello.txt", 0, 0);
    if (fd < 0) {
        writelit("[guest] fatal: /rom/hello.txt open failed\n");
        _sys(NR_exit_group, 1, 0, 0, 0);
    }
    long n = _sys(NR_read, fd, (long)buf, sizeof(buf), 0);
    writelit("[guest] 读到 ROM 文件内容: ");
    _sys(NR_write, 1, (long)buf, n, 0);
    writelit("\n");

    /* 隔离证明：宿主文件系统里的 /etc/passwd 必须不存在（用户态内核回 ENOENT） */
    long bad = _sys(NR_openat, -100, (long)"/etc/passwd", 0, 0);
    if (bad < 0) writelit("[guest] 隔离验证: /etc/passwd -> ENOENT（宿主文件系统不可见）\n");

    writelit("[guest] TwinVM Stage 0 guest 执行完毕，正常关机。\n");
    _sys(NR_exit_group, 0, 0, 0, 0);
    for (;;) {}  /* 不会到达 */
}
