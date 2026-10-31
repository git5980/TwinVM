/* TwinVM Stage 3.1 guest —— binder 事务闭环演示（futex 阻塞 + mmap 零拷贝）。
 * 父：fork+execve server 与 client（回归 2.1）；server 注册 context mgr，
 * client 发 BC_TRANSACTION，server 收 BR_TRANSACTION 回 BC_REPLY，
 * client 收 BR_REPLY——全部经用户态 binder 驱动。
 * 3.1 新用法（对齐真 binder）：
 *   - guest mmap(/dev/binder) 拿事务缓冲段 → 数据走共享区零拷贝（ptrace 后端
 *     降级拷贝模式，同一份 guest 代码两路都对）
 *   - WRITE_READ 阻塞语义：sigsys 后端驱动内 futex_wait，被对端唤醒后返回；
 *     ptrace 后端 read_consumed=0 立即返回 → guest usleep 兜底重试
 *   - payload 地址统一从 td.data_buffer 取（零拷贝=共享区指针 / 拷贝=内嵌偏移） */
#include <stdint.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define BINDER_WRITE_READ     _IOWR('b', 1, 64)
#define BINDER_SET_CONTEXT_MGR _IOW('b', 7, 4)
#define BC_TRANSACTION _IOW('c', 0, 64)
#define BC_REPLY       _IOW('c', 1, 64)
#define BR_TRANSACTION _IOR('r', 2, 64)
#define BR_REPLY       _IOR('r', 3, 64)
#define BR_NOOP        _IOR('r', 12, 4)

typedef struct {
    uint64_t write_size, write_consumed, write_buffer;
    uint64_t read_size, read_consumed, read_buffer;
} Bwr;

typedef struct {
    uint32_t target_handle; uint32_t pad0;
    uint64_t cookie;
    uint64_t code;
    uint64_t flags;
    int64_t  sender_pid;
    uint64_t data_size, offsets_size, data_buffer;
} Txn;

#define PAYLOAD "PING-TWINVM-BINDER"
#define REPLY   "PONG-FROM-MGR"

/* 读回调命令（br 码 + td），payload 指针统一从 td.data_buffer 取 */
static int read_br(const uint8_t* rb, uint32_t* br, Txn* td) {
    memcpy(br, rb, 4);
    if (*br != (uint32_t)BR_TRANSACTION && *br != (uint32_t)BR_REPLY &&
        *br != (uint32_t)BR_NOOP) return -1;
    memcpy(td, rb + 4, sizeof(*td));
    return 0;
}

static int server_main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("[server] 启动，打开 /dev/binder...\n");
    int fd = open("/dev/binder", O_RDWR);
    if (fd < 0) { printf("[server] open binder fail\n"); return 1; }

    char* seg = (char*)mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (seg == MAP_FAILED) printf("[server] mmap 段不可用 → 拷贝模式（本地路径）\n");
    else printf("[server] mmap 零拷贝段 = %p\n", (void*)seg);

    if (ioctl(fd, BINDER_SET_CONTEXT_MGR, 0) != 0) { printf("[server] set mgr fail\n"); return 1; }
    printf("[server] context mgr 就绪，阻塞等待事务（futex）...\n");

    for (int noop_n = 0; noop_n < 3; noop_n++) {   /* 20s 空闲×3 = 60s 总上限 */
        uint8_t rb[512];
        Bwr bwr; memset(&bwr, 0, sizeof(bwr));
        bwr.read_size = sizeof(rb);
        bwr.read_buffer = (uintptr_t)rb;
        if (ioctl(fd, BINDER_WRITE_READ, &bwr) != 0) { printf("[server] bwr fail\n"); return 1; }
        if (bwr.read_consumed == 0) { usleep(50000); continue; }   /* ptrace 兜底 */
        uint32_t br; Txn td;
        if (read_br(rb, &br, &td) != 0) { printf("[server] 未知 br 码\n"); return 1; }
        if (br == (uint32_t)BR_NOOP) { printf("[server] 空闲 NOOP #%d\n", noop_n); continue; }
        if (br != (uint32_t)BR_TRANSACTION) { printf("[server] 期望 BR_TRANSACTION\n"); return 1; }

        const char* payload = (const char*)(uintptr_t)td.data_buffer;
        printf("[server] BR_TRANSACTION code=%u from=%d payload=%.*s（%s）\n",
               td.code, (int)td.sender_pid, (int)td.data_size, payload,
               seg != MAP_FAILED ? "零拷贝" : "拷贝");
        if (strncmp(payload, PAYLOAD, td.data_size) != 0) {
            printf("[server] 校验失败!\n"); return 1;
        }

        /* BC_REPLY：REPLY 写进自己的 mmap 段 → data_buffer=段地址（零拷贝） */
        Bwr bwr2; memset(&bwr2, 0, sizeof(bwr2));
        uint8_t wb[256];
        uint32_t cmd = (uint32_t)BC_REPLY;
        Txn out; memset(&out, 0, sizeof(out));
        out.pad0 = td.pad0;                       /* 驱动私有位：txn 索引回传 */
        out.data_size = strlen(REPLY) + 1;
        if (seg != MAP_FAILED) {
            memcpy(seg, REPLY, out.data_size);
            out.data_buffer = (uintptr_t)seg;
        } else {
            static char emb[64]; memcpy(emb, REPLY, out.data_size);
            out.data_buffer = (uintptr_t)emb;
        }
        memcpy(wb, &cmd, 4); memcpy(wb + 4, &out, sizeof(out));
        bwr2.write_size = 4 + sizeof(out);
        bwr2.write_buffer = (uintptr_t)wb;
        if (ioctl(fd, BINDER_WRITE_READ, &bwr2) != 0) { printf("[server] reply fail\n"); return 1; }
        printf("[server] BC_REPLY 已发（%s）\n", seg != MAP_FAILED ? "零拷贝段" : "拷贝缓冲");
        return 7;
    }
    printf("[server] 60s 无事务，超时退出\n");
    return 1;
}

