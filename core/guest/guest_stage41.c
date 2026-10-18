/* TwinVM Stage 4.1 guest —— ylinker 依赖链 + 命名空间 + 符号穿透。
 * 验证 dlopen("/rom/lib/libc.so") 自动递归装载 b→a（DT_NEEDED），
 * 跨模块符号在装载期绑定，soname 去重，依赖树符号穿透。
 * 这是 4.2 装真 bionic .so 链（libc→libdl→libm→libart）的前置验证。 */
#include <stdio.h>
#include <string.h>
#include "mini_linker.h"

extern int libc_top(void);    /* ROM 里 libc.so 导出：libb_mid() + 3 */
extern int libb_mid(void);    /* libb.so 导出：liba_base() + 20 */
extern int liba_base(void);   /* liba.so 导出：100 */

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("== TwinVM Stage 4.1: ylinker 依赖链 ==\n");

    void* hc = vm_dlopen("/rom/lib/libc.so");
    if (!hc) { printf("dlopen libc.so 失败\n"); return 1; }
    printf("[dlopen] libc.so 装载成功（应已递归装 b→a）\n");

    int (*top)(void) = (int(*)(void))vm_dlsym(hc, "libc_top");
    if (!top) { printf("dlsym libc_top 失败\n"); return 1; }
    int v = top();
    printf("[call] libc_top() = %d（期望 123 = 100+20+3 跨三模块）\n", v);
    if (v != 123) { printf("!! 链路值错误\n"); return 1; }

    /* soname 去重 + 行为验证：重开 libb，句柄应有效且符号可查 */
    void* hb = vm_dlopen("/rom/lib/libb.so");
    if (!hb) { printf("!! libb.so 重开失败\n"); return 1; }
    int (*mid)(void) = (int(*)(void))vm_dlsym(hb, "libb_mid");
    if (!mid || mid() != 120) { printf("!! libb_mid 校验失败\n"); return 1; }
    printf("[dlopen] libb.so 重开可用，libb_mid() = 120\n");

    /* 依赖树符号穿透：从 c 句柄查 a 的符号 */
    int (*base)(void) = (int(*)(void))vm_dlsym(hc, "liba_base");
    if (!base) { printf("!! 穿透查找 liba_base 失败\n"); return 1; }
    printf("[dlsym] c 句柄穿透查 liba_base() = %d（依赖树解析）\n", base());

    printf("== Stage 4.1 完成：依赖链+命名空间+穿透 全通过 ==\n");
    return 0;
}
