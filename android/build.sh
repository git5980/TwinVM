#!/bin/bash
# TwinVM Stage 0 安卓构建链（无 Gradle：NDK + aapt + ECJ + d8 + apksigner）
set -e
cd "$(dirname "$0")"

SDK=/home/z/android-sdk
BT=$SDK/build-tools/34.0.0
PLATFORM=$SDK/platforms/android-34/android.jar
ECJ=/home/z/android-tools/ecj.jar
KEYTOOL=/home/z/re-tools/jdk-21.0.12.1+1/bin/keytool
NDKBIN=$(echo $SDK/ndk/*/toolchains/llvm/prebuilt/linux-x86_64/bin)

OUT=build
rm -rf "$OUT"
mkdir -p "$OUT/obj" "$OUT/apk/lib/arm64-v8a" "$OUT/romroot/bin" "$OUT/romroot/rom" assets

echo "== 1. guest 二进制（arm64 —— Stage 4.1 ylinker 依赖链） =="
# 依赖链三件（arm64，-nostdlib 纯净 DT_NEEDED）
mkdir -p "$OUT/so41" "$OUT/romroot/rom/lib"
cat > "$OUT/liba.c" << 'CEOF'
int liba_base(void) { return 100; }
CEOF
cat > "$OUT/libb.c" << 'CEOF'
int liba_base(void);
int libb_mid(void) { return liba_base() + 20; }
CEOF
cat > "$OUT/libc.c" << 'CEOF'
int libb_mid(void);
int libc_top(void) { return libb_mid() + 3; }
CEOF
"$NDKBIN/clang" -target aarch64-linux-android24 -O2 -shared -fPIC -nostdlib \
    -Wl,-soname,liba.so -o "$OUT/so41/liba.so" "$OUT/liba.c"
"$NDKBIN/clang" -target aarch64-linux-android24 -O2 -shared -fPIC -nostdlib \
    -Wl,-soname,libb.so -L"$OUT/so41" -o "$OUT/so41/libb.so" "$OUT/libb.c" -la
"$NDKBIN/clang" -target aarch64-linux-android24 -O2 -shared -fPIC -nostdlib \
    -Wl,-soname,libc.so -L"$OUT/so41" -o "$OUT/so41/libc.so" "$OUT/libc.c" -lb
"$NDKBIN/clang" -target aarch64-linux-android24 -O2 \
    -static -fno-pic -no-pie \
    -o "$OUT/guest64" ../core/guest/guest_stage42.c
"$NDKBIN/clang" -target aarch64-linux-android24 -O2 \
    -static -fno-pic -no-pie \
    -o "$OUT/child64" ../core/guest/guest_stage42.c
readelf -h "$OUT/guest64" | grep -E "Type:|Machine:" || true
TYPE=$(readelf -h "$OUT/guest64" | awk '/Type:/{print $2}')
if [ "$TYPE" != "EXEC" ]; then
    echo "!! guest 不是 ET_EXEC（Type=$TYPE），装载器只收静态 no-pie"
    exit 1
fi

echo "== 2. ROM 镜像（guest + Android 运行时全家桶） =="
cp ../rom/fixture/rom/hello.txt "$OUT/romroot/rom/"
cp "$OUT/guest64" "$OUT/romroot/bin/guest"
# v0.7.29：ROM 的库树来自 rom/fixture/android_root/system（= guest 看到的 /system）。
#   ★ 这一目录是 30MB+ 的系统库树，可能没随源码分发；缺它时**沿用已有 assets/demo.rom**，
#     不要因为一个 cp 失败就让整个构建挂掉（以前的写法在这里会直接 exit）。
if [ -d ../rom/fixture/android_root/system/lib64 ]; then   # v1.2.40 防护：必须见 lib64 才重打（etc-only 树会打出废 ROM——10-17 实锤过一次）
    # v0.7.37：android_root/ 下**所有**顶层目录整体进 ROM。
    # 旧写法只 cp 了 system/，于是 apex/（v0.7.35 采回来的 javalib/*.jar 与
    # etc/classpaths/*.pb）被静默丢掉 —— 真机表现就是 ART 起来后
    #   file_utils.cc:609 Check failed: ... BOOTCLASSPATH ... must not be empty
    # （boot classpath 指到 /apex/... 但 ROM 里根本没有 /apex）。
    for d in ../rom/fixture/android_root/*; do
        b=$(basename "$d")
        case "$b" in bin|rom) continue ;; esac      # 这两个由本脚本自己产出
        cp -r "$d" "$OUT/romroot/$b"
    done
    cp ../rom/fixture/dex/hello.dex "$OUT/romroot/rom/"
    python3 ../rom/rompack.py pack "$OUT/romroot" assets/demo.rom
else
    if [ ! -f assets/demo.rom ]; then
        echo "!! 既没有 ../rom/fixture/android_root/system，也没有 assets/demo.rom —— 无法产出 ROM"
        echo "   取法见 rom/fixture/android_root/README.txt 或 rom/syncrom.py --as-root"
        exit 1
    fi
    echo "!! 缺 ../rom/fixture/android_root/system —— 沿用已有 assets/demo.rom"
fi
# ROM 一致性守门（v0.7.29）：库必须来自同一次系统快照，否则真机 dlopen 报
#   cannot locate symbol "X" referenced by "libY.so"（libart/libnativeloader 都是 BIND_NOW）
python3 ../rom/checkrom.py assets/demo.rom --strict || {
    echo "!! ROM 一致性检查未过 —— 先 rom/syncrom.py 同步库再加包（见 修复报告-v0.7.29.md）"
    exit 1
}

echo "== 3. libtwinvm.so（引擎核心，arm64） =="
"$NDKBIN/clang" -target aarch64-linux-android24 -O2 -shared -fPIC \
    -I../core/include -I../core/src \
    -o "$OUT/apk/lib/arm64-v8a/libtwinvm.so" \
    ../core/src/vm_core.c ../core/src/vm_binder.c ../core/src/vm_rom.c ../core/src/vm_loader.c \
    ../core/src/vm_backend_sigsys.c ../core/src/vm_notif.c \
    jni/twinvm_jni.c -llog

echo "== 4. Java（ECJ，无 lambda——匿名内部类纪律） =="
java -jar "$ECJ" -source 8 -target 8 -nowarn \
    -bootclasspath "$PLATFORM" -d "$OUT/obj" \
    src/dev/twinbox/vm/Native.java src/dev/twinbox/vm/MainActivity.java src/dev/twinbox/vm/RunService.java

echo "== 5. dex（d8） =="
"$BT/d8" --release --min-api 24 --lib "$PLATFORM" \
    --output "$OUT/apk" $(find "$OUT/obj" -name '*.class')

echo "== 6. APK 组装 =="
"$BT/aapt" package -f -M AndroidManifest.xml -S res -A assets \
    -I "$PLATFORM" -F "$OUT/twinvm.unsigned.apk"
( cd "$OUT/apk" && "$BT/aapt" add ../twinvm.unsigned.apk classes.dex lib/arm64-v8a/libtwinvm.so )

echo "== 7. 对齐 + 签名 =="
"$BT/zipalign" -f 4 "$OUT/twinvm.unsigned.apk" "$OUT/twinvm.aligned.apk"
if [ ! -f twinvm.keystore ]; then
    "$KEYTOOL" -genkeypair -keystore twinvm.keystore -storepass twinvm123 \
        -keyalg RSA -keysize 2048 -validity 10000 -alias twinvm \
        -dname "CN=TwinVM" >/dev/null 2>&1
fi
OUTAPK=TwinVM-Stage4-v1.2.147.apk
"$BT/apksigner" sign --ks twinvm.keystore --ks-pass pass:twinvm123 \
    --out "$OUTAPK" "$OUT/twinvm.aligned.apk"

echo "== 8. 验证 =="
"$BT/apksigner" verify "$OUTAPK" && echo "SIGN_OK"
"$BT/aapt" dump badging "$OUTAPK" | head -4
unzip -l "$OUTAPK" | grep -E "libtwinvm|demo.rom|classes.dex"
ls -la "$OUTAPK"
