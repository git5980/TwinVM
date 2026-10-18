#!/bin/bash
# 本机适配版 APK 打包（无 Gradle：ECJ + d8 + aapt + zipalign + apksigner）
# 前置：tools/build_android_here.sh 已产出 android/libtwinvm.so
set -e
cd "$(dirname "$0")"
BT=/opt/android-sdk/build-tools/34.0.0
PLATFORM=/opt/android-sdk/platforms/android-34/android.jar
ECJ=/opt/android-sdk/ecj.jar
VER="${1:-1.2.18}"
OUT=build
rm -rf "$OUT"; mkdir -p "$OUT/obj" "$OUT/apk/lib/arm64-v8a"

echo "== Java（ECJ，无 lambda） =="
java -jar "$ECJ" -source 8 -target 8 -nowarn -bootclasspath "$PLATFORM" -d "$OUT/obj" \
    src/dev/twinbox/vm/Native.java src/dev/twinbox/vm/MainActivity.java

echo "== dex（d8） =="
"$BT/d8" --release --min-api 24 --lib "$PLATFORM" --output "$OUT/apk" \
    $(find "$OUT/obj" -name '*.class')

echo "== APK 组装 =="
cp libtwinvm.so "$OUT/apk/lib/arm64-v8a/"
"$BT/aapt" package -f -M AndroidManifest.xml -S res -A assets -I "$PLATFORM" -F "$OUT/u.apk"
( cd "$OUT/apk" && "$BT/aapt" add ../u.apk classes.dex lib/arm64-v8a/libtwinvm.so )

echo "== 对齐 + 签名 =="
"$BT/zipalign" -f 4 "$OUT/u.apk" "$OUT/a.apk"
OUTAPK="TwinVM-Stage4-v${VER}.apk"
"$BT/apksigner" sign --ks twinvm.keystore --ks-pass pass:twinvm123 --out "$OUTAPK" "$OUT/a.apk"

echo "== 验证 =="
"$BT/apksigner" verify "$OUTAPK" && echo SIGN_OK
"$BT/aapt" dump badging "$OUTAPK" | head -2
unzip -l "$OUTAPK" | grep -E "libtwinvm|demo.rom|classes.dex"
