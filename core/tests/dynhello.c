/* 4.2 白鼠：动态 glibc PIE——验证 argc/argv/envp/auxv 全链 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char** argv, char** envp) {
    const char* tv = "未找到";
    for (char** e = envp; *e; e++) if (strncmp(*e, "TWINVM=", 7) == 0) tv = *e + 7;
    printf("hello from dynamic glibc! argc=%d argv0=%s argv1=%s TWINVM=%s\n",
           argc, argv[0], argc > 1 ? argv[1] : "-", tv);
    return 42;
}
