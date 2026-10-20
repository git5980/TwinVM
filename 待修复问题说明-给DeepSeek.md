# TwinVM v1.2.88 待修复问题说明（移交文档）

> 写给接手的 AI（DeepSeek）：这是一台**用户态 ARM64 虚拟机**（无 root、纯
> ptrace 实现），现在卡在最后一关：**guest 的 Android ART（dalvikvm）启动链
> 在 linker 初始化阶段无限循环**。九个版本的侦查已把凶手逼到了墙角，这文档
> 是全部案卷。

---

## 一、这台机器是什么（30 秒版）

- **宿主**：一个普通 Android App（OnePlus 真机，Android 16）
- **guest**：App fork 出的子进程，被我们的 tracer 线程用 ptrace 逐个
  syscall 拦截/虚拟化（`tvm_virt_nr` 决定拦不拦，引擎 `vm_handle_syscall`
  代执行并回填 x0）
- **文件系统**：ROM 包（131 个文件：dalvikvm、linker64、libc、libart 全套）
  经 vfs 虚拟化，guest 看不到宿主文件
- **启动方式**：`exec_dyn` 在 fork 后把 dalvikvm+linker64 直接装载进
  guest 内存（不用内核 execve），跳转 linker64 的 entry，让 ART 自己跑起来

**运行入口**：点 App 的"运行 Stage 4.2 自检"→ 走到 [6/6] 已进入 ART →
然后 guest 在同一点无限打转，90 秒后被我们的优雅关机（graceful）SIGTERM 收尸。

---

## 二、要修什么（大白话）

**guest 的 linker64 每秒钟"把自己重新初始化"1~2 遍，永远走不到
"dlopen libart → 启动 Java 虚拟机"的下一步。**

就像一台电脑反复开机：BIOS 自检（linker 初始化）跑完 → 该交给操作系统
（dalvikvm 的 main）了 → 不知道为什么又按了重启键 → 再跑一遍自检……
循环 90 秒直到我们把它毙了。

**修好后的效果**：guest 走完 linker 初始化 → JniInvocationInit dlopen
libart.so → JNI_CreateJavaVM → 最终在控制台打出
`stress-all-pass in XXms` → 8 秒静默后 graceful 收尾 → App UI 翻到结果页。
（这条链的前半段已经全部打通，只卡在 linker init 这一步。）

---

## 三、凶手画像（九版侦查的证据链）

### 3.1 循环形态（每版日志一致）

```
syscall 停点分布：mprotect(nr=226) 占 99%（254/256）
固定的 4~5 个页面被反复改权限 R↔RW，每秒约 25 对：
  0x78d3bf5000、0x78d3ab7000、0x78d3ab8000  = libart.so 的 ELF 头/RELRO 页
  0x78d17ee000、0x78d17ef000                = libnativeloader 的 RW 段
无 futex WAIT（没睡，在干活）、无 stderr 输出、无崩溃
```

### 3.2 mprotect 的直接调用者：linker64 的 __cxa_atexit

反汇编定证（ROM 里的 /system/bin/linker64，vaddr 已核对）：

```
0x9ffac: mprotect(页, RW)    ← 登记 atexit 前临时解锁 RELRO 页
0x9fff0: mprotect(页, R)     ← 写完恢复
```

即：**上层有人每秒 25 次地调 __cxa_atexit（注册析构函数）**，
每次登记都对已上锁的登记页"开锁→写→上锁"。

### 3.3 调用者帧链（v1.2.88 atexit-probe 铁证，12 条全命中循环）

```
mprotect(0x78d3ab8000, RW) 的 x29 帧链（5 层，去 PAC 后）：

第1层  _GLOBAL__sub_I_linker.cpp+0x40 / +0x64 / +0xbc
       _GLOBAL__sub_I_linker_globals.cpp+0x50
       _GLOBAL__sub_I_linker_soinfo.cpp+0x30
       _GLOBAL__sub_I_linker_main.cpp+0x30        ← linker64 自己的静态构造们
第2层  soinfo::call_constructors+0x22c（0xfcd78，blr x23 的返回点）
第3层  __linker_init_post_relocation+0x100（0x16fa34，bl call_constructors）
第4层  __linker_init_post_relocation+0x7c4 附近
第5层  0x77c130900c = linker64 段起点+0xc = ELF entry 附近！
```

**解读**：linker 只该初始化自己一次（_start → __linker_init →
call_constructors(self) → 十几个静态构造 → 跳 dalvikvm 的 main）。
但帧链最底层稳定落在 **entry 附近**、linker 的构造函数们被无限次重跑
（每秒 25 个 cxa_atexit 分布在这些构造里）——
**guest 的执行流每隔约一秒就从 linker64 的 entry 完整重跑一遍 init**。

### 3.4 侦查排除法（这些都不是凶手，别再查了）

