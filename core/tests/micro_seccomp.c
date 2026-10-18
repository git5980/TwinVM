/* 微测试：本进程装我们生成的 BPF + SIGSYS handler + 调未放行 syscall(269)
 * 验证 SECCOMP_RET_TRAP 的信号投递在本环境是否工作 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <sys/prctl.h>
#include <linux/seccomp.h>
#include <linux/filter.h>
#include <linux/audit.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>

static void handler(int sig, siginfo_t* si, void* vuc) {
    char b[] = "[micro] SIGSYS! nr=xx\n";
    /* 纯 asm 打印避免任何依赖 */
    asm volatile(
        "movl $1, %%eax\n"
        "movl $2, %%edi\n"
        "movq %0, %%rsi\n"
        "movl %1, %%edx\n"
        "syscall\n"
        : : "r"(b), "r"((int)(sizeof(b)-1)) : "rax","rdi","rsi","rdx","rcx","r11","memory");
    (void)sig; (void)si; (void)vuc;
    _exit(77);   /* 到这就赢 */
}

int main(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSYS, &sa, NULL);

    #include "../src/bpf_gen.h"
    int console_fd = 2;   /* bpf_gen.h 引用 */
    struct sock_filter prog[] = VM_BPF_INSTRUCTIONS;
    struct sock_fprog fprog = { .len = (unsigned short)(sizeof(prog)/sizeof(prog[0])), .filter = prog };
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) { printf("nnp fail\n"); return 1; }
    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &fprog, 0, 0) != 0) { printf("seccomp fail errno=%d\n", __LINE__); return 1; }
    printf("[micro] filter installed (%zu insns), calling sendmmsg(269)...\n", sizeof(prog)/sizeof(prog[0]));
    fflush(stdout);
    /* 模拟 guest：FS 切到陌生 TLS 块（bionic 式抢占）再触发 TRAP */
    {
        extern int main(void);
        static char fake_tls[4096] __attribute__((aligned(16)));
        long fs_old;
        asm volatile("mov %%fs:0, %0" : "=r"(fs_old));
        asm volatile("mov %0, %%fs:0" : : "r"((long)fake_tls));
        long r = syscall(269);
        (void)r;
        asm volatile("mov %0, %%fs:0" : : "r"(fs_old));
    }
    long r = 0;
    printf("[micro] syscall returned %ld — handler did NOT run?!\n", r);
    return 2;
}
