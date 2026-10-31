/* TwinVM v0.7.24 白盒回归：直发通道护栏（vm_syscall6 直发前查过滤器）
 *
 * 背景（真机实锤 2026-10-13 17:09）：判决表里有一处 VM_PT3(203, ...)（connect）
 * 直发，但 connect(203) 并不在 BPF 放行清单里 → handler 内裸 svc 被自家过滤器
 * 二次 TRAP（此刻 SIGSYS 已阻塞）→ syscall 被丢弃 / 信号挂起 → 进程 code=-31。
 * 这类事故的根因是"直发集合 ⊆ 放行清单"只靠人工维护的表 + 启动自检，新加一处直发
 * 只要忘了登记就静默通过。v0.7.24 把护栏下沉到唯一通道 vm_syscall6()：
 * 每次直发前用 vm_filter_action() 问一遍过滤器，不放行就拒绝直发（-ENOSYS）+ 告警。
 *
 * 本测试用 #include vm_core.c 直接调 static 的 vm_syscall6，验证：
 *   1) 未放行的直发被拒绝（返回 -ENOSYS，且没有真的发出 svc）；
 *   2) 已放行的直发照常下到内核（不会误伤）；
 *   3) vm_passthrough_nrs / vm_engine_syscall_nrs 里每一项护栏都放行。
 *
 * 编译：gcc -O2 -Iinclude -o build/guardtest tests/guardtest.c \
 *        src/vm_backend_sigsys.c src/vm_rom.c src/vm_binder.c tests/compat/sys/reg.h
 *      （测试里 include 了 src/vm_core.c，故 vm_core.c 不再单独编译）
 */
#include "../src/vm_core.c"

#include <stdio.h>
#include <string.h>

static int g_fail;
static void ck(int ok, const char* what) {
    printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
    if (!ok) g_fail++;
}

int main(void) {
    printf("== v0.7.24 回归: 直发通道护栏（拒绝未放行的 handler 内裸 svc）==\n");
    vm_console_fd = 2;

    printf("-- 1. 未放行的直发必须被拒绝\n");
    {
        /* connect + 虚拟 fd(10000)：过滤器判 TRAP → 护栏拒绝，返回 -ENOSYS。
         * 若不拒绝，就会真的发出 connect(10000,...) 裸 svc（内核回 -EBADF），
         * 而在装了过滤器的真机上则是二次 TRAP → SIGSYS 挂起/崩槽位。 */
        long r = vm_syscall6(VM_NR_connect, 10000, 0, 0, 0, 0, 0);
        printf("       vm_syscall6(connect, fd=10000) = %ld\n", r);
        ck(r == -VM_ENOSYS, "虚拟 fd 的 connect 直发被拒绝（-ENOSYS）");
        long r2 = vm_syscall6(VM_NR_socket, 2 /* AF_INET，非 AF_UNIX */, 0, 0, 0, 0, 0);
        printf("       vm_syscall6(socket, AF_INET) = %ld\n", r2);
        ck(r2 == -VM_ENOSYS, "非 AF_UNIX 的 socket 直发被拒绝（-ENOSYS）");
    }

    printf("-- 2. 已放行的直发不能误伤（照常下内核）\n");
    {
        /* connect + 真 fd(5)：BPF 判 ALLOW → 护栏放行 → 真发出 svc。
         * fd 5 不是 socket，内核会回 -EBADF/-ENOTSOCK，关键是与 -ENOSYS 不同。 */
        long r = vm_syscall6(VM_NR_connect, 5, 0, 0, 0, 0, 0);
        printf("       vm_syscall6(connect, fd=5) = %ld（内核错误码，非护栏拒绝）\n", r);
        ck(r != -VM_ENOSYS, "真 fd 的 connect 直发正常下到内核");
        long r2 = vm_syscall6(VM_NR_futex, 0, 0, 0, 0, 0, 0);
        ck(r2 != -VM_ENOSYS, "futex（放行清单内）直发正常下到内核");
    }

    printf("-- 3. 两张表里每一项护栏都放行\n");
    {
        int bad = 0;
        for (int i = 0; i < vm_passthrough_nr_count; i++)
            if (vm_filter_action(vm_passthrough_nrs[i], 0, 0, 0, 0, -1, 0) != VM_FILTER_ALLOW) {
                printf("       判决表直发 nr=%ld 未放行\n", vm_passthrough_nrs[i]); bad++;
            }
        /* v0.7.76：引擎自用表已清空（只剩 -1 哨兵）——哨兵要跳过，
         * 否则会把 -1 当"未放行的 syscall"报假失败。 */
        for (int i = 0; i < vm_engine_syscall_nr_count; i++) {
            if (vm_engine_syscall_nrs[i] < 0) continue;
            if (vm_filter_action(vm_engine_syscall_nrs[i], 0, 0, 0, 0, 0, 0) != VM_FILTER_ALLOW) {
                printf("       引擎自用 nr=%ld 未放行\n", vm_engine_syscall_nrs[i]); bad++;
            }
        }
        ck(bad == 0, "判决表直发 + 引擎自用 syscall 全部放行");
    }

    printf("-- 4. connect 策略（v0.7.24：真 fd 原生直通 / 虚拟 fd 只回错误）\n");
    {
        long ok_real = vm_filter_action(VM_NR_connect, 5, 0, 0, 0, 0, 0);
        long virt    = vm_filter_action(VM_NR_connect, 10000, 0, 0, 0, 0, 0);
        printf("       connect(fd=5) -> 0x%lx   connect(fd=10000) -> 0x%lx\n",
               (unsigned long)ok_real, (unsigned long)virt);
        ck(ok_real == VM_FILTER_ALLOW, "connect(真 fd) 判 ALLOW —— ART 的 logd 连接可以原生跑");
        ck(virt != VM_FILTER_ALLOW, "connect(虚拟 fd) 判 TRAP —— 由判决表回错误，绝不直发");
    }

    printf("\n%s\n", g_fail ? "FAIL" : "PASS: 直发护栏生效（未放行的裸 svc 被挡在本地）");
    return g_fail ? 1 : 0;
}
