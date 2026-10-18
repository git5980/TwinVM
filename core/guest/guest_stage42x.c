/* 4.2 本地复现 guest：execve 动态 PIE（glibc ld.so 接管路径） */
#include <stdio.h>
#include <unistd.h>
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("== TwinVM Stage 4.2 local: dyn exec ==\n");
    char* a[] = {(char*)"dynhello", (char*)"local", NULL};
    char* e[] = {(char*)"LD_LIBRARY_PATH=/lib64", NULL};
    execve("/bin/dynhello", a, e);
    printf("!! execve 失败\n");
    _exit(98);
}
