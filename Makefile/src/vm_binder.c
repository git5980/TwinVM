/* TwinVM Stage 3 —— binder 用户态驱动（VMOS halbox 等价物的第一块）。
 *
 * 范围（3.0 教学闭环）：guest 两进程经用户态内核交换一次 binder 事务。
 *   client: ioctl(BINDER_WRITE_READ){write: BC_TRANSACTION, target.handle=0}
 *   server: ioctl(BINDER_WRITE_READ){read: BR_TRANSACTION} → 处理 → BC_REPLY
 *   client: 第二次 ioctl read → BR_REPLY
 * 真 uapi 面：BINDER_WRITE_READ 命令码 / binder_write_read 布局 /
 * BC_TRANSACTION/BR_TRANSACTION/BC_REPLY/BR_REPLY 码全按内核 uapi（LP64）。
 * 简化面（3.1 TODO）：mmap 共享缓冲零拷贝、完整 binder_node/ref 对象图、futex 阻塞。
 *
 * 跨进程状态：MAP_SHARED|MAP_ANONYMOUS 区在首次访问时创建；guest 内
 * fork（clone 直达）继承同一物理页——sigsys 后端每进程的 handler 操作的是
 * 同一块"内核全局区"（真内核全局性的用户态等价物）。ptrace 后端的 dispatch
 * 全在 supervisor 进程内跑，堆区天然共享。 */
#define _GNU_SOURCE
#include "vm_core.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

/* ── 真 uapi（kernel uapi/linux/android/binder.h 摘录，LP64） ── */
#define BINDER_WRITE_READ     _IOWR('b', 1, 64)   /* struct binder_write_read */
#define BINDER_SET_CONTEXT_MGR _IOW('b', 7, 4)

#define BC_TRANSACTION 0x40406300 + 0  /* _IOW('c',0,...)：码面取低 32 位协议值 */
#define BC_REPLY        0x40406301 + 0
#define BR_TRANSACTION  0x40406200 + 2  /* _IOR('r',2,...) */
#define BR_REPLY        0x40406200 + 3
#define BR_NOOP         0x40406200 + 12

/* 协议值直接用 canonical 常量（libbinder 里就是这些数） */
#undef BC_TRANSACTION
#undef BC_REPLY
#undef BR_TRANSACTION
#undef BR_REPLY
#undef BR_NOOP
#define BC_TRANSACTION _IOW('c', 0, 64)   /* binder_transaction_data 64B */
#define BC_REPLY       _IOW('c', 1, 64)
#define BR_TRANSACTION _IOR('r', 2, 64)
#define BR_REPLY       _IOR('r', 3, 64)
#define BR_NOOP        _IOR('r', 12, 4)

/* struct binder_write_read（LP64 布局，8B 对齐） */
typedef struct {
    uint64_t write_size;       /* 0 */
    uint64_t write_consumed;   /* 8 */
    uint64_t write_buffer;     /* 16 */
    uint64_t read_size;        /* 24 */
    uint64_t read_consumed;    /* 32 */
    uint64_t read_buffer;      /* 40 */
} VmBwr;

/* binder_transaction_data（LP64）——我们按 uapi 64B 布局搬运 */
typedef struct {
    uint32_t target_handle;    /* 0  union target.handle */
    uint32_t pad0;             /* 4  */
    uint64_t cookie;           /* 8 */
    uint64_t code;             /* 16 */
    uint64_t flags;             /* 24 */
    int64_t  sender_pid;       /* 32 */
    uint64_t data_size;        /* 40 */
    uint64_t offsets_size;     /* 48 */
    uint64_t data_buffer;      /* 56 union ptr.buffer（内嵌缓冲指针） */
} VmTxnData;

/* ── VM 内核全局区（MAP_SHARED，guest fork 继承） ── */
#define VM_BINDER_MAX_TXN 4096
typedef struct VmBinderTxn {
    volatile int32_t state;     /* 0=free 1=queued 2=replied 3=consumed */
    uint32_t code;
    int32_t  from_pid;
    uint64_t cookie;
    uint32_t data_len;
    uint32_t data_in_mmap;      /* 3.1：1=data 留 mmap 段原地（零拷贝），data_buffer 有效 */
    const uint8_t* data_ptr;    /*    零拷贝时指向共享区 mmap 段 */
    uint8_t  data[VM_BINDER_MAX_TXN];
} VmBinderTxn;

