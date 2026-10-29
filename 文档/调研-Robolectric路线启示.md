# 网络调研：无设备跑 Android 代码的成熟方案（13:00 TA 铁律执行）

## TA 指令
"不是只会翻源码，而是要在互联网上寻找解决问题的方法"——搜的是【方案】，不只源码。

## 找到的成熟方案：Robolectric（900+ star 级框架，同生态位）

### Robolectric 怎么解决我们正在解决的每一个问题

| 我们的问题（TwinVM） | Robolectric 的解法 | 对我们的启示 |
|---|---|---|
| framework native 缺（bridge v1-v10 手补） | **插桩时把所有 native 转成 no-op**（可 shadow 的非 native 方法） | 一次性根治，不用逐类补 |
| 静态初始化炸（烙印） | **构造器和 <clinit> 也可被 shadow 拦截**（ClassInstrumentor） | core-patch 的类替换是原始版；插桩是工业版 |
| Resources/A-Manager 链炸 | **ShadowAssetManager/ShadowResources**（900+ 现成 shadow） | 站在巨人肩上：抄它的 Shadow 实现清单 |
| framework-res.apk 不存在 | 资源从 android-resources jar（Maven 可下载）来 | ROM 可考虑带一个真的 framework-res |
| final 字段/类限制 | **strip final**（插桩） | 我们的 Unsafe 是绕，它是删 |
| 类行为不可控 | SandboxClassLoader + invokedynamic delegator（首调时动态路由真码/shadow） | JIT 幽灵的另一解法：路由是确定性的 |

### Robolectric 测试生命周期的初始化顺序（抄作业清单）
```
AndroidTestEnvironment.setUpApplication：
  Looper 模式 → 系统和应用资源初始化 → Application 上下文/系统上下文
  → 显示配置 → 【ActivityThread 设置】→ app 目录
——它把 Resources/ActivityThread 的初始化做成【受控序列】——
  我们 manualAttach 正是同一件事（方向被验证）
```

## 可以直接借力的资产
1. **android-all jar**（Maven Central，每版 Android）——真 framework 实现（我们 ROM 的 framework.jar 同源，但它的版本插桩过）
2. **Shadow 清单**（shadows/framework）——【哪些方法必须 shadow 的完整清单】——我们 bridge/core-patch 的补面对照表
3. **Resources 初始化序列**——AndroidTestEnvironment 源码即教科书

## 决策建议
- 短期：继续 v72 手工路线（M3-c 冲 5/5）
- 中期（M4）：把 Robolectric 的【Shadow 清单】过一遍——bridge 补面查漏
- 若卡 Resources 死路：考虑 ROM 加 framework-res.apk（从 TA 真机提取，Robolectric 证明这条路可行）
