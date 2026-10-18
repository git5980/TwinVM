/* Stage 4.2 单元测试：动态 PIE 装载协议（不装 seccomp——纯 loader 机制验证）。
 * 白鼠：宿主 glibc 的 ld.so + 一个普通 -pie 程序。
 * 验证点：PIE bias 映射 / auxv 完整性（AT_PHDR/BASE/ENTRY/RANDOM/HWCAP/EXECFN/
 * PLATFORM）/ 栈布局 argc-argv-envp / 真 linker 接管后装载 libc、跑 main、
 * 以 main 的 return 码退出。ld.so 从真实文件系统打开 libc.so.6（本测试
 * 无 VM/ROM——VM 侧 mmap/pread 在真机链路验证）。
 * 用法：./test_loader <pie_exe> <ld.so> [args...] */
#define _GNU_SOURCE
#include "vm_core.h"

#include <stdio.h>
#include <stdarg.h>
void vm_log(const char* fmt, ...) { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); printf("\n"); }
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uint8_t* slurp(const char* p, size_t* n) {
    FILE* f = fopen(p, "rb");
    if (!f) { printf("open %s 失败\n", p); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* b = malloc((size_t)sz);
    if (fread(b, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(b); return NULL; }
    fclose(f);
    *n = (size_t)sz;
    return b;
}

int main(int argc, char** argv) {
    if (argc < 3) { printf("用法: %s <pie_exe> <ld.so>\n", argv[0]); return 2; }
    size_t exez = 0, ldz = 0;
    uint8_t* exe = slurp(argv[1], &exez);
    uint8_t* ld  = slurp(argv[2], &ldz);
    if (!exe || !ld) return 2;
    printf("[test] exe=%zuB ld=%zuB —— 构造 auxv 接管协议\n", exez, ldz);

    char* av[8] = { argv[1], "hello-twinvm", NULL };
    char* ev[4] = { (char*)"PATH=/bin", (char*)"TWINVM=4.2", NULL };
    VmLaunch L;
    int r = vm_loader_exec_dyn(exe, exez, ld, ldz, argv[1], av, ev, &L);
    if (r != 0) { printf("[test] loader 失败: %d\n", r); return 1; }
    printf("[test] bias 装载完成，跳 ld.so 入口 %p（栈 %p）——链接器接管\n", (void*)L.entry, (void*)L.sp);
    vm_loader_jump(&L);   /* 不返回：成功 = 打印 + dynhello 的退出码 */
    return 0;
}