/* 3.1：guest mmap(/dev/binder) 拿到的零拷贝事务缓冲段（sigsys 后端=共享区
 * 原地址直通，同地址空间跨进程可见；ptrace 后端无映射，mmap 返回 -ENODEV
 * 由 guest 降级拷贝路径）。真 binder 同款语义：binder_alloc 的 mmap 区。 */
#define VM_BINDER_MMAP_SLOTS 4
#define VM_BINDER_MMAP_SZ   4096

#define VM_BINDER_QSLOTS 32
typedef struct VmBinderState {
    volatile int32_t mgr_pid;                 /* context manager 的 guest pid */
    volatile uint32_t q_event;                /* 队列事件字：q_push 后 ++（server 阻塞于此） */
    volatile uint32_t rep_event;              /* 回复事件字：BC_REPLY 后 ++（client 阻塞于此） */
    volatile uint32_t mmap_hint;              /* mmap 段轮转分配游标 */
    VmBinderTxn  txns[VM_BINDER_QSLOTS];      /* 事务池（索引即 handle） */
    volatile uint32_t q_head, q_tail;         /* FIFO 环形队列存 txn 索引+1 */
    uint32_t      q_items[VM_BINDER_QSLOTS];
    uint8_t       mmaps[VM_BINDER_MMAP_SLOTS][VM_BINDER_MMAP_SZ];
} VmBinderState;

/* futex word 全部 4B 对齐（futex 硬性要求），且必须落在 MAP_SHARED 区内
 * ——跨进程 wait/wake 才作用于同一物理字。 */

static VmBinderState* vm_binder_state(VmCpu* c) {
    static VmBinderState* s_b;
    if (!s_b) {
        void* p = mmap(NULL, sizeof(VmBinderState), PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) return NULL;
        memset(p, 0, sizeof(VmBinderState));
        s_b = (VmBinderState*)p;
        __sync_synchronize();
    }
    (void)c;
    return s_b;
}

/* guest openat("/dev/binder") 命中：VFS 占位 + is_binder 标记由调用方设置 */
int vm_binder_open(VmCpu* c) {
    VmVfs* vfs = c->vfs;
    for (int i = 0; i < VM_MAX_FILES; i++) {
        if (!vfs->files[i].in_use) {
            vfs->files[i].in_use = 1;
            vfs->files[i].is_dir = 0;
            vfs->files[i].rom_idx = -1;           /* 非 ROM：binder 设备 */
            vfs->files[i].is_binder = 1;
            vfs->files[i].pos = 0;
            vfs->files[i].dir_path[0] = 0;
            return VM_FD_BASE + i;
        }
    }
    return -12 /* ENOMEM */;
}

static int txn_alloc(VmBinderState* b) {
    for (int i = 0; i < VM_BINDER_QSLOTS; i++)
        if (__sync_bool_compare_and_swap(&b->txns[i].state, 0, 1)) {
            b->txns[i].data_in_mmap = 0;   /* 复用清零拷贝标志（CAS 后本方独占） */
            b->txns[i].data_ptr = 0;
            return i;
        }
    return -1;
}

/* 把一个 entry 挂到 mgr 的队列（写锁：单生产者场景 demo 够用，多生产者 3.1 上 CAS 环） */
static int q_push(VmBinderState* b, uint32_t idx1) {
    uint32_t t = b->q_tail;
    uint32_t n = (t + 1) % VM_BINDER_QSLOTS;
    if (n == b->q_head) return -1; /* full */
    b->q_items[t] = idx1;
    __sync_synchronize();
    b->q_tail = n;
    return 0;
}

static int q_pop(VmBinderState* b, uint32_t* out) {
    if (b->q_head == b->q_tail) return -1;
    *out = b->q_items[b->q_head];
    b->q_head = (b->q_head + 1) % VM_BINDER_QSLOTS;
    return 0;
}

int guest_pid_of(VmCpu* c);

/* ── 3.1：futex（宿主侧裸 syscall，绕 libc；futex 在 BPF ALWAYS 放行清单，
 * sigsys handler 内裸发不会二次 TRAP）── */
#define VM_FUTEX_WAIT 0
#define VM_FUTEX_WAKE 1

