/* freestanding 复现器：逐条逼近 bionic init 序列，找死点 */
#if defined(__x86_64__)
static long _sys(long n, long a, long b, long c, long d) {
    long ret; register long r10 __asm__("r10") = d;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10) : "rcx", "r11", "memory");
    return ret;
}
#define NR_arch_prctl 158
#define NR_getpid 39
#define NR_gettid 186
#define NR_rt_sigaction 13
#define NR_write 1
#define NR_exit_group 231
#define NR_prctl 157
#define NR_set_tid_address 218
#define NR_getrandom 318
#define ARCH_SET_FS 0x1002
#endif
static void w(const char* s, long n) { _sys(NR_write, 1, (long)s, n, 0); }
static char tb[16];
static void wn(const char* tag, long v) {
    for (int i = 0; i < 16; i++) tb[i] = 0;
    w(tag, 4);
    if (v < 0) { w("-", 1); v = -v; }
    char r[24]; int m = 0;
    do { r[m++] = '0' + (char)(v % 10); v /= 10; } while (v);
    char o[24]; for (int i = 0; i < m; i++) o[i] = r[m-1-i];
    w(o, m); w("\n", 1);
}
static char tls_area[4096] __attribute__((aligned(16)));

void _start(void) {
    wn("step", 0);
    long r = _sys(NR_arch_prctl, ARCH_SET_FS, (long)tls_area, 0, 0);
    wn("afs=", r);                    /* arch_prctl SET_FS */
    r = _sys(NR_getpid, 0, 0, 0, 0); wn("gpd=", r);
    r = _sys(NR_gettid, 0, 0, 0, 0); wn("gtd=", r);
    r = _sys(NR_set_tid_address, (long)&tls_area[0], 0, 0, 0); wn("std=", r);
    r = _sys(NR_prctl, 38 /*PR_SET_VMA*/, 0, 0, 0); wn("prc=", r);
    r = _sys(NR_getrandom, (long)tb, 8, 0, 0); wn("rnd=", r);
    /* rt_sigaction(13, act{SIG_DFL}, 0, 8) —— bionic init 会装一堆 */
    unsigned long act[4] = { 0 /*SIG_DFL*/, 0x04000000 /*SA_RESTORER*/, (unsigned long)0 /*restorer=0? 不行*/, 0 };
    w("sga> ...", 0);
    r = _sys(NR_rt_sigaction, 13, (long)act, 0, 8); wn("sga=", r);
    w("ALL OK\n", 7);
    _sys(NR_exit_group, 0, 0, 0, 0);
    for(;;){}
}
