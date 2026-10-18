/* TwinVM mini-ylinker —— 用户态动态链接器（致敬 VMOS libylinker）
 * guest 运行时从 ROM 装载 .so：解析 ELF → 段映射 → 依赖链（DT_NEEDED）
 * 递归装载 → 重定位（RELATIVE/GLOB_DAT/JUMP_SLOT）→ 全局符号查找。
 * guest 端纯用户代码，走 VM 的 openat/read（ROM VFS）+ mmap（直达）
 * ——内核无需新 syscall。
 *
 * 4.1 升级（对齐真 bionic linker 的装载语义）：
 *  - DT_NEEDED 递归装载：dlopen(A) 自动先装 A 的依赖（BFS，soname 去重
 *    防循环）——libart 的 libc/libdl/libm 依赖链就是这个形态。
 *  - 全局命名空间：符号查找顺序 = 自身 → 依赖树（深度优先）→ 宿主注册表。
 *    JUMP_SLOT/GLOB_DAT 解析时同一顺序——跨 .so 符号引用（b.so 用 a.so
 *    的函数）在装载期完成绑定。
 *
 * 与完整 rtld 的差异（刻意精简）：
 *  - 宿主符号（guest 侧导出）用注册表而非 guest .dynsym 扫描——确定性、
 *    免 -rdynamic 依赖（宿主表 vm_dl_register_host_symbols）。
 *  - dynsym 符号数用 (strtab - symtab)/24 推导——lld/ld 布局恒真。
 */
#ifndef TWINVM_MINI_LINKER_H
#define TWINVM_MINI_LINKER_H

#include <stdint.h>
#include <stddef.h>

typedef struct VmDlObj {
    uint8_t* base;        /* .so 装载基址（page 对齐） */
    void*    symtab;      /* .dynsym */
    char*    strtab;      /* .dynstr */
    size_t   nsyms;
    char     soname[64];  /* DT_SONAME 或文件路径尾部（命名空间去重键） */
    struct VmDlObj* deps[8];  /* DT_NEEDED 装好的依赖对象 */
    int      ndeps;
} VmDlObj;

typedef struct { const char* name; void* fn; } VmDlSym;

/* 注册宿主可导出符号（guest 在 dlopen 前调用） */
void vm_dl_register_host_symbols(const VmDlSym* syms, size_t n);

/* 装载共享库（含 DT_NEEDED 依赖链递归装载）：成功返回对象句柄，失败 NULL */
void* vm_dlopen(const char* path);

/* 符号查找：自身 → 依赖树 → 宿主注册表 */
void* vm_dlsym(void* handle, const char* name);

#endif
