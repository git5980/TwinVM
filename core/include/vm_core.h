/* TwinVM Stage 0 — 用户态虚拟机内核核（从零复刻 VMOS Pro 路线）
 *
 * 架构依据：VMOS Pro 3.1.4 全量逆向（2026-10-12）：
 *   libkrnloader(装载) + libvmoskernel(用户态内核) + libylinker(自定义链接器)
 *   + libvmkrnfs(ROM 镜像) + libhalbox(设备半虚拟化)
 * 免 root 原理：guest 是同架构原生代码，系统调用被截获后由本引擎
 * 在用户空间服务（seccomp-TRAP→SIGSYS），无需 KVM/内核特权。
 */
#ifndef TWINVM_CORE_H
#define TWINVM_CORE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── 错误码（Linux 通用 errno，两架构一致） ── */
#define VM_ENOENT  2
#define VM_EBADF   9
#define VM_EACCES 13
#define VM_ENAMETOOLONG 36
#define VM_ENOSYS 38

#define VM_MAX_FILES 96   /* v1.2.1：64→96——槽 64..95 留给 /apex 动态直读（rom_idx=-3） */
#define VM_MAX_PATH  256
#define VM_FD_BASE   10000  /* 4.0：真 fd（pipe/epoll）与虚拟 fd 空间隔离——
                             * pipe2 等内核 fd 从 3 起递增，虚拟 fd 高位避让 */

/* ── ROM 镜像（格式对齐 vmkrnfs 实测：inode 表 + per-file crc32 + xattr 槽） ── */
typedef struct VmRomInode {
    char     name[VM_MAX_PATH];  /* 含前导 '/'，如 "/rom/hello.txt" */
    uint32_t mode, uid, gid;
    uint64_t size;
    uint32_t crc32;              /* 数据校验 */
    uint32_t xattr_len;
    const uint8_t* map;          /* host 侧数据指针 */
} VmRomInode;

typedef struct VmRom {
    uint8_t*   base;
    size_t     size;
    uint32_t   inode_count;
    VmRomInode* inodes;
    uint32_t   file_crc;
} VmRom;

int  vm_rom_load(VmRom* rom, const char* path);
/* v1.2.120：快速装载（列表/提取场景——跳过逐 inode CRC） */
int  vm_rom_load_quick(VmRom* rom, const char* path);
/* v1.2.111：运行期向 ROM 追加文件（installApk 引擎层；返回 0 成功） */
int  vm_rom_install(const char* src_path, const char* rom_path,
                    const char* rom_dir, char* out_name, size_t out_name_sz);
int  vm_rom_load_mem(VmRom* rom, const uint8_t* data, size_t size);
void vm_rom_close(VmRom* rom);
const VmRomInode* vm_rom_find(const VmRom* rom, const char* path);
uint32_t vm_crc32(uint32_t crc, const uint8_t* buf, size_t len);

/* ── VFS：fd 全部是引擎虚拟句柄（guest 看不见宿主 fd 表） ── */
typedef struct VmFile { int in_use; int rom_idx; int is_dir; int is_binder; uint32_t dir_next; uint64_t pos; char dir_path[VM_MAX_PATH]; } VmFile;
typedef struct VmVfs  { const VmRom* rom; VmFile files[VM_MAX_FILES]; } VmVfs;

void vm_vfs_init(VmVfs* vfs, const VmRom* rom);
/* open: 精确 inode 命中=文件；命中前缀=目录（fd 记 is_dir 供 getdents64 枚举） */
int vm_vfs_open(VmVfs* vfs, const char* path);
/* stat: 0=文件 1=目录 -1=不存在（填 mode/size 出参） */
int vm_vfs_stat(const VmRom* rom, const char* path, uint32_t* mode, uint64_t* size);
long vm_vfs_read(VmVfs* vfs, int fd, void* buf, size_t n);
long vm_vfs_seek(VmVfs* vfs, int fd, long off, int whence);
int  vm_vfs_close(VmVfs* vfs, int fd);
typedef struct VmCpu VmCpu;   /* 前向声明：下面 vm_vfs_fd_data 要用 */
const uint8_t* vm_vfs_fd_data(const VmCpu* c, long fd, size_t* out_size);  /* v1.2.0: fd→ROM 数据（ptrace mmap-fill） */
size_t vm_render_maps(char* out, size_t cap);   /* v1.2.22 合并补：TA 侧隐式声明编译放行，头文件补正式声明 */

