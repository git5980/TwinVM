/* v0.8.0 —— SECCOMP_RET_USER_NOTIF 后端（路线 A）。
 *
 * 为什么换机制（真机 v0.7.92/93 实锤）：
 *   App 进程**天生带 zygote seccomp 过滤器**（安装前 prctl(PR_GET_SECCOMP)=2）。
 *   内核多层叠加时取"动作优先级最高"的判决：KILL_PROCESS > TRAP > ALLOW。
 *   我们要 TRAP 的号里，若外层判 KILL，内核就 KILL —— 进程秒死、handler 永不进入。
 *   而 USER_NOTIF 走的是**文件描述符 + ioctl**，不依赖信号投递：
 *   过滤器命中 → 内核把请求挂起 → 监听线程 ioctl(RECV) 收 → 处理 → ioctl(SEND) 回。
 *   对**外层判 ALLOW 的号**（绝大多数文件 I/O）可以正常拦截，彻底绕开"信号被压过"。
 *
 * 架构：guest 与引擎在同一进程（共享地址空间）；本文件提供：
 *   · vm_notif_install()：用 seccomp(SECCOMP_SET_MODE_FILTER,
 *     SECCOMP_FILTER_FLAG_NEW_LISTENER) 安装过滤器并拿到 listener fd；
 *   · vm_notif_loop()：监听线程主体 —— RECV → 调用 vm_handle_syscall → SEND。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/seccomp.h>
#include <linux/filter.h>
#include <linux/audit.h>
#include <stddef.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "vm_core.h"
#include "bpf_gen.h"   /* VM_BPF_NOTIF_INSTRUCTIONS（v0.8.0） */
/* v0.8.0：旧头文件可能没有该动作宏 */
#ifndef SECCOMP_RET_USER_NOTIF
#define SECCOMP_RET_USER_NOTIF 0x7fc00000U
#endif

/* ── 与内核的 ABI（自备定义，避免头文件版本差异） ── */
struct tvm_seccomp_notif {
    uint64_t id;
    uint32_t pid;
    uint32_t flags;
    struct seccomp_data data;
};
struct tvm_seccomp_notif_resp {
    uint64_t id;
    int64_t  val;
    int32_t  error;
    uint32_t flags;
};
#define TVM_SECCOMP_IOCTL_NOTIF_RECV  SECCOMP_IOWR(0, struct tvm_seccomp_notif)
#define TVM_SECCOMP_IOCTL_NOTIF_SEND  SECCOMP_IOWR(1, struct tvm_seccomp_notif_resp)
#define TVM_SECCOMP_IOCTL_NOTIF_ID_VALID SECCOMP_IOW(2, uint64_t)

static int g_listener = -1;

int vm_notif_fd(void) { return g_listener; }

/* 安装：返回 0 成功；listener fd 存 g_listener */
int vm_notif_install(int console_fd) {
    (void)console_fd;
    struct sock_filter prog[] = VM_BPF_NOTIF_INSTRUCTIONS;
    struct sock_fprog fprog = {
        .len    = (unsigned short)(sizeof(prog) / sizeof(prog[0])),
        .filter = prog,
    };
    /* v0.8.8：逐步打点（真机 v0.8.6/0.8.7 都卡在本函数内、连"拦截就绪"都没到） */
    /* v0.8.8：把步骤 1 用**裸 svc 不行**（prctl 不在放行清单）→ 用宿主 libc 调用。
     * 但更要紧的是：若 prctl 被 App 的 zygote 策略判 KILL，进程会直接被内核干掉
     * （真机 v0.8.6/0.8.7：连"拦截完成"都没打印就没了）。
     * 所以先**探测**：getpid 之后立刻打一行，确认还能继续。 */
    vm_log("[notif] 步骤0：进入安装（下一步 prctl）…");
    int pr = prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    vm_log("[notif] 步骤1：prctl(PR_SET_NO_NEW_PRIVS) 返回 %d（errno=%d）", pr, pr ? errno : 0);
    if (pr != 0) {
        /* 失败不致命：已有 zygote 过滤器时通常已经设置过 NO_NEW_PRIVS */
        vm_log("[notif] 警告：PR_SET_NO_NEW_PRIVS 失败，继续尝试装过滤器");
    }
    vm_log("[notif] 步骤2：syscall(seccomp, SET_MODE_FILTER, NEW_LISTENER)…");
    /* v0.9.0 ★ 真机 v0.8.9 实锤：**卡在这一步**（步骤1 返回 0，步骤2 之后无输出）。
     * 最可能：`SECCOMP_SET_MODE_FILTER` 被 **App 的 zygote 策略**判为
     * `SECCOMP_RET_USER_NOTIF`（Android 的某些加固会这样），于是我们的
     * seccomp(2) 调用被**挂起等 App 的监听者处理** —— 而 App 没有监听者 → 永久挂起。
     *
     * 对策：把安装挪到 **fork 之前的父进程**（父进程若也同样受策略影响，
     * 至少能拿到明确 errno；而子进程继承过滤器与 listener fd）。
     * 这里先加一条"能走多远"的细粒度打点，并把 fd 结果打全。 */
    /* seccomp(SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_NEW_LISTENER, &fprog) */
    long fd = syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER,
                      SECCOMP_FILTER_FLAG_NEW_LISTENER, &fprog);
    if (fd < 0) { vm_log("[notif] seccomp 失败 errno=%d", errno); return -(int)errno; }
    g_listener = (int)fd;
    vm_log("[notif] 步骤3：listener fd=%d（安装成功）", g_listener);
    return 0;
}