static long raw_futex(volatile uint32_t* uaddr, long op, uint32_t val,
                      const void* timeout /* struct timespec*, 仅 WAIT */) {
#if defined(__x86_64__)
    long r; register long rdi __asm__("rdi") = (long)uaddr;
    register long rsi __asm__("rsi") = op;
    register long rdx __asm__("rdx") = (long)val;
    register long r10 __asm__("r10") = (long)timeout;
    register long r8  __asm__("r8")  = 0;
    register long r9  __asm__("r9")  = 0;
    __asm__ volatile("syscall" : "=a"(r) : "a"(202L), "r"(rdi), "r"(rsi), "r"(rdx),
                     "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
    return r;
#else
    register long x8 __asm__("x8") = 98;      /* __NR_futex (aarch64) */
    register long x0 __asm__("x0") = (long)uaddr;
    register long x1 __asm__("x1") = op;
    register long x2 __asm__("x2") = (long)val;
    register long x3 __asm__("x3") = (long)timeout;
    register long x4 __asm__("x4") = 0;
    register long x5 __asm__("x5") = 0;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
                     : "memory", "cc");
    return x0;
#endif
}

/* 事件字自增 + 唤醒（完全屏障：先加字再 wake，waiter 醒来必见新值） */
static void evt_bump(volatile uint32_t* w) {
    __atomic_fetch_add(w, 1, __ATOMIC_SEQ_CST);
    raw_futex(w, VM_FUTEX_WAKE, 1, 0);
}

/* v0.4.4：单次等待——返回即重扫（唤醒/值变/EINTR/超时一律返回，由调用方
 * 的"扫描→等待→再扫描"交替循环消化一切假唤醒）。
 * v0.4.3 缺陷（真机 09:13 实锤）：evt_wait 在被 WAKE 唤醒后不重扫直接续睡
 * ——此时事件字已是新值，futex_wait(新值) 匹配成功再睡满 2s，10 轮
 * 18~20s 全超时 → 投 NOOP 兜底 → 全环靠 guest 重试完成（功能对、语义废）。 */
static void evt_wait_once(volatile uint32_t* w) {
    uint32_t cur = __atomic_load_n(w, __ATOMIC_ACQUIRE);
    struct { long tv_sec; long tv_nsec; } ts = { 2, 0 };   /* 单轮 2s */
    (void)raw_futex(w, VM_FUTEX_WAIT, cur, &ts);
}

/* fork 前预创建共享区：所有 guest 后代 fork 时继承同一物理页（v0.4.1 修复：
 * 惰性创建导致 server/client 各持私有副本 → 事务永远到不了 mgr → server 永轮询） */
void vm_binder_early_init(VmCpu* c) { (void)vm_binder_state(c); }

