# TwinVM —— 免 root 用户态安卓虚拟机

从零复刻 VMOS Pro 的用户态虚拟机：**不依赖宿主 zygote**，在普通 App 进程内
跑起完整的 ART（dalvikvm + Runtime + JIT + GC），guest 里执行 Java 代码并输出。

```
容器A：stress-all-pass in 20ms        ← 2026-10-21 Stage 4.2 全线贯通
容器B：world-all-pass in 601ms        ← 2026-10-23 M1 并发双活（独立进程）
```

## 当前状态：Stage 5 进行中（App 容器化）

| Stage | 内容 | 状态 |
|-------|------|------|
| 0~1 | 可行性 + 用户态内核（seccomp/ptrace 双后端、ROM VFS、BPF 生成器） | ✅ |
| 2~3 | exec_dyn 直载 + 真 linker64 自举 | ✅ |
| 4.2 | **ART 全线贯通**：JNI_CreateJavaVM → 类装载 → Java 字节码执行 → stress 10 项全绿 | ✅ 10-21 |
| 5-M1 | **多容器**：串行 ✓ + 并发双活 ✓（A 宿主进程 / B 独立 b-runner） | ✅ 10-23 |
| 5-M2 | APK 直装（真 APK 的 classes.dex 注入容器） | 🚧 进行中 |
| 5-M3/M4 | binder/ActivityThread 预研 → SurfaceFlinger 像素通道 | ⏳ |

## 核心架构（四层）

| 层 | 实现 |
|----|------|
| 拦截层 | seccomp-BPF（USER_NOTIF/SIGSYS）+ ptrace 双后端，guest syscall 全量虚拟化 |
| 用户态内核 | vm_core.c：syscall 语义模拟（VFS/信号/futex/mmap 管线/低 4GB JIT 选址代管） |
| ROM VFS | 自定义 ROM 格式（128MB / 132 文件：system + 5 个 APEX 树），跨进程只读共享 |
| 装载层 | 进程内 ELF 装载 + 真 bionic linker64 拉起真 dalvikvm（exec_dyn，不用内核 execve） |

## 多容器（Stage 5-M1，已验收）

```
宿主 App 进程
  ├─ 容器A：hello.dex（stress 体检）——JNI 直调，前台服务保活
  └─ fork → b-runner 独立进程
              └─ 容器B：world.dex（素数/回文/矩阵/NPE/线程）——纯 C 通道，零 JNI
双栏 UI：单容器单栏；点 B 自动切双栏（左绿 A / 右橙 B 实时尾随）
```

关键机制：fork 后纯 C 通道（`g_c_out_fd`）——fork 只继承调用线程，
ART 运行时锁/JNI 表处于中间态，子进程碰 Java 堆即崩（闪退教训）。

## 仓库布局

```
core/          引擎（vm_core.c / vm_loader.c / vm_rom.c / vm_notif.c …）
android/       App 壳（JNI 桥 + 无 Gradle 构建链：NDK+aapt+ECJ+d8）
rom/            ROM 工具链（rompack/checkrom/syncrom/rom_add）与 fixture 重建说明
tools/         辅助脚本（真机自检循环等）
文档/           修复报告（每版一案一报，v1.2.x 全系列）与里程碑档案
开发路线.md     逐案攻坚史（v0.1 → v1.2.100+，含 10-15 决战日 18 版）
```

## 构建

```bash
cd android && bash build.sh        # 需要 NDK r26+ / build-tools 34 / ECJ / d8
cd core && make test                # 引擎单测 5/5
```

## 详细文档

**[项目说明.md](项目说明.md)** —— 目标 · 当前进度 · 踩坑实录（全程复盘）
**[开发路线.md](开发路线.md)** —— 攻坚编年与教训库
**[文档/里程碑-Stage4.2全线贯通.md](文档/里程碑-Stage4.2全线贯通.md)** —— Stage 4.2 验收档案

## Stage 4.2 攻坚摘（10-19~21，27 版 v1.2.69→95）

假死循环真凶=自伤的速率节流 → close 宽容语义（10 万次→8 次）→
uffd 拒绝（282 恒 EPERM）→ fd 32 位截断规范化 → statfs 假结构体金丝雀
（fsid int[2]）→ 低 4GB rewind+FIXED+落位记账双重校验 → -Xverify:none。
每一案：源码定罪 → 最小修复 → 真机验证。

## License

研究与学习用途；ROM 内含从设备提取的 Android 系统组件（APEX 库/ICU 数据），
不另行分发——按 `rom/fixture/` 的说明从自有设备重建。
