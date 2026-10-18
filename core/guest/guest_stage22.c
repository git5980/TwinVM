/* TwinVM Stage 2.2 guest —— mini-ylinker：VM 内动态链接。
 * 流程：注册宿主符号 → dlopen("/rom/lib/libhello.so")（ROM 经 VFS 读入，
 * 段映射+重定位）→ dlsym → 调用 .so 导出的函数（其内部回调宿主符号，
 * JUMP_SLOT 重定位解析——真·双向跨模块）。附带 Stage 2.1 fork/exec 回归。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "mini_linker.h"

/* 宿主导出符号（.so 的 import 由这里解析——VMOS ylinker 的注册表式 API 面）。
 * __cxa_atexit/__register_atfork：NDK CRT 接口，静态 guest 提供无害桩。 */
static int vm_guest_api(int x) { return x * 6 + 1; }
static int stub_cxa_atexit(void* f, void* a, void* d) { (void)f;(void)a;(void)d; return 0; }
static int stub_register_atfork(void* a, void* b, void* c, void* d) { (void)a;(void)b;(void)c;(void)d; return 0; }
static void* stub_cxa_finalize(void* h) { (void)h; return 0; }
static const VmDlSym host_syms[] = {
    { "vm_guest_api",        (void*)vm_guest_api },
    { "__cxa_atexit",       (void*)stub_cxa_atexit },
    { "__cxa_finalize",     (void*)stub_cxa_finalize },
    { "__register_atfork",  (void*)stub_register_atfork },
};

int main(int argc, char** argv) {
    printf("== TwinVM Stage 2.2: mini-ylinker（VM 内动态链接） ==\n");

    /* ── Stage 2.2 主体：dlopen .so ── */
    vm_dl_register_host_symbols(host_syms, sizeof(host_syms) / sizeof(host_syms[0]));
    void* h = vm_dlopen("/rom/lib/libhello.so");
    if (!h) {
        printf("dlopen FAILED（ROM 缺 libhello.so？）\n");
        return 1;
    }
    printf("dlopen ok: base=%p, dynsym %zu 项\n", ((VmDlObj*)h)->base, ((VmDlObj*)h)->nsyms);

    int (*compute)(int) = (int (*)(int))vm_dlsym(h, "so_compute");
    const char* (*message)(void) = (const char* (*)(void))vm_dlsym(h, "so_message");
    if (!compute || !message) {
        printf("dlsym FAILED\n");
        return 1;
    }
    int r = compute(7);   /* so_compute → vm_guest_api(7)*2 = (7*6+1)*2 = 86 */
    printf("so_compute(7) = %d（期望 86：宿主符号经 JUMP_SLOT 解析）\n", r);
    printf("so_message(): %s\n", message());
    printf("动态链接 %s\n", r == 86 ? "验证通过" : "结果错误!");

    /* ── Stage 2.1 回归：fork + 虚拟 execve ── */
    pid_t pid = fork();
    if (pid == 0) {
        char* cargv[] = {(char*)"child", (char*)"--s22", NULL};
        execve("/bin/child", cargv, NULL);
        _exit(99);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    printf("[regression] fork+execve: %s\n",
           (WIFEXITED(st) && WEXITSTATUS(st) == 42) ? "退出码 42 ✓" : "失败");
    (void)argc; (void)argv;
    printf("== Stage 2.2 完成 ==\n");
    fflush(stdout);
    return 0;
}
