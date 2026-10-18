#!/usr/bin/env python3
"""rom_add.py —— 就地向 ROM 追加文件（v1.2.40）
用法: rom_add.py <in.rom> <out.rom> <本地路径:ROM路径> [...]
逐 inode 读原文件（数据/权限/CRC 原样），新文件按序插入，重打包。
不依赖 fixture 树（lib64 已从 fixture 退役到 stale-bak，全量重打会丢库）。
"""
import struct
import sys
import zlib

HDR = struct.Struct('<8sIIQQII24x')
INO = struct.Struct('<256sIIIQQII')


def load(rom):
    d = open(rom, 'rb').read()
    magic, cnt, isz, doff, dsize, fcrc, _ = HDR.unpack(d[:64])
    assert magic.startswith(b'TVMROM1') and isz == 292
    items = []
    for i in range(cnt):
        name, mode, uid, gid, size, off, crc, xlen = INO.unpack(
            d[64 + i * 292:64 + (i + 1) * 292])
        data = d[off:off + size]
        assert zlib.crc32(data) & 0xffffffff == crc, f'crc fail: {name}'
        items.append([name.split(b'\0', 1)[0].decode(), mode, uid, gid, data])
    return items


def save(items, out):
    items = sorted(items, key=lambda x: x[0])
    cnt = len(items)
    doff = 64 + cnt * 292
    blobs, inodes = [], []
    cur = doff
    for name, mode, uid, gid, data in items:
        inodes.append((name.encode()[:255], mode, uid, gid,
                       len(data), cur, zlib.crc32(data) & 0xffffffff, 0))
        blobs.append(data)
        cur += (len(data) + 15) & ~15
    hdr = HDR.pack(b'TVMROM1\0', cnt, 292, doff, cur - doff,
                   zlib.crc32(b''.join(blobs)) & 0xffffffff, 0)
    buf = bytearray(hdr)
    for ino in inodes:
        buf += INO.pack(*ino)
    assert len(buf) == doff
    for b in blobs:
        buf += b
        buf += b'\0' * ((-len(b)) % 16)
    open(out, 'wb').write(buf)
    print(f'ok: {cnt} inodes, {len(buf)} bytes -> {out}')
    for name, mode, _, _, data in items:
        print(f'  {name} ({len(data)} bytes)')


if __name__ == '__main__':
    src, dst = sys.argv[1], sys.argv[2]
    items = load(src)
    old = {it[0] for it in items}
    for spec in sys.argv[3:]:
        local, _, rpath = spec.partition(':')
        if not rpath.startswith('/'):
            sys.exit(f'ROM 路径须以 / 开头: {spec}')
        if rpath in old:
            sys.exit(f'已存在（如需覆盖请先扩展本工具）: {rpath}')
        data = open(local, 'rb').read()
        items.append([rpath, 0o100644, 1000, 1000, data])
        print(f'+ {rpath} <- {local} ({len(data)} bytes)')
    save(items, dst)
