#!/usr/bin/env python3
"""生成"缺失 native"补充注册表：只登记 ROM 有、桥里还没有的 (name, descriptor)。
不覆盖已有实现（自定义语义保留）。用法:
  gen_missing.py <tvmbridge.c> <gen_registry.h> <framework.jar> <out.h> <class...>
"""
import re
import sys

sys.path.insert(0, __file__.rsplit('/', 1)[0])
from dexnative import load as load_dex  # noqa: E402


def registered(src):
    """class(JNI 名) -> set((name, proto))"""
    var2cls = dict(re.findall(
        r'jclass\s+(\w+)\s*=\s*\(\*env\)->FindClass\(env,\s*"([^"]+)"\)', src))
    cls_list = re.search(r'jclass cls\[\]\s*=\s*\{([^}]*)\}', src)
    tabs_list = re.search(r'const JNINativeMethod\* tabs\[\]\s*=\s*\{([^}]*)\}', src)
    cls_names = [x.strip() for x in cls_list.group(1).split(',') if x.strip()]
    tab_names = [x.strip() for x in tabs_list.group(1).split(',') if x.strip()]
    tables = {}
    for m in re.finditer(r'static const JNINativeMethod (k\w+)\[\]\s*=\s*\{(.*?)\n\};',
                         src, re.S):
        tables[m.group(1)] = set(re.findall(
            r'\{\s*"([^"]+)"\s*,\s*"([^"]+)"', m.group(2)))
    out = {}
    for cv, tv in zip(cls_names, tab_names):
        out.setdefault('L' + var2cls[cv] + ';', set()).update(tables.get(tv, set()))
    return out


def stub(name, proto):
    if name.endswith('GetNativeFinalizer') or name.endswith('GetFinalizer'):
        return 'tvm_stub_finalizer'
    ret = proto.split(')', 1)[1]
    return {'V': 'tvm_stub_void', 'Z': 'tvm_stub_false', 'I': 'tvm_stub_int',
            'J': 'tvm_stub_one', 'F': 'tvm_stub_float', 'D': 'tvm_stub_double'
            }.get(ret, 'tvm_stub_null')


def main():
    bridge, gen, dex, out = sys.argv[1:5]
    want = sys.argv[5:]
    src = open(bridge).read() + open(gen).read()
    have = registered(src)
    natives = load_dex(dex)
    by = {}
    for cls, name, proto in natives:
        by.setdefault(cls, set()).add((name, proto))

    lines = ['/* 自动生成：tools/gen_missing.py（只补 ROM 有、桥缺的 native） */',
             '#ifndef TVM_GEN_MISSING_H', '#define TVM_GEN_MISSING_H', '']
    for c in want:
        jcls = 'L' + c + ';' if not c.startswith('L') else c
        miss = sorted(by.get(jcls, set()) - have.get(jcls, set()))
        if not miss:
            print(f'{c}: 无缺口', file=sys.stderr)
            continue
        san = 'kMiss_' + re.sub(r'\W', '_', c)
        lines.append(f'static const JNINativeMethod {san}[] = {{')
        for name, proto in miss:
            lines.append(f'    {{ "{name}", "{proto}", (void*){stub(name, proto)} }},')
        lines.append('};')
        lines.append('')
        print(f'{c}: +{len(miss)}', file=sys.stderr)
    lines.append('#endif')
    open(out, 'w').write('\n'.join(lines) + '\n')
    print('->', out, file=sys.stderr)


if __name__ == '__main__':
    main()