| 假说 | 版本 | 结论 |
|------|------|------|
| uffd（userfaultfd）heap 模式 | v1.2.84/85 | 拔掉（282 恒拒 EPERM）后循环原样 → 无关 |
| mmap fd 32位-1 截断 → EINVAL | v1.2.82/83 | 修了（x4 规范化），EINVAL 只发生 1 次，循环原样 |
| close 回 EBADF 重试 | v1.2.77 | 修了（宽容语义），循环原样（在更早阶段是另一个 bug，已好） |
| 系统冻结/信号问题 | v1.2.69~76 | 全部修完（graceful 三级判据工作正常），与冻结无关 |

---

## 四、下一步查哪里（给 DeepSeek 的调查方向）

**核心谜题：谁让 PC 回到了 linker entry？**

按优先级：

1. **exec_dyn 的跳转/返回机制**（`android/jni/twinvm_jni.c` 搜 `exec_dyn`）
   - 我们装载完后怎么跳进 entry 的？如果用的是"构造初始栈 + ret"的方式，
     栈上的【返回地址】是谁？如果 guest 在某个 ret 落回了我们装载代码的
     循环里（比如装载函数的重试/多轮跳转逻辑），就会造成"每轮重新跳
     entry"的假象。
   - 重点看：跳转后 guest 走完 __linker_init 时，它会 ret 到哪里？
     （linker 的 _start 在 init 后会把控制权交给 main/或 __libc_init，
     这一步的地址是怎么算出来的？算错就可能跳回 entry。）

2. **ptrace 回填是否污染寄存器**（搜 `tvm_virt_take` / `SETREGSET`）
   - 每个 syscall exit 我们会把引擎结果写回 x0。如果【entry/exit 待办配对】
     在多线程/嵌套场景错位，可能把别的值写进 x0/PC。
     （v1.1.2 有过前科：close 的 -EBADF 覆盖所有 syscall 的 x0。）

3. **linker init 完成条件的内存是否被我们破坏**
   - linker 的 init_array 遍历（0xfcd40-0xfcd8c）用的 x21=数组指针、
     w25=数量、x22=计数器。如果这些字段所在的内存在 fill/灌入时被踩
     （map-fill 用 process_vm_writev 往 guest 灌段内容——检查是否越界），
     也可能造成诡异的循环。
   - 但注意：帧链显示【构造函数们确实在依次执行】（不同 ra），不是
     单个构造卡死——所以更像"外层每轮重来"。

4. **一个快速实验**（如果能跑模拟）：
   在 tracer 里对"PC 落在 entry 附近（0x77c1309000±0x1000）的停点"打日志，
   确认重启的精确频率和每次重启前的最后一个 syscall——重启前的那次
   syscall 的 exit 返回值（x0）就是真凶的作案工具。

---

## 五、关键文件/位置索引

| 文件 | 内容 |
|------|------|
| `android/jni/twinvm_jni.c` | 主战场：tracer 循环、拦截表(tvm_virt_nr ~790行)、exec_dyn 跳转、探针、graceful 判据 |
| `core/src/vm_core.c` | 引擎：syscall 代执行（vm_handle_syscall 664 行起）、vfs、close 宽容语义、userfaultfd EPERM(282) |
| `core/src/vm_notif.c` | seccomp/ptrace 后端 |
| `android/src/dev/twinbox/vm/` | Java 侧：MainActivity（运行按钮）、RunService（前台服务）、Native |
| `android/AndroidManifest.xml` | 当前 238 / 1.2.88 |
| `android/assets/demo.rom` | ROM 131 文件（8d57f5a0）——含 linker64/dalvikvm/libart 全套，可抽出来反汇编对照 |
| `文档/修复报告-v1.2.*.md` | 全部版本的历史案卷 |
| `开发路线.md` | 项目全史+教训库 |

## 六、历史教训（血泪，请遵守）

1. syscall 号必须查 arm64 unistd.h（x86 的号不能搬——129 在 arm64 是 kill，
   userfaultfd 是 282，撞号差点出 P0）
2. python replace 静默失败不报错——所有替换必须 assert + rg 复核 + 反汇编抽查
   （v1.2.84 就是这么翻车的）
3. 停点"忙"的判据要看速率不能看间隔；信号机关（setitimer/SIGALRM）在真机
   App 环境行为不可假设
4. guest 的 AArch64 调用惯例：w4 写 -1 零扩展 → fd=0xffffffff（不是 64 位 -1）
5. ptrace 模式下内存原语（mmap/mprotect/brk）必须 guest 原生执行，不能引擎代劳

---

## 七、当前版本状态

- v1.2.88（238）：源码可构建（`cd android && bash build.sh`，无 Gradle）
- make test 5/5 全过（core/）
- graceful 优雅关机链在两种模式（探针/ART 全量）都验证通过
- 卡点 100% 复现：每次"运行"都在同一点循环 90 秒后收尸
