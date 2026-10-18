rom/fixture/android_root/  —— guest 看到的 /system 树（ROM 的库来源）
=====================================================================
android/build.sh 第 2 步会：
    cp -r rom/fixture/android_root/system  <build>/romroot/system
    python3 rom/rompack.py pack <build>/romroot  android/assets/demo.rom
也就是说：**改这里的库 = 改 ROM**；直接改 assets/demo.rom 会在下一次 build.sh 被覆盖。

本目录内容（v0.7.29，38 inode ROM 的 /system 部分，30MB）：
  system/bin/dalvikvm          ← 设备 /apex/com.android.art/bin/dalvikvm
  system/bin/linker64          ← 设备 /system/bin/linker64
  system/etc/ld.config.txt     ← 项目合成（namespace：system 链接 art，搜索路径 /system/lib64）
  system/lib64/*.so            ← ART 相关优先取 /apex/com.android.art/lib64，其余取 /system/lib64

铁律：这些库必须来自**同一次系统快照**。libart.so / libnativeloader.so 都是 BIND_NOW，
强未定义符号缺一个就是加载期 dlopen 失败（真机症状：cannot locate symbol "X" referenced by "libY.so"）。
实例（2026-10-13）：自造 libnativebridge.so shim 没导出 OpenSystemLibrary →
  dlopen libart.so 失败（libnativeloader 需要它）。

换机/系统升级后重新生成本目录（容器/root shell 里能直接读 /apex 时）：
    python3 rom/syncrom.py --rom android/assets/demo.rom --as-root rom/fixture/android_root --device-root /
    python3 rom/checkrom.py android/assets/demo.rom --strict        # 必须 PASS 再打包
（PC 上先用 adb 把库拉到本地目录，再 --device-root <该目录> / --map 指定。）

--- v0.7.37 补充 ---------------------------------------------------------------
本目录顶层**所有**目录都会被打进 ROM（android/build.sh 第 2 步的 for 循环）：
  system/  → guest 的 /system（dalvikvm、linker64、etc/ld.config.txt、lib64/*.so）
  apex/    → guest 的 /apex（ART 的 boot classpath 与启动镜像，**以前这里被漏掉**）
    apex/com.android.art/javalib/*.jar          6 个 boot jar（= 设备 APEX，md5 已核对）
    apex/com.android.art/javalib/boot.{art,oat,vdex}   ART 默认推算的镜像位置
    apex/com.android.art/etc/classpaths/*.pb     设备用它描述 boot classpath
    apex/com.android.art/etc/{boot-image.prof,dirty-image-objects}
漏掉 apex/ 的真机症状：ART 起来后
  file_utils.cc:609 Check failed: ... BOOTCLASSPATH ... must not be empty（然后 code=127）
自检：python3 rom/checkrom.py android/assets/demo.rom --strict   # 会检查 jar/pb 完整性
重建：python3 rom/syncrom.py --rom android/assets/demo.rom --as-root rom/fixture/android_root --device-root /
