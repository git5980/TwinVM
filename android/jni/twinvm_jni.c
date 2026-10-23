/* TwinVM Stage 0 — Android JNI 桥
 * stage0(): 完整跑一遍"用户态虚拟机"（fork 槽位 → seccomp 拦截 → ROM 装载 guest → 原生执行）
 * probe(): 探测本机 seccomp 拦截可用性（fork 子进程试装，不污染主进程） */
#include <jni.h>
#include "vm_core.h"

#include <android/log.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>   /* v1.1.6：tee 读 guest 内存用 process_vm_readv */
#include "bpf_gen.h"
#include "probe_policy_blob.h"   /* v0.7.71: 内嵌独立探针（无 libc） */   /* v0.7.51: 全量放行清单（策略审计用） */
#include <sys/prctl.h>
#include <sys/ucontext.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/ptrace.h>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "TwinVM", __VA_ARGS__)

/* ── v0.7.40：卡死可见化 ──────────────────────────────────────────────
 * 真机教训：ART 越走越深之后，某一步会**卡住**（不是崩），而日志是 native 返回后
 * 才落盘的 → 卡死时既没有日志、UI 又永远停在"运行中"，没法定位。
 * 现在：① 控制台每读到一段就**立刻追加**写进 twinvm-live.log（卡死也留现场）；
 *       ② poll + 看门狗：连续 VM_HANG_SILENCE_S 秒没有任何输出（或总时长超 VM_HANG_TOTAL_S）
 *          就判定卡死、SIGKILL 槽位、把"最后现场"交回 UI。 */
#ifndef VM_HANG_SILENCE_S
#define VM_HANG_SILENCE_S 90   /* v0.7.60：示踪版每 syscall 两次停点，guest 显著变慢——放宽 */
#endif
/* v1.2.56：优雅关机宽限（秒）。真机 v1.2.55 实测 ART 收尾活动在
 * stress-all-pass 后 ~3s 内完全静止；8s 已是 2.6 倍裕量。 */
#ifndef VM_EXIT_GRACE_S
#define VM_EXIT_GRACE_S 8
#endif
#ifndef VM_EXIT_GRACE2_S
/* v1.2.78 ★ 分级判据之慢速兜底：健康慢速活动（如 ART 信号准备 47/s）
 * 不再被"N*20<峰*8"的峰值下限误杀（v1.2.77 真机实锤：375 停点被杀，
 * 现场 sigaction/getrlimit/sigprocmask——ART init 正常阶段）。 */
#define VM_EXIT_GRACE2_S 240   /* v1.2.95：冷类装载需要更长（无 dalvik-cache） */
#endif
#ifndef VM_EXIT_MIN_ALIVE
/* 分级判据之"活着"线：静默窗口内 ≥8 停点/s = guest 明显在干活，不杀 */
#define VM_EXIT_MIN_ALIVE 64
#endif
#ifndef VM_HANG_TOTAL_S
#define VM_HANG_TOTAL_S  240   /* v0.7.60：同步放宽总时长（原 120s） */
#endif
#define VM_LIVE_MAX      (4u << 20)
/* v0.7.41：日志暴走（= syscall 死循环）判定。正常一整轮自检的控制台输出约 0.5MB；
 * 8.7M 次 ENOSYS 那种暴走会瞬间冲到几 MB。到 VM_CONSOLE_MAX 就判"暴走"、强杀槽位、
 * 立刻把现场交回 UI —— 不用等看门狗的超时。 */
#define VM_CONSOLE_MAX   (6u << 20)

static int g_live_fd = -1;

static void live_write(const char* p, size_t n) {
    if (g_live_fd >= 0 && n) { ssize_t w = write(g_live_fd, p, n); (void)w; }
}

/* v1.2.28 ★★★ **把示踪器从"2ms 轮询"里解放出来**。
 *
 * 病根：示踪循环原本是 `waitpid(WNOHANG)` + `usleep(2000)` 轮询。
 *  真机实测吞吐：v1.2.25/26/27 三轮都是 **8900 停点 / 20s ≈ 445 syscall/s**，
 *  即 **2.247ms 一次** —— 与 2ms 轮询周期**吻合到小数点**。
 *  也就是说：**guest 的每一次 syscall 都在等我们的轮询节拍**。
 * 代价：ART 创建 Java VM 时要跑几十万次 syscall（`Heap::Heap` → 低 4GB 分配
 *  逐页 memfd 复制 + msync），被限速成**几分钟** → 看门狗判"卡死"强杀。
 *  这就是 `JNI_CreateJavaVM` 永远走不完的直接原因。
 * 改法：`waitpid` **阻塞**等待（停点即到即处理，不再有 2ms 地板）；
 *  需要"guest 阻塞在 futex 时也能体检"的场景，改由**信号唤醒** ——
 *  看门狗置好标志后 `pthread_kill(tracer, SIGUSR1)` 把 `waitpid` 打断（EINTR），
 *  示踪器回到循环顶部处理标志，再重新阻塞。
 * 关键：SIGUSR1 处理函数**不能带 SA_RESTART**（否则 waitpid 会被自动重启，不会返回 EINTR）。
 * v1.2.24 反向验证：当时 `JNI_CreateJavaVM` 卡住 → 调用链与本轮一致。 */
static void tvm_sig_wake(int sig) { (void)sig; }

/* v1.2.30 ★★★★★ **不再安装 SIGUSR1 处理函数**（保留此函数体作反例记录）。
 *
 * 事故：v1.2.28 为"唤醒阻塞在 waitpid 的示踪器"而装了 SIGUSR1 空处理函数。
 *  v1.2.29 真机随后出现新阻塞点：单线程卡在
 *    JNI_CreateJavaVM → Runtime::Init → **art::Thread::Startup()**
 *    → Thread::Attach → Thread::Thread → Thread::Init
 *  → `futex(uaddr=<malloc 区>, FUTEX_WAIT|PRIVATE, 期望值=3, 无超时)`，
 *  锁字 3 = bionic 互斥的 **LOCKED|WAITERS**（`owner_tid=0` **不能**排除自死锁：
 *  bionic 的**快路径 CAS 只写 state、不写 owner_tid**，我上一轮读它读得太乐观了）。
 *  **ART 用 SIGUSR1 承担线程挂起(suspend)协议**；把它覆盖成空处理函数 →
 *  挂起请求被静默吞掉 → 等"挂起完成"的线程在屏障上永久等待。
 *  时间线吻合：v1.2.28 才让 guest 第一次跑到 Thread::Startup（前几轮到不了），
 *  所以这个新阻塞点极可能就是 v1.2.28 引入的。
 *
 * 结论：**这个信号根本不需要** ——
 *  看门狗的体检由**主线程**直接用 /proc + process_vm_readv 完成（不需要停点）；
 *  抓帧也已由 PAC 修复后的 procfs 栈扫描覆盖。
 *  因此：撤掉信号安装与全部 pthread_kill，只保留"阻塞 waitpid"这一项收益（34 倍吞吐）。 */
static void tvm_install_wake_sig_DISABLED(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = tvm_sig_wake;
    sa.sa_flags = 0;                 /* ★ 不带 SA_RESTART */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);
}

/* v1.2.22：给引擎的诊断直写通道（tracer 线程里 vm_log 会丢） */
static void tvm_live_log_hook(const char* line, int len) {
    if (len > 0) live_write(line, (size_t)len);
}

/* v1.1.1 ★★ 重要：**tracer 线程里 `vm_log` 写不出去**（它走 console fd，
 * 而 console 在 guest 启动后已被接管）—— 真机 v1.1.0 实锤：
 * ptrace 循环里的 6 条 GSI 诊断、回填留痕、GETREGSET 错误**全部没出现在日志里**，
 * 而同位置的 [trace] 却在（因为它用 live_write 直写实时文件）。
 * 所以：**tracer 线程内的所有诊断一律走 live_write**。 */
static void ptrace_log(const char* fmt, ...) {
    char buf[512];
    int k = snprintf(buf, sizeof(buf), "[TwinVM] ");
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf + k, sizeof(buf) - (size_t)k - 1, fmt, ap);
    va_end(ap);
    size_t n = strlen(buf);
    if (n + 1 < sizeof(buf)) { buf[n] = '\n'; n++; }
    live_write(buf, n);
    LOGI("%s", buf);
}


static void jni_fault_diag(int sig, siginfo_t* si, void* vuc) {
    ucontext_t* uc = (ucontext_t*)vuc;
    /* 注意：regs[] 只有 x0..x30（31 项），sp/pc/pstate 是 struct sigcontext 的独立字段，
     * v0.2.7 用 r[31]/r[32] 顶 sp/pc 属于越界读（读到 __reserved），v0.2.8 改正。 */
    uint64_t* r = uc->uc_mcontext.regs;
    char b[1024];
    int k = snprintf(b, sizeof(b),
        "[TwinVM] GUEST FAULT: sig=%d addr=%p pc=0x%lx\n"
        "  x0=0x%llx x1=0x%llx x2=0x%llx x3=0x%llx\n"
        "  x4=0x%llx x5=0x%llx sp=0x%lx fp(x29)=0x%llx lr=0x%llx\n",
        sig, si->si_addr, (unsigned long)uc->uc_mcontext.pc,
        (unsigned long long)r[0], (unsigned long long)r[1],
        (unsigned long long)r[2], (unsigned long long)r[3],
        (unsigned long long)r[4], (unsigned long long)r[5],
        (unsigned long)uc->uc_mcontext.sp, (unsigned long long)r[29],
        (unsigned long long)r[30]);
    vm_console_write(b, (size_t)k);
    /* FP 回溯（arm64 帧指针链：[fp]=caller fp, [fp+8]=ret addr） */
    uint64_t fp = r[29];
    k = snprintf(b, sizeof(b), "  backtrace:");
    for (int i = 0; i < 8 && fp > 0x1000; i++) {
        uint64_t fpv[2];
        if (vm_mem_read(&vm_machine_current()->mem, fpv, fp, 16) != 0) break;
        k += snprintf(b + k, sizeof(b) - (size_t)k, " 0x%llx", (unsigned long long)fpv[1]);
        fp = fpv[0];
        if (k > 900) break;
    }
    snprintf(b + k, sizeof(b) - (size_t)k, "\n");
    vm_console_write(b, strlen(b));
    /* 最近 TRAP 的 syscall 序列 */
    VmCpu* c = vm_machine_current();
    k = snprintf(b, sizeof(b), "  recent syscalls:");
    int n = c ? c->nr_hist_n : 0;
    for (int i = n < 12 ? 0 : n - 12; i < n; i++)
        k += snprintf(b + k, sizeof(b) - (size_t)k, " %ld", c->nr_hist[i]);
    snprintf(b + k, sizeof(b) - (size_t)k, "\n");
    vm_console_write(b, strlen(b));
    _exit(89);
}

/* 4.2.1：把捕获的控制台字节流变成 Java 字符串。
 * 关键坑（2026-10-13 16:25 日志实锤）：NewStringUTF 收的是 C 字符串，**遇到第一个
 * 0x00 就结束**。guest 往 logd socket 的 writev 负载（struct log_msg，4KB 里大片是 0）
 * 被引擎 tee 到控制台后，整个日志就在那一刻被截断——连本函数追加的
 * "guest 槽位退出: code=…（死于信号 …）" 尾巴都被吞掉，现场看起来像"进程死在
 * writev 返回后、无下文"。
 * 这里做双保险：引擎侧已在 vm_console_write() 丢弃 NUL；本函数再逐字节扫一遍，
 * NUL/其余 C0 控制字符折成 '.'，并给总长设上限（防止 ART 引导期日志无限增长把
 * Java 字符串撑爆）。 */
/* v1.2.103：b-runner 纯 C 通道——fd>=0 时 stage0 的返回走 write(fd)，
 * JNI env 全部旁路（fork 后子进程不碰 Java 堆——闪退根治） */
static int g_c_out_fd = -1;
static char g_c_rom_path[512] = "", g_c_live_path[512] = "";   /* b-runner 入参 */

static void tvm_c_out_write(const char* buf, size_t len) {
    if (g_c_out_fd >= 0 && buf && len) {
        size_t off = 0;
        while (off < len) {
            ssize_t w = write(g_c_out_fd, buf + off, len - off);
            if (w <= 0) break;
            off += (size_t)w;
        }
    }
}

static jstring console_to_jstring(JNIEnv* env, const char* buf, size_t len) {
    if (g_c_out_fd >= 0) {           /* b-runner 纯 C 通道 */
        tvm_c_out_write(buf, len);
        return NULL;
    }
    size_t cap = len;
    const size_t LIMIT = (size_t)3 << 20;        /* 3MB 上限 */
    int truncated = 0;
    if (cap > LIMIT) { cap = LIMIT; truncated = 1; }
    char* s = (char*)malloc(cap + 64);
    if (!s) return (*env)->NewStringUTF(env, "[TwinVM] 日志缓冲区分配失败");
    size_t k = 0;
    for (size_t i = 0; i < cap; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c == 0) continue;                    /* NUL：绝不能让 C 字符串提前结束 */
        if ((c < 0x20 && c != '\n' && c != '\r' && c != '\t') || c == 0x7F) c = '.';
        s[k++] = (char)c;
    }
    if (truncated) {
        const char* note = "\n[TwinVM] （日志超 3MB 已截断）\n";
        memcpy(s + k, note, strlen(note));
        k += strlen(note);
    }
    s[k] = 0;
    jstring r = (*env)->NewStringUTF(env, s);
    free(s);
    return r;
}

/* bionic 的 strsignal 不认识 SIGSYS(31)（会打 "Unknown signal 31"），
 * 而它恰好是这套引擎最要紧的信号 —— 自己查表，别让日志含糊。 */
static const char* jni_signal_name(int sig) {
    switch (sig) {
        case 4:  return "SIGILL";
        case 5:  return "SIGTRAP";
        case 6:  return "SIGABRT";
        case 7:  return "SIGBUS";
        case 8:  return "SIGFPE";
        case 9:  return "SIGKILL";
        case 11: return "SIGSEGV";
        case 31: return "SIGSYS（系统调用被过滤器/策略拒绝）";
        default: { const char* n = strsignal(sig); return n ? n : "unknown"; }
    }
}

static void jni_install_fatal_diag(void) {
    /* 关键：不覆盖 SIGSYS（那是引擎的系统调用入口）。其余致命信号都装上诊断，
     * 这样任何"无下文"死亡都会在控制台留下 pc/寄存器/回溯 + 最近 TRAP 序列。 */
    static const int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGABRT, SIGFPE, SIGTRAP };
    struct sigaction d;
    memset(&d, 0, sizeof(d));
    d.sa_sigaction = jni_fault_diag;
    d.sa_flags = SA_SIGINFO;
    for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
        sigaction(sigs[i], &d, NULL);
}

/* 子进程 = guest 槽位（VMOS :vmsXX 的 Stage0 版） */

/* v0.7.48 —— App 策略 KILL 名单探针。
 * 真机实锤（v0.7.47）：槽位死于 SIGSYS(-31)，但**所有 TRAP 日志里都没有它**，
 * 且 blackbox 显示死亡时 SIGSYS 还处于“被屏蔽”状态 —— 只有内核直接杀
 * （SECCOMP_RET_KILL_PROCESS）才会这样绕过 handler 与屏蔽位。
 * 本函数在 App 进程里逐个 fork 子进程去调用可疑 syscall：被信号杀掉的就是
 * App 策略 KILL 名单里的（参数随意，seccomp 在参数校验之前就判）。只杀子进程。 */
struct VmPolicyProbe { const char* name; long nr; };
/* v0.7.51 —— 全量放行清单审计。
 * v0.7.50 的教训：共享黑匣子里**没有**那次致命 syscall，说明它被我们的 BPF 放行了
 * （放行 = 原生执行 = 不经过引擎 = 黑匣子看不到），随后被 App 策略
 * SECCOMP_RET_KILL_PROCESS 直接杀（无 handler、无 TRAP 行、code=-31）。
 * 所以必须把**放行清单里每一条**都拿去做一次"会不会被 App 策略杀"的实测。 */
static const struct VmPolicyProbe kAllowAudit[] = VM_BPF_ALLOW_NRS;
/* 之前已实测出 KILL 的（v0.7.48，保留做回归对照） */
/* v0.7.53：**参数化放行**的那些（不在 VM_BPF_ALLOW_NRS 里，但 BPF 会按参数放行）——
 * 全量清单审计覆盖不到它们，所以单独测。 */
/* v0.7.71：外层 zygote seccomp 拦截候选（实测逐条试调）。 */
static const struct VmPolicyProbe kZygoteBlocked[] = {
    { "sched_setattr", 274 }, { "sched_getattr", 275 },
    { "io_uring_setup", 425 }, { "io_uring_enter", 426 }, { "io_uring_register", 427 },
    { "memfd_create", 279 }, { "userfaultfd", 282 }, { "perf_event_open", 241 },
    { "bpf", 280 }, { "kcmp", 272 }, { "lookup_dcookie", 18 }, { "mount", 40 },
    { "umount2", 39 }, { "pivot_root", 41 }, { "swapon", 224 }, { "swapoff", 225 },
    { "reboot", 142 }, { "kexec_load", 104 }, { "init_module", 105 }, { "finit_module", 273 },
    { "delete_module", 106 }, { "open_by_handle_at", 265 }, { "name_to_handle_at", 264 },
    { "setns", 268 }, { "unshare", 97 }, { "ptrace", 117 }, { "process_vm_readv", 270 },
    { "process_vm_writev", 271 }, { "clone3", 435 }, { "landlock_create_ruleset", 444 },
    { NULL, 0 }
};
static const struct VmPolicyProbe kParamAllowed[] = {
    { "write", 64 }, { "writev", 66 }, { "read", 63 }, { "close", 57 }, { "ioctl", 29 },
    { "socket", 198 }, { "connect", 203 }, { "pread64", 67 }, { "mmap", 222 },
    { "rt_sigaction", 134 }, { "fstat", 80 }, { "openat", 56 }, { NULL, 0 }
};
static const struct VmPolicyProbe kKnownKilled[] = {
    { "rseq", 293 }, { "mbind", 235 }, { "get_mempolicy", 236 }, { "set_mempolicy", 237 },
    { "migrate_pages", 238 }, { "move_pages", 239 }, { "keyctl", 250 }, { "add_key", 248 },
    { "request_key", 249 }, { "openat2", 437 }, { "faccessat2", 439 }, { NULL, 0 }
};
static int vm_try_syscall(long nr) {
    pid_t p = fork();
    if (p == 0) {
        prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);   /* v1.2.112：安静死（同 tvm_probe_one） */
        register long x8 __asm__("x8") = nr;
        register long x0 __asm__("x0") = 0, x1 __asm__("x1") = 0, x2 __asm__("x2") = 0;
        __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
        _exit(0);
    }
    if (p < 0) return -1;
    int st = 0;
    waitpid(p, &st, 0);
    if (WIFSIGNALED(st)) return WTERMSIG(st);
    return 0;
}
/* v0.7.71 —— 探针由**独立进程**执行（与 App 无亲缘依赖）。
 * 教训：`SECCOMP_RET_KILL_PROCESS` 杀的是整个进程，App 里 fork 出的子进程照样连坐
 * （tombstone 实锤 syscall 306 / 320）。这里用 fork+execv 起 ROM 之外的独立进程，
 * 它死了与 App 无关，结果经管道回传。 */
static void vm_policy_probe_external(const char* exe) {
    char line[192];
    int n = snprintf(line, sizeof(line), "[TwinVM] [policy] 用独立进程探针：%s\n", exe);
    LOGI("%s", line); live_write(line, (size_t)n);

    static char argbuf[4096];
    char* argv[96];
    int argc = 0; size_t off = 0;
    argv[argc++] = (char*)"probe_policy";
    for (int i = 0; i < VM_BPF_ALLOW_NRS_COUNT && argc < 92; i++) {
        int m = snprintf(argbuf + off, sizeof(argbuf) - off, "%ld", kAllowAudit[i].nr);
        if (m <= 0 || off + (size_t)m + 2 >= sizeof(argbuf)) break;
        argv[argc++] = argbuf + off; off += (size_t)m + 1;
    }
    for (int i = 0; kZygoteBlocked[i].name && argc < 94; i++) {
        int m = snprintf(argbuf + off, sizeof(argbuf) - off, "%ld", kZygoteBlocked[i].nr);
        if (m <= 0 || off + (size_t)m + 2 >= sizeof(argbuf)) break;
        argv[argc++] = argbuf + off; off += (size_t)m + 1;
    }
    argv[argc] = NULL;

    int pfd[2];
    if (pipe(pfd) != 0) return;
    pid_t p = fork();
    if (p == 0) {
        dup2(pfd[1], 1); close(pfd[0]); close(pfd[1]);
        execv(exe, argv);
        _exit(127);
    }
    close(pfd[1]);
    FILE* f = fdopen(pfd[0], "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            char out[224];
            snprintf(out, sizeof(out), "[TwinVM] [policy] 独立进程结果: %s", line);
            LOGI("%s", out); live_write(out, strlen(out));
        }
        fclose(f);
    }
    int st = 0; waitpid(p, &st, 0);
    snprintf(line, sizeof(line), "[TwinVM] [policy] 探针进程退出：%d\n", WEXITSTATUS(st));
    LOGI("%s", line); live_write(line, (size_t)strlen(line));
}

static void vm_policy_probe(void) {
    char msg[320];
    int n = snprintf(msg, sizeof(msg),
        "[TwinVM] [policy] \u5ba1\u8ba1\u653e\u884c\u6e05\u5355\uff08%d \u6761\uff09\uff1a\u9010\u6761 fork \u5b50\u8fdb\u7a0b\u5b9e\u6d4b\n",
        VM_BPF_ALLOW_NRS_COUNT);
    LOGI("%s", msg); live_write(msg, (size_t)n);
    int killed = 0;
    for (int i = 0; i < VM_BPF_ALLOW_NRS_COUNT; i++) {
        int sig = vm_try_syscall(kAllowAudit[i].nr);
        if (sig == 31) {
            killed++;
            n = snprintf(msg, sizeof(msg),
                "[TwinVM] [policy] !! %s(%ld) -> SIGSYS \u2014 App \u7b56\u7565 KILL\uff01"
                "\u5fc5\u987b\u4ece\u653e\u884c\u6e05\u5355\u6458\u6389\n",
                kAllowAudit[i].name, kAllowAudit[i].nr);
        } else if (sig > 0) {
            n = snprintf(msg, sizeof(msg),
                "[TwinVM] [policy]    %s(%ld) -> \u672c\u8eab\u62a5\u4fe1\u53f7 %d\uff08\u975e KILL\uff09\n",
                kAllowAudit[i].name, kAllowAudit[i].nr, sig);
        } else {
            n = snprintf(msg, sizeof(msg),
                "[TwinVM] [policy]    %s(%ld) -> \u5b89\u5168\n", kAllowAudit[i].name, kAllowAudit[i].nr);
        }
        LOGI("%s", msg); live_write(msg, (size_t)n);
    }
    for (int i = 0; kParamAllowed[i].name; i++) {
        int sig = vm_try_syscall(kParamAllowed[i].nr);
        n = snprintf(msg, sizeof(msg),
            "[TwinVM] [policy] (\u53c2\u6570\u5316) %s(%ld) -> \u4fe1\u53f7 %d%s\n",
            kParamAllowed[i].name, kParamAllowed[i].nr, sig,
            sig == 31 ? " \u2190 App \u7b56\u7565 KILL\uff01" : "");
        LOGI("%s", msg); live_write(msg, (size_t)n);
    }
    for (int i = 0; kKnownKilled[i].name; i++) {
        int sig = vm_try_syscall(kKnownKilled[i].nr);
        n = snprintf(msg, sizeof(msg), "[TwinVM] [policy] (\u5df2\u77e5) %s(%ld) -> \u4fe1\u53f7 %d\n",
                     kKnownKilled[i].name, kKnownKilled[i].nr, sig);
        LOGI("%s", msg); live_write(msg, (size_t)n);
    }
    n = snprintf(msg, sizeof(msg),
        "[TwinVM] [policy] \u653e\u884c\u6e05\u5355\u91cc\u6709 %d \u6761\u4f1a\u88ab App \u7b56\u7565 KILL\n", killed);
    LOGI("%s", msg); live_write(msg, (size_t)n);
}
static volatile int g_notif_inherited = 0;   /* v0.9.0：父进程是否已装好过滤器 */
static volatile int g_notif_suspended = 0;   /* v0.9.1：seccomp(2) 被外层永久挂起 */
static volatile int g_ptrace_backend = 0;    /* v1.0.0：改用 ptrace 后端 */

/* v0.9.1：在独立线程里安装（便于主线程超时判定） */
static volatile int* g_inst_done_p = NULL;
static int* g_inst_rc_p = NULL;
static void* tvm_inst_thread(void* a) {
    int* rc = (int*)a;
    *rc = vm_notif_install(2);
    if (g_inst_done_p) *g_inst_done_p = 1;
    return NULL;
}

extern void (*vm_live_log_hook)(const char*, int);

