==========================================================================
 tvm4f  libapexsupport.so  打包说明 / README
==========================================================================

打包时间 : 2026-10-13
打包方式 : Termux 普通 shell（非 root，无 su）
来源设备 : Android（AArch64，64 位内核）
源分区   : /system  （只读挂载，直接读取）

--------------------------------------------------------------------------
1. 文件清单
--------------------------------------------------------------------------
tvm4f/system/lib64/libapexsupport.so    169736 字节
tvm4f/system/lib/libapexsupport.so      111976 字节
tvm4f-README.txt                       （本文件）

sha256:
  946a3f0d3755897ea6281c32c92990c6aaacfee4bc50cbed07d1631eee2cc43a  lib64/libapexsupport.so
  23c80cd872f5bf9a18f9f58a8238411f2141d2d5b4616aad9cf34a8c0c5911b6  lib/libapexsupport.so

ELF 头（od -N20 实测）:
  lib64: 7f 45 4c 46 02 01 01 00 ...  03 00 b7 00   -> ELF64 / AArch64 / DYN
  lib  : 7f 45 4c 46 01 01 01 00 ...  03 00 28 00   -> ELF32 / AArch64 / DYN

readelf -h 确认（lib64）:
  Class   : ELF64
  Type    : DYN (Shared object file)
  Machine : AArch64

--------------------------------------------------------------------------
2. 位置更正（重要）
--------------------------------------------------------------------------
最初请求的路径 /apex/com.android.art/lib64/libapexsupport.so
在本机不存在。已用 find 全盘搜索（/apex 全部挂载点、/system、
/vendor、/product、/system_ext、/odm、/data/app、/data/local/tmp、
/sdcard、Termux files），全机仅两处命中：

  /system/lib/libapexsupport.so      (32 位)
  /system/lib64/libapexsupport.so    (64 位)

该库属于 system 分区，不属于 com.android.art APEX。请注意修正引用路径。

--------------------------------------------------------------------------
3. 文件性质
--------------------------------------------------------------------------
两个文件均为普通文件，不是软链接（ls -ld 显示 -rw-r--r--），
解包后可直接使用。属主 root:root，权限 0644。

--------------------------------------------------------------------------
4. 这是 Rust 静态库，不是纯 C/C++ 库
--------------------------------------------------------------------------
符号表显示内部大量 _RNv... 修饰名（Rust v0 mangling），并静态链入了
Rust 标准库。dynsym 中还可见：

  rust_panic, rust_begin_unwind, rust_eh_personality,
  __rust_alloc, __rust_dealloc, __rust_realloc,
  __rdl_alloc, __rdl_dealloc, __rdl_realloc,
  __rust_alloc_error_handler, __rg_oom,
  __rust_no_alloc_shim_is_unstable

含义：本库自带动分配器与 panic 运行时，不依赖 libcdc++ 的
operator new。链接时无需额外提供 Rust std 动态库。

--------------------------------------------------------------------------
5. 对外公开的 C ABI 符号（lib64 实测）
--------------------------------------------------------------------------
  AApexInfo_create()        580 字节  FUNC GLOBAL
  AApexInfo_destroy()        64 字节  FUNC GLOBAL
  AApexInfo_getName()        28 字节  FUNC GLOBAL
  AApexInfo_getVersion()     28 字节  FUNC GLOBAL
  AApexSupport_loadLibrary() 1124 字节 FUNC GLOBAL

共 5 个 C 接口。这就是全部可用面；其余导出符号均为 Rust 内部符号，
不应直接调用。

内部实现细节（strings 实测）：
  - 依赖 android_dlopen_ext / android_get_exported_namespace
    （来自 libdl_android.so）把 APEX 库加载进正确的 linker namespace
  - 依赖 __android_log_print（liblog.so）打日志
  - 会读 /apex 下的 apex_manifest.pb，并用 protobuf 解析
    ApexManifest / CompressedApexMetadata
  - 错误枚举含 PathNotFromApex / ExePathUnavailable / InvalidApex /
    PermissionDenied / AddrNotAvailable 等

--------------------------------------------------------------------------
6. DT_NEEDED 依赖（未包含在本包内）
--------------------------------------------------------------------------
lib64 版动态段实测依赖 6 个库：

  libdl_android.so     <- Android linker 扩展接口，关键
  liblog.so            <- __android_log_print
  libc++.so            <- C++ 运行时
  libc.so
  libm.so
  libdl.so

均未打包。请从本机 /system/lib64/ 与 /system/lib/ 同名取得
（属普通文件，可直接复制）。BIND_NOW / FLAGS_1=NOW 已置位，
即启动时全量立即绑定，缺少任一依赖会在 dlopen 阶段直接失败，
不会延迟到调用时才报错。

--------------------------------------------------------------------------
7. 部署方式
--------------------------------------------------------------------------
A. 同名路径部署（最稳）
   adb push libapexsupport.so /system/lib64/
   adb push libapexsupport.so /system/lib/
   （需 remount，且 /system 需可写）

B. 目录加载
   LD_LIBRARY_PATH=/path/to/dir <你的程序>
   同时把第 6 节列的 6 个依赖放进同一目录，否则 BIND_NOW 解析失败。

C. chroot / sysroot 场景
   放入 <root>/system/lib64/ 与 <root>/system/lib/，
   并保证 libdl_android.so、liblog.so、libc++.so 在同搜索路径内。

D. 自编译程序链接
   -lapexsupport 即可，但运行时必须满足第 6 节依赖。

--------------------------------------------------------------------------
8. 版本一致性风险
--------------------------------------------------------------------------
- AApexInfo_* / AApexSupport_* 是较新 AOSP 的 NDK 侧 apexsupport 接口，
  与 apexd、libapexd、linker namespace 规则强耦合。
- 不可跨 ROM / 跨机型 / 跨 Android 版本混用。若目标机 API level
  或 APEX 布局不同，请重新从目标机提取，不要沿用本包。
- 静态内联的 Rust std 会带来符号膨胀，若你的程序也链 Rust，
  注意符号重复与 ODR 冲突。

--------------------------------------------------------------------------
9. 打包环境说明
--------------------------------------------------------------------------
- 全程在 Termux 非 root shell 下完成，未使用 su。
- 复制用 cp -a 保留时间戳；sha256 在复制后重新计算，与源一致。
- 校验 ELF 用 readelf -h / -d / --dyn-syms 与 od，未使用 file
  （Termux 未安装 file）。

==========================================================================
