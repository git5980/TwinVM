tvm4e 库文件打包说明
========================================

打包来源
----------------------------------------
本包内所有 .so 文件均直接从设备 /system/lib64/ 原样复制，
未做任何修改、剥离或重命名。文件时间戳保持原始值
（Aug 11 18:02，即 ROM 镜像内的原始 mtime）。

文件清单（共 2 个）
----------------------------------------
  894280  tvm4e/system/lib64/libbinder.so   sha256 297aad352eee9b522677e0568ca1ab69d67e8baba4c34a9c329fc7064cfae5a6
  135656  tvm4e/system/lib64/libutils.so   sha256 809917829b4d4921b2a1a9d6c9323e0c0c39f0a6aa5267aa313c111c6bd1da74

SHA256 校验
----------------------------------------
解包后进入 tvm4e/system/lib64/ 执行：
  sha256sum libbinder.so libutils.so
应与上方数值完全一致，否则说明文件损坏或被替换。

特殊说明（重要）
----------------------------------------
1. 两个文件均为普通文件，不是符号链接。
   ls -l 输出为 "-rw-r--r--"，首字符是 "-" 而非 "l"，
   因此解包后可直接使用，无需恢复软链关系。

2. 仅包含 64 位 arm64 版本，不含 32 位 system/lib 版本。

3. 用途：
   - libbinder.so   ：Binder 的 framework/Native Activity 侧实现
                     （BBinder/BParcel/BpBinder 等），与
                     libbinder_ndk.so 是两套不同接口。
                     本包即为 tvm4d 中「未包含 libbinder.so」的补充。
   - libutils.so    ：Android 基础工具库，提供 String8/String16、
                     RefBase/ sp<>、Threads、Locker、SharedBuffer 等，
                     是 libbinder.so 的强依赖，二者必须配套。

4. 运行时依赖（本包未包含，需另行准备）：
   liblog.so、libcutils.so、libbase.so、libc.so、libdl.so、
   libm.so、libz.so、libvndksupport.so、libunwind.so。
   其中 libc/libdl/libm/liblog/libcutils/libz/libunwind 可从
   tvm4b.zip 取得；libbase.so 若单独需要请另行提取。

5. 部署方式：将文件放回目标机同名路径 /system/lib64/，
   或用 LD_LIBRARY_PATH / chroot sysroot 覆盖。

6. 版本一致性风险：Binder 相关库与内核 binder 驱动、
   libc++/libbase 强耦合，OTA 或不同机型之间不可混用。
   请尽量整机成套取用同一 ROM 的库。

7. 本包在 Termux 非 root shell 环境下完成。
   不要在 su root 环境下重新打包——部分 ROM 的 root namespace
   对 /system/lib64 的可见性不同，可能漏文件或取错版本。

生成时间：2026-10-13 12:38
生成环境：Termux (u0_a443)
