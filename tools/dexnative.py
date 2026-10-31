#!/usr/bin/env python3
"""从 DEX 里导出 native 方法（类名.方法名 描述符）——用于 JNI 注册表对齐 ROM。

用法：
  dexnative.py <dex|jar> [类名过滤正则]
jar 会自动展开其中所有 classes*.dex。
"""
import re
import struct
import sys
import zipfile


def u16(d, o):
    return struct.unpack_from('<H', d, o)[0]


def u32(d, o):
    return struct.unpack_from('<I', d, o)[0]


def uleb(d, o):
    r = s = 0
    while True:
        b = d[o]
        o += 1
        r |= (b & 0x7f) << s
        s += 7
        if not b & 0x80:
            return r, o


def parse_dex(d):
    (string_ids_size, string_ids_off, type_ids_size, type_ids_off,
     proto_ids_size, proto_ids_off, field_ids_size, field_ids_off,
     method_ids_size, method_ids_off, class_defs_size, class_defs_off) = struct.unpack_from(
        '<12I', d, 0x38)
    strings = []
    for i in range(string_ids_size):
        so = u32(d, string_ids_off + 4 * i)
        ln, o = uleb(d, so)
        strings.append(d[o:o + ln].decode('utf-8', 'replace'))
    types = [strings[u32(d, type_ids_off + 4 * i)] for i in range(type_ids_size)]
    protos = []
    for i in range(proto_ids_size):
        _shorty, ret, poff = struct.unpack_from('<III', d, proto_ids_off + 12 * i)
        params = []
        if poff:
            n = u32(d, poff)
            params = [types[u16(d, poff + 4 + 2 * j)] for j in range(n)]
        protos.append('(' + ''.join(params) + ')' + types[ret])
    methods = []
    for i in range(method_ids_size):
        cls_idx, proto_idx, name_idx = struct.unpack_from('<HHI', d, method_ids_off + 8 * i)
        methods.append((types[cls_idx], strings[name_idx], protos[proto_idx]))
    out = []
    for i in range(class_defs_size):
        off = class_defs_off + 32 * i
        class_idx = u32(d, off)
        cdata = u32(d, off + 24)
        cname = types[class_idx]
        if cdata == 0:
            continue
        o = cdata
        sf, o = uleb(d, o)
        inf, o = uleb(d, o)
        dm, o = uleb(d, o)
        vm, o = uleb(d, o)
        for _ in range(sf + inf):
            _, o = uleb(d, o)
            _, o = uleb(d, o)
        for kind in (0, 1):
            midx = 0
            for _ in range(dm if kind == 0 else vm):
                diff, o = uleb(d, o)
                midx += diff
                acc, o = uleb(d, o)
                _, o = uleb(d, o)   # code_off
                if acc & 0x100:
                    cls, name, proto = methods[midx]
                    out.append((cls, name, proto))
    return out


def load(path):
    if path.endswith('.dex'):
        return parse_dex(open(path, 'rb').read())
    z = zipfile.ZipFile(path)
    res = []
    for n in sorted(x for x in z.namelist() if re.match(r'classes\d*\.dex$', x)):
        res += parse_dex(z.read(n))
    return res


if __name__ == '__main__':
    path = sys.argv[1]
    pat = re.compile(sys.argv[2]) if len(sys.argv) > 2 else None
    for cls, name, proto in load(path):
        if pat and not pat.search(cls):
            continue
        print(f'{cls}\t{name}\t{proto}')
