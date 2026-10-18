#!/usr/bin/env python3
# 生成 TwinVM BPF 过滤器（安全原语放行清单 + 文件 I/O TRAP）
# 输出 core/src/bpf_gen.h（vm_backend_sigsys.c 直接 include）
import os

X86 = {
    'read': 0, 'write': 1, 'close': 3, 'fstat': 5, 'lseek': 8, 'mmap': 9,
    'mprotect': 10, 'munmap': 11, 'brk': 12, 'rt_sigaction': 13,
    'rt_sigprocmask': 14, 'ioctl': 16, 'pread64': 17, 'writev': 20,
    'mremap': 25, 'madvise': 28, 'exit': 60, 'wait4': 61, 'kill': 62,
    'uname': 63, 'fcntl': 72, 'getppid': 110, 'getcwd': 79, 'clone': 56, 'getpid': 39,
    'exit_group': 231, 'gettid': 186, 'futex': 202, 'set_tid_address': 218,
    'clock_gettime': 228, 'getrandom': 318, 'arch_prctl': 158,
    'rt_sigtimedwait': 428,   # v1.2.53：x86 侧对称（arm64 137 的对齐项）
    'sigaltstack': 131, 'tgkill': 234, 'sched_yield': 24, 'prctl': 157,
    'epoll_create1': 291, 'epoll_ctl': 233, 'epoll_wait': 232, 'epoll_pwait': 281, 'socket': 41,
    'connect': 42,
    'pipe2': 317, 'getcpu': 309, 'sched_setaffinity': 203, 'set_robust_list': 273, 'rseq': 334,
    'nanosleep': 35, 'clock_nanosleep': 230,
    'membarrier': 324, 'prlimit64': 302, 'gettimeofday': 96, 'userfaultfd': 323,
    'process_vm_readv': 310, 'process_vm_writev': 311,
    'sched_getaffinity': 203, 'rt_sigreturn': 15,
}
ARM = {
    'getcwd': 17, 'ioctl': 29, 'fcntl': 25, 'getdents64': 61, 'lseek': 62,
    'read': 63, 'write': 64, 'writev': 66, 'close': 57, 'pread64': 67, 'exit': 93,
    'exit_group': 94, 'set_tid_address': 96, 'futex': 98, 'kill': 129,
    'rt_sigaction': 134, 'rt_sigprocmask': 135, 'sigaltstack': 132,
    'rt_sigtimedwait': 137,   # v1.2.53：ART Signal Catcher sigwait
    'tgkill': 131, 'clone': 220, 'sched_yield': 124, 'sched_getaffinity': 123,
    'uname': 160, 'getpid': 172, 'getppid': 173, 'gettid': 178, 'brk': 214, 'munmap': 215,
    'mremap': 216, 'mmap': 222, 'mprotect': 226, 'clock_gettime': 113,
    'getrandom': 278, 'prctl': 167, 'madvise': 233, 'rt_sigreturn': 139,
    'nanosleep': 101, 'clock_nanosleep': 115,
    'epoll_create1': 20, 'epoll_ctl': 21, 'epoll_pwait': 22, 'pread64': 67,
    'pipe2': 59, 'getcpu': 168, 'sched_setaffinity': 122, 'set_robust_list': 99, 'rseq': 293,
    'socket': 198, 'connect': 203,
    'membarrier': 283, 'prlimit64': 261, 'gettimeofday': 169, 'userfaultfd': 282,
    'process_vm_readv': 270, 'process_vm_writev': 271,
    'wait4': 260,
}