static int client_main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("[client] 启动...\n");
    int fd = open("/dev/binder", O_RDWR);
    if (fd < 0) { printf("[client] open binder fail\n"); return 1; }

    char* seg = (char*)mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (seg == MAP_FAILED) printf("[client] mmap 段不可用 → 拷贝模式（本地路径）\n");
    else printf("[client] mmap 零拷贝段 = %p\n", (void*)seg);

    for (int retry = 0; retry < 10; retry++) {    /* 等 mgr 注册：BR_NOOP→100ms 重试 */
        uint8_t rb[512];
        Bwr bwr; memset(&bwr, 0, sizeof(bwr));
        uint8_t wb[256];
        uint32_t cmd = (uint32_t)BC_TRANSACTION;
        Txn out; memset(&out, 0, sizeof(out));
        out.code = 0x42;
        out.data_size = strlen(PAYLOAD) + 1;
        if (seg != MAP_FAILED) {
            memcpy(seg, PAYLOAD, out.data_size);
            out.data_buffer = (uintptr_t)seg;
        } else {
            static char emb[64]; memcpy(emb, PAYLOAD, out.data_size);
            out.data_buffer = (uintptr_t)emb;
        }
        memcpy(wb, &cmd, 4); memcpy(wb + 4, &out, sizeof(out));
        bwr.write_size = 4 + sizeof(out);
        bwr.write_buffer = (uintptr_t)wb;
        bwr.read_size = sizeof(rb);               /* 单次 WRITE_READ：发+阻塞收 */
        bwr.read_buffer = (uintptr_t)rb;
        if (ioctl(fd, BINDER_WRITE_READ, &bwr) != 0) { printf("[client] bwr fail\n"); return 1; }
        printf("[client] BC_TRANSACTION 已发 (code=0x42, %u 字节%s)——阻塞等回复...\n",
               (unsigned)out.data_size, seg != MAP_FAILED ? "，零拷贝" : "");

        for (;;) {   /* ptrace 兜底轮询；sigsys 首轮即阻塞返回带数据 */
            if (bwr.read_consumed == 0) {
                usleep(50000);
                Bwr bwr2; memset(&bwr2, 0, sizeof(bwr2));
                bwr2.read_size = sizeof(rb);
                bwr2.read_buffer = (uintptr_t)rb;
                if (ioctl(fd, BINDER_WRITE_READ, &bwr2) != 0) { printf("[client] poll fail\n"); return 1; }
                bwr = bwr2;   /* struct 赋值替换（write 侧已清） */
                continue;
            }
            uint32_t br; Txn td;
            if (read_br(rb, &br, &td) != 0) { printf("[client] 未知 br 码\n"); return 1; }
            if (br == (uint32_t)BR_NOOP) { printf("[client] mgr 未就绪，100ms 后重试\n"); usleep(100000); break; }
            if (br != (uint32_t)BR_REPLY) { printf("[client] 期望 BR_REPLY\n"); return 1; }
            const char* payload = (const char*)(uintptr_t)td.data_buffer;
            printf("[client] BR_REPLY data=%.*s（%s）\n", (int)td.data_size, payload,
                   seg != MAP_FAILED ? "零拷贝" : "拷贝");
            if (strncmp(payload, REPLY, td.data_size) != 0) { printf("[client] 回复校验失败!\n"); return 1; }
            printf("[client] binder 事务闭环完成\n");
            return 9;
        }
    }
    printf("[client] 超时无回复\n");
    return 1;
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 1 && strcmp(argv[1], "server-mode") == 0) return server_main();
    if (argc > 1 && strcmp(argv[1], "client-mode") == 0) return client_main();

    printf("== TwinVM Stage 3.1: binder futex 阻塞 + mmap 零拷贝 ==\n");
    printf("[parent] fork server...\n"); fflush(stdout);
    pid_t sp = fork();
    if (sp == 0) {
        char* a[] = {(char*)"binder_demo", (char*)"server-mode", NULL};
        execve("/bin/binder_demo", a, NULL);
        _exit(98);
    }
    printf("[parent] fork client...\n"); fflush(stdout);
    pid_t cp = fork();
    if (cp == 0) {
        char* a[] = {(char*)"binder_demo", (char*)"client-mode", NULL};
        execve("/bin/binder_demo", a, NULL);
        _exit(98);
    }
    int st1, st2;
    waitpid(sp, &st1, 0);
    waitpid(cp, &st2, 0);
    int ok = (WIFEXITED(st1) && WEXITSTATUS(st1) == 7 && WIFEXITED(st2) && WEXITSTATUS(st2) == 9);
    printf("[parent] server=%d client=%d → %s\n",
           WIFEXITED(st1) ? WEXITSTATUS(st1) : -WTERMSIG(st1),
           WIFEXITED(st2) ? WEXITSTATUS(st2) : -WTERMSIG(st2),
           ok ? "binder 全环验证通过（futex 阻塞语义）" : "失败");
    printf("== Stage 3.1 完成 ==\n");
    return ok ? 0 : 1;
}
