# TwinVM Stage 5 开工方案（App 容器化 · 案 B：多槽位并发）

> 前置：Stage 4.2 全线贯通（stress-all-pass 20ms，v1.2.95）。
> 本文 = 预研文档（Stage5-预研-zygote化.md）的案 B 落地版。

## 里程碑定义（M1→M4）

**M1 双容器并发跑 dex**（本阶段验收）：
```
容器A：跑 stress dex（stress-all-pass）
容器B：同时跑 hello dex（hello TwinVM）
两份 live 日志独立、可分别启停
```

**M2 APK 直装**：把一个真 APK 的 classes.dex 注入 ROM /data/app/，
dalvikvm -cp 直接跑它的 dex（无 PackageManager，先跑"无 UI 的 APK"
——例如含 main() 的工具类 APK 或 Instrumentation）。

**M3 App 进程模型**：ActivityThread/binder 预研（zygote fork 模拟）。

**M4 完整 App**：带 UI 的 App（需 SurfaceFlinger 通道——预研像素缓冲
转发，参考 TwinBox 双生盒的嫁接经验）。

## M1 技术方案（今天开工）

### 1. JNI 多实例化（核心工程）
`twinvm_jni.c` 全局态收进 `VmSlot` 结构：
```
struct VmSlot {
  int live_fd;            /* 每 slot 独立日志 */
  pid_t guest_pid;
  /* tracer 线程独立 */ pthread_t tracer_thr;
  /* 虚拟化待办表、计数器、探针配额… */
};
static VmSlot g_slots[TVM_MAX_SLOTS];   /* 首版 2 个 */
JNI 方法带 slotId 参数（0/1）。
```
难点：g_cpu/g_vfs（引擎单例）——**fork 后每 slot 进程天然独立**！
（guest 在 slot 进程里，引擎也在其中——多 slot = 多 fork = 各自一份，
无需 per-VM 化——案 B 的根本优势兑现点）

### 2. 宿主侧多 slot 编排
MainActivity：容器列表（A/B），每个容器独立（运行按钮/日志页/停止）。
RunService 保活已有。

### 3. ROM 共享
两 slot open 同一 demo.rom（内核页缓存共享，物理内存只一份）。

## 风险
- 宿主 App 的子进程数量软限（3~5 个安全：2 slot + 探针）
- 双 slot 同时 ptrace：互不干扰（各 tracer 线程管各的子进程）
