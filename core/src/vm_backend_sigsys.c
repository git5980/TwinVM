/* TwinVM 拦截后端：seccomp-BPF + SIGSYS（进程内内核）。
 *
 * VMOS 同型的免 root 虚拟化核心机制：
 *   1. guest 代码原生执行（同架构 → 零模拟损耗）
 *   2. BPF 过滤器把 guest 的系统调用全部转成 SIGSYS 信号
 *   3. SIGSYS 处理器在用户空间"执行"这次系统调用（VFS/ROM/控制台桥）
 *   4. 修改被中断上下文的返回值寄存器，跳过原指令继续
 *
 * 过滤器放行清单（其余全部 TRAP，见 gen_bpf.py / bpf_gen.h 的 VM_BPF_ALLOW_NRS）：
 *   exit_group / exit —— guest 关机 / 线程退出（bionic __exit）
 *   rt_sigreturn —— 信号框架完整性
 *   gettid 等"判决表直发"的安全原语（VM_PT*）—— 铁律：直发集合必须 ⊆ 放行清单，
 *      否则 handler 里裸 svc 会被本过滤器二次 TRAP（v0.2.8 修的 Stage 1 code=-31）
 *   write(fd==console) —— 控制台桥直通（处理器自身写管道不递归）
 */
#define _GNU_SOURCE
#include "vm_core.h"

#include <elf.h>

#include <errno.h>
#include <stdlib.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/prctl.h>
/* v0.8.0：旧头文件可能没有该动作宏 */
#ifndef SECCOMP_RET_USER_NOTIF
#define SECCOMP_RET_USER_NOTIF 0x7fc00000U
#endif
#include <sys/ucontext.h>
#if defined(__x86_64__)
#include <sys/reg.h>   /* REG_* 只有 x86_64 用得到（glibc-aarch64 没有这个头） */
#endif
/* glibc 的 REG_* 枚举不含 ORIG_RAX；gregs[15] 即 kernel user_regs_struct.orig_rax（REG_RIP=16 旁证） */
#define VM_REG_ORIG_RAX 15
#include <unistd.h>

#if defined(__x86_64__)
#define VM_ARCH_AUDIT AUDIT_ARCH_X86_64
#define VM_NR_write   1
#define VM_NR_rt_sigreturn 15
#define VM_NR_exit_group 231
#define VM_NR_arch_prctl 158
#elif defined(__aarch64__)
#define VM_ARCH_AUDIT AUDIT_ARCH_AARCH64
#define VM_NR_write   64
#define VM_NR_rt_sigreturn 139
#define VM_NR_exit_group 94
#else
#error "sigsys 后端仅支持 x86_64/aarch64"
#endif

/* BPF 程序（13 条，跳转偏移手工推导，改动需重算）：
 *   0 LD arch            1 JEQ ARCH,→2,→12(KILL)
 *   2 LD nr              3 JEQ exit_group →11(ALLOW)
 *   4 LD nr              5 JEQ rt_sigreturn →11(ALLOW)
 *   6 LD nr              7 JEQ write,→8,→10(TRAP)
 *   8 LD args[0]低32位   9 JEQ console_fd →11(ALLOW) else →10(TRAP)
 *  10 RET TRAP          11 RET ALLOW          12 RET KILL            */
static int vm_seccomp_install(int console_fd) {
    #include "bpf_gen.h"
    struct sock_filter prog[] = VM_BPF_INSTRUCTIONS;
    struct sock_fprog fprog = {
        .len = (unsigned short)(sizeof(prog) / sizeof(prog[0])),
        .filter = prog,
    };
    /* v0.7.92：安装**之前**问内核当前 seccomp 状态（此刻还没装上，
     * 调用 prctl 不会被自己拦）。装完后再问就没意义了——那会被自己 TRAP。 */
    long pre = prctl(PR_GET_SECCOMP);
    vm_log("[TwinVM] 安装前 prctl(PR_GET_SECCOMP)=%ld（0=无 1=strict 2=filter）", pre);

    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) return -errno;
    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &fprog, 0, 0) != 0) return -errno;
    return 0;
}

/* ── TLS 基址切换 ──
 * guest（bionic）会 arch_prctl/tpidr 设自己的 TLS；SIGSYS handler 是 glibc 宿主代码，
 * printf/锁全依赖宿主 TLS 基址——进入 handler 必须切回宿主，返回前恢复 guest。 */
static unsigned long g_libc_tls;

/* 后端自用的直发 syscall（vm_core 的 vm_syscall6 是 static） */
static long be_sys(long nr, long a, long b) {
#if defined(__x86_64__)
    long ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(nr), "D"(a), "S"(b) : "rcx", "r11", "memory");
    return ret;
#else
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1) : "memory", "cc");
    return x0;
#endif
}

