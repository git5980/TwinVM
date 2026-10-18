/* TwinVM mini-ylinker 实现（VMOS libylinker 同型功能）
 * 4.1：DT_NEEDED 递归装载 + 全局命名空间符号解析 */
#include "mini_linker.h"

#include <elf.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static const VmDlSym* g_host_syms;
static size_t        g_host_nsyms;

/* 装载命名空间（soname 去重 + 循环检测） */
static VmDlObj* g_objs[16];
static int      g_nobjs;

void vm_dl_register_host_symbols(const VmDlSym* syms, size_t n) {
    g_host_syms = syms;
    g_host_nsyms = n;
}

static void* host_lookup(const char* name) {
    for (size_t i = 0; i < g_host_nsyms; i++)
        if (strcmp(g_host_syms[i].name, name) == 0) return g_host_syms[i].fn;
    return NULL;
}

/* .so 自身符号查找（绑定本模块内部引用） */
static void* self_lookup(const VmDlObj* o, const char* name) {
    Elf64_Sym* sym = (Elf64_Sym*)o->symtab;
    for (size_t i = 0; i < o->nsyms; i++) {
        if (sym[i].st_shndx == SHN_UNDEF) continue;
        const char* sn = o->strtab + sym[i].st_name;
        if (strcmp(sn, name) == 0) return o->base + sym[i].st_value;
    }
    return NULL;
}

/* 全局查找：自身 → 依赖树（DFS，已访问剪枝）→ 宿主表。
 * visited 用位图（≤16 对象）防依赖环。 */
static void* global_lookup(const VmDlObj* o, const char* name, unsigned visited) {
    if (!o) return NULL;
    for (int i = 0; i < g_nobjs; i++)
        if (g_objs[i] == o && (visited & (1u << i))) return NULL;   /* 环 */
    for (int i = 0; i < g_nobjs; i++)
        if (g_objs[i] == o) { visited |= (1u << i); break; }

    void* a = self_lookup(o, name);
    if (a) return a;
    for (int i = 0; i < o->ndeps; i++) {
        a = global_lookup(o->deps[i], name, visited);
        if (a) return a;
    }
    return NULL;
}

void* vm_dlsym(void* handle, const char* name) {
    void* a = global_lookup((VmDlObj*)handle, name, 0);
    return a ? a : host_lookup(name);
}

static int prot_of(uint32_t f) {
    int p = 0;
    if (f & PF_R) p |= PROT_READ;
    if (f & PF_W) p |= PROT_WRITE;
    if (f & PF_X) p |= PROT_EXEC;
    return p ? p : PROT_READ;
}

static void soname_of(char* out, size_t cap, const char* path, const char* strtab) {
    /* 优先 DT_SONAME；回退路径尾部（"rom/lib/liba.so" → "liba.so"） */
    out[0] = 0;
    if (strtab && strtab[0]) { snprintf(out, cap, "%s", strtab); return; }
    const char* slash = strrchr(path, '/');
    snprintf(out, cap, "%s", slash ? slash + 1 : path);
}

static void* load_one(const char* path, int depth);

/* DT_NEEDED 依赖装载：need 数组是 strtab 内偏移；缺省目录 /rom/lib/ */
static void load_deps(VmDlObj* o, Elf64_Dyn* dyn) {
    char* strtab = o->strtab;
    Elf64_Dyn* d = dyn;
    /* 先取 SONAME */
    const char* son = NULL;
    for (; d->d_tag != DT_NULL; d++)
        if (d->d_tag == DT_SONAME) son = strtab + d->d_un.d_val;
    soname_of(o->soname, sizeof(o->soname), "", son);

    d = dyn;
    for (; d->d_tag != DT_NULL && o->ndeps < 8; d++) {
        if (d->d_tag != DT_NEEDED) continue;
        const char* n = strtab + d->d_un.d_val;
        /* 命名空间去重：同名对象复用（rtld 同款语义） */
        VmDlObj* found = NULL;
        for (int i = 0; i < g_nobjs; i++)
            if (strcmp(g_objs[i]->soname, n) == 0) { found = g_objs[i]; break; }
        if (!found) {
            char p[256];
            snprintf(p, sizeof(p), "/rom/lib/%s", n);
            found = load_one(p, 0);
            if (!found) { printf("[mini-linker] 依赖缺失: %s\n", n); continue; }
        }
        o->deps[o->ndeps++] = found;
    }
}

