tvm4d — Android system/lib64 binder/procinfo 依赖库
打包时间：2026-10-13 11:56
打包环境：Termux (ZeroTermux) 非 root shell
打包方式：在设备上直接读取 /system/lib64（app namespace 权限），未使用 adb
打包命令：
  for f in libprocinfo.so libbinder_ndk.so; do cp -p /system/lib64/$f tvm4d/system/lib64/; done
  zip -q /sdcard/tvm4d.zip tvm4d/system/lib64/*.so tvm4d-README.txt

内容清单（2 个二进制文件）
  system/lib64/libprocinfo.so     15560 bytes
  system/lib64/libbinder_ndk.so  163256 bytes

sha256（设备源文件实测，完整值）
  libprocinfo.so    180d8f5734f7abab8c64677af349e55aa63a82a2af21ea47b363a0564cfd6c74
  libbinder_ndk.so  0ef7b4a9844df4084f4b4e87a661224b4ca3de7d527a109a893c328aae03f100

解包后可用以下命令复核：
  cd system/lib64 && sha256sum libprocinfo.so libbinder_ndk.so

特殊说明
1. 两个文件均为普通文件，不是符号链接（ls -l 显示 -rw-r--r--，非 l 开头），
   解包后可直接按普通 .so 使用，无需恢复软链关系。
2. 这是 64 位（arm64）版本，位于 system/lib64，未包含 32 位 system/lib 版本。
3. libbinder_ndk.so 是 Binder NDK 后端运行时库，libprocinfo.so 提供进程信息
   接口；二者通常与 libc/libdl/liblog/libbase 等已有依赖一起使用，本包不含
   这些基础库（如需要请取 tvm4b.zip）。
4. 文件取自本机 system/lib64 只读挂载点。解包后若要给其他可执行文件使用，
   需放到目标机同名路径，或通过 LD_LIBRARY_PATH / sysroot 指定搜索路径。
5. 版本一致性风险：取自本机系统版本，目标机经过 OTA 或不同机型/Android
   版本可能导致 ABI 不匹配，建议核对版本后再使用。
6. 本包未包含 libbinder.so（Native Activity / framework 侧），若需要请单独提取。
7. 不要在 root（su）环境下重新打包：部分 Android ROM 的 root mount namespace
   下 /system/lib64 可见性不同，可能漏文件或取到错误版本。本包是在 Termux
   非 root shell 下完成的。

解包方式
  unzip tvm4d.zip
  目录结构将还原为 system/lib64/*.so，请保留目录结构以免路径丢失。
