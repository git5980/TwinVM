/* TwinVM 装载器 — 进程内静态 ELF 映射 + 直接跳转。
 * 为什么不用 execve：Android 10+ SELinux 禁止执行应用数据目录里的文件
 * （execute_no_trans / W^X）。VMOS 的解法（libkrnloader + libylinker）就是
 * 把 guest 二进制当数据 mmap 进内存再跳转——我们 Stage 0 同款最小实现。
 *
 * Stage 2.1：vm_loader_exec 支持真 argv/envp 栈构造 —— 虚拟 execve 的地基
 * （guest 里 execve("/bin/child", argv, envp) 被用户态内核拦截后走这里，
 * 新映像映射 + 新栈 + argc/argv/envp/auxv 齐活，由后端改写 ucontext 跳转）。 */
#define _GNU_SOURCE
#include "vm_core.h"

#include <elf.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

static int prot_of(uint32_t p_flags) {
    int p = 0;
    if (p_flags & PF_R) p |= PROT_READ;
    if (p_flags & PF_W) p |= PROT_WRITE;
    if (p_flags & PF_X) p |= PROT_EXEC;
    return p ? p : PROT_READ;
}

int vm_loader_exec(const uint8_t* elf, size_t size,
                   char* const argv[], char* const envp[], VmLaunch* out) {
    if (size < sizeof(Elf64_Ehdr)) return -1;
    const Elf64_Ehdr* eh = (const Elf64_Ehdr*)elf;
    if (memcmp(eh->e_ident, ELFMAG, 4) != 0) return -2;
    if (eh->e_ident[EI_CLASS] != ELFCLASS64) return -3;
    if (eh->e_type != ET_EXEC) return -4;  /* Stage 0 只装载静态 no-pie guest */
#if defined(__x86_64__)
    if (eh->e_machine != EM_X86_64) return -5;
#elif defined(__aarch64__)
    if (eh->e_machine != EM_AARCH64) return -5;
#endif
    if (eh->e_phoff + (size_t)eh->e_phnum * sizeof(Elf64_Phdr) > size) return -6;
    const Elf64_Phdr* ph = (const Elf64_Phdr*)(elf + eh->e_phoff);

    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        uintptr_t base = ph[i].p_vaddr & ~0xFFFUL;
        size_t    span = (ph[i].p_vaddr + ph[i].p_memsz) - base;
        span = (span + 0xFFF) & ~0xFFFUL;
        if (ph[i].p_offset + ph[i].p_filesz > size) return -7;
        int final_prot = prot_of(ph[i].p_flags);
        /* 拷贝期需要 W：先 (final|W) 映射 → memcpy → mprotect 收紧（经典装载器两段式） */
        void* at = mmap((void*)base, span, final_prot | PROT_WRITE,
                        MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (at == MAP_FAILED) return -8;
        memcpy((void*)ph[i].p_vaddr, elf + ph[i].p_offset, ph[i].p_filesz);
        /* v0.7.55：同 vm_core —— 执行前必须清 i-cache */
        __builtin___clear_cache((char*)ph[i].p_vaddr,
                                (char*)ph[i].p_vaddr + ph[i].p_memsz);
        if (mprotect((void*)base, span, final_prot) != 0) return -10;
    }

    /* 启动栈（每次 exec 都 mmap 全新 1MB——旧栈遗留无害，Stage 2.1 不做栈回收）
     * 布局（高→低）：AT_RANDOM 种子 | argv/envp 字符串 | [sp]: argc,argv[],NULL,envp[],NULL,auxv */
    void* stack = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stack == MAP_FAILED) return -9;
    uintptr_t top = (uintptr_t)stack + (1 << 20) - 4096;
    top &= ~0xFUL;
    /* AT_RANDOM 16 字节随机种子（真机取证实锤：bionic __libc_safe_arc4random_buf
     * getauxval(AT_RANDOM)==NULL → memmove(src=NULL) 直接崩） */
    static uint8_t seed[16] = { 0x9E,0x37,0x79,0xB9,0x7F,0x4A,0x7C,0x15,
                                0xF3,0x9C,0xA2,0x28,0xD4,0x06,0x88,0x11 };
    uintptr_t seed_at = top - 32;
    memcpy((void*)seed_at, seed, sizeof(seed));

    int argc = 0, envc = 0;
    while (argv && argv[argc] && argc < 63) argc++;
    while (envp && envp[envc] && envc < 63) envc++;
    uintptr_t argv_ptr[64], envp_ptr[64];
    uintptr_t str_top = seed_at;                     /* 字符串从种子往下堆 */
    for (int i = argc - 1; i >= 0; i--) {
        size_t l = strlen(argv[i]) + 1;
        if (l > 512) return -11;
        str_top -= l;
        memcpy((void*)str_top, argv[i], l);
        argv_ptr[i] = str_top;
    }
    for (int i = envc - 1; i >= 0; i--) {
        size_t l = strlen(envp[i]) + 1;
        if (l > 512) return -11;
        str_top -= l;
        memcpy((void*)str_top, envp[i], l);
        envp_ptr[i] = str_top;
    }
    size_t vec_bytes = (size_t)(1 + argc + 1 + envc + 1 + 16) * sizeof(uintptr_t);
    uintptr_t* w = (uintptr_t*)((str_top - vec_bytes) & ~(uintptr_t)0xF);
    uintptr_t sp_final = (uintptr_t)w;                /* argc 所在位置 = 新栈顶 */
    *w++ = (uintptr_t)argc;
    for (int i = 0; i < argc; i++) *w++ = argv_ptr[i];
    *w++ = 0;
    for (int i = 0; i < envc; i++) *w++ = envp_ptr[i];
    *w++ = 0;
    *w++ = AT_PAGESZ;  *w++ = 4096;
    *w++ = AT_RANDOM;  *w++ = seed_at;         /* bionic 随机种子（必需！） */
    *w++ = AT_UID;     *w++ = 0;               /* VM 内 root */
    *w++ = AT_EUID;    *w++ = 0;
    *w++ = AT_GID;     *w++ = 0;
    *w++ = AT_EGID;    *w++ = 0;
    *w++ = AT_SECURE;  *w++ = 0;
    *w++ = AT_NULL;    *w++ = 0;

    out->entry = (void (*)(void))eh->e_entry;
    out->sp = sp_final;
    return 0;
}