/* 宿主（bionic）TLS 交换。v0.7.38：整块允许被预定义覆盖 ——
 * 白盒测试跑在 glibc 进程里，msr tpidr_el0 会把 libc 的 TLS 换掉直接崩。 */
#ifndef VM_TLS_GET
#if defined(__x86_64__)
#define VM_TLS_GET(p)  be_sys(158 /*arch_prctl*/, 0x1003 /*ARCH_GET_FS*/, (long)(p))
#define VM_TLS_SET(v)  be_sys(158, 0x1002 /*ARCH_SET_FS*/, (long)(v))
#else
static unsigned long rd_tpidr(void) { unsigned long v; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(v)); return v; }
static void wr_tpidr(unsigned long v) { __asm__ volatile("msr tpidr_el0, %0" :: "r"(v)); }
#define VM_TLS_GET(p)  (*(unsigned long*)(p) = rd_tpidr(), 0)
#define VM_TLS_SET(v)  wr_tpidr((unsigned long)(v))
#endif
#endif /* VM_TLS_GET */

#ifndef SYS_SECCOMP
#define SYS_SECCOMP 1
#endif

/* ── 极简 seccomp-BPF 解释器（启动自检用）──
 * 只支持本生成器产出的指令形态：LD|W|ABS(0/4/16) / JEQ K / RET K。
 * 用途：在真正 prctl(PR_SET_SECCOMP) 之前，把"判决表直发集合"逐个喂进过滤器，
 * 确认它们都被判成 ALLOW —— 这是 v0.2.8 修的 bug 的运行时护栏。 */
static long vm_bpf_eval(const struct sock_filter* prog, size_t n, uint32_t nr,
                        uint32_t arch, uint64_t arg0, uint32_t console_fd, uint64_t arg4) {
    uint32_t a = 0;
    size_t pc = 0;
    for (int guard = 0; guard < 4096 && pc < n; guard++) {
        uint16_t code = prog[pc].code;
        uint8_t cls = (uint8_t)(code & 0x07);
        if (cls == 0x00 && (code & 0x18) == 0x00) {          /* BPF_LD|BPF_W|BPF_ABS */
            switch (prog[pc].k) {
                case 0:  a = nr;             break;
                case 4:  a = arch;           break;
                case 16: a = (uint32_t)arg0; break;
                case 48: a = (uint32_t)arg4; break;   /* 3.1：mmap args[4]=fd */
                default: return -1;
            }
            pc++;
        } else if (cls == 0x05 && (code & 0xf0) == 0x10) {   /* BPF_JMP|BPF_JEQ|BPF_K */
            uint32_t k = prog[pc].k;
            pc += 1 + (size_t)((a == k) ? prog[pc].jt : prog[pc].jf);
        } else if (cls == 0x05 && (code & 0xf0) == 0x30) {   /* BPF_JMP|BPF_JGE|BPF_K */
            uint32_t k = prog[pc].k;
            pc += 1 + (size_t)((a >= k) ? prog[pc].jt : prog[pc].jf);
        } else if (cls == 0x05 && (code & 0xf0) == 0x20) {   /* BPF_JMP|BPF_JGT|BPF_K */
            uint32_t k = prog[pc].k;
            pc += 1 + (size_t)((a > k) ? prog[pc].jt : prog[pc].jf);
        } else if (cls == 0x06) {                            /* BPF_RET|BPF_K */
            return (long)prog[pc].k;
        } else {
            return -1;
        }
    }
    return -1;
}

#define VM_SECCOMP_RET_ALLOW 0x7fff0000L

/* v0.7.24：给 vm_core 的"直发内核"(VM_PT*)通道做运行时护栏用。
 * 返回过滤器对这次调用的判决（VM_SECCOMP_RET_ALLOW / TRAP / KILL_PROCESS），-1=不认识。
 * 为什么要它：只靠"直发清单表 + 启动自检"是**手维护**的——2026-10-13 17:09 真机实锤：
 * 有人在判决表里加了 connect(203) 的 VM_PT3 直发，但没登记进表，自检静默通过，
 * 结果 handler 内裸 svc 被自家过滤器二次 TRAP（SIGSYS 阻塞态）→ 挂起 → code=-31。
 * 现在每次直发前都问一遍过滤器：不放行就拒绝直发、返回 -ENOSYS，这类事故不可能再发生。 */
long vm_filter_action(long nr, long a0, long a1, long a2, long a3, long a4, long a5) {
    (void)a1; (void)a2; (void)a3; (void)a5;
    int console_fd = vm_console_fd;
    struct sock_filter prog[] = VM_BPF_INSTRUCTIONS;
    size_t n = sizeof(prog) / sizeof(prog[0]);
    return vm_bpf_eval(prog, n, (uint32_t)nr, VM_ARCH_AUDIT,
                       (uint64_t)a0, (uint32_t)console_fd, (uint64_t)a4);
}
#if defined(__x86_64__)
#define VM_NR_mmap_selftest 9
#else
#define VM_NR_mmap_selftest 222
#endif