/* ── v1.2.36 ★★★ exec_dyn 引擎栈登记（"改账"的真相源）──
 * 真机 v1.2.35 符号化实锤：guest 主线程跑在 exec_dyn 的 mmap 栈上，而
 * /proc/self/maps 的 [stack] 行、/proc/self/stat 第 28 格 startstack 都是
 * 内核栈真值（fork 留下的原生栈）→ bionic pthread_getattr_np 报给 ART 一套
 * "内核栈"边界（源码公式：stack_base = [含 startstack 的区间].hi − RLIMIT_STACK）
 * → art::Thread::Init 的 CHECK(FindStackTop<GetStackEnd) 翻车 → LOG(FATAL)
 * 又死锁在日志里（abort 消息被吞，表现为"静默挂起"）。
 * 修复：exec_dyn 把引擎栈三要素登记在此（子进程写），maps/stat 伪造处跨进程
 * 读它改账（父进程读，fork 无 exec → 同 .so 同地址，教训五第二击同款）。 */
typedef struct VmExecStack { unsigned long long lo, hi, sp; } VmExecStack;
extern VmExecStack g_exec_stack;
void vm_set_exec_stack(unsigned long long lo, unsigned long long hi, unsigned long long sp);

/* ── 内存访问抽象：sigsys 后端=本进程直拷；ptrace 后端=跨进程读写 ──
 * 这是多进程 VM 的雏形：每个 guest 槽位一个 VmMem。 */
typedef struct VmMem { int pid; int in_process; } VmMem;
int vm_mem_read (VmMem* m, void* dst, uintptr_t src, size_t n);
int vm_mem_write(VmMem* m, uintptr_t dst, const void* src, size_t n);

/* ── 控制台桥：唯一允许直通宿主的输出通道（BPF 按 fd 放行） ── */
extern int vm_console_fd;
void vm_console_write(const char* data, size_t n);
void vm_log(const char* fmt, ...);

/* ── 调度核心：一次虚拟系统调用 ──
 * 返回值即 guest 语义（负数= -errno）；halt=guest 请求关机。 */
typedef struct VmCpu {
    VmVfs* vfs;
    VmMem  mem;
    int    halt;
    uint64_t cycles;   /* 已服务 syscall 数（性能观测） */
    long nr_hist[24];  /* 最近 24 次 TRAP 的 syscall 号（崩溃取证） */
    int  nr_hist_n;
    uintptr_t guest_sp;   /* v0.7.44: 最近一次 TRAP 的 guest SP（maps 合成兜底 [stack]） */
} VmCpu;

long vm_handle_syscall(VmCpu* c, long nr, long a0, long a1, long a2, long a3, long a4, long a5);

/* Stage 3：binder 用户态驱动 */
int  vm_binder_open(VmCpu* c);
long vm_binder_ioctl(VmCpu* c, unsigned long cmd, unsigned long arg);
void vm_binder_set_guest_pid(int pid);   /* ptrace 后端 dispatch 时置真实 guest pid */
void vm_binder_early_init(VmCpu* c);  /* fork 前预创建共享区（v0.4.1） */
uint64_t vm_binder_mmap(VmCpu* c, size_t len);   /* 3.1：mmap binder fd → 共享区零拷贝段 */

/* ── 后端：seccomp-TRAP + SIGSYS（进程内内核，VMOS 同型） ── */
int vm_sigsys_install(int console_fd);      /* 0=ok, -errno */

/* ── 直发清单一致性自检（v0.2.8）──
 * 判决表里所有"直发内核"的 syscall（VM_PT*）都必须被 BPF 放行，否则 handler
 * 内的裸 svc 会二次 TRAP：内核不执行该 syscall、SIGSYS 挂起，等 guest 恢复
 * 信号掩码后迟到投递 —— 此时 ucontext 已不是系统调用现场，用户态内核会用
 * 垃圾参数执行一次完全无关的 syscall（Stage 1 自检 pthread 一步
 * "code=-31（SIGSYS）" 且不再打 trap#N 的根因）。 */
