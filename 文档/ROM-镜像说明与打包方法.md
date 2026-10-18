# TwinVM ROM 镜像：用途与打包方法

> 适用版本：v0.7.69 ｜ 当前随包 ROM：`android/assets/demo.rom`
> （**67 个 inode / 61,164,300 字节 / md5 前缀 `fa88cc28`**）

---

## 一、ROM 是什么，为什么必须有它

TwinVM 是**免 root 的用户态 VM**：guest（被虚拟化的 Android 运行时，最终是 ART）跑在
**App 自己的进程里**，与宿主共享地址空间，靠 seccomp-BPF + SIGSYS handler 做 syscall 虚拟化。

因为**没有 root、也没有 mount namespace**，guest **看不到真机的 `/system` 和 `/apex`**。
但这些目录正是 ART 启动的命根子（`libart.so`、`libc.so`、`boot classpath` 的 jar、启动镜像…）。

**ROM 就是把这些目录做成一个自包含的只读磁盘镜像，随 APK 一起带走**，
再由引擎在 syscall 层"假装"它们存在：

| guest 的动作 | 引擎的处理 |
|---|---|
| `openat("/system/lib64/libc.so")` | 查 ROM inode 表 → 造**虚拟 fd**（编号 ≥ 10000）返回 |
| `read` / `pread64` / `fstat` / `mmap`(该 fd) | 按虚拟 fd 路由到 ROM 里的数据段（内存拷贝） |
| `openat("/proc/self/maps")` 等 | 由引擎**合成**（不在 ROM 里） |

真机日志里这类行就是它：

```
[TwinVM] openat("/system/lib64/libc.so") -> fd=10000 [ROM]
[TwinVM] openat("/apex/com.android.art/javalib/core-oj.jar") -> fd=10000 [ROM]
```

---

## 二、ROM 里装了什么（当前 demo.rom）

| 顶层 | 条数 | 内容与作用 |
|---|---|---|
| `/system` | 48 | `bin/dalvikvm`（ART 入口）、`bin/linker64`、`etc/ld.config.txt`、`lib64/*.so`（约 41 MB：libart / libc / libnativebridge / libnativeloader / libunwindstack …） |
| `/apex` | 14 | `com.android.art/javalib/*.jar`（6 个 boot jar = BOOTCLASSPATH）、`javalib/boot.{art,oat,vdex}`（启动镜像）、`etc/classpaths/*.pb`、`etc/boot-image.prof`、`dirty-image-objects` |
| `/bin` | 2 | **我们自己编的**：`guest`（VM 里跑的第一段原生代码）、`probe`（诊断跳板） |
| `/rom` | 2 | `hello.txt` / `hello.dex`（最小验证用） |
| `/README.txt` | 1 | 说明 |

> ### ⚠️ 铁律：库里所有 `.so` 必须来自**同一次系统快照**
> `libart.so`、`libnativeloader.so` 都是 **BIND_NOW**（加载期解析全部符号）。
> 混用不同版本会出现：
> ```
> dlopen failed: cannot locate symbol "NativeBridgeIsNativeBridgeFunctionPointer"
>   referenced by "/system/lib64/libart.so"
> ```
> 换机、系统升级、从别人机器拷库之后，**必须重采 + 重跑 `checkrom --strict`**。

---

## 三、镜像格式（`rom/rompack.py`）

```
┌ 64 字节头
│   magic        "TVMROM1\0"
│   inode_count  u32
│   inode_size   u32 = 292
│   data_off     u64（= 64 + inode_count × 292）
│   data_size    u64
│   file_crc     u32（全体数据 crc32）
│   reserved     24 字节
├ inode 表：每条 292 字节
│   name[256]  文件路径（含前导 '/'，第一个 NUL 截断）
│   mode u32   uid u32   gid u32
│   size u64   data_off u64   crc32 u32   xattr_len u32
└ 数据区：各文件顺序存放，**每块 16 字节对齐**
```

* 每个文件带 **crc32**，装载时逐条校验；
* `/bin/*` 会自动带上可执行位（`rompack.py` 按路径前缀判断）。

以当前包为例：`64 + 67×292 = 19,628` 字节之后开始数据区，数据区 `61,144,672` 字节。

引擎侧：`vm_rom_load()` 把整个文件 `mmap` 进来 → 解析 inode 表 → `rom->inodes[i].map`
直接指向数据区（零拷贝），之后所有"磁盘读"都是内存拷贝。

---

## 四、怎么打包 ROM

### 4.1 标准流程（源码包自带，需 NDK）

```bash
cd android && bash build.sh
```

其中第 2 步就是打 ROM：

```bash
mkdir -p $OUT/romroot/{bin,rom}
cp $OUT/guest64              $OUT/romroot/bin/guest     # 自编 guest
cp ../rom/fixture/dex/hello.dex $OUT/romroot/rom/
for d in ../rom/fixture/android_root/*; do              # ★ 库树整体打入
    b=$(basename "$d"); case "$b" in bin|rom) continue;; esac
    cp -r "$d" "$OUT/romroot/$b"
done
python3 ../rom/rompack.py pack "$OUT/romroot" assets/demo.rom
python3 ../rom/checkrom.py assets/demo.rom --strict     # 门禁：不过即中止
```

