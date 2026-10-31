/* vmdemo —— Linux 宿主上的 TwinVM Stage 0 演示/验证器。
 * 流程：读 ROM → fork 出 guest 槽位进程 → 子进程装 seccomp 拦截 +
 * 进程内装载 ROM 里的 guest → 原生执行；父进程收控制台输出。
 * 若沙箱禁 seccomp（如 Docker 默认策略），--backend ptrace 走监督者
 * 模式（fork+exec+PTRACE_SYSCALL，与 proot 同型）继续验证内核语义。 */
#define _GNU_SOURCE
#include "vm_core.h"

#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <linux/types.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <sys/ucontext.h>
#include <unistd.h>

/* v0.7.3：裸打印版 fault_diag——禁止 snprintf/glibc（bionic FS 下会二次崩溃=静默死） */
static void raw_diag(const char* tag, unsigned long v) {
    char b[96]; int k = 0;
    while (tag[k]) { b[k] = tag[k]; k++; }
    b[k++] = '=';
    char tmp[24]; int m = 0; unsigned long x = v;
    if (!x) tmp[m++] = '0';
    while (x && m < 20) { tmp[m++] = "0123456789abcdef"[x & 15]; x >>= 4; }
    while (m) b[k++] = tmp[--m];
    b[k++] = '\n';
    vm_console_write(b, (size_t)k);
}
static void guest_fault_diag(int sig, siginfo_t* si, void* vuc) {
    ucontext_t* uc = (ucontext_t*)vuc;
    raw_diag("[fault] sig", (unsigned long)sig);
    raw_diag("[fault] addr", (unsigned long)si->si_addr);
    raw_diag("[fault] rip", (unsigned long)uc->uc_mcontext.gregs[REG_RIP]);
    raw_diag("[fault] rsp", (unsigned long)uc->uc_mcontext.gregs[REG_RSP]);
    _exit(89);
}

static void run_guest_inchild(const VmRom* rom, int console_fd) {
    /* 子进程 = 一个 guest 槽位（对应 VMOS 的 :vmsXX） */
    VmCpu* cpu = vm_machine_setup(rom, getpid(), 1);
    (void)cpu;
    vm_console_fd = console_fd;

    const VmRomInode* g = vm_rom_find(rom, "/bin/guest");
    if (!g) {
        vm_log("ROM 缺 /bin/guest");
        _exit(98);
    }
    VmLaunch launch;
    int r = vm_loader_prepare(g->map, g->size, &launch);
    if (r != 0) {
        vm_log("loader prepare 失败: %d", r);
        _exit(98);
    }
    {   /* SIGSEGV 诊断（跳转前装好，抓 guest 崩溃现场） */
        struct sigaction d;
        memset(&d, 0, sizeof(d));
        d.sa_sigaction = guest_fault_diag;
        d.sa_flags = SA_SIGINFO;
        sigaction(SIGSEGV, &d, NULL);
        sigaction(SIGBUS, &d, NULL);
    }
    int sr = vm_sigsys_install(console_fd);
    if (sr != 0) {
        vm_log("seccomp 安装被拒(可能沙箱策略): errno=%d —— 退出码 97 交由宿主降级", -sr);
        _exit(97);
    }
    vm_log("guest 装载完成 entry=%p，拦截就绪，跳转执行（原生 %s 代码）",
           (void*)launch.entry, vm_arch_name());
    vm_loader_jump(&launch);   /* 不返回：guest 的 exit_group 结束本进程 */
    _exit(99);
}

static int pump_pipe(int fd) {
    char buf[1024];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        ssize_t w = write(1, buf, (size_t)n);
        (void)w;
    }
    return 0;
}

