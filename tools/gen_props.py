#!/usr/bin/env python3
"""gen_props.py —— 生成 bionic PreSplit 单文件属性区（v1.2.41）
用法: gen_props.py <out.bin> name=value ...
格式（bionic android16-release 源码核对，libc/system_properties/*）：
  prop_area 头 128B: bytes_used_(u32) serial(u32) magic(u32=0x504f5250)
                     version(u32=0xfc6ed0ab) reserved[28](112B)
  data_（偏移 128 起，offset 相对其）:
    root trie node at 0（namelen=0，无 prop）
    prop_trie_node: namelen u32 | prop u32 | left u32 | right u32 | children u32
                    | name[namelen+1]   （allocate_obj 4 字节对齐）
    prop_info:      serial u32(=valuelen<<24) | value[92] | name[namelen+1]（对齐）
  查找: 名字按 '.' 分段 → 每段在父 children 的 BST（strcmp 序）找 → 叶节点 prop
BST 构造：kids 排序后"右斜树"（根=最小 kid，right=下一个更大 kid，left=0），
对有序序列是合法 BST，与 find_prop_trie_node 的 walk 兼容。
"""
import struct
import sys

MAGIC = 0x504f5250
VERSION = 0xfc6ed0ab
PROP_VALUE_MAX = 92


class Node:
    def __init__(self, name):
        self.name = name
        self.kids = {}
        self.prop = None


def align4(x):
    return (x + 3) & ~3


def build_file(root):
    trie = []

    def walk(n):
        trie.append(n)
        for k in sorted(n.kids):
            walk(n.kids[k])

    walk(root)
    off, pinfo = {}, {}
    cur = 0
    for n in trie:
        off[id(n)] = cur
        cur += align4(20 + len(n.name) + 1)
    for n in trie:
        if n.prop is not None:
            pinfo[id(n)] = cur
            cur += align4(4 + PROP_VALUE_MAX + len(n.prop[0]) + 1)
    total = cur

    buf = bytearray(128 + total)
    struct.pack_into('<IIII', buf, 0, 128 + total, 0, MAGIC, VERSION)
    for n in trie:
        o = 128 + off[id(n)]
        struct.pack_into('<IIIII', buf, o, len(n.name), pinfo.get(id(n), 0), 0, 0, 0)
        if n.name:
            struct.pack_into(f'<{len(n.name)+1}s', buf, o + 20, n.name.encode() + b'\0')
    for n in trie:
        o = 128 + off[id(n)]
        kids = sorted(n.kids)
        if kids:
            struct.pack_into('<I', buf, o + 16, off[id(n.kids[kids[0]])])
            for i, k in enumerate(kids[:-1]):
                ko = 128 + off[id(n.kids[k])]
                struct.pack_into('<I', buf, ko + 12, off[id(n.kids[kids[i + 1]])])
    for n in trie:
        if n.prop is not None:
            name, value = n.prop
            o = 128 + pinfo[id(n)]
            vb = value.encode()
            struct.pack_into('<I', buf, o, len(vb) << 24)
            buf[o + 4:o + 4 + len(vb) + 1] = vb + b'\0'
            struct.pack_into(f'<{len(name)+1}s', buf, o + 4 + PROP_VALUE_MAX,
                             name.encode() + b'\0')
    return bytes(buf)


def verify(data, props):
    """模拟 bionic find_property：分段 → children BST walk → prop"""
    def obj(off):
        return 128 + off

    def rd_node(o):
        namelen, prop, left, right, children = struct.unpack_from('<IIIII', data, o)
        name = data[o + 20:o + 20 + namelen].decode()
        return name, prop, left, right, children

    def find_in_bst(children_off, seg):
        cur = children_off
        while cur:
            name, prop, left, right, children = rd_node(obj(cur))
            if seg == name:
                return cur, prop
            cur = right if seg > name else left
        return 0, 0

    root = rd_node(obj(0))
    ok = 0
    for name, value in props:
        segs = name.split('.')
        cur_children = root[4]
        prop_off = 0
        for seg in segs:
            cur_children, prop_off = find_in_bst(cur_children, seg)
            if not cur_children:
                break
            cur_children = rd_node(obj(cur_children))[4]
        if prop_off:
            po = obj(prop_off)
            serial, = struct.unpack_from('<I', data, po)
            vlen = serial >> 24
            got = data[po + 4:po + 4 + vlen].decode()
            if got == value:
                ok += 1
                continue
        print(f'!! {name}: 查找失败')
    return ok


def main():
    out = sys.argv[1]
    props = []
    for a in sys.argv[2:]:
        k, _, v = a.partition('=')
        assert k and len(v) < PROP_VALUE_MAX, a
        props.append((k, v))
    root = Node('')
    for name, value in props:
        cur = root
        for seg in name.split('.'):
            cur = cur.kids.setdefault(seg, Node(seg))
        cur.prop = (name, value)
    data = build_file(root)
    open(out, 'wb').write(data)
    ok = verify(data, props)
    print(f'{out}: {len(data)} bytes, 校验 {ok}/{len(props)} 属性通过')
    assert ok == len(props)


if __name__ == '__main__':
    main()