# 安全原语直达清单（guest 无需虚拟化的内核服务——半虚拟化分层）
#
# 铁律（v0.2.8 修复）：**判决表里任何"直发内核"(VM_PT*)的 syscall 都必须在这张放行清单里**。
# 原因：SIGSYS handler 里用裸 svc 直发同样会被本过滤器再判一次；若该 syscall 未被放行，
# 就会在 SIGSYS 被阻塞的状态下二次 TRAP —— 内核不执行该 syscall、信号挂起，等 guest 自己
# 恢复信号掩码后才"迟到"投递，此时 ucontext 已不是系统调用现场（寄存器是别处的垃圾），
# 用户态内核会拿垃圾参数执行一次完全无关的 syscall；更糟的是可能形成 handler 自递归。
# bionic 的 pthread_create 恰好在 clone 前后 sigprocmask(屏蔽全部信号)，
# 线程入口 __start_thread 的 gettid 与 __exit(线程退出 exit) 正好落在这个窗口里 ——
# 这就是 Stage 1 自检在 pthread 一步 "code=-31（SIGSYS）"、且控制台不再打 trap#N 的根因。
#   * exit      ：bionic __exit() 用它做线程退出；放行 = 该线程真的退出（而不是整个槽位）
#   * gettid    ：bionic __start_thread 第一件事；必须返回真实 tid，
#                 伪造 tid 会让 __pthread_internal_find()/pthread_kill 失去身份
#   * exit_group：guest 关机
ALWAYS = ['exit_group', 'rt_sigreturn', 'exit', 'gettid', 'getpid', 'getppid', 'brk', 'munmap',
          'mprotect', 'mremap', 'madvise', 'futex', 'set_tid_address',
          # v0.7.83 **移除 prctl**：guest（bionic）会用它安装自己的 seccomp 过滤器
          #   prctl(PR_SET_NO_NEW_PRIVS=38) + prctl(PR_SET_SECCOMP=22, MODE_FILTER, &fprog)
          # 真机实锤（v0.7.81/82）：放行它 → guest 那层策略（KILL_PROCESS）装到我们
          # 之上（内核里后装的排前面）→ 此后每个 TRAP 第一条就 KILL_PROCESS，
          # 我们的 handler 永远进不去（HND#/TRAP#/[HR] 全零、黑匣子 seq=0）。
          # 摘掉后 prctl 会 TRAP 进 handler，由引擎"假装成功"地吞掉这两个请求。
          'rt_sigprocmask', 'sigaltstack', 'kill', 'tgkill',
          'rt_sigtimedwait',   # v1.2.53：ART Signal Catcher 的 sigwait（arm64=137）
          'wait4', 'clone', 'clock_gettime', 'getrandom', 'sched_yield',
          'sched_getaffinity',
          'membarrier', 'prlimit64', 'gettimeofday', 'nanosleep', 'clock_nanosleep',
          'userfaultfd',   # v0.7.42：ART CC GC/FaultHandler 要它（真机日志：ENOSYS nr=282 → 之后静默卡死）
          'epoll_create1', 'epoll_ctl', 'epoll_wait', 'epoll_pwait',
          'pipe2', 'getcpu', 'sched_setaffinity',
          # v0.7.62：fcntl 从 ALWAYS 除名 → fd 参数化（见 fd_blocks）。
          # v0.7.61 真机实锤：libziparchive 打开 boot classpath jar 后用
          # fcntl(fd, F_GETFD) 体检 fd——放行清单直发把虚拟 fd(10000+)交给内核
          # → EBADF ×5 → jar 全部读废 → "Image component does not match"
          # → ClassLinker CHECK abort（code=127）。
          # 真机实测（v0.7.48 策略探针，fork 子进程逐个试调）——以下 11 条被
          # SECCOMP_RET_KILL_PROCESS 直接杀（绕过 handler 与屏蔽位，表现为
          # "无任何 TRAP 行 + code=-31"）：
          #   rseq(293) mbind(235) get_mempolicy(236) set_mempolicy(237)
          #   migrate_pages(238) move_pages(239) keyctl(250) add_key(248)
          #   request_key(249) openat2(437) faccessat2(439)
          # v0.7.52 补充（全量放行清单审计实测）：set_robust_list(99) 也在 KILL 名单里！
          #   bionic **每个线程**都会调它 → 曾导致 ART 在某次线程初始化后"无痕 code=-31"。
          # 其中 rseq 曾被本清单放行 —— ART/bionic 每个新线程都会调它，于是
          # 槽位必死。摘掉后它会 TRAP → 引擎回 -ENOSYS，bionic 按"内核不支持
          # rseq"正常退化。
          # v0.7.22：引擎自身在 handler 内用它们做安全访存（vm_mem_read/vm_mem_write），
          # guest 传野指针时只会 -EFAULT 而不是 handler 内 SIGSEGV 崩槽位。
          # Android app seccomp 策略允许（见 bionic SYSCALLS.TXT，不在 APP blocklist）。
          # v0.7.76 **移除** process_vm_readv/writev：
          # 它们原本是 v0.7.22 为了让"引擎在 handler 内安全访存"而放行的，
          # 但那个机制早已废弃（引擎现在用自己的 vm_mem_read/vm_mem_write）。
          # 残留的后果极其隐蔽：guest（ART）调 process_vm_readv 时**绕过 handler
          # 直达内核**，于是我们新写的 VM 语义实现永远不执行，ART 读到的内容
          # 与它的地址空间假设不符 —— 表现为 abort 时日志/原因行全都递不出来。
          # 摘掉后它们会 TRAP → 走引擎实现（vectored 拷贝）。
          ]
# 注意：mmap/pread64/rt_sigaction 不在此表——参数化（fd 路由/sig==31 保护）（见 emit）：fd 落虚拟区间才 TRAP，其余放行。
# 真文件 fd 误落区间时 handler 里 is_binder 复查兜底（宁可多 TRAP，不可漏截）。


