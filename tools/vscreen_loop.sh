#!/bin/bash
# TwinVM 虚拟屏自动闭环 v1.1.0
#   构建 → 装(一次授权) → 虚拟屏启动 → 点「运行」→ 轮询进度 → 抓日志 → 分析归档
set -u
ROOT=/root/DshaWorks/sync_066
H=http://127.0.0.1:3090
CK=/root/.dsh/.bridge_headers
BK=/root/dsh-bin/adb-shell
PKG=dev.twinbox.vm
OUT="$ROOT/自动测试日志"; mkdir -p "$OUT"
VER="${1:-auto}"; WAIT="${2:-120}"
say(){ echo "[$(date +%H:%M:%S)] $*"; }

api(){ timeout 90 curl -s -H @"$CK" "$H$1"; }
jf(){ python3 -c "
import sys,json
try:
    r=json.loads(json.load(sys.stdin).get('result','{}'))
    print(r.get('$1',''))
except Exception: print('')"; }
snap(){  # snap <name>  → 存预览图
  api "/app/vscreen/see" > /tmp/_vs.json
  python3 -c "
import json,base64,sys
try:
    r=json.loads(json.load(open('/tmp/_vs.json'))['result'])
    open('/tmp/$1.jpg','wb').write(base64.b64decode(r.get('previewB64','')))
    print(r.get('frameSeq'))
except Exception as e: print('0')"
}
taprun(){ # 点击「运行」按钮（display 1008x1792 下 x=504,y=218）
  local FS GEN
  FS=$(api "/app/vscreen/status" | jf frameSeq)
  GEN=$(api "/app/vscreen/status" | jf generation)
  api "/app/vscreen/tap?x=504&y=218&frameSeq=$FS&generation=$GEN" >/dev/null
}

say "① 最新 APK"
APK=$(ls -t "$ROOT"/android/TwinVM-*.apk 2>/dev/null | head -1)
[ -n "$APK" ] || { say "无 APK"; exit 1; }
say "   $(basename "$APK")"

say "② 虚拟屏"
api "/app/vscreen/close" >/dev/null 2>&1; sleep 1
api "/app/vscreen/create?orientation=portrait" | head -c 130; echo
sleep 2

say "③ 虚拟屏内启动 $PKG"
api "/app/vscreen/launch?package=$PKG" | head -c 150; echo
sleep 5

say "④ 点「运行」"
taprun; sleep 4
F=$(snap after-tap); say "   点击后 frameSeq=$F → /tmp/after-tap.jpg"

say "⑤ 轮询进度（${WAIT}s）"
for i in $(seq 1 "$WAIT"); do
  sleep 1
  if [ $((i % 15)) -eq 0 ]; then
    st=$(api "/app/vscreen/status")
    fs=$(echo "$st" | jf frameSeq)
    sz=$(timeout 20 $BK "ls -la /sdcard/Android/data/$PKG/files/twinvm-live.log" 2>/dev/null | tr -s ' ' | cut -d' ' -f5)
    say "   ${i}s ｜ frameSeq=$fs ｜ 日志 ${sz:-0} B"
  fi
done

say "⑥ 抓日志 + 截屏"
snap "final-$VER" >/dev/null
# 日志回传：设备 shell 直接 cat（cp 到工作区被策略拦；readfile/export 不收 .log）
timeout 300 $BK "cp /sdcard/Android/data/$PKG/files/twinvm-live.log /sdcard/Download/auto-live.log" >/dev/null 2>&1
timeout 300 $BK "cat /sdcard/Download/auto-live.log" > "$OUT/live-$VER.log" 2>/dev/null
say "   $(wc -c < "$OUT/live-$VER.log" 2>/dev/null || echo 0) 字节 → 自动测试日志/live-$VER.log"
cp /tmp/final-$VER.jpg "$OUT/screen-$VER.jpg" 2>/dev/null && say "   截图 → 自动测试日志/screen-$VER.jpg"

say "⑦ 关键结论"
grep -aE "▶|GUEST FAULT|回填|GETREGSET|看门狗|槽位退出|code=|已虚拟化" "$OUT/live-$VER.log" 2>/dev/null | tail -14 | sed 's/^/   /'
say "完成（虚拟屏保持打开，可复查）"
