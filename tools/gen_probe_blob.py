#!/usr/bin/env python3
"""把编译好的 probe_policy 二进制转成 C 数组头（供 libtwinvm.so 内嵌）。"""
import sys
src, dst = sys.argv[1], sys.argv[2]
d = open(src, 'rb').read()
out = []
out.append('/* 自动生成（tools/gen_probe_blob.py）：独立进程 seccomp 探针，%d 字节。' % len(d))
out.append(' * 无 libc、纯 svc；App 落盘到 /data/local/tmp 再 execv。 */')
body = ','.join(str(b) for b in d)
out.append('static const unsigned char kProbePolicyBlob[] = {%s};' % body)
out.append('static const unsigned int kProbePolicyBlobLen = %d;' % len(d))
open(dst, 'w').write('\n'.join(out) + '\n')
print('blob: %s -> %s (%d 字节)' % (src, dst, len(d)))