static char g_dex_path[96] = "/rom/hello.dex";   /* v1.2.100：容器 dex 选择（setDex 写，fork 快照消费） */
static char g_main_class[64] = "";               /* v1.2.108 M2：主类（setMainClass 写；空=从 dex 文件名推） */
static char g_cls_snapshot[64] = "";             /* v1.2.112：fork 前定死的类名快照 */
static int g_boot_mode = 0;                          /* v1.2.118 M3：引导模式（Activity→Bootstrap 反射拉生命周期） */
static void snapshot_main_class(void);           /* 定义在 stage0 前（依赖 live_write） */
static void jni_guest_main(const VmRom* rom, int console_fd) {
    /* v1.0.7 ★★ 关闭**继承来的宿主 fd**（这是 guest 槽位的真正入口！）
     * 真机 v1.0.6 实锤：我在 tvm_tracer_main 的 fork 里加的清理**没生效**，
     * 因为 guest 槽位是 jni_guest_main 内部再 fork 出来的 —— 那条路径没清理，
     * ART 依然拿到宿主 fd（先 99、后来 5）并反复 read → futex 死等。
     * 这里放在**每个槽位进程的第一步**，保证彻底。 */
    /* v1.0.8 ★ 顺序很关键：**先设 console、先打日志，再关 fd**。
     * 真机 v1.0.7 实锤：清理循环（5.7 万次 close）确实执行了（trace 里 #248~#251
     * 就是 close(252..255)），但那行"槽位已清理"**丢失**了 —— 因为 close 与
     * vm_log 的写发生在同一时间窗，日志 fd 被波及。
     * 现在：先 vm_console_fd = console_fd（保证日志通道就绪）→ 打日志 → 再清理。 */
    vm_console_fd = console_fd;
    vm_live_log_hook = tvm_live_log_hook;   /* v1.2.22：引擎诊断直写实时日志 */
    vm_log("▶ 槽位启动：清理继承的宿主 fd（保留 console=%d）…", console_fd);
    {
        int keep[3]; int nk = 0;
        keep[nk++] = console_fd;
        if (vm_notif_fd() >= 0) keep[nk++] = vm_notif_fd();
        /* v1.0.9 ★ 只关**高位** fd（≥64）—— 真机 v1.0.8 实锤：全量关闭
         * (3..255) 把 ART/loader 依赖的 fd 也关了，卡死点从 1444 提前到 348，
         * 属于"治过头"。而原来泄漏的坏 fd 是 99 / 高位的宿主 fd ——
         * 关高位即可命中目标，同时保住 3..63 这些 loader 常用区间。 */
        int closed = 0;
        for (int fd = 64; fd < 512; fd++) {
            int k = 0;
            for (int q = 0; q < nk; q++) if (fd == keep[q]) k = 1;
            if (!k) { if (close(fd) == 0) closed++; }
        }
        vm_log("▶ 槽位 fd 清理完成（关掉 %d 个高位 fd，保留 console=%d）", closed, console_fd);
    }
    vm_machine_setup(rom, (int)getpid(), 1);

    jni_install_fatal_diag();

    vm_log("▶ [1/6] 槽位启动 pid=%d，ROM %u 个文件就绪", (int)getpid(), rom->inode_count);

    const VmRomInode* g = vm_rom_find(rom, "/bin/guest");
    if (!g) { vm_log("ROM 缺 /bin/guest"); _exit(98); }

    /* v0.7.84 ★★★ 顺序修正（本项目的总根源）：
     * 以前是"先装载器、后装过滤器"，而 **vm_loader_prepare() 会执行 guest 代码**
     * （它自己跑 linker 的 ELF 解释器）——也就是说：**装载阶段完全在过滤器之外**。
     * 真机实锤：装载阶段里 guest 用
     *     prctl(PR_SET_NO_NEW_PRIVS=38, 1)
     *     prctl(PR_SET_SECCOMP=22, FILTER, &fprog)
     * 把**它自己的 seccomp 策略**装上了（Android bionic 的 zygote policy，KILL_PROCESS）。
     * 内核里 seccomp 后装的排最前 → 我们的过滤器被盖住 → 之后任何 TRAP 第一条就
     * KILL_PROCESS → 我们的 handler 永远进不去（HND#/TRAP#/[HR] 全零、黑匣子 seq=0、
     * "死前末尾 nr=0"=退出码被当成 syscall 号）。这正是 v0.7.55~83 一直"看不见现场"的根因。
     *
     * 修正：**先装过滤器，再装载**。这样装载阶段 guest 的每一个 syscall
     * （含那两个 prctl）都先经过我们的 handler，被我们拦下/伪造。 */
    /* v0.8.0（路线 A）：优先用 **SECCOMP_RET_USER_NOTIF** 后端 —— 不吃信号，
     * 因此不受"外层 zygote 策略判 KILL 压过我们的 TRAP"影响（真机 v0.7.93 根因）。
     * 可用环境变量 TVM_LEGACY_TRAP=1 回退到旧的 SIGSYS 方案便于对比。 */
    int sr;
    if (getenv("TVM_LEGACY_TRAP")) {
        vm_log("▶ [2/6] 安装拦截器（旧 SIGSYS/TRAP 后端）…");
        sr = vm_sigsys_install(console_fd);
        if (sr != 0) { vm_log("▶ ✗ seccomp 安装失败: %d", -sr); _exit(97); }
        vm_log("▶ [2/6] ✓ 拦截器就绪（SIGSYS/TRAP）");
    } else {
        vm_log("▶ [2/6] 安装拦截器（USER_NOTIF）…");
        if (g_notif_suspended) {   /* v1.0.2 ★ 只要发生过"seccomp 挂起"，就**坚决只走 ptrace** */
            /* 真机 v1.0.1 实锤：seccomp(2) 在超时之后**仍然返回成功**（listener fd=118），
             * 于是 ptrace 与 USER_NOTIF 两个后端**同时启用** → 同一个 syscall 被
             * 双重处理 → guest 拿到不一致的结果 → x0=0xf3 野指针 → SIGSEGV。
             * 所以：一旦判为 suspended，就**忽略**后来的 seccomp 成功，只用 ptrace。 */
            /* v1.0.0 ★ 三条路里 seccomp 已被系统彻底封死（v0.9.1 实测：
             * USER_NOTIF 的 seccomp(2) 永久挂起；TRAP 的 SIGSYS 永不投递）。
             * 改为**纯 ptrace 后端** —— 不装任何 seccomp，靠父进程的
             * PTRACE_SYSCALL 停点做"跳过 + 改写返回值"（见 tvm_tracer_main）。
             * 这也是项目里唯一已验证可用的拦截通道（示踪器早就稳定工作）。 */
            sr = 0;
            g_ptrace_backend = 1;
            vm_log("▶ [2/6] ✓ 拦截器就绪（**ptrace 后端**：seccomp 已被系统封死）");
            /* v1.0.2：ptrace 模式**不启动** USER_NOTIF 监听线程 ——
             * 它会对一个没有请求的 listener 做阻塞 RECV，徒增一个卡住点。 */
            vm_log("▶ [3/6] ptrace 模式：跳过监听线程（拦截在示踪器里完成）");
        } else if (g_notif_inherited && vm_notif_fd() >= 0) {
            /* v0.9.0：父进程已装好，子进程直接继承（不再调用 seccomp(2)，
             * 因为真机实锤它会永久挂起）。 */
            sr = 0;
            vm_log("▶ [2/6] ✓ 复用父进程继承的过滤器（listener fd=%d）", vm_notif_fd());
        } else {
            sr = vm_notif_install(console_fd);
        }
        if (sr != 0) { vm_log("▶ ⚠ USER_NOTIF 安装失败: %d（回退 SIGSYS）", -sr);
                       sr = vm_sigsys_install(console_fd);
                       if (sr != 0) { vm_log("seccomp 拦截安装失败: %d", -sr); _exit(97); } }
        else if (!g_ptrace_backend) {
            vm_log("▶ [2/6] ✓ 拦截器就绪（USER_NOTIF, listener fd=%d）", vm_notif_fd());
            pthread_t nth;
            if (pthread_create(&nth, NULL, vm_notif_loop, NULL) == 0) {
                pthread_detach(nth);
                vm_log("▶ [3/6] ✓ 监听线程已启动");
            } else {
                vm_log("监听线程创建失败 —— 回退 SIGSYS");
                sr = vm_sigsys_install(console_fd);
                if (sr != 0) _exit(97);
            }
        }
    }

    /* v0.8.1 ★ USER_NOTIF 下无法用"改 ucontext 跳转"接管 execve（那是信号机制特有），
     * 于是**干脆不产生这次 execve**：引擎直接把真正的入口（dalvikvm + linker64）
     * 装载好并跳过去。guest 的 /bin/guest 仍然保留在 ROM 里做备用（旧后端用）。 */
    VmLaunch launch;
    int r;
    {
        const VmRomInode* dv = vm_rom_find(rom, "/system/bin/dalvikvm");
        const VmRomInode* ld = vm_rom_find(rom, "/system/bin/linker64");
        if (dv && ld && getenv("TVM_LEGACY_TRAP") == NULL) {
            /* v1.2.100 ★★★【B 容器跑 hello 的真凶】：本函数（fork 后子进程）
             * 的 gargv 一直【写死 /rom/hello.dex】——setDex 改的 g_dex_path
             * 在父进程里，从来没传进来！修：fork 后用 g_dex_path（fork 复制
             * 的是 setDex 已生效的快照 ✓）+ 类名动态推导。
             * （-Xverify:none：冷校验在无 dalvik-cache 下慢千倍，沙箱内无意义） */
            /* v1.2.117 ★★★：改用 g_cls_snapshot（stage0 入口的快照——
             * 112 修的 pargv 只是 /bin/guest 分支与 cmdline 合成源，
             * 这里（fork 后 exec_dyn 的真 gargv）仍是文件名推断 →
             * CNFE 'TwinVM-M2-DemoApp' 三案连发的最终真凶）。 */
            /* v1.2.118 M3：引导模式 → -cp apk:bootstrap.jar，跑 Bootstrap
             * （Activity 类名作 argv[4] 传入，Bootstrap 反射拉生命周期）。
             * 注意：静态表先声明后填充（C 语句顺序）。 */
            static char* gargv[] = { (char*)"dalvikvm", (char*)"-Xverify:none",
                                     (char*)"-cp", g_dex_path,
                                     g_cls_snapshot, NULL };
            static char* gargv_boot[] = { (char*)"dalvikvm", (char*)"-Xverify:none",
                                          (char*)"-cp", NULL,
                                          (char*)"com.twinvm.m3.Bootstrap", NULL };
            static char s_cp2[192];
            static char s_act2[64];
            if (g_boot_mode) {
                snprintf(s_cp2, sizeof(s_cp2), "%s:/system/framework/bootstrap.jar",
                         g_dex_path);
                snprintf(s_act2, sizeof(s_act2), "%s", g_cls_snapshot);
                gargv_boot[3] = s_cp2;
                gargv_boot[4] = s_act2;   /* argv[4]=Activity 类名（Bootstrap args[0]） */
            }
            char** const use_argv = g_boot_mode ? gargv_boot : gargv;
            static char* genvp[] = {
                (char*)"BOOTCLASSPATH=/apex/com.android.art/javalib/core-oj.jar:"
                       "/apex/com.android.art/javalib/core-libart.jar:"
                       "/apex/com.android.i18n/javalib/core-icu4j.jar:"
                       "/apex/com.android.art/javalib/okhttp.jar:"
                       "/apex/com.android.art/javalib/bouncycastle.jar:"
                       "/apex/com.android.art/javalib/apache-xml.jar",
                /* v1.2.50 ★ 第六件 core-icu4j（真机 init.environ.rc 形态：第三位）：
                 * 真机 v1.2.49 实锤——libicu_jni 的 JNI 注册 findClass
                 * "com/android/icu/text/TimeZoneNamesNative" 失败（该类定义在
                 * i18n APEX 的 core-icu4j.jar，core-libart 只引用不定义；
                 * dex 解析实锤：core-libart defs=545 无 icu 类，
                 * core-icu4j defs=1795 含 TimeZoneNamesNative）。
                 * DEX2OAT 按 ART 前缀规则同步补成前三件。 */
                (char*)"DEX2OATBOOTCLASSPATH=/apex/com.android.art/javalib/core-oj.jar:"
                       "/apex/com.android.art/javalib/core-libart.jar:"
                       "/apex/com.android.i18n/javalib/core-icu4j.jar",
                (char*)"ANDROID_ROOT=/system",
                (char*)"ANDROID_DATA=/data",
                (char*)"ANDROID_ASSETS=/system/app",
                (char*)"ANDROID_STORAGE=/storage",
                /* v1.2.48 ★★★ APEX 时代环境变量三件套（真机由 zygote 设置）：
                 * 真机 v1.2.47 实锤——libicuuc 读不到 ANDROID_TZDATA_ROOT 直接
                 * FATAL（"ANDROID_TZDATA_ROOT environment variable not set"，
                 * 串在 apex i18n 的 libicuuc.so；紧接着 signal 6 abort）。
                 * libicu_jni.so 装载 ok 后第一件事就是 ICU 初始化。 */
                (char*)"ANDROID_ART_ROOT=/apex/com.android.art",
                (char*)"ANDROID_TZDATA_ROOT=/apex/com.android.tzdata",
                (char*)"ANDROID_I18N_ROOT=/apex/com.android.i18n",
                (char*)"LD_LIBRARY_PATH=/system/lib64",
                (char*)"PATH=/system/bin:/system/xbin",
                (char*)"TMPDIR=/data/local/tmp",
                NULL };
            vm_log("▶ [4/6] 装载 dalvikvm + linker64…");
            r = vm_loader_exec_dyn(dv->map, dv->size, ld->map, ld->size,
                                   "/system/bin/dalvikvm", use_argv, genvp, &launch);
            if (r != 0) { vm_log("exec_dyn 失败: %d —— 退回 /bin/guest", r); r = -1; }
        } else {
            r = vm_loader_prepare(g->map, g->size, &launch);
        }
    }
    if (r != 0) { vm_log("装载器失败: %d", r); _exit(98); }

    vm_log("▶ [5/6] ✓ 装载完成 entry=%p，跳转入 guest（ART 启动）", (void*)launch.entry);
    vm_log("▶ [6/6] 已进入 ART，后续输出见下方实时日志");
    vm_loader_jump(&launch);   /* guest 的 exit_group 结束本进程 */
    _exit(99);
}

/* ═══════════════ v0.7.58 诊断双管 ══════════════════════════════════════
 * 死因链：v0.7.56 已把 BPF 末位改 TRAP（未知 syscall → handler → ENOSYS 大声日志），
 * 但 v0.7.57 真机依然无声 code=-31 且无 TRAP 行 —— 结论：**App（zygote）seccomp
 * 策略对某个 syscall 判了 KILL_PROCESS**。seccomp 多层过滤取最严判决，KILL 赢过
 * 我们的 TRAP：handler 不运行、黑匣子记不到、日志一片死寂。
 *
 * 双管齐下：
 *   ① 全量扫雷（0..500）：fork 子进程（只带 App 策略、不带引擎过滤器）逐号裸调，
 *      死于 31 的 = App 策略真 KILL —— 一次画出完整地雷图；
 *   ② ptrace 示踪器：内核 syscall 入口路径是「先 ptrace 停点、后 seccomp 评估」
 *      （kernel 原注释：Do seccomp after ptrace）—— 所以示踪器能看见 guest
 *      临死前的每一个 syscall（含被 KILL 的那个）。死前最后一个 entry = 凶手。 */

static int tvm_probe_one(int nr) {
    pid_t p = fork();
    if (p == 0) {
        /* v1.2.112：安静死——实测子进程死于 SIGSYS 是设计内结果，
         * PR_SET_DUMPABLE(0) 让 debuggerd/Logfox 不抓（消 tombstone 噪音，
         * 16:17 那包 25 连"闪退"实为扫雷子进程的正常死法） */
        prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
        alarm(1);                    /* 挂死自兜（有些 syscall 会阻塞） */
        register long x8 __asm__("x8") = nr;
        register long x0 __asm__("x0") = 0, x1 __asm__("x1") = 0, x2 __asm__("x2") = 0;
        __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
        _exit(0);
    }
    if (p < 0) return 0;
    for (int i = 0; i < 12; i++) {   /* 父端 1.2 秒保底（v0.7.59 提速） */
        int st;
        pid_t r = waitpid(p, &st, WNOHANG);
        if (r == p) return WIFSIGNALED(st) ? WTERMSIG(st) : 0;
        usleep(100000);
    }
    kill(p, SIGKILL);
    int st;
    waitpid(p, &st, 0);
    return -1;                       /* 超时 = 挂 */
}

static void vm_policy_sweep(void) {
    char msg[1024];
    int n = snprintf(msg, sizeof(msg),
        "[TwinVM] [sweep] v0.7.59 全量 syscall 扫雷（0..500，仅 App 策略，参数 0,0,0）...\n"
        "[TwinVM] [sweep] 注意: 参数全 0 —— 策略里带参数条件的规则可能过报（真凶以 [trace] 为准）\n");
    LOGI("%s", msg);
    live_write(msg, (size_t)n);
    int kill31[512], nk = 0, nhang = 0, nother = 0;
    time_t t0 = time(NULL);
    for (int nr = 0; nr <= 500; nr++) {
        if (time(NULL) - t0 > 50) {  /* v0.7.59：预算 25→50 秒，目标跑完全程 */
            n = snprintf(msg, sizeof(msg), "[TwinVM] [sweep] 时间预算用尽，只扫到 nr=%d\n", nr);
            live_write(msg, (size_t)n);
            break;
        }
        int sig = tvm_probe_one(nr);
        if (sig == 31) kill31[nk++] = nr;
        else if (sig == -1) nhang++;
        else if (sig) nother++;
    }
    n = snprintf(msg, sizeof(msg),
        "[TwinVM] [sweep] App 策略 KILL 名单（死于 31）共 %d 个：\n[TwinVM] [sweep]  ", nk);
    for (int i = 0; i < nk && n < (int)sizeof(msg) - 16; i++)
        n += snprintf(msg + n, sizeof(msg) - (size_t)n, "%d ", kill31[i]);
    n += snprintf(msg + n, sizeof(msg) - (size_t)n,
                   "\n[TwinVM] [sweep]  （另：挂 %d 个、其它信号死 %d 个=参数无效，非策略）\n",
                   nhang, nother);
    LOGI("%s", msg);
    live_write(msg, (size_t)n);
}

/* ── ptrace 示踪器（v0.7.60：fork/握手/等待全在本线程——ptrace 停点只能由
 * 追踪者任务本人 wait；别的线程 waitpid(-1) 立刻 ECHILD，v0.7.59 真机实锤：
 * 握手 5/0/0 成功但 0 事件 0 输出，子进程冻死在第一个 syscall 入口停点）── */
#define TVM_PSI_GSI   0x420e        /* PTRACE_GET_SYSCALL_INFO，自带定义防头文件差异 */
#define TVM_PSI_ENTRY 1
#define TVM_RING      256
struct tvm_psi {                     /* 内核 ptrace_syscall_info 布局 */
    unsigned char op, pad[3];
    unsigned int arch;
    unsigned long instruction_pointer, stack_pointer;
    union {                           /* v0.7.64：union 按最大成员（seccomp=64B）补全
                                       * 内核 sizeof=88B；只放 entry(56B) 会传 len=80
                                       * → PTRACE_GET_SYSCALL_INFO 直接 EINVAL
                                       * → v0.7.63 真机 0 事件的根因！ */
        struct { unsigned long nr, args[6]; } entry;                    /* 56 */
        struct { unsigned long nr, args[6], ret_data; } seccomp;        /* 64 */
        /* v1.0.3：exit 停点（内核 ptrace_syscall_info.op=2 用这一支） */
        struct { unsigned long nr; long rval; int is_error; } exit_stop;
        unsigned char _reserve[64];
    } u;
};
static pthread_t g_t_thr;
static int      g_t_pid;
static volatile int g_t_done, g_t_status, g_t_events;
/* v1.2.19 ★★★ 看门狗的判据修正（用户实测指出）：
 * 旧判据是"**90 秒无控制台输出**"，但日志只记录**被拦截的** syscall ——
 * 而 msync/mmap/mprotect/futex/brk 等**不在拦截清单**的 syscall 干活再久也一行不输出。
 * 真机 v1.2.18 实锤：guest 正在跑 msync 前向遍历（地址 0x3c3de000→0x3d01f000 递增、
 * State: R running），却被判"90 秒无输出＝卡死"并强杀。
 * 正解：**用 guest 的真实活动（任何 ptrace 停点）来重置计时** ——
 * 只要 guest 还在产生停点，就说明它在前进，绝不能判卡死。 */
static volatile unsigned long long g_guest_ticks = 0;   /* 每个 ptrace 停点 +1 */
/* v1.2.57：优雅关机 v2 的独立时钟——只在**真实控制台输出**时更新（v1.2.19
 * 的 last_out 会被收尾打转刷新，不可用作"演完了"判据）。 */
static time_t g_last_real_out = 0;                  /* 最后一次真实输出的时刻 */
static char g_logs_dir[480] = "";                   /* v1.2.61：live 日志目录（缓存用） */
static unsigned long long g_ticks_at_real_out = 0;  /* 该时刻的停点总数 */
static unsigned long long g_guest_ticks_prev_sec = 0; /* 秒采样基准 */
static unsigned long long g_sec_now = 0;            /* 秒计数 */
static volatile unsigned long long g_t_last_pc = 0;     /* 最近一次 syscall 停点的 pc */
static volatile unsigned long long g_mmap_hint = 0, g_mmap_len = 0;   /* v1.2.23：匿名 mmap 的 hint */
/* v1.2.91：低 4GB 强制落位状态（exit 侧发现未满足 → entry 侧接管） */
static volatile int g_low4_active = 0;
static volatile int g_low4_rewound = 0;   /* v1.2.92：rewind 重执行标志 */
static volatile unsigned long long g_low4_want_len = 0;
/* v1.2.93 ★ 落位记账（修 rewind 重复落位：v1.2.92 真机实锤两次 512MB
 * 都 FIXED 到 0x12000000——第二块直接覆盖第一块（ART heap 被踩）。
 * 落过的区间记下来，空洞查找时排除。 */
#define TVM_LOW4_USED 16
static struct { unsigned long long lo, len; int used; } g_low4_used[TVM_LOW4_USED];
static void tvm_low4_mark(unsigned long long lo, unsigned long long len) {
    for (int i = 0; i < TVM_LOW4_USED; i++)
        if (!g_low4_used[i].used) {
            g_low4_used[i].used = 1;
            g_low4_used[i].lo = lo; g_low4_used[i].len = len;
            return;
        }
}
/* 在 guest 的 /proc/<pid>/maps 里找低 4GB 的空洞（≥len，对齐 1MB）。
 * 返回 0=没找到。 */
static unsigned long long tvm_low4_hole(pid_t pid, unsigned long long len) {
    /* 收集低 4GB 的已映射区间，排序找间隙 */
    enum { MAXSEG = 96 };
    static struct Seg { unsigned long long lo, hi; } seg[MAXSEG];
    int n = 0;
    char path[48], ln[256];
    snprintf(path, sizeof(path), "/proc/%d/maps", (int)pid);
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    while (n < MAXSEG && fgets(ln, sizeof(ln), f)) {
        unsigned long long a, b;
        if (sscanf(ln, "%llx-%llx", &a, &b) == 2 && b <= 0x100000000ULL) {
            seg[n].lo = a; seg[n].hi = b; n++;
        }
    }
    fclose(f);
    /* v1.2.93：并入落位记账区（FIXED 已占的也算已用——防重复落位覆盖） */
    for (int i = 0; i < TVM_LOW4_USED && n < MAXSEG; i++)
        if (g_low4_used[i].used) {
            seg[n].lo = g_low4_used[i].lo;
            seg[n].hi = g_low4_used[i].lo + g_low4_used[i].len;
            n++;
        }
    /* 排序（插入即可，段数少） */
    for (int i = 1; i < n; i++) {
        struct Seg k = seg[i];
        int j = i - 1;
        while (j >= 0 && seg[j].lo > k.lo) { seg[j+1] = seg[j]; j--; }
        seg[j+1] = k;
    }
    /* v1.2.94 ★ 候选枚举重写（修 v1.2.93 记账未生效之谜）：
     * 逐间隙从高往低枚举所有候选，每个先 1MB 对齐再【双重校验】：
     *   ① 与记账表不重叠；② 与本函数收集的段不重叠（防御排序/时序
     *      边角——段表里本该有记账区，但窗口期可能不同步）。
     * 校验不过就换下一个间隙，绝不返回与已用区冲突的地址。 */
    unsigned long long prev_hi2 = 0;
    for (int i = 0; i <= n; i++) {
        unsigned long long gap_end = (i == n) ? 0x100000000ULL : seg[i].lo;
        if (i > 0) {
            unsigned long long gap_start = prev_hi2;
            if (gap_end > gap_start && gap_end - gap_start >= len + 0xFFFFFULL) {
                unsigned long long cand = (gap_start + 0xFFFFFUL) & ~0xFFFFFULL;
                /* 校验①：记账表不重叠 */
                int overlap = 0;
                for (int k = 0; k < TVM_LOW4_USED; k++)
                    if (g_low4_used[k].used
                        && cand < g_low4_used[k].lo + g_low4_used[k].len
                        && cand + len > g_low4_used[k].lo) { overlap = 1; break; }
                /* 校验②：段表不重叠 */
                if (!overlap)
                    for (int k = 0; k < n; k++)
                        if (cand < seg[k].hi && cand + len > seg[k].lo) { overlap = 1; break; }
                if (!overlap) return cand;
            }
        }
        if (i < n) prev_hi2 = seg[i].hi;
    }
    return 0;
}
static volatile int g_mmap_pend = 0;
/* v1.2.25：循环/卡死现场的抓帧请求（看门狗或 [loop] 检测器置位） */
/* v1.2.31 ★★★ **futex 调用环形留痕**。
 * 目的：v1.2.29/30 两轮都卡在 `futex(uaddr, FUTEX_WAIT|PRIVATE, 期望值=3, 无超时)`，
 * 而"谁该唤醒它"始终没证据。只记最后 256 次 futex 调用（op/uaddr/val），
 * 卡死时整表倒出来 —— 一眼就能分清三种情形：
 *   ① 只有 WAIT、**从无 WAKE** → 等一个不存在的线程/丢掉唤醒
 *   ② 同一地址先 LOCK 再 WAIT（无 UNLOCK） → 自死锁
 *   ③ WAKE 有但 waiter 数为 0 → 唤醒发早了（竞态）
 * `FUTEX_WAIT`=0、`FUTEX_WAKE`=1、`FUTEX_REQUEUE`=3、`FUTEX_CMP_REQUEUE`=4、
 * `FUTEX_WAKE_OP`=5、`FUTEX_LOCK_PI`=6、`FUTEX_UNLOCK_PI`=7、`FUTEX_WAIT_BITSET`=9；
 * 与 `FUTEX_PRIVATE_FLAG`(0x80) 按位或。 */
#define TVM_FUTEX_RING 256
static struct { long op; unsigned long long uaddr, val; } g_futex_ring[TVM_FUTEX_RING];
static volatile int g_futex_idx = 0;
/* v1.2.34：syscall nr 直方图环形（256 条，体检时统计 top5）。
 * 真机 v1.2.33 现场是 munmap/msync/mmap 三重奏死循环，只能人肉数 trace
 * 序号猜循环成员；直方图让体检报告直接给出"转圈的都有谁、各占多少"。 */
#define TVM_NR_RING 256
static long g_nr_ring[TVM_NR_RING];
static volatile int g_nr_idx = 0;

/* v1.2.32 ★★★★ **失败 syscall 留痕**。
 * 真机 v1.2.31 已把根因收窄到：**整个运行里 clone/clone3 一次都没发生**
 * （黑匣子只采到 98 futex 与 227 msync），而 guest 卡在等"只可能由另一个线程
 * 送来的唤醒"上 —— 即 ART 需要的第二个线程从未被创建。
 * 于是问题变成：**`pthread_create` 为什么在 `clone` 之前就失败了？**
 * bionic 的 pthread_create 在 clone 之前要做：mmap 线程栈、mprotect 守卫页、
 * `__init_tls`（读 auxv/program headers）、rt_sigprocmask 等 —— 任一步返回错误
 * 都会"静默返回失败、不调 clone"。
 * 所以这里盯住这条链上的 syscall 返回值：**只看 watchlist，且只在返回 -errno 时打印**
 * （每个 nr 最多 6 条），对吞吐影响可忽略。 */
static struct { long nr; long err; int cnt; } g_fail[64];
static volatile int g_nfail = 0;
/* v1.2.33：记录 watchlist syscall 的 entry 参数，供失败时一并打出。
 * 只有 nr/errno 无法定位问题：mmap 的 EINVAL（-22）必须看 hint/len/prot/flags/fd/off
 * 才知道是"MAP_FIXED + 非页对齐"、"长度非法"还是"地址越界"。 */
static unsigned long long g_wargs[6];
static volatile long g_wargs_nr = -1;
static int tvm_fail_watch(long nr) {
    switch (nr) {
        case 222: /* mmap */      case 215: /* munmap */   case 226: /* mprotect */
        case 220: /* clone */     case 435: /* clone3 */   case 214: /* brk */
        case 98:  /* futex */     case 135: /* rt_sigprocmask */
        case 167: /* prctl */     case 99:  /* set_robust_list */
        case 96:  /* set_tid_address */  case 261: /* prlimit64 */
        case 163: /* getrlimit */ case 260: /* wait4 */
        case 123: /* sched_getaffinity */ case 132: /* sigaltstack */
        case 134: /* rt_sigaction */     case 172: /* getpid */
        case 178: /* gettid */           case 293: /* rseq */
            return 1;
        default: return 0;
    }
}

static const char* tvm_futex_op_name(long op) {
    switch (op & 0x7f) {
        case 0: return "WAIT";
        case 1: return "WAKE";
        case 2: return "FD";
        case 3: return "REQUEUE";
        case 4: return "CMP_REQUEUE";
        case 5: return "WAKE_OP";
        case 6: return "LOCK_PI";
        case 7: return "UNLOCK_PI";
        case 8: return "TRYLOCK_PI";
        case 9: return "WAIT_BITSET";
        case 10: return "WAKE_BITSET";
        default: return "?";
    }
}

static volatile int g_want_stack = 0;
static volatile int g_stack_done = 0;
/* v1.2.23 ★★★ 为什么需要这个 volatile 中转：
 * clang 在 -O1 及以上会把 `if (tvm_getregs(...) == 0) { 拦截主体 }` **整块消除**
 * —— 构建产物里 [caller]/SETREGSET/已拦截 串全部消失（-O0 时还在）。
 * 而拦截主体里含 `rr.regs[8] = -1`（让内核跳过原 syscall）与 `SETREGSET`，
 * 一旦被消除，**拦截就形同虚设**（guest 原生执行 + 我们再回填 x0 → 结果错乱）。
 * 经 file-scope volatile 中转后，编译器无法推导该值（可能被信号处理/其它线程改），
 * 条件不再可证，主体必然保留。 */
static volatile long g_getregs_rc = 0;
static volatile int g_t_last_nr = -1;
static volatile pid_t g_t_last_tid;
static char g_t_ring[TVM_RING][96];
static int  g_t_ring_idx, g_t_ring_cnt;

struct tvm_launch { VmRom* rom; void* bb; int rfd, wfd; };
static struct tvm_launch g_launch;
static sem_t g_launch_sem;


/* ── v1.0.0：ptrace 后端 —— 把"只观察"升级为"拦截 + 改写返回值" ──
 * 背景：本设备上自装 seccomp 被策略封死（v0.9.1 实锤），改走 ptrace。
 * aarch64 寄存器：x0..x30 / sp / pc / pstate（NT_PRSTATUS）。 */
