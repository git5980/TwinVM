#!/usr/bin/env python3
# TwinVM ROM 镜像工具 v1 —— 对齐 core/src/vm_rom.c 的磁盘格式：
#   头(64B): magic"TVMROM1\0" + u32 inode_count + u32 inode_size(292)
#            + u64 data_off + u64 data_size + u32 file_crc + 保留
#   inode(292B, 小端): name[256] mode u32 uid u32 gid u32
#            size u64 data_off u64 crc32 u32 xattr_len u32
#   data: 顺序数据块（16 字节对齐）
import os
import struct
import sys
import zlib

HDR = struct.Struct('<8sIIQQII24x')      # 64B
INO = struct.Struct('<256sIIIQQII')     # 292B


def pack(root, out):
    files = []
    for dp, _, dns in os.walk(root):
        dns.sort()
        for fn in sorted(os.listdir(dp)):
            full = os.path.join(dp, fn)
            if not os.path.isfile(full):
                continue
            rel = '/' + os.path.relpath(full, root).replace(os.sep, '/')
            files.append((rel, full))
    files.sort()
    if not files:
        sys.exit('empty rom root')

    inode_count = len(files)
    data_off = 64 + inode_count * 292
    blobs, inodes = [], []
    cur = data_off
    for rel, full in files:
        data = open(full, 'rb').read()
        mode = 0o100644 | (0o111 if rel.startswith('/bin/') else 0)
        inodes.append((rel.encode()[:255], mode, 1000, 1000,
                       len(data), cur, zlib.crc32(data) & 0xffffffff, 0))
        blobs.append(data)
        cur += (len(data) + 15) & ~15
    data_size = cur - data_off

    hdr = HDR.pack(b'TVMROM1\0', inode_count, 292, data_off, data_size,
                   zlib.crc32(b''.join(blobs)) & 0xffffffff, 0)
    buf = bytearray(hdr)
    for ino in inodes:
        buf += INO.pack(*ino)
    buf += b'\0' * (data_off - len(buf))       # inode 表尾对齐
    for b in blobs:
        buf += b
        buf += b'\0' * ((-len(b)) % 16)   # 16B 对齐（与 inode.off 一致）
    open(out, 'wb').write(buf)
    print(f'packed {inode_count} inodes, {len(buf)} bytes -> {out}')
    for rel, full in files:
        print(f'  {rel} ({os.path.getsize(full)} bytes)')


def unpack(rom, root):
    d = open(rom, 'rb').read()
    # HDR 有 7 个字段（8s + I + I + Q + Q + I + I），v0.2.7 只解了 6 个 → ValueError（v0.2.8 修正）
    magic, cnt, isz, doff, dsize, fcrc, _reserved = HDR.unpack(d[:64])
    assert magic.startswith(b'TVMROM1'), 'bad magic'
    assert isz == 292
    for i in range(cnt):
        name, mode, uid, gid, size, off, crc, xlen = INO.unpack(d[64 + i * 292:64 + (i + 1) * 292])
        data = d[off:off + size]
        assert zlib.crc32(data) & 0xffffffff == crc, f'crc fail: {name}'
        # name 是 256B 定长字段：必须按第一个 NUL 截断（v0.2.7 直接 decode 会带 "\0" → 路径非法）
        fname = name.split(b'\0', 1)[0].decode()
        p = os.path.join(root, fname.lstrip('/'))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, 'wb').write(data)
        print(f'  {fname} ({size} bytes, crc ok)')


if __name__ == '__main__':
    cmd = sys.argv[1]
    if cmd == 'pack':
        pack(sys.argv[2], sys.argv[3])
    elif cmd == 'unpack':
        unpack(sys.argv[2], sys.argv[3])
    else:
        sys.exit('usage: rompack.py pack <dir> <out.rom> | unpack <in.rom> <dir>')
