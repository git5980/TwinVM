# TwinVM Stage 5 预研：zygote 化（一个 VM 起多个 guest 容器）

> 前置：Hello 里程碑（v1.2.54）已证明单 guest 全链可行。
> 本文回答：怎么从"跑一个 dex"走到"跑多个独立 app 环境"。

## 一、现状盘点（可复用资产）

| 资产 | 状态 | zygote 化复用度 |
|------|------|----------------|
| 用户态内核（seccomp/ptrace 双后端） | ✓ 真机稳定 | 直接复用（每 guest 一套） |
| ROM VFS（131 文件，128MB） | ✓ | **共享只读**（mmap 一次全 guest 复用） |
| APEX 真库 + ICU 数据 + core-icu4j | ✓ | 同上（真身只读共享） |
| exec_dyn（进程内 ELF 装载） | ✓ | 每 guest 独立执行 |
| 低 4GB 布局（JIT/堆） | ✓ 引擎代管 | 每 guest 独立布局（防冲突） |
| Binder 用户态驱动 | ✓ 单通道 | 需扩展：多 guest 各自的 SM 表 |

## 二、架构方向（三案对比）

### 案 A：进程级多开（fork 伺服）
真 zygote 思路：第一个 guest 起来后**停在 ready 点**，之后每个新 app
= fork 该进程 + 换 dex。优点：ART 初始化只做一次（~2s）；难点：fork 后
ROM fd/低 4GB 布局/ptrace 关系全要重新校准（ptrace 的 fork 语义、
seccomp TSYNC 都要过）。

### 案 B：多槽位并发（推荐首版）
保持现有"一个 slot 进程 = 一个 guest"，宿主 App 里跑 N 个 slot。
- guest 间天然强隔离（不同进程）；
- 代价：每 guest 重复 ART 启动（~2s）+ ROM 内存（可用 mmap 共享
  压到 ~20MB/guest 增量）；
- 增量改造小：MainActivity 加"容器列表"，每个容器一个 (rom, live_log,
  slot pid) 三元组；watchdog 已按 pid 参数化。

### 案 C：进程内多 ART 实例
一个 slot 进程里多个 JavaVM（JNI Invocation API 天然支持）。难点：
我们的"用户态内核"是进程级单例（g_cpu/g_vfs 全局）→ 需要全面
per-VM 化改造，工作量最大，暂缓。

## 三、案 B 落地清单（建议顺序）

1. **JNI 层多实例化**：twinvm_jni.c 的全局态收进 VmSlot 结构
   （g_live_fd/g_virt/g_fill/g_t_events…），JNI 方法带 slotId；
2. **ROM mmap 共享**：demo.rom 从 assets 落盘一次，多 slot open 同一
   文件 mmap（内核页缓存天然共享，128MB 只占一份物理内存）；
3. **UI 容器化**：MainActivity 容器列表（创建/启动/停止/看日志），
   复用 run 按钮 wiring；
4. **资源限额**：每 slot 的 fd 表/映射表已有上限（VM_MAX_FILES=64/
   VM_MAX_MAPS=128），加 per-slot 停止（kill pid + 清 fd）；
5. **验收样例**：slot1 跑 stress dex、slot2 同时跑 hello dex，
   两份 live 日志独立完整。

## 四、风险预告

- App 进程 fork 限制：Android 对前台 App 的子进程数量/内存有软限
  （lmk 视角），3~5 个 slot 应安全，更多需要前台服务保活（TwinBox
  的 GuestGuardService 经验可移植）;
- 低 4GB 布局冲突：多 slot 是不同进程 → 各自独立地址空间，无冲突
  （这正是案 B 优于案 C 的根本原因）；
- logd 通道：每 slot 的 fd=37 write 已隔离（各自 socket），无互扰。

## 五、工时估算（案 B）

JNI 多实例化（1 天）+ UI 容器列表（0.5 天）+ ROM 共享（0.5 天）+
真机联调（0.5 天）≈ **2.5 天到"双容器并发跑 dex"演示**。
