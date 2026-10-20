/* TwinVM Stage 0 核心实现：虚拟系统调用调度 / VFS / 内存桥 / 控制台
 * —— 这就是"用户态内核"的心脏：guest 的每一次系统调用都到这里判决。 */
#define _GNU_SOURCE
#include "vm_core.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>   /* v1.2.1: /apex 直读的 stat/fstat */
#include <sys/uio.h>
#include <unistd.h>

#if defined(__x86_64__)
#define VM_NR_read          0
#define VM_NR_write         1
#define VM_NR_open          2
#define VM_NR_close         3
#define VM_NR_fstat         5
#define VM_NR_lseek         8
#define VM_NR_mmap          9
#define VM_NR_mq_open       240
#define VM_NR_mq_unlink     241
#define VM_NR_mprotect     10
#define VM_NR_munmap       11
#define VM_NR_brk          12
#define VM_NR_rt_sigaction 13
#define VM_NR_set_robust_list 273
#define VM_NR_rt_sigprocmask 14
#define VM_NR_ioctl        16
#define VM_NR_pread64      17
#define VM_NR_writev      20
#define VM_NR_sched_yield 24
#define VM_NR_madvise     28
#define VM_NR_msync       26
#define VM_NR_getpid      39
#define VM_NR_clone       56
#define VM_NR_mremap      25
#define VM_NR_fcntl       72
#define VM_NR_getcwd      79
#define VM_NR_kill        62
#define VM_NR_uname       63
#define VM_NR_wait4       61
#define VM_NR_exit        60
#define VM_NR_getuid     102
#define VM_NR_getgid     104
#define VM_NR_geteuid    107
#define VM_NR_getegid    108
#define VM_NR_getppid    110
#define VM_NR_gettid     186
#define VM_NR_sigaltstack 131
#define VM_NR_rt_sigtimedwait 428   /* v1.2.53：x86 侧（arm64=137） */
#define VM_NR_futex      202
#define VM_NR_sched_getaffinity 203
#define VM_NR_set_tid_address 218
#define VM_NR_getdents64 217
#define VM_NR_clock_gettime 228
#define VM_NR_getrandom  318
#define VM_NR_newfstatat 262
#define VM_NR_readlinkat 267
#define VM_NR_arch_prctl 158
#define VM_NR_tgkill     234
#define VM_NR_rt_sigreturn 15
#define VM_NR_openat     257
#define VM_NR_prctl      157
#define VM_NR_exit_group 231
#define VM_NR_pread64_x86 17
#define VM_NR_process_vm_readv 310
#define VM_NR_process_vm_writev 311
#define VM_NR_socket 41
#define VM_NR_connect 42
#elif defined(__aarch64__)
#define VM_NR_getcwd      17
#define VM_NR_wait4      260
#define VM_NR_ioctl       29
#define VM_NR_fcntl       25
#define VM_NR_getdents64  61
#define VM_NR_lseek       62
#define VM_NR_read        63
#define VM_NR_write       64
#define VM_NR_writev      66
#define VM_NR_pread64     67
#define VM_NR_readlinkat  78
#define VM_NR_newfstatat  79
#define VM_NR_fstat       80
#define VM_NR_exit        93
#define VM_NR_exit_group  94
#define VM_NR_set_tid_address 96
#define VM_NR_futex       98
#define VM_NR_kill       129
#define VM_NR_rt_sigaction 134
#define VM_NR_set_robust_list 99
#define VM_NR_rt_sigprocmask 135
#define VM_NR_sigaltstack 132
#define VM_NR_tgkill      131
#define VM_NR_clone       220
#define VM_NR_sched_yield 124
#define VM_NR_sched_getaffinity 123
#define VM_NR_sched_setscheduler 154
#define VM_NR_sched_getscheduler 155
#define VM_NR_uname       160
#define VM_NR_getpid     172
#define VM_NR_getppid    173
#define VM_NR_getuid     174
#define VM_NR_geteuid    175
#define VM_NR_getgid     176
#define VM_NR_getegid    177
#define VM_NR_gettid     178
#define VM_NR_brk        214
#define VM_NR_munmap     215
#define VM_NR_mremap     216
#define VM_NR_mmap       222
#define VM_NR_mq_open       240
#define VM_NR_mq_unlink     241
#define VM_NR_rt_sigtimedwait 137   /* v1.2.53：ART Signal Catcher 的 sigwait 通道 */
#define VM_NR_mprotect   226
#define VM_NR_clock_gettime 113
#define VM_NR_getrandom  278
#define VM_NR_fallocate_arm 47
#define VM_NR_memfd_create 279
#define VM_NR_fallocate  47
#define VM_NR_prctl      167
#define VM_NR_madvise    233
#define VM_NR_msync      227
#define VM_NR_rt_sigreturn 139
#define VM_NR_openat      56
#define VM_NR_close       57
#define VM_NR_pread64_arm 67
#define VM_NR_process_vm_readv 270
#define VM_NR_process_vm_writev 271
#define VM_NR_socket 198
#define VM_NR_connect 203
#else
#error "TwinVM Stage 1 支持 x86_64(本地验证) 与 aarch64(真机)"
#endif

/* 注：VM_NR_pread64 已在上面各架构块里定义（x86_64=17 / aarch64=67）；
 * v0.7.24 删掉这里曾经的二次别名定义（编译器 redefined 警告，值相同但吵）。 */

/* ── 引擎自身在 SIGSYS handler 内调用的 syscall（v0.7.22）──
 * 同一条铁律：只要 handler 会执行它，过滤器就必须放行，否则二次 TRAP。
 *   process_vm_readv/writev：vm_mem_read/vm_mem_write 的安全访存（guest 野指针 → -EFAULT）
 *   write(console)：vm_console_write（BPF 里按 fd 参数化放行，单独核对）
 * 该表由 gen_bpf.py 的 ALWAYS 提供对应放行条目，vm_filter_check_passthrough() 逐条核对。 */
/* v0.7.76：**清空**。以前这里列着 process_vm_readv/writev，是因为旧版引擎
 * 在 handler 内用它们做安全访存；现在引擎用的是自己的 vm_mem_read/vm_mem_write
 * （纯内存拷贝，不碰内核），所以引擎**不再需要**任何自用 syscall。
 * 继续把它们留在放行清单里会造成极隐蔽的故障：guest（ART）调用时绕过 handler
 * 直达内核，我们实现的 VM 语义版本永远不执行（真机 v0.7.75 实锤）。 */
/* 空表：引擎不再使用任何自用 syscall（v0.7.76）。用 -1 哨兵让解析器识别为空。 */
const long vm_engine_syscall_nrs[] = { -1 };
const int vm_engine_syscall_nr_count =
    (int)(sizeof(vm_engine_syscall_nrs) / sizeof(vm_engine_syscall_nrs[0]));

/* ═══ v0.2.8：判决表"直发内核"集合（不变量自检用） ═══
 * 下面每个 syscall 在 vm_handle_syscall 里都是 VM_PT*（handler 内裸 svc 直发）。
 * 它必须同时出现在 BPF 放行清单（bpf_gen.h 的 VM_BPF_ALLOW_NRS）里：
 * 否则该 svc 会被本过滤器二次 TRAP，而此刻 SIGSYS 处于阻塞态 → 内核不执行该
 * syscall、信号挂起，等 guest 自己恢复掩码后才迟到投递（ucontext 已非系统调用
 * 现场）→ 用垃圾参数执行一次无关 syscall / handler 自递归。
 * bionic 的 pthread_create 恰好在 clone 前后屏蔽全部信号，线程入口的 gettid 与
 * __exit(exit) 正落在这个窗口 —— 这就是 Stage 1 自检 pthread 一步 code=-31 的根因。
 * vm_sigsys_install() 会用 vm_filter_check_passthrough() 逐条核对并在不一致时告警。 */
const long vm_passthrough_nrs[] = {
    VM_NR_exit_group, VM_NR_rt_sigreturn, VM_NR_exit, VM_NR_gettid,
    VM_NR_brk, VM_NR_mmap, VM_NR_munmap, VM_NR_mprotect, VM_NR_mremap, VM_NR_madvise,
    VM_NR_futex, VM_NR_set_tid_address,
    /* v0.7.83：prctl 移出直发表 —— 已改为宿主原生调用（见 case 处注释） */
    VM_NR_rt_sigprocmask, VM_NR_sigaltstack,
    VM_NR_rt_sigtimedwait,        /* v1.2.53：arm64 137——Signal Catcher 的 sigwait */
    VM_NR_kill, VM_NR_tgkill, VM_NR_wait4, VM_NR_clone,
    VM_NR_clock_gettime, VM_NR_getrandom, VM_NR_sched_yield,
    VM_NR_sched_getaffinity, VM_NR_pread64,
    /* v0.7.62：fcntl 移出此表 → BPF fd 参数化（fd≥10000 TRAP / 真 fd ALLOW）。
     * 留在表里会被 bpf_sim 用 fd=0 验证（stdio→TRAP）误判。直发安全性由
     * auto-scan（arg_sets fd=5）+ handler 三层路由保证。 */
#if defined(__x86_64__)
    VM_NR_arch_prctl,
#endif
};
const int vm_passthrough_nr_count = (int)(sizeof(vm_passthrough_nrs) / sizeof(vm_passthrough_nrs[0]));

/* ── Stage 1：内核代发原语（透传通道） ──
 * v0.7.24：**每次直发前先问过滤器**。裸 svc 同样受本过滤器管辖：如果这次调用的判决
 * 不是 ALLOW，内核会把它变成"在 SIGSYS handler 里二次 TRAP"——此刻 SIGSYS 已阻塞，
 * syscall 被丢弃、信号挂起，等 guest 恢复掩码才迟到投递（ucontext 已非系统调用现场）
 * 或者直接以 SIGSYS 结束进程。2026-10-13 17:09 真机的 connect 直发就是这样炸的。
 * 所以：未放行 → 拒绝直发，返回 -ENOSYS 并告警（一次一条，避免刷屏）。
 * 这样即使以后有人往判决表里加了新的直发、忘了同步 gen_bpf.py，也不会再崩槽位。 */
static long vm_syscall6(long nr, long a, long b, long c, long d, long e, long f) {
    long act = vm_filter_action(nr, a, b, c, d, e, f);
    if (act != VM_FILTER_ALLOW) {
        static long warned_nr = -1;
        if (warned_nr != nr) {
            warned_nr = nr;
            vm_log("!! 拒绝直发 nr=%ld：过滤器判决 0x%lx（非 ALLOW）——放行清单缺项，"
                   "直发会二次 TRAP（SIGSYS 挂起/崩槽位）。已改为返回 -ENOSYS", nr, act);
        }
        return -VM_ENOSYS;
    }
#if defined(__x86_64__)
    long ret;
    register long r10 __asm__("r10") = d;
    register long r8  __asm__("r8")  = e;
    register long r9  __asm__("r9")  = f;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return ret;
#else
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    register long x4 __asm__("x4") = e;
    register long x5 __asm__("x5") = f;
    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
                     : "memory", "cc");
    return x0;
#endif
}
#define VM_PT5(nr,a,b,c,d,e)  vm_syscall6((nr),(long)(a),(long)(b),(long)(c),(long)(d),(long)(e),0)
#define VM_PT6(nr,a,b,c,d,e,f) vm_syscall6((nr),(long)(a),(long)(b),(long)(c),(long)(d),(long)(e),(long)(f))
#define VM_PT3(nr,a,b,c)      vm_syscall6((nr),(long)(a),(long)(b),(long)(c),0,0,0)
#define VM_PT4(nr,a,b,c,d)    vm_syscall6((nr),(long)(a),(long)(b),(long)(c),(long)(d),0,0)

#define VM_ENOTTY 25
#define VM_ENODATA 61

int vm_console_fd = 2;   /* 默认 stderr；宿主启动后改指管道/.stdout */

/* 4.2：当前映像路径——/proc/self/exe 虚拟化的数据源（execve 时由后端设置） */
static char g_exec_path[VM_MAX_PATH + 1] = "";
void vm_set_exec_path(const char* p) { snprintf(g_exec_path, sizeof(g_exec_path), "%s", p); }
const char* vm_get_exec_path(void) { return g_exec_path; }

/* v1.2.36：exec_dyn 引擎栈登记（maps [stack]/stat startstack 改账的真相源，
 * 详见 vm_core.h 注释）。子进程（exec_dyn）写，父进程（syscall 处理）跨进程读。 */
VmExecStack g_exec_stack = {0, 0, 0};
void vm_set_exec_stack(unsigned long long lo, unsigned long long hi, unsigned long long sp) {
    g_exec_stack.lo = lo; g_exec_stack.hi = hi; g_exec_stack.sp = sp;
    vm_log("[exec-stack] 引擎栈登记: %llx-%llx 初始sp=%llx", lo, hi, sp);
}

/* 4.2：guest 的 cmdline（\0 分隔 argv）——/proc/self/cmdline 虚拟化数据源 */
/* v0.7.43: /proc/self/maps synthesis -------------------------------------
 * ART (Runtime init) reads /proc/self/maps: unwinder maps a PC to a file,
 * bionic pthread_getattr_np finds the thread stack bounds. With ENOENT it
 * went silent right after. We record every mapping the engine makes on the
 * guest's behalf (exe/linker/stack from loader, ROM virtual-fd mmaps, anonymous
 * mmaps) and render a Linux-style maps file. */
#define VM_MAX_MAPS 256
typedef struct { uintptr_t addr; size_t len; int prot; char path[128]; } VmMapEnt;
static VmMapEnt g_vmmaps[VM_MAX_MAPS];
static int g_vmmaps_n = 0;
VmBlackBox* vm_bb = 0;
void vm_bb_attach(void* p) { vm_bb = (VmBlackBox*)p; if (vm_bb) { vm_bb->seq = 0; vm_bb->idx = 0; } }
void vm_bb_pc(unsigned long pc) {
    if (vm_bb) { int i = vm_bb->idx - 1; if (i < 0) i = 31; vm_bb->pc[i] = pc; }
}

static uintptr_t g_guest_stack_base = 0;
static size_t    g_guest_stack_len  = 0;
void vm_set_guest_stack(uintptr_t base, size_t len) { g_guest_stack_base = base; g_guest_stack_len = len; }
uintptr_t vm_get_guest_stack_base(void) { return g_guest_stack_base; }