int vm_filter_check_passthrough(void) {
    int console_fd = vm_console_fd;          /* VM_BPF_INSTRUCTIONS 里引用这个名字 */
    struct sock_filter prog[] = VM_BPF_INSTRUCTIONS;
    size_t n = sizeof(prog) / sizeof(prog[0]);
    int bad = 0;
    for (int i = 0; i < vm_passthrough_nr_count; i++) {
        long nr = vm_passthrough_nrs[i];
        /* 3.1：mmap 参数化——直发自检用普通 fd（非虚拟区间）评估 */
        long act = vm_bpf_eval(prog, n, (uint32_t)nr, VM_ARCH_AUDIT, 0, (uint32_t)console_fd, 0);
        if (act != VM_SECCOMP_RET_ALLOW) {
            bad++;
            vm_log("!! 直发清单自检失败: nr=%ld 在 BPF 里被判为 0x%lx（应为 ALLOW）"
                   "—— handler 内直发会二次 TRAP（Stage 1 code=-31 同型故障）", nr, act);
        }
    }
    /* 3.1 正向护栏：mmap+虚拟 binder fd 必须被判 TRAP（零拷贝段截获入口）。
     * 反向（普通 fd → ALLOW）已由上面直发清单的 mmap+arg4=0 项覆盖。 */
    {
        struct sock_filter prog2[] = VM_BPF_INSTRUCTIONS;
        size_t n2 = sizeof(prog2) / sizeof(prog2[0]);
        uint32_t mm = (uint32_t)VM_NR_mmap_selftest;
        long act = vm_bpf_eval(prog2, n2, mm, VM_ARCH_AUDIT, 0, (uint32_t)console_fd, VM_FD_BASE);
        if (act == VM_SECCOMP_RET_ALLOW) {
            bad++;
            vm_log("!! mmap 参数化护栏失败: 虚拟 fd=%d 应判 TRAP 却 ALLOW——零拷贝段截获将失效", VM_FD_BASE);
        }
    }
    /* v0.7.22：引擎自身在 handler 内调用的 syscall（process_vm_readv/writev）也必须放行。
     * 一旦漏了，vm_mem_read/write 在 handler 里就会二次 TRAP —— 野指针防护反而变成死机源。 */
    for (int i = 0; i < vm_engine_syscall_nr_count; i++) {
        long nr = vm_engine_syscall_nrs[i];
        /* v0.7.85：表已清空（只剩 -1 哨兵），必须跳过 —— 否则每次启动都误报
         * "引擎自用 syscall 未放行: nr=-1"（真机 v0.7.84 日志可见）。 */
        if (nr < 0) continue;
        long act = vm_bpf_eval(prog, n, (uint32_t)nr, VM_ARCH_AUDIT, 0, (uint32_t)console_fd, 0);
        if (act != VM_SECCOMP_RET_ALLOW) {
            bad++;
            vm_log("!! 引擎自用 syscall 未放行: nr=%ld 判为 0x%lx（应为 ALLOW）"
                   "—— handler 内安全访存会二次 TRAP（会崩槽位）", nr, act);
        }
    }
    return bad;
}

/* ── 迟到/串味投递防护（v0.2.8）──
 * 若 guest 在自己把 SIGSYS 屏蔽了的窗口里触发 TRAP（bionic pthread_create 在
 * clone 前后就是这么干的），内核不会立刻投递：syscall 被丢弃、SIGSYS 挂起，直到
 * guest 自己 rt_sigprocmask 恢复掩码才投递。此刻 ucontext 里的寄存器早已不是系统
 * 调用现场，拿 r[8] 当 nr、拿 r[0..5] 当参数就会执行一次完全无关的 syscall
 * （甚至 handler 自递归）。判据：seccomp 生成信号时内核记下 si_call_addr
 * （= 出事当时的 PC）；若它与信号真正投递时的 ucontext PC 不一致，即为迟到投递 ——
 * 只告警、不判决。 */