struct tvm_arm_regs { unsigned long long regs[31], sp, pc, pstate; };
#define TVM_NT_PRSTATUS 1
static long tvm_getregs(pid_t tid, struct tvm_arm_regs* r) {
    struct iovec iov; iov.iov_base = r; iov.iov_len = sizeof(*r);
    return ptrace(PTRACE_GETREGSET, tid, (void*)(long)TVM_NT_PRSTATUS, &iov);
}
static long tvm_setregs(pid_t tid, struct tvm_arm_regs* r) {
    struct iovec iov; iov.iov_base = r; iov.iov_len = sizeof(*r);
    return ptrace(PTRACE_SETREGSET, tid, (void*)(long)TVM_NT_PRSTATUS, &iov);
}
/* 需要虚拟化的号（ptrace 无自递归问题，可放宽） */
static int tvm_virt_nr(long nr) {
    switch (nr) {
        case 56: case 57: case 62: case 63: case 64: case 66: case 67:
        case 78: case 79: case 80: case 89: /* 137 已移除（v1.2.55） */
        case 160: case 17:
        case 49: case 48: case 29: case 61: case 25:
        case 96: case 99: case 163: case 167: case 43: case 44: case 221:
        case 282: /* v1.2.84 ★ userfaultfd：guest 拿到真 uffd 后 ART 的
                    uffd-heap 模式在 VM 里无 handler → 构造函数 mprotect
                    死循环（v1.2.80~83 四版定罪：call_constructors 内循环）。
                    拦下回 EPERM → ART 降级普通 heap（无 uffd 路径），
                    引擎里 userfaultfd 已有 EPERM 语义。 */            /* ★★★ 本行 `return 1;` 必须保留！
             * v1.2.23 曾因删除 `case 227:` 时把它一并删掉，
             * 导致 `tvm_virt_nr()` **永远返回 0 → syscall 拦截被整体关闭**
             *（连锁现象：tvm_intercept_now 被判"未使用"而删除、构建产物里
             *  [caller]/SETREGSET/已拦截 串全部消失、只剩失败分支）。
             * 教训：删 case 时先确认它有没有与其它 case **共享语句**。 */
            return 1;
        /* v1.2.19 ★★★ 补回被 ptrace 迁移漏掉的 msync(227)。
         * 引擎里 v0.7.41 就有正确实现（`case VM_NR_msync: return 0;`，
         * 当年血泪修复：ART 对映射调 msync，拿到 ENOSYS 就**无限重试 8.7M 次**，
         * UI 永久"运行中"）。但 SIGSYS 后端靠"不在放行清单即 TRAP"自动覆盖到它，
         * 而 ptrace 后端是**显式白名单** —— 227 没写进来 → 引擎那段成了**死代码**
         * → 请求直达真实内核 → 对未映射区间返回 ENOMEM → ART 逐页往前扫
         * （真机 v1.2.18 实锤：地址 0x39268000→0x3d01f000 单调递增、约 1MB/s，
         *  每页一次 MS_SYNC 真回写）。加回后走引擎的 return 0：语义安全
         * （ROM 只读、映射皆 MAP_PRIVATE），且**立刻返回**。 */
        /* v1.2.23 ★★★ **撤销 v1.2.19 对 msync(227) 的拦截** —— 那是一处架构性错误。
         * 原因：ptrace 后端下引擎运行在**父进程**，而 guest 在**子进程**；
         * 凡"操作 tracee 内存地址"的 syscall（mmap/munmap/mprotect/mremap/msync/madvise/brk），
         * 引擎**根本无法替它完成**（父进程的 memcpy/mmap 只会作用于自己的地址空间）。
         * 于是 `case VM_NR_msync: return 0;` 变成一句**谎言**：
         * 它对任意地址都答"成功"，而 ART 的 `MapInternalArtLow4GBAllocator`
         * 恰恰**用 msync 的成败来判断候选区间是否可用**（真机 v1.2.20/21 实锤：
         * 它在 0x02000000 以下逐页 msync、地址单调递增、永不收敛）。
         * 让 msync **不被拦截 → 由子进程原生执行 → 内核给出真实答案**，
         * 才是这段逻辑唯一正确的语义。
         * （v0.7.41 的"8.7M 次重试"是 SIGSYS 后端回 ENOSYS 造成的，与原生行为无关。） */
        /* v1.0.1 ★ 明确**不拦截**的号：
         *   process_vm_readv/writev(270/271)：它们是引擎的安全访存原语，
         *     guest 侧拿它们读自身内存是正常行为；v1.0.0 真机把它们拦下来后
         *     引擎返回值不对 → 调用方拿到 0xf3 野指针 → SIGSEGV（GUEST FAULT 实锤）。
         *   mmap/mprotect/munmap/brk：内存原语，ptrace 改写风险高收益低。
         *   futex/clone/rt_sigprocmask：线程与信号基础设施。
         *   exit/exit_group：收尾。
         *   getpid/gettid 等：让它们走真实内核（同一进程，值本就正确）。
         * v1.2.38 ★★★ 补充：**socket(198)/connect(203) 也移出拦截**——
         *   ptrace 后端下"被拦截的 syscall 其实会先原生执行（arm64 在 entry
         *   锁定 syscall 号）"，引擎最后覆盖返回值；而 socket 的旧处理是
         *   **引擎在 tracer 进程里原生 socket()**——返回的 fd 属于 tracer，
         *   **guest 的 fd 表里根本没有**（fd 所有权 bug）→ guest 的
         *   connect → EBADF → liblog logd 写失败 → __android_log_assert
         *   (FATAL) → abort（真机 v1.2.37 实锤：liblog assert 链
         *   Runtime::Abort←libbase←liblog 已符号化定罪）。
         *   改为 guest 原生 socket/connect：fd 就在 guest 里，connect 到
         *   宿主 /dev/socket/logd（App 权限足够）→ 日志进宿主 logcat（额外福利）。
         *   （sigsys 后端同进程不受影响——vm_core.c 的 case 198 保留。） */
        case 198: case 203:
        case 270: case 271: case 222: case 226: case 215: case 214:
        case 98: case 220: case 135: case 132: case 94: case 93:
        case 172: case 178: case 173: case 174: case 175: case 176: case 177:
            return 0;
        default: return 0;
    }
}
/* 每个 tid 的"待办虚拟化"记录（简单表，容量 64） */
#define TVM_VIRT_SLOTS 64
struct tvm_virt_slot { pid_t tid; long nr; int active; unsigned long long args[6]; unsigned long long seq; };
static struct tvm_virt_slot g_virt[TVM_VIRT_SLOTS];
static unsigned long long g_virt_seq = 0;   /* v1.1.2：entry/exit 配对序号 */
/* v1.2.17：主线程（看门狗）→ tracer 的体检请求标志（ptrace 只认 tracer 线程） */
static volatile int g_hang_probe = 0;
static volatile int g_probe_stop = 0;   /* v1.2.18：本次停点由我们 PTRACE_INTERRUPT 造成，SIGSTOP 必须吞掉 */

/* ── v1.2.0 ★★★ mmap(虚拟 fd) 的"参数改写 + 原生执行 + 内容回填" ──
 * 真机 v1.1.9 实锤：guest linker 加载 liblog.so，openat/fstat/readlink 全过，
 * 卡在 fstatfs(44) EBADF（名单漏了，本轮已补）；下一个必死点 = mmap(222)
 * 带虚拟 fd 直通宿主内核（fd=10000 在真内核不存在 → EBADF）。
 * 引擎的 mmap 分支是 SIGSYS 时代设计（vm_syscall6+memcpy 在**调用者**进程跑
 * ——ptrace 后端下会映射进父进程，guest 拿到的是父进程地址，完全错误）。
 * 正解（本项目称 mmap-fill）：
 *   entry：不改 x8（不跳过），把参数改写成 匿名+可写（保 MAP_FIXED/hint），
 *          fd=-1、offset=0 —— 让**子进程的内核**真造页，返回真实 guest 地址；
 *   exit ：x0=guest 新页地址，父进程用 process_vm_writev 把 ROM inode 内容
 *          灌进去（源=父进程 ROM 映射，fork 继承同地址，本就共享）。
 * 权限收紧留给 guest linker 自己的 mprotect（bionic 装完段必 mprotect）；
 * i-cache：全新匿名页从未执行过，无陈旧行；linker 重定位后也会自 flush。
 *   （风险已知：REUSE 的地址段若曾是代码页则有 I$ 陈旧风险——[sig] 会现形。） */
#define TVM_FILL_SLOTS 64         /* v1.2.35：8→64——dlopen 大库并发段多，8 槽有耗尽风险 */
struct tvm_fill_slot { pid_t tid; int active; long fd; unsigned long long len, off;
                        unsigned long long at; char path[52]; };   /* v1.2.35：at+路径——体检符号化直查 */
static struct tvm_fill_slot g_fill[TVM_FILL_SLOTS];
/* v1.2.35 ★★ fill 段登记（at→路径）：真机 v1.2.34 实锤 maps 里 fill 灌的段全是
 * 无名 rwxp，sp 扫描只能打 "[anon]+偏移"，要回本地用 exec 段大小指纹猜库。
 * fill 完成时把 at/len/path 登记在此，体检时直接打印库名。 */
#define TVM_SEGNOTE 256
static struct { unsigned long long lo, hi; char path[52]; int used; } g_segnote[TVM_SEGNOTE];
static void tvm_segnote_add(unsigned long long at, unsigned long long len, long fd) {
    const char* p = "fd?";
    VmCpu* cpu = vm_machine_current();
    if (cpu) {
        int i = (int)fd - VM_FD_BASE;
        if (i >= 0 && i < VM_MAX_FILES && cpu->vfs && cpu->vfs->files[i].in_use
            && cpu->vfs->files[i].rom_idx >= 0)
            p = cpu->vfs->rom->inodes[cpu->vfs->files[i].rom_idx].name;
        else if (i >= 0 && i < VM_MAX_FILES && cpu->vfs && cpu->vfs->files[i].in_use
                 && cpu->vfs->files[i].rom_idx == -3)
            p = "/apex 直读(宿主)";
    }
    for (int k = 0; k < TVM_SEGNOTE; k++) {
        if (!g_segnote[k].used) {
            g_segnote[k].used = 1;
            g_segnote[k].lo = at;
            g_segnote[k].hi = at + len;
            strncpy(g_segnote[k].path, p, 51);
            return;
        }
    }
}
static const char* tvm_segnote_find(unsigned long long a, unsigned long long* off_out) {
    for (int k = 0; k < TVM_SEGNOTE; k++)
        if (g_segnote[k].used && a >= g_segnote[k].lo && a < g_segnote[k].hi) {
            if (off_out) *off_out = a - g_segnote[k].lo;
            return g_segnote[k].path;
        }
    return NULL;
}

/* ── v1.1.4 ★ write/writev(fd≤2) payload 抄送 ──
 * 动机（v1.1.3 真机日志实锤）：进 ART 后仅 31 个 syscall，其中 3 次
 * writev(fd=2) 是 ART 临终的 stderr 报错原文，随后 EXIT=0 —— 但 trace 行只
 * 记了参数指针，**报错内容没进 live 日志**（等于 abort 原因仍在黑洞里）。
 * 做法：entry 停点（tracee 静止、参数内存完整）用 PEEKDATA 把 payload
 * 读出来、ASCII 化后抄进 live 日志。刻意只用 PEEKDATA —— 零新 syscall，
 * 不碰外层 seccomp 红线（process_vm_readv(270) 虽在 sweep 里不在 KILL 名单，
 * 但 PEEKDATA 是已被握手实证可用的唯一原语，风险最低）。 */
static long tvm_peek(pid_t t, unsigned long long addr) {
    errno = 0;
    long w = ptrace(PTRACE_PEEKDATA, t, (void*)(uintptr_t)addr, 0);
    return (errno == 0) ? w : -1;
}
/* v1.1.6 ★ 读 tracee 内存改用 process_vm_readv（引擎 vm_mem_read 同款原语）。
 * 真机 v1.1.5 实锤：PEEKDATA 对 guest 的栈/堆地址屡屡 EIO（[tee] 全部"0 B"），
 * 而引擎的 process_vm_readv 路径在同一时刻工作正常——不再依赖 PEEKDATA，
 * 保留它做兜底 + errno 落日志（下一轮直接看到失败原因）。 */
static int tvm_read_pv(pid_t t, unsigned long long addr, size_t n, char* out) {
    if (n == 0) return 0;
    /* bionic 头不带 _GNU_SOURCE 不暴露声明——自声明（API 23+ 符号存在） */
    extern ssize_t process_vm_readv(pid_t, const struct iovec*, unsigned long,
                                     const struct iovec*, unsigned long, unsigned long);
    struct iovec l = { out, n }, r = { (void*)(uintptr_t)addr, n };
    if (process_vm_readv(t, &l, 1, &r, 1, 0) == (ssize_t)n) return 0;
    int e1 = errno;
    /* 兜底：PEEKDATA 逐字 */
    size_t got = 0;
    while (got < n) {
        long w = tvm_peek(t, addr + got);
        if (errno != 0) {
            static int s_rd = 0;
            if (s_rd++ < 8)
                ptrace_log("[tee] 读 guest 内存失败 addr=0x%llx off=%zu pv_errno=%d peek_errno=%d（tid=%d）",
                           (unsigned long long)addr, got, e1, errno, (int)t);
            return -1;
        }
        size_t c = n - got < 8 ? n - got : 8;
        memcpy(out + got, &w, c);
        got += c;
    }
    return 0;
}
static size_t tvm_read_tracee(pid_t t, unsigned long long addr, size_t n, char* out) {
    if (tvm_read_pv(t, addr, n, out) == 0) return n;
    return 0;                     /* 失败：返回 0（老语义），errno 已由 pv 落日志 */
}

/* v1.2.62 ★ 优雅关机 v6：判定核心（tracer 线程专用，停点驱动+静默驱动共用）。
 * 返回 1=已触发（调用方应退出示踪循环）。 */
static int tvm_graceful_check(int kill_it) {
    static time_t t_last_chk = 0;
    static unsigned long long t_peak = 0, t_prev = 0;
    static time_t t_rate_sec = 0;
    static time_t t_grace_done = 0;
    static time_t t_watch_last = 0;              /* v1.2.65：tracer 侧 watch */
    time_t tnow = time(NULL);
    if (tnow == t_last_chk) return 0;           /* 每秒至多一次 */
    t_last_chk = tnow;
    {   /* v1.2.67 ★ 灵异排查：check 到底在跑吗（前 60 次逐条直播） */
        static int dbg_n = 0;
        if (dbg_n < 60) { dbg_n++;
            char db[192];
            int dk = snprintf(db, sizeof(db),
                "[chk] #%d t=%ld 距真出%lds g_lro=%ld\n",
                dbg_n, (long)tnow, g_last_real_out ? (long)(tnow - g_last_real_out) : -1L,
                (long)g_last_real_out);
            live_write(db, (size_t)dk);
        }
    }
    /* v1.2.65 ★ tracer 侧 watch 心跳（主线程死后也有观测）：真实输出停止
     * 2s 后每 2s 打判据值——下次日志自带答案。 */
    if (g_last_real_out > 0 && tnow - g_last_real_out >= 2
        && tnow - t_watch_last >= 2) {
        t_watch_last = tnow;
        char wb[288];
        int wk = snprintf(wb, sizeof(wb),
            "[TwinVM] [t-watch] 距真实输出 %lds、窗口停点 %llu、"
            "峰值 %llu/s（分级判据：≥%d停点/s不杀；静默≥%ds且<%d停点=死锁；静默≥%ds兜底）\n",
            (long)(tnow - g_last_real_out),
            g_guest_ticks - g_ticks_at_real_out, t_peak,
            VM_EXIT_MIN_ALIVE / VM_EXIT_GRACE_S,
            VM_EXIT_GRACE_S, VM_EXIT_MIN_ALIVE, VM_EXIT_GRACE2_S);
        live_write(wb, (size_t)wk);
    }
    if (t_rate_sec == 0) t_rate_sec = tnow;
    if (tnow != t_rate_sec) {                    /* 墙钟秒采样峰值 */
        unsigned long long d = g_guest_ticks - t_prev;
        if (d > t_peak) t_peak = d;
        t_prev = g_guest_ticks;
        t_rate_sec = tnow;
    }
    unsigned long long peak_eff = t_peak > 1000 ? t_peak : 1000;
    if (t_grace_done || g_last_real_out <= 0) return 0;
    /* v1.2.78 ★★★ 分级判据（替代"N*20<峰*8"的峰值公式——其 1000 下限
     * 会把健康慢速（47/s=4.7%）误判为"骤降"——v1.2.77 真机实锤：ART 信号
     * 准备阶段 N=375 被杀，黑盒现场 sigaction/getrlimit/sigprocmask）。
     *   ① 静默 ≥VM_EXIT_GRACE2_S(90s)：兜底——慢速打转永不前进也该收尸；
     *   ② 静默 ≥VM_EXIT_GRACE_S(8s) 且窗口停点 <VM_EXIT_MIN_ALIVE(64)
     *      = 平均 <8 停点/s：真死锁（futex 死等/退出死等）；
     *   ③ 其余（≥8 停点/s 的慢速健康活动）：不杀，继续观察。 */
    long sil = (long)(tnow - g_last_real_out);
    unsigned long long Nwin = g_guest_ticks - g_ticks_at_real_out;
    if (sil < VM_EXIT_GRACE2_S && !(sil >= VM_EXIT_GRACE_S && Nwin < VM_EXIT_MIN_ALIVE))
        return 0;
    t_grace_done = tnow;
    char gb[384];
    int gk = snprintf(gb, sizeof(gb),
        "[TwinVM] [graceful] 真实输出静默 %lds、窗口停点 %llu、峰值 %llu/s"
        " —— tracer 代执行优雅关机：SIGTERM→2s→SIGKILL\n",
        (long)(tnow - g_last_real_out),
        g_guest_ticks - g_ticks_at_real_out, peak_eff);
    live_write(gb, (size_t)gk);
    if (kill_it) {
        int gst = 0;
        kill(g_t_pid, SIGTERM);
        for (int gw = 0; gw < 20; gw++) {
            if (waitpid(g_t_pid, &gst, WNOHANG) == g_t_pid) break;
            usleep(100 * 1000);
        }
        if (waitpid(g_t_pid, &gst, WNOHANG) != g_t_pid)
            kill(g_t_pid, SIGKILL);
    }
    return 1;
}

static void tvm_tee_stdio(pid_t t, long nr, const unsigned long long* a) {
    /* v1.2.60 ★★★ 时钟更新必须在 budget 之前！v1.2.59 真机实锤：启动期
     * stderr tee（44+ 次）耗光 256 预算 → budget<=0 提前 return →
     * stress 的 stdout 到达时函数直接退出 → g_last_real_out 从未更新。
     * budget 只该限"打印"，不能限"计时"。 */
    if ((nr == 64 || nr == 66) && a[0] == 1) {
        g_last_real_out = time(NULL);
        g_ticks_at_real_out = g_guest_ticks;
    }
    static int budget = 256;               /* 限流：防止后期 stderr 刷屏 */
    if (budget <= 0) return;
    char pay[208]; size_t plen = 0;
    if (nr == 64) {                            /* write(fd, buf, count) */
        plen = tvm_read_tracee(t, a[1],
                 (size_t)(a[2] < 200 ? a[2] : 200), pay);
    } else if (nr == 66) {                     /* writev(fd, iov, count) */
        unsigned long long iov[16];
        size_t cnt = (size_t)(a[2] < 8 ? a[2] : 8);
        size_t got = tvm_read_tracee(t, a[1], cnt * 16, (char*)iov);
        for (size_t i = 0; i < got / 16 && plen < 200; i++) {
            size_t want = iov[2*i+1] < 200 ? (size_t)iov[2*i+1] : 200;
            size_t room = 200 - plen;
            if (want > room) want = room;
            if (want == 0) continue;
            plen += tvm_read_tracee(t, iov[2*i], want, pay + plen);
        }
        /* v1.1.5：0 字节 writev 是 v1.1.4 悬案（三笔 async_safe "Fatal signal"
         * 疑云）——裸 dump iov 数组让真相自己说话 */
        if (plen == 0 && got >= 32) {
            char m2[256];
            int k2 = snprintf(m2, sizeof(m2), "[TwinVM] [tee] fd=%llu writev 0 字节！iov 原文:",
                              a[0]);
            for (size_t i = 0; i < got / 16 && k2 < (int)sizeof(m2) - 32; i++)
                k2 += snprintf(m2 + k2, sizeof(m2) - (size_t)k2, " [%zu]base=0x%llx len=%llu",
                              i, iov[2*i], iov[2*i+1]);
            m2[k2++] = '\n';
            live_write(m2, (size_t)k2);
            budget--;
            return;
        }
    } else if (nr == 56 || nr == 48 || nr == 79) {
        /* v1.1.5：路径类 syscall 抄路径（openat/faccessat/newfstatat，
         * 路径在 a1；guest 崩溃前最后 stat/open 的是什么，这里直接见字） */
        size_t got = tvm_read_tracee(t, a[1], 96, pay);
        if (got == 0) return;
        char msg[160];
        int k = snprintf(msg, sizeof(msg), "[TwinVM] [tee] %s 路径: ",
                         nr == 56 ? "openat" : nr == 48 ? "faccessat" : "newfstatat");
        for (size_t i = 0; i < got && k < (int)sizeof(msg) - 4 && pay[i]; i++)
            msg[k++] = (pay[i] >= 0x20 && pay[i] < 0x7f) ? pay[i] : '.';
        msg[k++] = '\n';
        live_write(msg, (size_t)k);
        budget--;
        return;
    } else return;
    char msg[320];
    int k = snprintf(msg, sizeof(msg),
                     "[TwinVM] [tee] fd=%llu %s（%zu B）: ",
                     a[0], nr == 64 ? "write" : "writev", plen);
    for (size_t i = 0; i < plen && k < (int)sizeof(msg) - 2; i++) {
        unsigned char c = (unsigned char)pay[i];
        msg[k++] = (c >= 0x20 && c < 0x7f) ? (char)c : (c == '\n') ? '\n' : '.';
    }
    while (k > 0 && (msg[k-1] == '\n' || msg[k-1] == '.')) k--;   /* 去尾部噪声 */
    msg[k++] = '\n';
    live_write(msg, (size_t)k);
    budget--;
}

static void tvm_virt_set(pid_t tid, long nr, const unsigned long long* args) {
    for (int i = 0; i < TVM_VIRT_SLOTS; i++)
        if (!g_virt[i].active) {
            g_virt[i].tid = tid; g_virt[i].nr = nr; g_virt[i].active = 1;
            g_virt[i].seq = ++g_virt_seq;
            for (int k = 0; k < 6; k++) g_virt[i].args[k] = args ? args[k] : 0;
            return;
        }
}
/* v1.0.3 ★ 取待办：**必须同时匹配 syscall 号** —— exit 停点用 id 匹配不够，
 * 一旦 tid 复用/交错就会张冠李戴（真机实锤：guest 把 243 字节当成指针 → addr=0xf3）。 */
/* v1.1.2 ★★★ 真机实锤的错误：exit 停点内核报 `nr = -1`（因为我们在 entry 把 x8
 * 改成了 -1）。而 v1.0.8 的校验写的是 `if (nr > 0 && got != nr) return -1;` ——
 * 于是 **`nr=-1` 时根本不校验**，直接取该 tid 的**第一条**待办！
 *
 * 后果（v1.1.1 日志实锤）：
 *   `close(57)` 被反复拦截，待办表里堆了多条；每次任何 syscall 的 exit 停点
 *   都取到第一条（nr=57）→ 用 close 的结果 `-EBADF(-9)` 覆盖**所有** syscall 的 x0
 *   → guest 处处拿到 -9 → 行为完全错乱。
 *
 * 正确做法：**entry/exit 严格 FIFO 配对**（同一 tid 的待办按入队顺序取用），
 * 并且 exit 时**不再用 nr 匹配**（它已被我们改成 -1，没有信息量）。
 * 加序号 seq 以便日志核对。 */
static int tvm_virt_take(pid_t tid, long nr, unsigned long long* out_args) {
    (void)nr;   /* exit 停点的 nr 已无意义（我们改过 x8） */
    int best = -1;
    unsigned long long best_seq = ~0ULL;
    for (int i = 0; i < TVM_VIRT_SLOTS; i++)
        if (g_virt[i].active && g_virt[i].tid == tid && g_virt[i].seq < best_seq) {
            best_seq = g_virt[i].seq; best = i;
        }
    if (best < 0) return -1;
    long got = g_virt[best].nr;
    g_virt[best].active = 0;
    for (int k = 0; k < 6; k++) out_args[k] = g_virt[best].args[k];
    return (int)got;
}

/* v1.2.20 ★★★ **不依赖 ptrace 的等待现场体检**（可任意线程调用）。
 *
 * 为什么换做法：
 *   · v1.2.18 用 PTRACE_INTERRUPT 强制拉停 → v1.2.19 真机实锤
 *     `PTRACE_INTERRUPT 失败 errno=5`（EIO）—— 该请求**只支持 PTRACE_SEIZE
 *     附属的 tracee**，而当时是 PTRACE_TRACEME，注定失败（内核 ptrace.c
 *     源码在案；v1.2.37 已改 SEIZE 附着，INTERRUPT 复活为备用通道，
 *     但体检主通道仍用 /proc——它对运行中任务也有效、不打断 guest）；
 *   · 于是体检又回到"必须等一个天然停点"，而 guest 睡在 futex 里永远没有停点。
 *
 * 真正可行的取数通道（都不要求对方停止、也不要求是 tracer）：
 *   · /proc/<pid>/syscall  →  nr a0 a1 a2 a3 a4 a5 sp pc
 *     （运行中的任务也能读；睡在 syscall 里时 sp/pc 就是现场值）
 *   · /proc/<pid>/maps     →  各段范围与模块名
 *   · process_vm_readv     →  读对方内存（跨进程普通读，非 ptrace 门控）
 *
 * 副产品：**不再受"必须由 tracer 线程执行"的限制** —— v1.2.16~19 四轮
 * 折腾的"谁在哪个线程干活"问题，从这个设计里直接消失。
 */
/* v1.2.23 拦截主体：entry 停点把 x8 改成 -1，让内核跳过原 syscall，
 * 并把参数记入待办（exit 停点再按 FIFO 回填引擎算出的结果）。
 * ★ 必须独立成函数且 noinline —— 内联进示踪循环时会被 clang 静默优化掉（见调用处注释）。 */
__attribute__((noinline))
static void tvm_intercept_now(pid_t t, long nr, const unsigned long long* args) {
    struct tvm_arm_regs rr;
    if (tvm_getregs(t, &rr) != 0) {
        static int s_gr = 0;
        if (s_gr++ < 5)
            ptrace_log("[ptrace] !! GETREGSET 失败 errno=%d（tid=%d）—— 无法改 x8", errno, (int)t);
        return;
    }
    tvm_virt_set(t, nr, args);
    {
        static int s_seen[512];
        static int s_hits = 0;
        s_hits++;
        if (nr >= 0 && nr < 512 && s_seen[nr] < 6) {
            s_seen[nr]++;
            ptrace_log("[TwinVM] [caller] nr=%ld #%d lr=0x%llx pc=0x%llx a0=0x%llx a1=0x%llx",
                       nr, s_seen[nr],
                       (unsigned long long)rr.regs[30], (unsigned long long)rr.pc,
                       (unsigned long long)rr.regs[0], (unsigned long long)rr.regs[1]);
        }
        if ((s_hits % 500) == 0)
            ptrace_log("[ptrace] 已拦截 %d 次（x8 改写 + 交引擎）", s_hits);
    }
    rr.regs[8] = (unsigned long long)-1;
    if (tvm_setregs(t, &rr) != 0) {
        static int s_sr = 0;
        if (s_sr++ < 5)
            ptrace_log("[ptrace] !! SETREGSET 失败 errno=%d（tid=%d）", errno, (int)t);
    }
}

/* v1.2.25 ★★★ **循环现场抓帧**：在**真实 ptrace 停点**上取全套寄存器并沿 x29 帧链回溯。
 *
 * 为什么必须用 ptrace 而不是 /proc/<pid>/syscall（v1.2.24 的教训）：
 *   /proc/syscall 只给 nr/a0..a5/sp/pc —— **没有 x29（帧指针）与 x30（LR）**。
 *   而 AArch64 的调用链是 **x29 链**（[x29]=上一帧 fp，[x29+8]=返回地址），
 *   光有 sp 去扫栈经常一个都扫不到（真机 v1.2.24 实锤："栈上无可执行段地址"）。
 * 触发方式：看门狗/[loop] 检测器置 g_want_stack，示踪器在**下一个 syscall 停点**
 *（此时 tracee 处于停点，寄存器权威且稳定）抓一次，抓完清标志。
 * 段表用**全量**（含非可执行段，用于判定地址归属；可执行段额外标记用于符号化）。 */