int vm_loader_prepare(const uint8_t* elf, size_t size, VmLaunch* out) {
    static char* const def_argv[] = { (char*)"guest", NULL };
    return vm_loader_exec(elf, size, def_argv, NULL, out);
}

/* ── Stage 4.2：动态 ELF 装载（真 linker 接管协议）──
 * 我们扮演内核对 PIE 的全部职责：
 *   1. 把目标 PIE 的 PT_LOAD 映射进内存（自选 bias）
 *   2. 把动态链接器（linker64 / ld.so）映射进内存
 *   3. 构造完整 auxv 栈：AT_PHDR/AT_PHENT/AT_PHNUM（目标）、AT_BASE（linker
 *      bias）、AT_ENTRY（目标 entry+bias）、AT_EXECFN、AT_RANDOM、AT_HWCAP、
 *      AT_PLATFORM、AT_PAGESZ、AT_UID 族、AT_SECURE、AT_NULL
 *   4. 跳进 linker 入口——装载 DT_NEEDED、TLS 建立、重定位、跳 e_entry
 *      全部由真链接器完成（它自己 mmap 我们的虚拟 fd）
 * VMOS 的 app_process 引导就是这个形态。 */
#include <sys/auxv.h>

static int prot_of_ph(uint32_t p_flags) { return prot_of(p_flags); }

/* PIE 段映射：reserve span → 逐段 FIXED 映射+拷贝 → 收紧权限。
 * 返回 load bias（0 失败）。地址选择交给内核（mmap NULL）。 */