static uintptr_t vm_ucontext_pc(ucontext_t* uc) {
#if defined(__x86_64__)
    return (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
#else
    return (uintptr_t)uc->uc_mcontext.pc;
#endif
}

static void vm_warn_stale(long nr, uintptr_t trap_pc, uintptr_t now_pc) {
    static long stale_count;      /* 限频：前 8 条 + 每 64 条一条 */
    if (stale_count < 8 || (stale_count & 0x3F) == 0) {
        char b[192];
        int k = snprintf(b, sizeof(b),
                         "[TwinVM] 迟到 TRAP nr=%ld（生成于 0x%lx，投递于 0x%lx）："
                         "该 syscall 在 SIGSYS 被屏蔽的窗口里已被内核丢弃，本次不判决\n",
                         nr, (unsigned long)trap_pc, (unsigned long)now_pc);
        vm_console_write(b, (size_t)k);
    }
    stale_count++;
}

/* SIGSYS：guest 的系统调用落点 —— 用户态内核入口 */
static void vm_sigsys_handler(int sig, siginfo_t* si, void* vuc) {
    /* v0.7.85：**绝对第一行**裸 write 探针 —— 不依赖 libc/TLS/任何状态。
     * 之前的 HND# 走 vm_log（glibc printf，依赖宿主 TLS），在 TLS 被 guest 抢占的
     * 窗口里会静默失败。这一条只要 fd 可写就必然留痕：[HR] nr=N\n */
    {
        extern int vm_console_fd;
        long nr0 = (long)si->si_syscall;
        char b[32]; int k = 0;
        b[k++]='['; b[k++]='H'; b[k++]='R'; b[k++]=']'; b[k++]=' ';
        char d[12]; int dn = 0; long v = nr0 < 0 ? -nr0 : nr0;
        if (v == 0) d[dn++]='0';
        while (v > 0) { d[dn++] = (char)('0' + (v % 10)); v /= 10; }
        if (nr0 < 0) b[k++]='-';
        for (int q = dn - 1; q >= 0; q--) b[k++] = d[q];
        b[k++] = '\n';
        if (vm_console_fd >= 0) {
#if defined(__x86_64__)
            /* v1.1.3：补 x86_64 宿主孪生分支（此前无守卫，v0.7.85 引入移植性回归） */
            long hr_ret;
            __asm__ volatile("syscall" : "=a"(hr_ret)
                             : "a"(1L /* write */), "D"((long)vm_console_fd),
                               "S"((long)b), "d"((long)k)
                             : "rcx", "r11", "memory");
            (void)hr_ret;
#else
            register long r0 __asm__("x0") = 64;          /* write */
            register long r1 __asm__("x1") = (long)vm_console_fd;
            register long r2 __asm__("x2") = (long)b;
            register long r3 __asm__("x3") = (long)k;
            register long r8 __asm__("x8") = 64;
            __asm__ volatile("svc #0" : "+r"(r0)
                             : "r"(r1), "r"(r2), "r"(r3), "r"(r8) : "memory");
#endif
        }
    }
    (void)sig;
    ucontext_t* uc = (ucontext_t*)vuc;
    VmCpu* c = vm_cpu_current();
    if (!c) _exit(91);
    /* v0.7.53 的 in-handler sigaction() 自查已移除（v0.7.54）：
     * 信号处理器里调用 sigaction 不是异步信号安全的，而且它只能证明"现在"，
     * 证明不了"死的那一刻"。改用父进程读 /proc/<pid>/status 的 SigCgt
     * （blackbox 已经在打）——僵尸进程的该字段仍然保留，能直接看出
     * SIGSYS 是否还在"被捕获"位里。 */

#if defined(__x86_64__)
    c->guest_sp = (uintptr_t)uc->uc_mcontext.gregs[REG_RSP];
#else
    c->guest_sp = (uintptr_t)uc->uc_mcontext.sp;
#endif

    /* v0.7.81：**最原始**的探针 —— 裸 write() 直写 console fd。
     * 之前的 HND# 走 vm_log（glibc printf 路径，依赖宿主 TLS），
     * 若 TLS 状态不对就静默失败。这里不依赖任何库状态，只要 fd 可写就能留痕。 */
    {
        extern int vm_console_fd;
        long nr0 = (long)si->si_syscall;
        char b[48]; int k = 0;
        b[k++]='['; b[k++]='H'; b[k++]='R'; b[k++]=']'; b[k++]=' ';
        const char* pfx = "nr="; for (int q=0;q<3;q++) b[k++]=pfx[q];
        char d[12]; int dn=0; long v = nr0 < 0 ? -nr0 : nr0;
        if (v==0) d[dn++]='0';
        while (v>0) { d[dn++]=(char)('0'+v%10); v/=10; }
        if (nr0<0) b[k++]='-';
        for (int q=dn-1;q>=0;q--) b[k++]=d[q];
        b[k++]='\n';
        if (vm_console_fd >= 0) {
#if defined(__x86_64__)
            /* v1.1.3：补 x86_64 宿主孪生分支（此前裸 svc 无守卫，x86 编不过） */
            long r;
            __asm__ volatile("syscall" : "=a"(r)
                             : "a"(1L /* write */), "D"((long)vm_console_fd),
                               "S"((long)b), "d"((long)k)
                             : "rcx", "r11", "memory");
            (void)r;
#else
            /* v1.1.3：改用 x0-x3/x8 固定寄存器约束（原写法 7 个 "r" 操作数但
             * 指令串不引用 %N —— 寄存器由编译器随机分配，svc 却固定读 x0-x6，
             * arm64 上探针写的实为随机寄存器）。与上方 HND# 探针同款正确写法。 */
            register long r0 __asm__("x0") = 64;          /* write */
            register long r1 __asm__("x1") = (long)vm_console_fd;
            register long r2 __asm__("x2") = (long)b;
            register long r3 __asm__("x3") = (long)k;
            register long r8 __asm__("x8") = 64;
            __asm__ volatile("svc #0" : "+r"(r0)
                             : "r"(r1), "r"(r2), "r"(r3), "r"(r8) : "memory");
#endif
        }
    }
    long nr = (long)si->si_syscall;                 /* 内核给的号 = 唯一可信来源 */
    /* v0.7.79：**无死角**记录进入 handler 的每一个 syscall 号（前 400 个）。
     * 之前 TRAP 行只在 "n4<4096 && nr!=227" 条件下打，且可能被 stale 早退吞掉；
     * 现在在最早的位置、无条件记录，用来回答"process_vm_readv 到底进没进 handler"。 */
    {
        static long hb_cnt = 0;
        if (hb_cnt < 400) {
            hb_cnt++;
            vm_log("[4.2dbg] HND#%ld nr=%ld pc=0x%llx", hb_cnt, nr,
                   (unsigned long long)vm_ucontext_pc(uc));
        }
    }
    vm_bb_pc((unsigned long)vm_ucontext_pc(uc));    /* v0.7.50: 共享黑匣子补 PC */
    uintptr_t now_pc = vm_ucontext_pc(uc);
#ifdef si_call_addr
    uintptr_t trap_pc = (uintptr_t)si->si_call_addr;   /* 内核生成信号瞬间的 PC */
#else
    uintptr_t trap_pc = 0;                             /* 头文件没暴露该字段 */
#endif
    /* v0.7.86 ★ arm64 上没有 si_call_addr —— 迟到投递检测以前**完全失效**。
     * 真机实锤（v0.7.85 trace）：#25 faccessat(48) 之后紧跟的 #26 参数完全错位
     * （nr=64 却 a1=0x5 / a2=0x70ecfb8710），这是典型的"信号迟到投递"：
     * guest 在屏蔽窗口里触发 TRAP → 信号挂起 → 掩码恢复后才投递，
     * 此刻 ucontext 里的寄存器早已是**另一个**调用的现场。
     *
     * 交叉校验（arm64 专用）：seccomp 语义下，触发 TRAP 的系统调用号必须在
     * `seccomp_data.nr`（=si_syscall）与寄存器 x8（Linux 系统调用号载体）**一致**；
     * 不一致即为迟到/串味投递 → 绝不按系统调用判决，直接按"原样返回 -ENOSYS"处理。
     * 这样既不会误执行，也不会把错位参数喂给我们的实现。 */
    int stale = (si->si_code == SYS_SECCOMP && trap_pc != 0 && trap_pc != now_pc);
#if defined(__aarch64__)
    if (!stale && si->si_code == SYS_SECCOMP) {
        unsigned long x8v = (unsigned long)uc->uc_mcontext.regs[8];
        if (x8v != (unsigned long)nr) {
            stale = 1;
            vm_log("[TwinVM] !! 迟到投递：si_syscall=%ld 但 x8=%lu —— 按 -ENOSYS 原样返回",
                   nr, x8v);
        }
    }
#endif

    if (stale) {                                    /* 迟到投递：绝不按系统调用判决 */
        vm_warn_stale(nr, trap_pc, now_pc);
        return;
    }

    /* TLS 保全：guest 与 libc 宿主的 TLS 基址切换（bionic 抢占 FS/tpidr 后
     * 宿主 glibc 的 printf/锁会读错地址——第一次能打日志第二次必炸的根因） */
    unsigned long saved_tls = 0;   /* 局部保存=线程安全（bionic 子线程 TRAP 时不踩主线程槽） */
    int tls_swapped = 0;
    VM_TLS_GET(&saved_tls);
    if (saved_tls != g_libc_tls) { VM_TLS_SET(g_libc_tls); tls_swapped = 1; }

    {   /* 4.2 排障：前 512 条 TRAP 打 nr+pc+参数。必须在 TLS 交换后（vm_log=glibc：
         * bionic FS 下 snprintf 读 TLS 内部直接 SIGSEGV——x86 本地静态死之谜的真相）
         * v0.7.18：64→512（.so 装载阶段就吃满 64，ART init 全程盲区） */
        static long n4;
        if (n4 < 4096 && nr != 227) {   /* 227=msync：ART 会逐页 flush，噪声太大 */   /* v0.7.46: 512 太小 —— 死亡现场恰好总在 512 之后 */
            n4++;
#if defined(__x86_64__)
            greg_t* gx0 = uc->uc_mcontext.gregs;
            vm_log("[4.2dbg] TRAP#%ld nr=%ld pc=0x%llx a0=0x%llx a1=0x%llx a2=0x%llx stale=%d", n4, nr,
                   (unsigned long long)now_pc,
                   (unsigned long long)(uintptr_t)gx0[REG_RDI],
                   (unsigned long long)(uintptr_t)gx0[REG_RSI],
                   (unsigned long long)(uintptr_t)gx0[REG_RDX], stale);
#else
            uint64_t* rx0 = uc->uc_mcontext.regs;
            vm_log("[4.2dbg] TRAP#%ld nr=%ld pc=0x%llx a0=0x%llx a1=0x%llx a2=0x%llx stale=%d", n4, nr,
                   (unsigned long long)now_pc,
                   (unsigned long long)rx0[0],
                   (unsigned long long)rx0[1],
                   (unsigned long long)rx0[2], stale);
#endif
        }
    }
#if defined(__x86_64__)
#define VM_NR_execve 59
#else
#define VM_NR_execve 221
#endif

    /* Stage 2.1：虚拟 execve —— guest 的进程创建走用户态内核装载（VMOS libkrnloader 语义）。
     * 从 ROM 装载新映像 + 构造真 argv/envp 新栈，改写被中断上下文 PC/SP 直接跳进
     * 新程序 = execve 成功语义（永不返回）。ROM 未命中 → -ENOENT。
     * 注意：必须在 TLS 交换之后执行（loader 用宿主 libc）；static 字符串缓冲避免
     * 大数组在信号栈上的溢出风险。 */
    if (nr == VM_NR_execve) {
        char path[VM_MAX_PATH + 1];
        long e0 = -2 /* ENOENT */;
#if defined(__x86_64__)
        greg_t* gx = uc->uc_mcontext.gregs;
        uintptr_t p0 = (uintptr_t)gx[REG_RDI];
        uintptr_t p1 = (uintptr_t)gx[REG_RSI];
        uintptr_t p2 = (uintptr_t)gx[REG_RDX];
#else
        uint64_t* rx = uc->uc_mcontext.regs;
        uintptr_t p0 = (uintptr_t)rx[0];
        uintptr_t p1 = (uintptr_t)rx[1];
        uintptr_t p2 = (uintptr_t)rx[2];
#endif
        do {
            if (vm_mem_read(&c->mem, path, p0, VM_MAX_PATH) != 0) { e0 = -14; break; }
            path[VM_MAX_PATH] = 0;
            const VmRomInode* ino = vm_rom_find(c->vfs->rom, path);
            if (!ino) break;
            static char strs[16384];
            size_t so = 0;
            char* gav[64]; char* gev[64];
            int na = 0, ne = 0, ok = 1;
            for (int side = 0; side < 2 && ok; side++) {
                uintptr_t vec = side ? p2 : p1;
                if (!vec) break;
                char** dst = side ? gev : gav;
                int* cnt = side ? &ne : &na;
                for (int i = 0; i < 63 && ok; i++) {
                    uintptr_t sp = 0;
                    if (vm_mem_read(&c->mem, &sp, vec + (size_t)i * 8, 8) != 0) { ok = 0; break; }
                    if (sp == 0) break;
                    size_t l = 0;
                    while (l < 1024) {
                        if (vm_mem_read(&c->mem, strs + so + l, sp + l, 1) != 0) { ok = 0; break; }
                        if (strs[so + l] == 0) break;
                        l++;
                    }
                    if (!ok || so + l + 1 > sizeof(strs)) { ok = 0; break; }
                    dst[i] = strs + so;
                    so += l + 1;
                    (*cnt)++;
                }
            }
            if (!ok) { e0 = -14; break; }
            gav[na] = NULL; gev[ne] = NULL;
            VmLaunch L;
            const Elf64_Ehdr* teh = (const Elf64_Ehdr*)ino->map;
            vm_log("[4.2dbg] execve: ET_DYN=%u interp=%s", teh->e_type, "…");
            if (teh->e_type == ET_DYN) {
                /* 4.2：动态 PIE——找 PT_INTERP，ROM 里取真 linker，内核角色全演 */
                const Elf64_Phdr* tph = (const Elf64_Phdr*)(ino->map + teh->e_phoff);
                const char* ipath = NULL;
                for (int i = 0; i < teh->e_phnum && !ipath; i++)
                    if (tph[i].p_type == PT_INTERP)
                        ipath = (const char*)(ino->map + tph[i].p_offset);
                const VmRomInode* ld = NULL;
                if (ipath && ipath + strlen(ipath) < (const char*)ino->map + ino->size
                    && (ld = vm_rom_find(c->vfs->rom, ipath)) != NULL) {
                    vm_set_exec_path(path);
                    vm_set_exec_cmdline(gav);   /* /proc/self/cmdline */   /* /proc/self/exe 数据源 */
                    int rr = vm_loader_exec_dyn(ino->map, ino->size, ld->map, ld->size,
                                                 path, gav, gev, &L);
                    vm_log("[4.2dbg] exec_dyn ret=%d", rr);
                    if (rr == 0) {
                        e0 = 0;
                    } else { e0 = -8; break; }
                } else { vm_log("[4.2dbg] exec_dyn 未执行：interp=%s ld=%s", ipath ? ipath : "(null)", ld ? "ok" : "ROM缺"); e0 = -2; break; }
            } else {
                vm_set_exec_path(path);
                if (vm_loader_exec(ino->map, ino->size, gav, gev, &L) != 0) { e0 = -8; break; }
            }
#if defined(__x86_64__)
            gx[REG_RIP] = (greg_t)L.entry;
            gx[REG_RSP] = (greg_t)L.sp;
            gx[REG_RAX] = 0;
#else
            uc->uc_mcontext.pc = (unsigned long)L.entry;
            uc->uc_mcontext.sp = (unsigned long)L.sp;
            rx[0] = 0;
#endif
            if (tls_swapped) VM_TLS_SET(saved_tls);
            return;
        } while (0);
        /* execve 失败：写 -errno 正常返回（execve 失败会返回） */
#if defined(__x86_64__)
        gx[REG_RAX] = (greg_t)e0;
#else
        rx[0] = (unsigned long)e0;
#endif
        if (tls_swapped) VM_TLS_SET(saved_tls);
        return;
    }


#if defined(__x86_64__)
    greg_t* g = uc->uc_mcontext.gregs;
    long a0 = g[REG_RDI], a1 = g[REG_RSI], a2 = g[REG_RDX], a3 = g[REG_R10];
    long a4 = g[REG_R8],  a5 = g[REG_R9];
    long ret = vm_handle_syscall(c, nr, a0, a1, a2, a3, a4, a5);
    g[REG_RAX] = (greg_t)ret;
#else
    uint64_t* r = uc->uc_mcontext.regs;
    /* 同步投递时 r[8] 与 si_syscall 一致；不一致说明现场已串味，一律以内核号为准 */
    long ret = vm_handle_syscall(c, nr, (long)r[0], (long)r[1], (long)r[2],
                                 (long)r[3], (long)r[4], (long)r[5]);
    r[0] = (unsigned long)ret;
#endif
    if (tls_swapped) VM_TLS_SET(saved_tls);   /* 恢复本线程进入时的 TLS 基址 */
}

int vm_sigsys_install(int console_fd) {
    VM_TLS_GET(&g_libc_tls);   /* 宿主 TLS 基址必须在 guest 抢占前捕获 */
    if (vm_filter_check_passthrough() != 0)     /* 直发清单 ⊆ BPF 放行清单（v0.2.8 铁律） */
        vm_log("警告：过滤器与判决表不一致，guest 线程/直发路径可能二次 TRAP");
    /* v0.7.80：给 SIGSYS handler 一个**独立的大栈**。
     * 真机 v0.7.79 实锤：handler 只成功进入过一次（HND#1），第二次就崩 ——
     * 因为此前没建 altstack、也没 SA_ONSTACK，SIGSYS 处理直接跑在 **guest 的应用栈**上
     * （1MB，而且 guest 正在用它）。handler 里任何稍大的局部对象都会把它撑爆。
     * 这里挂 8MB 独立栈：既解决问题，也让 handler 可以放心用几 KB 的临时缓冲。 */
    static char altstack_mem[8 * 1024 * 1024];
    stack_t ss;
    memset(&ss, 0, sizeof(ss));
    ss.ss_sp = altstack_mem;
    ss.ss_size = sizeof(altstack_mem);
    ss.ss_flags = 0;
    if (sigaltstack(&ss, NULL) != 0) return -errno;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = vm_sigsys_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;   /* ★ 用上面那块 8MB 栈 */
    if (sigaction(SIGSYS, &sa, NULL) != 0) return -errno;
    int rc = vm_seccomp_install(console_fd);
    if (rc != 0) return rc;
    /* v0.7.87 自检：装完过滤器后**立即触发一次 TRAP**，验证 handler 真的会被调用。
     * 真机 v0.7.85/86 的症状是"过滤器生效后所有 TRAP 都没有 handler"，
     * 这一测能一刀切开"过滤器没生效 / 信号没投递 / handler 有问题"三种可能。
     * 用一个必然 TRAP 且无副作用的 syscall：faccessat(48)（不在放行清单里）。 */
    {
        /* v0.7.88：自检前先把"当前 SIGSYS 处置/掩码/altstack"读回来打日志 ——
         * 这一测能区分"处置被换掉""被屏蔽""altstack 无效"三种情形。 */
        {
            struct sigaction cur;
            memset(&cur, 0, sizeof(cur));
            int ra = sigaction(SIGSYS, NULL, &cur);
            stack_t curss;
            memset(&curss, 0, sizeof(curss));
            int rs = sigaltstack(NULL, &curss);
            sigset_t cm;
            sigemptyset(&cm);
            int rm = sigprocmask(SIG_SETMASK, NULL, &cm);
            vm_log("[TwinVM] 自检前状态：sigaction=%d handler=%p flags=0x%x | "
                   "altstack=%d sp=%p size=%zu | 掩码读=%d SIGSYS被屏蔽=%d",
                   ra, (void*)cur.sa_sigaction, (unsigned)cur.sa_flags,
                   rs, curss.ss_sp, (size_t)curss.ss_size,
                   rm, sigismember(&cm, SIGSYS));
        }
        /* v0.7.89 ★★★ 用引擎自己的解释器**评估刚装进内核的那份程序**对 48 的判决。
         * 这一测把"过滤器判 ALLOW"与"信号没投递"彻底分开：
         *   判 TRAP  → 过滤器没问题，是投递链；判 ALLOW → 程序本身就不拦 48。 */
        {
            extern int vm_last_prog_len_dummy;
            /* 复用 vm_seccomp_install 里那份 prog：这里重新构造一份同源的做评估 */
            struct sock_filter prog2[] = VM_BPF_INSTRUCTIONS;
            size_t n2 = sizeof(prog2) / sizeof(prog2[0]);
            long act48 = vm_bpf_eval(prog2, n2, 48, VM_ARCH_AUDIT, 0, (uint32_t)console_fd, 0);
            long act_log = vm_bpf_eval(prog2, n2, 64, VM_ARCH_AUDIT, 1, (uint32_t)console_fd, 0);
            long act_fut = vm_bpf_eval(prog2, n2, 98, VM_ARCH_AUDIT, 0, (uint32_t)console_fd, 0);
            vm_log("[TwinVM] 自检前 BPF 评估：48(faccessat)=0x%lx  64(write,fd=1)=0x%lx  98(futex)=0x%lx"
                   "   [TRAP=0x30000 ALLOW=0x7fff0000 KILL=0x80000000]",
                   act48, act_log, act_fut);
        }
        /* v0.7.90 ★ 读 /proc/self/status 的 Seccomp / Seccomp_filters 字段 ——
         * 这是**内核对自己过滤器状态的权威陈述**：
         *   Seccomp: 0=disabled 1=strict 2=filter
         *   Seccomp_filters: 已叠加的过滤器**层数**
         * 若 Seccomp=2 且 filters>=1 → 我们的过滤器确实生效（问题投递链）；
         * 若 Seccomp=2 且 filters>=2 → **有别的层叠在我们上面**（真凶！）；
         * 若 Seccomp=0/1         → prctl 其实没生效（安装被忽略）。 */
        /* v0.7.92 ★ 教训：**引擎自己不能在过滤器生效后调未放行的 syscall**！
         * v0.7.91 我在这里调 prctl(PR_GET_SECCOMP)（21），而 prctl 自 v0.7.83 起
         * 已被移出 BPF 放行清单 —— 于是这条"自检"自己触发了 TRAP，
         * 在 handler 里再次调用引擎函数 → 二次 TRAP/死锁 → 后面的自检 TRAP
         * 根本没机会执行（真机日志：BPF 评估行之后什么都没有）。
         * 结论：**自检必须放在过滤器安装之前**（或只做纯内存判断）。 */
        /* v0.7.93 ★ 决定性实验：nefarious "我们的 TRAP 是否被外层 zygote 策略吞掉"。
         * 装完过滤器后分别试调：
         *   (a) faccessat(48)：**不在我们放行清单**，应 TRAP → 看 handler 是否被调用；
         *   (b) write(1, …)：在我们放行清单（console 特判），应 ALLOW → 必然成功；
         *   (c) futex(98)：放行清单内 → ALLOW。
         * 若 (b)(c) 正常而 (a) 永远无 handler 且进程随后消失 → 证实
         * **外层策略把我们的 TRAP 变成了 KILL/忽略**（架构性冲突）。 */
        {
            extern int prctl(int, ...);
            vm_log("[TwinVM] 自检：马上触发一次 TRAP（faccessat）…");
        }
#if defined(__aarch64__)
        /* v1.1.3：自检 TRAP 仅 arm64 有意义（x86_64 本机跑离线回归用不到）。
         * 此前无守卫 → x86_64 宿主编译报 invalid register name（v0.7.93 引入的
         * 移植性回归；同文件其余 8 处均有 __x86_64__/__aarch64__ 双守卫）。 */
        register long x0 __asm__("x0") = -100;      /* AT_FDCWD */
        register long x1 __asm__("x1") = 0;
        register long x2 __asm__("x2") = 0;
        register long x8 __asm__("x8") = 48;        /* faccessat */
        __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
        vm_log("[TwinVM] 自检：TRAP 返回 x0=%ld —— handler 工作正常", (long)x0);
#else
        vm_log("[TwinVM] 自检：非 arm64 宿主，跳过 TRAP 实验项");
#endif
    }
    return 0;
}