static void tvm_dump_frames(pid_t t, long nr, struct tvm_arm_regs* r) {
    enum { SEGN = 512 };
    static struct { unsigned long long lo, hi, base; char name[56]; int named, exec; } sg[SEGN];
    int n = 0;
    char path[64], ln[256];
    snprintf(path, sizeof(path), "/proc/%d/maps", (int)t);
    FILE* mf = fopen(path, "r");
    if (mf) {
        while (fgets(ln, sizeof(ln), mf) && n < SEGN) {
            unsigned long long lo, hi, fo;
            char pm[8] = {0}, nm[56] = {0};
            if (sscanf(ln, "%llx-%llx %4s %llx %*s %*s %55[^\n]", &lo, &hi, pm, &fo, nm) >= 4) {
                /* v1.2.35 ★★ 修"段表=512 截断"补刀：v1.2.34 的过滤条件
                 * (非 exec 且无名 → 跳过) 拦不住 **有名字的非 exec 段**——
                 * 真机实锤 [anon:dalvik-*] 全带名字，几百条照样占满 512 槽，
                 * 0x72 区可执行段（帧链 ra 的家）依旧进不了表（"★模块未知"）。
                 * ra 归属只需要可执行段——只收 exec，一劳永逸。 */
                if (pm[2] != 'x') continue;
                sg[n].lo = lo; sg[n].hi = hi;
                sg[n].exec = (pm[2] == 'x');
                if (nm[0]) { sg[n].named = 1; sg[n].base = lo - fo; strncpy(sg[n].name, nm, 55); }
                else {
                    /* v1.2.80 ★ segnote 覆盖：map-fill 灌的段（guest 匿名 mmap
                     * + 父进程 process_vm_writev 灌库内容）在 maps 里是 [anon]——
                     * 但父进程有 fd→路径登记（tvm_segnote_add）。命中即补名，
                     * 帧回溯从此能说'liblog.so+0x1234'而不是'[anon] +0x…'。 */
                    unsigned long long s_off = 0;
                    const char* sn = tvm_segnote_find(lo, &s_off);
                    if (sn && s_off == 0) {         /* 段起点命中：整段补名 */
                        sg[n].named = 1; sg[n].base = lo;
                        strncpy(sg[n].name, sn, 55);
                    } else { sg[n].named = 0; sg[n].base = lo; sg[n].name[0] = 0; }
                }
                n++;
            }
        }
        fclose(mf);
    }
    {   /* 先报"停点全貌"：nr / lr / sp / x29 —— 这几项足以定出等待者 */
        char o[352];
        int k = snprintf(o, sizeof(o),
            "[TwinVM] [frames] nr=%ld lr=0x%llx sp=0x%llx x29=0x%llx 段表=%d\n",
            nr, (unsigned long long)r->regs[30], (unsigned long long)r->sp,
            (unsigned long long)r->regs[29], n);
        ptrace_log("%s", o + 9);
    }
    /* 沿 x29 帧链回溯最多 16 帧，把每个返回地址映射到"模块 + 偏移" */
    unsigned long long fp = r->regs[29];
    for (int i = 0; i < 16 && fp; i++) {
        unsigned long long pair[2] = {0, 0};
        if (tvm_read_pv(t, fp, sizeof(pair), (char*)pair) != 0) break;
        unsigned long long ra = pair[1];
        if (!ra) break;
        /* v1.2.26 ★ 本机开着 PAC + TBI（真机 tombstone：pac_enabled_keys=0xf、
         * tagged_addr_ctrl=PR_TAGGED_ADDR_ENABLE）：**栈上的返回地址高位带签名/标签**，
         * 真机 v1.2.25 实锤 ra=0x6876f23aef60ec —— 高位是签名，**低 39 位才是真地址**
         *（Android 应用 39 位 VA）。这里依次尝试 39..56 位掩码，取第一个落在已映射段内的值。
         * 不做这一步，帧链永远显示"★模块未知"（v1.2.25 就是这样白抓了一轮）。 */
        unsigned long long a = ra;
        int hit = -1, used = 0;
        for (int bits = 39; bits <= 56 && hit < 0; bits++) {
            unsigned long long cand = ra & ((bits >= 64) ? ~0ULL : ((1ULL << bits) - 1));
            for (int q = 0; q < n; q++)
                if (cand >= sg[q].lo && cand < sg[q].hi) { hit = q; a = cand; used = bits; break; }
        }
        char o[384];
        int k = snprintf(o, sizeof(o),
            "[TwinVM] [frames]   #%02d ra=0x%llx (去PAC%d位) %s +0x%llx  段=%lluKB %s\n",
            i, ra, used,
            hit < 0 ? "★模块未知" : (sg[hit].named ? sg[hit].name : "[anon]"),
            hit < 0 ? 0ULL : (sg[hit].named ? (a - sg[hit].base) : (a - sg[hit].lo)),
            hit < 0 ? 0ULL : (unsigned long long)((sg[hit].hi - sg[hit].lo) / 1024),
            hit < 0 ? "?" : (sg[hit].exec ? "可执行" : "非可执行"));
        if (k > 0) live_write(o, (size_t)k);
        fp = pair[0];
        if (fp < sg[0].lo) break;   /* 明显不合法则停 */
    }
}

static void tvm_hang_dump_procfs(pid_t pid) {
    char path[64], ln[320];
    unsigned long long a0=0,a1=0,a2=0,a3=0,a4=0,a5=0,sp=0,pc=0;
    long hnr = -1;
    int ok = 0;
    snprintf(path, sizeof(path), "/proc/%d/syscall", (int)pid);
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        ssize_t k = read(fd, ln, sizeof(ln) - 1);
        close(fd);
        if (k > 0) {
            ln[k] = 0;
            if (sscanf(ln, "%ld %llx %llx %llx %llx %llx %llx %llx %llx",
                       &hnr, &a0, &a1, &a2, &a3, &a4, &a5, &sp, &pc) >= 9) ok = 1;
            else {
                char e[200];
                int m = snprintf(e, sizeof(e),
                    "[TwinVM] [hang] (procfs) syscall 内容无法解析: %s\n", ln);
                live_write(e, (size_t)m);
            }
        }
    }
    if (!ok) {
        char e[200];
        int m = snprintf(e, sizeof(e),
            "[TwinVM] [hang] (procfs) 读 /proc/%d/syscall 失败 errno=%d\n", (int)pid, errno);
        live_write(e, (size_t)m);
        return;
    }
    {
        char o[352];
        int m = snprintf(o, sizeof(o),
            "[TwinVM] [hang] nr=%ld sp=0x%llx pc=0x%llx a0=0x%llx a1=0x%llx a2=0x%llx a3=0x%llx\n",
            hnr, sp, pc, a0, a1, a2, a3);
        live_write(o, (size_t)m);
    }
    /* maps → 可执行段表（用于把栈上的地址映射回模块+偏移） */
    /* v1.2.27 ★★★ **全量收录 + exec 标记 + 容量放大**。
     * 真机 v1.2.22~1.2.26 连续五轮都报 "sp 所在段=★不在任何映射"：
     * 实测 sp=0x723c157010 明明落在 723c058000-723c158000 rw-p（栈）里 ——
     * 是老过滤条件 `pm[2]=='x'` 把非可执行段全丢了，**是过滤器造成的假象**。
     * 容量从 256 提到 8192：真机 maps 有 3176~3195 段，256 根本装不下。 */
    static struct { unsigned long long lo, hi, base; char name[56]; int named, exec; } seg[8192];
    int nseg = 0;
    snprintf(path, sizeof(path), "/proc/%d/maps", (int)pid);
    FILE* mf = fopen(path, "r");
    if (mf) {
        char ml[256];
        while (fgets(ml, sizeof(ml), mf) && nseg < 8192) {
            unsigned long long lo, hi, fo;
            char pm[8] = {0}, nm[56] = {0};
            if (sscanf(ml, "%llx-%llx %4s %llx %*s %*s %55[^\n]", &lo, &hi, pm, &fo, nm) >= 4) {
                seg[nseg].lo = lo; seg[nseg].hi = hi;
                seg[nseg].exec = (pm[2] == 'x');       /* 可执行性留给"找返回地址"用 */
                if (nm[0]) { seg[nseg].named = 1; seg[nseg].base = lo - fo; strncpy(seg[nseg].name, nm, 55); }
                else { seg[nseg].named = 0; seg[nseg].base = lo; seg[nseg].name[0] = 0; }
                nseg++;
            }
        }
        fclose(mf);
    }
    /* 从 sp 起读 8KB 栈，找出指向可执行段的返回地址 */
    /* v1.2.29 ★★★★ **futex 等待者取证**。
     * 真机 v1.2.28 实锤：建堆阶段跑完后（吞吐修复让 62 万次 syscall 在 ~50s 内完成），
     * guest 转为阻塞在 `futex(uaddr=0x723b04d984, FUTEX_WAIT|PRIVATE, 期望值=3, 无超时)`。
     * 调用链符号化为：
     *   JNI_CreateJavaVM → Runtime::Create → Runtime::Init
     *     → **art::Thread::Startup()** → Thread::Attach → Thread::Thread → Thread::Init → futex
     * 而进程 `num_threads=1`、`clone` 0 次（**从不创建线程**）。
     * 期望值 3 恰是 bionic `pthread_mutex_t` 的 **LOCKED|WAITERS** —— "在等一把已被持有的锁"。
     * 但**到底谁持的**必须看 owner_tid 才能定案（两者修法完全不同）：
     *   owner == 自己 → **自死锁**（同线程二次加锁，非递归锁）
     *   owner != 自己 → **在等一个不存在的线程**（该线程从未被 clone 出来）
     * 所以这里把锁内容原样取回来。 */
    /* v1.2.31：倒出 futex 环形留痕（最近 64 条，时间正序） */
    {
        int total = g_futex_idx;
        int show = total < 64 ? total : 64;
        char hd[160];
        int hk = snprintf(hd, sizeof(hd),
            "[TwinVM] [futex-hist] 共记录 %d 次 futex 调用，倒出最近 %d 条：\n", total, show);
        live_write(hd, (size_t)hk);
        for (int z = show; z >= 1; z--) {
            int fi4 = (total - z) % TVM_FUTEX_RING;
            long op = g_futex_ring[fi4].op;
            char ln2[220];
            int lk2 = snprintf(ln2, sizeof(ln2),
                "[TwinVM] [futex-hist]   #%-4d %-9s%s uaddr=0x%llx val=%llu\n",
                total - z, tvm_futex_op_name(op), (op & 0x80) ? "(PRIVATE)" : "         ",
                g_futex_ring[fi4].uaddr, g_futex_ring[fi4].val);
            live_write(ln2, (size_t)lk2);
        }
    }
    /* v1.2.34：syscall nr 直方图（最近 256 条，top5） */
    {
        int tot2 = g_nr_idx < TVM_NR_RING ? g_nr_idx : TVM_NR_RING;
        static long alln[256]; static int allc[256];
        int un = 0;
        for (int i = 0; i < tot2 && un < 256; i++) {
            long v = g_nr_ring[i];
            int j;
            for (j = 0; j < un && alln[j] != v; j++) {}
            if (j < un) { allc[j]++; continue; }
            alln[un] = v; allc[un] = 1; un++;
        }
        for (int a = 0; a < 5 && a < un; a++) {     /* 选择排序 top5 */
            int mx = a;
            for (int b = a + 1; b < un; b++) if (allc[b] > allc[mx]) mx = b;
            if (mx != a) { long tn2 = alln[a]; int tc2 = allc[a]; alln[a] = alln[mx]; allc[a] = allc[mx]; alln[mx] = tn2; allc[mx] = tc2; }
        }
        char hd[96];
        int hk = snprintf(hd, sizeof(hd),
            "[TwinVM] [nr-hist] 最近 %d 条 syscall 直方图（死循环现形）：\n", tot2);
        live_write(hd, (size_t)hk);
        for (int a = 0; a < 5 && a < un; a++) {
            char ln2[128];
            int lk2 = snprintf(ln2, sizeof(ln2),
                "[TwinVM] [nr-hist]   nr=%-4ld  %2d%%  (%d/%d)\n",
                alln[a], tot2 ? (allc[a] * 100 + tot2 / 2) / tot2 : 0, allc[a], tot2);
            live_write(ln2, (size_t)lk2);
        }
    }
    if (hnr == 98 /* futex */) {
        unsigned long long uaddr = a0 & 0x007FFFFFFFFFULL;   /* 去 TBI 标签 */
        unsigned char lb[80];                     /* v1.2.35：32→80（uaddr-16 起 64B 上下文） */
        unsigned char* lbp = lb + 16;              /* lb[16..] = uaddr 处 */
        memset(lb, 0, sizeof(lb));
        int q = -1;
        for (int z = 0; z < nseg; z++) if (uaddr >= seg[z].lo && uaddr < seg[z].hi) { q = z; break; }
        if (tvm_read_pv(pid, uaddr - 16, 64, (char*)lbp) == 0) {
            unsigned int lockword = (unsigned)lbp[0] | ((unsigned)lbp[1] << 8) |
                                    ((unsigned)lbp[2] << 16) | ((unsigned)lbp[3] << 24);
            unsigned int owner = (unsigned)lbp[4] | ((unsigned)lbp[5] << 8) |
                                 ((unsigned)lbp[6] << 16) | ((unsigned)lbp[7] << 24);
            char o[360];
            int k = snprintf(o, sizeof(o),
                "[TwinVM] [futex] uaddr=0x%llx 段=%lluKB%s lockword=0x%x(LOCKED=%d WAITERS=%d) "
                "owner_tid=%u 本pid=%d → %s\n",
                uaddr, q < 0 ? 0ULL : (unsigned long long)((seg[q].hi - seg[q].lo) / 1024),
                (q >= 0 && seg[q].named) ? seg[q].name : "",
                lockword, lockword & 1, (lockword >> 1) & 1, owner, (int)pid,
                (owner && (int)owner == (int)pid) ? "★自死锁（同一线程重复加锁）"
                : (owner ? "★等一个不存在的线程（owner 非本线程）" : "★无 owner（cond/once 语义）"));
            live_write(o, (size_t)k);
            char hx[280];
            int hk = snprintf(hx, sizeof(hx),
                "[TwinVM] [futex] 原始 64 字节（uaddr-16 起，等待对象上下文）：\n");
            live_write(hx, (size_t)hk);
            for (int r = 0; r < 4; r++) {
                char row[120];
                int rk = snprintf(row, sizeof(row),
                    "[TwinVM] [futex]   +%02d: %02x %02x %02x %02x %02x %02x %02x %02x "
                    "%02x %02x %02x %02x %02x %02x %02x %02x\n",
                    r*16,
                    lb[r*16],lb[r*16+1],lb[r*16+2],lb[r*16+3],lb[r*16+4],lb[r*16+5],lb[r*16+6],lb[r*16+7],
                    lb[r*16+8],lb[r*16+9],lb[r*16+10],lb[r*16+11],lb[r*16+12],lb[r*16+13],lb[r*16+14],lb[r*16+15]);
                live_write(row, (size_t)rk);
            }
            live_write(hx, (size_t)hk);
        } else {
            char o2[160];
            int k2 = snprintf(o2, sizeof(o2), "[TwinVM] [futex] uaddr=0x%llx 读取失败 errno=%d\n", uaddr, errno);
            live_write(o2, (size_t)k2);
        }
    }
    /* v1.2.23：先自检 sp 本身落在哪个段 —— 若 sp 不在任何映射里，
     * 说明 /proc/syscall 给的 sp 不可信（或读得太早），栈扫描注定为空却看不出原因。 */
    {
        int sp_in = -1, pc_in = -1;
        for (int q = 0; q < nseg; q++) {
            if (sp >= seg[q].lo && sp < seg[q].hi) sp_in = q;
            if (pc >= seg[q].lo && pc < seg[q].hi) pc_in = q;
        }
        char w1[320];
        int w1k = snprintf(w1, sizeof(w1),
            "[TwinVM] [hang] sp 所在段=%s pc 所在段=%s\n",
            sp_in >= 0 ? (seg[sp_in].named ? seg[sp_in].name : "[anon]") : "★不在任何映射",
            pc_in >= 0 ? (seg[pc_in].named ? seg[pc_in].name : "[anon]") : "★不在任何映射");
        live_write(w1, (size_t)w1k);
    }
    static char sb[8192];
    int rv = (nseg > 0) ? tvm_read_pv(pid, sp, sizeof(sb), sb) : -99;
    if (rv != 0) {
        char e3[200];
        int e3k = snprintf(e3, sizeof(e3),
            "[TwinVM] [hang] 栈读取失败：tvm_read_pv=%d errno=%d nseg=%d（扫不了）\n",
            rv, errno, nseg);
        live_write(e3, (size_t)e3k);
    }
    if (rv == 0) {
        int hits = 0;
        for (size_t so = 0; so + 8 <= sizeof(sb) && hits < 24; so += 8) {
            unsigned long long v;
            memcpy(&v, sb + so, 8);
            if (!v) continue;
            /* v1.2.27 ★★★ **PAC/TBI 去掩码** —— 这是"（栈上无可执行段地址）"
             * 连续五轮不退的**真因**：本机开着 PAC（tombstone: pac_enabled_keys=0xf），
             * 栈上的返回地址高位带签名（实测形如 0x6876f23aef60ec），
             * 直接拿原值与段区间比较**永远比不中**。
             * 与 [frames] 帧链同一套办法：依次试 39..56 位掩码，取第一个命中**可执行段**的值。 */
            int hit = -1; unsigned long long a = v; int bits_used = 0;
            for (int bits = 39; bits <= 56 && hit < 0; bits++) {
                unsigned long long cand = v & ((bits >= 64) ? ~0ULL : ((1ULL << bits) - 1));
                for (int q = 0; q < nseg; q++)
                    if (seg[q].exec && cand >= seg[q].lo && cand < seg[q].hi) {
                        hit = q; a = cand; bits_used = bits; break;
                    }
            }
            if (hit >= 0) {
                /* v1.2.35：无名 exec 段（fill 灌的库）→ 查 fill 登记表给路径 */
                char tagbuf[80];
                if (!seg[hit].named) {
                    unsigned long long foff = 0;
                    const char* fp = tvm_segnote_find(a, &foff);
                    if (fp) snprintf(tagbuf, sizeof(tagbuf), "[fill]%s", fp);
                    else    snprintf(tagbuf, sizeof(tagbuf), "%s", "[anon]");
                } else {
                    snprintf(tagbuf, sizeof(tagbuf), "%s", seg[hit].name);
                }
                char l2[288];
                int lk = snprintf(l2, sizeof(l2),
                    "[TwinVM] [hang] sp+0x%-4zx 0x%llx (去PAC%d位) %s +0x%llx\n",
                    so, a, bits_used, tagbuf,
                    seg[hit].named ? (a - seg[hit].base) : (a - seg[hit].lo));
                live_write(l2, (size_t)lk);
                hits++;
            }
        }
        if (hits == 0) {
            const char* e2 = "[TwinVM] [hang] （栈上无可执行段地址）\n";
            live_write(e2, strlen(e2));
        }
    }
    /* v1.2.22 ★★★ **引擎自己的段登记表**（带模块路径）—— 比 /proc maps 准得多：
     * /proc maps 是"host App + guest 混在一起"的真实映射（几千条），
     * 而这张表只记 guest 通过虚拟 fd 映射的 ROM 段，每条都带**源文件路径**。
     * 有了它，任何 pc/lr 属于哪个库一目了然（真机 v1.2.21 里
     * "180KB 匿名 rwxp 是哪个库"卡了整整一轮）。 */
    {
        static char vmbuf[96 * 1024];
        size_t vmn = vm_render_maps(vmbuf, sizeof(vmbuf));
        const char* vh = "[TwinVM] [vmmap] 引擎段登记表（guest 视角，带模块路径）：\n";
        live_write(vh, strlen(vh));
        if (vmn > 0) live_write(vmbuf, vmn);
        else { const char* ve = "[TwinVM] [vmmap] （登记表为空）\n"; live_write(ve, strlen(ve)); }
    }
    /* maps 全量落盘（供离线符号化） */
    const char* mh = "[TwinVM] [hang-maps] guest 布局：\n";
    live_write(mh, strlen(mh));
    mf = fopen(path, "r");
    if (mf) {
        char mb[2048]; size_t mn;
        while ((mn = fread(mb, 1, sizeof(mb), mf)) > 0) live_write(mb, mn);
        fclose(mf);
    }
}

/* v1.2.23 ★★★ **禁止对本函数做优化**。
 * 真机实测（构建产物比对，-O0 vs -O1/-O2/-Os）：clang 会把本函数里
 *   `if (tvm_getregs(...) == 0) { tvm_virt_set(...); rr.regs[8] = -1; tvm_setregs(...); }`
 * **整块删掉** —— 构建产物里 [caller] / SETREGSET / 已拦截 三处串全部消失，
 * 而 `if (gr != 0)`（失败告警）却还在。也就是说：**拦截主体（把 x8 改成 -1
 * 让内核跳过原 syscall）会被静默优化掉**，只剩失败分支。
 * 前端 `-Wunreachable-code` 不报警，`-O0` 时又在 → 属优化器单独判定，极难察觉。
 * 影响：拦截形同虚设，guest 原生执行 + 我们再按待办回填 x0 → 结果错乱。
 * 代价：本函数不优化（它是 syscall 转发循环，慢一点远好过"悄悄失效"）。 */
#pragma clang optimize off
static void tvm_alrm_noop(int sig) { (void)sig; }   /* v1.2.63：SIGALRM 空处理 */

