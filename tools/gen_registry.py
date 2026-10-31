#!/usr/bin/env python3
"""从 ROM framework.dex 生成 JNI 注册表（族级桩）：descriptor 零猜测。

用法:
  gen_registry.py <framework.jar> <out.h> android/graphics/Canvas android/graphics/BaseCanvas ...
生成:
  static const JNINativeMethod kGen_<Class>[] = { { "name", "desc", (void*)stub }, ... };
桩函数按返回类型选择（在 tvmbridge.c 定义）:
  V→tvm_stub_void  Z→tvm_stub_false  I→tvm_stub_int  J→tvm_stub_one
  F→tvm_stub_float D→tvm_stub_double L/[→tvm_stub_null
  *GetNativeFinalizer / *GetFinalizer → tvm_stub_finalizer（真实 no-op 函数地址）
"""
import re
import sys

sys.path.insert(0, __file__.rsplit('/', 1)[0])
from dexnative import load as load_dex  # noqa: E402


def stub_for(name, proto):
    if name.endswith('GetNativeFinalizer') or name.endswith('GetFinalizer'):
        return 'tvm_stub_finalizer'
    ret = proto.split(')', 1)[1]
    if ret == 'V':
        return 'tvm_stub_void'
    if ret == 'Z':
        return 'tvm_stub_false'
    if ret == 'I':
        return 'tvm_stub_int'
    if ret == 'J':
        return 'tvm_stub_one'
    if ret == 'F':
        return 'tvm_stub_float'
    if ret == 'D':
        return 'tvm_stub_double'
    return 'tvm_stub_null'


def main():
    dex, out = sys.argv[1], sys.argv[2]
    want = sys.argv[3:]
    natives = load_dex(dex)
    by_cls = {}
    for cls, name, proto in natives:
        by_cls.setdefault(cls, []).append((name, proto))

    lines = ['/* 自动生成：tools/gen_registry.py（ROM framework.dex 实测描述符） */',
             '#ifndef TVM_GEN_REGISTRY_H', '#define TVM_GEN_REGISTRY_H', '']
    tables = []
    for c in want:
        jcls = 'L' + c + ';' if not c.startswith('L') else c
        entries = sorted(set(by_cls.get(jcls, [])))
        if not entries:
            print(f'!! {c} 无 native（检查类名）', file=sys.stderr)
            continue
        san = re.sub(r'\W', '_', c)
        lines.append(f'static const JNINativeMethod kGen_{san}[] = {{')
        for name, proto in entries:
            lines.append(f'    {{ "{name}", "{proto}", (void*){stub_for(name, proto)} }},')
        lines.append('};')
        lines.append('')
        tables.append((c, san, len(entries)))
        print(f'{c}: {len(entries)} 条', file=sys.stderr)

    lines.append('#endif')
    open(out, 'w').write('\n'.join(lines) + '\n')
    print(f'-> {out}', file=sys.stderr)
    for c, san, n in tables:
        print(f'{san}\t{c}\t{n}')


if __name__ == '__main__':
    main()
