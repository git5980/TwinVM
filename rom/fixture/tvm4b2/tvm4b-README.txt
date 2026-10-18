# tvm4b.zip 说明与注意事项

本压缩包从本机（Android 设备）提取了 18 个二进制文件，zip 内路径按设备上的真实路径存放（`system/...`、`apex/...`），解包时需保留目录结构。

## 文件清单（18 个二进制文件）

- `system/bin/linker64`
- `system/lib64/libc++.so`
- `system/lib64/libz.so`
- `system/lib64/liblz4.so`
- `system/lib64/liblzma.so`
- `system/lib64/libunwindstack.so`
- `system/lib64/heapprofd_client_api.so`
- `system/lib64/ld-android.so`
- `system/lib64/libnativebridge.so`
- `system/lib64/libnativeloader.so`
- `system/lib64/libdl_android.so`
- `apex/com.android.art/lib64/libartbase.so`
- `apex/com.android.art/lib64/libartpalette.so`
- `apex/com.android.art/lib64/libdexfile.so`
- `apex/com.android.art/lib64/libprofile.so`
- `apex/com.android.art/lib64/libsigchain.so`
- `apex/com.android.os.statsd/lib64/libstatspull.so`
- `apex/com.android.os.statsd/lib64/libstatssocket.so`

sha256 前 16 位（提取时实测）：

- `libstatspull.so` → `5734dc2228f804ac`
- `libstatssocket.so` → `26acec2bdf278ea8`

## 注意事项

1. **`libdl_android.so` 原本是符号链接**
   设备上 `system/lib64/libdl_android.so` 是指向 `/apex/com.android.runtime/lib64/bionic/libdl_android.so` 的软链，zip 中存放的是**文件本体内容**而非软链。若使用方按路径解析符号链接关系（linker 查找、依赖路径推导等），解包后需自行恢复为对应软链，否则可能出现加载异常。

2. **不要用 root（KernelSU/`su`）重新打包这些文件**
   本机实测：root 语境下 `/apex/com.android.os.statsd/lib64/` 目录可见但内容不可读（同目录 `bin/statsd` 显示 `-??????????`），`su -c tar` 会直接报 `No such file or directory`，并误报 `system/lib64/libnativebridge.so`、`system/lib64/libnativeloader.so`、`apex/com.android.art/lib64/libdl_android.so` 不存在。这是 mount namespace 差异，不是文件真的缺失。Termux app 的 namespace 下这些文件均可正常读取，因此本包是在 Termux 非 root shell 下完成打包的。

3. **全部为 64 位（lib64 / arm64）**
   未包含 `lib/` 下的 32 位同名文件。若目标环境是 32 位进程，需要另行提取。

4. **`apex/` 下的文件在设备上位于只读 APEX 挂载点**
   解包后需放到目标机的同名路径或相应的 sysroot / LD_LIBRARY_PATH 中，才能被 linker 正常找到。

5. **版本一致性风险**
   文件取自本机当前系统版本；目标机若经过 OTA 升级（或为不同机型/Android 版本），APEX 与 system 分区版本不一致可能导致 ABI 或符号不匹配，建议按目标设备重新提取。

6. **zip 内保留了目录项**（`apex/`、`apex/com.android.art/`、`apex/com.android.art/lib64/` 等），解包请使用支持目录结构的 `unzip`，避免只抽取文件导致路径丢失。