extern const long vm_passthrough_nrs[];   /* 判决表直发集合 */
extern const int  vm_passthrough_nr_count;
/* v0.7.22：引擎**自身**在 SIGSYS handler 内调用的 syscall（process_vm_readv/writev，
 * vm_mem_read/vm_mem_write 的安全访存）——同一条铁律：handler 会执行它，过滤器就必须
 * 放行，否则二次 TRAP。单独一张表便于自检时区分"guest 直发"与"引擎自用"。 */
extern const long vm_engine_syscall_nrs[];
extern const int  vm_engine_syscall_nr_count;
/* 启动自检：逐条比对 BPF 判决；返回不一致条数（0=一致），不一致时写控制台告警 */
int vm_filter_check_passthrough(void);
/* v0.7.24：查过滤器对某次 syscall 的判决（0x7fff0000=ALLOW）。
 * vm_syscall6() 每次直发前都查一次，未放行就拒绝直发（返回 -ENOSYS）——
 * 把"直发集合 ⊆ 放行清单"从人工纪律升级成机制。 */
long vm_filter_action(long nr, long a0, long a1, long a2, long a3, long a4, long a5);
#define VM_FILTER_ALLOW 0x7fff0000L

/* ── 装载器：进程内静态 ELF 映射 + 跳转（绕 exec/W^X，VMOS libkrnloader 同思路） ── */
typedef struct VmLaunch { void (*entry)(void); uintptr_t sp; } VmLaunch;
/* Stage 2.1：真 argv/envp 装载（虚拟 execve 的地基） */
void vm_set_exec_path(const char* p);
void vm_set_exec_cmdline(char* const argv[]);   /* 4.2：/proc/self/cmdline */      /* 4.2：execve 时设，/proc/self/exe 用 */
const char* vm_get_exec_path(void);
int  vm_loader_exec_dyn(const uint8_t* elf, size_t size,
                       const uint8_t* ldso, size_t ldso_size,
                       const char* execfn,
                       char* const argv[], char* const envp[],
                       VmLaunch* out);   /* 4.2：动态 PIE（真 linker 接管） */
int  vm_loader_exec(const uint8_t* elf, size_t size,
                    char* const argv[], char* const envp[], VmLaunch* out);
int  vm_loader_prepare(const uint8_t* elf, size_t size, VmLaunch* out);
void vm_loader_jump(const VmLaunch* l);    /* 不返回 */

const char* vm_arch_name(void);

/* 本进程已就绪的机器（单实例 Stage 0）：装配 VFS+内存桥，返回调度器句柄 */
VmCpu* vm_machine_setup(const VmRom* rom, int pid, int in_process);

/* v0.8.0：SECCOMP_RET_USER_NOTIF 后端（路线 A） */
int  vm_notif_install(int console_fd);
void* vm_notif_loop(void* arg);
int  vm_notif_fd(void);

/* v0.7.43: register a mapping for /proc/self/maps synthesis */
void vm_map_add(uintptr_t addr, size_t len, int prot, const char* path);

/* v0.7.50 共享黑匣子：放在 fork 之前的 MAP_SHARED 页里。
 * 为什么需要它：App 策略的 SECCOMP_RET_KILL_PROCESS **绕过一切 handler**，
 * 子进程连遗言都留不下（真机 v0.7.47/49 都是"无 TRAP 行 + code=-31"）。
 * 每次 syscall 由引擎把 (nr, arg0, pc) 写进这块共享内存，父进程在收尸时读出来
 * —— 内核杀不死它。 */
typedef struct VmBlackBox {
    volatile unsigned long seq;
    volatile int idx;
    volatile long nr[32];
    volatile unsigned long a0[32];
    volatile unsigned long pc[32];
} VmBlackBox;
extern VmBlackBox* vm_bb;
void vm_bb_attach(void* p);
void vm_bb_pc(unsigned long pc);          /* 后端补 PC */
/* v0.7.45: guest 主线程栈基址 —— /proc/self/stat 第 28 字段 startstack；
 * bionic 的 __find_main_stack_limits() 靠它去 maps 里定位 [stack]。 */
void vm_set_guest_stack(uintptr_t base, size_t len);
uintptr_t vm_get_guest_stack_base(void);
#define vm_cpu_current vm_machine_current
VmCpu* vm_machine_current(void);

#ifdef __cplusplus
}
#endif
#endif