static void* load_one(const char* path, int depth) {
    if (depth > 6) { printf("[mini-linker] 依赖层级过深\n"); return NULL; }

    /* 1) 经 VM VFS 读整个 ELF（openat/read → ROM） */
    int fd = open(path, O_RDONLY);
    if (fd < 0) { printf("[mini-linker] open %s 失败\n", path); return NULL; }
    size_t cap = 1 << 20, len = 0;
    uint8_t* img = (uint8_t*)malloc(cap);
    for (;;) {
        if (len == cap) { cap *= 2; img = (uint8_t*)realloc(img, cap); }
        ssize_t n = read(fd, img + len, cap - len);
        if (n <= 0) break;
        len += (size_t)n;
    }
    close(fd);
    if (len < sizeof(Elf64_Ehdr)) { free(img); return NULL; }

    Elf64_Ehdr* eh = (Elf64_Ehdr*)img;
    if (memcmp(eh->e_ident, ELFMAG, 4) != 0 || eh->e_type != ET_DYN) { free(img); return NULL; }

    /* 2) PT_LOAD 总跨度 → 一次性 anon mmap（guest mmap 直达；RWX 简化，
     *    Android 对匿名可执行内存无限制——JIT 同款路径） */
    Elf64_Phdr* ph = (Elf64_Phdr*)(img + eh->e_phoff);
    uintptr_t lo = ~(uintptr_t)0, hi = 0;
    for (int i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_LOAD) {
            uintptr_t s = ph[i].p_vaddr & ~(uintptr_t)0xFFF;
            uintptr_t e = (ph[i].p_vaddr + ph[i].p_memsz + 0xFFF) & ~(uintptr_t)0xFFF;
            if (s < lo) lo = s;
            if (e > hi) hi = e;
        }
    size_t span = hi - lo;
    uint8_t* base = (uint8_t*)mmap(NULL, span, PROT_READ | PROT_WRITE | PROT_EXEC,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) { free(img); return NULL; }
    for (int i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_LOAD)
            memcpy(base + ph[i].p_vaddr, img + ph[i].p_offset, ph[i].p_filesz);

    /* 3) 解析 .dynamic */
    Elf64_Dyn* dyn = NULL;
    for (int i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_DYNAMIC) dyn = (Elf64_Dyn*)(base + ph[i].p_vaddr);
    if (!dyn) { munmap(base, span); free(img); return NULL; }

    Elf64_Rela* rela = NULL; size_t relasz = 0;
    Elf64_Rela* jmprel = NULL; size_t pltrelsz = 0;
    void* symtab = NULL; char* strtab = NULL;
    for (Elf64_Dyn* d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_RELA:    rela = (Elf64_Rela*)(base + d->d_un.d_ptr); break;
        case DT_RELASZ:  relasz = d->d_un.d_val; break;
        case DT_JMPREL:  jmprel = (Elf64_Rela*)(base + d->d_un.d_ptr); break;
        case DT_PLTRELSZ: pltrelsz = d->d_un.d_val; break;
        case DT_SYMTAB:  symtab = base + d->d_un.d_ptr; break;
        case DT_STRTAB:  strtab = (char*)(base + d->d_un.d_ptr); break;
        }
    }

    VmDlObj* o = (VmDlObj*)calloc(1, sizeof(VmDlObj));
    o->base = base;
    o->symtab = symtab;
    o->strtab = strtab;
    /* nsyms 推导：lld/ld 布局中 .dynstr 紧随 .dynsym（24B/项） */
    o->nsyms = strtab && symtab ? ((size_t)((uint8_t*)strtab - (uint8_t*)symtab)) / sizeof(Elf64_Sym) : 0;

    /* 4) 注册进命名空间（重定位符号解析要用——依赖先装好） */
    if (g_nobjs >= 16) { printf("[mini-linker] 命名空间满\n"); munmap(base, span); free(img); free(o); return NULL; }
    g_objs[g_nobjs++] = o;

    /* 4.1：DT_NEEDED 依赖链递归装载（先依赖后重定位——依赖符号此时可解析） */
    load_deps(o, dyn);

    /* 5) 重定位：符号查找顺序 = 自身 → 依赖树 → 宿主表 */
    for (int pass = 0; pass < 2; pass++) {
        Elf64_Rela* tab = pass == 0 ? rela : jmprel;
        size_t tsz = pass == 0 ? relasz : pltrelsz;
        for (size_t off = 0; off + sizeof(Elf64_Rela) <= tsz; off += sizeof(Elf64_Rela)) {
            Elf64_Rela* r = (Elf64_Rela*)((uint8_t*)tab + off);
            uint32_t type = ELF64_R_TYPE(r->r_info);
            uint32_t sidx = ELF64_R_SYM(r->r_info);
            void** slot = (void**)(base + r->r_offset);
#if defined(__aarch64__)
            if (type == R_AARCH64_RELATIVE) {
                *slot = base + r->r_addend;
            } else if (type == R_AARCH64_GLOB_DAT || type == R_AARCH64_JUMP_SLOT) {
                const char* name = strtab + ((Elf64_Sym*)symtab)[sidx].st_name;
                void* addr = global_lookup(o, name, 0);
                if (!addr) addr = host_lookup(name);
                if (!addr) { printf("[mini-linker] 未解析符号: %s\n", name); *slot = NULL; }
                else *slot = addr;
            }
#else
            if (type == R_X86_64_RELATIVE) {
                *slot = base + r->r_addend;
            } else if (type == R_X86_64_GLOB_DAT || type == R_X86_64_JUMP_SLOT) {
                const char* name = strtab + ((Elf64_Sym*)symtab)[sidx].st_name;
                void* addr = global_lookup(o, name, 0);
                if (!addr) addr = host_lookup(name);
                if (!addr) { printf("[mini-linker] 未解析符号: %s\n", name); *slot = NULL; }
                else *slot = addr;
            }
#endif
        }
    }
    free(img);
    return o;
}

void* vm_dlopen(const char* path) {
    /* 命名空间去重：同名 soname 直接复用（rtld 语义） */
    char tail[64];
    const char* slash = strrchr(path, '/');
    snprintf(tail, sizeof(tail), "%s", slash ? slash + 1 : path);
    for (int i = 0; i < g_nobjs; i++)
        if (strcmp(g_objs[i]->soname, tail) == 0) return g_objs[i];
    return load_one(path, 0);
}
