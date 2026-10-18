# 🏆 里程碑：Hello TwinVM from ART（2026-10-18 13:59）

```
[TwinVM] [tee] fd=1 write（22 B）: hello TwinVM from ART
```

日志 107678 行。guest 里的 Java 代码（/rom/hello.dex）：

```java
public class Hello {
    public static void main(String[] args) {
        System.out.println("hello TwinVM from ART");
    }
}
```

经 ART 完整执行：解释器 → sget System.out → const-string → invoke println →
libjavacore 的 write(1) → 虚拟控制台 → 引擎 tee 落盘。

## 意义

**虚拟机心跳正式确认**：用户态内核（seccomp+SIGSYS/ptrace）+ ROM VFS +
进程内 ELF 装载 + 真身 linker/dalvikvm/ART 全链跑通——Java 代码在完全
脱离宿主 zygote 的环境里执行并输出。

## 攻坚全程（v1.2.36→54，19 版，单日 10-17~10-18）

| 版本 | 破的案 |
|------|--------|
| v1.2.37 | 栈账本三行成套（Thread::Init CHECK） |
| v1.2.38-39 | SEIZE→TRACEME 回退（ptrace 环境对抗） |
| v1.2.37 | logd fd 所有权（socket 原生化） |
| v1.2.40 | public.libraries.txt（nativeloader FATAL） |
| v1.2.41 | /dev/__properties__ 属性区合成 |
| v1.2.43 | /proc/self/exe realpath（ld.config 段匹配） |
| v1.2.44 | links 行 shared_libs（配置整份无效案） |
| v1.2.45 | 假 "system" namespace（libc 重装→TLS IE） |
| v1.2.46 | com_android_art（APEX namespace） |
| v1.2.47 | APEX 真库 37 个（art 29+i18n 7） |
| v1.2.48 | APEX env 三件套（TZDATA_ROOT） |
| v1.2.49 | icudt76l.dat+tzdata 树（30MB 数据） |
| v1.2.50 | core-icu4j.jar（BCP 六件套） |
| v1.2.51 | /data 目录树 + Documents 日志 |
| v1.2.52 | JIT Low4GB 引擎代管选址 |
| v1.2.53 | sigtimedwait=137 号张冠李戴 |
| v1.2.54 | ptrace dispatch 补 case |

## 次要遗留（不影响 Hello）

- main 退出后的 Runtime 收尾 abort：Signal Catcher 被 ExitTime 唤醒，
  sigwait 返 EINTR/EPERM → LOG(FATAL)。VMOS 同类实机也会在 exit 时刷
  dump——后续做"优雅关机"（转 SIGQUIT 语义或吞掉收尾 FATAL）。
- "Unable to set the name of current thread"（prctl PR_SET_NAME EPERM）×N
  ——纯日志噪音（宿主拒绝 guest 起线程名），可伪造成功消音。