void vm_map_add(uintptr_t addr, size_t len, int prot, const char* path) {
    if (!addr || !len) return;
    int i;
    if (g_vmmaps_n < VM_MAX_MAPS) {
        i = g_vmmaps_n++;
    } else {
        /* v0.7.44：表满时优先淘汰**匿名项**（path 为空）。有名项（exe/linker/[stack]/各 .so）
         * 是 bionic pthread_getattr_np 与 ART unwinder 的命根子 ——
         * 真机实锤：128 条满后被挤出 → "stack not found in /proc/self/maps" → ART 静默卡死。 */
        i = -1;
        for (int k = 0; k < VM_MAX_MAPS; k++)
            if (!g_vmmaps[k].path[0]) { i = k; break; }
        if (i < 0) i = 0;
    }
    g_vmmaps[i].addr = addr & ~(uintptr_t)0xFFF;
    g_vmmaps[i].len = ((addr & 0xFFF) + len + 0xFFF) & ~(size_t)0xFFF;
    g_vmmaps[i].prot = prot;
    snprintf(g_vmmaps[i].path, sizeof(g_vmmaps[i].path), "%s", path ? path : "");
}
size_t vm_render_maps(char* out, size_t cap) {   /* v1.2.22：非 static —— 体检要用 */
    size_t o = 0;
    /* v0.7.44：兜底 —— 当前线程的栈必须在 maps 里。
     * bionic pthread_getattr_np 靠"哪条映射包含当前 SP"找线程栈边界，找不到就报
     * "stack not found in /proc/self/maps"（真机 2026-10-14 07:47 实锤）→ ART 卡死。
     * 这里按最近一次 TRAP 的 guest SP 补一条匿名栈区。 */
    {
        VmCpu* cc = vm_machine_current();
        uintptr_t sp = cc ? cc->guest_sp : 0;
        if (sp) {
            int have = 0;
            for (int k = 0; k < g_vmmaps_n; k++)
                if (g_vmmaps[k].len && sp >= g_vmmaps[k].addr
                    && sp < g_vmmaps[k].addr + g_vmmaps[k].len) { have = 1; break; }
            if (!have) {
                uintptr_t base = (sp & ~(uintptr_t)0xFFF) - (uintptr_t)(2 << 20);
                vm_map_add(base, (size_t)(3 << 20), 3 /*RW*/, "[stack]");
            }
        }
    }
    /* v0.7.61：非破坏性渲染。旧实现用 len=0 当"已输出"标记 → 渲染一次就抹掉
     * 整张表 → 第二次 openat("/proc/self/maps") 只剩兜底 [stack] 一行（52B）。
     * v0.7.60 真机实锤：ART abort 的 unwinder 读到 52B → "Failed to parse maps"，
     * 而且任何**第二次**读 maps 的组件都会拿到残废数据。改用 done[] 位图。 */
    int done[VM_MAX_MAPS];
    memset(done, 0, sizeof(done));
    for (int guard = 0; guard < VM_MAX_MAPS; guard++) {
        int best = -1;
        for (int i = 0; i < g_vmmaps_n; i++) {
            if (done[i] || !g_vmmaps[i].addr || !g_vmmaps[i].len) continue;
            if (best < 0 || g_vmmaps[i].addr < g_vmmaps[best].addr) best = i;
        }
        if (best < 0) break;
        done[best] = 1;
        VmMapEnt e = g_vmmaps[best];
        /* v0.7.63：perms 第 4 位必须是 'p'（private）——真内核 maps 的第 4 字符
         * 只有 p/s 两种。v0.7.62 真机实锤：libunwindstack（Android 16 已 Rust 化，
         * 严格解析）遇到 "rw--" 直接判 "Failed to parse maps" → abort dump 的
         * native 栈全灭（"Unwind failed for thread …"），CHECK 现场丢失。
         * 我们所有映射都是 MAP_PRIVATE → 一律 'p'。 */
        char perm[5] = "---p";
        perm[0] = (e.prot & 1) ? 'r' : '-';
        perm[1] = (e.prot & 2) ? 'w' : '-';
        perm[2] = (e.prot & 4) ? 'x' : '-';
        if (o + 160 >= cap) break;
        /* 空 path 不写尾随空格（严格解析器对 "0 \n" 这种尾巴敏感） */
        if (e.path[0])
            o += (size_t)snprintf(out + o, cap - o, "%lx-%lx %s 00000000 00:00 0 %s\n",
                                  (unsigned long)e.addr, (unsigned long)(e.addr + e.len),
                                  perm, e.path);
        else
            o += (size_t)snprintf(out + o, cap - o, "%lx-%lx %s 00000000 00:00 0\n",
                                  (unsigned long)e.addr, (unsigned long)(e.addr + e.len),
                                  perm);
    }
    if (o == 0)
        o = (size_t)snprintf(out, cap, "0-0 r--p 00000000 00:00 0 [empty]\n");
    return o;
}

static char g_exec_cmdline[512];
static int  g_exec_cmdline_len = 0;
void vm_set_exec_cmdline(char* const argv[]) {
    size_t off = 0;
    for (int i = 0; argv && argv[i] && off + 2 < sizeof(g_exec_cmdline); i++) {
        size_t l = strlen(argv[i]);
        if (off + l + 1 >= sizeof(g_exec_cmdline)) break;
        memcpy(g_exec_cmdline + off, argv[i], l + 1);
        off += l + 1;
    }
    g_exec_cmdline[off] = 0;
    g_exec_cmdline_len = (int)off;
}

/* ── 控制台（唯一放行的宿主出口） ──
 * v0.7.22：**控制台是文本通道，必须 NUL/控制字符安全**。
 * 真机实锤（2026-10-13 16:25 日志）：guest 把 logd 的 writev 负载（struct log_msg，
 * 4KB 里绝大部分是 0）从 fd 83 tee 到控制台，第一个 0x00 就被 JNI 侧
 * NewStringUTF 当成字符串结尾 —— 输出在 "[via fd 83, iovcnt=4] " 处**整段截断**，
 * 连 JNI 追加的 "guest 槽位退出: code=…（死于信号 …）" 尾巴一起吞掉，
 * 现场看起来就是"进程死在 writev 返回后、无下文"（作者注释里的 TRAP#469 断流之谜）。
 * 对策：NUL 直接丢弃（文本日志里它无信息量，且丢弃能把二进制结构塌缩成可读文本），
 * 其它 C0 控制字符（除 \t\n\r）与 DEL 折成 '.'。'\0' 之后的内容照常输出，
 * 因此退出码/致命信号尾巴一定能看到。 */
void vm_console_write(const char* data, size_t n) {
    size_t off = 0;
    char out[512];
    while (off < n) {
        size_t chunk = n - off > sizeof(out) ? sizeof(out) : n - off;
        size_t k = 0;
        for (size_t i = 0; i < chunk; i++) {
            unsigned char c = (unsigned char)data[off + i];
            if (c == 0) continue;                       /* NUL：丢弃（不能进 JNI 字符串） */
            if (c < 0x20 && c != '\n' && c != '\r' && c != '\t') c = '.';
            else if (c == 0x7F) c = '.';
            out[k++] = (char)c;
        }
        size_t woff = 0;
        while (woff < k) {
            ssize_t w = write(vm_console_fd, out + woff, k - woff);
            if (w <= 0) return;
            woff += (size_t)w;
        }
        off += chunk;
    }
}

void (*vm_live_log_hook)(const char* line, int len) = NULL;

void vm_diag(const char* fmt, ...) {
    char buf[320];
    int k = snprintf(buf, sizeof(buf), "[TwinVM] ");
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf + k, sizeof(buf) - (size_t)k - 1, fmt, ap);
    va_end(ap);
    size_t n = strlen(buf);
    if (n + 1 < sizeof(buf)) { buf[n] = '\n'; n++; }
    if (vm_live_log_hook) vm_live_log_hook(buf, (int)n);
    else vm_log("%s", buf + 9);   /* 兜底：钩子未注册时退回 console */
}

void vm_log(const char* fmt, ...) {
    char buf[640];
    int k = snprintf(buf, sizeof(buf), "[TwinVM] ");
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + k, sizeof(buf) - (size_t)k - 1, fmt, ap);
    va_end(ap);
    size_t n = strlen(buf);
    if (n + 1 < sizeof(buf)) { buf[n] = '\n'; n++; }
    vm_console_write(buf, n);
}

const char* vm_arch_name(void) {
#if defined(__x86_64__)
    return "x86_64";
#elif defined(__aarch64__)
    return "aarch64";
#endif
}

/* ── 内存桥 ──
 * v0.7.22：**guest 指针一律走 process_vm_*，不再 memcpy**。
 * 旧实现 in_process 时直接 memcpy：guest 给一个野指针（例：writev 的 iov[i].base
 * 落在未映射页）就是 handler 内 SIGSEGV —— 内核杀槽位，日志里只剩"writev 返回后
 * 无下文"。而 process_vm_readv/writev(自身 pid) 对非法地址只会返回 -EFAULT，
 * 调用方可以按错误处理（writev 的 [bad iov #N]/[iov#N 域非法] 分支这才真正生效）。
 * 代价：每次访存多一次系统调用（SIGSYS 本身已是信号往返，可接受）。
 * 注意：这两个 syscall 已在 gen_bpf.py 放行清单里（VM_ENGINE_SYSCALL_NRS）——
 * handler 内调用若被自家过滤器 TRAP，就是 v0.2.8 那个"二次 TRAP→挂起→迟到投递"死法。 */
int vm_mem_read(VmMem* m, void* dst, uintptr_t src, size_t n) {
    if (n == 0) return 0;
    struct iovec l = { dst, n }, r = { (void*)src, n };
    if (process_vm_readv(m->pid, &l, 1, &r, 1, 0) == (ssize_t)n) return 0;
    if (m->in_process && errno == ENOSYS) {          /* 极老内核没这个 syscall：退化为直拷 */
        static int warned;
        if (!warned) { warned = 1; vm_log("警告：process_vm_readv 不可用，退化为直拷（野指针会崩槽位）"); }
        memcpy(dst, (const void*)src, n);
        return 0;
    }
    return -1;
}
int vm_mem_write(VmMem* m, uintptr_t dst, const void* src, size_t n) {
    if (n == 0) return 0;
    struct iovec l = { (void*)src, n }, r = { (void*)dst, n };
    if (process_vm_writev(m->pid, &l, 1, &r, 1, 0) == (ssize_t)n) return 0;
    if (m->in_process && errno == ENOSYS) {
        static int warned;
        if (!warned) { warned = 1; vm_log("警告：process_vm_writev 不可用，退化为直拷（野指针会崩槽位）"); }
        memcpy((void*)dst, src, n);
        return 0;
    }
    return -1;
}

/* ── VFS ── */
void vm_vfs_init(VmVfs* vfs, const VmRom* rom) {
    memset(vfs, 0, sizeof(*vfs));
    vfs->rom = rom;
}

/* v1.2.1 ★ /apex 动态直读文件（rom_idx==-3）——ROM 没有的 art 库
 * （真机 v1.2.0 实锤：linker 找 libartbased.so，ROM/odm/vendor 全 ENOENT）。
 * 来源=宿主 /apex/com.android.art/（App 可读、同一台机同版本=同快照铁律满足）。
 * 父进程 syscall 处理时装载 → mmap-fill/引擎同一进程可见。 */
#define VM_DYN_BASE 64
#define VM_DYN_SLOTS 32
static uint8_t* g_dyn_data[VM_DYN_SLOTS];
static size_t    g_dyn_len[VM_DYN_SLOTS];

/* Stage 1：path 是文件（精确命中）还是目录（作为前缀命中） */
int vm_vfs_stat(const VmRom* rom, const char* path, uint32_t* mode, uint64_t* size) {
    size_t plen = strlen(path);
    const VmRomInode* hit = vm_rom_find(rom, path);
    if (hit) {
        if (mode) *mode = hit->mode;
        if (size) *size = hit->size;
        return 0;
    }
    /* 目录：存在以 "path/" 开头的 inode（或 path=="/"） */
    if (plen == 0) return -1;
    char pfx[VM_MAX_PATH + 2];
    if (plen >= VM_MAX_PATH) return -1;
    if (strcmp(path, "/") == 0) strcpy(pfx, "/");
    else { memcpy(pfx, path, plen); if (pfx[plen - 1] != '/') { pfx[plen] = '/'; pfx[plen + 1] = 0; } else pfx[plen] = 0; }
    size_t n = strlen(pfx);
    for (uint32_t i = 0; i < rom->inode_count; i++) {
        if (strncmp(rom->inodes[i].name, pfx, n) == 0) {
            if (mode) *mode = 0040555;  /* S_IFDIR|0555 */
            if (size) *size = 4096;
            return 1;
        }
    }
    return -1;
}

int vm_vfs_open(VmVfs* vfs, const char* path) {
    uint32_t mode; uint64_t size;
    int kind = vm_vfs_stat(vfs->rom, path, &mode, &size);
    if (kind < 0) return -VM_ENOENT;
    for (int i = 0; i < VM_MAX_FILES; i++) {
        if (!vfs->files[i].in_use) {
            vfs->files[i].in_use = 1;
            vfs->files[i].is_dir = (kind == 1);
            vfs->files[i].rom_idx = (kind == 0) ? (int)(vm_rom_find(vfs->rom, path) - vfs->rom->inodes) : 0;
            vfs->files[i].dir_next = 0;
            vfs->files[i].pos = 0;
            snprintf(vfs->files[i].dir_path, VM_MAX_PATH, "%s", path);
            return VM_FD_BASE + i;
        }
    }
    return -VM_ENOENT;
}

long vm_vfs_seek(VmVfs* vfs, int fd, long off, int whence) {
    int i = fd - VM_FD_BASE;
    if (i < 0 || i >= VM_MAX_FILES || !vfs->files[i].in_use) return -VM_EBADF;
    VmFile* f = &vfs->files[i];
    if (f->is_dir) return -29 /* ESPIPE */;
    int di3 = f->rom_idx == -3 ? i - VM_DYN_BASE : -1;   /* v1.2.1: 动态直读槽 */
    uint64_t size = di3 >= 0 ? g_dyn_len[di3]
                             : vfs->rom->inodes[f->rom_idx].size;
    long base = whence == 0 ? 0 : whence == 1 ? (long)f->pos : (long)size;
    if (base + off < 0) return -22;
    f->pos = (uint64_t)(base + off);
    return (long)f->pos;
}

/* 4.2：合成文件（/proc/self/cmdline 动态内容）——专用槽 60~63 */
#define VM_SYN_BASE 60
#define VM_SYN_SLOTS 6   /* v1.2.7: 4→6（maps+comm 同时开） */
static const uint8_t* g_syn_data[VM_SYN_SLOTS];
static int g_syn_len[VM_SYN_SLOTS];
/* v1.1.7：guest 显式关掉的 stdio fd 记账（/dev/null 的 POSIX 最低 fd 复用） */
static int g_stdio_closed[3];
/* v1.1.8：/proc/self/comm 合成缓冲（synthetic 存指针，须静态存活） */
static uint8_t g_syn_scratch[64];
/* v1.2.7：cpu 拓扑合成缓冲（与 comm 分开——synth 存指针不能互踩） */
static char g_cpu_scratch[32];
int vm_vfs_open_synthetic(VmVfs* vfs, const char* name, const uint8_t* data, int len) {
    for (int i = 0; i < VM_SYN_SLOTS; i++) {
        int slot = VM_SYN_BASE + i;
        if (!vfs->files[slot].in_use) {
            vfs->files[slot].in_use = 1;
            vfs->files[slot].is_dir = 0;
            vfs->files[slot].rom_idx = -2;
            snprintf(vfs->files[slot].dir_path, VM_MAX_PATH, "/proc/self/%s", name);
            vfs->files[slot].pos = 0;
            g_syn_data[i] = data; g_syn_len[i] = len;
            return VM_FD_BASE + slot;
        }
    }
    return -VM_ENOENT;
}

long vm_vfs_read(VmVfs* vfs, int fd, void* buf, size_t n) {
    int i = fd - VM_FD_BASE;
    if (i < 0 || i >= VM_MAX_FILES || !vfs->files[i].in_use) return -VM_EBADF;
    VmFile* f = &vfs->files[i];
    if (f->rom_idx == -2) {
        int si = i - VM_SYN_BASE;
        if (si < 0 || si >= VM_SYN_SLOTS || !g_syn_data[si]) return -VM_EBADF;
        if (f->pos >= (uint64_t)g_syn_len[si]) return 0;
        size_t want = (size_t)g_syn_len[si] - f->pos;
        if (n < want) want = n;
        memcpy(buf, g_syn_data[si] + f->pos, want);
        f->pos += want;
        return (long)want;
    }
    if (f->rom_idx == -3) {                     /* v1.2.1: /apex 动态直读 */
        int di3 = i - VM_DYN_BASE;
        if (di3 < 0 || di3 >= VM_DYN_SLOTS || !g_dyn_data[di3]) return -VM_EBADF;
        if (f->pos >= g_dyn_len[di3]) return 0;
        size_t want = g_dyn_len[di3] - f->pos;
        if (n < want) want = n;
        memcpy(buf, g_dyn_data[di3] + f->pos, want);
        f->pos += want;
        return (long)want;
    }
    const VmRomInode* ino = &vfs->rom->inodes[f->rom_idx];
    if (f->pos >= ino->size) return 0;
    size_t want = ino->size - f->pos;
    if (n < want) want = n;
    memcpy(buf, ino->map + f->pos, want);
    f->pos += want;
    return (long)want;
}

