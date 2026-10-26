#!/bin/bash
# TwinVM 热更发布器：版本化文件名（CDN 缓存免疫）+ 新 tag + meta 更新
# 用法: ./publish.sh <bootstrap.jar路径> [libtvmbridge.so路径]
set -e
cd /home/z/my-project/twinvm
VER=$(date +%m%d%H%M)
TAG="hotfix-${VER}"

cp "${1:?用法: publish.sh <bootstrap.jar> [so]}" "patches/bootstrap-${VER}.bin"
[ -n "$2" ] && cp "$2" patches/libtvmbridge.so

python3 - "$VER" <<'PYEOF'
import json, hashlib, os, sys
ver = sys.argv[1]
m = {"files": [], "ver": int(ver), "note": f"hotfix {ver}"}
for name, inner in [(f"bootstrap-{ver}.bin", "/system/framework/bootstrap.jar"),
                    ("libtvmbridge.so", "/system/lib64/libtvmbridge.so")]:
    p = f"patches/{name}"
    if not os.path.exists(p): continue
    m["files"].append({"name": name, "inner": inner,
                       "md5": hashlib.md5(open(p,'rb').read()).hexdigest(),
                       "size": os.path.getsize(p)})
json.dump(m, open("patches/meta.json","w"), indent=2)
print(json.dumps(m, indent=2))
PYEOF

git add patches/
git commit -q -m "热更 ${VER}：bootstrap-${VER}.bin（版本化文件名=新缓存键）"
git tag -f "${TAG}"
echo "已提交并打 tag: ${TAG}"