static int run_sigsys(const VmRom* rom) {
    int pfd[2];
    if (pipe(pfd) != 0) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        close(pfd[0]);
        run_guest_inchild(rom, pfd[1]);
        _exit(99);
    }
    close(pfd[1]);
    pump_pipe(pfd[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    close(pfd[0]);
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    if (WIFSIGNALED(st))
        vm_log("guest 槽位死于信号 %d (SIGSYS=31 SIGSEGV=11 SIGKILL=9 SIGABRT=6)", WTERMSIG(st));
    return 100;
}

/* ptrace 后端放行集（guest 独立进程内自洽的原语：内存/线程/信号/时钟）。
 * 这些不能由宿主代行——指针/地址参数属于 guest 地址空间，宿主一碰就崩；
 * 放行给 guest 自己执行，语义恰好正确（这正是 sigsys 后端的 BPF allow 集）。 */
static int pt_allow(long nr) {
    switch (nr) {
    case 9: case 10: case 11: case 12: case 25: case 28:          /* mmap/mprotect/munmap/brk/mremap/madvise */
    case 13: case 14: case 131: case 158:                         /* rt_sigaction/rt_sigprocmask/sigaltstack/arch_prctl */
    case 15:                                                      /* rt_sigreturn */
    case 56: case 62: case 202: case 218: case 234: case 61:      /* clone/kill/futex/set_tid_address/tgkill/wait4 */
    case 228: case 318: case 24: case 203: case 157:               /* clock/getrandom/yield/affinity/prctl */
        return 1;
    default:
        return 0;
    }
}

/* per-pid pending 结果（exit-stop 写回用——entry 改 orig_rax 后 exit 时 rax 是替身调用真值） */
#define PT_MAX_TRACK 16
static int   pt_pids[PT_MAX_TRACK];
static long  pt_results[PT_MAX_TRACK];
static int   pt_n;
static void pt_set_pending(int pid, long r) {
    for (int i = 0; i < pt_n; i++) if (pt_pids[i] == pid) { pt_results[i] = r; return; }
    if (pt_n < PT_MAX_TRACK) { pt_pids[pt_n] = pid; pt_results[pt_n] = r; pt_n++; }
}
static int pt_get_pending(int pid, long* out) {
    for (int i = 0; i < pt_n; i++) if (pt_pids[i] == pid) { *out = pt_results[i]; return 1; }
    return 0;
}
static void pt_clear_pending(int pid) {
    for (int i = 0; i < pt_n; i++) if (pt_pids[i] == pid) {
        for (int j = i + 1; j < pt_n; j++) { pt_pids[j-1] = pt_pids[j]; pt_results[j-1] = pt_results[j]; }
        pt_n--; return;
    }
}

/* ptrace 监督者模式（x86_64，本地沙箱无 seccomp 时的降级验证） */
static int run_ptrace(const VmRom* rom, const char* guest_path) {
    vm_log("backend=ptrace(supervisor) —— 监督者代行内核语义");
    pid_t pid = fork();
    if (pid == 0) {
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        char* argv[] = {(char*)"guest", NULL};
        char* envp[] = {NULL};
        execve(guest_path, argv, envp);
        _exit(96);
    }
    int st = 0;
    waitpid(pid, &st, 0);  /* exec 陷阱 */
    ptrace(PTRACE_SETOPTIONS, pid, 0, PTRACE_O_TRACESYSGOOD | PTRACE_O_EXITKILL);

    VmCpu* cpu = vm_machine_setup(rom, pid, 0);
    vm_console_fd = 1;
    int expect_entry = 1;
    long pending_result = 0;
    int have_pending = 0;

    /* Stage 2.1：多进程监督 —— fork 出的 guest 进程全部纳入 trace
     * （TRACECLONE/TRACEFORK/TRACEEXEC + waitpid(-1, __WALL)）；entry/exit 用
     * PTRACE_GET_SYSCALL_INFO 区分（per-pid 状态自治，不再依赖共享 expect_entry）。 */
    ptrace(PTRACE_SETOPTIONS, pid, 0, PTRACE_O_TRACESYSGOOD | PTRACE_O_EXITKILL
           | PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK | PTRACE_O_TRACECLONE | PTRACE_O_TRACEEXEC);
    ptrace(PTRACE_SYSCALL, pid, 0, 0);
    (void)expect_entry;
    for (;;) {
        int st2 = 0;
        int sp = waitpid(-1, &st2, __WALL);
        if (sp <= 0) break;
        if (WIFEXITED(st2)) {
            if (sp == pid) {
                vm_log("guest 退出码 %d，共服务 %llu 次系统调用", WEXITSTATUS(st2), (unsigned long long)cpu->cycles);
                return WEXITSTATUS(st2);
            }
            continue;
        }
        if (WIFSIGNALED(st2)) { if (sp == pid) return 100; continue; }
        if (!WIFSTOPPED(st2)) continue;
        int ev = (int)((unsigned)st2 >> 16);
        if (ev == PTRACE_EVENT_FORK || ev == PTRACE_EVENT_VFORK || ev == PTRACE_EVENT_CLONE) {
            vm_log("[supervisor] guest fork/clone 新进程纳入监督");
            ptrace(PTRACE_SYSCALL, sp, 0, 0);
            continue;
        }
        if (ev == PTRACE_EVENT_EXEC) {
            ptrace(PTRACE_SYSCALL, sp, 0, 0);
            continue;
        }
        int sig = WSTOPSIG(st2);
        if (sig != (SIGTRAP | 0x80)) { ptrace(PTRACE_SYSCALL, sp, 0, (void*)(long)sig); continue; }

        struct {   /* 手工声明内核 ABI（glibc 头无此结构） */
            __u8 op; __u8 pad[3]; __u32 arch;
            __u64 instruction_pointer; __u64 stack_pointer;
            union { struct { __u64 nr; __u64 args[6]; } entry;
                    struct { __s64 rval; __u8 is_error; } exit_; };
        } si;
        if (ptrace((enum __ptrace_request)0x420e /*PTRACE_GET_SYSCALL_INFO*/, sp, sizeof(si), &si) <= 0)
            { ptrace(PTRACE_SYSCALL, sp, 0, 0); continue; }
        if (si.op == 2 /* exit */) {
            long pr; int has = pt_get_pending(sp, &pr);
            if (has) {
                struct user_regs_struct xr;
                if (ptrace(PTRACE_GETREGS, sp, 0, &xr) == 0) {
                    xr.rax = (unsigned long long)pr;
                    ptrace(PTRACE_SETREGS, sp, 0, &xr);
                }
                pt_clear_pending(sp);
            }
            ptrace(PTRACE_SYSCALL, sp, 0, 0);
            continue;
        }
        if (si.op != 1 /* entry */) { ptrace(PTRACE_SYSCALL, sp, 0, 0); continue; }

        cpu->mem.pid = sp;   /* multi-guest: mem bridge follows stop pid */
        vm_binder_set_guest_pid(sp);   /* binder 身份：本 stop 的真实 guest pid */
        struct user_regs_struct regs;
        if (ptrace(PTRACE_GETREGS, sp, 0, &regs) != 0) { ptrace(PTRACE_SYSCALL, sp, 0, 0); continue; }
        long nr = (long)si.entry.nr;

        if (nr == 231) { ptrace(PTRACE_SYSCALL, sp, 0, 0); continue; }  /* exit_group 放行 */
        if (nr == 59) { /* execve 虚拟化：提取+注入+放行 */
            char epath[VM_MAX_PATH + 1];
            epath[VM_MAX_PATH] = 0;
            if (process_vm_readv(sp, &(struct iovec){epath, VM_MAX_PATH}, 1,
                                &(struct iovec){(void*)regs.rdi, VM_MAX_PATH}, 1, 0) > 0) {
                const VmRomInode* ino = vm_rom_find(rom, epath);
                if (ino) {
                    char tmp[] = "/tmp/twinvm-exec-XXXXXX";
                    int tfd = mkstemp(tmp);
                    if (tfd >= 0) {
                        ssize_t wn = write(tfd, ino->map, ino->size);
                        fchmod(tfd, 0700);
                        close(tfd);
                        if ((size_t)wn == ino->size) {
                            uintptr_t paddr = (uintptr_t)(regs.rsp - 1024) & ~(uintptr_t)0xF;
                            size_t pl = strlen(tmp) + 1;
                            if (process_vm_writev(sp, &(struct iovec){tmp, pl}, 1,
                                                  &(struct iovec){(void*)paddr, pl}, 1, 0) > 0) {
                                regs.rdi = (unsigned long long)paddr;
                                ptrace(PTRACE_SETREGS, sp, 0, &regs);
                                vm_log("execve(%s) 虚拟执行（ROM %lluB）", epath, (unsigned long long)ino->size);
                                ptrace(PTRACE_SYSCALL, sp, 0, 0);
                                continue;
                            }
                        }
                    }
                }
            }
            ptrace(PTRACE_SYSCALL, sp, 0, 0);
            continue;
        }
        /* 4.0：真 fd（pipe/epoll 等内核对象）的 read/write/close/ioctl → 原生放行。
         * 虚拟 fd 空间已迁至 [VM_FD_BASE=10000, +64)；[3,10000) 皆 guest 自建内核对象。 */
        if (nr == 0 || nr == 1 || nr == 3 || nr == 16) {
            long fd0 = (long)regs.rdi;
            if (fd0 > 2 && fd0 < 10000) { ptrace(PTRACE_SYSCALL, sp, 0, 0); continue; }
        }
        /* 拦截集：文件 I/O 虚拟化 */
        if (nr == 0 || nr == 1 || nr == 2 || nr == 3 || nr == 257
            || nr == 8 || nr == 17 || nr == 20
            || nr == 5 || nr == 262
            || nr == 217 || nr == 16 || nr == 63
            || nr == 102 || nr == 107 || nr == 104 || nr == 108
            || nr == 79 || nr == 267) {
            long r = vm_handle_syscall(cpu, nr, (long)regs.rdi, (long)regs.rsi,
                                       (long)regs.rdx, (long)regs.r10,
                                       (long)regs.r8, (long)regs.r9);
            regs.orig_rax = 39;
            ptrace(PTRACE_SETREGS, sp, 0, &regs);
            pt_set_pending(sp, r);   /* exit-stop 写回（exit 时内核会覆盖 rax=getpid 真值） */
        }
        ptrace(PTRACE_SYSCALL, sp, 0, 0);
    }
    return 100;
}

int main(int argc, char** argv) {
    const char* rompath = argc > 1 ? argv[1] : "demo.rom";
    const char* backend = (argc > 2) ? argv[2] : "auto";

    VmRom rom;
    int r = vm_rom_load(&rom, rompath);
    if (r != 0) {
        fprintf(stderr, "ROM 装载失败: %d (%s)\n", r, rompath);
        return 1;
    }
    vm_console_fd = 1;
    vm_log("boot: ROM=%s (%zu bytes, %u inodes, crc32 全部通过)", rompath,
           rom.size, rom.inode_count);
    for (uint32_t i = 0; i < rom.inode_count; i++)
        vm_log("  inode %u: %s (%llu bytes, mode=%o)", i, rom.inodes[i].name,
               (unsigned long long)rom.inodes[i].size, rom.inodes[i].mode);
    vm_log("架构: %s · 内核模式: 用户态截获（免 root）", vm_arch_name());

    if (strcmp(backend, "ptrace") == 0) {
        char tmp[] = "/tmp/twinvm-guest-XXXXXX";
        int tfd = mkstemp(tmp);
        const VmRomInode* g = vm_rom_find(&rom, "/bin/guest");
        if (!g) return 2;
        write(tfd, g->map, g->size);
        fchmod(tfd, 0700);
        close(tfd);
        return run_ptrace(&rom, tmp);
    }

    int rc = run_sigsys(&rom);
    if (rc == 97) {
        vm_log("seccomp 在此沙箱不可用 → 自动降级 ptrace 监督者模式继续验证");
        char tmp[] = "/tmp/twinvm-guest-XXXXXX";
        int tfd = mkstemp(tmp);
        const VmRomInode* g = vm_rom_find(&rom, "/bin/guest");
        if (!g) return 2;
        write(tfd, g->map, g->size);
        fchmod(tfd, 0700);
        close(tfd);
        return run_ptrace(&rom, tmp);
    }
    return rc == 0 ? 0 : rc;
}
