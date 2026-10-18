#!/bin/bash
# 在「aarch64 容器 + 无 x86_64 NDK 工具」的环境里交叉编译 Android arm64 产物。
# 原理：Ubuntu clang 18 负责前端/驱动，NDK r26d 的 sysroot（bionic 头/库，架构相关但与宿主无关）
#       提供 Android API-24 的头与库，lld 负责链接。等价于 build.sh 里 clang --target=aarch64-linux-android24。
set -e
NDK=${NDK:-/root/android-ndk-r26d/toolchains/llvm/prebuilt/linux-x86_64}
# v1.1.3：资源目录随 NDK 自带 clang 版本走（r26d=17、r29=21），允许外部覆盖
RD=${RD:-$(ls -d "$NDK/lib/clang/"*/ 2>/dev/null | tail -1 | sed 's:/$::')}
[ -n "$RD" ] || RD=$NDK/lib/clang/17
S=$NDK/sysroot
CC="clang --target=aarch64-linux-android24 --sysroot=$S -resource-dir=$RD --rtlib=compiler-rt -O2 -Wall -Wshadow -fno-stack-protector"
cd "$(dirname "$0")/.."

echo "== libtwinvm.so =="
$CC -Icore/src -shared -fPIC -Icore/include -o android/libtwinvm.so \
    core/src/vm_core.c core/src/vm_binder.c core/src/vm_rom.c core/src/vm_loader.c \
    core/src/vm_backend_sigsys.c core/src/vm_notif.c android/jni/twinvm_jni.c -llog

echo "== /bin/guest（静态 EXEC） =="
$CC -static -fno-pic -no-pie -o build_guest64 core/guest/guest_stage42.c

echo "== /bin/probe（动态 PIE，诊断用） =="
$CC -pie -fPIE -o build_probe core/guest/guest_probe.c

echo "== 独立进程策略探针（无 libc，几 KB；同时嵌进 .so + 放进 ROM） =="
$CC -nostdlib -static -fno-pic -o build_probe_policy core/guest/guest_probe_policy.c
python3 tools/gen_probe_blob.py build_probe_policy android/jni/probe_policy_blob.h

echo "== ROM（fixture 有库树则重打；否则沿用已有 demo.rom） =="
if [ -d rom/fixture/android_root/system ]; then
    OUT=build_romroot; rm -rf $OUT; mkdir -p $OUT/bin $OUT/rom
    cp build_guest64 $OUT/bin/guest; cp build_probe $OUT/bin/probe
    cp build_probe_policy $OUT/bin/probe_policy
    cp rom/fixture/rom/hello.txt $OUT/rom/ 2>/dev/null || true
    cp rom/fixture/dex/hello.dex $OUT/rom/ 2>/dev/null || true
    for d in rom/fixture/android_root/*; do
        b=$(basename "$d"); case "$b" in bin|rom) continue ;; esac
        cp -r "$d" "$OUT/$b"
    done
    python3 rom/rompack.py pack $OUT android/assets/demo.rom
else
    echo "!! 缺 rom/fixture/android_root/system —— 沿用已有 android/assets/demo.rom（不重打）"
fi
python3 rom/checkrom.py android/assets/demo.rom --strict