> **改 ROM 的正确姿势是改 `rom/fixture/android_root/`，不是直接改 `assets/demo.rom`**
> —— 后者会在下一次 `build.sh` 被覆盖。
> `rom/fixture/android_root/` 下的**每一个顶层目录**都会进 ROM
> （`system/` → guest 的 `/system`，`apex/` → guest 的 `/apex`；`bin/`、`rom/` 由脚本自己产出）。

### 4.2 库树从哪来（源码包里通常没有那 30 MB 系统库）

在能读到设备 `/system`、`/apex` 的环境（设备 root shell 或本容器的 proot）里：

```bash
# 从设备采集 → 回写 ROM
python3 rom/syncrom.py --rom android/assets/demo.rom --out demo-fixed.rom --device-root /

# 或者只生成 build.sh 要的 fixture 树（推荐，之后走 4.1 正常打包）
python3 rom/syncrom.py --rom android/assets/demo.rom \
        --as-root rom/fixture/android_root --device-root /

python3 rom/checkrom.py android/assets/demo.rom --strict   # 必须 PASS 再打包
```

常用参数：

| 参数 | 作用 |
|---|---|
| `--as-root <dir>` | 不打包，只把同步后的树写成 `rom/fixture/android_root` |
| `--out <rom>` | 直接输出一个新 ROM 文件 |
| `--drop a,b` | 从 ROM 里删掉指定路径（例如自造 shim 已被真库取代） |
| `--map system/lib64=./sysdump/apex-ART` | 额外路径映射（PC 上先 adb 拉库再打包） |
| `--dry-run` | 只演示不写盘 |

### 4.3 无 NDK 时的替代路线（本仓库 `tools/build_android_here.sh`）

用 Ubuntu clang + NDK sysroot 交叉编译，然后：

* **有** `rom/fixture/android_root/system` → 用 `rom/rompack.py pack` 重打 ROM；
* **没有**（源码包通常不带库树）→ **沿用已有的 `android/assets/demo.rom`**，只替换
  `libtwinvm.so`/Java 层。这样能把 ROM 从"每次构建的变量"变成"固定基线"，
  避免把 58 MB 库树打坏（本次修复全程 ROM `md5` 未变）。

---

## 五、门禁：`rom/checkrom.py` 查什么

```
python3 rom/checkrom.py android/assets/demo.rom            # 检查
python3 rom/checkrom.py android/assets/demo.rom --strict    # 有缺失即返回码 1（CI 用）
```

1. 每个 `.so` 的 `DT_NEEDED` 是否都在 ROM 内；
2. 每个 **未定义符号（UND）** 是否都能被 ROM 里某个 `.so` 提供（WEAK 允许悬空）；
3. **boot classpath 完整性**：ROM 里有 `dalvikvm` ⇒ 必须带
   `/apex/com.android.art/javalib/*.jar`，且 classpath 里点到的 jar 都要存在。

> 历史教训：v0.7.37 之前 `build.sh` 只 `cp system/`，把 `apex/` 静默丢掉 →
> ART 报 `file_utils.cc:609 Check failed: … BOOTCLASSPATH … must not be empty`。
> 这就是 checkrom 第 3 条要守的门。

---

## 六、查看 / 解包现成 ROM

```bash
python3 rom/rompack.py unpack android/assets/demo.rom /tmp/romdump   # 逐条校验 crc
python3 rom/rompack.py pack   <目录> <输出.rom>
```

当前 `demo.rom` 解出来是：

```
system/  41M      ← ART 与系统库
apex/    15M      ← boot jar、启动镜像、classpath 描述
bin/     3.4M     ← guest / probe
rom/     12K      ← hello.txt / hello.dex
README.txt
```

（`/bin/probe_policy` 会随下次重打 ROM 进包 —— 它是"独立进程 seccomp 探针"，
见 `修复报告-v0.7.69.md`。）

---

## 七、常见坑速查

| 症状 | 原因 | 处置 |
|---|---|---|
| `cannot locate symbol "X" referenced by "Y.so"` | 库不是同一次快照 | 重新 `syncrom.py` 采集 + `checkrom --strict` |
| `BOOTCLASSPATH … must not be empty` | ROM 里缺 `/apex/.../javalib/*.jar` | 把 `apex/` 一起打进 ROM（4.1 的循环） |
| 改了 `assets/demo.rom` 却没生效 | 下一次 `build.sh` 用 fixture 重打了 | 改 `rom/fixture/android_root/` |
| `guest` 装不进 ROM | guest 不是静态 `ET_EXEC` | `build.sh` 已检查 `Type: EXEC`，别用 PIE |
| ROM 太大塞不进 APK | 库树全量打进（58 MB 正常） | 正常现象；必要时 `--drop` 掉不用的库 |
