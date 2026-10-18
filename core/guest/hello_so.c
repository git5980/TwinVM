/* Stage 2.2 的 ROM .so —— 被 mini-ylinker 在 VM 内装载的共享库。
 * 导入 vm_guest_api（guest 注册的宿主符号）→ 演示真·跨模块符号解析。 */
int vm_guest_api(int x);

int so_compute(int x) {
    return vm_guest_api(x) * 2;          /* 调宿主 → PLT/JUMP_SLOT 重定位 */
}

const char* so_message(void) {
    return "hello from a .so dynamically linked INSIDE the VM (mini-ylinker)";
}
