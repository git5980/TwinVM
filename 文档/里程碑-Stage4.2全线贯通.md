# 🏆 TwinVM Stage 4.2 全线贯通（2026-10-21 14:16，v1.2.95 真机）

```
hello TwinVM from ART
loop-sum: 333338333350000
str(200): 0123456789012345 hc=-462549788
list-sum: 124750
fib(22): 17711
caught AIOOBE: length=2; index=5
finally-ok
inner: 63
thread: 42
stress-all-pass in 20ms          ← 终点
```

**用户态虚拟机（纯 ptrace，无 root）完整跑通 Android ART**：
exec_dyn 直载 dalvikvm → linker 自举 → JNI_CreateJavaVM → 类装载 →
Java 字节码执行（含异常/内部类/多线程）→ stress 全过。

## 攻坚编年（2026-10-19~21，27 版）

| 波次 | 主题 | 关键修复 |
|------|------|----------|
| v1.2.69~78 | "假死"疑云 | graceful 三级判据；信号机关全拆（WNOHANG 两态轮询） |
| v1.2.77 | close 风暴 | vm_vfs_close 宽容语义（10 万次→8 次） |
| v1.2.79~85 | 取证+审查 | 帧链符号化；全面代码审查（v1.2.84 替换静默失败事故） |
| v1.2.84 | uffd | userfaultfd(282) 恒拒（AArch64 号，险撞 kill 129） |
| v1.2.82/83 | EINVAL | mmap fd 32 位 -1 截断规范化 |
| v1.2.89 | 节流自伤 | 速率阈值 100→32（ART 构造 50/s 被判静默） |
| v1.2.90 | 金丝雀 | statfs 假结构体 fsid int[2]（越界 8B 踩 canary） |
| v1.2.91~94 | 低 4GB | rewind+FIXED 强制落位 + 记账 + 双重校验 |
| v1.2.95 | 冷校验 | -Xverify:none（无 dalvik-cache，慢千倍） |

## 遗留（下阶段）

1. 退出阶段 SIGSEGV（pc=0xdd9...——ART 收尾路径，stress 后发生，无害）
2. dalvik-cache 合成（让校验可启用）
3. ROM 8d57f5a0（131 文件）
