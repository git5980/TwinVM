#!/usr/bin/env python3
"""TwinVM ROM 库同步器 —— 用设备当前系统里的同名文件替换 ROM 里的旧库（v0.7.22 新增）
================================================================================
背景：ROM 里所有 .so 必须来自**同一次系统快照**。设备升级/换机后，ROM 里只要有
一个库还是旧版，guest 的 linker 就会在 dlopen 时报
`cannot locate symbol "X" referenced by "libY.so"`（真机 2026-10-13 16:48 实锤：
libart.so 是设备当前 ART，但 libnativebridge.so 是旧版，少导出
NativeBridgeIsNativeBridgeFunctionPointer → dalvikvm 退出 code=1）。

本工具只替换「设备上找得到、且内容不一致」的条目，并把结果重新打包 + 做一致性检查，
项目自带文件（/bin/guest、/rom/*、/system/etc/ld.config.txt 等）一律不动。

查找顺序（安卓 10+ ART 在 APEX 里，/system/lib64 下的同名文件常是旧版或 lazy 版）：
  <root>/apex/com.android.art/lib64/X   ← ART 库优先
  <root>/system/lib64/X
  <root>/apex/com.android.art/bin/X     ← dalvikvm 等
  <root>/system/bin/X
  <root>/apex/com.android.runtime/lib64/X、<root>/apex/com.android.i18n/lib64/X

用法：
  # 在本机（能直接看到 /apex、/system）：
  python3 rom/syncrom.py --rom android/assets/demo.rom --out demo-fixed.rom --device-root /
  # 在 PC 上用 adb 先把库拉到本地目录再同步：
  python3 rom/syncrom.py --rom demo.rom --out demo-fixed.rom --device-root ./sysdump \\
      --map system/lib64=/apex/com.android.art/lib64,system/lib64

同步完会调用 rom/checkrom.py 做校验（有致命的未定义符号就报 FAIL）。
"""
import argparse
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import rompack  # noqa: E402  复用打包器（格式/权限/crc 约定一致）
import checkrom  # noqa: E402

HDR = rompack.HDR
INO = rompack.INO

# 项目自带、绝不能从设备覆盖的条目
KEEP = ('/bin/guest', '/rom/hello.txt', '/rom/hello.dex', '/system/etc/ld.config.txt')

SEARCH = {
    'system/lib64/': ['apex/com.android.art/lib64', 'system/lib64',
                      'apex/com.android.runtime/lib64', 'apex/com.android.i18n/lib64',
                      'apex/com.android.conscrypt/lib64', 'apex/com.android.os.statsd/lib64'],
    'system/bin/':   ['apex/com.android.art/bin', 'system/bin', 'apex/com.android.runtime/bin'],
    'system/etc/':   ['system/etc'],
}


def md5(b):
    return hashlib.md5(b).hexdigest()


def unpack_to(rom_path, dest):
    """按 rompack 的格式把 ROM 展开成目录（保留 1000:1000 / 0644 / bin 可执行约定）"""
    d = open(rom_path, 'rb').read()
    magic, cnt, isz, doff, dsize, fcrc, _ = HDR.unpack(d[:64])
    assert magic.startswith(b'TVMROM1'), 'bad magic: %r' % magic
    assert isz == 292, 'unexpected inode size %d' % isz
    for i in range(cnt):
        name, mode, uid, gid, size, off, crc, xlen = INO.unpack(d[64 + i * 292:64 + (i + 1) * 292])
        name = name.split(b'\0', 1)[0].decode()
        data = d[off:off + size]
        p = os.path.join(dest, name.lstrip('/'))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, 'wb').write(data)
    return cnt


