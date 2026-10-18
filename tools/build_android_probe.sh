#!/bin/bash
# 用「容器 aarch64 gcc + NDK sysroot（bionic 头/库）」交叉编译 Android arm64 产物
set -e
NDK=/root/android-ndk-r26d/toolchains/llvm/prebuilt/linux-x86_64
S=$NDK/sysroot
API=24
CC="aarch64-linux-gnu-gcc"
GCCINC=$(aarch64-linux-gnu-gcc -print-file-name=include)
INCS="-nostdinc -isystem $S/usr/include -isystem $S/usr/include/aarch64-linux-android -isystem $GCCINC"
LIBS="$S/usr/lib/aarch64-linux-android/$API"
RT="$NDK/lib/clang/17/lib/linux/libclang_rt.builtins-aarch64-android.a"

build_so() {   # $1=输出 $2..=源文件
  out="$1"; shift
  $CC -O2 -shared -fPIC -fno-stack-protector -Wl,-soname,$(basename $out) \
      $INCS -Icore/include \
      -o "$out" "$LIBS/crtbegin_so.o" "$@" \
      -L"$LIBS" -llog -Wl,--allow-shlib-undefined "$LIBS/crtend_so.o" "$RT"
}
build_guest() { # $1=输出 $2=源码
  $CC -O2 -static -fno-pic -no-pie -fno-stack-protector -Wl,-e,_start \
      $INCS -o "$1" "$LIBS/crtbegin_static.o" "$2" \
      -L"$LIBS" -lc -lm -ldl "$LIBS/crtend_android.o" "$RT"
}
"$@"