/* ── ioctl 判决（fd 是虚拟 binder fd 时进入） ── */
long vm_binder_ioctl(VmCpu* c, unsigned long cmd, unsigned long arg) {
    VmBinderState* b = vm_binder_state(c);
    if (!b) return -12;

    if (cmd == BINDER_SET_CONTEXT_MGR) {
        b->mgr_pid = (int32_t)guest_pid_of(c);
        vm_log("binder: SET_CONTEXT_MGR by pid %d", b->mgr_pid);
        return 0;
    }
    if (cmd != BINDER_WRITE_READ)
        return -25 /* ENOTTY（3.0 只实现 WRITE_READ） */;

    VmBwr bwr;
    if (vm_mem_read(&c->mem, &bwr, (uintptr_t)arg, sizeof(bwr)) != 0) return -14;

    /* write 侧：BC_TRANSACTION / BC_REPLY（3.1：mmap 段内数据零拷贝直挂指针） */
    int noop = 0;
    if (bwr.write_size >= 8 && bwr.write_buffer) {
        uint64_t wb = bwr.write_buffer + bwr.write_consumed;
        uint32_t bc;
        if (vm_mem_read(&c->mem, &bc, (uintptr_t)wb, 4) != 0) return -14;
        if (bc == (uint32_t)BC_TRANSACTION) {
            VmTxnData td;
            if (vm_mem_read(&c->mem, &td, (uintptr_t)(wb + 4), sizeof(td)) != 0) return -14;
            if (b->mgr_pid == 0) {
                /* server 未注册：真 binder 会 BR_DEAD_REPLY；demo 用 BR_NOOP 让
                 * client 微睡重试（demo 场景=启动竞态，不是死句柄） */
                noop = 1;
            } else if (td.target_handle != 0) {
                return -22;   /* EINVAL：只支持发给 context mgr（handle 0） */
            } else {
                int ti = txn_alloc(b);
                if (ti < 0) return -12;
                VmBinderTxn* t = &b->txns[ti];
                t->code = (uint32_t)td.code;
                t->cookie = td.cookie;
                t->from_pid = guest_pid_of(c);
                t->data_len = (uint32_t)(td.data_size > VM_BINDER_MAX_TXN ? VM_BINDER_MAX_TXN : td.data_size);
                /* 零拷贝：data_buffer 落在共享区 mmap 段内 → 数据原地不动，挂指针 */
                if (td.data_size && td.data_buffer) {
                    uint64_t lo = (uint64_t)(uintptr_t)&b->mmaps[0][0];
                    uint64_t hi = lo + sizeof(b->mmaps);
                    if (td.data_buffer >= lo && td.data_buffer + td.data_size <= hi) {
                        t->data_in_mmap = 1;
                        t->data_ptr = (const uint8_t*)(uintptr_t)td.data_buffer;
                    } else if (vm_mem_read(&c->mem, t->data, (uintptr_t)td.data_buffer, t->data_len) != 0) {
                        t->state = 0; return -14;
                    }
                }
                if (q_push(b, (uint32_t)(ti + 1)) != 0) { t->state = 0; return -12; }
                evt_bump(&b->q_event);      /* 唤醒阻塞中的 server */
                vm_log("binder: pid %d BC_TRANSACTION code=%u len=%u%s → 队列",
                       t->from_pid, t->code, t->data_len, t->data_in_mmap ? "（mmap 零拷贝）" : "");
                bwr.write_consumed = bwr.write_size;   /* 消费完毕（4 cmd + 64 data） */
            }
        } else if (bc == (uint32_t)BC_REPLY) {
            VmTxnData td;
            if (vm_mem_read(&c->mem, &td, (uintptr_t)(wb + 4), sizeof(td)) != 0) return -14;
            /* 回给 original txn（td.pad0 复用为 txn 索引——demo 协议私有位） */
            int ti = (int)td.pad0;
            if (ti < 0 || ti >= VM_BINDER_QSLOTS) return -22;
            VmBinderTxn* t = &b->txns[ti];
            if (t->state != 2) return -22;
            t->data_len = (uint32_t)(td.data_size > VM_BINDER_MAX_TXN ? VM_BINDER_MAX_TXN : td.data_size);
            if (td.data_size && td.data_buffer) {
                uint64_t lo = (uint64_t)(uintptr_t)&b->mmaps[0][0];
                uint64_t hi = lo + sizeof(b->mmaps);
                if (td.data_buffer >= lo && td.data_buffer + td.data_size <= hi) {
                    t->data_in_mmap = 1;
                    t->data_ptr = (const uint8_t*)(uintptr_t)td.data_buffer;
                } else if (vm_mem_read(&c->mem, t->data, (uintptr_t)td.data_buffer, t->data_len) != 0) return -14;
            }
            __atomic_store_n(&t->state, 3, __ATOMIC_RELEASE);   /* replied：client 可读 */
            evt_bump(&b->rep_event);      /* 唤醒阻塞中的 client */
            vm_log("binder: pid %d BC_REPLY → txn#%d（%u 字节%s）",
                   guest_pid_of(c), ti, t->data_len, t->data_in_mmap ? "，零拷贝" : "");
            bwr.write_consumed = bwr.write_size;
        }
    }

    /* read 侧：mgr 收 BR_TRANSACTION / 原发者收 BR_REPLY。
     * 3.1 阻塞语义（对齐真 binder 的 ioctl 默认阻塞）：
     *   sigsys 后端 → futex_wait 共享区事件字，被生产者唤醒后重扫；
     *   ptrace 后端 → 立即返回（guest usleep 兜底轮询，本地回归路径）。
     * 20s 总上限耗尽 → 投 BR_NOOP（真 binder 的 idle 保活命令，guest 自行计数退出）。 */
    if (bwr.read_size >= 8 && bwr.read_buffer) {
        if (noop) {
            uint64_t rb = bwr.read_buffer + bwr.read_consumed;
            uint32_t br = (uint32_t)BR_NOOP;
            if (vm_mem_write(&c->mem, (uintptr_t)rb, &br, 4) != 0) return -14;
            bwr.read_consumed = 4;
            goto out;
        }
        if (guest_pid_of(c) == b->mgr_pid) {
            for (int polls = 0; polls < 10; polls++) {   /* 扫描→等待交替；10×2s 上限 */
                uint32_t idx1;
                if (q_pop(b, &idx1) == 0) {
                    int ti = (int)idx1 - 1;
                    VmBinderTxn* t = &b->txns[ti];
                    uint64_t rb = bwr.read_buffer + bwr.read_consumed;
                    uint32_t br = (uint32_t)BR_TRANSACTION;
                    VmTxnData td; memset(&td, 0, sizeof(td));
                    td.target_handle = 0;
                    td.code = t->code;
                    td.cookie = t->cookie;
                    td.sender_pid = t->from_pid;
                    td.pad0 = (uint32_t)ti;          /* 私有位：txn 索引（回复时用） */
                    if (t->data_in_mmap) {           /* 零拷贝投递：只写头，数据留段内 */
                        td.data_size = t->data_len;
                        td.data_buffer = (uint64_t)(uintptr_t)t->data_ptr;
                        if (vm_mem_write(&c->mem, (uintptr_t)rb, &br, 4) != 0) return -14;
                        if (vm_mem_write(&c->mem, (uintptr_t)(rb + 4), &td, sizeof(td)) != 0) return -14;
                        bwr.read_consumed = 4 + sizeof(VmTxnData);
                    } else {
                        td.data_size = t->data_len;
                        td.data_buffer = rb + 4 + sizeof(td);   /* 内嵌缓冲 */
                        if (vm_mem_write(&c->mem, (uintptr_t)rb, &br, 4) != 0) return -14;
                        if (vm_mem_write(&c->mem, (uintptr_t)(rb + 4), &td, sizeof(td)) != 0) return -14;
                        if (t->data_len)
                            if (vm_mem_write(&c->mem, (uintptr_t)(rb + 4 + sizeof(td)), t->data, t->data_len) != 0) return -14;
                        bwr.read_consumed = 4 + sizeof(VmTxnData) + t->data_len;
                    }
                    __atomic_store_n(&t->state, 2, __ATOMIC_RELEASE);   /* delivered，等 reply */
                    vm_log("binder: mgr 收 BR_TRANSACTION code=%u from pid %d%s",
                           t->code, t->from_pid, t->data_in_mmap ? "（零拷贝）" : "");
                    goto out;
                }
                if (!c->mem.in_process) break;              /* ptrace：非阻塞降级 */
                evt_wait_once(&b->q_event);                 /* 醒来即回循环头重扫 */
            }
        } else {
            /* client：等自己的 txn 被 replied（扫描→等待交替，BC_REPLY 者唤醒） */
            for (int polls = 0; polls < 10; polls++) {
                for (int i = 0; i < VM_BINDER_QSLOTS; i++) {
                    VmBinderTxn* t = &b->txns[i];
                    if (__atomic_load_n(&t->state, __ATOMIC_ACQUIRE) == 3 && t->from_pid == guest_pid_of(c)) {
                        uint64_t rb = bwr.read_buffer + bwr.read_consumed;
                        uint32_t br = (uint32_t)BR_REPLY;
                        VmTxnData td; memset(&td, 0, sizeof(td));
                        td.code = t->code;
                        if (t->data_in_mmap) {
                            td.data_size = t->data_len;
                            td.data_buffer = (uint64_t)(uintptr_t)t->data_ptr;
                            if (vm_mem_write(&c->mem, (uintptr_t)rb, &br, 4) != 0) return -14;
                            if (vm_mem_write(&c->mem, (uintptr_t)(rb + 4), &td, sizeof(td)) != 0) return -14;
                            bwr.read_consumed = 4 + sizeof(VmTxnData);
                        } else {
                            td.data_size = t->data_len;
                            td.data_buffer = rb + 4 + sizeof(td);
                            if (vm_mem_write(&c->mem, (uintptr_t)rb, &br, 4) != 0) return -14;
                            if (vm_mem_write(&c->mem, (uintptr_t)(rb + 4), &td, sizeof(td)) != 0) return -14;
                            if (t->data_len)
                                if (vm_mem_write(&c->mem, (uintptr_t)(rb + 4 + sizeof(td)), t->data, t->data_len) != 0) return -14;
                            bwr.read_consumed = 4 + sizeof(VmTxnData) + t->data_len;
                        }
                        t->state = 0;   /* 完成回收 */
                        vm_log("binder: pid %d 收 BR_REPLY（%u 字节%s）",
                               guest_pid_of(c), t->data_len, t->data_in_mmap ? "，零拷贝" : "");
                        goto out;
                    }
                }
                if (!c->mem.in_process) break;
                evt_wait_once(&b->rep_event);                /* 醒来即回循环头重扫 */
            }
        }
        /* 到此 = 无事可读（ptrace 降级 / 20s 上限）：投 BR_NOOP 或静默返回 */
        if (bwr.write_consumed == bwr.write_size) {
            if (c->mem.in_process) {         /* 只有真阻塞过的（超时）才投 NOOP */
                uint64_t rb = bwr.read_buffer + bwr.read_consumed;
                uint32_t br = (uint32_t)BR_NOOP;
                if (vm_mem_write(&c->mem, (uintptr_t)rb, &br, 4) != 0) return -14;
                bwr.read_consumed = 4;
            } else {
                bwr.read_consumed = 0;       /* ptrace：老语义，guest usleep 重试 */
            }
            goto out;
        }
    }
out:
    if (vm_mem_write(&c->mem, (uintptr_t)arg, &bwr, sizeof(bwr)) != 0) return -14;
    return 0;
}

