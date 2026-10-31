#!/usr/bin/env python3
"""TwinVM JNI 注册表审计：tvmbridge.c ↔ ROM framework.dex 逐项对照。

输出：
  ✗ 注册了但 ROM 里不存在该 descriptor（僵尸登记 / 签名错位）
  ✗ ROM 里有 native 但没注册（会在调用时 UnsatisfiedLinkError）
  ✓ 完全一致的类
用法: audit_bridge.py <tvmbridge.c> <framework.jar|dex>
"""
import re
import sys

sys.path.insert(0, __file__.rsplit('/', 1)[0])
from dexnative import load as load_dex  # noqa: E402


def parse_bridge(paths):
    if isinstance(paths, str):
        paths = [paths]
    src = '\n'.join(open(p, encoding='utf-8').read() for p in paths)
    # FindClass 变量 → 类名
    var2cls = dict(re.findall(
        r'jclass\s+(\w+)\s*=\s*\(\*env\)->FindClass\(env,\s*"([^"]+)"\)', src))
    # 注册表数组
    tables = {}
    for m in re.finditer(r'static const JNINativeMethod (k\w+)\[\]\s*=\s*\{(.*?)\n\};',
                         src, re.S):
        body = m.group(2)
        entries = []
        for e in re.finditer(r'\{\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*\(void\*\)(\w+)\s*\}', body):
            entries.append((e.group(1), e.group(2)))
        tables[m.group(1)] = entries
    # cls[] / tabs[] 顺序
    cls_list = re.search(r'jclass cls\[\]\s*=\s*\{([^}]*)\}', src)
    tabs_list = re.search(r'const JNINativeMethod\* tabs\[\]\s*=\s*\{([^}]*)\}', src)
    mapping = []
    if cls_list and tabs_list:
        cls_names = [x.strip() for x in cls_list.group(1).split(',') if x.strip()]
        tab_names = [x.strip() for x in tabs_list.group(1).split(',') if x.strip()]
        for cv, tv in zip(cls_names, tab_names):
            mapping.append((var2cls.get(cv, f'<{cv}>'), tv))
    return mapping, tables


def main():
    bridge, dex = sys.argv[1:-1], sys.argv[-1]
    mapping, tables = parse_bridge(bridge)
    natives = load_dex(dex)
    by_cls = {}
    for cls, name, proto in natives:
        by_cls.setdefault(cls, set()).add((name, proto))

    total_bad = total_missing = 0
    gen_by_san = {}
    for cls in by_cls:
        san = 'kGen_' + re.sub(r'\W', '_', cls[1:-1])
        gen_by_san[san] = cls
    for cls, tab in mapping:
        jni_cls = 'L' + cls.replace('.', '/') + ';' if not cls.startswith('L') else cls
        if tab.startswith('kGen_'):
            jni_cls = gen_by_san.get(tab)
            if jni_cls is None:
                continue
            cls = jni_cls[1:-1].replace('/', '.')
        have = set(tables.get(tab, []))
        want = by_cls.get(jni_cls, set())
        bad = sorted(have - want)
        missing = sorted(want - have)
        # 同名不同签名也常见：单独列出
        status = '✓' if not bad and not missing else '✗'
        print(f'{status} {cls}  [{tab}]  注册 {len(have)} / ROM {len(want)}')
        for n, p in bad:
            print(f'    ✗ 僵尸登记 {n}{p}')
        for n, p in missing:
            print(f'    ✗ 缺失     {n}{p}')
        total_bad += len(bad)
        total_missing += len(missing)
    print(f'\n合计：僵尸/错位 {total_bad} 条，缺失 {total_missing} 条')


if __name__ == '__main__':
    main()
