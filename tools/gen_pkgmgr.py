#!/usr/bin/env python3
"""生成 MiniPackageManager：PackageManager 的 94 个抽象方法最小实现。
默认语义：无系统特性、无包信息、查询返回空、权限 DENIED。
用途：guest 里 MiniContext.getPackageManager() 不再返回 null（Button.useWearMaterial3Style 案）。
"""
import re
import sys

src = sys.argv[1]
out = sys.argv[2]

lines = open(src).read().split('\n')
methods = []
for ln in lines:
    m = re.match(r'\s*public abstract (.+?) (\w+)\((.*)\)( throws ([\w.,$ ]+))?;$', ln)
    if m:
        methods.append((m.group(1), m.group(2), m.group(3), m.group(5)))
print('abstract methods:', len(methods), file=sys.stderr)

EMPTY_LISTS = {'getInstalledPackages', 'getInstalledApplications', 'queryIntentActivities',
               'queryIntentActivityOptions', 'queryBroadcastReceivers', 'queryInstrumentation',
               'queryContentProviders', 'getPreferredPackages', 'getAllPermissionGroups',
               'getPackagesHoldingPermissions', 'getSharedLibraries'}
THROW_NF = {'getActivityInfo', 'getActivityBanner', 'getActivityIcon', 'getActivityLogo',
            'getServiceInfo', 'getProviderInfo', 'getReceiverInfo', 'getInstrumentationInfo',
            'getPermissionInfo', 'getPermissionGroupInfo', 'getPackageUid', 'getPackageGids',
            'getResourcesForActivity', 'getResourcesForApplication'}
DRAWABLE_NULL = {'getDefaultActivityIcon', 'getDrawable', 'getUserBadgedDrawableForDensity',
                 'getUserBadgedIcon', 'getApplicationBanner', 'getApplicationIcon',
                 'getApplicationLogo'}


def default_expr(ret, name, params):
    if name in ('hasSystemFeature',):
        return 'false'
    if name == 'checkPermission':
        return 'android.content.pm.PackageManager.PERMISSION_DENIED'
    if name in ('getApplicationEnabledSetting', 'getComponentEnabledSetting'):
        return 'android.content.pm.PackageManager.COMPONENT_ENABLED_STATE_DEFAULT'
    if name in ('getPackageInfo',):
        return ('miniPackageInfo(a0.getPackageName())' if 'VersionedPackage' in params else 'miniPackageInfo(a0)')
    if name in ('getApplicationInfo',):
        return 'miniApplicationInfo(a0)'
    if name in EMPTY_LISTS:
        return 'new java.util.ArrayList<>()'
    if name == 'getSystemAvailableFeatures':
        return 'new android.content.pm.FeatureInfo[0]'
    if name == 'getSystemSharedLibraryNames':
        return 'new String[0]'
    if name in ('canonicalToCurrentPackageNames', 'currentToCanonicalPackageNames',
                'getPackagesForUid', 'getPackagesForUid'):
        return 'new String[0]'
    if name in ('getPackageGids',):
        return 'new int[0]'
    if name == 'getInstantAppCookie':
        return 'new byte[0]'
    if name == 'getPreferredActivities':
        return '0'
    if name == 'checkSignatures':
        return 'android.content.pm.PackageManager.SIGNATURE_NO_MATCH'
    if ret == 'void':
        return None
    if ret == 'boolean':
        return 'false'
    if ret == 'int':
        return '0'
    if ret == 'long':
        return '0L'
    if ret.endswith('[]'):
        return 'null'
    if ret.startswith('java.util.List<'):
        return 'new java.util.ArrayList<>()'
    return 'null'


def param_name(ptype, i):
    return f'a{i}'


def throws_clause(throws, name):
    if throws:
        return ' throws ' + throws.replace('$', '.')
    return ''


body = []
body.append('package com.twinvm.m3;')
body.append('')
body.append('/** 自动生成（tools/gen_pkgmgr.py）：PackageManager 最小壳。')
body.append(' *  guest 无真实 PackageManager：hasSystemFeature=false、查询为空、包信息最小化。 */')
body.append('public class MiniPackageManager extends android.content.pm.PackageManager {')
body.append('')

body.append('    static android.content.pm.ApplicationInfo miniApplicationInfo(String pkg) {')
body.append('        android.content.pm.ApplicationInfo ai = new android.content.pm.ApplicationInfo();')
body.append('        ai.packageName = pkg;')
body.append('        ai.uid = 2000;')
body.append('        ai.flags = 0;')
body.append('        ai.sourceDir = "/data/app/TwinVM-M2-DemoApp.apk";')
body.append('        ai.dataDir = "/data/data/" + pkg;')
body.append('        ai.targetSdkVersion = 36;')
body.append('        ai.minSdkVersion = 24;')
body.append('        return ai;')
body.append('    }')
body.append('')
body.append('    static android.content.pm.PackageInfo miniPackageInfo(String pkg) {')
body.append('        android.content.pm.PackageInfo pi = new android.content.pm.PackageInfo();')
body.append('        pi.packageName = pkg;')
body.append('        pi.versionName = "1.0";')
body.append('        pi.versionCode = 1;')
body.append('        pi.applicationInfo = miniApplicationInfo(pkg);')
body.append('        return pi;')
body.append('    }')
body.append('')

for ret, name, params, throws in methods:
    plist = []
    if params.strip():
        for i, p in enumerate([x.strip() for x in params.split(',')]):
            plist.append(f'{p} a{i}')
    sig = f'    @Override public {ret} {name}({", ".join(plist)})'
    sig_thr = throws_clause(throws, name)
    if name in THROW_NF:
        arg = 'a0' if plist else '""'
        if ret == 'int[]':
            expr = 'throw new android.content.pm.PackageManager.NameNotFoundException(String.valueOf(' + arg + '))'
        else:
            expr = 'throw new android.content.pm.PackageManager.NameNotFoundException(String.valueOf(' + arg + '))'
        body.append(sig + sig_thr + ' {')
        body.append('        ' + expr + ';')
        body.append('    }')
        continue
    expr = default_expr(ret, name, params)
    body.append(sig + sig_thr + ' {')
    if expr is None:
        body.append('    }')
    else:
        body.append(f'        return {expr};')
        body.append('    }')
body.append('}')

open(out, 'w').write('\n'.join(body) + '\n')
print('wrote', out, file=sys.stderr)
