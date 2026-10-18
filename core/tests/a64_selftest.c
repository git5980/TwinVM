/* TwinVM aarch64 自检宿主（等价于 Android JNI 的 stage0 路径，用于真机/容器复现）
 * 用法: ./a64_selftest demo.rom
 * 与 android/jni/twinvm_jni.c 的 jni_guest_main 同构：fork 槽位 → 装载 ROM 里的
 * /bin/guest → 装 seccomp-SIGSYS → 跳转；父进程收管道输出 + waitpid 报退出码。 */
#define _GNU_SOURCE
#include "vm_core.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ucontext.h>
#include <sys/wait.h>
#include <unistd.h>

static void guest_fault_diag(int sig, siginfo_t* si, void* vuc) {
    ucontext_t* uc = (ucontext_t*)vuc;
    uint64_t* r = uc->uc_mcontext.regs;
    char b[512];
    int k = snprintf(b, sizeof(b),
                     "[TwinVM] GUEST FAULT sig=%d addr=%p pc=0x%llx\n",
                     sig, si->si_addr, (unsigned long long)r[32]);
    vm_console_write(b, (size_t)k);
    _exit(89);
}

static void guest_main(const VmRom* rom, int console_fd) {
    vm_console_fd = console_fd;
    vm_machine_setup(rom, (int)getpid(), 1);

    struct sigaction d;
    memset(&d, 0, sizeof(d));
    d.sa_sigaction = guest_fault_diag;
    d.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &d, NULL);
    sigaction(SIGBUS, &d, NULL);

    vm_log("guest 槽位启动 pid=%d：ROM %u inodes 就绪", (int)getpid(), rom->inode_count);

    const VmRomInode* g = vm_rom_find(rom, "/bin/guest");
    if (!g) { vm_log("ROM 缺 /bin/guest"); _exit(98); }

    VmLaunch launch;
    int r = vm_loader_prepare(g->map, g->size, &launch);
    if (r != 0) { vm_log("装载器失败: %d", r); _exit(98); }

    int sr = vm_sigsys_install(console_fd);
    if (sr != 0) { vm_log("seccomp 拦截安装失败: %d", -sr); _exit(97); }

    vm_log("拦截就绪，装载 guest entry=%p（原生 %s 代码）", (void*)launch.entry, vm_arch_name());
    vm_loader_jump(&launch);
    _exit(99);
}

int main(int argc, char** argv) {
    const char* rompath = argc > 1 ? argv[1] : "demo.rom";
    VmRom rom;
    int rc = vm_rom_load(&rom, rompath);
    if (rc != 0) { fprintf(stderr, "ROM 装载失败: %d (%s)\n", rc, rompath); return 1; }
    vm_console_fd = 1;
    vm_log("boot: ROM=%s (%zu bytes, %u inodes)", rompath, rom.size, rom.inode_count);

    int pfd[2];
    if (pipe(pfd) != 0) return 1;
    pid_t pid = fork();
    if (pid == 0) {
        close(pfd[0]);
        guest_main(&rom, pfd[1]);
        _exit(99);
    }
    close(pfd[1]);
    char buf[4096];
    ssize_t n;
    while ((n = read(pfd[0], buf, sizeof(buf))) > 0) {
        fwrite(buf, 1, (size_t)n, stdout);
        fflush(stdout);
    }
    close(pfd[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    int code = WIFEXITED(st) ? WEXITSTATUS(st) : -WTERMSIG(st);
    printf("\n[TwinVM] guest 槽位退出: code=%d %s\n", code,
           code == 0 ? "（正常关机）" : "（异常）");
    if (WIFSIGNALED(st)) printf("[TwinVM] 死于信号 %d (%s)\n", WTERMSIG(st), strsignal(WTERMSIG(st)));
    return code == 0 ? 0 : 1;
}
