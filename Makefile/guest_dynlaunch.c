/* 静态引导：execve 动态白鼠（PT_INTERP 由 VM 接管） */
#include <unistd.h>
int main(void) {
    char* a[] = {(char*)"/bin/dynhello", (char*)"vm-dyn", 0};
    char* e[] = {(char*)"TWINVM=DYN", 0};
    execve("/bin/dynhello", a, e);
    _exit(98);
}