def emit_notif(nr_table, arch_const, has_arch_prctl, notif_actions):
    """v0.8.0：**SECCOMP_RET_USER_NOTIF 版过滤器**。
    为什么换机制：真机实测（v0.7.92/93）App 进程**天生带 zygote seccomp 过滤器**，
    而我们想要 `SECCOMP_RET_TRAP` 的那些号里，很多**外层判 KILL_PROCESS**
    （内核多层取最严动作）→ 进程被直接 KILL、我们的 handler 永远进不去。
    `SECCOMP_RET_USER_NOTIF` 同样是"用户态内核"，但它对**外层判 ALLOW 的号**
    可以正常拦截；而且它不依赖信号投递。
    生成规则：ALLOW 清单内的号 → RET ALLOW；
              需要监听的号（虚拟化对象）→ RET USER_NOTIF；
              其余 → RET TRAP（保持旧行为，用于没被外层限制的号）或 ALLOW。
    """
    lines = []
    prog = []
    prog.append(('LD_ARCH', 4))
    prog.append(('JEQ_ARCH', arch_const, None, 'KILL'))
    # 无条件放行清单
    always = [n for n in ALWAYS if n in nr_table] + (['arch_prctl'] if has_arch_prctl else [])
    for name in always:
        prog.append(('LD', 0, None, None))
        prog.append(('JEQ', nr_table[name], None, 'ALLOW'))
    # USER_NOTIF 集合（需要用户态处理的号）
    for n in notif_actions:
        prog.append(('LD', 0, None, None))
        prog.append(('JEQ', n, None, 'NOTIF'))
    prog.append(('RET', 'SECCOMP_RET_ALLOW', None, None))
    prog.append(('RET', 'SECCOMP_RET_USER_NOTIF', None, None))
    prog.append(('RET', 'SECCOMP_RET_TRAP', None, None))
    prog.append(('RET', 'SECCOMP_RET_KILL_PROCESS', None, None))
    ret_idx = {}
    for i, ins in enumerate(prog):
        if ins[0] == 'RET':
            ret_idx[ins[1]] = i
    allow_last = max(i for i, ins in enumerate(prog) if ins[0] == 'RET' and ins[1] == 'SECCOMP_RET_ALLOW')
    notif_last = max(i for i, ins in enumerate(prog) if ins[0] == 'RET' and ins[1] == 'SECCOMP_RET_USER_NOTIF')
    out = []
    for i, ins in enumerate(prog):
        if ins[0] == 'LD_ARCH':
            out.append('        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 4),')
        elif ins[0] == 'LD':
            out.append(f'        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, {ins[1]}),')
        elif ins[0] == 'JEQ_ARCH':
            dst = ret_idx['SECCOMP_RET_KILL_PROCESS']
            out.append(f'        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, {ins[1]}, 0, {dst - (i + 1)}),')
        elif ins[0] == 'JEQ':
            tgt = ins[3]
            if tgt == 'NOTIF':
                dst = notif_last
                out.append(f'        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, {ins[1]}, {dst - (i + 1)}, 0),')
            else:
                dst = allow_last
                out.append(f'        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, {ins[1]}, {dst - (i + 1)}, 0),')
        elif ins[0] == 'RET':
            out.append(f'        BPF_STMT(BPF_RET | BPF_K, {ins[1]}),')
    return '\n'.join(out), len(prog)