int vm_vfs_close(VmVfs* vfs, int fd) {
    int i = fd - VM_FD_BASE;
    /* v1.2.77 ★★★ ART init 后期 close 循环扫到虚拟号段（fd≥10000）：
     * 不在表里的虚拟 fd 回 EBADF → guest 判"没关掉"→ 重试 → **无限循环**
     * （真机实锤：close(10060) 重复出现，10 万次 close 占停点一半，
     *  ART 启动拖 172s+）。语义同 v1.1.3 真 fd 修复：guest 想关的任何 fd
     * 都"关成功了"——不在表里=本来就没人用=关了也无害。 */
    if (i < 0 || i >= VM_MAX_FILES) return 0;           /* 越界号段：假装成功 */
    if (!vfs->files[i].in_use) return 0;                /* 空槽：假装成功 */
    vfs->files[i].in_use = 0;
    return 0;
}

/* v1.2.0：ptrace 后端 mmap-fill 用——虚拟 fd → ROM inode 数据指针。
 * 父进程侧调用（ROM mmap 由 fork 继承共享，父进程读它天经地义）。 */
const uint8_t* vm_vfs_fd_data(const VmCpu* c, long fd, size_t* out_size) {
    int i = (int)fd - VM_FD_BASE;
    if (!c->vfs || i < 0 || i >= VM_MAX_FILES || !c->vfs->files[i].in_use)
        { if (out_size) *out_size = 0; return NULL; }
    if (c->vfs->files[i].is_dir)
        { if (out_size) *out_size = 0; return NULL; }
    if (c->vfs->files[i].rom_idx == -3) {       /* v1.2.1: /apex 动态直读 */
        int di3 = i - VM_DYN_BASE;
        if (di3 < 0 || di3 >= VM_DYN_SLOTS || !g_dyn_data[di3])
            { if (out_size) *out_size = 0; return NULL; }
        if (out_size) *out_size = g_dyn_len[di3];
        return g_dyn_data[di3];
    }
    if (c->vfs->files[i].rom_idx < 0)
        { if (out_size) *out_size = 0; return NULL; }
    const VmRomInode* ino = &c->vfs->rom->inodes[c->vfs->files[i].rom_idx];
    if (out_size) *out_size = ino->size;
    return ino->map;
}

/* ── 虚拟系统调用判决 ── */
static VmCpu g_cpu;
static VmVfs g_vfs;

VmCpu* vm_machine_current(void) { return &g_cpu; }

/* 宿主侧装配一台"机器"（Stage 0 单实例） */
static void vm_machine_init(const VmRom* rom, int pid, int in_process) {
    vm_vfs_init(&g_vfs, rom);
    g_cpu.vfs = &g_vfs;
    g_cpu.mem.pid = pid;
    g_cpu.mem.in_process = in_process;
    g_cpu.halt = 0;
    g_cpu.cycles = 0;
    vm_binder_early_init(&g_cpu);   /* fork 前预创建（后代共享） */
}

/* 对外：后端在装配后拿 CPU；ptrace 模式由宿主进程代行 */
VmCpu* vm_machine_setup(const VmRom* rom, int pid, int in_process) {
    vm_machine_init(rom, pid, in_process);
    return &g_cpu;
}

/* ═══════════ Stage 1：虚拟系统调用判决（四类大表） ═══════════
 * 虚拟化：openat/read/write/writev/close/lseek/getdents64/fstat/newfstatat（ROM VFS）
 * 伪造  ：uname/getpid 族/ioctl/getcwd/readlinkat（VM 身份，宿主不可见）
 * 透传  ：内存(brk/mmap 族)/futex/信号/clone/wait4/时钟/杂项（安全原语）
 * 拒绝  ：其余（ENOSYS + 限频日志） */
