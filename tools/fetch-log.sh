#!/bin/bash
# 拉取 TA 的上行日志（v1.2.162 日志通道）
# 用法: ./fetch-log.sh [c|b|空]   （容器C/B/A）
WHICH="${1:-c}"
NAME="twinvm-live${WHICH:+-$WHICH}.log"
[ "$WHICH" = "c" ] && NAME="twinvm-live-c.log"
[ "$WHICH" = "b" ] && NAME="twinvm-live-b.log"
[ "$WHICH" = "a" ] && NAME="twinvm-live.log"
URL="https://raw.githubusercontent.com/git5980/TwinVM/logs/logs/$NAME"
OUT="/tmp/auto-$NAME"
for i in 1 2 3; do
    code=$(curl -sL --max-time 25 "$URL" -o "$OUT" -w "%{http_code}")
    if [ "$code" = "200" ] && [ -s "$OUT" ]; then
        echo "✓ $NAME ($(stat -c%s "$OUT")B) → $OUT"
        exit 0
    fi
    sleep 4
done
echo "✗ 未拉到（code=$code）——TA 未上行或未配置"
exit 1