static void* tvm_tracer_main(void* arg) {
    struct tvm_launch* L = arg;
    /* v0.9.0 ★ 关键改动：**在 fork 之前**（父进程里）先把 USER_NOTIF 过滤器装好。
     * 原因：真机 v0.8.9 实锤 —— 子进程里 `seccomp(SET_MODE_FILTER, NEW_LISTENER)`
     * 调用**被永久挂起**（步骤1 返回 0，步骤2 之后无输出）。推测是 App 的 zygote
     * 策略把 `seccomp(2)` 判成 USER_NOTIF，需要"外部监听者"应答，而没人应答。
     * 放到父进程执行：① 若被策略拦，会拿到明确 errno；② 过滤器与 listener fd
     * 都会被子进程**继承**（seccomp 过滤器与 listener fd 均跨 fork 保留）。 */
    /* v0.9.1：把安装放到**独立线程**并设 3 秒上限 —— 若它被外层策略永久挂起，
     * 主线程不会被拖住，且我们能明确地把"挂起"作为结论（而不是黑屏等 90 秒）。 */
    {
        static volatile int inst_done = 0; static int inst_rc = -99;
        pthread_t ith;
        if (pthread_create(&ith, NULL, tvm_inst_thread, &inst_rc) == 0) {
            pthread_detach(ith);
            ptrace_log("▶ [2/6] 正在安装拦截器（最多等 1.2 秒）…");
            g_inst_done_p = &inst_done; g_inst_rc_p = &inst_rc;
            for (int ms = 0; ms < 1200; ms += 50) {
                if (inst_done) break;
                usleep(50 * 1000);
            }
            if (inst_done && inst_rc == 0) {
                if (g_notif_suspended) {
                    /* v1.0.2：超时后姗姗来迟的成功 —— 不用它（避免双后端并存） */
                    ptrace_log("▶ ⚠ seccomp 超时后才返回成功（fd=%d）—— 忽略，只用 ptrace",
                           vm_notif_fd());
                } else {
                    g_notif_inherited = 1;
                    ptrace_log("▶ [2/6] ✓ 父进程已装好 USER_NOTIF（listener fd=%d），子进程继承",
                           vm_notif_fd());
                }
            } else if (inst_done) {
                ptrace_log("▶ ⚠ 父进程装 USER_NOTIF 失败: %d（子进程再试）", -inst_rc);
            } else {
                ptrace_log("▶ ✗ **seccomp(2) 被永久挂起**（1.2 秒无返回）：外层策略把它"
                       "当成了需要外部应答的操作。改用 SIGSYS/TRAP 后端继续。");
                g_notif_suspended = 1;
            }
        } else {
            ptrace_log("▶ ⚠ 安装线程创建失败，走子进程自行安装");
        }
    }
    pid_t pid = fork();
    if (pid == 0) {
        /* 子进程：guest 槽位（管道/ROM/黑匣子全继承） */
        close(L->rfd);
        /* v1.1.7 ★★★ 槽位第一件事：PR_SET_DUMPABLE(1)。
         * 真机 v1.1.6 实锤：guest 槽位继承 App 进程的 dumpable=0（zygote 传统）→
         * 父进程（引擎+tee+FP 链）对它的 process_vm_readv/PEEKDATA 从事件 #1 起
         * 全部 EPERM/EIO：openat 路径读不出（EACCES）、writev 载荷 [bad iov]、
         * [sig] FP 链断、/proc/pid/maps ENOENT。而同一日志里，崩溃处理器
         * prctl(PR_SET_DUMPABLE,1)（事件 #486）**之后**对 dump 线程的读取全部
         * 成功——同源根因，铁证。dumpable=1 恢复同 uid 父进程的
         * ptrace_may_access，观测通道全开（ART 的 FATAL 消息将随 writev 落日志）。 */
        prctl(PR_SET_DUMPABLE, 1);
        /* v1.0.6 ★ **关闭继承来的宿主 fd**（≥3），只保留必要的：
         *   L->wfd（console 管道）、listener fd（USER_NOTIF 用，ptrace 模式下没有）。
         * 真机 v1.0.5 实锤：ART 拿到 `fd=99`（宿主/引擎的 fd）后反复 read →
         * 重试循环 → futex 死等。根因就是 fork 时**把所有宿主 fd 都带进了子进程**。
         * 这一关闭既是隔离修复，也消除"guest 操作宿主资源"的一整类问题。 */
        {
            int keep[4]; int nk = 0;
            keep[nk++] = L->wfd;
            if (vm_notif_fd() >= 0) keep[nk++] = vm_notif_fd();
            for (int fd = 3; fd < 256; fd++) {
                int k = 0;
                for (int q = 0; q < nk; q++) if (fd == keep[q]) k = 1;
                if (!k) close(fd);
            }
            ptrace_log("▶ 已关闭继承的宿主 fd（保留 console=%d）", L->wfd);
        }
        sigset_t em; sigemptyset(&em);
        sigprocmask(SIG_SETMASK, &em, NULL);  /* 去掉继承的 SIGCHLD 屏蔽，让 guest 看到正常世界 */
        /* v0.7.61：释放宿主 ART 的哨兵页。fork 从 zygote 系的宿主继承了
         * 0xebad6000（宿主自己的 Runtime::Init 保留的 "sentinel fault page"，
         * AOSP 著名毒地址 0xebad6070 所在页）。guest 的 ART 也要在**这个精确
         * 地址**保留一页（hint mmap，无 MAP_FIXED）——被宿主占着 → 内核给了
         * 别处 → "Failed to mmap at expected address" → PLOG(FATAL)
         * "Could not reserve sentinel fault page"（v0.7.55~60 四个版本全部
         * 死于此，v0.7.60 日志实锤定位）。fork 后子进程地址空间独立，
         * munmap 只影响本槽位，宿主无感。 */
        munmap((void*)0xebad6000UL, 0x1000UL);
        /* v1.2.39 ★★★ 回退 TRACEME 主路（SEIZE 被环境层拒绝）。
         * 真机 v1.2.38（10-17 12:07，设备重启后 pid=2750）实锤：
         * PTRACE_SEIZE 返回 -1 → 无 tracer 接手 → 孩子 raise(SIGSTOP) 后
         * 停在组停、父进程 waitpid（无 WUNTRACED）永远等不到事件 →
         * 90 秒看门狗杀场（握手三连 -1）。同代码上午 09:22（pid=28936，
         * 未重启）却 seize=0——**环境层（重启后的安全状态）拒绝 parent→child
         * 附着**。内核源码（TA 上传 kernel/ptrace.c）裁定：SEIZE 要过
         * __ptrace_may_access(PTRACE_MODE_ATTACH_REALCREDS)（受 dumpable/
         * LSM/Yama 摆布，返回点都在 ptrace_attach 开头）；TRACEME 则是孩子
         * 侧自我声明，本机 100+ 版本全过。SEIZE 的唯一收益是 PTRACE_INTERRUPT
         * 资格——体检主通道 /proc 不需要它，不值这个险。
         * TRACEME 失败（理论上前所未见）→ _exit(98)，父进程 waitpid 会拿到
         * 退出状态快速失败（有界轮询见下），不会再死等。 */
        if (ptrace(PTRACE_TRACEME, 0, 0, 0) != 0) _exit(98);  /* attach 失败：raise 会真杀进程 */
        raise(SIGTRAP);                     /* 信号投递停点；等本线程（追踪者）上选项 */
        vm_bb_attach((VmBlackBox*)L->bb);
        jni_guest_main((VmRom*)L->rom, L->wfd);
        _exit(99);
    }
    close(L->wfd);                           /* 父侧唯一写端在此关（主线程不再 close） */
    g_t_pid = pid;
    sem_post(&g_launch_sem);                 /* 通知主线程：fork 完成，pid 就绪 */

    /* v1.2.66 ★★★ 真凶修复：tracer 线程自行 UNBLOCK SIGALRM。
     * 信号掩码是 per-thread 的——本线程由主线程在 ALRM 屏蔽**之后**创建
     * （pthread_create 继承创建时刻掩码），主线程后补的 UNBLOCK 只解它自己
     * → tracer 永远屏蔽 ALRM → setitimer 到期全员屏蔽 → waitpid 永无
     * EINTR → 永不进轮询态 → t-watch 停摆（v1.2.65 真机实锤：stress 后
     * t-watch 一条不出，但停点驱动检查 62s 前正常）。 */
    {
        sigset_t am; sigemptyset(&am); sigaddset(&am, SIGALRM);
        pthread_sigmask(SIG_UNBLOCK, &am, NULL);
        static int ub_once = 0;
        if (!ub_once) { ub_once = 1;
            ptrace_log("[alrm] ✓ tracer 已解除 SIGALRM 屏蔽（轮询态通道就绪）");
        }
    }

    /* v1.1.6 ★★★ 疑似总根因修复：父进程引擎装配。
     * 唯一的 vm_machine_setup 原来跑在**子进程**（jni_guest_main:366）里——
     * fork 后父子地址空间独立，父进程（示踪线程）的 g_cpu 始终是零值：
     *   vfs=NULL → guest 的 openat 路径读不出来 → 一律 -EACCES；
     *   mem.pid=0 → process_vm_readv(0)=ESRCH → write/writev 载荷全丢
     *   （ART 的 Fatal signal/abort 原文进不了日志的直接原因！）。
     * 子进程侧的 loader 是进程内直调 vm_vfs_open（不走 syscall），所以装载
     * 阶段不受影响——这正好解释"boot 全过但 ART 一到真 openat 就死"。
     * 现在父进程在 fork 后立刻装配（in_process=0，pid=guest）：exit 停点的
     * vm_handle_syscall 从此有真的 VFS 和访存通道。若 g_cpu 已被装配过
     * （防御未来架构变化），重装同值无害。 */
    vm_machine_setup((const VmRom*)L->rom, pid, 0);
    /* v1.1.9 ★★★ 父进程的"自我认知"三件套（教训五第二击：v1.1.8 把
     * vm_set_exec_path 放在 exec_dyn 里＝子进程调用，而 syscall 处理在父进程
     * ——fork 内存隔离，父侧 g_exec_path/g_exec_cmdline 依旧恒空。真机
     * v1.1.8 实锤：comm 合成返回 "guest"、stat("/proc/self/exe") 仍 ENOENT、
     * "unable to stat" 依旧。父进程必须自己设，判定条件与子进程 exec_dyn
     * 处完全一致（dv&&ld&&!TVM_LEGACY_TRAP）。 */
    {
        const VmRomInode* dv = vm_rom_find((const VmRom*)L->rom, "/system/bin/dalvikvm");
        const VmRomInode* ldr = vm_rom_find((const VmRom*)L->rom, "/system/bin/linker64");
        if (dv && ldr && getenv("TVM_LEGACY_TRAP") == NULL) {
            vm_set_exec_path("/system/bin/dalvikvm");
            /* v1.2.95 ★★★ -Xverify:none：冷校验是 90s 兜底杀不完的主因
             * （v1.2.93/94 真机：28~70 bytecodes/s——正常应数万；真机 zygote
             * 靠 boot.art 预校验，我们没有 dalvik-cache 只能全量冷校验）。
             * VM 沙箱内校验无安全意义（guest 代码本就是我们 ROM 自带），
             * 跳过校验直通执行。同时兜底 90s→240s（首次类装载仍需时间）。 */
            /* v1.2.96：类名从 dex 路径推（/rom/hello.dex→Hello, world.dex→World）。
             * v1.2.108 ★ M2：.apk 类路径走 g_main_class（setMainClass 设置，
             * 默认从文件名推——APK 名推不出入口类，必须显式指定）。 */
            /* v1.2.112：用 fork 前的快照（原在此读跨线程全局——实锤竞态） */
            const char* clsname = g_cls_snapshot;
            char* const pargv[] = { (char*)"dalvikvm", (char*)"-Xverify:none",
                                    (char*)"-cp", g_dex_path,
                                    (char*)clsname, NULL };
            /* v1.2.118：boot 模式下 cmdline 也按引导形态合成（/proc/self/cmdline 一致性） */
            static char s_cp3[192];
            static char* pargv_boot[] = { (char*)"dalvikvm", (char*)"-Xverify:none",
                                          (char*)"-cp", NULL,
                                          (char*)"com.twinvm.m3.Bootstrap",
                                          (char*)"", NULL };
            if (g_boot_mode) {
                snprintf(s_cp3, sizeof(s_cp3), "%s:/system/framework/bootstrap.jar",
                         g_dex_path);
                pargv_boot[3] = s_cp3;
                pargv_boot[5] = g_cls_snapshot;
            }
            vm_set_exec_cmdline(g_boot_mode ? pargv_boot : pargv);
        } else {
            vm_set_exec_path("/bin/guest");
            static char* const pargv[] = { (char*)"guest", NULL };
            vm_set_exec_cmdline(pargv);
        }
    }
    /* v1.1.6：父进程引擎的 console 直通 live 文件——此前 vm_console_fd=2
     * （App stderr=/dev/null），引擎 tee 的 write/writev 载荷（正是 ART 的
     * Fatal signal/abort 原文！）全部进黑洞。子进程不受影响（它自设管道）。 */
    if (g_live_fd >= 0) vm_console_fd = g_live_fd;
    {
        char m2[160];
        int k2 = snprintf(m2, sizeof(m2),
            "[TwinVM] [engine] 父进程引擎装配完成：pid=%d vfs=%s mem_pid=%d console=%d\n",
            (int)pid, (vm_machine_current()->vfs ? "OK" : "NULL"),
            (int)vm_machine_current()->mem.pid, vm_console_fd);
        live_write(m2, (size_t)k2);
    }

    /* 握手（必须本线程做：追踪者=wait 者）
     * v1.2.39：TRACEME 主路 + **有界等待**——v1.2.38 真机实锤 SEIZE 可被
     * 环境层拒绝（seize=-1 → waitpid 死等 90s）。新规则：握手任何一步失败
     * 都快速失败（≤3s）并落 errno，绝不拖到看门狗。
     * 顺序（与 v1.2.36 相同）：waitpid 拿 SIGTRAP 停点 → SETOPTIONS →
     * PTRACE_SYSCALL 续跑。 */
    {
        int hst = 0, wr = 0, spins;
        for (spins = 0; spins < 300; spins++) {          /* ≤3s，绝不死等 */
            wr = waitpid(pid, &hst, WNOHANG);
            if (wr == pid) break;                        /* 停点或退出都算事件 */
            if (wr < 0 && errno != EINTR) break;        /* 真错误（ECHILD 等） */
            usleep(10 * 1000);
        }
        int hs = (wr == pid && WIFSTOPPED(hst)) ? WSTOPSIG(hst) : -1;
        long so = -1, ps = -1;
        if (hs >= 0) {
            so = ptrace(PTRACE_SETOPTIONS, pid, 0,
                        (void*)(long)(PTRACE_O_TRACECLONE | PTRACE_O_TRACESYSGOOD
                                      | PTRACE_O_EXITKILL));
            ps = ptrace(PTRACE_SYSCALL, pid, 0, 0);
        }
        char hm[256];
        int hn = snprintf(hm, sizeof(hm),
            "[TwinVM] [trace] 握手(TRACEME): 停点信号=%d waitpid=%d setoptions=%ld 重启=%ld errno=%d (pid=%d, 由示踪线程 fork)\n",
            hs, wr, so, ps, errno, (int)pid);
        live_write(hm, (size_t)hn);
        LOGI("%s", hm);
        if (hs < 0 || so != 0 || ps != 0) {
            /* 快速失败：孩子可能已退出（TRACEME 被拒 → _exit(98)）或卡死——
             * 强杀并明说，绝不留 90 秒死等 */
            int ec = (wr == pid && WIFEXITED(hst)) ? WEXITSTATUS(hst) : -1;
            char fm[192];
            int fn = snprintf(fm, sizeof(fm),
                "[TwinVM] !! 握手失败：hs=%d setoptions=%ld 重启=%ld 孩子退出码=%d（98=TRACEME 被拒）——强杀槽位\n",
                hs, so, ps, ec);
            live_write(fm, (size_t)fn);
            kill(pid, SIGKILL);
            g_t_status = hst; g_t_done = 1;
            /* 走收尾路径：交给主线程按既有退出流程处理 */
        }
    }
    for (;;) {
        int st;
        pid_t t;
        /* v1.2.18 ★★★ 关键修复：**非阻塞轮询**。
         * 真机 v1.2.17 实锤（日志：`[hang] GETREGSET 失败 errno=3` = ESRCH）：
         * guest 阻塞在 `futex_wait_queue` 时**不会产生任何新的 ptrace 停点** ——
         * PTRACE_SYSCALL 只在 syscall 的 entry/exit 停，**阻塞期间没有停点**。
         * 于是 tracer 死等 `waitpid`，循环不转，`g_hang_probe` 永远轮不到，
         * 体检只能由主线程发 GETREGSET → ESRCH 失败。
         * 改为 WNOHANG + 2ms 轮询：忙时行为不变（无额外开销），
         * 阻塞时每 2ms 醒一次，能检查"体检代办"标志 —— 体检本身由主线程用
         * /proc/<pid>/syscall + process_vm_readv 完成（不需要停点、不需要 tracer 身份）。 */
        /* v1.2.28 ★ 阻塞等待（原来是 WNOHANG + usleep(2000)，那 2ms 就是吞吐地板）。
         * 需要处理"体检/抓帧"标志时，看门狗会 pthread_kill 打断这里 → errno==EINTR
         * → 回到循环顶部处理标志 → 再重新阻塞。 */
        /* v1.2.63 ★★★ 混合模式（修 v1.2.62 自伤：WNOHANG+10ms 全程轮询把
         * 吞吐拖到 ~100 停点/s，真机日志 1063/11s=97/s 铁证；ART init 几万
         * 停点从 40s 变 40 分钟）。
         *   忙时：阻塞式 waitpid（内核唤醒即处理，满速——v1.2.28 原味），
         *         前挂 200ms setitimer 保底（SIGALRM 已定向本线程、空 handler）；
         *   连续 200ms 无停点：SIGALRM 打断 waitpid（EINTR）→ 降级
         *         WNOHANG+20ms 轮询态（驱动 graceful 检查）；
         *   新停点一到：立即回满速阻塞态。 */
        {
            /* v1.2.75 ★★★ 全 WNOHANG 两态轮询（一刀切掉全部信号机关）：
             * 病史：ALRM 八版未破（可用声明+抢收修复后仍零 EINTR）；
             * v1.2.74 的 20ms 车轮又把 mprotect 风暴拖慢 8 倍
             * （27k/s→3.5k/s，风暴 42s 拖成 323s+——0.7% 估算彻底翻车：
             * 停点处理极便宜，20ms 睡眠绝对值压倒一切）。
             * 新机制（零信号依赖，两态自动切换）：
             * ① 停点新鲜（<300ms）：WNOHANG 紧密自旋——停点间隔 ~35µs，
             *    几微秒即命中，吞吐满速（v1.2.73 实测 27k/s）；
             * ② 停点静默 ≥300ms：每轮睡 20ms 再探——futex 死锁期驱动
             *    graceful（静默 8s 判据），CPU 趋零。 */
            /* v1.2.89 ★★★ 终解（v1.2.76 判据的自杀性副作用）：
             * v1.2.76 把"忙"定为 ≥100 停点/s，其余 20ms 慢轮询——本意
             * 是抓 futex 死锁。但 ART 构造期（libart 298 构造/heap 逐页
             * 初始化）的真实停点速率只有 ~50/s（大量纯用户态计算）：
             * 每个停点被 20ms 轮询节流 → mprotect 间隔恰 40ms（=2×20ms
             * 停点延迟）→ 启动被拖慢 4~10 倍 → 90s 兜底误杀。
             * 修复：忙阈值 100→32（50/s 的 ART 构造恢复满速；futex
             * 死锁 0/s 依旧走慢轮询）；慢轮询 20ms→5ms（兜底灵敏度×4）。
             * 自洽性检查：真死锁（0 停点）8s 判据不受影响；风暴期
             * 39044/s 依旧满速；graceful 三级判据原样保留。 */
            static long last_stop_ms = 0;
            static unsigned spin_cnt = 0;
            struct timespec now_ts;
            clock_gettime(CLOCK_MONOTONIC, &now_ts);
            long now_ms = now_ts.tv_sec * 1000 + now_ts.tv_nsec / 1000000;
            if (t > 0) { last_stop_ms = now_ms; spin_cnt++; }
            t = waitpid(-1, &st, WNOHANG | __WALL);
            if (t == 0) {
                if (now_ms - last_stop_ms < 1000 && spin_cnt >= 32) {
                    /* v1.2.104：并发（b-runner）模式下降速——两个 ART + 两个
                     * tracer 自旋 + 宿主 = 4 核满载，真机实测双容器同时饿死
                     * 294s（cgroup throttling）。B 容器的 tracer 加 10ms 让步。 */
                    if (g_c_out_fd >= 0) {
                        struct timespec ts2 = {0, 10 * 1000 * 1000};
                        nanosleep(&ts2, NULL);
                    }
                    continue;                    /* 忙期（≥32停点/s）：满速自旋（A）/半速（B 并发） */
                }
                struct timespec ts = {0, 5 * 1000 * 1000};    /* 5ms */
                nanosleep(&ts, NULL);
                spin_cnt = 0;                    /* 清零：速率窗口重启 */
                if (tvm_graceful_check(1)) { g_t_status = 0; g_t_done = 1; break; }
                continue;                        /* 真静默：5ms 慢轮询 */
            }
        }
        if (t == 0) {
            /* v1.2.20 ★★★ 这里**不再尝试 PTRACE_INTERRUPT**。
             * 真机 v1.2.19 实锤：`PTRACE_INTERRUPT 失败 errno=5`（EIO）——
             * 该请求只支持 PTRACE_SEIZE 附属的 tracee，而当时是 PTRACE_TRACEME，
             * 注定失败（内核 ptrace.c 源码定罪；v1.2.37~38 试过 SEIZE，v1.2.39 回退 TRACEME
             * ——SEIZE 受环境层摆布，INTERRUPT 资格暂弃，需要时再议）；
             * 何况"强制拉停"本不必要：
             * 主线程已经用 /proc/<pid>/syscall + process_vm_readv 完成体检
             *（见 tvm_hang_dump_procfs，两者都不要求对方停止、也不要求是 tracer）。
             * 因此这里只需清掉标志，继续轮询即可。 */
            g_hang_probe = 0;
            usleep(2000);
            continue;
        }
        if (t < 0) {
            if (errno == EINTR) continue;
            /* v1.2.73 ★★★ 七版悬案终章：guest 全员退出（exit_group 被 BPF
             * 放行=原生执行=trace 不打印——48 停点是 Shutdown 舞步余晖，
             * 之后 waitpid ECHILD）。此前 break 无声——现在大声说出来，
             * 并广播终局（g_t_done=1 让主线程收尾）。 */
            ptrace_log("[tracer] waitpid=%d errno=%d（%s）—— guest 已全员退出，示踪终局",
                       (int)t, errno, errno == ECHILD ? "ECHILD" : "其他");
            g_t_status = 0; g_t_done = 1;
            break;                                 /* ECHILD 等：收完，终局 */
        }
        /* v1.2.17 ★ 看门狗的体检代办：主线程置 g_hang_probe，tracer（本线程）
         * 在停点执行——ptrace 请求只有本线程有资格（教训五第五击）。
         * tracee 此刻睡在 futex，本次停点是 PTRACE_SYSCALL 的即时返回。 */
        g_guest_ticks++;                     /* v1.2.19：任何停点都算"guest 在前进" */
        /* v1.2.68 ★ 终极取证：停点级 [beat] 心跳（每 10s 无条件）。
         * [chk] 直播揭示：stress 后 check 停跑但停点仍在——本心跳区分
         * "循环死了"（无 beat）vs "检查块被绕过"（有 beat 无 chk）。 */
        {
            static time_t b_last = 0;
            time_t bnow = time(NULL);
            if (bnow - b_last >= 10) {
                b_last = bnow;
                char bb[160];
                int bk = snprintf(bb, sizeof(bb),
                    "[TwinVM] [beat] 循环活着（ticks=%llu 最近 nr=%d）\n",
                    g_guest_ticks, g_t_last_nr);
                live_write(bb, (size_t)bk);
            }
        }
        /* v1.2.62 ★★★ 优雅关机 v6·架构级：判定搬到 **tracer 线程**（真机
         * v1.2.61 实锤：主线程在 guest 启动 38~40s 间悄然死亡——alive 心跳
         * 与 graceful-watch 同时停摆，而 tracer 的 tee/trace 照常打到
         * stress 输出之后。主线程一死，驻留其上的优雅关机全成空谈。
         * tracer 是永不退场的示踪者：guest 的每个停点都经它的手——
         * 判定在这里做，主线程死活无关。停点驱动天然节流：收尾打转
         * 每秒几十停点=每秒检查几十次；真静默时 waitpid 阻塞=不空转。 */
        {   /* v1.2.62：判定抽成公共函数（停点驱动 + 静默驱动共用） */
            if (tvm_graceful_check(1)) {
                g_t_status = 0; g_t_done = 1;
                break;                         /* 终局：退出示踪循环 */
            }
        }
        if (g_hang_probe && t == g_t_pid) {
            g_hang_probe = 0;
            struct tvm_arm_regs hr;
            memset(&hr, 0, sizeof(hr));
            if (tvm_getregs(t, &hr) == 0) {
                char rline[256];
                int rk = snprintf(rline, sizeof(rline),
                    "[TwinVM] [hang] pc=0x%llx lr=0x%llx sp=0x%llx x0=0x%llx x1=0x%llx x2=0x%llx\n",
                    hr.pc, hr.regs[30], hr.sp, hr.regs[0], hr.regs[1], hr.regs[2]);
                live_write(rline, (size_t)rk);
                /* 栈扫描 */
                char hpath[48];
                snprintf(hpath, sizeof(hpath), "/proc/%d/maps", (int)t);
                FILE* hmf = fopen(hpath, "r");
                if (hmf) {
                    static struct tvm_hseg2 {
                        unsigned long long lo, hi, base;
                        char name[56]; int named;
                    } hsegs[256];
                    int hn = 0;
                    char hln[256];
                    while (fgets(hln, sizeof(hln), hmf) && hn < 256) {
                        unsigned long long lo, hi, fo;
                        char pm[8] = {0}, nm[56] = {0};
                        /* v1.2.26：**全量收录**（原来只收可执行段 → "sp 所在段"永远显示
                         * "★不在任何映射"，是过滤器造成的假象；真机 v1.2.24/25 两轮都栽在这）。
                         * 是否可执行留给下面的扫描条件判断。 */
                        if (sscanf(hln, "%llx-%llx %4s %llx %*s %*s %55[^\n]",
                                   &lo, &hi, pm, &fo, nm) >= 4) {
                            hsegs[hn].lo = lo; hsegs[hn].hi = hi;
                            if (nm[0]) { hsegs[hn].named = 1; hsegs[hn].base = lo - fo;
                                         strcpy(hsegs[hn].name, nm); }
                            else { hsegs[hn].named = 0; hsegs[hn].base = lo;
                                   hsegs[hn].name[0] = 0; }
                            hn++;
                        }
                    }
                    fclose(hmf);
                    char sb2[8192];
                    if (hn > 0 && tvm_read_pv(t, hr.sp, sizeof(sb2), sb2) == 0) {
                        int hits = 0;
                        for (size_t so = 0; so + 8 <= sizeof(sb2) && hits < 20; so += 8) {
                            unsigned long long v;
                            memcpy(&v, sb2 + so, 8);
                            for (int q = 0; q < hn; q++)
                                if (v >= hsegs[q].lo && v < hsegs[q].hi) {
                                    char l2[200];
                                    int lk = snprintf(l2, sizeof(l2),
                                        "[TwinVM] [hang] sp+0x%-4zx 0x%llx %s +0x%llx\n",
                                        so, v, hsegs[q].named ? hsegs[q].name : "[anon]",
                                        hsegs[q].named ? (v - hsegs[q].base) : (v - hsegs[q].lo));
                                    live_write(l2, (size_t)lk);
                                    hits++;
                                    break;
                                }
                        }
                        if (hits == 0) {
                            const char* e2 = "[TwinVM] [hang] （栈上无可执行段地址）\n";
                            live_write(e2, strlen(e2));
                        }
                    }
                    /* maps 全量落盘 */
                    const char* mh = "[TwinVM] [hang-maps] guest 布局：\n";
                    live_write(mh, strlen(mh));
                    hmf = fopen(hpath, "r");
                    if (hmf) {
                        char mb2[2048];
                        size_t mn;
                        while ((mn = fread(mb2, 1, sizeof(mb2), hmf)) > 0)
                            live_write(mb2, mn);
                        fclose(hmf);
                    }
                }
            } else {
                char el[128];
                int ek = snprintf(el, sizeof(el), "[TwinVM] [hang] GETREGSET 失败 errno=%d\n", errno);
                live_write(el, (size_t)ek);
            }
        }
        if (WIFSTOPPED(st)) {
            int sig = WSTOPSIG(st);
            /* v0.7.63（关键）：syscall-entry/exit 停点的 wait 状态是 (SIGTRAP|0x80)<<8
             * → WSTOPSIG 返回 0x85，不是 5！v0.7.62 真机实锤：旧判断 sig==SIGTRAP
             * 漏掉所有 syscall 停点（0 事件），且把它们当"真信号"转发注入
             * signal 133（内核恰好丢弃才没炸）。补上 0x80 位掩码。 */
            if (sig == (SIGTRAP | 0x80)) {
                struct tvm_psi info;
                memset(&info, 0, sizeof(info));
                long r = ptrace(TVM_PSI_GSI, t, (void*)(long)sizeof(info), &info);
                /* v1.1.0 ★ 打印 GSI 的原始返回：r / op —— 之前一直假设 op==1 是 entry，
                 * 但"回填从未发生、openat 从未被拦"说明这个假设可能不成立。 */
                {
                    static int s_gsi = 0;
                    if (s_gsi < 20) { s_gsi++;
                        ptrace_log("[ptrace] GSI: r=%ld op=%d arch=%u nr=%ld（t=%d）",
                               r, (int)info.op, info.arch,
                               (r > 0 && info.op == TVM_PSI_ENTRY) ? (long)info.u.entry.nr : -1L,
                               (int)t);
                    }
                }
                if (r > 0 && info.op == TVM_PSI_ENTRY) {
                    /* v1.0.0：ptrace 后端 —— 需要虚拟化的号：把 x8 改成 -1，让内核
                     * 直接返回 ENOSYS（等于"跳过"该 syscall），并记下待办；
                     * exit 停点再把返回值改写成引擎的结果。 */
                    if (info.u.entry.nr == 220 || info.u.entry.nr == 435) {   /* clone/clone3 */
                        ptrace_log("[trace] ★ clone(nr=%ld) 由 t=%d 发起 —— 期待新线程停点",
                               (long)info.u.entry.nr, (int)t);
                    }
                    /* v1.2.0 ★ mmap-fill：mmap 带 ROM 虚拟 fd —— 不跳过、改参数
                     * 原生匿名执行（内核真造页），exit 停点灌 ROM 内容。 */
                    if (info.u.entry.nr == 222 /* mmap */
                        && !(info.u.entry.args[3] & 0x20 /* MAP_ANONYMOUS */)
                        && (long)info.u.entry.args[4] >= VM_FD_BASE) {
                        struct tvm_arm_regs mr;
                        if (tvm_getregs(t, &mr) == 0) {
                            unsigned long long fl = mr.regs[3];
                            fl = (fl & 0x10 /*MAP_FIXED*/) | 0x02 /*MAP_PRIVATE*/
                               | 0x20 /*MAP_ANONYMOUS*/;
                            int fi2 = -1;
                            for (int i = 0; i < TVM_FILL_SLOTS; i++)
                                if (!g_fill[i].active) { fi2 = i; break; }
                            if (fi2 >= 0) {
                                g_fill[fi2].active = 1;
                                g_fill[fi2].tid = t;
                                g_fill[fi2].fd = (long)mr.regs[4];
                                g_fill[fi2].len = mr.regs[1];
                                g_fill[fi2].off = mr.regs[5];
                                g_fill[fi2].at = 0;
                                mr.regs[2] |= 0x2 /*PROT_WRITE*/;
                                mr.regs[3] = fl;
                                mr.regs[4] = ~0ULL;     /* fd=-1 */
                                mr.regs[5] = 0;         /* offset=0 */
                                if (tvm_setregs(t, &mr) == 0) {
                                    ptrace_log("[mmap-fill] fd=%ld len=%llu off=%llu → 匿名原生执行（t=%d）",
                                               g_fill[fi2].fd, g_fill[fi2].len,
                                               g_fill[fi2].off, (int)t);
                                    ptrace(PTRACE_SYSCALL, t, 0, 0);
                                    continue;           /* 不走跳过路径 */
                                }
                                g_fill[fi2].active = 0;
                            } else {
                                /* v1.2.35：槽满不再静默 fallthrough（会走原生 mmap(虚拟fd)
                                 * → 内核 EBADF/EINVAL → dlopen 失败死循环——真机
                                 * v1.2.34 的 read/mmap/munmap 循环嫌疑路径） */
                                static int s_full = 0;
                                if (s_full++ < 5)
                                    ptrace_log("[mmap-fill] !! 槽满 %d——本次 fd=%lld 原生执行（可能 EINVAL/EBADF）",
                                               TVM_FILL_SLOTS,
                                               (long long)info.u.entry.args[4]);
                            }
                        }
                    }
                    /* v1.2.6 ★★★ ioctl(真 fd) 原生直通——终结 ART 之死：
                     * 真机 v1.2.5 三步定罪（栈扫描符号化 + 反汇编）：
                     *   ① guest 死于 libart 的 mark_compact.cc 静态构造器链
                     *   ② KernelSupportsUffd(): syscall(282) 建 uffd（282 未拦，
                     *      拿到真 fd=37）→ ioctl(fd, 0xc018aa3f=ColorOS uffd
                     *      API 握手, &api)——被我们的拦截名单吃掉、引擎一律回
                     *      ENOTTY（旧 BPF 时代"真 fd ioctl 原生放行"的语义在
                     *      ptrace 后端丢了）
                     *   ③ art 视为内核异常 → LOG(FATAL)（消息进 logd socket）
                     *      → 无遗言 abort
                     * 修：entry 停点见 ioctl 且 fd∈[3,10000) → 不改写、原生执行
                     * （子进程内核对真 uffd 握手，成功写回 guest 内存）。 */
                    /* v1.2.86 ★★★ mprotect 调用者直击：循环期 99% nr=226，
                     * 帧 #00 ra=linker64 call_constructors+0x22c（blr x23 的
                     * 返回点）——真凶是 298 个 init_array 构造函数之一。
                     * 这里在 mprotect entry 抓【构造函数地址】：
                     * call_constructors 的循环寄存器 x23=当前构造函数指针
                     * （v1.2.85 反汇编定证 0xfcd74: blr x23）。x23 在
                     * mprotect 的现场（callee-saved）仍保留！打印它+lr。 */
                    if (info.u.entry.nr == 226
                        && (long)info.u.entry.args[2] == 3       /* RW 方向 */
                        && (unsigned long long)info.u.entry.args[0] == 0x78d3ab8000ULL) {
                        /* v1.2.88 ★ 修正：v1.2.87 的 6 条额度全被启动期正常
                         * mprotect 吃掉（帧链全是装载段保护）——循环特征地址
                         * 过滤：v1.2.86 探针实锤循环只翻 0x78d3ab8000 这页
                         * （libart ELF 头/RELRO 页）。只抓它。 */
                        /* v1.2.87 ★ 定罪：循环=linker __cxa_atexit 对 libart
                         * RELRO 头页 R↔RW 反复翻转（9ffac/9fff0 两调用点）。
                         * 上层有人在循环调 cxa_atexit——抓调用者 x29 帧链。 */
                        static int s_atx = 0;
                        if (s_atx < 12) {
                            s_atx++;
                            struct tvm_arm_regs pr;
                            if (tvm_getregs(t, &pr) == 0) {
                                unsigned long long fp = pr.regs[29];
                                char out[512];
                                int o = snprintf(out, sizeof(out),
                                    "[atexit-probe] #%d mprotect(0x%llx,RW) 帧链：",
                                    s_atx, (unsigned long long)info.u.entry.args[0]);
                                for (int fi = 0; fi < 5 && o < 400; fi++) {
                                    unsigned long long pair[2];
                                    if (tvm_read_tracee(t, (long)fp, 16, (char*)pair) != 16) break;
                                    o += snprintf(out+o, sizeof(out)-o, " ra=0x%llx", pair[1]);
                                    if (pair[0] < 0x1000) break;
                                    fp = pair[0];
                                }
                                snprintf(out+o, sizeof(out)-o, "\n");
                                ptrace_log("%s", out);
                            }
                        }
                    }
                    /* v1.2.83 ★★★ mmap fd 截断修复（EINVAL 终结）：
                     * v1.2.82 真机铁证：fd=0x00000000ffffffff（AArch64 w4 写 -1
                     * 零扩展）→ 内核把 4294967295 当合法 fd 号检查 → EINVAL →
                     * ART uffd heap 判 MMAP_TRYLOCK 不支持 → 降级重建 →
                     * 无限 dlopen 循环（v1.2.80 帧链定罪全案）。
                     * 修：entry 停点改写 x4 为 64 位 -1（语义等价：MAP_ANONYMOUS
                     * 本就该无 fd；非匿名场景该形态也只可能是 -1 笔误）。 */
                    if (info.u.entry.nr == 222) {
                        if ((unsigned long long)info.u.entry.args[4]
                                == 0x00000000ffffffffULL) {
                            struct tvm_arm_regs mr4;
                            if (tvm_getregs(t, &mr4) == 0) {
                                mr4.regs[4] = (unsigned long long)-1;
                                tvm_setregs(t, &mr4);
                                static int s_fd_n = 0;
                                if (s_fd_n++ < 8)
                                    ptrace_log("[mmap-fd32] 第 %d 次：x4 0xffffffff → 64 位 -1 规范化（t=%d）",
                                               s_fd_n, (int)t);
                            }
                        }
                    }
                    if (info.u.entry.nr == 29 /* ioctl */) {
                        struct tvm_arm_regs ir;
                        if (tvm_getregs(t, &ir) == 0
                            && ir.regs[0] >= 3 && ir.regs[0] < 10000) {
                            /* v1.2.81 ★ UFFDIO 直通结果观测（exit 读 x0）：
                             * req=0xc018aa3f UFFDIO_API / 0xc020aa04 REGISTER——
                             * ART16 的 uffd heap 靠它。失败→MMAP_TRYLOCK 提示→
                             * 降级重建循环（v1.2.80 真机定罪）。记 req 供 exit 配对。 */
                            static unsigned long long s_uffd_req = 0;
                            if (ir.regs[1] == 0xc018aa3fULL || ir.regs[1] == 0xc020aa04ULL)
                                s_uffd_req = ir.regs[1];
                            static int s_ictl = 0;
                            if (s_ictl++ < 12)
                                ptrace_log("[ioctl] fd=%llu 真直通（req=0x%llx，t=%d）",
                                           ir.regs[0], ir.regs[1], (int)t);
                            ptrace(PTRACE_SYSCALL, t, 0, 0);
                            continue;               /* 不进跳过路径 */
                        }
                    }
                    /* v1.2.23：拦截主体已抽成独立 noinline 函数 tvm_intercept_now()。
                     * 原因：真机构建比对（-O0 vs -O1/-O2/-Os）显示，这段内联在示踪循环里的
                     * 代码会被 clang **静默整块删除**（产物里 [caller]/SETREGSET/已拦截 串全没了，
                     * 而失败分支却留下）——意味着 `x8 = -1`（让内核跳过原 syscall）也没执行，
                     * 拦截形同虚设。前端 -Wunreachable-code 不报警，# pragma 也压不住，
                     * 只有独立函数体（noinline）能保证被完整编译出来。 */
                    if (tvm_virt_nr((long)info.u.entry.nr))
                        tvm_intercept_now(t, (long)info.u.entry.nr, info.u.entry.args);
                    /* v1.2.23：mmap 原生执行 → 引擎看不到结果，但 tracer 能看到 entry/exit 两个停点：
                     * entry 记 hint、exit 读 x0，即可回答"内核到底有没有按低位 hint 给地址"
                     *（ART 的 MapInternalArtLow4GBAllocator 正是靠这个在低 4GB 找落点）。 */
                    if ((long)info.u.entry.nr == 222
                        && ((long)info.u.entry.args[4] == -1
                            || (unsigned int)info.u.entry.args[4] == 0xffffffffU)) {
                        /* v1.2.34 ★ 修探针盲区：真机 v1.2.33 实锤 guest 匿名 mmap
                         * 的 fd 寄存器值是 0x00000000ffffffff 而非 64 位全 1——
                         * AArch64 调用惯例 int 参数写 w4 会清零 x4 高 32 位。
                         * 旧判定 (long)==-1 从未命中 → v1.2.22 以来 [mmap] hint→ret
                         * 观测行一条都没打过（本想看 Low4GB 分配器拿没拿到
                         * hint 地址，探针瞎了三轮）。两条判定都收。 */
                        g_mmap_hint = (unsigned long long)info.u.entry.args[0];
                        g_mmap_len  = (unsigned long long)info.u.entry.args[1];
                        g_mmap_pend = 1;
                        /* v1.2.91 ★★★ 低 4GB 强制落位（entry 侧接管）：
                         * 上一次 mmap 的低 4GB hint 未被内核满足（exit 侧置
                         * g_low4_active）——本轮 mmap 无论带不带 hint，
                         * 只要目标尺寸匹配，就在 guest 低 4GB 找空洞，
                         * 改写 x0=空洞地址、x3 |= MAP_FIXED(0x10) 放行。 */
                        if (g_low4_active && g_mmap_len == g_low4_want_len) {
                            g_low4_active = 0;
                            unsigned long long hole = tvm_low4_hole(t, g_mmap_len);
                            if (hole) {
                                struct tvm_arm_regs lr4;
                                if (tvm_getregs(t, &lr4) == 0) {
                                    lr4.regs[0] = hole;
                                    lr4.regs[3] = lr4.regs[3] | 0x10;  /* MAP_FIXED */
                                    tvm_setregs(t, &lr4);
                                    g_mmap_hint = hole;   /* exit 观测对齐 */
                                    tvm_low4_mark(hole, g_mmap_len);   /* v1.2.93 记账 */
                                    static int s_low4fix = 0;
                                    if (s_low4fix++ < 12)
                                        ptrace_log("[low4-fix] #%d 强制落位 0x%llx len=0x%llx（MAP_FIXED）",
                                                   s_low4fix, hole, g_mmap_len);
                                }
                            } else {
                                static int s_no_hole = 0;
                                if (s_no_hole++ < 4)
                                    ptrace_log("[low4-fix] !! 低4GB 无 %lluB 空洞——放弃接管",
                                               g_low4_want_len);
                                g_low4_active = 1;   /* 下轮再试 */
                            }
                        }
                    }
                    /* v1.0.5：**只记录被拦截的 syscall** —— 否则 ART 的 GC 循环
                     * （mmap/munmap/msync 各 8 万+ 次）会把日志刷到几十 MB。
                     * 真正有用的现场被淹没（真机 v1.0.4：26.6 MB）。 */
                    int tvm_intercepted = tvm_virt_nr((long)info.u.entry.nr);
                    char line[96];
                    /* v1.2.33：watchlist syscall 的 entry 参数留痕 */
                    if (tvm_fail_watch((long)info.u.entry.nr)) {
                        for (int z = 0; z < 6; z++)
                            g_wargs[z] = (unsigned long long)info.u.entry.args[z];
                        g_wargs_nr = (long)info.u.entry.nr;
                    }
                    /* v1.2.31：futex 调用留痕（环形，只记 op/uaddr/val，开销可忽略） */
                    if ((long)info.u.entry.nr == 98) {
                        int fi3 = g_futex_idx % TVM_FUTEX_RING;
                        g_futex_ring[fi3].op    = (long)info.u.entry.args[1];
                        g_futex_ring[fi3].uaddr = (unsigned long long)info.u.entry.args[0];
                        g_futex_ring[fi3].val   = (unsigned long long)info.u.entry.args[2];
                        g_futex_idx++;
                        /* v1.2.35 ★★★ futex WAIT 的调用者留痕（≤8 次）——
                         * 真机 v1.2.34 符号化链：Thread::Startup → pthread_key_create
                         * 内部 futex WAIT，但 uaddr 落在堆上（非 g_tls_keys 静态区），
                         * 说明等待者藏在更深一层。WAIT 的 lr 一打，符号化直取答案。
                         * FUTEX_WAIT=0 / FUTEX_WAIT_BITSET=9（含 PRIVATE 位 0x80）。 */
                        long fop = (long)info.u.entry.args[1] & 0x7f;
                        if (fop == 0 || fop == 9) {
                            static int s_fwc = 0;
                            if (s_fwc < 8) {
                                struct tvm_arm_regs fw;
                                if (tvm_getregs(t, &fw) == 0) {
                                    s_fwc++;
                                    ptrace_log("[caller] ★futex-WAIT #%d lr=0x%llx pc=0x%llx uaddr=0x%llx val=%llu",
                                               s_fwc,
                                               (unsigned long long)fw.regs[30],
                                               (unsigned long long)fw.pc,
                                               (unsigned long long)info.u.entry.args[0],
                                               (unsigned long long)info.u.entry.args[2]);
                                }
                            }
                        }
                    }
                    /* v1.2.34：nr 直方图留痕（环形，每次 entry 一次写，开销可忽略） */
                    g_nr_ring[g_nr_idx % TVM_NR_RING] = (long)info.u.entry.nr;
                    g_nr_idx++;
                    /* v1.2.25：抓帧请求 —— 在真实停点上取 x29/x30 并回溯帧链 */
                    if (g_want_stack && !g_stack_done) {
                        g_stack_done = 1;
                        struct tvm_arm_regs fr;
                        if (tvm_getregs(t, &fr) == 0)
                            tvm_dump_frames(t, (long)info.u.entry.nr, &fr);
                    }
                    g_t_last_nr = (int)info.u.entry.nr;
                    g_t_last_tid = t;
                    g_t_last_pc = (unsigned long long)info.instruction_pointer;
                    int k = tvm_intercepted ? snprintf(line, sizeof(line),
                                     "[TwinVM] [trace] #%-6d t%-6d nr=%-4ld a0=0x%llx a1=0x%llx a2=0x%llx\n",
                                     ++g_t_events, (int)t, (long)info.u.entry.nr,
                                     (unsigned long long)info.u.entry.args[0],
                                     (unsigned long long)info.u.entry.args[1],
                                     (unsigned long long)info.u.entry.args[2]) : 0;
                    /* v1.0.1：后端心跳 —— 每 2000 个 syscall 写一条，避免大段静默 */
                    if (g_ptrace_backend && (g_t_events % 200) == 0) {
                        char hb[160];
                        int hn = snprintf(hb, sizeof(hb),
                            "[TwinVM] ▶ 已虚拟化 %d 个 syscall（最近 nr=%ld tid=%d）\n",
                            g_t_events, (long)info.u.entry.nr, (int)t);
                        if (hn > 0) { live_write(hb, (size_t)hn); LOGI("%s", hb); }
                    }
                    if (k > 0) {
                        memcpy(g_t_ring[g_t_ring_idx], line, (size_t)k + 1);
                        g_t_ring_idx = (g_t_ring_idx + 1) % TVM_RING;
                        if (g_t_ring_cnt < TVM_RING) g_t_ring_cnt++;
                        live_write(line, (size_t)k);
                        /* v1.1.5：tee 扩面 —— write/writev 全 fd payload +
                         * 0 字节 iov 裸 dump + openat/faccessat/newfstatat 路径 */
                        if (info.u.entry.nr == 64 || info.u.entry.nr == 66 ||
                            info.u.entry.nr == 56 || info.u.entry.nr == 48 ||
                            info.u.entry.nr == 79)
                            tvm_tee_stdio(t, (long)info.u.entry.nr, info.u.entry.args);
                    }
                }
                /* v1.0.0：exit 停点 —— 若该 tid 有待办，说明这次是我们"跳过"的
                 * syscall；用引擎算出结果并写回 x0（返回值寄存器）。 */
                if (r > 0 && info.op == 2 /* PSI_EXIT */ && g_mmap_pend) {
                    g_mmap_pend = 0;
                    struct tvm_arm_regs mr;
                    if (tvm_getregs(t, &mr) == 0) {
                        static int s_mmlog = 0;
                        /* v1.2.35：低 4GB hint 全打（Low4GB 分配器特征），
                         * 普通的限 40 条——上轮 40 条配额全被启动早期普通
                         * mmap 吃掉，分配器行为反而没观测到。 */
                        int low4 = g_mmap_hint != 0
                                 && g_mmap_hint < 0x100000000ULL;
                        if (low4 || s_mmlog < 40) {
                            if (!low4) s_mmlog++;
                            ptrace_log("[mmap] hint=0x%llx len=0x%llx -> ret=0x%llx%s",
                                       g_mmap_hint, g_mmap_len, (unsigned long long)mr.regs[0],
                                       ((long long)mr.regs[0] > 0 &&
                                        (unsigned long long)mr.regs[0] != g_mmap_hint)
                                       ? "  ← 未按 hint" : "");
                        }
                        /* v1.2.91 ★★★ 低 4GB 强制落位（v1.2.20/21 老案重现终结）：
                         * ART 的 MapInternalArtLow4GBAllocator 要求低 4GB（压缩
                         * 指针）。内核"未按 hint"给高地址时（低 4GB 碎片化），
                         * ART 会用 msync 逐页扫描区间可用性 → 全 ENOMEM →
                         * 死循环（v1.2.90 真机实锤：196/s 的 msync 扫描 90s）。
                         * 修（无需注入）：guest 下一次 mmap(低4GB hint) 的
                         * 【entry 停点】处，由 tracer 用 /proc/<pid>/maps 在
                         * 低 4GB 找一块够大的空洞，改写 x0(地址)+x3(加
                         * MAP_FIXED) 后放行——内核原生成像，guest 拿到精确
                         * 落点。本次 exit 只留观测（改参在 entry 做）。 */
                        if (low4
                            && (long long)mr.regs[0] > 0
                            && (unsigned long long)mr.regs[0] != g_mmap_hint
                            && g_mmap_len >= 0x1000ULL) {
                            /* v1.2.92 ★★★ rewind 重执行（v1.2.91 的等下轮
                             * 失败实锤：ART 拿高地址后转 msync 扫描，同尺寸
                             * mmap 永不再来——接管窗口就此错过）。
                             * 现场直接补救：找空洞 → PC 回拨到 svc 指令前 →
                             * 改写 x0=空洞/x3|=MAP_FIXED → 让 guest 把同一
                             * 条 mmap 再执行一遍（内核 FIXED 精确落位）。
                             * 高地址结果先 munmap 退还（由 guest 重执行时
                             * FIXED 覆盖同尺寸？不——旧结果在高地址，FIXED
                             * 落在新空洞，旧的会泄漏：先让重执行前 munmap
                             * ——不能注入就【接受】这次泄漏（ART 退出时全
                             * 释放；8MB×几次可忽略）——先跑通再说）。 */
                            unsigned long long hole = tvm_low4_hole(t, g_mmap_len);
                            if (hole && hole < 0x100000000ULL) {
                                /* PC 回拨：svc #0 是 4 字节指令，exit 停点
                                 * 的 pc 已越过它——回拨 4。 */
                                unsigned long long spc = (unsigned long long)
                                    info.instruction_pointer;
                                mr.regs[0] = hole;
                                mr.regs[3] = mr.regs[3] | 0x10;  /* MAP_FIXED */
                                /* 用 tvm_arm_regs 复刻的 pc 字段（结构里
                                 * 命名 pc）——直接写 regs[32]？我们的
                                 * tvm_arm_regs 有独立 pc 字段。 */
                                mr.pc = spc - 4;
                                tvm_setregs(t, &mr);
                                g_mmap_hint = hole;
                                static int s_rw = 0;
                                if (s_rw++ < 12)
                                    ptrace_log("[low4-rewind] #%d len=0x%llx → rewind+FIXED 至 0x%llx",
                                               s_rw, g_mmap_len, hole);
                                tvm_low4_mark(hole, g_mmap_len);   /* v1.2.93 记账 */
                                g_low4_rewound = 1;
                                /* 注意：本函数继续走会把 x0 覆盖成观测值——
                                 * 必须立刻跳过后续 x0 写回。做法：rewind 后
                                 * 直接 PTRACE_SYSCALL 放行并 continue。 */
                                ptrace(PTRACE_SYSCALL, t, 0, 0);
                                continue;
                            } else {
                                static int s_no_hole = 0;
                                if (s_no_hole++ < 4)
                                    ptrace_log("[low4] !! 低4GB 无 %lluB 空洞——放弃",
                                               g_mmap_len);
                            }
                        }
                    }
                }
                /* v1.2.32：watchlist 上的 syscall，若返回 -errno 就留痕 */
                if (r > 0 && info.op == 2 /* PSI_EXIT */ && tvm_fail_watch((long)g_t_last_nr)) {
                    struct tvm_arm_regs fr2;
                    if (tvm_getregs(t, &fr2) == 0) {
                        long rv2 = (long)fr2.regs[0];
                        if (rv2 < 0 && rv2 > -4096) {
                            long enr = (long)g_t_last_nr, eerr = -rv2;
                            for (int z = 0; z < g_nfail && z < 64; z++) {
                                if (g_fail[z].nr == enr) {
                                    g_fail[z].err = eerr;
                                    /* v1.2.81 ★ mmap EINVAL 深挖：64 位 fd 形态直播
                                     * （32 位 -1 截断嫌疑：0xffffffff vs 0xffff...ff）。 */
                                    if (enr == 222 && (eerr == 22 || eerr == -22)) {
                                        static int s_mmei = 0;
                                        if (s_mmei < 8) { s_mmei++;
                                            ptrace_log("[mmap-einval] #%d addr=0x%llx len=0x%llx prot=0x%llx flags=0x%llx fd=0x%llx off=0x%llx",
                                                       s_mmei,
                                                       g_wargs[0], g_wargs[1], g_wargs[2], g_wargs[3], g_wargs[4], g_wargs[5]);
                                        }
                                    }
                                    if (g_fail[z].cnt++ < 6)
                                        ptrace_log("[fail] nr=%ld → -%ld（第 %d 次） "
                                                   "args=0x%llx,0x%llx,0x%llx,0x%llx,0x%llx,0x%llx",
                                                   enr, eerr, g_fail[z].cnt,
                                                   g_wargs_nr == enr ? g_wargs[0] : 0ULL,
                                                   g_wargs_nr == enr ? g_wargs[1] : 0ULL,
                                                   g_wargs_nr == enr ? g_wargs[2] : 0ULL,
                                                   g_wargs_nr == enr ? g_wargs[3] : 0ULL,
                                                   g_wargs_nr == enr ? g_wargs[4] : 0ULL,
                                                   g_wargs_nr == enr ? g_wargs[5] : 0ULL);
                                    goto fail_done;
                                }
                            }
                            if (g_nfail < 64) {
                                int z = g_nfail++;
                                g_fail[z].nr = enr; g_fail[z].err = eerr; g_fail[z].cnt = 1;
                                /* v1.2.82 ★ mmap EINVAL 直播（首次路径——v1.2.81
                                 * 只插在老条目命中路径，真机实锤首次走这里没经过） */
                                if (enr == 222 && (eerr == 22 || eerr == -22)) {
                                    ptrace_log("[mmap-einval] addr=0x%llx len=0x%llx prot=0x%llx flags=0x%llx fd=0x%llx off=0x%llx",
                                               g_wargs_nr == enr ? g_wargs[0] : 0ULL,
                                               g_wargs_nr == enr ? g_wargs[1] : 0ULL,
                                               g_wargs_nr == enr ? g_wargs[2] : 0ULL,
                                               g_wargs_nr == enr ? g_wargs[3] : 0ULL,
                                               g_wargs_nr == enr ? g_wargs[4] : 0ULL,
                                               g_wargs_nr == enr ? g_wargs[5] : 0ULL);
                                }
                                ptrace_log("[fail] nr=%ld → -%ld（首次） "
                                           "args=0x%llx,0x%llx,0x%llx,0x%llx,0x%llx,0x%llx",
                                           enr, eerr,
                                           g_wargs_nr == enr ? g_wargs[0] : 0ULL,
                                           g_wargs_nr == enr ? g_wargs[1] : 0ULL,
                                           g_wargs_nr == enr ? g_wargs[2] : 0ULL,
                                           g_wargs_nr == enr ? g_wargs[3] : 0ULL,
                                           g_wargs_nr == enr ? g_wargs[4] : 0ULL,
                                           g_wargs_nr == enr ? g_wargs[5] : 0ULL);
                            }
                        }
                    }
                }
            fail_done:
                if (r > 0 && info.op == 2 /* PSI_EXIT */) {
                    /* v1.2.0 ★ mmap-fill：这个 tid 有改写过的 mmap 待灌——
                     * x0 就是 guest 新页地址，现在把 ROM 内容灌进去。 */
                    for (int i = 0; i < TVM_FILL_SLOTS; i++) {
                        if (g_fill[i].active && g_fill[i].tid == t) {
                            g_fill[i].active = 0;
                            struct tvm_arm_regs fr;
                            memset(&fr, 0, sizeof(fr));
                            int gok = tvm_getregs(t, &fr);
                            if (gok == 0
                                && fr.regs[0] < 0xfffffffffffff000ULL /* 成功地址 */) {
                                unsigned long long at = fr.regs[0];
                                VmCpu* cpu = vm_machine_current();
                                size_t ino_size = 0;
                                const uint8_t* data = cpu ? vm_vfs_fd_data(cpu, g_fill[i].fd, &ino_size) : NULL;
                                if (data && g_fill[i].off < ino_size) {
                                    size_t n = ino_size - (size_t)g_fill[i].off;
                                    if (n > (size_t)g_fill[i].len) n = (size_t)g_fill[i].len;
                                    size_t done = 0;
                                    while (done < n) {
                                        size_t ch = n - done > (256u << 10) ? (256u << 10) : n - done;
                                        struct iovec l = { (void*)(data + g_fill[i].off + done), ch };
                                        struct iovec rv = { (void*)(uintptr_t)(at + done), ch };
                                        extern ssize_t process_vm_writev(pid_t,
                                            const struct iovec*, unsigned long,
                                            const struct iovec*, unsigned long, unsigned long);
                                        ssize_t w = process_vm_writev(t, &l, 1, &rv, 1, 0);
                                        if (w != (ssize_t)ch) {
                                            ptrace_log("[mmap-fill] !! 灌入中断 done=%zu/%zu errno=%d（t=%d）",
                                                       done, n, errno, (int)t);
                                            break;
                                        }
                                        done += ch;
                                    }
                                    ptrace_log("[mmap-fill] ✓ fd=%ld at=0x%llx off=%llu n=%zu 灌入完成（t=%d）",
                                               g_fill[i].fd, at, g_fill[i].off, n, (int)t);
                                    /* v1.2.35：fill 段登记（at→路径）——体检符号化直查 */
                                    tvm_segnote_add(at, n, g_fill[i].fd);
                                } else {
                                    ptrace_log("[mmap-fill] !! fd=%ld 无 ROM 数据（fd 已关/合成？）",
                                               g_fill[i].fd);
                                }
                            } else {
                                ptrace_log("[mmap-fill] !! mmap 失败 x0=0x%llx",
                                           fr.regs[0]);
                            }
                        }
                    }
                    unsigned long long sv[6];
                    int pending = tvm_virt_take(t, (long)info.u.exit_stop.nr, sv);
                    if (pending >= 0) {
                        struct tvm_arm_regs rr;
                        long gr2 = tvm_getregs(t, &rr);
                        if (gr2 != 0) {
                            static int s_gr2 = 0;
                            if (s_gr2++ < 5)
                                ptrace_log("[ptrace] !! exit GETREGSET 失败 errno=%d —— 无法回填", errno);
                        }
                        if (gr2 == 0) {
                            VmCpu* cpu = vm_machine_current();
                            /* v1.0.3 ★ 用 **entry 保存的参数**（不再从 exit 停点读，
                             * 那里已是返回值/垃圾）；并校验 syscall 号一致。 */
                            long res = cpu ? vm_handle_syscall(cpu, (long)pending,
                                        (long)sv[0], (long)sv[1], (long)sv[2],
                                        (long)sv[3], (long)sv[4], (long)sv[5]) : -38;
                            /* v1.2.79 ★★★ mprotect 返回值直播（限 12 条）：
                             * v1.2.78 真机 nr-hist 99% nr=226 同点循环（0x78d172a000
                             * 页 3↔1 交替=liblog RELRO 收尾）——循环条件必依赖
                             * mprotect 返回值。这里把 (addr,len,prot)→res 全打出来。 */
                            if (pending == 226) {
                                static int s_mp = 0;
                                if (s_mp < 12) { s_mp++;
                                    ptrace_log("[mprotect] #%d (0x%llx,0x%llx,prot=%d) → x0=%ld",
                                               s_mp, sv[0], sv[1], (long)sv[2], res);
                                }
                            }
                            rr.regs[0] = (unsigned long long)res;
                            tvm_setregs(t, &rr);
                            /* v1.0.8：回填留痕（限流，每 200 次记一条），
                             * 用来确认"跳过 + 回填"这条链真的在跑。 */
                            static int s_fill = 0;
                            if ((++s_fill % 200) == 1)
                                ptrace_log("[ptrace] 回填 #%d：nr=%d → x0=%ld（tid=%d）",
                                       s_fill, pending, res, (int)t);
                        }
                    }
                }
                ptrace(PTRACE_SYSCALL, t, 0, 0);    /* syscall 停点：吞掉继续 */
            } else if (sig == SIGTRAP) {
                /* PTRACE_EVENT（clone 等）或真 SIGTRAP 投递：不注入信号继续 */
                ptrace(PTRACE_SYSCALL, t, 0, 0);
            } else if ((st >> 8) == (SIGTRAP | (PTRACE_EVENT_CLONE << 8))) {
                /* v1.0.5 ★★ PTRACE_EVENT_CLONE：clone 事件本身！
                 * 以前这个事件会落到最后的 else（"真信号转发"），把 PTRACE_EVENT_CLONE
                 * 当成信号注入 → 语义错乱；而**新线程**随后会自己停一次
                 * （SIGSTOP 或 SIGTRAP），我们没有正确接住它 → 新线程永不运行 →
                 * ART 主线程 futex 死等（真机 v1.0.3/v1.0.4 实锤：全程单线程、
                 * 无 clone 日志、53 万 syscall 后静止在 futex）。
                 * 处理：取出新 tid 并**显式续跑它**，然后继续父线程。 */
                unsigned long newtid = 0;
                ptrace(PTRACE_GETEVENTMSG, t, 0, &newtid);
                ptrace_log("[trace] ★ PTRACE_EVENT_CLONE：父 t=%d → 新线程 tid=%lu（显式续跑）",
                       (int)t, newtid);
                if (newtid > 0) {
                    /* 新线程此刻处于"初始停点"：直接 PTRACE_SYSCALL 让它跑起来 */
                    ptrace(PTRACE_SYSCALL, (pid_t)newtid, 0, 0);
                }
                ptrace(PTRACE_SYSCALL, t, 0, 0);
            } else if (sig == SIGSTOP && t != g_t_pid) {
                /* v1.0.4 ★ 新线程的初始停点！PTRACE_O_TRACECLONE 会让 clone 出来的
                 * 线程**先停一下**（内核发 SIGSTOP 给 tracer）。以前这个分支会走
                 * "真信号转发"（把 SIGSTOP 注入给新线程）—— 而新线程在 SIGSTOP 下
                 * 不会运行 → **ART 等不到它的唤醒** → 主线程 futex 死等
                 * （真机 v1.0.3 实锤：全程只有 t26046 一个线程、无任何 clone 后续）。
                 * 正确做法：**吞掉这个 SIGSTOP**（不注入），直接 PTRACE_SYSCALL 续跑。 */
                ptrace_log("[trace] 新线程 t=%d 初始停点：吞掉 SIGSTOP，续跑", (int)t);
                ptrace(PTRACE_SYSCALL, t, 0, 0);
            } else if (g_probe_stop && t == g_t_pid) {
                /* v1.2.18：这是我们自己 PTRACE_INTERRUPT 造成的组停 —— 吞掉 SIGSTOP。
                 * 若按"真信号转发"注入 SIGSTOP，guest 会被永久停住（SIGSTOP 不可捕获）。 */
                g_probe_stop = 0;
                ptrace(PTRACE_SYSCALL, t, 0, 0);
            } else {
                /* v1.1.5 ★★ 信号停点全记录；v1.1.6 ★★★ 致命信号加 FP 链回溯
                 * + /proc/<tid>/maps 落盘（离线符号化的完整弹药）。
                 * v1.1.5 真机实锤：SIGABRT pc/lr 落在 __dl_abort——abort() 自身，
                 * 真正要的是**谁调了 abort**：x29 帧指针链（[x29]=上一帧，[x29+8]=返回
                 * 地址）逐帧读出 16 帧返回地址，配合 maps 的模块基址即可全部符号化。
                 * 内存读取改用 process_vm_readv（与引擎同款已实证原语；PEEKDATA
                 * 在 v1.1.5 的 tee 里对 guest 栈区多次 EIO，原因待查，不再依赖）。 */
                {
                    siginfo_t si;
                    int have_si = ptrace(PTRACE_GETSIGINFO, t, 0, &si) == 0;
                    struct tvm_arm_regs rr;
                    int have_r = tvm_getregs(t, &rr) == 0;
                    if (have_r) {
                        if (have_si && (si.si_code == SI_USER || si.si_code <= 0)) {
                            ptrace_log("[sig] t=%d 收到信号 %d（code=%d 发送方 pid=%d）pc=0x%llx lr=0x%llx",
                                       (int)t, sig, si.si_code, (int)si.si_pid,
                                       rr.pc, rr.regs[30]);
                            /* v1.2.3 ★★★ Abort message 现形：si_code=-1（SI_QUEUE）
                             * 的 SIGABRT 走 rt_tgsigqueueinfo——si_value.sival_ptr 指向
                             * android_set_abort_message 存的消息串（libc abort+0x70 处
                             * 的 tgkill 就是发它）。v1.2.2 真机实锤：pc=abort+0xa0、
                             * 原因只进 logd 黑洞——读这个指针，abort 原文一击现形。 */
                            if (si.si_code == -1 /* SI_QUEUE */ && sig == SIGABRT) {
                                unsigned long long mp =
                                    (unsigned long long)(uintptr_t)si.si_value.sival_ptr;
                                if (mp >= 0x1000) {
                                    char mb[224];
                                    size_t ml = tvm_read_tracee(t, mp, 223, mb);
                                    if (ml > 0) {
                                        int mk = 0;
                                        mb[ml] = 0;
                                        for (size_t q = 0; q < ml && mb[q]; q++) {
                                            unsigned char cch = (unsigned char)mb[q];
                                            mb[mk++] = (cch >= 0x20 && cch < 0x7f) ? (char)cch : '.';
                                        }
                                        mb[mk] = 0;
                                        ptrace_log("[sig] ★★★ Abort message: %s", mb);
                                    } else {
                                        ptrace_log("[sig] （si_ptr=0x%llx 读失败）", mp);
                                    }
                                } else {
                                    ptrace_log("[sig] （Abort message=NULL——裸 abort 无消息，靠栈扫描回溯）");
                                }
                            }
                        } else if (have_si)
                            ptrace_log("[sig] t=%d 收到信号 %d（code=%d addr=0x%llx）pc=0x%llx lr=0x%llx sp=0x%llx",
                                       (int)t, sig, si.si_code,
                                       (unsigned long long)(uintptr_t)si.si_addr,
                                       rr.pc, rr.regs[30], rr.sp);
                        else
                            ptrace_log("[sig] t=%d 收到信号 %d pc=0x%llx lr=0x%llx",
                                       (int)t, sig, rr.pc, rr.regs[30]);
                        /* v1.1.6：致命信号 → FP 链回溯 + maps
                         * v1.2.4 ★★ 加栈扫描回溯：FP 链在 abort 场景是垃圾值
                         * （v1.2.3 真机实锤），但调用者的返回地址还在栈上——
                         * 用真实 maps 的 r-xp 段当筛子扫 [sp, sp+8KB)，
                         * 命中即打印 模块+偏移。si_value=NULL 的裸 abort
                         * （无 Abort message）从此也有调用现场。 */
                        if (sig == 4 || sig == 6 || sig == 7 || sig == 8 ||
                            sig == 11 || sig == 31) {
                            unsigned long long fp = rr.regs[29];
                            ptrace_log("[sig] ★ 致命信号 FP 链回溯（x29=0x%llx）：", fp);
                            for (int f = 0; f < 16 && fp >= 0x1000; f++) {
                                unsigned long long w[2];
                                if (tvm_read_pv(t, fp, 16, (char*)w) != 0) {
                                    ptrace_log("[sig]   #%-2d x29=0x%llx（读失败，链断）", f, fp);
                                    break;
                                }
                                ptrace_log("[sig]   #%-2d ret=0x%llx next_fp=0x%llx",
                                           f, w[1], w[0]);
                                if (w[0] <= fp) break;      /* 环/坏链保护 */
                                fp = w[0];
                            }
                            /* ── v1.2.5 栈扫描回溯（重写：v1.2.4 双 bug——
                             * ① acc[2049] 缓冲装不下 350KB 的 maps，首次 read
                             *   4096B 即被守卫拒收 → 缓冲恒空 → 静默跳过
                             *   （真机实锤：只打了 NULL 行没有扫描输出）；
                             * ② sscanf 字段错位：%llu 解析路径必失败。
                             * 改：fgets 流式逐行 + 正确字段序
                             *   lo-hi perms offset dev inode [path]，
                             * 段表 256，另附原始栈 hex dump 兜底（可离线符号化）。 ── */
                            {
                                static struct tvm_execseg2 {
                                    unsigned long long lo, hi, base;
                                    char name[64];
                                    int named;
                                } segs[256];
                                int nseg = 0;
                                char mpath[48];
                                snprintf(mpath, sizeof(mpath), "/proc/%d/maps", (int)t);
                                FILE* mfs = fopen(mpath, "r");
                                if (mfs) {
                                    char ln[256];
                                    while (fgets(ln, sizeof(ln), mfs) && nseg < 256) {
                                        unsigned long long lo, hi, foff;
                                        char perms[8] = {0}, nm[64] = {0};
                                        int n = sscanf(ln, "%llx-%llx %4s %llx %*s %*s %63[^\n]",
                                                       &lo, &hi, perms, &foff, nm);
                                        if (n < 4 || perms[2] != 'x') continue;
                                        if (nm[0]) {
                                            int found = -1;
                                            for (int q = 0; q < nseg; q++)
                                                if (segs[q].named && strcmp(segs[q].name, nm) == 0)
                                                    { found = q; break; }
                                            if (found < 0) {
                                                found = nseg++;
                                                segs[found].lo = lo; segs[found].hi = hi;
                                                segs[found].base = lo - foff;
                                                segs[found].named = 1;
                                                strcpy(segs[found].name, nm);
                                            } else {
                                                if (hi > segs[found].hi) segs[found].hi = hi;
                                                if (lo < segs[found].lo) segs[found].lo = lo;
                                            }
                                        } else {
                                            segs[nseg].lo = lo; segs[nseg].hi = hi;
                                            segs[nseg].base = lo; segs[nseg].named = 0;
                                            segs[nseg].name[0] = 0;
                                            nseg++;
                                        }
                                    }
                                    fclose(mfs);
                                } else {
                                    ptrace_log("[sig] （maps 打不开 errno=%d——栈扫描跳过）", errno);
                                }
                                if (nseg > 0) {
                                    ptrace_log("[sig] ★ 栈扫描回溯（sp=0x%llx，%d 个可执行段；sp+偏移 地址 模块+文件偏移）：",
                                               rr.sp, nseg);
                                    char sb[8192];
                                    if (tvm_read_pv(t, rr.sp, sizeof(sb), sb) == 0) {
                                        int hits = 0;
                                        for (size_t so = 0; so + 8 <= sizeof(sb) && hits < 24; so += 8) {
                                            unsigned long long v;
                                            memcpy(&v, sb + so, 8);
                                            for (int q = 0; q < nseg; q++) {
                                                if (v >= segs[q].lo && v < segs[q].hi) {
                                                    if (segs[q].named)
                                                        ptrace_log("[sig]   sp+0x%-4zx 0x%llx  %s +0x%llx",
                                                                   so, v, segs[q].name, v - segs[q].base);
                                                    else
                                                        ptrace_log("[sig]   sp+0x%-4zx 0x%llx  [anon 0x%llx-0x%llx]",
                                                                   so, v, segs[q].lo, segs[q].hi);
                                                    hits++;
                                                    break;
                                                }
                                            }
                                        }
                                        if (hits == 0)
                                            ptrace_log("[sig]   （栈上无可执行段地址——栈扫描空）");
                                    } else {
                                        ptrace_log("[sig]   （栈读取失败 errno=%d）", errno);
                                    }
                                } else {
                                    ptrace_log("[sig] （可执行段收集为空——nseg=0）");
                                }
                                /* 原始栈兜底 dump：首 768B（96 个 qword），
                                 * 配合日志尾部 maps 全文可完全离线符号化 */
                                {
                                    char sbd[768];
                                    if (tvm_read_pv(t, rr.sp, sizeof(sbd), sbd) == 0) {
                                        ptrace_log("[sig] ★ 原始栈 dump（sp+0x000..0x2ff，96 qword）：");
                                        for (int r = 0; r < 12; r++) {
                                            char hl[160]; int hk = 0;
                                            hk += snprintf(hl + hk, sizeof(hl) - (size_t)hk,
                                                           "[sig]   sp+0x%03x:", r * 64);
                                            for (int c = 0; c < 8; c++) {
                                                unsigned long long v;
                                                memcpy(&v, sbd + r * 64 + c * 8, 8);
                                                hk += snprintf(hl + hk, sizeof(hl) - (size_t)hk,
                                                               " %016llx", v);
                                            }
                                            ptrace_log("%s", hl);
                                        }
                                    }
                                }
                            }
                            static int s_maps_done = 0;
                            if (!s_maps_done) {
                                s_maps_done = 1;
                                char ppath[48];
                                snprintf(ppath, sizeof(ppath), "/proc/%d/maps", (int)t);
                                int mf = open(ppath, O_RDONLY);
                                if (mf >= 0) {
                                    live_write("[TwinVM] [maps] guest 布局落盘：\n", 30);
                                    char mb[2048];
                                    ssize_t mn;
                                    while ((mn = read(mf, mb, sizeof(mb))) > 0)
                                        live_write(mb, (size_t)mn);
                                    close(mf);
                                } else {
                                    ptrace_log("[sig] maps 读取失败 errno=%d", errno);
                                }
                            }
                        }
                    } else {
                        ptrace_log("[sig] t=%d 收到信号 %d（取寄存器失败 errno=%d）", (int)t, sig, errno);
                    }
                }
                ptrace(PTRACE_SYSCALL, t, 0, sig);  /* 真信号：必须转发（SIGSYS=引擎命脉） */
            }
            continue;
        }
        if (WIFEXITED(st) || WIFSIGNALED(st)) {
            if (t == g_t_pid) { g_t_status = st; g_t_done = 1; break; }
        }
    }
    /* v0.7.60：主 tracee 终局后，非阻塞排空残留在队列里的 clone 线程事件，
     * 防泄漏成僵尸；随后 waitpid 自然 ECHILD，线程干净退出。 */
    for (int i = 0; i < 4096; i++) {
        int ds;
        if (waitpid(-1, &ds, WNOHANG | __WALL) <= 0) break;
    }
    return NULL;
}

