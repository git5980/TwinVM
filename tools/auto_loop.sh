#!/bin/bash
# TwinVM 自动闭环 v2 —— 构建 → 安装(需一次授权) → 启动 → 等待 → 抓日志 → 分析 → 归档
# 除"安装"外全自动；日志读取走受保护的设备 shell（不依赖无障碍/虚拟屏）。
set -u
ROOT=/root/DshaWorks/sync_066
H=http://127.0.0.1:3090
CK=/root/.dsh/.bridge_headers
BK=/root/dsh-bin/adb-shell
PKG=dev.twinbox.vm
OUT="$ROOT/自动测试日志"; mkdir -p "$OUT"
VER="${1:-auto}"; WAIT="${2:-95}"
say(){ echo "[$(date +%H:%M:%S)] $*"; }

say "① 找最新 APK"
APK=$(ls -t "$ROOT"/android/TwinVM-*.apk 2>/dev/null | head -1)
[ -n "$APK" ] || { say "没有 APK"; exit 1; }
say "   $(basename "$APK")  ($(stat -c%s "$APK") B)"

say "② 导出到设备（Download/DSHA 会提示安装）"
timeout 600 curl -s -G "$H/app/export" --data-urlencode "path=$APK" -H @"$CK" | head -c 140; echo

say "③ 清空旧日志，准备启动"
timeout 60 $BK "rm -f /sdcard/Android/data/$PKG/files/twinvm-live.log" >/dev/null 2>&1
timeout 60 $BK "rm -f /sdcard/Download/auto-live.log" >/dev/null 2>&1

say "④ 启动 App（主屏）"
timeout 60 curl -s -H @"$CK" "$H/app/launch?pkg=$PKG" | head -c 120; echo
sleep 3

say "⑤ 提示：请在手机上点一次「运行 STAGE 4.2 自检」（或用通知栏远程确认）"
timeout 60 curl -s -G "$H/app/notify" --data-urlencode "title=TwinVM 自动测试" \
  --data-urlencode "text=请点 App 里的「运行」按钮，$WAIT 秒后自动收日志" -H @"$CK" >/dev/null 2>&1

say "⑥ 等待 ${WAIT}s 采集"
for i in $(seq 1 "$WAIT"); do
  sleep 1
  if [ $((i % 20)) -eq 0 ]; then
    SZ=$(timeout 20 $BK "ls -la /sdcard/Android/data/$PKG/files/twinvm-live.log" 2>/dev/null | tr -s ' ' | cut -d' ' -f5)
    say "   ${i}s … 日志 ${SZ:-0} B"
  fi
done

say "⑦ 抓日志"
timeout 300 $BK "cp /sdcard/Android/data/$PKG/files/twinvm-live.log /sdcard/Download/auto-live.log" >/dev/null 2>&1
timeout 300 $BK "cp /sdcard/Download/auto-live.log /root/DshaWorks/sync_066/自动测试日志/live-$VER.log" >/dev/null 2>&1
N=$(wc -c < "$OUT/live-$VER.log" 2>/dev/null || echo 0)
say "   $N 字节 → 自动测试日志/live-$VER.log"

say "⑧ 关键结论提取"
grep -aE "▶|GUEST FAULT|回填|GETREGSET|看门狗|槽位退出|code=" "$OUT/live-$VER.log" 2>/dev/null | tail -12 | sed 's/^/   /'
say "完成"