/* ── 3.1：guest mmap(虚拟 binder fd) → 共享区事务缓冲段（零拷贝基地址）──
 * 真 binder 同款：fd 对应 binder_alloc 的 mmap 区，事务数据在此跨进程共享。
 *   sigsys 后端：guest 与宿主同地址空间 → 直接返回段地址，fork 的兄弟进程
 *   天然可见（MAP_SHARED 同物理页），BC_/BR_ 投递挂指针不搬数据。
 *   ptrace 后端：共享区在 supervisor 堆，guest 无此映射 → -ENODEV，
 *   guest 降级走内嵌拷贝路径（demo 打印「拷贝模式」）。 */
uint64_t vm_binder_mmap(VmCpu* c, size_t len) {
    VmBinderState* b = vm_binder_state(c);
    if (!b) return (uint64_t)-12;
    if (!c->mem.in_process) return (uint64_t)-19;   /* ENODEV：本地回归降级 */
    if (len == 0 || len > VM_BINDER_MMAP_SZ) return (uint64_t)-22;
    uint32_t slot = __atomic_fetch_add(&b->mmap_hint, 1, __ATOMIC_RELAXED);
    slot %= VM_BINDER_MMAP_SLOTS;   /* 轮转复用（demo：单事务生命周期极短） */
    uint64_t p = (uint64_t)(uintptr_t)&b->mmaps[slot][0];
    vm_log("binder: pid %d mmap 段#%u = 0x%llx（%zuB，零拷贝缓冲）",
           guest_pid_of(c), slot, (unsigned long long)p, len);
    return p;
}

