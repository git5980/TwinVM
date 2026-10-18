/* 独立复现器：SIGSYS handler 里处理 nr=9(mmap fd>=10000) 直接改 rax 返回。
 * 零 VM 依赖。死 = 内核/handler 机制问题；活 = VM 代码问题。 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <linux/seccomp.h>
#include <linux/filter.h>
#include <linux/audit.h>
#include <signal.h>
#include <unistd.h>
#include <stdint.h>

static void h(int sig, siginfo_t* si, void* vuc) {
    ucontext_t* uc = (ucontext_t*)vuc;
    long nr = (long)si->si_syscall;
    /* 只对 nr=9 改写 rax，其它原样（改 RIP 前进语义交内核） */
    if (nr == 9) {
        uc->uc_mcontext.gregs[REG_RAX] = 0x600000000000;   /* 假地址 */
    } else {
        uc->uc_mcontext.gregs[REG_RAX] = -38;              /* ENOSYS */
    }
    (void)sig;
}

int main(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = h;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSYS, &sa, NULL);

    /* BPF: nr==mmap(9) && args[4]>=10000 → TRAP；其余 ALLOW（同 TwinVM 形态） */
    struct sock_filter prog[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 4),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 0, 5),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, 9, 0, 3),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 48),
        BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, 10000, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, 0x30000),
        BPF_STMT(BPF_RET | BPF_K, 0x7fff0000),
    };
    struct sock_fprog fp = { .len = sizeof(prog)/sizeof(prog[0]), .filter = prog };
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) return 1;
    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &fp, 0, 0) != 0) { printf("install fail\n"); return 1; }

    printf("before mmap\n"); fflush(stdout);
    void* p = (void*)0x500000000000;
    long r;
    __asm__ volatile("syscall"
                     : "=a"(r)
                     : "a"(9L), "D"(0x500000000000UL), "S"(4096UL), "d"(1L),
                       "r"((long)0x32), "r"((long)10000), "r"((long)0)
                     : "rcx", "r11", "memory");
    (void)p;
    printf("after mmap r=%lx (handler ran? dead means TRAP loop)\n", r);
    fflush(stdout);
    return 0;
}
