/* TwinVM Stage 2.1 guest（父进程）—— 虚拟进程模型演示。
 * fork → child execve("/bin/child")（用户态内核从 ROM 装载新映像）→
 * parent waitpid 收割 + 检查退出码。execve 从这里发起后被 sigsys 拦截，
 * 由 vm_loader_exec 换映像换栈改写上下文——VM 内的"进程创建"。
 * 同时回归 Stage 1 的关键能力项（malloc/ROM 读/隔离）。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char** argv) {
    printf("== TwinVM Stage 2.1: 虚拟进程模型 ==\n");
    printf("父进程 pid=%d, argv[0]=%s, argc=%d\n", (int)getpid(),
           argc > 0 ? argv[0] : "(null)", argc);

    /* Stage 1 能力回归（简版） */
    char* p = (char*)malloc(64 * 1024);
    if (!p) { printf("malloc fail\n"); return 1; }
    memset(p, 0x5A, 64 * 1024);
    printf("malloc 64KB ok\n");
    free(p);

    /* fork：真实进程（clone 直达）*/
    pid_t pid = fork();
    if (pid < 0) {
        printf("fork fail\n");
        return 1;
    }
    if (pid == 0) {
        /* 子进程：execve 到 ROM 里的另一个映像（虚拟 exec） */
        char* cargv[] = {(char*)"child", (char*)"--from-parent", NULL};
        char* cenvp[] = {(char*)"TWINVM=stage2", NULL};
        printf("[child-pre-exec] fork ok, 即将 execve /bin/child\n");
        int r = execve("/bin/child", cargv, cenvp);
        /* 只有失败才返回 */
        printf("[child] execve FAILED: %d (ROM 缺映像?)\n", r);
        _exit(99);
    }

    /* 父进程：收割子进程 */
    printf("[parent] child pid=%d, waiting...\n", (int)pid);
    int st = 0;
    pid_t w = waitpid(pid, &st, 0);
    if (w != pid) {
        printf("waitpid fail\n");
        return 1;
    }
    if (WIFEXITED(st))
        printf("[parent] child 退出码=%d %s\n", WEXITSTATUS(st),
               WEXITSTATUS(st) == 42 ? "(约定暗号正确——exec 真的换了映像)" : "");
    else
        printf("[parent] child 死于信号 %d\n", WTERMSIG(st));
    printf("== Stage 2.1 父进程完成 ==\n");
    return 0;
}