def emit(nr_table, arch_const, has_arch_prctl):
    always = [n for n in ALWAYS if n in nr_table] + (['arch_prctl'] if has_arch_prctl else [])
    write_nr = nr_table['write']
    mmap_nr = nr_table['mmap']
    lines = []
    # 0: LD arch; 1: JEQ arch → +0 / →KILL（偏移最后 patch）
    lines.append('        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 4),')
    # 指令构建（占位跳转，稍后回填）
    prog = [('LD_ARCH', 4)]
    prog.append(('JEQ_ARCH', arch_const, None, 'KILL'))   # arch 检查：匹配→继续，不匹配→KILL
    for name in always:
        prog.append(('LD', 0, None, None))
        prog.append(('JEQ', nr_table[name], None, 'ALLOW'))
    # ── 4.0：fd 参数化四件套（真 fd 原生放行——handler 直发会二次 TRAP = v0.2.8 铁律违反，
    # 真机 v0.5.0 实锤：write(pipe_fd) TRAP→handler 直发→再 TRAP（SIGSYS 阻塞态）→code=-31 死）。
    # 语义：真内核 fd（pipe/epoll 等，3..9999）由 BPF 直接 ALLOW——guest 原生执行，不经 handler；
    #       console（write 直写；read/close/ioctl 保护性 TRAP）、stdio(0,1,2) 与虚拟区间(10000+) → TRAP。
    fd_blocks = []
    # v0.7.66：write 的 3..9999 从 ALLOW 改 TRAP——logd socket(fd 5) 上的 write
    # 藏着 ART 的 abort 原因行（v0.7.65 真机实锤：trace 显示原因走 write(5)，
    # 原生放行=不可见）。TRAP 后 handler tee 到 console + 假装成功（v0.7.19
    # writev 同款先例；不直发=无二次 TRAP 风险）。console 管道 fd 由安装期
    # CONSOLE 补丁特判 ALLOW——vm_console_write 的宿主侧写不受影响。
    for sname, cons_tgt in [('write', 'ALLOW'), ('read', 'TRAP'), ('close', 'TRAP'),
                            ('ioctl', 'TRAP'), ('fcntl', 'ALLOW')]:   # v0.7.62：console 是真管道 fd，fcntl 原生无害
        real_fd_tgt = 'TRAP' if sname == 'write' else 'ALLOW'   # v0.7.66：write 真 fd 也 TRAP（tee）
        if sname not in nr_table:
            continue
        fd_blocks.append([
            ('LD', 0, None, None),
            ('JEQ', nr_table[sname], None, 'FD_BLK'),       # 命中→顺序查 fd；未命中→跳下一块
            ('LD', 16, None, None),                          # args[0] lo32 = fd
            ('JEQ', 'CONSOLE', None, cons_tgt),              # console 特判
            ('JGE', 10000, None, 'TRAP'),                    # fd≥10000（虚拟区间）→TRAP
            ('JGT', 2, None, real_fd_tgt),                   # 3..9999：write→TRAP(tee)；其余→ALLOW 原生
            ('RET', 'SECCOMP_RET_TRAP', None, None),         # 0,1,2→TRAP（VM stdio 语义）
        ])
    # v0.7.16：socket 参数化——AF_UNIX(1) → ALLOW 原生直通（本地 IPC：stats/logd
    # 探测；connect 到不存在路径拿 ENOENT 无害）；其它族 → 顺序落 TRAP（handler 报
    # EACCES，VM 无真网络栈——Stage 5 halbox 桥再做）。
    # 铁律注意：socket 一律不在 handler 直发（v0.7.16 真机 -31 死实锤）。
    if 'socket' in nr_table:
        prog.append(('LD', 0, None, None))
        prog.append(('JEQ', nr_table['socket'], None, 'FD_BLK'))
        prog.append(('LD', 16, None, None))               # a0 = family
        prog.append(('JEQ', 1, None, 'ALLOW_LAST'))       # AF_UNIX → 尾部主 ALLOW
        prog.append(('RET', 'SECCOMP_RET_TRAP', None, None))  # 其它族 → 本块 TRAP（顺序落）
    # v0.7.24：connect 参数化——真 fd(3..9999) → ALLOW 原生（AF_UNIX：logd/statsd 必须能连上，
    # ART 的第一条日志就走这里）；虚拟 fd(≥10000)/stdio → TRAP 进 handler（只回错误，绝不直发）。
    # 铁律：handler 里**不许**用裸 svc 直发 connect（真机 2026-10-13 17:09 实锤：
    #   vm_core 里 VM_PT3(203) 直发 → 自家过滤器二次 TRAP → SIGSYS 挂起 → code=-31）。
    if 'connect' in nr_table:
        fd_blocks.append([
            ('LD', 0, None, None),
            ('JEQ', nr_table['connect'], None, 'FD_BLK'),
            ('LD', 16, None, None),
            ('JEQ', 'CONSOLE', None, 'TRAP'),
            ('JGE', 10000, None, 'TRAP'),
            ('JGT', 2, None, 'ALLOW'),
            ('RET', 'SECCOMP_RET_TRAP', None, None),
        ])
    # v0.7.6：rt_sigaction(sig==SIGSYS=31) → TRAP（保护处理器——厂商 linker 的
    # flowctrl 反调试会重置信号处置；伪造成功，VM handler 不动）
    if 'rt_sigaction' in nr_table:
        prog.append(('LD', 0, None, None))
        prog.append(('JEQ', nr_table['rt_sigaction'], None, 'FD_BLK'))
        prog.append(('LD', 16, None, None))               # a0 = sig
        # v0.7.53：sig==31 继续往下查 act；其它信号直接原生放行
        prog.append(('JEQ', 31, None, 'SIGSYS_CHK'))
        prog.append(('RET', 'SECCOMP_RET_ALLOW', None, None))  # 其它信号原生
        prog.append(('LD', 24, None, None))               # a1 = act（NULL=纯查询）
        prog.append(('JEQ', 0, None, 'ALLOW_LAST'))       # 纯查询 → 放行（引擎据此自查 handler）
        prog.append(('RET', 'SECCOMP_RET_TRAP', None, None))   # 安装企图 → 拦截（引擎伪造成功）
    # pread64：fd<10000（真 fd）→ ALLOW 原生；≥10000（虚拟）→ TRAP 进 handler（ROM 读）
    if 'pread64' in nr_table:
        prog.append(('LD', 0, None, None))
        prog.append(('JEQ', nr_table['pread64'], None, 'FD_BLK'))
        prog.append(('LD', 16, None, None))                # args[0]=fd（v0.7.5 修复：漏了这条）
        prog.append(('JGE', 10000, None, 'PREAD_TRAP'))   # 虚拟 fd → TRAP
        prog.append(('RET', 'SECCOMP_RET_ALLOW', None, None))  # 真 fd 原生
    for blk in fd_blocks:
        prog.extend(blk)
    # ── 3.1：mmap 参数化放行（真内核同款 fd 路由）──
    prog.append(('LD', 0, None, None))
    prog.append(('JEQ', mmap_nr, None, 'MMAP_CHK'))    # 命中 mmap→顺序查fd；未命中→默认 TRAP
    prog.append(('LD', 48, None, None))                 # args[4] lo32 = fd（LP64 LE）
    prog.append(('JGE', 10000, None, 'ALLOW'))          # fd < VM_FD_BASE(10000) → ALLOW
    prog.append(('JGT', 10000 + 64 - 1, None, 'ALLOW')) # fd > 10063 → ALLOW；区间内 → 顺序 RET TRAP
    prog.append(('RET', 'SECCOMP_RET_TRAP', None, None))
    prog.append(('RET', 'SECCOMP_RET_ALLOW', None, None))
    # v0.7.56 铁律改版：末位默认判决 KILL_PROCESS → TRAP。
    # 真机 v0.7.55 实锤：未知 syscall 走 KILL_PROCESS = 无声死（handler 不运行、
    # 黑匣子无 pc、日志无 TRAP 行）——ART 读完 /proc/self/maps 后的下一个 syscall
    # 就这样死了，死因完全不可见。改为 TRAP 后：
    #   1. syscall 依然**绝不进入内核**（TRAP = seccomp 拦下后交 handler）——防逃逸不降级；
    #   2. 落进 vm_handle_syscall 的 default → ENOSYS + 大声日志（含参数）；
    #   3. ART 对 ENOSYS 一贯优雅退化（membarrier 先例）。
    # 唯一仍判 KILL 的是第 2 条指令的 arch 不匹配（铁律测试锚点）。
    prog.append(('RET', 'SECCOMP_RET_TRAP', None, None))
    # arch 不匹配专用 KILL 真身（v0.7.56：唯一允许 KILL_PROCESS 的判决；
    # 正常流到不了这里——它前面的 TRAP 兜底一切未知 syscall。
    # 保留 KILL 而非 TRAP：非本机架构号=框架性异常，无 handler 可救）
    prog.append(('RET', 'SECCOMP_RET_KILL_PROCESS', None, None))

    # 回填跳转偏移
    # 找 RET 位置
    ret_idx = {}
    for i, ins in enumerate(prog):
        if ins[0] == 'RET':
            ret_idx[ins[1]] = i
    # 尾部主 RET（prog 末三连：TRAP/ALLOW/KILL）——跨块跳转的唯一合法目标。
    # 注意 ret_idx 被中间块覆盖，尾部真身 = 最后一次出现的位置：
    allow_last_idx = max(i for i, ins in enumerate(prog)
                         if ins[0] == 'RET' and ins[1] == 'SECCOMP_RET_ALLOW')
    trap_last_idx = max(i for i, ins in enumerate(prog)
                         if ins[0] == 'RET' and ins[1] == 'SECCOMP_RET_TRAP')
    kill_last_idx = max(i for i, ins in enumerate(prog)
                         if ins[0] == 'RET' and ins[1] == 'SECCOMP_RET_KILL_PROCESS')
    mmap_blk = None
    for i, ins in enumerate(prog):
        if ins[0] == 'LD' and ins[1] == 48:
            mmap_blk = i - 1   # JEQ mmap 那条
            break
    # fd 参数化块起点表（FD_BLK 的 jf 目标 = 下一块起点；末块跳 mmap 块）
    fd_starts = []
    for i, ins in enumerate(prog):
        if ins[0] == 'JEQ' and ins[3] == 'FD_BLK':
            fd_starts.append(i)   # 块内 JEQ 位置；块起点 = i-1（LD 0）
    out = []
    for i, ins in enumerate(prog):
        if ins[0] == 'LD_ARCH':
            out.append('BPF_STMT(BPF_LD | BPF_W | BPF_ABS, 4),')
        elif ins[0] == 'LD':
            out.append(f'BPF_STMT(BPF_LD | BPF_W | BPF_ABS, {ins[1]}),')
        elif ins[0] == 'JEQ_ARCH':
            dst = ret_idx['SECCOMP_RET_KILL_PROCESS']
            jt = dst - (i + 1)
            out.append(f'BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, {ins[1]}, 0, {jt}),')
        elif ins[0] == 'JEQ' and ins[3] == 'MMAP_CHK':
            # 命中 mmap → 顺序查 fd；未命中 → 默认 TRAP
            dst = ret_idx['SECCOMP_RET_TRAP']
            jf = dst - (i + 1)
            out.append(f'BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, {ins[1]}, 0, {jf}),')
        elif ins[0] == 'JEQ' and ins[3] == 'FD_BLK':
            # 命中→顺序查 fd（jt=0）；未命中→跳下一 fd 块起点（末块→mmap 块）
            bi = fd_starts.index(i)
            nxt = fd_starts[bi + 1] - 1 if bi + 1 < len(fd_starts) else mmap_blk
            jf = nxt - (i + 1)
            out.append(f'BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, {ins[1]}, 0, {jf}),')
        elif ins[0] == 'JEQ' and ins[3] == 'ALLOW_LAST':
            dst = allow_last_idx          # 尾部主 ALLOW（唯一真身，中间块内 ALLOW 不可跳）
            jt = dst - (i + 1)
            out.append(f'BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, {ins[1]}, {jt}, 0),')
        elif ins[0] == 'JEQ' and ins[3] == 'SIGSYS_CHK':
            # sig==31 → 落到下一条（继续查 act）；否则 → 本块末尾的 RET ALLOW
            skip_to = None
            for k in range(i + 1, len(prog)):
                if prog[k][0] == 'RET' and prog[k][1] == 'SECCOMP_RET_ALLOW':
                    skip_to = k
                    break
            # v0.7.54 修正：JEQ 后面紧跟的就是 RET ALLOW，所以
            #   sig==31 → 必须**跳过**那条 RET ALLOW（jt = skip_to+1-i-1）
            #   sig!=31 → 直接落下（jf=0）命中 RET ALLOW
            # 之前把 jf 当成"跳到 RET ALLOW"算成了 0，等于两条路都落进 ALLOW ——
            # 真机后果：链接器把自己的 SIGSYS handler 装上了，槽位第 14 个 syscall 就死。
            jt = (skip_to + 1) - (i + 1)
            out.append(f'BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, {ins[1]}, {jt}, 0),')
        elif ins[0] == 'JEQ' and ins[3] == 'SIGSYS_TRAP':
            dst = trap_last_idx           # 尾部主 TRAP（防中间块 RET 误跳）
            jt = dst - (i + 1)
            out.append(f'BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, {ins[1]}, {jt}, 0),')
        elif ins[0] == 'JEQ':
            val, target = ins[1], ins[3]
            dst = ret_idx['SECCOMP_RET_' + ('KILL_PROCESS' if target == 'KILL' else
                              'ALLOW' if target == 'ALLOW' else 'TRAP')]
            # JEQ val, jt(命中跳 dst), jf(落到 next=0)
            jt = dst - (i + 1)
            if isinstance(val, str):
                v = val  # arch 常量字符串
            else:
                v = str(val)
            if val == 'CONSOLE':
                v = '(uint32_t)console_fd'   # 4.0：console 值统一转运行时 fd（不限目标）
            elif target == 'KILL':
                v = 'AUDIT_ARCH_X86_64' if 'X86' in str(ins[1]) else 'AUDIT_ARCH_AARCH64'
                # arch 常量由 emit 参数给；这里退回用 arch_const
            out.append(f'BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, {v}, {jt}, 0),')
        elif ins[0] == 'JGE':
            if ins[3] == 'PREAD_TRAP':
                dst = ret_idx['SECCOMP_RET_TRAP']
                jt = dst - (i + 1)
                out.append(f'BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, {ins[1]}, {jt}, 0),')
            elif ins[3] == 'ALLOW':
                # mmap 语义：a<k（真 fd）→ ALLOW；a≥k → 顺序（下一检查）
                dst = ret_idx['SECCOMP_RET_ALLOW']
                jf = dst - (i + 1)
                out.append(f'BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, {ins[1]}, 0, {jf}),')
            else:
                # fd 块语义：a≥k（虚拟区间）→ TRAP；a<k → 顺序（JGT 检查）
                dst = ret_idx['SECCOMP_RET_TRAP']
                jt = dst - (i + 1)
                out.append(f'BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, {ins[1]}, {jt}, 0),')
        elif ins[0] == 'JGT':
            # JGT k：真（fd>k）→ ins[3] 指定目标（'ALLOW' 原生 / v0.7.66 'TRAP'=write tee）；
            # 假 → 顺序落（RET TRAP / 下一检查）
            tgt = ins[3] if ins[3] in ('TRAP', 'ALLOW') else 'ALLOW'
            dst = ret_idx['SECCOMP_RET_' + tgt]
            jt = dst - (i + 1)
            out.append(f'BPF_JUMP(BPF_JMP | BPF_JGT | BPF_K, {ins[1]}, {jt}, 0),')
        elif ins[0] == 'RET':
            out.append(f'BPF_STMT(BPF_RET | BPF_K, {ins[1]}),')
    return ' \\\n'.join(out), len(prog), always