def find_device_file(root, rel, extra_map=None):
    """rel 形如 'system/lib64/libart.so'；返回设备上对应文件路径或 None。

    v0.7.37：先走各目录的优先搜索顺序，最后加一条**同名路径兜底** ——
    ROM 里的 /apex/com.android.art/javalib/*.jar、etc/classpaths/*.pb、
    boot.art/oat/vdex 在设备上就是同一个路径（以前这条路径匹配不到，
    于是 --as-root 重建 fixture 时这些文件只能沿用 ROM 里的旧副本）。"""
    for prefix, dirs in SEARCH.items():
        if rel.startswith(prefix):
            tail = rel[len(prefix):]
            for d in dirs:
                c = os.path.join(root, d, tail)
                if os.path.isfile(c):
                    return c
            break
    direct = os.path.join(root, rel)          # 同名路径兜底
    if os.path.isfile(direct):
        return direct
    # ART 启动镜像：ROM 里放在 boot classpath 目录下（ART 默认推算位置），
    # 设备上在 /system/framework/<isa>/boot.{art,oat,vdex}
    if rel in ('apex/com.android.art/javalib/boot.art',
               'apex/com.android.art/javalib/boot.oat',
               'apex/com.android.art/javalib/boot.vdex'):
        for isa in ('arm64', 'arm'):
            c = os.path.join(root, 'system/framework', isa, os.path.basename(rel))
            if os.path.isfile(c):
                return c
    if extra_map:
        for dev_dir, _ in extra_map:
            c = os.path.join(root, dev_dir, os.path.basename(rel))
            if os.path.isfile(c):
                return c
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--rom', required=True)
    ap.add_argument('--out', default='', help='输出 ROM 路径（与 --as-root 二选一）')
    ap.add_argument('--as-root', default='',
                    help='不打包 ROM，而是把同步后的树写成 build.sh 要的 '
                         'rom/fixture/android_root（含 system/ apex/ 等顶层目录）')
    ap.add_argument('--device-root', default='/')
    ap.add_argument('--map', default='', help='额外映射，如 system/lib64=./sysdump/apex-ART')
    ap.add_argument('--drop', default='',
                    help='逗号分隔：从 ROM 里删掉的路径（如自造 shim 已被真库取代时）')
    ap.add_argument('--dry-run', action='store_true')
    a = ap.parse_args()

    extra = []
    for item in [x for x in a.map.split(',') if x.strip()]:
        if '=' in item:
            k, v = item.split('=', 1)
            extra.append((v, k))
    root = a.device_root.rstrip('/') or '/'

    tmp = tempfile.mkdtemp(prefix='tvm-rom-')
    try:
        n = unpack_to(a.rom, tmp)
        print(f'ROM {a.rom}: {n} 个 inode 已展开')

        dropped = []
        for rel in [x.strip().lstrip('/') for x in a.drop.split(',') if x.strip()]:
            p = os.path.join(tmp, rel)
            if os.path.isfile(p):
                os.remove(p); dropped.append(rel)
        if dropped:
            print('删除（--drop）:', ', '.join(dropped))

        replaced, unchanged, notfound = [], [], []
        for dp, _, fns in os.walk(tmp):
            for fn in sorted(fns):
                p = os.path.join(dp, fn)
                rel = os.path.relpath(p, tmp).replace(os.sep, '/')
                if '/' + rel in KEEP:
                    continue
                dev = find_device_file(root, rel, extra)
                if not dev:
                    notfound.append(rel)
                    continue
                old = open(p, 'rb').read()
                new = open(dev, 'rb').read()
                if md5(old) == md5(new):
                    unchanged.append(rel)
                    continue
                print(f'  替换 {rel}: {len(old)}B({md5(old)[:12]}) -> {len(new)}B({md5(new)[:12]})  [{dev}]')
                if not a.dry_run:
                    open(p, 'wb').write(new)
                replaced.append(rel)

        print(f'\n替换 {len(replaced)} 项，一致 {len(unchanged)} 项，设备上无对应 {len(notfound)} 项')
        for x in notfound:
            print(f'  (保留原样) {x}')
        if a.dry_run:
            return 0
        if a.as_root:
            # 直接把同步后的树写成 build.sh 要的 fixture（system/ apex/ ... 所有顶层目录）
            os.makedirs(a.as_root, exist_ok=True)
            for top in sorted(os.listdir(tmp)):
                if top in ('bin', 'rom'):
                    continue                      # 这两个由 build.sh 自己产出
                src, dst = os.path.join(tmp, top), os.path.join(a.as_root, top)
                if os.path.isdir(dst):
                    shutil.rmtree(dst)
                shutil.copytree(src, dst)
                print(f'  写出 {dst}')
            print(f'\n已写出 fixture 树: {a.as_root}（android/build.sh 第 2 步会用它的所有顶层目录打 ROM）')
            return 0
        if not a.out:
            print('!! 需要 --out <rom> 或 --as-root <dir>')
            return 2
        rompack.pack(tmp, a.out)
        print(f'\n已写出 {a.out}')
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print('\n===== 一致性检查 =====')
    return subprocess.call([sys.executable, os.path.join(HERE, 'checkrom.py'), a.out, '--strict'])


if __name__ == '__main__':
    sys.exit(main())
