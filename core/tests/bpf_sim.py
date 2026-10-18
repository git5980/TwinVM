#!/usr/bin/env python3
"""TwinVM 过滤器 / 判决表一致性回归测试（离线，不需要 seccomp）。

两件事：
 1. 仿真 bpf_gen.h 生成的 seccomp-BPF，逐条核对判决动作（ALLOW / TRAP / KILL）。
 2. **铁律检查**：vm_core.c 判决表里所有"直发内核"(VM_PT*)的 syscall，必须落在
    BPF 放行清单里。原版违反了这条：exit(93) 被 TRAP，但 handler 里当直发用
    → handler 内裸 svc 二次 TRAP → SIGSYS 挂起 → 迟到投递乱判决
    → Stage 1 自检在 pthread 一步 code=-31（SIGSYS）。（x86_64 的 prctl 同样漏了。）

用法: python3 tests/bpf_sim.py [core 目录，默认本文件上级]
"""
import os
import re
import sys

CORE = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else
                       os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
HDR = os.path.join(CORE, 'src', 'bpf_gen.h')
COREC = os.path.join(CORE, 'src', 'vm_core.c')

RET_KILL_PROCESS = 0x80000000
RET_USER_NOTIF = 0x7fc00000   # v0.8.0：SECCOMP_RET_USER_NOTIF
RET_TRAP = 0x00030000
RET_ALLOW = 0x7fff0000
AUDIT_ARCH_AARCH64 = 0xC00000B7
AUDIT_ARCH_X86_64 = 0xC000003E

ARM = {  # 号表（与 gen_bpf.py 的 ARM 一致），仅用于打印人话
    94: 'exit_group', 139: 'rt_sigreturn', 93: 'exit', 178: 'gettid', 214: 'brk',
    222: 'mmap', 215: 'munmap', 226: 'mprotect', 216: 'mremap', 233: 'madvise',
    98: 'futex', 96: 'set_tid_address', 167: 'prctl', 134: 'rt_sigaction',
    135: 'rt_sigprocmask', 132: 'sigaltstack', 129: 'kill', 131: 'tgkill',
    260: 'wait4', 220: 'clone', 113: 'clock_gettime', 278: 'getrandom',
    124: 'sched_yield', 123: 'sched_getaffinity', 67: 'pread64', 25: 'fcntl',
    64: 'write', 56: 'openat', 63: 'read', 57: 'close', 79: 'newfstatat',
    80: 'fstat', 61: 'getdents64', 160: 'uname', 172: 'getpid', 48: 'faccessat',
    99: 'set_robust_list', 293: 'rseq',
}
X86 = {94: 'exit_group', 15: 'rt_sigreturn', 60: 'exit', 186: 'gettid', 12: 'brk',
       9: 'mmap', 11: 'munmap', 10: 'mprotect', 25: 'mremap', 28: 'madvise',
       202: 'futex', 218: 'set_tid_address', 157: 'prctl', 13: 'rt_sigaction',
       14: 'rt_sigprocmask', 131: 'sigaltstack', 62: 'kill', 234: 'tgkill',
       61: 'wait4', 56: 'clone', 228: 'clock_gettime', 318: 'getrandom',
       24: 'sched_yield', 203: 'sched_getaffinity', 17: 'pread64', 72: 'fcntl',
       158: 'arch_prctl', 1: 'write', 257: 'openat', 0: 'read', 3: 'close',
       262: 'newfstatat', 5: 'fstat', 217: 'getdents64', 39: 'getpid',
       269: 'faccessat', 334: 'rseq'}
WRITE_NR = {'aarch64': 64, 'x86_64': 1}
# Stage 2.1：getpid/getppid 从 TRAP(伪造=1) 改为 ALLOW(真实 pid——多进程 wait4 一致性)
TRAP_PROBE = {'aarch64': [56, 63, 57, 79, 80, 61, 160, 48, 64, 64],
              'x86_64': [257, 0, 3, 262, 5, 217, 63, 269, 1]}   # rseq/set_robust_list 4.0 起放行（pthread 必需）


def _arch_body(text, arch):
    m = re.search(r'#if defined\(__x86_64__\)(.*?)#else(.*?)#endif', text, re.S)
    return m.group(1) if arch == 'x86_64' else m.group(2)


