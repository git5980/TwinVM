#!/usr/bin/env python3
"""TwinVM ROM 一致性检查器（v0.7.22 新增）
========================================
为什么需要它：ROM 是 guest 的"磁盘"，里面所有 .so 必须来自**同一次系统快照**。
真机实锤（2026-10-13 16:48 日志）：ROM 里的 libart.so 是设备当前 ART（APEX 版），
但 libnativebridge.so 是旧版（22656B），少导出 libart.so 需要的
`NativeBridgeIsNativeBridgeFunctionPointer`，于是：

    dlopen failed: cannot locate symbol "NativeBridgeIsNativeBridgeFunctionPointer"
      referenced by "/system/lib64/libart.so"
    -> Failed to initialize JNI invocation API from (null)
    -> dalvikvm 退出 code=1

本工具做三件事（纯 Python，不依赖 readelf，手机/PC 都能跑）：
  1. 每个 .so 的 DT_NEEDED 是否都在 ROM 里；
  2. 每个 .so 的未定义符号（UND）是否都能被 ROM 里**某个** .so 定义；
  3. 打印/汇总缺失项（含"哪个库的哪个符号没人提供"）。

用法:
  python3 rom/checkrom.py <demo.rom>            # 检查
  python3 rom/checkrom.py <demo.rom> --strict   # 有缺失即返回码 1（CI 用）
"""
import os
import re
import struct
import sys
import zlib

HDR = struct.Struct('<8sIIQQII24x')
INO = struct.Struct('<256sIIIQQII')

SHT_DYNSYM, SHT_DYNAMIC, SHT_STRTAB = 11, 6, 3
DT_NULL, DT_NEEDED, DT_SONAME, DT_STRTAB, DT_SYMTAB = 0, 1, 14, 5, 6
SHN_UNDEF = 0


def load_rom(path):
    d = open(path, 'rb').read()
    magic, cnt, isz, doff, dsize, fcrc, _ = HDR.unpack(d[:64])
    assert magic.startswith(b'TVMROM1'), 'bad magic'
    files = {}
    for i in range(cnt):
        name, mode, uid, gid, size, off, crc, xlen = INO.unpack(d[64 + i * 292:64 + (i + 1) * 292])
        name = name.split(b'\0', 1)[0].decode()
        data = d[off:off + size]
        assert zlib.crc32(data) & 0xffffffff == crc, f'crc fail: {name}'
        files[name] = data
    return files


