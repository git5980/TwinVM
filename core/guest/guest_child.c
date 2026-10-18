/* TwinVM Stage 2.1 guest（子进程映像）—— 被 execve 装载的"另一个程序"。
 * 证明 exec 真的换了映像：argv/envp 是 execve 传进来的（不是父进程的）、
 * pid 是 fork 出来的真实 pid、退出码 42 是与父进程约定的暗号。 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char** argv, char** envp) {
    printf("[child-exec] 新映像运行中! pid=%d argc=%d\n", (int)getpid(), argc);
    for (int i = 0; i < argc; i++)
        printf("[child-exec] argv[%d]=%s\n", i, argv[i]);
    for (char** e = envp; e && *e; e++)
        printf("[child-exec] env: %s\n", *e);
    fflush(stdout);
    _exit(42);   /* 约定暗号 */
}