def load_program(arch):
    body = _arch_body(open(HDR).read(), arch)
    prog = []
    for line in body.splitlines():
        line = line.strip().rstrip('\\').strip().rstrip(',')
        m2 = re.match(r'BPF_STMT\((.*)\)$', line)
        m3 = re.match(r'BPF_JUMP\((.*)\)$', line)
        if m2:
            code, k = [x.strip() for x in m2.group(1).split(',')]
            c = 0
            for part in code.split('|'):
                c |= {'BPF_LD': 0x00, 'BPF_RET': 0x06, 'BPF_W': 0x00, 'BPF_ABS': 0x20,
                      'BPF_K': 0x00}[part.strip()]
            if k == 'SECCOMP_RET_TRAP': k = RET_TRAP
            elif k == 'SECCOMP_RET_ALLOW': k = RET_ALLOW
            elif k == 'SECCOMP_RET_KILL_PROCESS': k = RET_KILL_PROCESS
            elif k == 'SECCOMP_RET_USER_NOTIF': k = RET_USER_NOTIF
            else: k = int(k, 0)
            prog.append((c, 0, 0, k))
        elif m3:
            code, k, jt, jf = [x.strip() for x in m3.group(1).split(',')]
            c = 0x05
            for part in code.split('|'):
                c |= {'BPF_JMP': 0x05, 'BPF_JEQ': 0x10, 'BPF_JGT': 0x20, 'BPF_JGE': 0x30,
                      'BPF_K': 0x00}[part.strip()]
            if k == 'AUDIT_ARCH_X86_64': k = AUDIT_ARCH_X86_64
            elif k == 'AUDIT_ARCH_AARCH64': k = AUDIT_ARCH_AARCH64
            elif k == '(uint32_t)console_fd': k = 'CONSOLE'
            else: k = int(k, 0)
            prog.append((c, int(jt), int(jf), k))
    return prog


def allow_names(arch):
    body = _arch_body(open(HDR).read(), arch)
    m = re.search(r'#define VM_BPF_ALLOW_NRS \{(.*)\}', body, re.S)
    if not m:
        return None            # 旧版生成物（没有机器可读放行清单）
    return dict(re.findall(r'\{"([a-z_0-9]+)",\s*(\d+)\}', m.group(1)))


def nr_defines(arch):
    text = open(COREC).read()
    m = re.search(r'#if defined\(__x86_64__\)(.*?)#elif defined\(__aarch64__\)(.*?)#else', text, re.S)
    body = m.group(1) if arch == 'x86_64' else m.group(2)
    return {name: int(num) for name, num in re.findall(r'#define\s+VM_NR_(\w+)\s+(\d+)', body)}


def passthrough_names(arch):
    text = open(COREC).read()
    i = text.index('vm_passthrough_nrs[] = {')
    body = text[i:text.index('};', i)]
    inner = re.search(r'#if defined\(__x86_64__\)(.*?)#endif', body, re.S)
    if inner:
        body = body.replace(inner.group(0), inner.group(1) if arch == 'x86_64' else '')
    # v0.7.76：表清空后只剩哨兵（-1 或 0），此时自然解析出空表。
    # ★ 不要用 re.sub 去"清理数字"——那会把 VM_NR_wait4 / VM_NR_pread64 里的
    #   4 / 64 一起吃掉，变成 wait / pread（踩过一次）。
    names = re.findall(r'VM_NR_([A-Za-z_0-9]+)', body)
    return names