def elf_dyn(data):
    """返回 (soname, needed[], undefined[], defined[])；非 ELF/无节头返回 None"""
    if data[:4] != b'\x7fELF' or data[4] != 2:
        return None
    e_shoff, = struct.unpack_from('<Q', data, 0x28)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from('<HHH', data, 0x3a)
    if e_shoff == 0 or e_shnum == 0:
        return None
    secs = []
    for i in range(e_shnum):
        o = e_shoff + i * e_shentsize
        nameoff, typ, flags, addr, off, size, link, info, align, entsize = struct.unpack_from(
            '<IIQQQQIIQQ', data, o)
        secs.append(dict(nameoff=nameoff, type=typ, off=off, size=size, link=link, entsize=entsize))
    strtab = None
    if e_shstrndx < e_shnum:
        so = secs[e_shstrndx]
        strtab = data[so['off']:so['off'] + so['size']]

    def sname(sec):
        n = sec['nameoff']
        end = strtab.find(b'\0', n)
        return strtab[n:end].decode('utf-8', 'replace')

    dynsym = dynstr = dynamic = None
    for s in secs:
        nm = sname(s)
        if nm == '.dynsym':   dynsym = s
        elif nm == '.dynstr': dynstr = s
        elif nm == '.dynamic': dynamic = s
    if not (dynsym and dynstr and dynamic):
        return None
    dstr = data[dynstr['off']:dynstr['off'] + dynstr['size']]

    def dstr_at(off):
        end = dstr.find(b'\0', off)
        return dstr[off:end].decode('utf-8', 'replace')

    needed, soname = [], None
    for i in range(dynamic['size'] // 16):
        tag, val = struct.unpack_from('<qQ', data, dynamic['off'] + i * 16)
        if tag == DT_NULL:
            break
        if tag == DT_NEEDED:
            needed.append(dstr_at(val))
        elif tag == DT_SONAME:
            soname = dstr_at(val)

    undefined, weak_undef, defined = [], [], []
    for i in range(dynsym['size'] // 24):
        st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from(
            '<IBBHQQ', data, dynsym['off'] + i * 24)
        if st_name == 0:
            continue
        nm = dstr_at(st_name)
        bind = st_info >> 4                      # 0=LOCAL 1=GLOBAL 2=WEAK
        if st_shndx == SHN_UNDEF:
            # WEAK 未定义符号是 AOSP 有意为之（可选钩子，如 __hwasan_*、CallStack::*），
            # 不加载提供者也合法，不算 ROM 不一致
            (weak_undef if bind == 2 else undefined).append(nm)
        else:
            defined.append(nm)
    return soname, needed, undefined, defined, weak_undef


def main():
    rom = sys.argv[1] if len(sys.argv) > 1 else None
    if not rom:
        print(__doc__)
        return 2
    strict = '--strict' in sys.argv
    files = load_rom(rom)
    libs = {}
    for name, data in files.items():
        info = elf_dyn(data)          # 共享库与可执行文件都解析（dalvikvm 也可能提供符号）
        if info:
            libs[name] = info
    print(f'ROM: {rom}  {len(files)} 个文件，其中 {len(libs)} 个 ELF（含可执行文件）\n')

    # 提供者索引：符号名 -> 定义它的文件（basename）
    provider = {}
    for name, (soname, needed, undef, defined, weak) in libs.items():
        base = os.path.basename(name)
        for s in defined:
            provider.setdefault(s, []).append(base)
    have = {os.path.basename(n): n for n in libs}

    bad_missing_lib = bad_missing_sym = 0
    n_weak = 0
    for name in sorted(libs):
        soname, needed, undef, defined, weak = libs[name]
        miss_libs = [n for n in needed if n not in have]
        miss_syms = [s for s in undef if s not in provider]
        n_weak += len([s for s in weak if s not in provider])
        if not miss_libs and not miss_syms:
            continue
        print(f'!! {name}  (soname={soname})')
        for n in miss_libs:
            bad_missing_lib += 1
            print(f'     DT_NEEDED 缺失: {n}（ROM 里没有这个库）')
        for s in sorted(set(miss_syms)):
            bad_missing_sym += 1
            print(f'     未定义符号无人提供: {s}')
    print()
    # ── 启动类路径完整性（v0.7.37 新增）──
    # 真机实锤（2026-10-13 21:22）：ROM 里没有 /apex/**，而 guest 的 BOOTCLASSPATH
    # 指向 /apex/com.android.art/javalib/*.jar，ART 起来就
    #   file_utils.cc:609 Check failed: ... BOOTCLASSPATH ... must not be empty
    # 根因是 android/build.sh 第 2 步只 cp 了 fixture 的 system/，apex/ 被丢掉。
    # 这里把"ROM 里有 dalvikvm ⇒ boot classpath 必须完整"变成机器可查的规则。
    bad_bcp = 0
    has_dvm = any(os.path.basename(n) == 'dalvikvm' for n in files)
    jars = sorted(n for n in files if n.endswith('.jar'))
    pbs = sorted(n for n in files if n.endswith('.pb') and 'classpath' in n)
    if has_dvm or jars:
        for pb in pbs:
            for raw in sorted(set(re.findall(rb'/[A-Za-z0-9_./-]+\.jar', files[pb]))):
                p = raw.decode()
                if p not in files:
                    bad_bcp += 1
                    print(f'!! {pb} 列了 {p}，但 ROM 里没有这个 jar')
        if has_dvm and not any('/apex/com.android.art/javalib/' in n for n in jars):
            bad_bcp += 1
            print('!! ROM 里有 dalvikvm，却没有任何 /apex/com.android.art/javalib/*.jar')
            print('   → guest 的 BOOTCLASSPATH 指不到实体文件（ART: Check failed / 起不来）')
            print('   → 查 android/build.sh 第 2 步有没有把 fixture 的 apex/ 一起打进 ROM')
        elif jars:
            print(f'启动类路径: ROM 内 {len(jars)} 个 jar'
                  + (f'，{len(pbs)} 个 classpaths/*.pb 列出的 jar 全部命中' if pbs else ''))

    if bad_missing_lib == 0 and bad_missing_sym == 0 and bad_bcp == 0:
        print(f'PASS: ROM 内所有 DT_NEEDED 与强未定义符号都能在 ROM 内部解决'
              f'（另有 {n_weak} 个 WEAK 未定义符号按 AOSP 语义允许悬空）')
        return 0
    if bad_missing_lib or bad_missing_sym:
        print(f'FAIL: DT_NEEDED 缺失 {bad_missing_lib} 项，未定义符号无人提供 {bad_missing_sym} 项')
        print('提示：这些库必须来自同一次系统快照。ART 相关库优先取设备 APEX：')
        print('  adb pull /apex/com.android.art/lib64/<lib>   （/system/lib64 下的同名文件可能是旧版/lazy 版）')
    if bad_bcp:
        print(f'FAIL: 启动类路径不完整 {bad_bcp} 项（ART 起不来）')
        print('修法：python3 rom/syncrom.py --rom android/assets/demo.rom '
              '--as-root rom/fixture/android_root --device-root /')
        print('      并让 android/build.sh 第 2 步把 fixture/android_root/ 下所有顶层目录都打进 ROM')
    return 1 if strict else 0


if __name__ == '__main__':
    sys.exit(main())
