/* v0.7.70 —— 独立进程的"seccomp 拦截名单"探针（**无 libc** 版本，几 KB）。
 *
 * 为什么要独立进程：`SECCOMP_RET_KILL_PROCESS` 杀的是**整个进程**（含所有 fork 出的
 * 子进程）。真机实锤（v0.7.67 tombstone：syscall 306；v0.7.68：syscall 320）都是
 * "探针一碰被禁 syscall，App 连同 VM 一起被内核干掉"。
 *
 * 这个版本：
 *   · 不链接 libc（只用原始 svc：clone/wait4/write/exit）→ 体积 ~几 KB，可嵌进 .so；
 *   · 由 App 落盘到 /data/local/tmp 再 execv，**与 App 无亲缘依赖**；
 *   · 输出 "nr sig\n" 到 stdout，父进程读管道。
 *
 * 用法：probe_policy <nr> <nr> ...
 */
typedef unsigned long u64;
typedef long i64;

static long raw6(long nr, long a0, long a1, long a2, long a3, long a4, long a5) {
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    register long x3 __asm__("x3") = a3;
    register long x4 __asm__("x4") = a4;
    register long x5 __asm__("x5") = a5;
    __asm__ volatile("svc #0" : "+r"(x0)
                     : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory");
    return x0;
}
#define NR_clone 220
#define NR_wait4 260
#define NR_write 64
#define NR_exit  93
#define NR_exit_group 94
#define NR_getpid 172
#define NR_nanosleep 101

static unsigned slen(const char* s) { unsigned n = 0; while (s[n]) n++; return n; }

/* 极简整数转十进制（避免 libc） */
static void itoa(long v, char* out, int* len) {
    char tmp[24]; int n = 0;
    int neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) tmp[n++] = '0';
    while (v > 0) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    int k = 0;
    if (neg) out[k++] = '-';
    while (n > 0) out[k++] = tmp[--n];
    out[k] = 0; *len = k;
}

/* 试调一个 syscall：返回 0=安全，>0=死于此信号 */
static long try_one(long nr) {
    /* clone(SIGCHLD, 0, 0, 0, 0) —— arm64 子进程返回 0 */
    long p = raw6(NR_clone, 17 /*SIGCHLD*/, 0, 0, 0, 0, 0);
    if (p == 0) {
        raw6(nr, 0, 0, 0, 0, 0, 0);
        raw6(NR_exit, 0, 0, 0, 0, 0, 0);
    }
    if (p < 0) return -2;
    long st = 0;
    raw6(NR_wait4, p, (long)&st, 0, 0, 0, 0);
    /* WIFSIGNALED: (st & 0x7f) != 0 && != 0x7f */
    long sig = st & 0x7f;
    if (sig != 0 && sig != 0x7f) return sig;
    return 0;
}

/* v0.7.70：入口用汇编取初始 sp（arm64 的 _start 处 sp 指向 argc）：
 *   asm("mov %0, sp") 在函数序言之前执行才可靠 —— 这里用一个 naked 包装。 */
__attribute__((naked)) void _start(void) {
    __asm__ volatile(
        "mov x0, sp\n"        /* x0 = 指向 argc 的栈顶 */
        "b   tvm_probe_main\n");
}

void tvm_probe_main(long* sp) {
    long argc = sp[0];
    char** argv = (char**)(sp + 1);

    char buf[64];
    for (long i = 1; i < argc; i++) {
        long nr = 0; int neg = 0; char* s = argv[i];
        if (*s == '-') { neg = 1; s++; }
        while (*s >= '0' && *s <= '9') { nr = nr * 10 + (*s - '0'); s++; }
        if (neg) nr = -nr;
        long sig = try_one(nr);
        int n1 = 0, n2 = 0; char a[24], b[24];
        itoa(nr, a, &n1); itoa(sig, b, &n2);
        int k = 0;
        for (int q = 0; q < n1; q++) buf[k++] = a[q];
        buf[k++] = ' ';
        for (int q = 0; q < n2; q++) buf[k++] = b[q];
        buf[k++] = '\n';
        raw6(NR_write, 1, (long)buf, k, 0, 0, 0);
    }
    raw6(NR_exit_group, 0, 0, 0, 0, 0, 0);
    for (;;) {}
}

/* v0.7.70：-nostdlib 下编译器可能生成 memcpy 调用，自带一个极简实现 */
void* memcpy(void* d, const void* s, unsigned long n) {
    unsigned char* dd = (unsigned char*)d;
    const unsigned char* ss = (const unsigned char*)s;
    for (unsigned long i = 0; i < n; i++) dd[i] = ss[i];
    return d;
}
void* memset(void* d, int c, unsigned long n) {
    unsigned char* dd = (unsigned char*)d;
    for (unsigned long i = 0; i < n; i++) dd[i] = (unsigned char)c;
    return d;
}
