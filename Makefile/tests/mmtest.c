/* sigsys 环境下 mmap(fd=10000) TRAP 验证 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <sys/mman.h>
int main(void) {
    void* p = mmap((void*)0x500000000000, 4096, PROT_READ, MAP_FIXED|MAP_PRIVATE, 10000, 0);
    printf("mmap(10000) = %p (期望 TRAP 后走 VM 语义)\n", p);
    return 0;
}