static uintptr_t map_elf_bias(const uint8_t* elf, size_t size) {
    const Elf64_Ehdr* eh = (const Elf64_Ehdr*)elf;
    if (size < sizeof(*eh) || memcmp(eh->e_ident, ELFMAG, 4) != 0) return 0;
    const Elf64_Phdr* ph = (const Elf64_Phdr*)(elf + eh->e_phoff);
    uintptr_t min_v = ~0UL, max_v = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        uintptr_t lo = ph[i].p_vaddr & ~0xfffUL;
        uintptr_t hi = (ph[i].p_vaddr + ph[i].p_memsz + 0xfffUL) & ~0xfffUL;
        if (lo < min_v) min_v = lo;
        if (hi > max_v) max_v = hi;
    }
    if (min_v == ~0UL || max_v <= min_v) return 0;
    void* reserve = mmap(NULL, max_v - min_v, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (reserve == MAP_FAILED) return 0;
    uintptr_t bias = (uintptr_t)reserve - min_v;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        uintptr_t seg = ph[i].p_vaddr & ~0xfffUL;
        uintptr_t off = ph[i].p_offset & ~0xfffUL;
        size_t maplen = ((ph[i].p_vaddr + ph[i].p_memsz + 0xfffUL) & ~0xfffUL) - seg;
        int prot = prot_of_ph(ph[i].p_flags);
        void* at = mmap((void*)(bias + seg), maplen, prot | PROT_WRITE,
                        MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
        if (at == MAP_FAILED) return 0;
        if (ph[i].p_filesz) {
            size_t avail = size - (size_t)off;
            size_t n = ph[i].p_filesz;
            if (n > avail) n = avail;
            memcpy((void*)(bias + ph[i].p_vaddr), elf + ph[i].p_offset, n);
            /* v0.7.55：exe/linker 段执行前清 i-cache（否则可能 SIGILL） */
            __builtin___clear_cache((char*)(bias + ph[i].p_vaddr),
                                    (char*)(bias + ph[i].p_vaddr + ph[i].p_memsz));
        }
        mprotect((void*)(bias + seg), maplen, prot);
    }
    return bias;
}

/* auxv 常量（防依赖系统头差异） */
#define VA_PAGESZ   6
#define VA_BASE     7
#define VA_FLAGS    8
#define VA_ENTRY    9
#define VA_UID     11
#define VA_EUID    12
#define VA_GID     13
#define VA_EGID    14
#define VA_PLATFORM 15
#define VA_HWCAP   16
#define VA_CLKTCK  17
#define VA_PHDR    3
#define VA_PHENT   4
#define VA_PHNUM   5
#define VA_RANDOM  25
#define VA_EXECFN  31
#define VA_SECURE  23