def all_passthrough_nrs(arch):
    """扫 vm_core.c 里**所有** VM_PT*(...) 直发的第一个参数（VM_NR_名 or 数字字面量）。
    这是 v0.7.24 的关键升级：只用 vm_passthrough_nrs[] 手维护表会漏——
    2026-10-13 17:09 真机就是"判决表里加了 connect(203) 直发、表里没登记"，
    自检静默通过 → 二次 TRAP → code=-31。"""
    text = open(COREC).read()
    defs = nr_defines(arch)
    defs_all = {}
    for a in ('x86_64', 'aarch64'):
        try: defs_all.update(nr_defines(a))
        except Exception: pass
    # 别名（#define VM_NR_pread64 VM_NR_pread64_x86）也要能解析
    aliases = dict(re.findall(r'#define\s+VM_NR_(\w+)\s+VM_NR_(\w+)', text))
    # 只扫调用点，跳过宏定义本身（#define VM_PT3(nr,a,b,c) ...）
    calls = [l for l in text.splitlines() if 'VM_PT' in l and not l.lstrip().startswith('#define')]
    raw = []
    for l in calls:
        raw += re.findall(r'VM_PT[3-6]\(\s*([A-Za-z_0-9]+)', l)
    out = []
    for a in raw:
        if a.isdigit():
            out.append(int(a)); continue
        name = a[6:] if a.startswith('VM_NR_') else a
        seen = set()
        while name in aliases and name not in seen:
            seen.add(name); name = aliases[name]
        if name in defs:
            out.append(defs[name])
        elif name in defs_all:
            continue                      # 另一个架构的宏（如 x86_64 的 arch_prctl）→ 本架构跳过
        else:
            out.append(('UNRESOLVED', a))
    return out


def engine_syscall_names(arch):
    """vm_engine_syscall_nrs[]（引擎自身在 handler 内调用的 syscall）"""
    text = open(COREC).read()
    i = text.index('vm_engine_syscall_nrs[] = {')
    body = text[i:text.index('};', i)]
    return re.findall(r'VM_NR_([A-Za-z_0-9]+)', body)


def run(prog, nr, arch, args, console_fd):
    a, pc = 0, 0
    while pc < len(prog):
        code, jt, jf, k = prog[pc]
        cls = code & 0x07
        if cls == 0x00:
            if k == 0: a = nr
            elif k == 4: a = arch
            elif k == 16: a = (args[0] if len(args) > 0 else 0) & 0xFFFFFFFF
            elif k == 24: a = (args[1] if len(args) > 1 else 0) & 0xFFFFFFFF   # v0.7.54：rt_sigaction args[1]=act
            elif k == 32: a = (args[2] if len(args) > 2 else 0) & 0xFFFFFFFF
            elif k == 48: a = (args[4] if len(args) > 4 else 0) & 0xFFFFFFFF   # 3.1：mmap args[4]=fd
            else: raise ValueError('unsupported load offset %r' % k)
            pc += 1
        elif cls == 0x05:
            kk = console_fd if k == 'CONSOLE' else k
            op = code & 0xf0
            if op == 0x10:   cond = (a == kk)
            elif op == 0x20: cond = (a > kk)
            elif op == 0x30: cond = (a >= kk)
            else: raise ValueError('unsupported jmp op 0x%x' % op)
            pc += 1 + (jt if cond else jf)
        elif cls == 0x06:
            return k
        else:
            raise ValueError('unsupported insn 0x%x at %d' % (code, pc))
    raise ValueError('program ran off the end')


