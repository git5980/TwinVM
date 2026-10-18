twinvm4.zip — Android ART / Bionic 运行时文件收集包
====================================================

一、这个包里有什么
------------------

按原始指令清单收集的 10 个文件（一个不少），目录结构如下：

    bin/app_process64
    bin/dalvikvm
    lib64/libart.so
    lib64/libnativehelper.so
    lib64/libc.so
    lib64/libm.so
    lib64/libdl.so
    lib64/liblog.so
    lib64/libbase.so
    lib64/libcutils.so


二、"搬过家"的文件说明（重点）
------------------------------

原指令假设这些文件都直接位于 /system 下。实际上现代 Android 已把 ART
运行时拆分到 APEX 模块中，下面 3 个文件在 /system/lib64、/system/bin
里已经找不到了，需要从 /apex/com.android.art/ 下取。包里已把它们
还原成指令要求的路径名，使用方式与原文件一致。

1) lib64/libart.so
   实际位置：/apex/com.android.art/lib64/libart.so
   大小：约 21.3 MB
   说明：这是包里最大的文件。ART（Android Runtime）虚拟机的主库，
        负责解释执行 / JIT / AOT 编译 dex/oat 代码。Android 7.0 引入
        APEX 后，它从 /system/lib64 迁入了 com.android.art 模块，
        系统启动时会通过 APEX 的命名空间机制挂载给应用进程使用。
        本包中仍以 lib64/libart.so 命名存放。

2) lib64/libnativehelper.so
   实际位置：/apex/com.android.art/lib64/libnativehelper.so
   大小：约 31 KB
   说明：JNI 辅助库，提供 RegisterNatives、创建 JavaVM 等原生接口。
        同样随 ART 一起迁入 APEX。它体积很小但依赖链关键，缺失会
        导致 JNI_OnLoad 相关调用失败。
        本包中仍以 lib64/libnativehelper.so 命名存放。

3) bin/dalvikvm
   实际位置：/apex/com.android.art/bin/dalvikvm64（符号链接 dalvikvm
   也指向它）
   大小：约 18 KB
   说明：独立启动一个 Dalvik/ART 虚拟机的命令行可执行文件，类似
        Java 的 java 命令，可用来在命令行运行 dex。注意源文件名是
        dalvikvm64，本包已重命名为指令要求的 dalvikvm。
        另：/system/bin/app_process 也是指向 app_process64 的符号
        链接，因原指令只要 app_process64，故未做处理。


三、直接来自 /system 的文件
---------------------------

以下 7 个文件在 /system 中原位取得，未做任何改动：

    bin/app_process64       约 23 KB   （Zygote 进程主入口）
    lib64/libc.so           约 1.3 MB  （Bionic C 库）
    lib64/libm.so           约 233 KB  （数学库）
    lib64/libdl.so          约 14 KB   （动态链接器辅助）
    lib64/liblog.so         约 87 KB   （logcat / __android_log_print）
    lib64/libbase.so        约 194 KB  （libbase 基础工具）
    lib64/libcutils.so      约 310 KB  （旧版 C 工具函数，多为
                                        libbase 所取代）


四、打包过程补充
----------------

- 最初在 /system 下检查时，上面 3 个文件确实显示为 MISSING，原因是
  它们已迁入 APEX 模块，并非权限或路径错误。
- 打包时使用 tar 的 --transform 把 dalvikvm64、libart.so、
  libnativehelper.so 的路径重写为指令要求的 bin/dalvikvm、
  lib64/libart.so、lib64/libnativehelper.so。
- 由于要放入一个 README 文本，且用户要求最终交付 zip 格式，
  本包由 twinvm4.tgz 解包后重新用 zip 压缩而成；原 .tgz 已删除。
- 全部文件均可正常读取，不存在缺失项。


五、注意事项
------------

- APEX 模块内容随系统 OTA 可能变化，若在其他设备上使用需重新收集。
- 这些文件属于系统私有实现，无稳定 ABI，不要在自己应用里 dlopen。
- 若需要在模拟器 / 双开环境中还原运行时，直接把 bin/ 和 lib64/
  按相同相对路径覆盖即可。