/* guest pid：多进程下 sigsys 后端=真实 pid；ptrace 后端 dispatch 时由 supervisor 置 */
static __thread int g_guest_pid_cache = 0;
void vm_binder_set_guest_pid(int pid) { g_guest_pid_cache = pid; }

/* v0.4.2：裸 syscall 取 pid——绕过 libc 的 pid 缓存。
 * sigsys 后端的 fork 是 handler 内裸 svc clone，bionic 的 fork 记账不会运行，
 * 子进程里 libc getpid() 返回陈旧缓存（真机 v0.4.1 实锤：server/client 全报
 * 槽位 pid 1868 → client 的回复轮询被路由进 mgr 分支抢走自己的事务 → 双超时）。
 * getpid 在 BPF 放行清单（ALWAYS），handler 内裸发不会二次 TRAP。
 * 本地 glibc 自 2.25 起不缓存 pid，行为不变。 */
static long raw_getpid(void) {
#if defined(__x86_64__)
    long r; __asm__ volatile("syscall" : "=a"(r) : "a"(39L) : "rcx", "r11", "memory");
    return r;
#else
    register long x8 __asm__("x8") = 172;   /* __NR_getpid (aarch64) */
    register long x0 __asm__("x0") = 0;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory", "cc");
    return x0;
#endif
}
int  guest_pid_of(VmCpu* c) {
    (void)c;
    if (g_guest_pid_cache) return g_guest_pid_cache;
    return (int)raw_getpid();   /* sigsys 后端：内核真值，免疫 libc 缓存 */
}
