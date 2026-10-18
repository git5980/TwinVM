/* raw_futex 内联汇编实现直测（宿主进程直接跑，x86_64） */
#include <stdio.h>
#include <stdint.h>
#include <time.h>

static long raw_futex(volatile uint32_t* uaddr, long op, uint32_t val, const void* timeout) {
    long r; register long rdi __asm__("rdi") = (long)uaddr;
    register long rsi __asm__("rsi") = op;
    register long rdx __asm__("rdx") = (long)val;
    register long r10 __asm__("r10") = (long)timeout;
    register long r8 __asm__("r8") = 0;
    register long r9 __asm__("r9") = 0;
    __asm__ volatile("syscall" : "=a"(r) : "a"(202L), "r"(rdi), "r"(rsi), "r"(rdx),
                     "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
    return r;
}

int main(void) {
    struct { long tv_sec; long tv_nsec; } ts = { 1, 0 };
    volatile uint32_t w = 0;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    long r = raw_futex(&w, 0 /*WAIT*/, 0 /*val*/, &ts);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    printf("futex_wait ret=%ld (期望 -110 ETIMEDOUT), 耗时 %.1fms (期望约1000)\n", r, ms);

    r = raw_futex(&w, 0, 5, &ts);
    printf("futex_wait val=5 实际=0: ret=%ld (期望 -11 EAGAIN)\n", r);
    return 0;
}
