/* 最小 syscall 跟踪器 v2：fork+exec + PTRACE_SYSCALL + TRACESYSGOOD */
#include <stdio.h>
#include <sys/ptrace.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
int main(int argc, char** argv) {
    pid_t p = fork();
    if (p == 0) { ptrace(PTRACE_TRACEME, 0, 0, 0); execv(argv[1], argv + 1); _exit(127); }
    int st, entered = 0;
    waitpid(p, &st, 0);
    ptrace(PTRACE_SETOPTIONS, p, 0, (void*)(long)PTRACE_O_TRACESYSGOOD);
    ptrace(PTRACE_SYSCALL, p, 0, 0);
    long prev_nr = -1;
    while (waitpid(p, &st, 0) > 0 && !WIFEXITED(st) && !WIFSIGNALED(st)) {
        if (WSTOPSIG(st) == (SIGTRAP | 0x80)) {
            struct user_regs_struct r;
            ptrace(PTRACE_GETREGS, p, 0, &r);
            if (!entered) { prev_nr = (long)r.orig_rax; entered = 1; }
            else {
                printf("nr=%ld ret=%ld\n", prev_nr, (long)r.rax);
                entered = 0;
            }
        }
        ptrace(PTRACE_SYSCALL, p, 0, 0);
    }
    printf("== child done: %d (signal %d)\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1, WIFSIGNALED(st) ? WTERMSIG(st) : 0);
    return 0;
}
