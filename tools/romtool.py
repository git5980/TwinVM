#!/usr/bin/env python3
"""TwinVM ROM 镜像工具（与 core/src/vm_rom.c 的 vm_rom_replace 逐字节等价）

TVMROM1 布局：
  0x00 magic[8]="TVMROM1\0"
  0x08 u32 inode_count
  0x0C u32 inode_size(292)
  0x10 u64 data_off
  0x18 u64 data_size
  0x20 u32 file_crc（按 inode 顺序串接 blob 的 zlib crc32）
  0x24..0x3F 保留
  64B 头 + count*292 inode 表 + 16 对齐数据块

inode(292B)：
  0    char[256] path
  256  u32 mode
  260  u32 uid
  264  u32 gid
  268  u64 size
  276  u64 data_off
  284  u32 crc32
  288  u32 xattr_len

用法：
  romtool.py list   <rom>
  romtool.py extract <rom> <inner_path> <local_out>
  romtool.py replace <rom> <inner_path> <local_in> [<out_rom>]
  romtool.py add     <rom> <inner_path> <local_in> [<out_rom>]
  romtool.py verify  <rom>
"""
import struct
import sys
import zlib

HDR = struct.Struct('<8sIIQQII24x')
INO = struct.Struct('<256sIIIQQII')
MAGIC = b'TVMROM1\0'
ISIZE = 292


class Rom:
    def __init__(self, path):
        self.path = path
        self.data = bytearray(open(path, 'rb').read())
        magic, cnt, isz, doff, dsize, fcrc, _ = HDR.unpack(self.data[:64])
        if not magic.startswith(b'TVMROM1') or isz != ISIZE:
            raise SystemExit(f'不是 TVMROM1 镜像: {path}')
        self.count, self.isize = cnt, isz
        self.data_off, self.data_size, self.file_crc = doff, dsize, fcrc
        self.inodes = []
        for i in range(cnt):
            name, mode, uid, gid, size, off, crc, xlen = INO.unpack(
                self.data[64 + i * ISIZE:64 + (i + 1) * ISIZE])
            self.inodes.append({
                'name': name.split(b'\0', 1)[0].decode('utf-8', 'replace'),
                'raw_name': name, 'mode': mode, 'uid': uid, 'gid': gid,
                'size': size, 'off': off, 'crc': crc, 'xlen': xlen,
            })

    def blob(self, ino):
        return bytes(self.data[ino['off']:ino['off'] + ino['size']])

    def find(self, path):
        for i, ino in enumerate(self.inodes):
            if ino['name'] == path:
                return i
        return -1

    def verify(self):
        for ino in self.inodes:
            b = self.blob(ino)
            if len(b) != ino['size'] or zlib.crc32(b) & 0xffffffff != ino['crc']:
                raise SystemExit(f'inode crc fail: {ino["name"]}')
        cat = b''.join(self.blob(ino) for ino in self.inodes)
        if zlib.crc32(cat) & 0xffffffff != self.file_crc:
            raise SystemExit(f'file_crc fail: {self.file_crc:#x} != '
                             f'{zlib.crc32(cat) & 0xffffffff:#x}')
        return True

    def build(self, blobs):
        """blobs: [(raw_name, mode, uid, gid, data)]，保持给定顺序。"""
        cnt = len(blobs)
        doff = 64 + cnt * ISIZE
        buf = bytearray(doff)
        cur = doff
        inos = []
        body = bytearray()
        for raw_name, mode, uid, gid, data in blobs:
            crc = zlib.crc32(data) & 0xffffffff
            inos.append((raw_name, mode, uid, gid, len(data), cur, crc, 0))
            body += data
            body += b'\0' * ((-len(data)) % 16)
            cur += (len(data) + 15) & ~15
        file_crc = zlib.crc32(b''.join(b[4] for b in blobs)) & 0xffffffff
        HDR.pack_into(buf, 0, MAGIC, cnt, ISIZE, doff, cur - doff, file_crc, 0)
        for i, ino in enumerate(inos):
            INO.pack_into(buf, 64 + i * ISIZE, *ino)
        return bytes(buf) + bytes(body)

    def replace(self, path, new_data, out=None):
        i = self.find(path)
        if i < 0:
            raise SystemExit(f'ROM 内不存在: {path}（需要 add 语义）')
        blobs = []
        for j, ino in enumerate(self.inodes):
            data = new_data if j == i else self.blob(ino)
            mode = ino['mode']
            if j == i and not (mode & 0o777):
                mode = 0o100644
            blobs.append((ino['raw_name'], mode, ino['uid'], ino['gid'], data))
        out = out or (self.path + '.new')
        open(out, 'wb').write(self.build(blobs))
        return out

    def add(self, path, new_data, out=None):
        if self.find(path) >= 0:
            raise SystemExit(f'已存在: {path}')
        blobs = [(ino['raw_name'], ino['mode'], ino['uid'], ino['gid'], self.blob(ino))
                 for ino in self.inodes]
        raw = path.encode('utf-8')[:255]
        blobs.append((raw, 0o100644, 1000, 1000, new_data))
        out = out or (self.path + '.new')
        open(out, 'wb').write(self.build(blobs))
        return out


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    cmd, rom = sys.argv[1], sys.argv[2]
    r = Rom(rom)
    if cmd == 'list':
        for i, ino in enumerate(r.inodes):
            print(f'{i:3d} {ino["mode"]:o} {ino["size"]:9d} '
                  f'{ino["off"]:10d} {ino["crc"]:08x} {ino["name"]}')
    elif cmd == 'verify':
        r.verify()
        print(f'OK: {r.count} inodes, file_crc={r.file_crc:#010x}')
    elif cmd == 'extract':
        p, out = sys.argv[3], sys.argv[4]
        i = r.find(p)
        if i < 0:
            raise SystemExit(f'不存在: {p}')
        open(out, 'wb').write(r.blob(r.inodes[i]))
        print(f'extracted {p} -> {out} ({r.inodes[i]["size"]} bytes)')
    elif cmd in ('replace', 'add'):
        p, src = sys.argv[3], sys.argv[4]
        out = sys.argv[5] if len(sys.argv) > 5 else rom + '.new'
        data = open(src, 'rb').read()
        out = r.replace(p, data, out) if cmd == 'replace' else r.add(p, data, out)
        print(f'{cmd} {p} <- {src} ({len(data)} bytes) -> {out}')
        r2 = Rom(out)
        r2.verify()
        print(f'verify OK: {r2.count} inodes, {len(r2.data)} bytes')
    else:
        raise SystemExit(__doc__)


if __name__ == '__main__':
    main()