#pragma clang optimize on

/* v1.2.112 ★★★ 类名快照：决策挪到 stage0 入口（主进程 dispatch 线程）——
 * v1.2.111 实锤：决策在 fork 后子进程里读跨线程全局 g_main_class，读到旧值
 * （'SampleApp' 而非 com.twinvm.sample.Main）。Thread.start 的 happens-before
 * 保证 dispatch 线程看见 UI 线程的全部写入；子进程只读快照（fork 继承）——
 * 结构上无条件正确。 */
static void snapshot_main_class(void) {
    if (g_main_class[0]) {
        snprintf(g_cls_snapshot, sizeof(g_cls_snapshot), "%s", g_main_class);
    } else {
        const char* base = strrchr(g_dex_path, '/');
        base = base ? base + 1 : g_dex_path;
        snprintf(g_cls_snapshot, sizeof(g_cls_snapshot), "%s", base);
        char* dot = strchr(g_cls_snapshot, '.');
        if (dot) *dot = 0;
        if (g_cls_snapshot[0] >= 'a' && g_cls_snapshot[0] <= 'z') g_cls_snapshot[0] -= 32;
    }
    {
        char d[192];
        int dn = snprintf(d, sizeof(d),
            "[TwinVM] [cls] 快照：clsname='%s'（g_main_class='%s' dex='%s'）\n",
            g_cls_snapshot, g_main_class, g_dex_path);
        LOGI("%s", d);
        live_write(d, (size_t)dn);
    }
}