def check(arch, fails, verbose=True):
    table = ARM if arch == 'aarch64' else X86
    prog = load_program(arch)
    arch_const = AUDIT_ARCH_AARCH64 if arch == 'aarch64' else AUDIT_ARCH_X86_64
    console_fd = 7
    names = allow_names(arch)
    pn = passthrough_names(arch)
    defs = nr_defines(arch)

    print(f'── {arch}: 过滤器 {len(prog)} 条指令, 放行 {0 if names is None else len(names)} 项, '
          f'判决表直发 {len(pn)} 项')
    if names is None:
        fails.append(f'{arch}: bpf_gen.h 缺 VM_BPF_ALLOW_NRS（旧版生成物）—— 请重跑 gen_bpf.py')

    if run(prog, 172, 0x40000028, [0] * 6, console_fd) != RET_KILL_PROCESS:
        fails.append(f'{arch}: 架构不匹配未返回 KILL_PROCESS')
    for name, nr in (names or {}).items():
        if run(prog, int(nr), arch_const, [0] * 6, console_fd) != RET_ALLOW:
            fails.append(f'{arch}: 放行清单 {name}({nr}) 实际不是 ALLOW')
    write_nr = WRITE_NR[arch]
    if run(prog, write_nr, arch_const, [console_fd, 0, 8], console_fd) != RET_ALLOW:
        fails.append(f'{arch}: write(console_fd) 期望 ALLOW')
    if run(prog, write_nr, arch_const, [1, 0, 8], console_fd) != RET_TRAP:
        fails.append(f'{arch}: write(fd=1) 期望 TRAP')
    for nr in TRAP_PROBE[arch]:
        if run(prog, nr, arch_const, [0] * 6, console_fd) != RET_TRAP:
            fails.append(f'{arch}: {table.get(nr, nr)}({nr}) 期望 TRAP')

    # 4.0：fd 参数化四件套（真 fd 3..9999 → ALLOW 原生；console/stdio/虚拟区间 → TRAP）
    for nm, fn in [('write', lambda: write_nr), ('read', lambda: defs['read']),
                   ('close', lambda: defs['close']), ('ioctl', lambda: defs['ioctl'])]:
        snr = fn()
        if nm == 'write':
            # v0.7.66：write 真 fd 也 TRAP（tee 到 console——logd 的 abort 原因行可见）
            if run(prog, snr, arch_const, [5, 0, 8], console_fd) != RET_TRAP:
                fails.append(f'{arch}: write(pipe fd=5) 期望 TRAP（v0.7.66 tee 语义）')
        else:
            if run(prog, snr, arch_const, [5, 0, 0], console_fd) != RET_ALLOW:
                fails.append(f'{arch}: {nm}(fd=5) 期望 ALLOW（原生直通）')
            if run(prog, snr, arch_const, [console_fd, 0, 0], console_fd) != RET_TRAP:
                fails.append(f'{arch}: {nm}(console) 期望 TRAP（保护 console）')
        if run(prog, snr, arch_const, [1, 0, 0], console_fd) != RET_TRAP:
            fails.append(f'{arch}: {nm}(fd=1 stdio) 期望 TRAP')
        if run(prog, snr, arch_const, [10000, 0, 0], console_fd) != RET_TRAP:
            fails.append(f'{arch}: {nm}(fd=10000 虚拟) 期望 TRAP')

    # 3.1：mmap 参数化路由（fd∈[VM_FD_BASE=10000, +64) → TRAP；否则 ALLOW）
    mmap_nr = defs['mmap']
    if run(prog, mmap_nr, arch_const, [0, 0, 0, 0, 10000, 0], console_fd) != RET_TRAP:
        fails.append(f'{arch}: mmap(fd=10000 虚拟 binder) 期望 TRAP')
    if run(prog, mmap_nr, arch_const, [0, 0, 0, 0, 100, 0], console_fd) != RET_ALLOW:
        fails.append(f'{arch}: mmap(fd=100 普通 pipe 级) 期望 ALLOW')
    if run(prog, mmap_nr, arch_const, [0, 0, 0, 0, -1, 0], console_fd) != RET_ALLOW:
        fails.append(f'{arch}: mmap(fd=-1 ANON) 期望 ALLOW')

    # 铁律：判决表直发集合 ⊆ BPF 放行清单
    for n in pn:
        if n not in defs:
            fails.append(f'{arch}: vm_passthrough_nrs 里有无法解析的名字 {n}')
            continue
        nr = defs[n]
        act = run(prog, nr, arch_const, [0] * 6, console_fd)
        if verbose:
            print(f'   {"OK " if act == RET_ALLOW else "!! "}直发 {n:20s} nr={nr:3d} -> '
                  f'{"ALLOW" if act == RET_ALLOW else hex(act)}')
        if act != RET_ALLOW:
            fails.append(f'{arch}: 判决表直发 {n}({nr}) 未被 BPF 放行（0x{act:08x}）'
                         f' —— handler 内裸 svc 会二次 TRAP（Stage 1 code=-31 同型故障）')

    # v0.7.24 铁律 3：源码里**每一处** VM_PT* 直发都必须被放行（自动扫描，不依赖手维护表）
    # 参数化规则（socket/connect/mmap/pread64/write/rt_sigaction）按各自合法参数取值：
    # 只要存在一组参数被判 ALLOW，就说明"过滤器认识这条 syscall"，其余非法参数由
    # v0.7.24 的运行时护栏（vm_syscall6 直发前查 vm_filter_action）兜住。
    arg_sets = [[5, 0, 0, 0, -1, 0],      # 真 fd / mmap 匿名
                [1, 0, 0, 0, -1, 0],      # AF_UNIX
                [3, 0, 0, 0, 5, 0],       # 真 fd / mmap 真 fd
                [-1, 0, 0, 0, -1, 0]]     # AT_FDCWD 等
    auto = all_passthrough_nrs(arch)
    bad_auto = []
    for nr in auto:
        if isinstance(nr, tuple):
            bad_auto.append(f'无法解析的直发参数 {nr[1]}')
            continue
        best = None
        for aset in arg_sets:
            best = run(prog, nr, arch_const, aset, console_fd)
            if best == RET_ALLOW:
                break
        if best != RET_ALLOW:
            bad_auto.append(f'直发 nr={nr}（{table.get(nr, "?")}）任何合法参数下都不是 ALLOW'
                            f'（最好 0x{best:08x}）')
    if bad_auto:
        for b in bad_auto:
            fails.append(f'{arch}: 源码直发未放行 —— {b}（handler 内裸 svc 会二次 TRAP）')
    if verbose:
        print(f'   自动扫描源码 VM_PT* 直发 {len(auto)} 处，未放行 {len(bad_auto)} 处')

    # v0.7.54 铁律 5：**SIGSYS 处置绝不能被 guest 改掉**（这是 VM 的命根子）。
    # 真机 v0.7.53 实锤：改 rt_sigaction 规则时把跳转偏移算错一位，结果
    # "安装 SIGSYS handler"也变成放行 → 链接器立刻装了自己的处理器 →
    # 槽位在第 14 个 syscall 就 code=-31 死掉。这里离线钉死四种组合。
    _prog = load_program(arch)
    _ac = 0xC00000B7 if arch == 'aarch64' else 0xC000003E
    _cfd = 99
    _sigact = 134 if arch == 'aarch64' else 13
    for _nm, _nr, _args, _exp in [
        ('rt_sigaction(sig=31,act=NULL)', _sigact, [31, 0, 0, 8, 0, 0], RET_ALLOW),
        ('rt_sigaction(sig=31,act=ptr)',  _sigact, [31, 0x1234, 0, 8, 0, 0], RET_TRAP),
        ('rt_sigaction(sig=11,act=ptr)',  _sigact, [11, 0x1234, 0, 8, 0, 0], RET_ALLOW),
        ('rt_sigaction(sig=11,act=NULL)', _sigact, [11, 0, 0, 8, 0, 0], RET_ALLOW),
    ]:
        _act = run(_prog, _nr, _ac, _args, _cfd)
        if _act != _exp:
            fails.append(f'{arch}: {_nm} 判决 0x{_act:08x}，期望 0x{_exp:08x}'
                         f' —— SIGSYS 保护被破坏（真机会无痕 code=-31）')
        elif verbose:
            print(f'   OK {_nm:30s} -> {"TRAP" if _act == RET_TRAP else "ALLOW"}')
    # v0.7.49 铁律 4：App 的 seccomp 策略 KILL（SECCOMP_RET_KILL_PROCESS）的 syscall
    # 一律不得放行 —— 真机实测（v0.7.48 策略探针）：命中即"无 TRAP 行 + code=-31"。
    APP_KILLED = {
        'rseq': (293, 334), 'set_robust_list': (99, 273), 'mbind': (235, 237), 'get_mempolicy': (236, 239),
        'set_mempolicy': (237, 238), 'migrate_pages': (238, 256),
        'move_pages': (239, 279), 'keyctl': (250, 250), 'add_key': (248, 248),
        'request_key': (249, 249), 'openat2': (437, 437), 'set_robust_list': (99, 273), 'faccessat2': (439, 439),
    }
    # 头文件里有两份（x86_64 在前、arm64 在后）——必须按架构挑对，否则号会串台
    _cands = []
    for _l in open(HDR):
        _t = _l.strip()
        if _t.startswith('*') or _t.startswith('/*') or _t.startswith('//'):
            continue                      # 注释里也会出现这个名字（踩过一次）
        if 'VM_BPF_ALLOW_NRS' in _l and '{"' in _l:
            _cands.append(_l)
    _mark = 270 if arch == 'aarch64' else 310      # process_vm_readv 的架构号
    kill_line = ''
    for _l in _cands:
        if re.search(r'\{"process_vm_readv",\s*%d\}' % _mark, _l):
            kill_line = _l
            break
    if not kill_line and _cands:
        kill_line = _cands[-1] if arch == 'aarch64' else _cands[0]
    kill_pairs = re.findall(r'\{"(\w+)",\s*(\d+)\}', kill_line)
    kill_names = set(n for n, _ in kill_pairs)
    kill_nums = set(int(v) for _, v in kill_pairs)
    bad_kill = []
    for name, (arm, x86) in APP_KILLED.items():
        if name in kill_names or arm in kill_nums or x86 in kill_nums:
            bad_kill.append(f'{name}(arm {arm})')
    if bad_kill:
        fails.append(f'{arch}: App 策略 KILL 的 syscall 被放行 —— {", ".join(bad_kill)}'
                     f'（真机会直接 code=-31）')
    if verbose:
        print(f'   App-KILL 名单检查: {len(APP_KILLED)} 条，违规 {len(bad_kill)}')
    # v0.7.56 铁律 5：未知 syscall 一律 TRAP（可诊断），绝不 ALLOW（逃逸）也绝不
    # KILL_PROCESS（无声死——v0.7.55 真机实锤：ART 读完 maps 后下一个 syscall
    # 无声阵亡，死因不可见）。取若干"肯定没实现"的号抽查。
    for probe_nr in (0, 42, 191, 512, 999):
        act = run(prog, probe_nr, arch_const, [0] * 6, console_fd)
        if act != RET_TRAP:
            fails.append(f'{arch}: 未知 nr={probe_nr} 判决 0x{act:08x}，期望 TRAP'
                         f'（ALLOW=逃逸 / KILL=无声死，都违规）')
    if verbose:
        print('   未知 syscall → TRAP 检查: 5 个抽查号全过')
    # v0.7.22 铁律 2：引擎**自身**在 handler 内调用的 syscall（vm_engine_syscall_nrs，
    # 目前是 process_vm_readv/writev —— vm_mem_read/write 的安全访存）也必须放行，
    # 否则"野指针防护"本身会在 handler 里二次 TRAP，反而崩槽位。
    # v0.7.76：引擎已不再使用任何自用 syscall（表为空）；-1 哨兵直接跳过。
    en = engine_syscall_names(arch)
    if not en:
        if verbose:
            print('   -- 引擎自用 syscall 表为空（v0.7.76：不再需要）')
    for n in en:
        if n not in defs:
            fails.append(f'{arch}: vm_engine_syscall_nrs 里有无法解析的名字 {n}')
            continue
        nr = defs[n]
        act = run(prog, nr, arch_const, [0] * 6, console_fd)
        if verbose:
            print(f'   {"OK " if act == RET_ALLOW else "!! "}引擎自用 {n:18s} nr={nr:3d} -> '
                  f'{"ALLOW" if act == RET_ALLOW else hex(act)}')
        if act != RET_ALLOW:
            fails.append(f'{arch}: 引擎自用 {n}({nr}) 未被 BPF 放行（0x{act:08x}）'
                         f' —— handler 内安全访存会二次 TRAP')

    other = 'x86_64' if arch == 'aarch64' else 'aarch64'
    onames = allow_names(other)
    if names and onames:
        extra = set(names) - set(onames) - {'arch_prctl', 'epoll_wait'}   # x86 独有 epoll_wait（arm64 用 epoll_pwait）
        miss = set(onames) - set(names) - {'arch_prctl', 'epoll_wait'}
        if extra or miss:
            fails.append(f'{arch}: 与 {other} 的放行清单不一致 多={sorted(extra)} 少={sorted(miss)}')
    print()


def main():
    fails = []
    for arch in ('aarch64', 'x86_64'):
        check(arch, fails)
    if fails:
        print('FAIL:')
        for f in fails:
            print('  -', f)
        return 1
    print('PASS: 过滤器判决与判决表一致；所有 VM_PT* 直发 syscall 都在放行清单里')
    return 0


if __name__ == '__main__':
    sys.exit(main())
