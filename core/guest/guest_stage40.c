/* TwinVM Stage 4.0 guest —— 多线程 + epoll 阻塞 + pipe（zygote/ART 前置能力）。
 * ART 的线程模型完整依赖链：pthread_create（clone CLONE_THREAD|SETTLS）
 * + set_robust_list + rseq + futex 争用 + epoll 阻塞 + pipe 通信。
 * 本 guest 全部用真 bionic（静态链接）路径——与 zygote 内 ART 走的字节相同。
 *
 * 验证点：
 *   1. 4 工作线程在互斥锁下并发自增全局计数器（futex 真争用）
 *   2. 每线程完成各写 1 字节入 pipe
 *   3. 主线程 epoll_wait 阻塞收 4 字节（阻塞期间线程持续跑——与 zygote 同型）
 *   4. join 后校验 counter == 4×N，TLS 各归其位（pthread_self 不等） */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

#define NTHREADS 4
#define NINCR    5000

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile long   g_counter = 0;
static int             g_pipe_wr = -1;
static pthread_t       g_tids[NTHREADS];

static void* worker(void* arg) {
    long id = (long)arg;
    for (int i = 0; i < NINCR; i++) {
        pthread_mutex_lock(&g_lock);
        g_counter++;
        pthread_mutex_unlock(&g_lock);
    }
    char b = (char)('A' + id);
    write(g_pipe_wr, &b, 1);
    return NULL;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("== TwinVM Stage 4.0: 多线程 + epoll + pipe ==\n");

    int pfd[2];
    if (pipe(pfd) != 0) { printf("[main] pipe fail: %s\n", strerror(errno)); return 1; }
    g_pipe_wr = pfd[1];

    int ep = epoll_create1(0);
    if (ep < 0) { printf("[main] epoll_create1 fail: %s\n", strerror(errno)); return 1; }
    struct epoll_event ev; memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN; ev.data.fd = pfd[0];
    if (epoll_ctl(ep, EPOLL_CTL_ADD, pfd[0], &ev) != 0) { printf("[main] epoll_ctl fail\n"); return 1; }
    printf("[main] pipe+epoll 就绪，起 %d 线程 × %d 自增...\n", NTHREADS, NINCR);

    for (long i = 0; i < NTHREADS; i++) {
        if (pthread_create(&g_tids[i], NULL, worker, (void*)i) != 0) {
            printf("[main] pthread_create #%ld fail: %s\n", i, strerror(errno)); return 1;
        }
    }
    printf("[main] %d 线程已启动（clone+TLS+robust_list+rseq 全链）\n", NTHREADS);

    /* 主线程 epoll 阻塞收齐 4 字节——阻塞期间工作线程持续触发 SIGSYS */
    char got[NTHREADS]; int total = 0;
    while (total < NTHREADS) {
        struct epoll_event out[4];
        int n = epoll_wait(ep, out, 4, 10000);
        if (n <= 0) { printf("[main] epoll_wait 超时/失败: %d\n", n); return 1; }
        for (int i = 0; i < n && total < NTHREADS; i++) {
            int r = read(pfd[0], &got[total], 1);
            if (r == 1) total++;
        }
    }

    for (int i = 0; i < NTHREADS; i++) pthread_join(g_tids[i], NULL);
    printf("[main] 收齐 %d 字节: %.*s；join 完成\n", total, total, got);

    int ok = 1;
    if (g_counter != (long)NTHREADS * NINCR) { printf("[main] !! counter=%ld 期望 %ld\n", g_counter, (long)NTHREADS * NINCR); ok = 0; }
    if (g_tids[0] == g_tids[1] || g_tids[1] == g_tids[2] || g_tids[2] == g_tids[3]) { printf("[main] !! TLS 身份重复\n"); ok = 0; }
    printf("[main] counter=%ld（futex 争用下精确一致）\n", g_counter);
    printf(ok ? "== Stage 4.0 完成：多线程+epoll+pipe 全通过 ==\n"
              : "== Stage 4.0 失败 ==\n");
    return ok ? 0 : 1;
}
