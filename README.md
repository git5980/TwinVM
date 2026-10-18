# TwinVM —— 免 root 用户态安卓虚拟机

从零复刻 VMOS Pro 的用户态虚拟机：**不依赖宿主 zygote**，在普通 App 进程内
跑起完整的 ART（dalvikvm + Runtime + JIT + GC），guest 里执行 Java 代码并输出。

```
[TwinVM] [tee] fd=1 write（22 B）: hello TwinVM from ART   ← 2026-10-18 达成
```

## 核心架构（四层）

| 层 | 实现 |
|----|------|
| 拦截层 | seccomp-BPF（USER_NOTIF/SIGSYS）+ ptrace 双后端，guest syscall 全量虚拟化 |
| 用户态内核 | vm_core.c：syscall 语义模拟（VFS/信号/futex/mmap 管线/低 4GB JIT 选址代管） |
| ROM VFS | 自定义 ROM 格式（128MB / 131 文件：system + 5 个 APEX 树），跨进程只读共享 |
| 装载层 | 进程内 ELF 装载（ylinker 路线）+ 真 bionic linker64 拉起真 dalvikvm |

## 仓库布局

```
core/          引擎（vm_core.c / vm_loader.c / vm_rom.c / vm_notif.c …）
android/       App 壳（JNI 桥 + 无 Gradle 构建链：NDK+aapt+ECJ+d8）
rom/           ROM 工具链（rompack/checkrom/syncrom）与 fixture 重建说明
tools/         辅助脚本（真机自检循环等）
文档/          修复报告（每版一案一报）与里程碑档案
开发路线.md    逐案攻坚史（v0.1 → v1.2.55，含 10-15 决战日 18 版）
```

## 构建

```bash
cd android && bash build.sh        # 需要 NDK r26+ / build-tools 34 / ECJ / d8
```

## 状态（2026-10-18）

- **Hello 里程碑达成**：guest Java 全链输出（v1.2.54）
- 优雅关机 + 压力 dex + 消音（v1.2.55，真机验证中）
- 下一步：Stage 5 zygote 化（见 文档/Stage5-预研-zygote化.md）

## 攻坚史摘（两日十九版）

栈账本 → SEIZE 对抗 → fd 所有权 → public.libraries → 属性区 → /proc realpath →
ld.config links → 假 namespace（TLS IE）→ com_android_art → APEX 37 真库 →
env 三件套 → ICU 30MB 数据 → core-icu4j → /data 树 → JIT Low4GB 代管 →
137 号张冠李戴 → ptrace dispatch——每一案：源码定罪 → 最小修复 → 真机验证。

## License

研究与学习用途；ROM 内含从设备提取的 Android 系统组件（APEX 库/ICU 数据），
不另行分发——按 `rom/fixture/` 的说明从自有设备重建。