def allow_table(nr_table, always):
    """放行清单（供运行时/离线一致性自检用）：{ 'name': nr, ... }"""
    return [(n, nr_table[n]) for n in always]


x86_code, x86_n, x86_always = emit(X86, 'AUDIT_ARCH_X86_64', True)

# ── v0.8.0：生成 **USER_NOTIF 版** 过滤器 ──
# 集合来源：vm_core.c 里引擎有专门 case 分支的 syscall（自动提取，避免手写出错）。
import re as _re
_core = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'src', 'vm_core.c'),
             'rb').read().decode('utf-8', 'replace')
_arm_defs, _x86_defs = {}, {}
_sec = None
for _l in _core.splitlines():
    if 'aarch64' in _l.lower() and 'define' not in _l:
        _sec = 'arm'
    m = _re.match(r'#define\s+(VM_NR_\w+)\s+(\d+)', _l.strip())
    if m:
        if _sec == 'arm':
            _arm_defs[m.group(1)] = int(m.group(2))
        else:
            _x86_defs.setdefault(m.group(1), int(m.group(2)))
_cases = set(_re.findall(r'case\s+(VM_NR_\w+)', _core)) | set(_re.findall(r'case\s+(\d+):', _core))
_named = [n for n in _cases if n.startswith('VM_NR_')]
_lit = [int(n) for n in _cases if n.isdigit()]
# v0.8.6：**判决表直发集合**对应的 arm64/x86 号（引擎里是 VM_PT* 裸 svc）。
# 这些号绝不能让监听线程执行（会自我挂起 → 死锁；真机 v0.8.5 实锤：futex）。
# 来源：vm_core.c 的 vm_passthrough_nrs[]（24 项），号表按 arch 手写对齐。
_PT_ARM_SET = {94, 139, 93, 178, 214, 222, 215, 226, 216, 233, 98, 96,
               135, 132, 129, 131, 260, 220, 113, 278, 124, 123, 67,
               137}   # v1.2.53：rt_sigtimedwait（Signal Catcher sigwait）
