# 🏁 里程碑：M2 APK 直装——验收通过（2026-10-23 22:47）

```
TwinVM 容器内运行真 APK 的完整输出：
  self-apk: /data/app/TwinVM-M2-DemoApp.apk
  calc: 3750075000
  DemoApp-Cli-all-ok
```

## 验收链（v1.2.117）

```
UI 列表选 APK（suggest 扫 dex 定入口）
  → setDex(/data/app/xxx.apk) + setMainClass(com.twinvm.demo.CliMain)
  → stage0 快照类名（[cls] 诊断行）
  → ROM VFS 提供 /data/app/TwinVM-M2-DemoApp.apk（zip 类路径）
  → map-fill 灌入 → linker64 → dalvikvm -cp *.apk CliMain
  → ART 完整启动 → APK 的 classes.dex 装载 → main(String[]) 执行
  → 三行输出 → 终点 all-ok
```

## M2 攻坚战（v1.2.107~117，十一版一日连环）

| 版 | 案 |
|----|-----|
| 107 | 容器C + setMainClass 通道 + SampleApp 合成注入 |
| 108 | DemoApp（可手动安装的真 APK，aapt+d8+apksigner 全链） |
| 109 | 运行期安装（installApk）——错位写入埋雷 |
| 110 | FGS 遗留账连坐闪退（stopWithTask + 清账兜底） |
| 111 | installApk 三错位引擎层重写 + ROM 双级自愈 + FGS 竞态防线 |
| 112 | 类名竞态（决策提前 stage0）+ tombstone 消音 + 日志自动存档 |
| 113 | 应用列表（listInstalledApks + dex 扫描建议） |
| 114 | 残留态防护 + DemoApp v2（CliMain 双入口） |
| 115 | 预装标注 + 内部类排除 |
| 116 | [cls] 诊断行黑洞 + ROM 预装 v2 |
| 117 | **exec_dyn gargv 快照——CNFE 三案真凶** |

## 两条铁律教训（已入教训库）

1. 写文件格式转换的 C 代码，先写主机侧全链 round-trip 测试再上真机
2. 同名数据可能有 N 条消费路径——grep 所有消费点再收工
   （pargv/gargv/cmdline 三处类名，112 只修两处，117 补齐）

## 下一站：M3 ActivityThread（跑标准 Activity APK——Via 就绪）