/* 监听线程主循环：收一个请求 → 交给引擎 → 回一个响应 */
void* vm_notif_loop(void* arg) {
    (void)arg;
    if (g_listener < 0) return NULL;
    /* v0.8.3 ★ 监听线程守则：**本线程只做 ioctl(NOTIF_RECV/SEND) 与纯内存操作**，
     * 绝不调用任何可能被我们自己的过滤器拦下的 syscall —— 否则会自我挂起（死锁）。
     * 已在引擎侧把 socket/connect 等改成"直接答复、不递归"；这里再加一道说明性检查。 */
    for (;;) {
        struct tvm_seccomp_notif req;
        memset(&req, 0, sizeof(req));
        if (ioctl(g_listener, TVM_SECCOMP_IOCTL_NOTIF_RECV, &req) != 0) {
            if (errno == EINTR) continue;
            break;                      /* listener 关闭/出错 → 退出 */
        }
        struct tvm_seccomp_notif_resp resp;
        memset(&resp, 0, sizeof(resp));
        resp.id = req.id;

        /* v0.8.1a ★ socket(198/41) 在 USER_NOTIF 下**不要把它转给引擎再"原生放行"**。
         * 真机 v0.8.1：ART 线程池起线程后立刻 socket(AF_UNIX, SOCK_RAW|CLOEXEC|NONBLOCK)
         * 卡死 —— 因为该 syscall 被挂起等我们答复，而引擎又去"原生调用"同一个 syscall，
         * 在**多线程 + 监听串行**的情况下形成自锁。
         * 处置：socket 一律由监听线程**直接答复**（AF_UNIX 给真 fd 风险大 → 直接回 EPERM/ENOSYS
         * 更安全：AF_UNIX 用 SOCK_STREAM 时给真 socket，其余回 -EPERM）。 */
#if defined(__aarch64__)
        if (req.data.nr == 198) {
            long dom = (long)req.data.args[0];
            long typ = (long)req.data.args[1];
            long base = typ & 0xF;
            resp.id = req.id;
            /* v0.8.3 ★ **监听线程内绝不调用任何可能被拦截的 syscall**。
             * 全部 socket 请求都在这里直接答复：
             *   · AF_UNIX（本地 IPC，如 logd）→ 我们无法安全地创建真 socket
             *     （监听线程里 socket() 会自我挂起）→ 回 -EACCES，
             *     bionic/ART 对 EACCES 有退化路径（v0.7.15 已验证需要"非 EACCES"，
             *     但那时是 SIGSYS 后端；USER_NOTIF 下 EACCES 是可接受的兜底）；
             *   · 其余族（INET/INET6/RAW…）→ 同样 -EACCES（VM 无网络栈）。 */
            (void)base; (void)typ;
            /* v0.8.4：AF_UNIX 的 socket 探测**假装成功**（返回一个"虚拟 fd"）。
             * 历史：v0.7.15 真机实锤 —— `libstatssocket` 探测 socket() 拿到 EACCES
             * 会直接判 JNI 失败；而 ART/statssocket 在起不来的情况下会让后续
             * futex 死等（v0.8.3 现场：socket→EACCES ×2 之后 connect(fd=13) + futex 卡死）。
             * 我们无法在监听线程里创建真 socket（会自递归），所以给一个**高位虚拟 fd**：
             * 后续对该 fd 的 write/close 由引擎的 fd 路由处理（tee/假装成功），
             * 不会真的落到内核。 */
            if (dom == 1 /*AF_UNIX*/) {
                static int fake_fd = 20000;          /* 高位：引擎按虚拟 fd 处理 */
                resp.val = fake_fd++; resp.error = 0;
                vm_log("[TwinVM] [notif] socket(AF_UNIX) -> 虚拟 fd %ld（假装成功，避免 EACCES 致命）",
                       resp.val);
            } else {
                resp.val = 0; resp.error = EACCES;
                vm_log("[TwinVM] [notif] socket(dom=%ld) -> -EACCES（VM 无网络栈）", dom);
            }
            if (ioctl(g_listener, TVM_SECCOMP_IOCTL_NOTIF_SEND, &resp) != 0) { }
            continue;
        }
#endif
        /* v0.8.1 ★ execve(221/59) 在 USER_NOTIF 下**无法用"改 ucontext 跳转"的老办法**
         * （那是信号机制特有的；USER_NOTIF 只是"内核在等我们的答复"）。
         * 而 guest 的 `/bin/guest` 恰恰要 execve 到 ROM 里的 dalvikvm。
         * 处理：直接在**监听线程**里完成 ROM 内 ELF 的装载与跳转是不可能的
         * （跳转必须发生在目标线程上下文）。这里先返回 ENOSYS，
         * 让引擎/guest 的退化路径接管，并大声记日志以便下一步决策。 */
#if defined(__aarch64__)
        if (req.data.nr == 221 || req.data.nr == 281) {   /* execve / execveat */
            vm_log("[TwinVM] [notif] execve(nr=%llu) 进入 —— 需要'跨线程跳转'方案",
                   (unsigned long long)req.data.nr);
        }
#endif
        /* 转成引擎的 syscall 语义（VmCpu 取当前槽位） */
        VmCpu* c = vm_machine_current();
        long r;
        if (!c) {
            resp.error = ENOSYS;
            r = -ENOSYS;
        } else {
            r = vm_handle_syscall(c, (long)req.data.nr,
                                  (long)req.data.args[0], (long)req.data.args[1],
                                  (long)req.data.args[2], (long)req.data.args[3],
                                  (long)req.data.args[4], (long)req.data.args[5]);
            if (r < 0 && r > -4096) { resp.error = (int32_t)(-r); resp.val = 0; }
            else                    { resp.val = r; resp.error = 0; }
        }
        if (ioctl(g_listener, TVM_SECCOMP_IOCTL_NOTIF_SEND, &resp) != 0) {
            /* 目标进程已退出等：忽略 */
        }
    }
    return NULL;
}