JNIEXPORT jstring JNICALL
Java_dev_twinbox_vm_Native_stage0(JNIEnv* env, jclass cls, jstring romPath, jstring liveLog) {
    const char* path;
    if (romPath) {
        path = (*env)->GetStringUTFChars(env, romPath, NULL);
    } else if (g_c_out_fd >= 0) {
        path = g_c_rom_path;            /* b-runner 纯 C 模式 */
    } else {
        path = "/nonexistent";
    }
    VmRom rom;
    int rc = vm_rom_load(&rom, path);
    if (romPath) (*env)->ReleaseStringUTFChars(env, romPath, path);
    if (rc != 0) {
        char b[160];
        snprintf(b, sizeof(b), "[TwinVM] ROM 装载失败: %d（assets 损坏？）", rc);
        return (*env)->NewStringUTF(env, b);
    }

    /* v0.7.48：live 日志提前打开（策略探针与后续控制台都往里写） */
    g_live_fd = -1;
    const char* lp_c = NULL;
    if (!liveLog && g_c_out_fd >= 0) lp_c = g_c_live_path[0] ? g_c_live_path : NULL;
    if (liveLog || lp_c) {
        const char* lp = lp_c ? lp_c : (*env)->GetStringUTFChars(env, liveLog, NULL);
        if (lp) {
            g_live_fd = open(lp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            /* v1.2.61：记住日志目录（扫雷指纹缓存放这里——App 必可写） */
            const char* slash = strrchr(lp, '/');
            if (slash && (size_t)(slash - lp) < sizeof(g_logs_dir))
                snprintf(g_logs_dir, sizeof(g_logs_dir), "%.*s", (int)(slash - lp), lp);
            if (!lp_c) (*env)->ReleaseStringUTFChars(env, liveLog, lp);
        }
    }

    /* v1.2.98b：容器身份横幅——日志第一行可辨（防"看起来都一样"）。
     * v1.2.105：live 判定修——b-runner（C 路径）下 liveLog jstring 为
     * NULL 但 g_c_live_path 有值（B 的 live=off 假情报曾误导排查）。 */
    {
        char banner[160];
        int bn = snprintf(banner, sizeof(banner),
            "[TwinVM] ══ 容器启动 ══ dex=%s ══ live=%s ══\n",
            g_dex_path,
            (liveLog || (g_c_out_fd >= 0 && g_c_live_path[0])) ? "on" : "off");
        if (bn > 0) live_write(banner, (size_t)bn);
    }
    /* v1.2.116：snapshot 挪到 banner 后——首版在 live fd 打开前调用，
     * [cls] 诊断行写进黑洞（19:42 日志无 [cls] 行实锤）。此时类名决策
     * 仍是 fork 前（下方 fork 晚于此处）——竞态修复语义不变。 */
    snapshot_main_class();
    /* v1.2.96 ★★★ Stage 5-M1：串行多容器复位。
     * 案 B 的第一步先走"串行复用"（第二次调用 stage0 = 第二个容器）：
     * 每次进入先复位运行期全局态，保证容器间零残留。
     * （并发多 slot 的 pthread/进程级隔离是 M1 第二步——VmSlot 化。） */
    {
        g_guest_ticks = 0; g_ticks_at_real_out = 0;
        g_last_real_out = 0; g_nr_idx = 0; g_futex_idx = 0;
        g_nfail = 0; g_t_events = 0; g_t_last_nr = -1;
        g_mmap_pend = 0; g_mmap_hint = 0; g_mmap_len = 0;
        g_low4_active = 0; g_low4_rewound = 0; g_low4_want_len = 0;
        memset(g_virt, 0, sizeof(g_virt));
        memset((void*)g_low4_used, 0, sizeof(g_low4_used));
        g_want_stack = 0; g_stack_done = 0; g_hang_probe = 0;
        g_t_done = 0; g_t_status = 0; g_t_pid = 0; g_virt_seq = 0;
        memset((void*)g_segnote, 0, sizeof(g_segnote));
        for (int zi = 0; zi < TVM_NR_RING; zi++) g_nr_ring[zi] = 0;
    }
    /* v0.7.71：策略探针 = 内嵌在 .so 里的独立可执行（无 libc，3248B）。
     * 落盘到 /data/local/tmp 再 fork+execv —— 与 App 无亲缘依赖，
     * 被 SECCOMP_RET_KILL_PROCESS 杀掉也不会连坐。
     * ★ 必须在 live 日志打开**之后**（v0.7.70 第一次就是放太早，结果没地方写而哑掉）。 */
    /* v1.2.59：外部探针并入指纹缓存门（与扫雷共用一个指纹） */
    /* v1.2.59 ★★★ 自检指纹缓存：真机截图实锤——每次启动卡在"扫雷"
     * （逐号 fork 探测 550 次，阻塞型 syscall 各吃满 1.2s 保底 → 十几秒）。
     * App 策略在安装期内不变（地雷图 12 条 KILL 名单 v0.7.52 已定案），
     * 同一设备重复扫雷是纯浪费。指纹 = 版本号+机型；命中缓存 → 只打
     * 结论摘要（1 行），跳过 fork；缓存缺失/版本变化 → 照跑全量（首启
     * 或升级后重扫一次）。清 App 数据或换机自动失效。 */
    {
        /* v1.2.61 ★ 缓存路径修复：/data/local/tmp 现代 Android 不许 App 写
         * （缓存永远写不进→每启都全量扫雷→"卡一下"没修掉）。改放 live 日志
         * 同目录（Documents/TwinVM，App 必可写）。版本号入文件名，升级自动重扫。 */
        char pcache[512];
        snprintf(pcache, sizeof(pcache), "%s/policy_ok_v211", g_logs_dir[0] ? g_logs_dir : ".");
        struct stat cst;
        if (stat(pcache, &cst) == 0 && cst.st_size == 4) {
            const char* c1 = "[TwinVM] [policy] 扫雷指纹命中（同版本已验证）——"
                             "跳过逐条探测（地雷图见历史日志；删缓存可重扫）\n";
            LOGI("%s", c1); live_write(c1, strlen(c1));
        } else {
            {   /* 外部探针（v0.7.71 原路径，一并受缓存门控） */
                char pprobe[512];
                snprintf(pprobe, sizeof(pprobe), "/data/local/tmp/tvm_probe_%d", (int)getpid());
                int pfd2 = open(pprobe, O_WRONLY | O_CREAT | O_TRUNC, 0700);
                if (pfd2 >= 0) {
                    ssize_t wr = write(pfd2, kProbePolicyBlob, kProbePolicyBlobLen);
                    close(pfd2);
                    chmod(pprobe, 0700);
                    if (wr == (ssize_t)kProbePolicyBlobLen) {
                        vm_policy_probe_external(pprobe);
                        unlink(pprobe);
                    }
                }
            }
            vm_policy_probe();   /* v0.7.48：先量一下 App 策略 KILL 名单 */
            vm_policy_sweep();   /* v0.7.58：全量扫雷（0..500），画出完整地雷图 */
            int cf = open(pcache, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (cf >= 0) { if (write(cf, "OKv1", 4) != 4) {} close(cf); }
        }
    }
    int pfd[2];
    if (pipe(pfd) != 0) return (*env)->NewStringUTF(env, "[TwinVM] pipe 失败");
    /* v0.7.50：共享黑匣子页（内核 KILL 也带得走子进程最后的 syscall） */
    VmBlackBox* bb = (VmBlackBox*)mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                                       MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (bb == MAP_FAILED) bb = NULL;
    /* v0.7.60：**fork 权移交示踪线程**（ptrace 停点只能由追踪者任务本人 wait，
     * v0.7.59 真机实锤：主线程 fork+吞握手停点 → 示踪线程 waitpid(-1) 立刻
     * ECHILD 返回 → 子进程冻死在第一个 syscall 入口停点，无人恢复）。
     * fork/握手/等待现在全部在示踪线程内闭环。SIGCHLD 仍先屏蔽防 EINTR，
     * 子进程内恢复空掩码让 guest 看到正常世界。 */
    sigset_t chld_m, old_m;
    sigemptyset(&chld_m); sigaddset(&chld_m, SIGCHLD);
    sigaddset(&chld_m, SIGALRM);      /* v1.2.63：ALRM 定向给 tracer（主线程屏蔽） */
    pthread_sigmask(SIG_BLOCK, &chld_m, &old_m);
    g_launch.rom = &rom; g_launch.bb = bb;
    g_launch.rfd = pfd[0]; g_launch.wfd = pfd[1];
    g_t_done = 0; g_t_status = 0; g_t_events = 0;
    g_t_last_nr = -1; g_t_last_tid = 0; g_t_ring_idx = 0; g_t_ring_cnt = 0;
    {   /* 信号量只初始化一次（重复 stage0 调用安全） */
        static int sem_once = 0;
        if (!sem_once) { sem_init(&g_launch_sem, 0, 0); sem_once = 1; }
    }
    if (pthread_create(&g_t_thr, NULL, tvm_tracer_main, &g_launch) != 0) {
        close(pfd[0]); close(pfd[1]);
        if (g_live_fd >= 0) { close(g_live_fd); g_live_fd = -1; }
        return (*env)->NewStringUTF(env, "[TwinVM] 示踪线程创建失败");
    }
    /* v1.2.63：SIGALRM 空处理器——setitimer 200ms 打断阻塞 waitpid 用
     * （默认动作=杀进程，绝不能裸用）。一次性安装。 */
    {
        static int alrm_once = 0;
        if (!alrm_once) {
            alrm_once = 1;
            struct sigaction sa;
            memset(&sa, 0, sizeof(sa));
            sa.sa_handler = tvm_alrm_noop; /* 空函数（不能用 SIG_IGN+SA_RESTART：waitpid 会自动重启拿不到 EINTR） */
            sa.sa_flags = 0;                 /* 无 SA_RESTART：waitpid 返回 EINTR */
            sigaction(SIGALRM, &sa, NULL);
        }
    }
    sem_wait(&g_launch_sem);               /* fork 完成，g_t_pid 就绪 */
    /* v1.2.72 ★★★ 终极真凶修复：信号投递目标=「任一未屏蔽线程」。
     * 旧代码恢复 old_m（fork 前干净掩码）→ 主线程也 unblocked ALRM →
     * **主线程抢收所有 SIGALRM**（空 handler 吃掉，waitpid 不被打断）
     * → tracer 的 EINTR 永不命中（v1.2.66~71 五版真机实锤 + setitimer
     * 「可用」却零 EINTR 的矛盾全解）。主线程必须保持 ALRM 屏蔽，
     * 只留 tracer 一个投递目标。 */
    {
        sigset_t rm = old_m;
        sigaddset(&rm, SIGALRM);            /* 恢复时强制带上 ALRM 屏蔽 */
        pthread_sigmask(SIG_SETMASK, &rm, NULL);
    }
    pid_t pid = g_t_pid;

    size_t cap = 8192, len = 0, live_total = 0;
    char* buf = (char*)malloc(cap);
    if (!buf) {
        kill(pid, SIGKILL);             /* 示踪器负责收尸 */
        pthread_join(g_t_thr, NULL);
        close(pfd[0]);
        if (g_live_fd >= 0) close(g_live_fd);
        return (*env)->NewStringUTF(env, "[TwinVM] 内存不足");
    }
    ssize_t n;
    int hang = 0;
    time_t t0 = time(NULL), last_out = t0;
    unsigned long long last_ticks = g_guest_ticks;
    time_t last_beat = t0;
    /* v1.2.21 ★★★ 循环检测（**只报告，不杀**）。
     * 真机 v1.2.20 实锤：guest 连跑 347s+、停点从 85K 涨到 141K，
     * 但 `nr` 只在 222(mmap)/227(msync)/215(munmap) 之间转、pc 只有 4 个固定值
     * —— 典型的"**活着但原地打转**"。看门狗按"无活动"判据永远不会响，
     * 于是我们既没杀它、也拿不到栈（体检只在判卡死时才做）。
     * 这里补一条独立通道：若某个 20 秒窗口内出现的不同 pc 极少（≤4）而停点很多，
     * 就判定"疑似同点循环"，打印报告并做一次 procfs 体检（含调用链）。
     * 注意：**不杀进程** —— 它可能只是慢，报告能让我们看清是谁在打转。 */
    time_t loop_t0 = t0, loop_report_at = 0;
    unsigned long long loop_ticks0 = g_guest_ticks;
    unsigned long long loop_pcs[8];
    int loop_npc = 0;
    unsigned long long loop_sampled = g_guest_ticks;
    for (;;) {
        struct pollfd pf;
        pf.fd = pfd[0]; pf.events = POLLIN; pf.revents = 0;
        int pr = poll(&pf, 1, 1000);
        if (pr > 0 && (pf.revents & POLLIN)) {
            /* v1.2.73 ★★★ 主线程"38s 死亡"之谜终章：read 想一次读满 8KB，
             * 但管道里只有 guest 写的 24B（stress 输出）→ **read 阻塞等满**
             * → poll 循环卡死在 read 里 → alive 心跳/EOF 收尾全停。
             * （经典 poll+read 死锁：poll 只保证"有数据"，不保证"够读满"。）
             * 修：小份读（256B）——多读几次无妨，绝不阻塞。 */
            size_t want = 256;
            if (want > cap - len) want = cap - len;
            n = read(pfd[0], buf + len, want);
            if (n > 0) {
                len += (size_t)n;
                last_out = time(NULL);
                { time_t rt = time(NULL); g_last_real_out = rt; g_ticks_at_real_out = g_guest_ticks; }
                if (live_total < VM_LIVE_MAX) {
                    size_t w = (size_t)n;
                    if (live_total + w > VM_LIVE_MAX) w = VM_LIVE_MAX - live_total;
                    live_write(buf + len - (size_t)n, w);
                    live_total += w;
                }
                if (len >= VM_CONSOLE_MAX) {    /* 日志暴走 → 判定死循环，停 */
                    const char* note =
                        "\n[TwinVM] !! 控制台输出超过 6MB —— 判定为日志暴走（极可能是某个 syscall\n"
                        "[TwinVM]    拿到错误后无限重试）。已强杀槽位；上面最后几行就是那个 syscall。\n";
                    size_t nl = strlen(note);
                    live_write(note, nl);
                    if (len + nl < cap) { memcpy(buf + len, note, nl); len += nl; }
                    kill(pid, SIGKILL);
                    hang = 1;
                    break;
                }
                if (len == cap) {
                    size_t ncap = cap * 2;
                    char* nb = (char*)realloc(buf, ncap);
                    if (!nb) break;             /* 内存紧张：停止收集（下方仍会给出退出码） */
                    buf = nb; cap = ncap;
                }
                continue;
            }
            if (n == 0) break;                  /* EOF：槽位退出 */
            if (errno == EINTR) continue;
            break;
        }
        /* v0.7.48：管道挂断 = 槽位已死（SIGSYS 若被内核直接杀，连 handler 都不会跑，
         * 控制台不会有任何收尾输出）。以前不处理这里，于是白等 30 秒看门狗。 */
        if (pr > 0 && (pf.revents & (POLLHUP | POLLERR | POLLNVAL))) break;
        if (pr < 0 && errno != EINTR) break;
        time_t now = time(NULL);
        /* v1.2.57 ★★★ 优雅关机补刀 v2（v1.2.56 判据失效修法）。
         * v1.2.56 真机实锤：判据 ②③ 永远不同时满足——v1.2.19 的"活动即前进"
         * 只要 guest 有**任何**停点就重置 last_out，而 ART 收尾的 Shutdown
         * 线程在 set_tid_address/读 /proc 的循环里打转，停点不断 → 静默计时
         * 永远清零 → graceful 永不触发（真机日志：stress 后停点又跑了 51 次，
         * [graceful] 一条没出）。
         * v2 判据（独立时钟 + 低频密度）：
         *   ① len > 0（控制台出过东西）；
         *   ② last_real_out（**仅真实控制台输出**重置的独立时钟）≥ GRACE_S；
         *   ③ 停点频率骤降：GRACE_S 窗口内的停点增量 / 运行期峰值速率
         *      < 5%——收尾打转是零星读 proc，正常干活是每秒数千停点。 */
        {
            static time_t grace_killed = 0;
            static unsigned long long peak_rate = 0;      /* 峰值 每秒停点数 */
            /* v1.2.59 ★ 修 v1.2.58 的峰值退化：旧条件"差 ≥1"在 poll 每轮
             * （~1ms）都为真 → prev_sec 每轮刷新 → 增量永远 1~2 →
             * peak_rate ≈ 2 → 判据 ③ (N*20 < 2*8=16) 永假。
             * 正解：按**真实墙钟秒**采样——秒变才结算。 */
            {
                static time_t rate_sec = 0;
                if (rate_sec == 0) rate_sec = now;
                if (now != rate_sec) {
                    unsigned long long delta = g_guest_ticks - g_guest_ticks_prev_sec;
                    if (delta > peak_rate) peak_rate = delta;
                    g_guest_ticks_prev_sec = g_guest_ticks;
                    rate_sec = now;
                    g_sec_now++;
                }
            }
            unsigned long long peak_eff = peak_rate > 1000 ? peak_rate : 1000;  /* 下限保护 */
            {
                static time_t watch_last = 0;
                if (g_last_real_out > 0 && now - g_last_real_out >= 2
                    && now - watch_last >= 2) {
                    watch_last = now;
                    char wb[256];
                    int wk = snprintf(wb, sizeof(wb),
                        "[TwinVM] [graceful-watch] 距真实输出 %lds、窗口停点 %llu、"
                        "峰值 %llu/s（分级判据：≥%d停点/s不杀；静默≥%ds且<%d停点=死锁；静默≥%ds兜底）\n",
                        (long)(now - g_last_real_out),
                        g_guest_ticks - g_ticks_at_real_out, peak_eff,
                        VM_EXIT_MIN_ALIVE / VM_EXIT_GRACE_S,
                        VM_EXIT_GRACE_S, VM_EXIT_MIN_ALIVE, VM_EXIT_GRACE2_S);
                    live_write(wb, (size_t)wk);
                }
            }

            /* v1.2.61 ★★★ 真根因：v1.2.56~60 五版全部失效的总闸——`len > 0`
             * 检查的是 console 管道，而 v1.1.6 已把 guest 输出改道直写 live
             * 文件（管道在 ART 时代恒干涸，len 恒 0）。本地模拟实锤：去掉
             * 此门后收尾打转场景 t=13s 触发、真干活 5000/s 场景不误杀。
             * g_last_real_out > 0 已蕴含"出过真实输出"。 */
            /* v1.2.78 ★★★ 主线程侧同步分级（与 tracer 侧 tvm_graceful_check
             * 同判据：≥8 停点/s 的健康慢速不杀；<8/s 且静默 8s=死锁快收；
             * 静默 90s 兜底。v1.2.77 真机误杀教训：峰值下限 1000 会让 47/s
             * 的 ART 信号准备（4.7%）踩线被杀。） */
            long m_sil = (long)(now - g_last_real_out);
            unsigned long long m_n = g_guest_ticks - g_ticks_at_real_out;
            int m_dead = (m_sil >= VM_EXIT_GRACE2_S)
                       || (m_sil >= VM_EXIT_GRACE_S && m_n < VM_EXIT_MIN_ALIVE);
            if (!grace_killed && g_last_real_out > 0 && m_dead) {
                grace_killed = now;
                int gst = 0; (void)gst;
                char gb[384];
                int gk = snprintf(gb, sizeof(gb),
                    "[TwinVM] [graceful] 真实输出静默 %lds、停点频率 %llu/%llu 峰值——"
                    "判定 main 已返回、收尾死等 → 优雅关机：SIGTERM→2s→SIGKILL\n",
                    (long)(now - g_last_real_out),
                    g_guest_ticks - g_ticks_at_real_out, peak_eff);
                live_write(gb, (size_t)gk);
                if (len + (size_t)gk < cap) { memcpy(buf + len, gb, (size_t)gk); len += (size_t)gk; }
                kill(pid, SIGTERM);
                for (int gw = 0; gw < 20; gw++) {         /* 2s 礼让窗口 */
                    if (waitpid(pid, &gst, WNOHANG) == pid) break;
                    usleep(100 * 1000);
                }
                if (waitpid(pid, &gst, WNOHANG) != pid)
                    kill(pid, SIGKILL);                   /* 兵 */
                break;
            }
        }
        /* v0.7.46 黑匣子：卡死期间每 2 秒抄子进程的 /proc/<pid>/syscall、wchan、status（SigPnd/SigBlk）。
         * SIGSYS 死亡发生在 512 条 TRAP 之后时，日志里什么都看不到，就靠它破。 */
        {
            static time_t last_probe = 0;
            if (now - last_probe >= 2) {
                last_probe = now;
                char pp[64], pbuf[512];
                snprintf(pp, sizeof(pp), "/proc/%d/syscall", (int)pid);
                int fd = open(pp, O_RDONLY);
                if (fd >= 0) {
                    ssize_t k = read(fd, pbuf, sizeof(pbuf) - 1);
                    close(fd);
                    if (k > 0) {
                        pbuf[k] = 0;
                        for (int q = 0; q < k; q++) if (pbuf[q] == '\n') pbuf[q] = ' ';
                        char out[640];
                        int n2 = snprintf(out, sizeof(out),
                            "[TwinVM] [blackbox] silent %ds; child syscall: %s\n",
                            (int)(now - last_out), pbuf);
                        live_write(out, (size_t)n2);
                        if (len + (size_t)n2 < cap) { memcpy(buf + len, out, (size_t)n2); len += (size_t)n2; }
                    }
                }
                snprintf(pp, sizeof(pp), "/proc/%d/wchan", (int)pid);
                fd = open(pp, O_RDONLY);
                if (fd >= 0) {
                    ssize_t k = read(fd, pbuf, sizeof(pbuf) - 1);
                    close(fd);
                    if (k > 0) {
                        pbuf[k] = 0;
                        char out[640];
                        int n2 = snprintf(out, sizeof(out),
                            "[TwinVM] [blackbox] child wchan: %s\n", pbuf);
                        live_write(out, (size_t)n2);
                        if (len + (size_t)n2 < cap) { memcpy(buf + len, out, (size_t)n2); len += (size_t)n2; }
                    }
                }
                snprintf(pp, sizeof(pp), "/proc/%d/status", (int)pid);
                fd = open(pp, O_RDONLY);
                if (fd >= 0) {
                    ssize_t k = read(fd, pbuf, sizeof(pbuf) - 1);
                    close(fd);
                    if (k > 0) {
                        pbuf[k] = 0;
                        const char* keep[4] = { "State:", "SigPnd:", "SigBlk:", "SigCgt:" };
                        for (int q = 0; q < 4; q++) {
                            char* hit = strstr(pbuf, keep[q]);
                            if (!hit) continue;
                            char* e = strchr(hit, '\n');
                            char tmp[160];
                            size_t tl = e ? (size_t)(e - hit) : strlen(hit);
                            if (tl > sizeof(tmp) - 1) tl = sizeof(tmp) - 1;
                            memcpy(tmp, hit, tl); tmp[tl] = 0;
                            char out[320];
                            int n2 = snprintf(out, sizeof(out), "[TwinVM] [blackbox] child %s\n", tmp);
                            live_write(out, (size_t)n2);
                            if (len + (size_t)n2 < cap) { memcpy(buf + len, out, (size_t)n2); len += (size_t)n2; }
                        }
                    }
                }
            }
        }
        /* ★ v1.2.19：**活动即前进** —— guest 每产生一个 ptrace 停点就重置静默计时。
         * 这样"长时间不打印但一直在跑"（如 msync 遍历）不再被误判为卡死。 */
        if (g_guest_ticks != last_ticks) {
            last_ticks = g_guest_ticks;
            last_out = now;
        }
        /* 进度心跳：有活动但无输出时，每 10 秒写一条，让日志能看见"它在动" */
        if (now - last_beat >= 10 && now - last_out < VM_HANG_SILENCE_S) {
            char beat[256];
            int bk = snprintf(beat, sizeof(beat),
                "[TwinVM] [alive] 已运行 %lds：guest 活动 %llu 次停点（最近 nr=%d pc=0x%llx）"
                "——无控制台输出属正常（未拦截的 syscall 不打印）\n",
                (long)(now - t0), g_guest_ticks, g_t_last_nr, g_t_last_pc);
            last_beat = now;
            live_write(beat, (size_t)bk);
            if (len + (size_t)bk < cap) { memcpy(buf + len, beat, (size_t)bk); len += (size_t)bk; }
        }
        /* v1.2.21：每 1 秒采样一次"最近 pc"，累积到本窗口的 pc 集合 */
        if (g_guest_ticks != loop_sampled) {
            loop_sampled = g_guest_ticks;
            unsigned long long p = g_t_last_pc;
            int found = 0;
            for (int q = 0; q < loop_npc; q++) if (loop_pcs[q] == p) { found = 1; break; }
            if (!found && loop_npc < 8) loop_pcs[loop_npc++] = p;
        }
        /* 结算 20 秒窗口 */
        if (now - loop_t0 >= 20) {
            unsigned long long dt = g_guest_ticks - loop_ticks0;
            if (loop_npc > 0 && loop_npc <= 4 && dt > 1000) {
                char lb[288];
                int lk = snprintf(lb, sizeof(lb),
                    "[TwinVM] [loop] 过去 20s 疑似**同点循环**：仅 %d 个不同 pc、"
                    "%llu 次停点（最近 nr=%d pc=0x%llx）——报告用，不杀进程\n",
                    loop_npc, dt, g_t_last_nr, g_t_last_pc);
                live_write(lb, (size_t)lk);
                if (len + (size_t)lk < cap) { memcpy(buf + len, lb, (size_t)lk); len += (size_t)lk; }
                /* 每 60 秒附带一次现场体检（含 sp/pc 与调用链） */
                if (now - loop_report_at >= 60) {
                    loop_report_at = now;
                    g_want_stack = 1; g_stack_done = 0;   /* v1.2.25：请示踪器抓帧 */
                    const char* lh = "[TwinVM] [loop] 附现场体检（procfs，不需停点）：\n";
                    live_write(lh, strlen(lh));
                    tvm_hang_dump_procfs(pid);
                }
            }
            loop_t0 = now; loop_npc = 0; loop_ticks0 = g_guest_ticks;
        }
        /* 判据：**只有"既没输出、也没有任何 guest 活动"才是真卡死** */
        if (now - last_out >= VM_HANG_SILENCE_S) {
            char hb[320];
            int hk = snprintf(hb, sizeof(hb),
                "\n[TwinVM] !! 看门狗：%ld 秒无任何控制台输出（累计 %ld 秒）——判定卡死。\n"
                "[TwinVM] ★ v1.2.17：请求示踪线程做等待现场体检（hang 与 crash 同等待遇）……\n",
                (long)(now - last_out), (long)(now - t0));
            live_write(hb, (size_t)hk);
            if (len + (size_t)hk < cap) { memcpy(buf + len, hb, (size_t)hk); len += (size_t)hk; }
            /* v1.2.17 ★ 修复：v1.2.16 的体检跑在**主线程**（pipe poll 循环），
             * 而 ptrace 只认**示踪线程**（fork 它的那个）——GETREGSET 从主线程
             * 发出＝ESRCH 静默失败，连嵌套的 maps 落盘都被跳过（真机 v1.2.16
             * 实锤：只有 header/footer，无 [hang] 行）。教训五第五击。
             * 正解：置 g_hang_probe=1，示踪线程在 waitpid 循环的下一轮
             * （tracee 睡在 futex，PTRACE_SYSCALL 会立即返回停点）代办体检。 */
            /* v1.2.20：**先做主线程可直接完成的 procfs 体检**（不依赖 ptrace、
             * 也不需要 tracer 线程）—— 这是本类问题最可靠的一条取数通道。 */
            tvm_hang_dump_procfs(pid);
            g_want_stack = 1; g_stack_done = 0;      /* v1.2.25：同时请示踪器抓帧 */
            g_hang_probe = 1;
            for (int wait_h = 0; wait_h < 30 && g_hang_probe; wait_h++)
                usleep(100 * 1000);        /* 等 tracer 完成体检（最多 3 秒） */
            const char* w2 = "[TwinVM] !! 体检完毕（见上方 [hang] 行），强杀槽位。\n";
            live_write(w2, strlen(w2));
            kill(pid, SIGKILL);
            hang = 1;
            break;
        }
    }
    close(pfd[0]);

    /* v0.7.58：示踪器收尾 —— 收尸 + 死前最后一个 syscall（凶手）*/
    if (!g_t_done) kill(pid, SIGKILL);   /* 管道 EOF 但槽位没死：病理状态，补刀防 join 卡死 */
    pthread_join(g_t_thr, NULL);
    int st = g_t_status;
    int sig = WIFSIGNALED(st) ? WTERMSIG(st) : 0;
    int code = WIFEXITED(st) ? WEXITSTATUS(st) : -sig;
    LOGI("stage0 guest slot exit=%d (signal=%d) hang=%d", code, sig, hang);

    /* v0.7.50：把共享黑匣子里的最后 32 次 syscall 倒出来（这是 KILL 也抹不掉的现场） */
    if (bb) {
        char bbl[2048];
        int off = snprintf(bbl, sizeof(bbl),
            "\n[TwinVM] [bb] \u5b50\u8fdb\u7a0b\u6700\u540e\u7684 syscall\uff08seq=%lu\uff0c\u6700\u65b0\u5728\u540e\uff09:\n", bb->seq);
        int cnt = (int)(bb->seq < 32 ? bb->seq : 32);
        int start = (bb->idx - cnt + 32) & 31;
        for (int i = 0; i < cnt && off < (int)sizeof(bbl) - 64; i++) {
            int k = (start + i) & 31;
            off += snprintf(bbl + off, sizeof(bbl) - (size_t)off,
                            "[TwinVM] [bb]   nr=%-4ld a0=0x%-12lx pc=0x%lx\n",
                            bb->nr[k], bb->a0[k], bb->pc[k]);
        }
        live_write(bbl, (size_t)off);
        if (len + (size_t)off < cap) { memcpy(buf + len, bbl, (size_t)off); len += (size_t)off; }
        munmap(bb, 4096);
    }

    /* v0.7.58：示踪器尾环 —— 死前最后 40 个 syscall entry（含参数）。
     * 死于信号 31 时，这里的**最后一个 entry** 就是凶手 syscall（ptrace 停点
     * 在 seccomp 评估之前，内核级 KILL 也拦不住它被看见）。 */
    {
        char tl[4608];
        int cnt = g_t_ring_cnt < 40 ? g_t_ring_cnt : 40;
        int off = snprintf(tl, sizeof(tl),
            "\n[TwinVM] [trace] 示踪器：共 %d 个 syscall 事件；死前最后 %d 个 entry（最新在后，"
            "末尾即凶手）:\n", g_t_events, cnt);
        int start = (g_t_ring_idx - cnt + TVM_RING) % TVM_RING;
        for (int i = 0; i < cnt && off < (int)sizeof(tl) - 100; i++) {
            int k = (start + i) % TVM_RING;
            int w = (int)strlen(g_t_ring[k]);
            if (w > 0 && off + w < (int)sizeof(tl) - 8) {
                memcpy(tl + off, g_t_ring[k], (size_t)w);
                off += w;
            }
        }
        if (g_t_last_nr >= 0)
            off += snprintf(tl + off, sizeof(tl) - (size_t)off,
                            "[TwinVM] [trace] ★ 死前最后 syscall: nr=%d (t%d)\n",
                            g_t_last_nr, (int)g_t_last_tid);
        live_write(tl, (size_t)off);
        if (len + (size_t)off < cap) { memcpy(buf + len, tl, (size_t)off); len += (size_t)off; }
    }
    if (g_live_fd >= 0) { close(g_live_fd); g_live_fd = -1; }

    char tail[512];
    if (hang)
        snprintf(tail, sizeof(tail),
                 "\n[TwinVM] guest 槽位退出: code=%d（被看门狗强杀 = 卡死，不是崩溃）\n"
                 "[TwinVM] 完整实时日志也已写盘（twinvm-live.log），上面最后几行即现场。\n", code);
    else if (sig)
        snprintf(tail, sizeof(tail),
                 "\n[TwinVM] guest 槽位退出: code=%d（死于信号 %d = %s）\n"
                 "[TwinVM] 提示: SIGSYS(31) = 系统调用被过滤器/策略拒绝（判决表/放行清单）；"
                 "SIGSEGV(11)/SIGILL(4) = 上面应有 GUEST FAULT 现场\n",
                 code, sig, jni_signal_name(sig));
    else
        snprintf(tail, sizeof(tail),
                 "\n[TwinVM] guest 槽位退出: code=%d %s\n",
                 code, code == 0 ? "（正常关机）" : "（异常）");

    char* full = (char*)malloc(len + strlen(tail) + 1);
    if (!full) { free(buf); return (*env)->NewStringUTF(env, "[TwinVM] 内存不足"); }
    memcpy(full, buf, len);
    memcpy(full + len, tail, strlen(tail) + 1);
    free(buf);
    jstring result = console_to_jstring(env, full, len + strlen(tail));   /* NUL 安全转换 */
    free(full);
    return result;
}

/* v1.2.101：b-runner 结果读取线程（宿主侧，后台收结果存文件） */
static void* tvm_brunner_reader(void* a) {
    struct { int fd; } *arg = a;
    int fd = arg->fd;
    free(arg);
    char buf[8192];
    size_t cap = 0, len = 0;
    char* acc = NULL;
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        if (len + (size_t)n > cap) {
            cap = (len + (size_t)n) * 2;
            char* nb = realloc(acc, cap);
            if (!nb) { free(acc); close(fd); return NULL; }
            acc = nb;
        }
        memcpy(acc + len, buf, (size_t)n);
        len += (size_t)n;
    }
    close(fd);
    if (len) {
        int f = open("/sdcard/Documents/TwinVM/twinvm-b-result.log",
                    O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (f >= 0) { ssize_t w = write(f, acc, len); (void)w; close(f); }
    }
    free(acc);
    return NULL;
}

/* v1.2.101 ★★★ M1 并发：并行容器入口。
 * stage0 全局态密集（g_live_fd/g_t_pid/g_launch/g_cpu…）不可重入——
 * 串行复用靠复位（v1.2.96），并发必须进程级隔离。
 * 方案：fork 一个 b-runner 进程，在其内调用现有 stage0（fork 快照=
 * 全套全局态的独立副本）；结果经管道回传，宿主立刻返回不阻塞。
 * live 日志由 b-runner 自己写（live-b.log）——与宿主互不干扰。 */
JNIEXPORT jstring JNICALL
Java_dev_twinbox_vm_Native_stage0Parallel(JNIEnv* env, jclass cls,
                                          jstring romPath, jstring liveLog) {
    /* 先记录参数（fork 前转 C 串） */
    const char* rom = (*env)->GetStringUTFChars(env, romPath, NULL);
    const char* live = liveLog ? (*env)->GetStringUTFChars(env, liveLog, NULL) : NULL;
    if (!rom) { if (live) (*env)->ReleaseStringUTFChars(env, liveLog, live);
                return (*env)->NewStringUTF(env, "[TwinVM] romPath 转换失败"); }

    int fds[2];
    if (pipe(fds) != 0) {
        (*env)->ReleaseStringUTFChars(env, romPath, rom);
        if (live) (*env)->ReleaseStringUTFChars(env, liveLog, live);
        return (*env)->NewStringUTF(env, "[TwinVM] pipe 失败");
    }

    /* v1.2.103 ★★★ 闪退真凶：fork 后子进程里调 NewStringUTF/stage0——
     * fork 只继承调用线程，ART 运行时的 GC 锁/JNI 表处于任意中间态，
     * 子进程一碰 Java 堆就崩（v1.2.102 真机闪退实锤；v1.2.101 的 13s
     * 只是运气）。正解：**fork 后只用 POSIX**——stage0 的核心逻辑在
     * tvm_run_rom（纯 C），子进程直接调它；返回串经管道回传。
     * JNI 环境（env）绝不在子进程使用。 */
    pid_t b = fork();
    if (b == 0) {
        close(fds[0]);
        /* v1.2.103 b-runner：纯 C 世界——fork 后不碰任何 JNI/Java 堆。
         * g_c_out_fd >= 0 时 stage0 的所有输出走 write(fd)，env 全旁路；
         * 入参走 g_c_rom_path/g_c_live_path（fork 前备好）。 */
        g_c_out_fd = fds[1];
        snprintf(g_c_rom_path, sizeof(g_c_rom_path), "%s", rom);
        snprintf(g_c_live_path, sizeof(g_c_live_path), "%s", live ? live : "");
        /* stage0 内部唯一的 JNI 危险点已全部双模式化；这里 env/cls 传原值
         * 但【不会】被触碰（g_c_out_fd 生效时所有 env-> 调用都被跳过）。 */
        jstring r = Java_dev_twinbox_vm_Native_stage0(NULL, NULL, NULL, NULL);
        (void)r;
        _exit(0);
    }
    close(fds[1]);
    (*env)->ReleaseStringUTFChars(env, romPath, rom);
    if (live) (*env)->ReleaseStringUTFChars(env, liveLog, live);

    /* 宿主侧：读结果用后台线程（不卡 UI），此处先返回启动确认 */
    int rdfd = fds[0];
    pthread_t rd;
    struct { int fd; } *arg = malloc(sizeof(*arg));
    arg->fd = rdfd;
    if (pthread_create(&rd, NULL, tvm_brunner_reader, arg) == 0) {
        pthread_detach(rd);
        return (*env)->NewStringUTF(env,
            "[TwinVM] ══ 容器B 已在独立进程启动（与 A 并发）══\n"
            "结果见 twinvm-live-b.log（完成后自动更新结果页提示）\n");
    }
    close(rdfd);
    free(arg);
    return (*env)->NewStringUTF(env, "[TwinVM] 读取线程创建失败（容器仍在跑，看日志）");
}

/* v1.2.96 Stage 5-M1：容器 dex 选择（声明在文件头，此处仅入口） */
/* v1.2.111 ★★★ M2：运行期安装（引擎层 vm_rom_install——v1.2.109 内嵌版
 * 三处错位写入已在引擎层重写并主机全链验证：装2次→重载→全136 inode CRC 全过）。
 * 返回安装后的 ROM 路径（null=失败）。 */
JNIEXPORT jstring JNICALL
Java_dev_twinbox_vm_Native_installApk(JNIEnv* env, jclass cls,
                                       jstring apkPath, jstring romPath) {
    const char* apk = apkPath ? (*env)->GetStringUTFChars(env, apkPath, NULL) : NULL;
    const char* rom = romPath ? (*env)->GetStringUTFChars(env, romPath, NULL) : NULL;
    char name[512];
    int r = -99;
    if (apk && rom)
        r = vm_rom_install(apk, rom, "/data/app", name, sizeof(name));
    if (apk) (*env)->ReleaseStringUTFChars(env, apkPath, apk);
    if (rom) (*env)->ReleaseStringUTFChars(env, romPath, rom);
    if (r != 0) { LOGI("installApk fail: %d", r); return NULL; }
    LOGI("installApk: -> %s ok", name);
    return (*env)->NewStringUTF(env, name);
}

/* v1.2.113 ★ M2：列出容器已装 APK（/data/app/ 下全部文件） */
JNIEXPORT jobjectArray JNICALL
Java_dev_twinbox_vm_Native_listInstalledApks(JNIEnv* env, jclass cls, jstring romPath) {
    const char* rom = romPath ? (*env)->GetStringUTFChars(env, romPath, NULL) : NULL;
    if (!rom) return NULL;
    VmRom r;
    if (vm_rom_load(&r, rom) != 0) {
        (*env)->ReleaseStringUTFChars(env, romPath, rom);
        return NULL;
    }
    /* 数 /data/app/ 前缀 */
    const char* P = "/data/app/";
    size_t pl = strlen(P);
    int n = 0;
    for (uint32_t i = 0; i < r.inode_count; i++)
        if (strncmp(r.inodes[i].name, P, pl) == 0) n++;
    jclass scls = (*env)->FindClass(env, "java/lang/String");
    jobjectArray arr = (*env)->NewObjectArray(env, n > 0 ? n : 1, scls, NULL);
    int k = 0;
    for (uint32_t i = 0; i < r.inode_count && k < n; i++) {
        if (strncmp(r.inodes[i].name, P, pl) == 0) {
            (*env)->SetObjectArrayElement(env, arr, k++,
                (*env)->NewStringUTF(env, r.inodes[i].name));
        }
    }
    vm_rom_close(&r);
    (*env)->ReleaseStringUTFChars(env, romPath, rom);
    return arr;
}

/* v1.2.113 M2：从 ROM 提取文件到宿主路径（Java 侧扫描 dex 用） */
JNIEXPORT jboolean JNICALL
Java_dev_twinbox_vm_Native_extractFile(JNIEnv* env, jclass cls,
                                        jstring romPath, jstring innerPath,
                                        jstring outPath) {
    const char* rom = romPath ? (*env)->GetStringUTFChars(env, romPath, NULL) : NULL;
    const char* inn = innerPath ? (*env)->GetStringUTFChars(env, innerPath, NULL) : NULL;
    const char* out = outPath ? (*env)->GetStringUTFChars(env, outPath, NULL) : NULL;
    jboolean ok = 0;
    if (rom && inn && out) {
        VmRom r;
        if (vm_rom_load(&r, rom) == 0) {
            const VmRomInode* ni = vm_rom_find(&r, inn);
            if (ni && ni->size > 0) {
                FILE* f = fopen(out, "wb");
                if (f) {
                    ok = fwrite(ni->map, 1, ni->size, f) == ni->size;
                    fclose(f);
                }
            }
            vm_rom_close(&r);
        }
    }
    if (rom) (*env)->ReleaseStringUTFChars(env, romPath, rom);
    if (inn) (*env)->ReleaseStringUTFChars(env, innerPath, inn);
    if (out) (*env)->ReleaseStringUTFChars(env, outPath, out);
    return ok;
}

/* v1.2.111：ROM 全量 CRC 验证（后台自愈用；损坏则删掉重提 assets） */
JNIEXPORT jboolean JNICALL
Java_dev_twinbox_vm_Native_verifyRom(JNIEnv* env, jclass cls, jstring romPath) {
    const char* rom = romPath ? (*env)->GetStringUTFChars(env, romPath, NULL) : NULL;
    if (!rom) return 0;
    VmRom r;
    int ok = vm_rom_load(&r, rom) == 0;
    if (ok) vm_rom_close(&r);
    LOGI("verifyRom: %s (%s)", rom, ok ? "OK" : "CORRUPT");
    (*env)->ReleaseStringUTFChars(env, romPath, rom);
    return (jboolean)ok;
}

/* v1.2.108 M2：主类显式指定（APK 类路径必须——manifest 解析后续做） */
JNIEXPORT void JNICALL
Java_dev_twinbox_vm_Native_setMainClass(JNIEnv* env, jclass cls, jstring mainClass) {
    const char* p = mainClass ? (*env)->GetStringUTFChars(env, mainClass, NULL) : NULL;
    if (p && p[0]) snprintf(g_main_class, sizeof(g_main_class), "%s", p);
    /* v1.2.118 M3：类名含 "Activity" 且非精确 …Main 结尾 → 引导模式
     * （com.twinvm.demo.CliMain 走直跑；…MainActivity 走 Bootstrap）。
     * 判据：含 "Activity" 子串。CliMain 不含 Activity 天然不误伤。 */
    g_boot_mode = (g_main_class[0] != '\0'
                   && strstr(g_main_class, "Activity") != NULL);
    LOGI("setMainClass: '%s' -> g_main_class='%s' boot=%d",
         p ? p : "(null)", g_main_class, g_boot_mode);
    if (p) (*env)->ReleaseStringUTFChars(env, mainClass, p);
}

JNIEXPORT void JNICALL
Java_dev_twinbox_vm_Native_setDex(JNIEnv* env, jclass cls, jstring dexPath) {
    const char* p = dexPath ? (*env)->GetStringUTFChars(env, dexPath, NULL) : NULL;
    if (p && p[0] == '/') {
        snprintf(g_dex_path, sizeof(g_dex_path), "%s", p);
    }
    if (p) (*env)->ReleaseStringUTFChars(env, dexPath, p);
}

JNIEXPORT jstring JNICALL
Java_dev_twinbox_vm_Native_probe(JNIEnv* env, jclass cls) {
    pid_t pid = fork();
    if (pid == 0) {
        /* 无害过滤器：全部 ALLOW——只测"能不能装" */
        struct sock_filter f[] = {
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        };
        struct sock_fprog prog = { 1, f };
        if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) _exit(97);
        if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) != 0) _exit(96);
        _exit(0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;

    char b[512];
    snprintf(b, sizeof(b),
             "[TwinVM] TwinVM 引擎能力探测\n"
             "  架构            : %s\n"
             "  seccomp 用户态拦截 : %s\n"
             "  进程内 ELF 装载器  : 内置（绕 W^X exec 禁令）\n"
             "  ROM 虚拟文件系统   : 内置（inode 表 + crc32）\n"
             "  内核模式        : 用户态（免 root，同 VMOS 路线）\n"
             "  多 guest 槽位     : Stage 1（fork 槽位池）\n",
             vm_arch_name(),
             ok ? "可用（核心前提成立）" : "不可用（内核限制？）");
    return (*env)->NewStringUTF(env, b);
}