int vm_loader_exec_dyn(const uint8_t* elf, size_t size,
                       const uint8_t* ldso, size_t ldso_size,
                       const char* execfn,
                       char* const argv[], char* const envp[],
                       VmLaunch* out) {
    const Elf64_Ehdr* eh = (const Elf64_Ehdr*)elf;
    const Elf64_Ehdr* lh = (const Elf64_Ehdr*)ldso;
    if (eh->e_type != ET_DYN || lh->e_type != ET_DYN) return -1;
    if (eh->e_machine != lh->e_machine) return -2;

    /* v1.1.8 ★★★ exec 的"自我定位"闭环：/proc/self/exe 的 stat/readlink
     * 特判依赖 g_exec_path，但 vm_set_exec_path 此前**从未被调用**（定义了
     * 却没人用）→ ART 的 stat("/proc/self/exe") 恒 ENOENT →
     * "unable to stat either /proc/self/exe or dalvikvm" → Fatal（v1.1.7
     * 真机实锤，Fatal signal 6 的完整因果）。在真正的 exec 点设上。 */
    if (execfn && execfn[0]) vm_set_exec_path(execfn);

    uintptr_t bias  = map_elf_bias(elf, size);
    vm_log("exec_dyn: exe mapped bias=0x%llx", (unsigned long long)bias);
    if (!bias) return -3;
    uintptr_t ldbias = map_elf_bias(ldso, ldso_size);
    vm_log("exec_dyn: linker mapped bias=0x%llx", (unsigned long long)ldbias);
    if (!ldbias) return -3;

    /* AT_PHDR：目标的程序头表（必须在已映射段内） */
    const Elf64_Phdr* ph = (const Elf64_Phdr*)(elf + eh->e_phoff);
    uintptr_t phdr = 0;
    for (int i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_PHDR) { phdr = ph[i].p_vaddr; break; }
    if (!phdr) phdr = ph[0].p_vaddr + eh->e_phoff;   /* 首段内（PIE 恒真） */
    phdr += bias;

    /* 栈：argc argv[] envp[] auxv[] + 字符串区（execfn/platform/随机数） */
    size_t argc = 0, envc = 0;
    while (argc < 64 && argv && argv[argc]) argc++;
    while (envc < 64 && envp && envp[envc]) envc++;
    /* v0.7.2：AT_RANDOM 种子取宿主 auxv 现成指针——零 syscall。
     * v0.7.0 教训：这里曾用 open("/dev/urandom")，在 sigsys handler 内二次
     * TRAP（SIGSYS 阻塞态）→ syscall 丢弃+信号挂起 → guest 死于 31（真机实锤）。
     * 铁律对 loader 同样适用：handler 路径上的代码禁止任何 TRAP syscall。 */
    const uint8_t* hrnd = (const uint8_t*)getauxval(AT_RANDOM);
    static uint64_t rnd16[2];
    if (hrnd) memcpy(rnd16, hrnd, sizeof(rnd16));

    static char execfn_s[256], platform_s[16];
    snprintf(execfn_s, sizeof(execfn_s), "%s", execfn ? execfn : "guest");
#if defined(__x86_64__)
    snprintf(platform_s, sizeof(platform_s), "x86_64");
#else
    snprintf(platform_s, sizeof(platform_s), "aarch64");
#endif
    /* v0.7.30 修复：字符串区必须**先算够再写**。
     * 旧代码只把 execfn/platform/AT_RANDOM 算进 strs，argv/envp 字符串却从 auxv 后面
     * 往上写 —— 真机 dalvikvm 的 BOOTCLASSPATH 一条就 266B，写完直接越过栈顶：
     * 覆盖 AT_RANDOM/AT_EXECFN/AT_PLATFORM，严重时写进相邻映射。
     * 现在：字符串总字节（argv+envp+execfn+platform+AT_RANDOM 16B）全部预留，
     * 布局与内核一致（字符串在最高处，其次指针数组/auxv，sp 指向 argc）。 */
    size_t strbytes = 16 + strlen(execfn_s) + 1 + strlen(platform_s) + 1;
    for (size_t i = 0; i < argc; i++) strbytes += strlen(argv[i]) + 1;
    for (size_t i = 0; i < envc; i++) strbytes += strlen(envp[i]) + 1;
    strbytes = (strbytes + 15) & ~(size_t)15;
    const size_t auxpairs = 18;                      /* 17 项 + AT_NULL */

    /* v1.2.36 ★★★ 栈规格升级：1MB → 8MB + 底部守护页 + 引擎栈登记。
     * 真机 v1.2.35 符号化实锤（bionic android16-release 源码核对）：
     *   pthread_getattr_np(主线程) → stack_base = [含 startstack 的 maps 区间].hi
     *   − RLIMIT_STACK(8MB)，而 maps [stack]/stat startstack 全是内核栈真值、
     *   线程实跑在这块 mmap 栈上 → ART Thread::Init 的栈边界 CHECK 翻车。
     * 8MB 恰使 bionic 公式自洽（base = hi − 8MB == 本栈 lo）；守护页防
     * 深递归越界踩邻居（内核 [stack] 有 grow-down 保护，mmap 栈没有）。 */
    size_t ssz = 8u << 20;
    void* raw = mmap(NULL, ssz + 4096, PROT_NONE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) return -4;
    void* stk = (char*)raw + 4096;               /* 跳过守护页 */
    if (mprotect(stk, ssz, PROT_READ | PROT_WRITE) != 0) return -4;
    vm_set_exec_stack((uintptr_t)stk, (uintptr_t)stk + ssz,
                      (uintptr_t)stk + ssz - 256);
    size_t total = (1 + argc + 1 + envc + 1) * 8 + auxpairs * 16 + strbytes;
    uintptr_t top = (uintptr_t)stk + ssz - 256;   /* 栈顶内缩 */
    uintptr_t base = (top - total) & ~0xfUL;
    uint64_t* sp = (uint64_t*)base;
    uint64_t* w = sp;
    *w++ = argc;
    for (size_t i = 0; i < argc; i++) *w++ = 0;      /* 占位，回头填 */
    *w++ = 0;
    for (size_t i = 0; i < envc; i++) *w++ = 0;
    *w++ = 0;
    /* auxv */
    uint64_t* ax = w;
    char* strarea = (char*)(ax + auxpairs * 2);
    char* srnd      = strarea;
    char* sexecfn   = srnd + 16;
    char* splatform = sexecfn + strlen(execfn_s) + 1;
    char* sargv     = splatform + strlen(platform_s) + 1;
    *w++ = VA_PHDR;    *w++ = phdr;
    *w++ = VA_PHENT;   *w++ = eh->e_phentsize;
    *w++ = VA_PHNUM;   *w++ = eh->e_phnum;
    *w++ = VA_PAGESZ;  *w++ = 4096;
    *w++ = VA_BASE;    *w++ = ldbias;
    *w++ = VA_FLAGS;   *w++ = 0;
    *w++ = VA_ENTRY;   *w++ = bias + eh->e_entry;
    *w++ = VA_RANDOM;  *w++ = (uint64_t)(uintptr_t)srnd;
    *w++ = VA_EXECFN;  *w++ = (uint64_t)(uintptr_t)sexecfn;
    *w++ = VA_PLATFORM; *w++ = (uint64_t)(uintptr_t)splatform;
    *w++ = VA_HWCAP;   *w++ = getauxval(AT_HWCAP);
    *w++ = VA_CLKTCK;  *w++ = 100;
    *w++ = VA_UID;     *w++ = 0;
    *w++ = VA_EUID;    *w++ = 0;
    *w++ = VA_GID;     *w++ = 0;
    *w++ = VA_EGID;    *w++ = 0;
    *w++ = VA_SECURE;  *w++ = 0;
    *w++ = 0 /*AT_NULL*/; *w++ = 0;

    /* 字符串区：AT_RANDOM(16B) | execfn | platform | argv... | envp... */
    memcpy(srnd, rnd16, 16);
    memcpy(sexecfn, execfn_s, strlen(execfn_s) + 1);
    memcpy(splatform, platform_s, strlen(platform_s) + 1);
    char* p = sargv;
    for (size_t i = 0; i < argc; i++) { size_t l = strlen(argv[i]) + 1; memcpy(p, argv[i], l); p += l; }
    for (size_t i = 0; i < envc; i++) { size_t l = strlen(envp[i]) + 1; memcpy(p, envp[i], l); p += l; }
    char* q = sargv;
    for (size_t i = 0; i < argc; i++) { sp[1 + i] = (uint64_t)(uintptr_t)q; q += strlen(q) + 1; }
    for (size_t i = 0; i < envc; i++) { sp[2 + argc + i] = (uint64_t)(uintptr_t)q; q += strlen(q) + 1; }

    /* v0.7.43: register exec/linker/stack for /proc/self/maps synthesis */
    {
        size_t exe_sz = 0, ld_sz = 0;
        for (int i = 0; i < eh->e_phnum; i++)
            if (ph[i].p_type == PT_LOAD && ph[i].p_vaddr + ph[i].p_memsz > exe_sz)
                exe_sz = (size_t)(ph[i].p_vaddr + ph[i].p_memsz);
        const Elf64_Phdr* lph = (const Elf64_Phdr*)(ldso + lh->e_phoff);
        for (int i = 0; i < lh->e_phnum; i++)
            if (lph[i].p_type == PT_LOAD && lph[i].p_vaddr + lph[i].p_memsz > ld_sz)
                ld_sz = (size_t)(lph[i].p_vaddr + lph[i].p_memsz);
        vm_map_add(bias, exe_sz, PROT_READ | PROT_EXEC, execfn ? execfn : "exe");
        vm_map_add(ldbias, ld_sz, PROT_READ | PROT_EXEC, "/system/bin/linker64");
        vm_map_add((uintptr_t)stk, ssz, PROT_READ | PROT_WRITE, "[stack]");
        vm_set_guest_stack((uintptr_t)stk, ssz);   /* v0.7.45: stat.startstack */
    }
    out->entry = (void (*)(void))(ldbias + lh->e_entry);
    out->sp = base;
    vm_log("exec_dyn: exe bias=0x%llx linker bias=0x%llx entry=0x%llx sp=0x%llx phdr=0x%llx phnum=%u",
           (unsigned long long)bias, (unsigned long long)ldbias,
           (unsigned long long)(uintptr_t)out->entry, (unsigned long long)out->sp,
           (unsigned long long)phdr, (unsigned)eh->e_phnum);
    return 0;
}

void vm_loader_jump(const VmLaunch* l) {
    /* v0.7.85：跳进 guest 之前**强制清空信号掩码**。
     * 若 SIGSYS 此刻处于屏蔽态，guest 的 TRAP 只会让信号"挂起"（系统调用不执行、
     * handler 不进入），表现为"没进 handler 却继续跑/莫名死" —— 正是本项目反复
     * 遇到的怪象。这里用 sigprocmask 把掩码清干净，保证第一条 TRAP 就能进 handler。 */
    {
        sigset_t em;
        sigemptyset(&em);
        sigprocmask(SIG_SETMASK, &em, 0);
    }
#if defined(__x86_64__)
    __asm__ volatile("mov %0, %%rsp\n\tjmp *%1\n"
                     :: "r"(l->sp), "r"(l->entry) : "memory");
#elif defined(__aarch64__)
    __asm__ volatile("mov sp, %0\n\tbr %1\n"
                     :: "r"(l->sp), "r"(l->entry) : "memory");
#endif
    __builtin_unreachable();
}
