/* TwinVM Stage 1 guest —— 静态 bionic（真 libc）。
 * 每一行 libc 背后都是被用户态内核截获服务的系统调用：
 * printf→write(全缓冲)  malloc→brk/mmap  fopen→openat/read/fstat  pthread→clone/futex
 * 它与宿主之间隔着一整层虚拟内核——这是"虚拟机里的程序"。
 */
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

static void* thread_fn(void* arg) {
    (void)arg;
    printf("[guest-thread] 虚拟机内线程存活（clone+futex 透传）\n");
    return NULL;
}

int main(void) {
    printf("== TwinVM Stage 1: 静态 bionic guest ==\n");

    /* 1) malloc：堆 brk/mmap 全走用户态内核 */
    size_t N = (size_t)1 << 20;
    char* p = (char*)malloc(N);
    memset(p, 0xAB, N);
    printf("malloc+memset 1MB ok, p[12345]=%02x\n", (unsigned char)p[12345]);
    free(p);

    /* 2) stdio 走 ROM VFS（openat→fstat→read→close） */
    FILE* f = fopen("/rom/hello.txt", "r");
    if (f) {
        char buf[128];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = 0;
        printf("fread ROM: %s", buf);
        fclose(f);
    } else {
        printf("fopen ROM fail?!\n");
    }

    /* 3) 隔离：宿主文件系统必须不可见 */
    FILE* g = fopen("/etc/passwd", "r");
    printf("隔离: /etc/passwd -> %s\n", g ? "泄漏!" : "不可见 (正确)");
    if (g) fclose(g);

    /* 4) uname：VM 身份 */
    struct utsname u;
    if (uname(&u) == 0)
        printf("uname: %s %s %s\n", u.sysname, u.release, u.machine);

    /* 5) 目录枚举（getdents64） */
    DIR* d = opendir("/rom");
    if (d) {
        struct dirent* e;
        printf("ROM 目录:");
        while ((e = readdir(d)) != NULL) printf(" %s", e->d_name);
        printf("\n");
        closedir(d);
    }

    /* 6) 虚拟机内多线程 */
    pthread_t t;
    if (pthread_create(&t, NULL, thread_fn, NULL) == 0) pthread_join(t, NULL);

    printf("== Stage 1 guest 完成 ==\n");
    return 0;
}