long vm_handle_syscall(VmCpu* c, long nr, long a0, long a1, long a2, long a3, long a4, long a5) {
    c->cycles++;
    if (vm_bb) {                       /* v0.7.50：共享黑匣子（内核 KILL 也带得走） */
        int bi = vm_bb->idx;
        vm_bb->nr[bi] = nr;
        vm_bb->a0[bi] = (unsigned long)a0;
        vm_bb->pc[bi] = 0;
        vm_bb->idx = (bi + 1) & 31;
        vm_bb->seq++;
    }
    if (c->nr_hist_n < 24) c->nr_hist[c->nr_hist_n++] = nr;   /* 崩溃取证用 */
    else { for (int i = 1; i < 24; i++) c->nr_hist[i-1] = c->nr_hist[i]; c->nr_hist[23] = nr; }

    switch (nr) {

    /* ── 虚拟化：文件 I/O 走 ROM VFS ── */
    case VM_NR_openat: {
        char path[VM_MAX_PATH + 1];
        if (vm_mem_read(&c->mem, path, (uintptr_t)a1, VM_MAX_PATH) != 0) return -VM_EACCES;
        path[VM_MAX_PATH] = 0;
        if (strcmp(path, "/proc/self/exe") == 0 && g_exec_path[0]) {
            /* v1.2.43 ★★★ openat 特判（v1.2.42 真机实锤：ENOENT）。
             * 内核语义：open("/proc/self/exe") 必须成功（返回自身 ELF 只读 fd）。
             * bionic linker get_executable_info → realpath("/proc/self/exe")
             * 解析失败 → result.path 停留字面 "/proc/self/exe" → ld.config 的
             * dir.system=/system/bin/ 段匹配不上 → 配置被弃用（no_config）→
             * g_exported_namespaces 恒空 → libnativebridge
             * "Failed to get system namespace" 断言（v1.2.40~42 三轮同源）。
             * 从 ROM 打开真身，realpath 即可解析出 /system/bin/dalvikvm。 */
            int erfd = vm_vfs_open(c->vfs, g_exec_path);
            vm_log("openat(\"/proc/self/exe\") -> fd=%d [ROM 真身 %s]", erfd, g_exec_path);
            if (erfd >= 0) return erfd;
            /* ROM 打开失败不拦截，走下面的常规路径（保持旧行为） */
        }
        /* v0.7.33：/proc/self/cmdline —— ART 读 argv（"Failed to load" 实锤）。
         * 合成 fd 直接返回内容（ROM cmdline 全局由 execve 设置）。 */
        if (strcmp(path, "/proc/self/cmdline") == 0 && g_exec_cmdline[0]) {
            int cfd = vm_vfs_open_synthetic(c->vfs, "cmdline",
                                            (const uint8_t*)g_exec_cmdline,
                                            g_exec_cmdline_len);
            vm_log("openat(\"/proc/self/cmdline\") -> fd=%d [合成 %dB]", cfd, g_exec_cmdline_len);
            return cfd;
        }
        /* v1.1.8：/proc/self/comm —— 线程名（v1.1.7 真机 Fatal signal 报告里
         * "tid 4525 (<name unknown>)" 实锤：async_safe 读 comm 拼线程名，
         * ENOENT 就叫 unknown）。取 exec basename（dalvikvm）。 */
        if (strcmp(path, "/proc/self/comm") == 0) {
            const char* base = g_exec_path[0] ? strrchr(g_exec_path, '/') : NULL;
            base = base ? base + 1 : (g_exec_path[0] ? g_exec_path : "guest");
            int n = snprintf((char*)g_syn_scratch, sizeof(g_syn_scratch), "%s\n", base);
            int mfd = vm_vfs_open_synthetic(c->vfs, "comm", g_syn_scratch, n);
            vm_log("openat(\"/proc/self/comm\") -> fd=%d [合成 %s]", mfd, base);
            return mfd;
        }
        /* v0.7.42：/proc/self/stat —— ART 初始化时读它做 CPU/线程统计
         * （真机日志 `couldn't open /proc/self/stat: No such file or directory`）。
         * 按 proc(5) 的经典 52 字段格式合成一行，值取合理占位。 */
        /* v1.2.7：/sys/devices/system/cpu/possible —— ART CPU 拓扑读取
         * （真机 v1.2.6 实锤：Runtime init 读它规划 GC 线程池；ENOENT 后
         * 接 stat/maps，随后挂起）。合成 "0-7\n"（8 核，OnePlus 保守值）。 */
        if (strcmp(path, "/sys/devices/system/cpu/possible") == 0
            || strcmp(path, "/sys/devices/system/cpu/present") == 0) {
            int n = snprintf(g_cpu_scratch, sizeof(g_cpu_scratch), "0-7\n");
            int yfd = vm_vfs_open_synthetic(c->vfs, "cpus", (const uint8_t*)g_cpu_scratch, n);
            vm_log("openat(\"%s\") -> fd=%d [合成 0-7]", path, yfd);
            return yfd;
        }
        if (strcmp(path, "/proc/self/maps") == 0) {
            /* v1.2.7 ★★★ maps 合成换真身直读：旧方案从 g_vmmaps 记账表渲染
             * （exec_dyn 初始映射），但 ptrace 后端的库映射全走 tracer 的
             * mmap-fill——引擎侧一个都记不到（真机 v1.2.6 实锤：synth 34B
             * "0 maps"，教训五第三实例）。guest 有真 pid，直接 cat 真身。
             * v1.2.15 ★★★ 综合修复（真机 v1.2.14 通读判读）：
             * ① 根因＝缓冲 200KB 截断——真机实锤读到 204799=200*1024-1，
             *    [stack] 行按地址序在 maps 末尾（栈最高），恰好被切；
             *    guest 读时无 [stack] → bionic "stack not found" → abort。
             *    （事后 tracer 流式落盘的 maps 有 [stack]——"事后照片"失真
             *    误导了三轮排查。）
             * ② 缓冲扩到 1MB（guest 实测 ~200KB，1MB 余量充足）；
             * ③ 读满检测：len 达容量即打日志（永不无声截断）；
             * ④ 保底：读后若无 [stack] 行，用 stat 第 28 格 startstack 合成
             *    一行 append（"ss页-ss页+1 rw-p … [stack]"——bionic 只需要
             *    存在包含 startstack 的区间，一页即语义正确）。 */
            static char mbuf[1024 * 1024];
            char rpath[40];
            snprintf(rpath, sizeof(rpath), "/proc/%d/maps", (int)c->mem.pid);
            size_t ml = 0;
            int rmf = open(rpath, O_RDONLY);
            if (rmf >= 0) {
                ssize_t rn;
                while (ml + 1024 < sizeof(mbuf)
                       && (rn = read(rmf, mbuf + ml, sizeof(mbuf) - ml - 1)) > 0)
                    ml += (size_t)rn;
                close(rmf);
                if (ml + 1024 >= sizeof(mbuf))
                    vm_log("[maps] !! 读满 1MB 缓冲（len=%zu）——仍可能截断！", ml);
            }
            if (ml == 0) {                 /* 真身读失败 → 退回记账表（老路径） */
                ml = vm_render_maps(mbuf, sizeof(mbuf));
            }
            /* v1.2.36 ★★★ [stack] 改账（引擎栈为真相源）：
             * 内核 [stack] 行描述的是 fork 留下的原生栈——线程实跑在
             * exec_dyn 的引擎 mmap 栈上（真机 v1.2.35 CHECK 实锤）。
             * bionic 公式（android16-release pthread_attr.cpp 源码核对）：
             *   stack_base = [含 startstack 的 maps 区间].hi − RLIMIT_STACK(8MB)
             * → 删内核 [stack] 行、追加引擎栈行；stat 第 28 格同步改写（[stat-exec]）。
             * 引擎栈三要素在子进程（exec_dyn 侧）——跨进程读：
             * fork 无 exec → 父子同 .so 同地址（教训五第二击同款）。 */
            VmExecStack es;
            int es_ok = (vm_mem_read(&c->mem, &es, (uintptr_t)&g_exec_stack, sizeof(es)) == 0
                         && es.lo != 0 && es.hi > es.lo);
            if (ml && es_ok) {
                char* rr = mbuf;                 /* in-place 删除内核 [stack] 行 */
                char* ww = mbuf;
                char* ee = mbuf + ml;
                while (rr < ee) {
                    char* nl = memchr(rr, '\n', (size_t)(ee - rr));
                    nl = nl ? nl + 1 : ee;
                    size_t ll = (size_t)(nl - rr);
                    if (!(ll >= 8 && memmem(rr, ll, "[stack]\n", 8))) {
                        memmove(ww, rr, ll);
                        ww += ll;
                    }
                    rr = nl;
                }
                ml = (size_t)(ww - mbuf);
                if (ml + 128 < sizeof(mbuf)) {
                    int al = snprintf(mbuf + ml, 128,
                                      "%llx-%llx rw-p 00000000 00:00 0\t\t\t\t[stack]\n",
                                      es.lo, es.hi);
                    if (al > 0) {
                        ml += (size_t)al;
                        vm_log("[stack-exec] ★ maps [stack] 改账：引擎栈 %llx-%llx（内核行已删）",
                               es.lo, es.hi);
                    }
                }
            }
            /* [stack] 行保底：无则用 startstack 合成 */
            if (ml && !memmem(mbuf, ml, "[stack]", 7)) {
                char spath[40];
                unsigned long ss_now = 0;
                snprintf(spath, sizeof(spath), "/proc/%d/stat", (int)c->mem.pid);
                int ssf = open(spath, O_RDONLY);
                if (ssf >= 0) {
                    char sbuf[768];
                    ssize_t sn = read(ssf, sbuf, sizeof(sbuf) - 1);
                    close(ssf);
                    if (sn > 0) {
                        sbuf[sn] = 0;
                        char* p = strrchr(sbuf, ')');
                        int tk = 0;
                        while (p && *p && tk < 26) {
                            p++;
                            while (*p && *p == ' ') p++;
                            if (*p) tk++;
                            while (*p && *p != ' ') p++;
                        }
                        if (p) {
                            char* f = p;
                            while (f > sbuf && *(f-1) != ' ') f--;
                            ss_now = strtoul(f, NULL, 10);
                        }
                    }
                }
                if (ss_now) {
                    int al = snprintf(mbuf + ml, sizeof(mbuf) - ml,
                                      "%lx-%lx rw-p 00000000 00:00 0   [stack]\n",
                                      ss_now & ~0xFFFUL, (ss_now & ~0xFFFUL) + 0x1000);
                    if (al > 0) {
                        ml += (size_t)al;
                        vm_log("[stack-synth] ★ maps 无 [stack] 行 → 用 startstack=0x%lx 合成一页",
                               ss_now);
                    }
                } else {
                    vm_log("[stack-synth] !! maps 无 [stack] 且 stat 读不到 startstack（len=%zu）", ml);
                }
            }
            /* v1.2.14 ★★ [stack] 行读时快照 + 保底钳制：
             * 真机 v1.2.13 实锤——startstack=0x7ffd769d30（真值）且在 abort 后
             * 落盘的 [stack]（7ffcf6d000-7ffd76c000，已撑满 8MB）区间内，
             * 但 bionic 读 maps 那一刻的 [stack] 可能还没长到那里（abort 处理
             * 才把它撑大）→ 读时不含 startstack → "stack not found"。
             * 做法：①读时快照 [stack] 行落日志（对账）；②从 stat token 走
             * 第 28 格拿 startstack，若 [stack] 行不含它 → 把 lo 钳到
             * startstack 所在页（保底语义成立：startstack 按定义在栈里）。 */
            {
                /* 取 startstack（token 走，与 stat-fix 同款定位） */
                char spath[40];
                unsigned long ss_now = 0;
                snprintf(spath, sizeof(spath), "/proc/%d/stat", (int)c->mem.pid);
                int ssf = open(spath, O_RDONLY);
                if (ssf >= 0) {
                    char sbuf[768];
                    ssize_t sn = read(ssf, sbuf, sizeof(sbuf) - 1);
                    close(ssf);
                    if (sn > 0) {
                        sbuf[sn] = 0;
                        char* p = strrchr(sbuf, ')');
                        int tk = 0;
                        while (p && *p && tk < 26) {
                            p++;
                            while (*p && *p == ' ') p++;
                            if (*p) tk++;
                            while (*p && *p != ' ') p++;
                        }
                        if (p) {
                            char* f = p;
                            while (f > sbuf && *(f-1) != ' ') f--;
                            ss_now = strtoul(f, NULL, 10);
                        }
                    }
                }
                /* 找 [stack] 行，快照 + 钳制 */
                char* sl = ml ? memmem(mbuf, ml, "[stack]", 7) : NULL;
                if (sl) {
                    char* ln_begin = sl;
                    while (ln_begin > mbuf && *(ln_begin-1) != '\n') ln_begin--;
                    unsigned long lo = strtoul(ln_begin, NULL, 16);
                    char* dash = strchr(ln_begin, '-');
                    unsigned long hi = dash ? strtoul(dash + 1, NULL, 16) : 0;
                    vm_log("[stack-snap] 读时 guest 视角: %s（%.48s）startstack=0x%lx",
                           "见下", ln_begin, ss_now);
                    if (ss_now && lo && hi && !es_ok && (ss_now < lo || ss_now > hi)) {
                        /* 钳制：把 lo 改写为 ss_now 所在页（保位数等长替换）
                         * v1.2.36：es_ok（引擎栈改账）时禁用——ss_now 读的是
                         * 内核 stat 真值，会误伤引擎栈行。 */
                        unsigned long nlo = ss_now & ~0xFFFUL;
                        char nlo_s[32];
                        int nlo_l = snprintf(nlo_s, sizeof(nlo_s), "%lx", nlo);
                        int lo_l = (int)(dash - ln_begin);
                        if (nlo_l <= lo_l) {
                            memset(ln_begin, '0', (size_t)lo_l);
                            memcpy(ln_begin + (lo_l - nlo_l), nlo_s, (size_t)nlo_l);
                            vm_log("[stack-clamp] ★ 读时区间 [%lx,%lx] 不含 startstack=0x%lx → lo 钳到 0x%lx",
                                   lo, hi, ss_now, nlo);
                        }
                    }
                } else {
                    vm_log("[stack-snap] 读时 maps 无 [stack] 行（len=%zu ss=0x%lx）", ml, ss_now);
                }
            }
            int cfd = vm_vfs_open_synthetic(c->vfs, "maps", (const uint8_t*)mbuf, (int)ml);
            vm_log("openat(\"/proc/self/maps\") -> fd=%d [synth %dB, %d maps]",
                   cfd, (int)ml, g_vmmaps_n);
            return cfd;
        }
        if (strcmp(path, "/proc/self/stat") == 0) {
            /* v0.7.45：bionic 的 __find_main_stack_limits() 用固定格式解析本文件：
             *   " %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %*u %*u %*d %*d %*d
             *    %*d %*d %*d %*u %*u %*d %*u %*u %*u %lu"
             * 它要的是**第 28 字段 startstack（主栈起点）**，随后拿它去 /proc/self/maps 里
             * 找包含它的区间当线程栈边界。以前该字段是 0 → 永远
             * "stack not found in /proc/self/maps"（真机 07:47/07:56 两次实锤）。
             * 字段顺序按 proc(5)：1 pid, 2 (comm), 3 state, 4 ppid, 5 pgrp, 6 session,
             * 7 tty_nr, 8 tpgid, 9 flags, 10..15 minflt..stime, 16..21 cutime..itrealvalue,
             * 22 starttime, 23 vsize, 24 rss, 25 rsslim, 26 startcode, 27 endcode,
             * 28 startstack, 29 kstkesp, 30 kstkeip, 其余占位。 */
            static char stbuf[768];
            /* v1.2.8 ★★★ stat 真身直读（教训五第四击）：startstack 必须落在
             * 真身 maps 的某个区间里，否则 bionic "stack not found in
             * /proc/self/maps" → fatal（真机 v1.2.7 实锤：startstack=0x0 死
             * 于此）。旧合成的 g_guest_stack_base 由**子进程**的 loader 设置，
             * 父进程（syscall 处理方）恒零。guest 有真 pid——直接读真身，
             * startstack/startcode 等字段天然正确。 */
            {
                char rpath[40];
                snprintf(rpath, sizeof(rpath), "/proc/%d/stat", (int)c->mem.pid);
                int rsf = open(rpath, O_RDONLY);
                if (rsf >= 0) {
                    ssize_t rn = read(rsf, stbuf, sizeof(stbuf) - 1);
                    close(rsf);
                    if (rn > 0) {
                        stbuf[rn] = 0;
                        /* 提 startstack 打日志（第 28 字段：proc(5)
                         * 1pid 2comm 3state 4pgrp 5session 6tty_nr 7tpgid
                         * 8flags 9-15minflt.. 16-21 22-27 28startstack）*/
                        unsigned long ss = 0;
                        /* v1.2.13 ★ 终极修正（大白话）：stat 第 28 个数字才是
                         * 栈地址。v1.2.9 少数一格（读 27=endcode），v1.2.12
                         * 号称改回又少数一格（还是 27）。本次用 26/27/28=
                         * 111/222/333 假数据本机考试通过：此串读出 333。
                         * 格子账：pid(1) comm(2) state(3) | 4-8 五格 | 9-15
                         * 七格 | 16-21 六格 | 22-27 六格 | %lu=第 28 格。 */
                        sscanf(stbuf, "%*d %*s %*c %*d %*d %*d %*d %*d %*u %*u "
                               "%*u %*u %*u %*u %*u %*d %*d %*d %*d %*d %*d "
                               "%*u %*u %*u %*u %*u %*u %lu", &ss);
                        /* v1.2.36 ★★★ startstack 改账：指向引擎栈初始 sp。
                         * 与 maps [stack] 改账配套（同真相源 g_exec_stack）——
                         * bionic __find_main_stack_limits 拿它去 maps 里找区间，
                         * 两处一致才让 pthread_getattr_np 报出引擎栈边界。 */
                        VmExecStack es2;
                        int es_ok = (vm_mem_read(&c->mem, &es2, (uintptr_t)&g_exec_stack,
                                                 sizeof(es2)) == 0
                                     && es2.lo != 0 && es2.hi > es2.lo);
                        if (es_ok) {
                            char* p28 = strchr(stbuf, ')');
                            int tk = 0;
                            while (p28 && *p28 && tk < 26) {
                                p28++;
                                while (*p28 && *p28 == ' ') p28++;
                                if (*p28 && *p28 != ' ') tk++;
                                while (*p28 && *p28 != ' ') p28++;
                            }
                            if (p28) {
                                char* fstart = p28;
                                while (fstart > stbuf && *(fstart-1) != ' ') fstart--;
                                char num[32];
                                int nl2 = snprintf(num, sizeof(num), "%llu", es2.sp);
                                int fl2 = (int)(p28 - fstart);
                                if (fl2 > 0 && nl2 <= fl2) {
                                    memcpy(fstart, num, (size_t)nl2);
                                    memset(fstart + nl2, ' ', (size_t)(fl2 - nl2));
                                    ss = (unsigned long)es2.sp;
                                    vm_log("[stat-exec] ★ startstack 改账 → 引擎栈初始 sp=0x%llx（等宽）",
                                           es2.sp);
                                } else if (fl2 > 0 && nl2 > fl2) {
                                    /* 位数变长 → 后移尾部（缓冲 768B 余量足） */
                                    size_t tail = strlen(p28);
                                    size_t delta = (size_t)(nl2 - fl2);
                                    if ((size_t)rn + delta + 2 < sizeof(stbuf)) {
                                        memmove(p28 + delta, p28, tail + 1);
                                        memcpy(fstart, num, (size_t)nl2);
                                        rn += (ssize_t)delta;
                                        ss = (unsigned long)es2.sp;
                                        vm_log("[stat-exec] ★ startstack 改账 → 0x%llx（变长+%zu）",
                                               es2.sp, delta);
                                    }
                                }
                            }
                        }
                        /* v1.2.9 ★ startstack=0 修补：fork-from-thread 的子进程
                         * 该字段可能为 0（kernel 边角），bionic "stack not found"
                         * → fatal。以真身 maps 的 [stack] 行起点替换 stat 里
                         * 第 28 字段（token 替换，保长度合法）。 */
                        if (ss == 0) {
                            char mpath2[40];
                            snprintf(mpath2, sizeof(mpath2), "/proc/%d/maps", (int)c->mem.pid);
                            FILE* msf = fopen(mpath2, "r");
                            if (msf) {
                                char mln[256];
                                unsigned long slo = 0;
                                while (fgets(mln, sizeof(mln), msf)) {
                                    if (strstr(mln, "[stack]")) {
                                        slo = strtoul(mln, NULL, 16);
                                        break;
                                    }
                                }
                                fclose(msf);
                                if (slo) {
                                    /* 定位第 28 字段：')' 后第 26 个 token
                                     *（')' 后序列是 state(3), pgrp(4)…startstack(28)
                                     *  —— 本机自测实锤差一：25 会取到 endcode） */
                                    char* p28 = strchr(stbuf, ')');
                                    int tk = 0;
                                    while (p28 && *p28 && tk < 26) {
                                        p28++;
                                        while (*p28 && *p28 == ' ') p28++;
                                        if (*p28 && *p28 != ' ') tk++;
                                        while (*p28 && *p28 != ' ') p28++;
                                    }
                                    /* p28 停在第 25 个 token 尾（=第 28 字段尾），
                                     * 回退到字段首 */
                                    if (p28) {
                                        char* fstart = p28;
                                        while (fstart > stbuf && *(fstart-1) != ' ') fstart--;
                                        char num[32];
                                        int nl = snprintf(num, sizeof(num), "%lu", slo);
                                        int fl = (int)(p28 - fstart);
                                        if (fl > 0 && nl <= fl) {
                                            memcpy(fstart, num, (size_t)nl);
                                            memset(fstart + nl, ' ', (size_t)(fl - nl));
                                            ss = slo;
                                            vm_log("[stat-fix] startstack=0 → 用 [stack] 起点 0x%lx 修补", slo);
                                        }
                                    }
                                }
                            }
                        }
                        vm_log("[stat-telemetry] 真身 %zdB: %.120s", rn, stbuf);
                        vm_log("[stat-telemetry] 解析 startstack=0x%lx（若 0 看原文第 28 字段）", ss);
                        int cfd = vm_vfs_open_synthetic(c->vfs, "stat",
                                                        (const uint8_t*)stbuf, (int)rn);
                        vm_log("openat(\"/proc/self/stat\") -> fd=%d [真身 %zdB, startstack=0x%lx]",
                               cfd, rn, ss);
                        return cfd;
                    }
                }
            }
            uintptr_t sb = g_guest_stack_base;
            if (!sb) {
                VmCpu* cc2 = vm_machine_current();
                sb = cc2 ? (cc2->guest_sp & ~(uintptr_t)0xFFF) : 0x7f0000000000UL;
            }
            const char* comm2 = g_exec_cmdline[0] ? g_exec_cmdline : "dalvikvm";
            int n = snprintf(stbuf, sizeof(stbuf),
                "%d (%s) R %d %d %d %d %d %lu %lu %lu %lu %lu %lu %lu %ld %ld %ld %ld %ld %ld "
                "%lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu "
                "%lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu\n",
                (int)getpid(), comm2,
                1, 1, 1, 0, -1,
                4194304UL,
                0UL, 0UL, 0UL, 0UL, 0UL, 0UL,
                0L, 0L, 0L, 0L, 0L, 0L,
                0UL, 0UL, 0UL,
                (unsigned long)(VM_MAX_MAPS * 4096UL),
                0UL, 0UL,
                (unsigned long)sb,
                0UL, 0UL,
                0UL, 0UL, 0UL, 0UL, 0UL, 0UL, 0UL, 0UL, 0UL, 0UL,
                0UL, 0UL, 0UL, 0UL, 0UL, 0UL, 0UL, 0UL, 0UL, 0UL,
                0UL, 0UL, 0UL, 0UL, 0UL, 0UL);
            if (n > 0) {
                int cfd = vm_vfs_open_synthetic(c->vfs, "stat", (const uint8_t*)stbuf, n);
                vm_log("openat(\"/proc/self/stat\") -> fd=%d [synth %dB, startstack=0x%lx]",
                       cfd, n, (unsigned long)sb);
                return cfd;
            }
        }
        /* v1.1.6 ★★ 字符设备虚拟化：bionic/debuggerd 会 open("/dev/null")
         * （O_RDWR）重定向输出——真机 v1.1.5 实锤被下面的只读守卫挡成 EACCES，
         * 日志末行 "libc: failed to open /dev/null: Permission denied" 后卡死。
         * v1.1.7 ★★★ POSIX 最低 fd 语义：debuggerd 伪线程 close(0..1023) 后
         * open("/dev/null") 要求拿回 fd 0（反汇编 0x10153c：fd!=0 →
         * fatal "expected /dev/null fd to be 0"；v1.1.6 真机拿到 10060 死于此）。
         * guest 关过的 stdio fd 按最低优先复用；write 走 console-tee
         * （tombstone 文本进日志），read=EOF（read 分支 fd 0 已是 EOF）。 */
        if (strcmp(path, "/dev/null") == 0 || strcmp(path, "/dev/zero") == 0) {
            for (int want = 0; want < 3; want++) {
                if (g_stdio_closed[want]) {
                    g_stdio_closed[want] = 0;
                    vm_log("openat(\"%s\") -> fd=%d [POSIX 最低 fd 复用]", path, want);
                    return want;
                }
            }
            int dfd = vm_vfs_open_synthetic(c->vfs, "devnull", (const uint8_t*)"", 0);
            vm_log("openat(\"%s\") -> fd=%d [虚拟字符设备]", path, dfd);
            return dfd;
        }
        if ((a2 & 3) != 0 && (a2 & /*O_DIRECTORY*/ 0200000) == 0
            && strcmp(path, "/dev/binder") != 0) {   /* Stage 3：binder 虚拟设备放行（fd 是虚拟句柄，不落宿主） */
            vm_log("openat(\"%s\") -> -EACCES (rdonly fs)", path);
            return -VM_EACCES;
        }
        if (strcmp(path, "/dev/binder") == 0) {   /* Stage 3：虚拟 binder 设备 */
            int bfd = vm_binder_open(c);
            vm_log("openat(\"/dev/binder\") -> fd=%d [虚拟 binder 驱动]", bfd);
            return bfd;
        }
        /* 4.2：裸 soname（如 "libart.so"——JniInvocation 的属性默认值）→
         * 自动补 /system/lib64/ 前缀再查 ROM（属性服务缺位的兜底路径） */
        if (strncmp(path, "lib", 3) == 0 && strstr(path, ".so")
            && !strstr(path, "/")) {
            char full[VM_MAX_PATH + 1];
            snprintf(full, sizeof(full), "/system/lib64/%s", path);
            int rfd = vm_vfs_open(c->vfs, full);
            if (rfd >= 0) {
                vm_log("openat(\"%s\") → fd=%d [ROM soname 补径]", path, rfd);
                return rfd;
            }
        }
        int fd = vm_vfs_open(c->vfs, path);
        if (fd < 0) {
            /* v1.2.1 ★ /apex/com.android.art/ 白名单直读宿主——ROM 缺的 art 库
             * （libartbased.so 等新拆分库）从设备真身取：同一台机同版本=同快照。
             * 读全量到动态槽（rom_idx=-3），read/seek/fstat/mmap-fill 全走 VFS。
             * v1.2.2 ★★ 同名回退：ROM 的 ld.config 搜索路径是 /system:/odm:/vendor
             * （项目合成版），linker 永远不会请求 /apex——但真机上 /system/lib64
             * 的 art 库本就是 /apex/com.android.art/lib64 的硬链接（Android 10+
             * 标准布局）。/system/lib64/<lib> miss → 自动映射 /apex 同名试宿主。 */
            char apex_path[VM_MAX_PATH + 1];
            const char* try_apex = NULL;
            if (strncmp(path, "/apex/com.android.art/", 22) == 0) try_apex = path;
            else if (strncmp(path, "/system/lib64/", 15) == 0) {
                snprintf(apex_path, sizeof(apex_path),
                         "/apex/com.android.art/lib64/%s", path + 15);
                try_apex = apex_path;
            }
            if (try_apex) {
                int hfd = open(try_apex, O_RDONLY | O_CLOEXEC);
                if (hfd >= 0) {
                    struct stat hst;
                    if (fstat(hfd, &hst) == 0 && hst.st_size > 0
                        && hst.st_size < (64L << 20)) {
                        size_t want = (size_t)hst.st_size;
                        for (int di = 0; di < VM_DYN_SLOTS; di++) {
                            int slot = VM_DYN_BASE + di;
                            if (c->vfs->files[slot].in_use) continue;
                            uint8_t* buf = (uint8_t*)malloc(want);
                            if (!buf) break;
                            size_t got = 0;
                            while (got < want) {
                                ssize_t rn = read(hfd, buf + got, want - got);
                                if (rn <= 0) break;
                                got += (size_t)rn;
                            }
                            if (got != want) { free(buf); break; }
                            close(hfd);
                            g_dyn_data[di] = buf;
                            g_dyn_len[di] = want;
                            c->vfs->files[slot].in_use = 1;
                            c->vfs->files[slot].is_dir = 0;
                            c->vfs->files[slot].rom_idx = -3;
                            c->vfs->files[slot].pos = 0;
                            snprintf(c->vfs->files[slot].dir_path, VM_MAX_PATH, "%s", path);
                            vm_log("openat(\"%s\") -> fd=%d [/apex 直读 %zuB ← %s]", path,
                                   VM_FD_BASE + slot, want, try_apex);
                            return VM_FD_BASE + slot;
                        }
                        if (hfd >= 0) close(hfd);
                    } else if (hfd >= 0) close(hfd);
                }
            }
            vm_log("openat(\"%s\") -> ENOENT (宿主文件系统不可见——隔离生效)", path);
            return -VM_ENOENT;
        }
        vm_log("openat(\"%s\") -> fd=%d [ROM]", path, fd);
        return fd;
    }
    case VM_NR_read: {
        if (a0 == 0) return 0;                        /* stdin：EOF（VM 无输入源） */
        /* v0.8.5 ★ 真 fd 守卫：guest 只能读 stdin(0) 与虚拟 fd(≥10000)。
         * 真机 v0.8.4 实锤：ART 拿到**宿主的 fd=100** 后 fstat + read 反复读，
         * 既是对宿主的隔离泄漏，也会让它读到不该看的内容而进入异常路径
         * （现场最后是 futex 停住、无后续 syscall）。
         * 这里对 3..9999 的真 fd 一律回 -EBADF（Linux 语义：该 fd 在 guest 眼里不存在）。 */
        if (a0 > 0 && a0 < VM_FD_BASE) {
            /* v1.0.6 ★ 从 "返回 0(EOF)" 改为 **-EBADF**：
             * 真机 v1.0.5 实锤 —— ART 对 `read(fd=99)` 连续调用 8 次以上反复重试
             * （因为返回 0 被它理解成"暂时没数据，稍后再读"）→ 最终 futex 死等。
             * 而 **EBADF 是"这个 fd 不存在"的明确信号**，调用方会直接放弃该 fd
             * 并走退化路径 —— 这才是 guest 该看到的结果（它本来就不该持有宿主 fd）。 */
            /* v1.0.7：**每次重试都记**（但限流：同一 fd 前 8 次），便于确认
             * "ART 反复 read 同一个坏 fd"这个模式是否还在。 */
            static long s_fd[8]; static int s_n[8]; static int s_cnt = 0;
            int slot = -1;
            for (int q = 0; q < s_cnt; q++) if (s_fd[q] == a0) { slot = q; break; }
            if (slot < 0 && s_cnt < 8) { slot = s_cnt++; s_fd[slot] = a0; s_n[slot] = 0; }
            if (slot >= 0 && s_n[slot]++ < 8)
                vm_log("!! read(真 fd=%ld) → -EBADF（第 %d 次）：guest 不该看到宿主 fd", a0, s_n[slot]);
            return -VM_EBADF;
        }
        /* 4.0.1：真 fd 由 BPF 参数化原生放行（绝不在此直发——v0.5.0 真机实锤：
         * write(pipe) TRAP→handler 直发→二次 TRAP（SIGSYS 阻塞态）→code=-31 死）。
         * 能到这里的 read 只剩 stdio/虚拟 fd，走 VM 语义。 */
        char tmp[4096];
        size_t want = (size_t)a2 > sizeof(tmp) ? sizeof(tmp) : (size_t)a2;
        long n = vm_vfs_read(c->vfs, (int)a0, tmp, want);
        if (n == -VM_EBADF) return -VM_EBADF;
        if (n > 0 && vm_mem_write(&c->mem, (uintptr_t)a1, tmp, (size_t)n) != 0) return -VM_EACCES;
        /* v1.2.10 ★★ read 回读校验（v1.2.11 升级为全长比对：
         * 48B 比对有盲区——stat 的 startstack 在 ~170B、maps 的 [stack]
         * 在文件末尾，部分写入恰好漏网。真机 v1.2.10 实锤前 48B 全 ✓
         * 但 bionic 仍 stack not found——嫌疑集中到部分写入/块尾）。 */
        if (n > 0) {
            static int s_rr = 0;
            static int s_rr_bad = 0;
            char* rb = (char*)malloc((size_t)n);
            if (rb) {
                int ok = vm_mem_read(&c->mem, rb, (uintptr_t)a1, (size_t)n) == 0
                         && memcmp(rb, tmp, (size_t)n) == 0;
                if (ok) {
                    if (s_rr++ < 6) {
                        char hb[80]; int hk = 0;
                        for (size_t q = 0; q < (size_t)n && q < 60 && hk < 76; q++)
                            hk += snprintf(hb + hk, sizeof(hb) - (size_t)hk, "%c",
                                           (tmp[q] >= 0x20 && tmp[q] < 0x7f) ? tmp[q] : '.');
                        hb[hk] = 0;
                        vm_log("[read-verify] fd=%ld n=%ld ✓ 全长一致：%.60s", (long)a0, n, hb);
                    }
                } else {
                    if (s_rr_bad++ < 8) {
                        /* 定位第一个不一致偏移 */
                        size_t diff = 0;
                        while (diff < (size_t)n && rb[diff] == tmp[diff]) diff++;
                        char hb[96]; int hk = 0;
                        for (size_t q = diff; q < (size_t)n && q < diff + 40 && hk < 92; q++)
                            hk += snprintf(hb + hk, sizeof(hb) - (size_t)hk, "%c",
                                           (rb[q] >= 0x20 && rb[q] < 0x7f) ? rb[q] : '.');
                        hb[hk] = 0;
                        vm_log("[read-verify] fd=%ld n=%ld ✗✗✗ 首个差异@+%zu（期望:%02x 实得:%02x）：%.40s",
                               (long)a0, n, diff, (unsigned char)tmp[diff],
                               (unsigned char)rb[diff], hb);
                    }
                }
                free(rb);
            }
            /* stat 特化：完整落盘第 28 字段区域（含 startstack 的中段）。
             * v1.2.14：改内容识别触发（原 s_rr<=3 门闸被计数器挡住永不触发
             * ——stat 是第 5 个被 verify 的 read，那时 s_rr 已 ≥5）。 */
            if (a0 >= VM_FD_BASE) {
                const char* probe = strstr(tmp, " (");
                if (probe && (size_t)n > 200) {
                    /* 内容像 stat（"数字 (名字) 字母 …"且够长）→ 打中段 */
                    char sb2[180]; int sk = 0;
                    for (size_t q = 120; q < (size_t)n && q < 290 && sk < 176; q++)
                        sk += snprintf(sb2 + sk, sizeof(sb2) - (size_t)sk, "%c",
                                       (tmp[q] >= 0x20 && tmp[q] < 0x7f) ? tmp[q] : '.');
                    sb2[sk] = 0;
                    vm_log("[stat-mid] fd=%ld n=%ld 中段[120,290)：%s", (long)a0, n, sb2);
                }
                if (strstr(tmp, "[stack]"))
                    vm_log("[maps-hit] fd=%ld 本块含 [stack] 行（n=%ld）", (long)a0, n);
            }
        }
        return n;
    }
    case VM_NR_write: {
        /* v0.7.66：真 fd（3..9999）也 TRAP 进来——tee 到 console + 假装成功。
         * 病史：v0.7.65 真机 trace 实锤 ART 的 abort 原因行走 write(fd5=logd
         * socket)，BPF 原生放行 = 内容进黑洞。tee 后所有 logd 流量可见。
         * console 管道 fd 由 BPF CONSOLE 特判 ALLOW（安装期补丁）——
         * vm_console_write 的宿主侧 write 依旧原生通过，无二次 TRAP。 */
        if (a0 != 1 && a0 != 2 && (a0 < 0 || a0 >= VM_FD_BASE))
            vm_log("[via fd %d] ", (int)a0);
        size_t len = (size_t)a2, off = 0;
        char tmp[1024];
        while (off < len) {                            /* 分块循环：bionic 全缓冲一次可 4KB+ */
            size_t chunk = len - off > sizeof(tmp) ? sizeof(tmp) : len - off;
            if (vm_mem_read(&c->mem, tmp, (uintptr_t)(a1 + off), chunk) != 0) {
                if (a0 == 1 || a0 == 2) return -VM_EACCES;
                break;                                 /* 真 fd：读不动的 iov 跳过，仍假装成功 */
            }
            vm_console_write((const char*)tmp, chunk);
            off += chunk;
        }
        return (long)len;                              /* console 与真 fd 都假装写满 */
    }
    case VM_NR_writev: {
        /* v0.7.19：一切 fd 的 writev 内容 tee 到 console——ART 的错误文本
         * 写往 logd socket（真 fd），旧版 EBADF 让错误进了黑洞。
         * fd 1/2=console 语义；其它 fd=抄送+[via fd N] 标记，返回假装成功。
         * v0.7.20/21：iov 读失败防御——旧版 in_process 是 memcpy，野指针直接
         * SIGSEGV 崩槽位（"[bad iov]" 分支永远走不到），现场表现为"writev 返回后
         * 无下文"。现在 vm_mem_read 走 process_vm_readv：非法 iov 只会读失败，
         * 逐项跳过 + 打标记，绝不中断（也不崩）。 */
        long total = 0, teed = 0;
        int is_console = (a0 == 1 || a0 == 2);
        if (!is_console && a0 >= 0 && a0 < VM_FD_BASE)
            vm_log("[via fd %d, iovcnt=%d] ", (int)a0, (int)a2);
        for (int i = 0; i < (int)a2; i++) {
            struct { uintptr_t base; size_t len; } iov;
            if (vm_mem_read(&c->mem, &iov, (uintptr_t)(a1 + (size_t)i * sizeof(iov)), sizeof(iov)) != 0) {
                vm_log("[bad iov #%d @0x%llx]", i, (unsigned long long)(a1 + (size_t)i * sizeof(iov)));
                continue;
            }
            if (iov.len > (1u << 20)) { vm_log("[iov#%d len=%zu 拒绝]", i, iov.len); continue; }
            if (iov.len == 0) continue;
            char tmp[1024];
            size_t off = 0;
            while (off < iov.len) {
                size_t chunk = iov.len - off > sizeof(tmp) ? sizeof(tmp) : iov.len - off;
                if (vm_mem_read(&c->mem, tmp, iov.base + off, chunk) != 0) {
                    vm_log("[iov#%d 域非法 base=0x%llx off=%zu len=%zu]", i,
                           (unsigned long long)iov.base, off, iov.len);
                    break;
                }
                if (teed < (4u << 20)) {        /* tee 上限 4MB：防止怪异负载把日志刷爆 */
                    vm_console_write((const char*)tmp, chunk);
                    teed += (long)chunk;
                } else if (teed == (4L << 20)) {
                    vm_log("[tee 上限 4MB——后续 writev 负载不再抄送控制台]");
                    teed++;
                }
                off += chunk;
            }
            total += (long)iov.len;
        }
        return total;
    }
    case 282: /* userfaultfd ★ v1.2.84：恒拒（EPERM）——guest 的 uffd-heap
                 * 在 VM 里没有事件 handler，只会让构造函数陷入 mprotect
                 * 死循环（v1.2.80~83 四版真机定罪）。拒掉后 ART 自动走
                 * 无 uffd 的普通 heap 路径。（AArch64 nr=282，不是 x86 的 129；
                 * 129 是 kill——差点撞号，教训九续） */
        return -1;   /* EPERM */
    case VM_NR_close: {
        /* v1.1.7：guest 显式关掉的 stdio 记账——/dev/null 的 POSIX 最低 fd
         * 复用要用（debuggerd 伪线程 close(0..1023) 后 open("/dev/null")
         * 要求拿回 fd 0，反汇编 0x10153c 实锤：fd!=0 → fatal）。 */
        if (a0 == 0 || a0 == 1 || a0 == 2) { g_stdio_closed[a0] = 1; return 0; }
        /* v1.1.3 ★★★ 真机实锤：`close` 被拦 **446 次**（占全部拦截的 87%），
         * 而且每次都返回 `-EBADF(-9)` —— 因为 guest 关的是"真 fd"（宿主 fd 号段），
         * 虚拟表里没有 → vm_vfs_close 回 EBADF → **调用方失败后重试 → 死循环**
         * → 界面卡住 300 秒不动（用户实测）。
         *
         * 正确语义：**对 guest 而言，任何它想关的 fd 都"关成功了"** ——
         * 我们本来就是虚拟文件系统，fd 的生死由我们发号，guest 关哪个都无害。
         * 真 fd（3..VM_FD_BASE-1）一律直接返回 0，绝不返回 EBADF。
         * 这一条同时消除"close 风暴"和随之而来的卡死。 */
        if (a0 >= 3 && a0 < VM_FD_BASE) return 0;      /* 真 fd：假装关闭成功 */
        return vm_vfs_close(c->vfs, (int)a0);          /* 虚拟 fd：正常回收槽位 */
    }
    case VM_NR_lseek: {
        long r = vm_vfs_seek(c->vfs, (int)a0, a1, (int)a2);
        return r;
    }
    case VM_NR_getdents64: {
        int i = (int)a0 - VM_FD_BASE;
        if (i < 0 || i >= VM_MAX_FILES || !c->vfs->files[i].in_use) return -VM_EBADF;
        VmFile* f = &c->vfs->files[i];
        if (!f->is_dir) return -20 /* ENOTDIR */;
        size_t plen = strlen(f->dir_path);
        char pfx[VM_MAX_PATH + 2];
        if (plen == 1 && f->dir_path[0] == '/') strcpy(pfx, "/");
        else { memcpy(pfx, f->dir_path, plen); if (pfx[plen-1] != '/') { pfx[plen] = '/'; pfx[plen+1] = 0; } else pfx[plen] = 0; }
        size_t pfxlen = strlen(pfx);
        uint8_t* out = (uint8_t*)a1;
        size_t cap = (size_t)a2, used = 0;
        while (f->dir_next < c->vfs->rom->inode_count) {
            const VmRomInode* ino = &c->vfs->rom->inodes[f->dir_next];
            const char* nm = ino->name;
            if (strncmp(nm, pfx, pfxlen) != 0) { f->dir_next++; continue; }
            const char* rest = nm + pfxlen;                       /* 直接子项：余部不含 '/' */
            const char* slash = strchr(rest, '/');
            char entry[VM_MAX_PATH]; size_t elen;
            if (slash == NULL) {                                   /* 普通文件 */
                elen = strlen(rest);
                memcpy(entry, rest, elen + 1);
            } else if (slash == rest) continue;                    /* "xxx//" 畸形，跳过 */
            else {                                                 /* 目录：取 '/' 前段 */
                elen = (size_t)(slash - rest);
                memcpy(entry, rest, elen); entry[elen] = 0;
            }
            /* 同目录去重（多文件共前缀时目录只出一次） */
            static char last_entry[VM_MAX_PATH]; static size_t last_len = 0xFFFFFFFF;
            if (elen == last_len && memcmp(entry, last_entry, elen) == 0) { f->dir_next++; continue; }
            size_t reclen = (19 + elen + 1 + 7) & ~(size_t)7;      /* ino8+off8+len2+type1+name+NUL → 8 对齐 */
            if (used + reclen > cap) break;
            uint64_t d_ino = 1000 + f->dir_next;
            int64_t  d_off = (int64_t)(used + reclen);
            uint16_t u16 = (uint16_t)reclen;
            uint8_t  type = slash ? 4 /* DT_DIR */ : 8 /* DT_REG */;
            /* v0.7.22：先在本地拼好整条 dirent 再一次性写入 guest。
             * （vm_mem_write 现在是 process_vm_writev(一次系统调用)，逐字段/逐字节写
             *   会把一条目录项放大成 ten 几次 syscall——本地拼接后只写 1 次。） */
            uint8_t rec[19 + VM_MAX_PATH + 8];
            memset(rec, 0, reclen);
            memcpy(rec, &d_ino, 8);
            memcpy(rec + 8, &d_off, 8);
            memcpy(rec + 16, &u16, 2);
            rec[18] = type;
            memcpy(rec + 19, entry, elen + 1);
            if (vm_mem_write(&c->mem, (uintptr_t)(out + used), rec, reclen) != 0) return -VM_EACCES;
            if (elen != last_len || memcmp(entry, last_entry, elen) != 0) {
                memcpy(last_entry, entry, elen + 1); last_len = elen;
            }
            used += reclen;
            f->dir_next++;
        }
        return (long)used;
    }

    /* ── 伪造：stat 族（console=字符设备；VM fd=ROM inode） ── */
    case VM_NR_fstat:
    case VM_NR_newfstatat: {
        /* v0.8.5：fstat 的真 fd 同样守卫（guest 不该看到宿主 fd） */
        /* v0.8.5：真 fd 的 fstat —— **填充一个合法但空的 stat**（size=0, mode=0），
         * 而不是 EBADF：ART/loader 常把 EBADF 当成"严重错误"进而走异常路径。 */
        int tvm_real_fd = (nr == VM_NR_fstat && a0 > 0 && a0 < VM_FD_BASE);
        char path[VM_MAX_PATH + 1];
        if (nr == VM_NR_newfstatat) {
            if (vm_mem_read(&c->mem, path, (uintptr_t)a1, VM_MAX_PATH) != 0) return -VM_EACCES;
            path[VM_MAX_PATH] = 0;
        }
#if defined(__x86_64__)
        uint8_t st[144];   /* sizeof(struct stat) x86_64 */
#else
        uint8_t st[128];   /* aarch64 */
#endif
        memset(st, 0, sizeof(st));
#if defined(__x86_64__)
        const int off_mode = 20, off_size = 44, off_blksize = 52;   /* glibc x86_64 */
#else
        /* bionic aarch64 struct stat（kernel asm-generic）：
         * st_dev@0 st_ino@8 st_mode@16 st_nlink@20 st_uid@24 st_gid@28
         * st_rdev@32 __pad1@40 st_size@48 st_blksize@56 st_blocks@64
         * v0.7.10 修复：原 20/40/48 是 x86 想当然——size 写进 pad，linker
         * 读到 st_size=0 → "invalid shdr offset/size"（真机 .log 实锤） */
        const int off_mode = 16, off_size = 48, off_blksize = 56;
#endif
        uint32_t mode = 0020620 /* S_IFCHR|0620 */; uint64_t size = 0; uint32_t bsize = 4096;
        int kind = -1;
        if (nr == VM_NR_fstat) {
            if (a0 >= VM_FD_BASE && (int)(a0 - VM_FD_BASE) < VM_MAX_FILES
                && c->vfs->files[a0 - VM_FD_BASE].in_use) {
                if (c->vfs->files[a0 - VM_FD_BASE].rom_idx == -2) {
                int si = (int)(a0 - VM_FD_BASE) - VM_SYN_BASE;
                mode = 0100444;
                size = (uint64_t)(si >= 0 && si < VM_SYN_SLOTS ? g_syn_len[si] : 0);
                kind = 0;
            } else if (c->vfs->files[a0 - VM_FD_BASE].rom_idx == -3) {
                /* v1.2.1: /apex 动态直读——普通文件语义 */
                int di3 = (int)(a0 - VM_FD_BASE) - VM_DYN_BASE;
                mode = 0100555;
                size = (di3 >= 0 && di3 < VM_DYN_SLOTS) ? g_dyn_len[di3] : 0;
                kind = 0;
            } else
                kind = vm_vfs_stat(c->vfs->rom, c->vfs->files[a0 - VM_FD_BASE].dir_path, &mode, &size);
                if (kind == 0) mode |= 0100000; else mode = 0040555;
            }
        } else {
            /* v1.2.43 ★★★ /proc 目录链（realpath("/proc/self/exe") 逐组件需要）：
             * "/proc" 与 "/proc/<pid>" → S_IFDIR；"/proc/self" → lstat=S_IFLNK
             * （readlink 特判回 /proc/<pid>），stat（跟随）→ S_IFDIR。
             * 没有这条链，realpath 在第一跳 lstat("/proc") 就 ENOENT →
             * linker 拿不到主程序真身路径 → ld.config 段匹配失败（v1.2.42 实锤）。 */
            char pidpath[40];
            snprintf(pidpath, sizeof(pidpath), "/proc/%d", (int)c->mem.pid);
            if (strcmp(path, "/proc") == 0 || strcmp(path, pidpath) == 0) {
                mode = 0040755; size = 40; kind = 0;
            } else if (strcmp(path, "/proc/self") == 0) {
                if (a3 & 0x100UL) {          /* lstat：symlink 本体 */
                    mode = 0120777; size = strlen(pidpath);
                } else {                     /* stat：跟随链接 → 目录 */
                    mode = 0040755; size = 40;
                }
                kind = 0;
            } else if (g_exec_path[0] && (strcmp(path, "/proc/self/exe") == 0 ||
                        (strncmp(path, pidpath, strlen(pidpath)) == 0 &&
                         strcmp(path + strlen(pidpath), "/exe") == 0))) {
                /* v1.2.43 ★★★ lstat 语义（AT_SYMLINK_NOFOLLOW=0x100）：
                 * bionic realpath 逐组件 lstat 发现 S_IFLNK 才 readlink。
                 * 旧实现无条件返回文件 stat → realpath 以为不是链接 →
                 * 不 readlink → result.path 停留字面（v1.2.42 真机实锤）。
                 * symlink stat：mode=S_IFLNK|0777，size=目标路径长度。 */
                if (a3 & 0x100UL) {
                    mode = 0120777;
                    size = strlen(g_exec_path);
                    kind = 0;
                } else {
                    /* 4.2：当前映像——ROM 查真身（文件 stat 语义不变） */
                    kind = vm_vfs_stat(c->vfs->rom, g_exec_path, &mode, &size);
                    if (kind == 0) mode |= 0100000; else return -VM_ENOENT;
                }
            } else {
                kind = vm_vfs_stat(c->vfs->rom, path, &mode, &size);
                if (kind == 0) mode |= 0100000; else if (kind == 1) mode = 0040555;
                else if (strncmp(path, "/apex/com.android.art/", 22) == 0) {
                    /* v1.2.1: /apex 直读路径的 stat——宿主真 stat（不装内容） */
                    struct stat hst;
                    if (stat(path, &hst) == 0) {
                        mode = 0100555; size = (uint64_t)hst.st_size; kind = 0;
                    } else return -VM_ENOENT;
                }
                else if (strncmp(path, "/system/lib64/", 15) == 0) {
                    /* v1.2.2: /system/lib64 miss → /apex 同名回退 stat（与 openat 一致） */
                    char apex_path[VM_MAX_PATH + 1];
                    snprintf(apex_path, sizeof(apex_path),
                             "/apex/com.android.art/lib64/%s", path + 15);
                    struct stat hst;
                    if (stat(apex_path, &hst) == 0) {
                        mode = 0100555; size = (uint64_t)hst.st_size; kind = 0;
                    } else return -VM_ENOENT;
                }
                else return -VM_ENOENT;
            }
        }
        memcpy(st + off_mode, &mode, 4);
        memcpy(st + off_size, &size, 8);
        memcpy(st + off_blksize, &bsize, 4);
        if (tvm_real_fd) {   /* v0.8.5：真 fd → 空 stat（size/mode=0），避免 EBADF 触发异常路径 */
            static int fw = 0;
            if (!fw) { fw = 1; vm_log("!! fstat(真 fd=%ld) → 空 stat（隔离守卫）", a0); }
            return 0;
        }
        uintptr_t buf = (nr == VM_NR_fstat) ? (uintptr_t)a1 : (uintptr_t)a2;
        size_t stsz = sizeof(st);
        if (vm_mem_write(&c->mem, buf, st, stsz) != 0) return -VM_EACCES;
        return 0;
    }

    /* ── 伪造：VM 身份 ── */
    /* Stage 2.1：getpid/gettid/getppid 已 BPF 放行=真实 pid 直通内核
     * （多进程 guest 的 wait4 返回值与 getpid 必须一致——伪造 1 会导致
     * child 报 pid=1 而 wait 收到真实 pid 的撕裂）。以下 case 仅为 BPF
     * 回退到 TRAP 时的兜底，正常路径不再触达。 */
    case VM_NR_getpid:  return VM_PT3(VM_NR_getpid, 0, 0, 0);
    case VM_NR_gettid:  return VM_PT3(VM_NR_gettid, 0, 0, 0);
    case VM_NR_getppid: return VM_PT3(VM_NR_getppid, 0, 0, 0);
    case VM_NR_getuid: case VM_NR_geteuid: case VM_NR_getgid: case VM_NR_getegid:
        return 0;                                       /* VM 内 root */
    case 141: /* getpriority(arm64) —— v0.7.61：ART 线程 dump 读 nice 值
              * （v0.7.60 真机 ENOSYS nr=141(0x8d) 实锤）。语义：成功返回 nice 值
              * （20..19），失败 -1+errno。返回 0 = nice 0，标准默认。 */
        return 0;
    case VM_NR_ioctl: {
        int fi = (int)a0 - VM_FD_BASE;
        if (fi >= 0 && fi < VM_MAX_FILES && c->vfs->files[fi].in_use && c->vfs->files[fi].is_binder)
            return vm_binder_ioctl(c, (unsigned long)a1, (unsigned long)a2);
        return -VM_ENOTTY;                              /* 真 fd 已由 BPF 原生放行；控制台不是 tty */                              /* 控制台不是 tty（bionic stdio 全缓冲语义） */
    }
    case VM_NR_uname: {
        /* struct utsname: 6 × 65 字节，glibc/bionic 同布局 */
        /* v0.7.31：sysname 必须 "Linux"（ART CheckFailed 实锤——strcmp(uts.sysname,"Linux")）。
         * 身份藏在 nodename/release 里，sysname 保持内核兼容。 */
        static const char* fields[6] = { "Linux", "twinvm", "1.0-stage4", "TwinVM user-space kernel", "aarch64 twinvm", "(none)" };
        for (int i = 0; i < 6; i++)
            if (vm_mem_write(&c->mem, (uintptr_t)(a0 + (size_t)i * 65), fields[i], strlen(fields[i]) + 1) != 0)
                return -VM_EACCES;
        return 0;
    }
    case VM_NR_getcwd: {
        if ((size_t)a1 < 2) return -28 /* ERANGE */;
        if (vm_mem_write(&c->mem, (uintptr_t)a0, "/", 2) != 0) return -VM_EACCES;
        return 1;
    }
    case VM_NR_readlinkat: {
        /* 4.2：/proc/self/exe → 当前映像；/proc/self/fd/N → 虚拟 fd 的 ROM 路径
         * （bionic linker 的 realpath 查询——ENOENT 只是 warning，支持后更干净） */
        char p[64];
        if (vm_mem_read(&c->mem, p, (uintptr_t)a1, 63) != 0) return -VM_EACCES;
        p[63] = 0;
        const char* target = NULL;
        char pidpath[40];
        snprintf(pidpath, sizeof(pidpath), "/proc/%d", (int)c->mem.pid);
        if (strncmp(p, "/proc/self/fd/", 14) == 0) {   /* 14=串长！15 会吃掉首数字与 NUL 比较永不匹配 */
            int vfd = atoi(p + 14);   /* 14=前缀长；+15 会吃掉首数字（10000→0） */
            int fi2 = vfd - VM_FD_BASE;
            if (fi2 >= 0 && fi2 < VM_MAX_FILES && c->vfs->files[fi2].in_use)
                target = c->vfs->files[fi2].dir_path;
        } else if (strcmp(p, "/proc/self/exe") == 0 && g_exec_path[0]) {
            target = g_exec_path;
        } else if (strcmp(p, "/proc/self") == 0) {
            /* v1.2.43：/proc/self → /proc/<pid>（realpath 逐组件需要） */
            target = pidpath;
        } else if (g_exec_path[0] && strncmp(p, pidpath, strlen(pidpath)) == 0
                   && strcmp(p + strlen(pidpath), "/exe") == 0) {
            /* /proc/<pid>/exe → 当前映像（realpath 解析完 self 后的路径形态） */
            target = g_exec_path;
        }
        if (target) {
            size_t n = strlen(target);
            if (n > (size_t)a3) n = (size_t)a3;
            if (n && vm_mem_write(&c->mem, (uintptr_t)a2, target, n) != 0) return -VM_EACCES;
            return (long)n;
        }
        return -VM_ENOENT;                              /* 其余 /proc 一律不存在 */
    }

    /* ── 透传：安全原语（guest 与内核同进程，这些不需要虚拟化） ── */
    case VM_NR_brk:     return VM_PT3(VM_NR_brk, a0, 0, 0);
    case VM_NR_mmap: {
        /* Stage 3.1：mmap(虚拟 binder fd) → 共享区零拷贝段 */
        long fd = (long)a4;
        int fi = (int)fd - VM_FD_BASE;
        if (!(a3 & MAP_ANONYMOUS) && fi >= 0 && fi < VM_MAX_FILES && c->vfs->files[fi].in_use
            && c->vfs->files[fi].is_binder)
            return (long)vm_binder_mmap(c, (size_t)a1);
        /* Stage 4.2：mmap(ROM 虚拟 fd) —— 真 linker 映射 .so 的核心路径。
         * 内核语义等价：匿名映射（W 先行）→ 从 ROM inode 拷贝 → mprotect 收紧。
         * linker 映射段恒带 MAP_FIXED；不带 FIXED 的 fd 请求交给原生（不会出现）。 */
        if (!(a3 & MAP_ANONYMOUS) && fi >= 0 && fi < VM_MAX_FILES && c->vfs->files[fi].in_use) {
            const VmRomInode* ino = &c->vfs->rom->inodes[c->vfs->files[fi].rom_idx];
            size_t len = (size_t)a1;
            int prot = (int)a2;
            uintptr_t addr = (uintptr_t)a0;
            int flags = ((int)a3 & MAP_FIXED) | MAP_PRIVATE | MAP_ANONYMOUS;

            /* v0.7.5：支持非 FIXED 映射（glibc ld.so 首段不带 FIXED——bionic 才恒 FIXED；
             * 原注释假设错误。非 FIXED → 内核自选地址，同样从 ROM 拷贝段数据） */
            /* v0.7.5 铁律：handler 内禁止 glibc wrapper（内部 syscall 不可控——
             * mmtest 实锤二次 TRAP 循环死）。裸 syscall：ANON fd=-1 → BPF 判
             * ALLOW（0xFFFFFFFF > 10063 无符号）恰好安全。 */
            long at = vm_syscall6(VM_NR_mmap, addr, len, (long)(prot | PROT_WRITE),
                                  (long)flags, -1, 0);
            if (at < 0 && at > -4096) return at;
            size_t off = (size_t)a5;
            if (off < ino->size) {
                size_t n = ino->size - off;
                if (n > len) n = len;
                memcpy((void*)at, ino->map + off, n);
            }
            /* v0.7.55 关键：ARM64 上"写完代码必须清 i-cache 才能执行"。
             * 真机 v0.7.54 实锤：没清 cache 的页执行到陈旧字节 → SIGILL(4)，
             * 而且发生在 ART 刚进 Runtime 之后（正是首次执行新映射 ROM 代码时）。
             * 顺序：先 flush 再 mprotect 收紧权限（标准做法），之后补一次。 */
            __builtin___clear_cache((char*)at, (char*)at + len);
            long mpr = vm_syscall6(VM_NR_mprotect, at, len, prot, 0, 0, 0);
            __builtin___clear_cache((char*)at, (char*)at + len);
            vm_map_add((uintptr_t)at, len, prot, c->vfs->files[fi].dir_path);
            /* v1.2.22 ★★★ **段映射留痕**：一行说明"哪个 ROM 文件被映射到了哪个地址、
             * 多少长度、什么权限、mprotect 是否成功"。
             * 起因：真机 v1.2.21 的循环现场里，msync 调用者落在一段 180KB 的
             * **匿名 rwxp** 上——按段大小推断是 libartbase.so，但该模块自带的
             * msync 调用点又对不上。**权限本该被 mprotect 收紧成 r-x，实测仍是 rwx**，
             * 说明收紧这一步可能失败了。没有这张表就只能靠猜，所以直接让它自报。 */
            vm_diag("[rom-map] %s addr=0x%lx len=0x%zx prot=0x%x off=0x%zx mprotect=%ld",
                   c->vfs->files[fi].dir_path, (unsigned long)at, len, prot, off, mpr);
            return at;
        }
        long mret = VM_PT6(VM_NR_mmap, a0, a1, a2, a3, a4, a5);
        /* v1.2.52 ★★★ JIT Low4GB 精确落位：真机 v1.2.51 实锤——ART 的 JIT
         * （MapInternalArtLow4GBAllocator）要求 code/data cache 落在低 4GB 的
         * **精确对齐地址**（hint 0x3bf00000/0x3c000000/0x88800000… 128MB 连片）。
         * 内核对非 FIXED mmap 不保证 hint → 返回 0x7c0c3c5000（高地址）→
         * ART 判定失败无限换 hint 重试（日志 8235~8240 行连片"未按 hint"）→
         * JIT 内部结构残缺 → 解释器/编译器 null 解引用 SEGV（pc=0x3c0009f0）。
         * 修法：**低 4GB hint 的匿名非 FIXED mmap 由引擎代管**——优先原 hint，
         * 冲突则在本进程低 4GB 空洞里按对齐找位（MAP_FIXED 强制落位）。
         * （JIT 之外不受影响：普通 mmap 无低地址 hint，照旧透传。） */
        if (mret > 0 && mret != (long)a0 && a0 != 0
            && (unsigned long long)a0 < 0x100000000ULL
            && (a3 & MAP_ANONYMOUS) && !(a3 & MAP_FIXED)) {
            uintptr_t want = (uintptr_t)a0 & ~(uintptr_t)0xFFF;
            size_t   alen  = ((size_t)a1 + 0xFFF) & ~(size_t)0xFFF;
            /* 三段式选位：hint 本体 → hint 前向 128MB 步进 → 64MB 空洞随机起点 */
            uintptr_t cands[24]; int nc = 0;
            cands[nc++] = want;
            for (int s = 1; s <= 11 && nc < 24; s++) {
                uintptr_t back = want - (uintptr_t)s * 0x8000000UL;   /* -128MB 步进 */
                if (back >= 0x01000000UL) cands[nc++] = back;
                uintptr_t fwd  = want + (uintptr_t)s * 0x8000000UL;   /* +128MB 步进 */
                if (fwd + alen < 0x100000000ULL) cands[nc++] = fwd;
            }
            for (int ci = 0; ci < nc; ci++) {
                uintptr_t c = cands[ci] & ~(uintptr_t)0xFFF;
                long r2 = vm_syscall6(VM_NR_mmap, (long)c, (long)alen,
                                      (long)(a2 | PROT_WRITE),
                                      (long)(MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS), -1, 0);
                if (r2 > 0 && r2 < 4096) continue;   /* 不该发生 */
                if (r2 == (long)c) {
                    /* 落位成功：撤掉内核给的高地址原映射，登记新地址 */
                    vm_syscall6(VM_NR_munmap, (long)mret, (long)alen, 0, 0, 0, 0);
                    vm_map_add(c, alen, (int)a2, "");
                    vm_diag("map] JIT-low4GB: hint=0x%lx len=0x%lx → 精确落位 0x%lx（候选#%d）",
                            (unsigned long)a0, (unsigned long)a1, (unsigned long)c, ci);
                    return (long)c;
                }
                /* r2 为负或落点异常：撤掉这次 FIXED 尝试，换下一个候选 */
                if (r2 > 0 && r2 != (long)c)
                    vm_syscall6(VM_NR_munmap, (long)r2, (long)alen, 0, 0, 0, 0);
            }
            /* 全候选失败：退回内核原结果（保持旧行为） */
        }
        if (mret > 0) vm_map_add((uintptr_t)mret, (size_t)a1, (int)a2, "");
        /* v1.2.22 ★★★ **mmap 提示地址 vs 实际返回** —— 直接验证"低 4GB 分配器为什么找不到位置"。
         * 真机 v1.2.21 现场：ART 的 `MapInternalArtLow4GBAllocator` 反复
         *   munmap(同一地址) → mmap(hint=低地址递增) → msync 逐页 touch
         * 且 maps 里 **0x02000000 以下没有任何映射** → 极可能是
         * "**请求低地址、内核没给低地址**" → 分配器判定失败 → 换下一个候选 → 无限循环。
         * 这行日志把 hint 与内核实际返回并排打出来，一次运行即可证实/推翻。 */
        {
            /* v1.2.35 ★ 聚焦化：前 40 次上限让 Low4GB 分配器（启动后期）永远进不了
             * 窗口（真机 v1.2.34 实锤 40 条全是启动早期普通 mmap）。
             * 策略：hint 落低 4GB（<0x100000000）的全打——这正是 Low4GB 分配器
             * 的特征；普通匿名 mmap 仍限前 40 条（防刷屏）。 */
            static int s_mm = 0;
            int low4 = ((unsigned long long)a0 < 0x100000000ULL) && a0 != 0;
            if ((a4 == -1 || (a4 & 0xffffffffU) == 0xffffffffU)
                && (low4 || s_mm < 40)) {      /* 匿名（w4 惯例双判定） */
                s_mm++;
                vm_diag("[mmap] hint=0x%lx len=0x%lx prot=0x%lx flags=0x%lx -> ret=0x%lx%s",
                       (unsigned long)a0, (unsigned long)a1, (unsigned long)a2,
                       (unsigned long)a3, (unsigned long)mret,
                       (mret > 0 && (unsigned long)mret != (unsigned long)a0) ? "  ← 未按 hint" : "");
            }
        }
        return mret;
    }
    case VM_NR_pread64: {
        /* Stage 4.2：pread64(虚拟 fd) —— bionic ElfReader 读 ELF 头/程序头的路径。
         * 真 fd 已由 BPF 参数化放行；到这里只剩虚拟 fd。 */
        int fi = (int)a0 - VM_FD_BASE;
        if (fi >= 0 && fi < VM_MAX_FILES && c->vfs->files[fi].in_use) {
            const VmRomInode* ino = &c->vfs->rom->inodes[c->vfs->files[fi].rom_idx];
            size_t off = (size_t)a3;
            size_t n = (size_t)a2;
            if (off >= ino->size) return 0;
            if (n > ino->size - off) n = ino->size - off;
            if (n && vm_mem_write(&c->mem, (uintptr_t)a1, ino->map + off, n) != 0) return -VM_EACCES;
            return (long)n;
        }
        return -VM_EBADF;
    }
    case VM_NR_munmap:  return VM_PT3(VM_NR_munmap, a0, a1, 0);
    case VM_NR_mprotect:return VM_PT4(VM_NR_mprotect, a0, a1, a2, 0);
    case VM_NR_mremap:  return VM_PT5(VM_NR_mremap, a0, a1, a2, a3, 0);
    case VM_NR_madvise: return VM_PT4(VM_NR_madvise, a0, a1, a2, 0);
    /* v0.7.41：msync（arm64 227 / x86_64 26）直接成功。
     * 真机实锤（2026-10-14 07:12 的 twinvm-live.log）：ART 走到加载启动镜像之后，
     * 对映射（含 ROM 虚拟 fd 的映射）调 msync，拿到 ENOSYS 就**无限重试** ——
     * 8MB live 日志里 130k 行全是 `ENOSYS nr=227`（实际约 8.7M 次调用），
     * UI 表现就是永久"运行中"。VM 里没有需要真实回写的对象
     * （ROM 只读、映射都是 MAP_PRIVATE），返回 0 语义上安全。 */
    case VM_NR_msync:   return 0;
    case VM_NR_futex:   return VM_PT6(VM_NR_futex, a0, a1, a2, a3, a4, a5);
    case VM_NR_set_tid_address: return VM_PT3(VM_NR_set_tid_address, a0, 0, 0);
    case VM_NR_prctl: {
        /* v0.7.82 ★★★ 真凶：**guest 自己安装 seccomp 过滤器**！
         * 真机实锤（v0.7.81 日志 #20/#21）：
         *     prctl(a0=0x26 = PR_SET_NO_NEW_PRIVS, a0=0x16 = PR_SET_SECCOMP) → 我们直接透传 → guest 把自己的
         *     seccomp 策略装上了（Android bionic 的 zygote policy，用 KILL_PROCESS）。
         * 后果：**我们自己的过滤器被替换/叠加**，之后 guest 的每一个 TRAP
         * 都按"它刚装的那层"处理 → 第一条就 KILL_PROCESS → 进程消失，
         * 我们的 handler 再也进不去（这正好解释了 HND# 只有 1 条、[HR] 一条没有、
         * 黑匣子 seq=0）。
         *
         * 处置：**这两个 prctl 绝不能透传**——
         *   PR_SET_NO_NEW_PRIVS(38=0x26? 见下) / PR_SET_SECCOMP(22=0x16)：
         *     直接"假装成功"返回 0（guest 以为装上了，实际什么都没发生），
         *     过滤器只由我们这一层掌控。
         * 注意 arm64 上两个常量：PR_SET_NO_NEW_PRIVS=38(0x26)、PR_SET_SECCOMP=22(0x16)，
         * 与 x86_64 相同（取自 linux/prctl.h）。 */
        if ((long)a0 == 38 || (long)a0 == 22) return 0;      /* 假装成功，绝不透传 */
        /* v1.2.55 ★ 线程名消音：ART 起每个守护线程都 prctl(PR_SET_NAME, "HeapTaskDaemon")，
         * 宿主真 prctl 在 App 进程回 EPERM → "Unable to set the name of current thread"
         * 刷屏（真机 Hello 里程碑日志 14 处）。线程名对 guest 只是自我标签，
         * 伪造成功无副作用（/proc/<tid>/comm 合成层不受影响）。 */
        if ((long)a0 == 15 /* PR_SET_NAME */) return 0;
        /* v1.1.5 ★ 真机 v1.1.4 实锤：guest 的 debuggerd_signal_handler 在致命
         * 信号后 prctl(PR_SET_PTRACER, ANY)（让外部 debuggerd 可附身写 tombstone）。
         * 引擎透传 → 宿主真 prctl 在 App 进程里回 EPERM（外层策略/Yama 所致）→
         * handler 走 fatal_errno("failed to set traceable") → 进程直接死。
         * 语义正解：**我们就是 tracer**——VM 内"谁都可以附身"恒真，返回 0。 */
        if ((long)a0 == 0x59616d61 /* PR_SET_PTRACER */) return 0;
        /* v0.7.83：prctl 已从 BPF 放行清单**摘除**（防止 guest 装 seccomp），
         * 所以这里**不能再直发**（VM_PT5 会二次 TRAP）——改为宿主原生调用。 */
        {
            extern int prctl(int, ...);
            return (long)prctl((int)a0, (long)a1, (long)a2, (long)a3, (long)0);
        }
    }
    case VM_NR_set_robust_list:
        /* v0.7.52：全量放行清单审计实测 —— App 策略 KILL 它，所以绝不能放行。
         * 它只是"告诉内核 robust futex 链表在哪"，伪造成功即可
         * （bionic 不校验返回值；我们也不需要 robust futex）。 */
        return 0;
    case VM_NR_rt_sigaction:
        /* v0.7.6：BPF 只对 sig==31(TRAP) 放进来，其余原生 ALLOW 不会到这。
         * 伪造成功——保护 VM 的 SIGSYS handler 不被 guest/linker 重置。
         * （旧 PT5 直发已删：sig==31 被参数化 TRAP 后直发=二次 TRAP 地雷） */
        return 0;
    case VM_NR_rt_sigprocmask: {
        return VM_PT4(VM_NR_rt_sigprocmask, a0, a1, a2, 0);
    }
    /* v1.2.54 ★ ptrace 后端补课：v1.2.53 只改了 BPF 表（NOTIF/ALLOW 路径），
     * 而 TA 真机跑的是 **ptrace 后端**（seccomp 被系统封死）——所有 syscall
     * 都进 handler 的 switch dispatch，137 无 case → default ENOSYS。
     * 真机 v1.2.53 实锤：`[caller] nr=137 #1`（TRAP 打印）+ ENOSYS →
     * "signal_set.h:61] sigwait failed" → abort。这里补 case 透传。 */
    case VM_NR_rt_sigtimedwait:
        return VM_PT4(VM_NR_rt_sigtimedwait, a0, a1, a2, a3);
    case VM_NR_sigaltstack:     return VM_PT4(VM_NR_sigaltstack, a0, a1, a2, 0);
    case VM_NR_kill:     return VM_PT3(VM_NR_kill, a0, a1, 0);
    case VM_NR_tgkill:  return VM_PT4(VM_NR_tgkill, a0, a1, a2, 0);
    case VM_NR_wait4:   return VM_PT5(VM_NR_wait4, a0, a1, a2, a3, 0);
    case VM_NR_clone:   return VM_PT6(VM_NR_clone, a0, a1, a2, a3, a4, a5);
    case VM_NR_clock_gettime: return VM_PT3(VM_NR_clock_gettime, a0, a1, 0);
    case VM_NR_getrandom:      return VM_PT4(VM_NR_getrandom, a0, a1, a2, 0);
    case VM_NR_sched_yield:    return VM_PT3(VM_NR_sched_yield, 0, 0, 0);
    case VM_NR_sched_getaffinity: return VM_PT4(VM_NR_sched_getaffinity, a0, a1, a2, 0);
    case 144: case 119: return 0;   /* setscheduler：伪造成功（SCHED_OTHER） */
    case 145: case 120: {           /* getscheduler：SCHED_OTHER=0（pthread_create 必需） */
        return 0;
    }
    case 121: {                     /* sched_getparam：填 sched_param{0} */
        uint32_t zero = 0;
        if (a1) vm_mem_write(&c->mem, (uintptr_t)a1, &zero, 4);
        return 0;
    }
#if defined(__x86_64__)
    case 97: {                      /* getrlimit(x86)：RLIM_INFINITY（ylinker 探栈限制） */
        uint64_t inf[2] = { ~0ULL, ~0ULL };
        if (a1) { if (vm_mem_write(&c->mem, (uintptr_t)a1, inf, sizeof(inf)) != 0) return -VM_EACCES; }
        return 0;
    }
    case 160:                       /* setrlimit(x86)：伪造成功 */
        return 0;
#else
#if defined(__x86_64__)
    case 138: case 137: {           /* fstatfs/statfs(x86)：linker 装载策略检查 */
#else
    case 44: {                      /* fstatfs(arm64)。137=rt_sigtimedwait —— v1.2.53 ★
                                     * 真机实锤：ART Signal Catcher 的 sigwait 走 137，
                                     * 被这里误当 x86 statfs 伪造 → 返回 0 但无等待语义
                                     * → "signal_set.h:61] sigwait failed" → abort。
                                     * arm64 的 137 改走透传（vm_passthrough + BPF 放行）；
                                     * statfs(43) 伪造保留。 */
#endif
        /* 4.2：填最小 statfs（f_type=0 未知 fs，bsize=4096，namelen=255）。
         * v0.7.8 真机实锤：linker 对 .so 做 fstatfs 检查，ENOSYS 直接拒绝装载。 */
        uint8_t sfs[128];
        memset(sfs, 0, sizeof(sfs));
        uint32_t bsz = 4096, nlen = 255;
        memcpy(sfs + 4, &bsz, 4);       /* f_bsize */
        memcpy(sfs + 56, &nlen, 4);     /* f_namelen（LP64 偏移） */
        if (vm_mem_write(&c->mem, (uintptr_t)a1, sfs, 88) != 0) return -VM_EACCES;
        return 0;
    }
    case 163: {                     /* getrlimit(arm64)  v1.2.33 ★★★ 按资源给真实值 */
        /* 真机 v1.2.32 实锤：`[fail] nr=222 → -22`（**mmap 返回 EINVAL**）出现在
         * guest 卡死（等一个从未创建的线程）之前；而帧链显示卡点在
         * `JNI_CreateJavaVM → Runtime::Init → art::Thread::Startup() → Thread::Attach
         *  → Thread::Thread → **Thread::Init**`（Thread::Init 会算线程栈/栈上限）。
         * 老实现**对所有资源一律回 RLIM_INFINITY**，其中 **RLIMIT_STACK 回"无限"
         * 与 Android 实际（8MB）不符**，会污染 bionic/ART 的线程栈与栈 HWM 计算。
         * 这里按资源给值：栈 8MB（Android 默认）、fd 1024、其余无限。 */
        uint64_t rl[2] = { ~0ULL, ~0ULL };
        switch ((int)a0) {
            case 3: rl[0] = 8ULL * 1024 * 1024; rl[1] = ~0ULL; break;   /* RLIMIT_STACK */
            case 7: rl[0] = 1024;               rl[1] = 4096;   break;  /* RLIMIT_NOFILE */
            default: break;                                             /* 其余无限 */
        }
        if (a1) { if (vm_mem_write(&c->mem, (uintptr_t)a1, rl, sizeof(rl)) != 0) return -VM_EACCES; }
        return 0;
    }
    case 164:                       /* setrlimit(arm64) */
        return 0;
#endif
    case VM_NR_connect: {
        /* v0.8.4 ★ 真 fd 的 connect 必须**真正连上**（ART 连 logd）。
         * 历史：SIGSYS 时代 BPF 按 fd 参数化放行真 fd，所以 handler 只处理虚拟 fd；
         * 但 **USER_NOTIF 的 NOTIF 集合里包含了 connect**，于是**真 fd 也进来了**
         * 而我们一律回 -ENOSYS —— ART 连不上 logd → 后续 futex 死等（真机 v0.8.3
         * 实锤：connect(fd=0xd) 之后 #337074 futex 后卡死）。
         * 处置：真 fd（3..VM_FD_BASE-1）由**别的线程**执行真实 connect；
         * 这里先直接返回 0（假装成功）—— logd 是 AF_UNIX 本地 socket，
         * 写入即被丢弃，不影响 ART 继续；虚拟 fd 仍回 -ENOSYS。 */
        unsigned fd = (unsigned)a0;
        if (fd >= 3 && fd < (unsigned)VM_FD_BASE) {
            vm_log("connect(fd=%u) -> 假装成功（真 fd，避免 ART 连 logd 失败后死等）", fd);
            return 0;
        }
        vm_log("connect(fd=%ld) -> -ENOSYS（虚拟 fd）", a0);
        return -VM_ENOSYS;
    }
    case 198: case 359: case 41: {   /* socket 族（arm64 198 / x86 41+359） */
        /* 4.2：AF_UNIX（本地 IPC）放行原生——stats/logd 的 socket() 探测不能 -EACCES
         * （v0.7.15 真机实锤：libstatssocket 初始化拿 EACCES 直接判 JNI 失败）。
         * AF_INET/INET6 仍拒绝（真网络栈是 Stage 5 的 halbox 桥）。 */
        if (a0 == 1 /* AF_UNIX */) {
            vm_log("socket(AF_UNIX) -> 宿主原生 socket()（本地 IPC）");
            /* v0.8.3 ★ 关键：**不能再用 VM_PT3（裸 svc 直发）**！
             * USER_NOTIF 下，监听线程本身就是"被拦截 syscall 的服务者"，
             * 若它再发一个同号的裸 svc，会**再次触发自己的过滤器** → 自我挂起 → 死锁
             * （真机 v0.8.1/0.8.2：socket(AF_UNIX) 后 90 秒无输出被判卡死）。
             * 改用**宿主 libc 的 socket()** —— 那是同一个 syscall，但由 C 库发起，
             * 语义相同且不经过我们自己的"用户态内核服务"路径（不递归）。
             * 注：宿主也是同一进程，同样会命中 USER_NOTIF；所以**监听线程里必须
             * 避免任何可能被拦截的 syscall** —— 这里用 socketpair 也不行（同族）。
             * 因此：AF_UNIX socket 直接**由监听线程答复**（见 vm_notif.c），
             * 引擎这一层只做兜底：返回 -EACCES 让调用方走退化路径，绝不阻塞。 */
            return -VM_EACCES;
        }
        vm_log("socket(%ld) -> -EACCES (VM 无网络栈，Stage3)", a0);
        return -VM_EACCES;
    }
    case VM_NR_fcntl: {
        /* v0.7.62：三层路由（fd 参数化后 BPF：真 fd 含 console→ALLOW 原生；
         * 虚拟 fd(10000+) 与 stdio(0/1/2) → TRAP 进这里）。
         * 病史：v0.7.61 前fcntl 在放行清单 → 原生直发 → 内核不认识虚拟 fd
         * → EBADF → libziparchive 的 jar 全部读废 → "Image component does not
         * match" → ClassLinker abort（真机 fcntl(10000, F_GETFD) failed ×5 实锤）。 */
        unsigned fd = (unsigned)a0;
        if (fd >= VM_FD_BASE && fd < VM_FD_BASE + VM_MAX_FILES) {
            int fi = (int)(fd - VM_FD_BASE);
            if (!c->vfs->files[fi].in_use) return -VM_EBADF;
            switch ((int)a1) {
                case 0: case 1030: {     /* F_DUPFD / F_DUPFD_CLOEXEC：真 dup（VFS 槽位复制） */
                    int nf = -1;
                    for (int k = 0; k < VM_MAX_FILES; k++)
                        if (!c->vfs->files[k].in_use) { nf = k; break; }
                    if (nf < 0) return -24 /*EMFILE*/;
                    c->vfs->files[nf] = c->vfs->files[fi];   /* 共享 rom_idx，pos 独立 */
                    c->vfs->files[nf].pos = c->vfs->files[fi].pos;
                    return VM_FD_BASE + nf;
                }
                case 1: return 0;                 /* F_GETFD */
                case 2: return 0;                 /* F_SETFD */
                case 3: return 0;                 /* F_GETFL → O_RDONLY */
                case 4: return 0;                 /* F_SETFL（ROM 只读，忽略） */
                case 5: case 6: case 7: return 0; /* F_GETLK/F_SETLK/F_SETLKW：无锁 */
                default: return 0;
            }
        }
        if (fd <= 2) {                            /* stdio：VM 控制台语义——伪造，绝不直发 */
            switch ((int)a1) {
                case 1: case 2: case 3: case 4: return 0;
                case 0: case 1030: return -22 /*EINVAL*/;   /* stdio 不 dup */
                default: return 0;
            }
        }
        return VM_PT4(VM_NR_fcntl, a0, a1, a2, 0);   /* 真 fd（含 console）：直发 */
    }
    /* v0.2.8：exit = bionic __exit() 线程退出。**必须由 BPF 放行**（见 gen_bpf.py 铁律），
     * 正常情况下根本不会 TRAP 到这里。万一过滤器放行清单漏了它，绝不能在 handler 里
     * 用裸 svc 直发（那是二次 TRAP → 信号挂起 → 迟到投递 → 乱判决/自递归，正是
     * Stage 1 自检 code=-31 的成因）。这里退化成"整槽位关机"：语义上牺牲一个线程，
     * 但保证引擎不会崩、且能把故障点明确打到控制台。 */
    case VM_NR_exit:
        vm_log("!! exit(%ld) 抵达判决表：BPF 放行清单缺 exit —— 直发会二次 TRAP，"
               "退化为整槽位退出（请重跑 gen_bpf.py 重新生成 bpf_gen.h）", a0);
        return VM_PT3(VM_NR_exit_group, a0, 0, 0);
#if defined(__x86_64__)
    case VM_NR_arch_prctl: return VM_PT3(VM_NR_arch_prctl, a0, a1, 0);
#endif

    case 269:   /* x86_64 faccessat */
    case 48:    /* arm64  faccessat —— ROM 里存在=可访问，宿主路径=不存在 */
    case 439: { /* faccessat2（两架构同号） */
        /* v1.2.55 ★ W_OK 语义修正：access(path, W_OK) 对 ROM 恒不可写
         * （真机 Hello 里程碑实锤：/rom/hello.dex mode 已 0444，但 ART 的
         * "Attempt to load writable dex file" 警告仍出现——因为旧实现
         * 对 W_OK 也返回 0"可访问"。ART 由此走 dex 校验降级路径。）
         * 规则：mode 含 W_OK(2) → 读 stat 的 mode 位：有写位才 0，否则 EACCES。 */

        char p[VM_MAX_PATH + 1];
        if (vm_mem_read(&c->mem, p, (uintptr_t)a1, VM_MAX_PATH) != 0) return -VM_EACCES;

        p[VM_MAX_PATH] = 0;
        uint32_t am = 0; uint64_t asz = 0;
        if (vm_vfs_stat(c->vfs->rom, p, &am, &asz) < 0) return -VM_ENOENT;
        if (((int)a2 & 2 /* W_OK */) && !(am & 0222)) return -VM_EACCES;
        return 0;
    }
    case 285:   /* x86_64 fallocate */
    case 47: {  /* arm64 fallocate —— bionic 初始化会做堆预留，伪造成功 */
        return 0;
    }
    case VM_NR_exit_group:
        c->halt = 1;
        vm_log("exit_group(%ld) — guest 关机，共服务 %llu 次系统调用",
               a0, (unsigned long long)c->cycles);
        return 0;
    case VM_NR_process_vm_readv:
    case VM_NR_process_vm_writev: {
        /* v0.7.77：**重写**（v0.7.74/75 版有两个致命 bug，真机一跑就崩槽位）：
         *   ① local 地址用了"全局累计 copied"当偏移 —— 应为该 local iovec 内的偏移；
         *   ② riov[ri].len - roff 为 0 时 chunk==0 → ri++ 后 while 条件仍成立 → 可能死循环。
         * 现在按 Linux 语义老老实实写：逐 local iovec × 逐 remote iovec 交叉搬运，
         * 每个 iovec 各自的偏移独立维护；任一地址不可访问就**停在当前已搬运字节数**
         * （Linux 语义：部分成功返回已拷贝量），绝不无限循环。 */
        /* v0.7.78：把调用参数与 iovec 内容打出来（前 8 次）。
         * v0.7.77 我整段重写时把诊断一起删了，结果真机上"看不到任何线索"。 */
        {
            static long dbg_pvm = 0;
            if (dbg_pvm < 8) {
                dbg_pvm++;
                unsigned long lp[2] = {0,0}, rp[2] = {0,0};
                vm_mem_read(&c->mem, lp, (uintptr_t)a1, sizeof(lp));
                vm_mem_read(&c->mem, rp, (uintptr_t)a3, sizeof(rp));
                vm_log("  ^^ pvm_%s a0=0x%lx a1=0x%lx[%d]=0x%lx/%lu a3=0x%lx[%d]=0x%lx/%lu a5=0x%lx",
                       nr == VM_NR_process_vm_readv ? "readv" : "writev",
                       (unsigned long)a0, (unsigned long)a1, (int)a2,
                       lp[0], lp[1], (unsigned long)a3, (int)a4, rp[0], rp[1],
                       (unsigned long)a5);
            }
        }
        const int is_read = (nr == VM_NR_process_vm_readv);
        int liovcnt = (int)a2, riovcnt = (int)a4;
        if (liovcnt <= 0 || riovcnt <= 0) return 0;      /* Linux：无 iovec 即 0 字节 */
        /* v0.7.78：**栈保护** —— SIGSYS handler 跑在 guest 应用栈上（没建 altstack），
         * 所以这里的一切都必须小：iovec 表最多 4 个、临时缓冲 512B 分块。 */
        if (liovcnt > 4)  liovcnt = 4;
        if (riovcnt > 4)  riovcnt = 4;

        struct { uintptr_t base; size_t len; size_t off; } liov[4], riov[4];
        for (int i = 0; i < liovcnt; i++) {
            unsigned long pair[2];
            if (vm_mem_read(&c->mem, pair, (uintptr_t)a1 + (uintptr_t)i * 16, sizeof(pair)) != 0) return -14;
            liov[i].base = (uintptr_t)pair[0]; liov[i].len = (size_t)pair[1]; liov[i].off = 0;
        }
        for (int i = 0; i < riovcnt; i++) {
            unsigned long pair[2];
            if (vm_mem_read(&c->mem, pair, (uintptr_t)a3 + (uintptr_t)i * 16, sizeof(pair)) != 0) return -14;
            riov[i].base = (uintptr_t)pair[0]; riov[i].len = (size_t)pair[1]; riov[i].off = 0;
        }

        long copied = 0;
        int ri = 0;
        for (int li = 0; li < liovcnt; li++) {
            while (liov[li].off < liov[li].len) {
                while (ri < riovcnt && riov[ri].off >= riov[ri].len) ri++;   /* 跳过用完的 remote */
                if (ri >= riovcnt) return copied;
                size_t want = liov[li].len - liov[li].off;
                size_t have = riov[ri].len - riov[ri].off;
                size_t n = want < have ? want : have;
                if (n == 0) return copied;
                if (n > 512) n = 512;
                char tmp[512];
                uintptr_t laddr = liov[li].base + liov[li].off;
                uintptr_t raddr = riov[ri].base + riov[ri].off;
                uintptr_t src = is_read ? raddr : laddr;
                uintptr_t dst = is_read ? laddr : raddr;
                if (vm_mem_read(&c->mem, tmp, src, n) != 0) return copied ? copied : -14;
                if (vm_mem_write(&c->mem, dst, tmp, n) != 0) return copied ? copied : -14;
                copied += (long)n;
                liov[li].off += n;
                riov[ri].off += n;
            }
        }
        return copied;
    }
    default: {
        /* v0.7.56：BPF 末位已改 TRAP（不再无声 KILL），未知 syscall 都落到这里。
         * 大声记全参数——这是 ART 深水区的"语音盒"：哪个 syscall 缺、参数是什么，
         * 一次运行全部暴露。限频 64 次/同号，防刷屏。 */
        static long last_nr = -1; static long suppress = 0;
        if (nr != last_nr) { last_nr = nr; suppress = 0; }
        if ((++suppress & 0x3F) == 1) {
            vm_log("ENOSYS nr=%ld(0x%lx) a0=0x%lx a1=0x%lx a2=0x%lx a3=0x%lx a4=0x%lx a5=0x%lx (x%ld)",
                   nr, nr, (unsigned long)a0, (unsigned long)a1, (unsigned long)a2,
                   (unsigned long)a3, (unsigned long)a4, (unsigned long)a5, suppress);
        }
        /* v0.7.72：**把字符串参数读出来**。
         * 真机 v0.7.71 实锤：ART abort 时调 mq_open(240) 给 debuggerd 发 abort message，
         * 我们回 ENOSYS → ART 拿不到 fd 直接 abort。它的 a0 指向名字字符串
         * （例如 "/dev/socket/..." 或其它），这一步能把"它想开什么"直接打到日志里。 */
        if (nr == VM_NR_mq_open || nr == VM_NR_mq_unlink) {
            char pbuf[256];
            pbuf[0] = 0;
            int r = vm_mem_read(&c->mem, pbuf, (uintptr_t)a0, sizeof(pbuf) - 1);
            pbuf[sizeof(pbuf) - 1] = 0;
            /* 只留可打印字符，其余显示为 '.' */
            char clean[256];
            int k = 0;
            for (int q = 0; q < 200 && pbuf[q]; q++) {
                unsigned char ch = (unsigned char)pbuf[q];
                clean[k++] = (ch >= 32 && ch < 127) ? (char)ch : '.';
            }
            clean[k] = 0;
            vm_log("  ^^ mq_open(%ld) 读 a0=0x%lx -> ret=%d 内容[%d]=\"%s\"",
                   nr, (unsigned long)a0, r, k, clean);
            /* 再试一次：ART 的 debuggerd 通道名字是编译期常量，也可能在只读段 */
            if (r != 0) {
                vm_log("  ^^ （a0 不可读；mq_open 常量为 0 表示它其实想开的是别的路径）");
            }
        }
        return -VM_ENOSYS;
    }
    }
}