_PT_X86_SET = {231, 15, 60, 186, 12, 9, 11, 10, 25, 28, 202, 218,
               14, 131, 62, 234, 61, 56, 228, 318, 24, 203, 17, 158}

_NOTIF_EXTRA_ARM = [221, 281]   # execve / execveat
_NOTIF_EXTRA_X86 = [59, 322]
# v0.8.6 ★ 显式"需要用户态内核处理"的号（文件 I/O + 进程信息 + 目录），
# 这是**权威来源** —— 之前从 case 标签自动提取会漏掉 `case 56:` 这类数字形式。
# v0.8.6 ★ 注意：**不要包含 process_vm_readv/writev(270/271)** ——
# 引擎在监听线程里用它们做安全访存（vm_mem_read/write）。若被挂起，
# 监听线程会自己阻塞 → 所有 guest 线程死等（真机 v0.8.5：t797 卡在 271，
# 主线程 t787 的 futex 永远等不到）。
_VFS_ARM = [56, 57, 62, 63, 64, 66, 67, 78, 79, 80, 89, 160, 17, 49, 47,   # v1.2.53：137 移出（改透传）
            48, 29, 61, 25, 198, 203, 172, 174, 175, 176, 177, 178, 96, 99,
            163, 167, 43, 44, 269, 260]
_VFS_X86 = [2, 3, 8, 0, 1, 20, 17, 89, 79, 5, 262, 267, 63, 78, 80, 21,
            16, 56, 41, 42, 39, 102, 104, 107, 108, 186, 273, 97, 157, 137,
            231, 217, 257, 260]
# v0.8.7 ★★★ **监听线程安全集**：这些 syscall 是监听线程自己工作所必需的，
# **绝不能进 NOTIF 集合** —— 否则监听线程一发就被自己挂起，整个 VM 立刻停摆。
#   ioctl(29/16)：NOTIF_RECV / NOTIF_SEND 本体（最致命）
#   read/write：日志与调试输出
#   close：清理
#   futex(98/202)：pthread 内部锁（监听线程/引擎都用）
#   mmap/munmap/mprotect：栈与内存
#   rt_sigprocmask/sigaltstack/rt_sigaction：信号基础设施
#   exit/exit_group
#   ★ ioctl(29/16) 是唯一"监听线程必然调用"的号（NOTIF_RECV/SEND 本体）——
#     必须移出；其余（read/write/close…）由"监听线程不主动调用"来保证，
#     保留在 NOTIF 里才能让 guest 的 write-tee / 虚拟 fd read 继续工作。
_LISTENER_SAFE_ARM = {29}
_LISTENER_SAFE_X86 = {16}
NOTIF_ARM = sorted((set(_VFS_ARM) | set(_NOTIF_EXTRA_ARM)) - _PT_ARM_SET - _LISTENER_SAFE_ARM)
NOTIF_X86 = sorted((set(_VFS_X86) | set(_NOTIF_EXTRA_X86)) - _PT_X86_SET - _LISTENER_SAFE_X86)

x86_notif_code, x86_notif_n = emit_notif(X86, 'AUDIT_ARCH_X86_64', True, NOTIF_X86)
arm_notif_code, arm_notif_n = emit_notif(ARM, 'AUDIT_ARCH_AARCH64', False, NOTIF_ARM)
arm_code, arm_n, arm_always = emit(ARM, 'AUDIT_ARCH_AARCH64', False)


def emit_allow_macro(always, nr_table):
    pairs = ', '.join(f'{{"{n}", {nr_table[n]}}}' for n in always)
    return '{ ' + pairs + ' }'


x86_allow = emit_allow_macro(x86_always, X86)
arm_allow = emit_allow_macro(arm_always, ARM)

c = f'''/* BPF 过滤器（生成于 2026-10-12 Stage1，gen_bpf.py）——请勿手改
 * 分层：安全原语直达内核（半虚拟化——同 VMOS），文件 I/O TRAP 进用户态内核，
 * write(console) 直通避免 handler 递归。共 x86_64={x86_n} / arm64={arm_n} 条
 *
 * v0.2.8：exit（线程退出）/ gettid（线程入口）加入放行清单 —— 判决表里所有
 * VM_PT* 直发内核的 syscall 必须出现在 VM_BPF_ALLOW_NRS 里，否则 handler 内
 * 直发会二次 TRAP（内核不执行 + 信号挂起 + 迟到投递乱判决）。
 * v0.7.22：引擎自身在 handler 内调用的 syscall（process_vm_readv/writev）同样
 * 必须在放行清单里（vm_engine_syscall_nrs 自检）。
 * v0.8.0：新增 VM_BPF_NOTIF_INSTRUCTIONS（SECCOMP_RET_USER_NOTIF 后端）。 */
#if defined(__x86_64__)
#define VM_BPF_ALLOW_NRS_COUNT {len(x86_always)}
#define VM_BPF_INSTRUCTIONS {{ \\
{x86_code} \\
}}
/* 放行清单（名字, 号）——运行时一致性自检 + 离线回归测试使用 */
#define VM_BPF_ALLOW_NRS {x86_allow}
#else
#define VM_BPF_INSTRUCTIONS {{ \\
{arm_code} \\
}}
/* v0.8.0：USER_NOTIF 版（同上） */
#define VM_BPF_NOTIF_INSTRUCTIONS {{ \\
{arm_notif_code} \\
}}
/* 放行清单（名字, 号）——运行时一致性自检 + 离线回归测试使用 */
#define VM_BPF_ALLOW_NRS_COUNT {len(arm_always)}
#define VM_BPF_ALLOW_NRS {arm_allow}
#endif
'''

out_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'src', 'bpf_gen.h')
# v0.8.0：NOTIF 段补续行符（只处理 BPF_ 行；'}' 原样保留）
_lines = c.split('\n')
_out, _in_notif = [], False
for _l in _lines:
    if _l.startswith('#define VM_BPF_NOTIF_INSTRUCTIONS'):
        _in_notif = True
        _out.append(_l)
        continue
    if _in_notif and (_l.startswith('#define') or _l.startswith('#endif')
                      or _l.startswith('#if') or _l.startswith('/*')):
        _in_notif = False
        _out.append(_l)
        continue
    if _in_notif and _l.strip().startswith('BPF_'):
        _clean = _l.rstrip().rstrip('\\').rstrip()
        # 宏体最后一行也需要续行符 —— 否则紧随其后的 '}' 会落到宏外
        _out.append(_clean + ' \\')
        continue
    _out.append(_l)
c = '\n'.join(_out)
open(out_path, 'w').write(c)
print(f'x86_64: {x86_n} insns, arm64: {arm_n} insns -> {out_path}')
print(f'放行清单: x86_64 {len(x86_always)} 项 / arm64 {len(arm_always)} 项')
for n, nr in allow_table(ARM, arm_always):
    print(f'  arm64 放行 {n} = {nr}')
