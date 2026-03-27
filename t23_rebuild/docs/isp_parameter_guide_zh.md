# ISP 参数说明手册

## 目的

这份文档专门解释当前网页调参工具里已经接入的 ISP 参数，并回答两个常见问题：

1. 每个参数调大或调小，到底意味着什么
2. 君正公开 SDK 是否只支持这几个参数

当前网页和串口守护进程实际接入的参数定义，位于：

- [main.c](/home/kuan/T23-C3-Project/t23_rebuild/app/isp_uartd/src/main.c)

对应参数表是 `g_params[]`。

## 一、当前已经接入网页的 8 个参数

这 8 个参数不是“SDK 全部能力”，而是当前第一版网页工具先挑出来的常用项。选择原则是：

- 公开 SDK 已经有稳定的 `get/set` 接口
- 调整效果比较直观，适合滑块操作
- 对当前“电视画面采集/氛围灯”应用有第一批价值

### 1. BRIGHTNESS

- 含义：整体亮度偏移
- 当前接口：`IMP_ISP_Tuning_SetBrightness()` / `IMP_ISP_Tuning_GetBrightness()`
- 当前范围：`0 ~ 255`
- SDK 默认值：`128`
- 调大效果：
  - 整体画面更亮
  - 暗部更容易看见
  - 过大时会让黑位发灰、整体“漂白”
- 调小效果：
  - 整体画面更暗
  - 黑位更深
  - 过小时暗部细节容易丢失
- 对你的应用的意义：
  - 如果相机拍电视屏幕时整体偏暗，可以先小幅提高
  - 不建议拿它替代曝光调节，因为它更像“后处理偏移”

### 2. CONTRAST

- 含义：亮部和暗部之间的分离程度
- 当前接口：`IMP_ISP_Tuning_SetContrast()` / `IMP_ISP_Tuning_GetContrast()`
- 当前范围：`0 ~ 255`
- SDK 默认值：`128`
- 调大效果：
  - 明暗更分明
  - 画面更“硬”
  - 过大时高光容易死白、暗部容易死黑
- 调小效果：
  - 画面更平
  - 明暗过渡更柔和
  - 过小时画面显得发灰、没层次
- 对你的应用的意义：
  - 如果灯带颜色跟随主要依赖大块明暗变化，适度提高对比度有时会更稳
  - 但过高会放大电视画面中的高光/字幕边缘，反而不利于稳定取色

### 3. SHARPNESS

- 含义：边缘增强强度
- 当前接口：`IMP_ISP_Tuning_SetSharpness()` / `IMP_ISP_Tuning_GetSharpness()`
- 当前范围：`0 ~ 255`
- SDK 默认值：`128`
- 调大效果：
  - 轮廓更清楚
  - 字幕、边缘、物体线条更“利”
  - 过大时会出现边缘白边、噪点被增强
- 调小效果：
  - 画面更柔和
  - 噪点不那么扎眼
  - 过小时会显得发糊
- 对你的应用的意义：
  - 如果你后续主要做“区域平均取色”，锐度通常不是越高越好
  - 建议不要把锐度开太大，否则电视字幕、边框、噪点会对颜色统计造成干扰

### 4. SATURATION

- 含义：颜色浓度
- 当前接口：`IMP_ISP_Tuning_SetSaturation()` / `IMP_ISP_Tuning_GetSaturation()`
- 当前范围：`0 ~ 255`
- SDK 默认值：`128`
- 调大效果：
  - 颜色更鲜艳
  - 红、蓝、绿更“冲”
  - 过大时颜色失真，尤其霓虹灯/彩色 UI 容易过饱和
- 调小效果：
  - 颜色更淡
  - 极端情况下接近灰度
- 对你的应用的意义：
  - 对灯带项目来说，这个参数很关键，因为它直接影响最终取色“鲜不鲜”
  - 但过高会让颜色跟随看起来太夸张，不一定更真实

### 5. AE_COMP

- 含义：自动曝光补偿目标
- 当前接口：`IMP_ISP_Tuning_SetAeComp()` / `IMP_ISP_Tuning_GetAeComp()`
- 当前范围：当前工具限制为 `90 ~ 250`
- 调大效果：
  - AE 会倾向把画面拉亮
  - 曝光时间/增益可能上升
  - 过大时高光容易爆，拖影/噪声也可能上来
- 调小效果：
  - AE 会倾向把画面压暗
  - 高光更稳
  - 过小时电视暗场容易丢细节
- 对你的应用的意义：
  - 它比 `BRIGHTNESS` 更像“根因级”的亮度调节
  - 拍电视画面时，如果环境灯光变化不大，建议先优先调 AE_COMP，再考虑亮度偏移

### 6. DPC

- 含义：坏点校正强度，`DPC = Defect Pixel Correction`
- 当前接口：`IMP_ISP_Tuning_SetDPC_Strength()` / `IMP_ISP_Tuning_GetDPC_Strength()`
- 当前范围：`0 ~ 255`
- SDK 默认值：通常围绕 `128`
- 调大效果：
  - 对孤立亮点/暗点的修正更强
  - 某些高增益噪点会被压掉
  - 过大时细小纹理可能被误伤
- 调小效果：
  - 修正更弱
  - 画面保真一些
  - 坏点、闪点更容易留下来
- 对你的应用的意义：
  - 如果最终用途是区域取色，适度 DPC 往往有帮助
  - 但如果你想尽量保留屏幕细节，不建议一上来就拉太高

### 7. DRC

- 含义：动态范围压缩，`DRC = Dynamic Range Compression`
- 当前接口：`IMP_ISP_Tuning_SetDRC_Strength()` / `IMP_ISP_Tuning_GetDRC_Strength()`
- 当前范围：`0 ~ 255`
- SDK 默认值：通常围绕 `128`
- 调大效果：
  - 高光被压住，暗部被抬起来
  - 一张图里亮暗都更“看得见”
  - 过大时画面容易发灰、层次不自然
- 调小效果：
  - 更接近原始明暗关系
  - 但高反差场景下暗部/亮部可能更容易丢细节
- 对你的应用的意义：
  - 电视屏幕本身亮度跨度较大时，DRC 往往很有用
  - 但如果你的目标是“灯效跟随主观观感”，DRC 太强会改变原画明暗关系，导致灯带表现不够真实

### 8. AWB_CT

- 含义：AWB 色温目标，单位通常可按 Kelvin 理解
- 当前接口：`IMP_ISP_Tuning_SetAwbCt()` / `IMP_ISP_Tuning_GetAWBCt()`
- 当前范围：当前工具限制为 `1500 ~ 12000`
- 调大效果：
  - 画面趋向冷色
  - 白色可能更偏蓝
- 调小效果：
  - 画面趋向暖色
  - 白色可能更偏黄/偏红
- 对你的应用的意义：
  - 如果相机拍电视时受环境暖光影响，AWB 可能把屏幕颜色带偏
  - 这个参数对灯带项目很重要，因为白平衡一旦漂了，所有取色都会跟着漂

## 二、这 8 个参数够不够

对“先把串口网页调参跑通”的第一阶段来说，够。

但对你的项目最终目标来说，不够。公开 SDK 里还有不少有价值的接口，只是我们暂时还没接进网页。

## 三、君正公开 API 只支持这些参数吗

不是。

我这次重新核了公开头文件：

- [imp_isp.h](/home/kuan/ingenic_t23_sdk/Ingenic-SDK-T23-1.1.2-20240204-en/sdk/include/imp/imp_isp.h)

从这个头文件能确认，公开 SDK 至少还提供了下面这些方向的接口。

### 1. 曝光相关

- `IMP_ISP_Tuning_SetExpr()` / `GetExpr()`
  - 自动/手动曝光
  - 曝光时间
  - 曝光单位
- `IMP_ISP_Tuning_SetAeFreeze()`
  - 冻结自动曝光
- `IMP_ISP_Tuning_GetAeLuma()`
  - 读取当前亮度统计
- `IMP_ISP_Tuning_AE_SetROI()` / `GetROI()`
  - 设置 AE 权重区域

这组接口对你的应用非常有价值，因为“拍电视屏幕”时，最怕 AE 被周围环境误导。

### 2. 白平衡相关

- `IMP_ISP_Tuning_SetWB()` / `GetWB()`
  - 自动/手动白平衡
- `IMP_ISP_Tuning_SetWB_ALGO()`
  - 白平衡算法模式
- `IMP_ISP_Tuning_SetAwbWeight()` / `GetAwbWeight()`
  - AWB 区域权重
- `IMP_ISP_Tuning_SetAwbCtTrend()` / `GetAwbCtTrend()`
- `IMP_ISP_Tuning_Awb_SetRgbCoefft()` / `GetRgbCoefft()`

这组接口对“颜色还原”和“灯带取色稳定性”也很重要。

### 3. 去噪和细节相关

- `IMP_ISP_Tuning_SetSinterStrength()`
  - 一般可理解为 2D 降噪强度
- `IMP_ISP_Tuning_SetTemperStrength()`
  - 一般可理解为 3D/时域降噪强度
- `IMP_ISP_Tuning_SetDPC_Strength()` / `GetDPC_Strength()`
- `IMP_ISP_Tuning_SetSharpness()` / `GetSharpness()`

如果电视画面里有字幕、边缘、暗场噪点，这组参数会很重要。

### 4. 画面对比和层次相关

- `IMP_ISP_Tuning_SetGamma()` / `GetGamma()`
- `IMP_ISP_Tuning_EnableDRC()`
- `IMP_ISP_Tuning_SetDRC_Strength()` / `GetDRC_Strength()`
- `IMP_ISP_Tuning_EnableDefog()`
- `IMP_ISP_Tuning_SetDefog_Strength()` / `GetDefog_Strength()`
- `IMP_ISP_Tuning_SetHiLightDepress()` / `GetHiLightDepress()`
- `IMP_ISP_Tuning_SetBacklightComp()` / `GetBacklightComp()`

对“屏幕高亮区域压不住、暗部太黑”这类现象，这组参数很有用。

### 5. 画面几何和取景相关

- `IMP_ISP_Tuning_SetFrontCrop()` / `GetFrontCrop()`
  - 前端裁剪
- `IMP_ISP_Tuning_SetHVFLIP()` / `GetHVFlip()`
  - 水平/垂直翻转
- `IMP_ISP_Tuning_SetMask()` / `GetMask()`
- `IMP_ISP_Tuning_SetMaskBlock()` / `GetMaskBlock()`
  - 局部遮挡区域

对你的项目来说，`FrontCrop` 和 `Mask` 很值得后面接进网页。
因为电视边框、支架、桌面反光、摄像头近处遮挡，都可以通过裁剪或 mask 处理掉。

### 6. 传感器与场景配套

- `IMP_ISP_Tuning_SetSensorFPS()` / `GetSensorFPS()`
- `IMP_ISP_Tuning_SetAntiFlickerAttr()` / `GetAntiFlickerAttr()`

其中 `AntiFlicker` 对你的场景尤其有价值。如果显示器/灯光环境与相机曝光节奏打架，50/60Hz 防闪烁是非常值得优先加进网页的。

## 四、那畸变矫正能不能调

### 1. 结论先说

从目前检查到的公开 `imp_isp.h` 看，没有发现一个明确的、公开给应用层直接调用的“镜头畸变矫正 / 鱼眼矫正 / dewarp / LDC”接口。

我这次专门查过这些关键词：

- `distort`
- `dewarp`
- `fisheye`
- `lens`
- `LDC`

目前没有在公开头文件里看到对应的应用层 API。

### 2. 这意味着什么

这不代表君正整个平台绝对做不了畸变矫正，而是表示：

- 至少在你现在能直接调用的公开 `IMP_ISP_Tuning_*` 接口里，没有明显暴露这项能力
- 官方 `ImageTool` 里如果以后真有类似选项，可能走的是闭源驱动能力，或者私有控制接口

### 3. 对你当前项目怎么判断

如果你的镜头畸变确实明显，尤其是广角镜头拍电视屏幕四角弯曲，那么后续可能有 3 条路：

- 先通过物理安装减少畸变
  - 调镜头距离
  - 调相机角度
  - 尽量让电视位于画面中心
- 先用 `FrontCrop` / `Mask` / ROI 规避边缘失真区域
- 后续如果一定要做严格几何校正，再考虑软件侧重映射

对你现在“先把调参链路做通”的阶段，我建议先不要把畸变矫正当第一优先级。

## 五、针对你的应用，下一批最值得接入网页的参数

如果我们下一步继续扩展网页，我建议优先顺序大致这样排：

1. `AntiFlicker`
   - 先解决屏幕/灯光闪烁问题
2. `Expr` / `AeFreeze`
   - 先把曝光稳定住
3. `Gamma`
   - 方便调主观层次
4. `Sinter` / `Temper`
   - 控制噪点和拖影
5. `FrontCrop`
   - 裁掉电视外的无关区域
6. `Mask`
   - 遮掉固定干扰物
7. `WB` / `AWB Weight`
   - 让颜色更稳

如果只从“氛围灯取色稳定”角度出发，我会优先做：

- `AntiFlicker`
- `Expr`
- `AeFreeze`
- `FrontCrop`
- `Mask`

## 六、现在刷新率为什么慢

当前网页预览慢，主要不是网页本身，而是这条链路本来就是“抓拍 + 串口传 JPEG”。

实际链路是：

```text
浏览器发 SNAP
-> T23 抓一张 JPEG
-> T23 把 JPEG 二进制逐字节经 UART 发回
-> 浏览器拼接并显示
```

这里至少有 3 个耗时来源：

1. T23 抓图和 JPEG 编码
2. UART 传输整张 JPEG
3. 浏览器接收和刷新 DOM

## 七、波特率调高会不会改善

会，而且是最直接的改善手段之一。

原因很简单：当前预览图是整张 JPEG 走串口传输，波特率越高，传完一张图越快。

以你当前常见的 `25KB` 左右 JPEG 为例，粗略估算如下。

### 115200

- 理论线速：`115200 bit/s`
- UART 8N1 实际常按 `10 bit` 传 `1 byte` 估算
- 有效吞吐大约：`11.5 KB/s`
- `25 KB` JPEG 单纯传输就约：`2.1 ~ 2.3 s`

### 230400

- 有效吞吐大约：`23 KB/s`
- `25 KB` JPEG 单纯传输约：`1.0 ~ 1.2 s`

### 921600

- 有效吞吐大约：`92 KB/s`
- `25 KB` JPEG 单纯传输约：`0.25 ~ 0.35 s`

但这还没算编码和前后处理，所以真实刷新时间会比这个再长一点。

## 八、那我是不是应该立刻把波特率拉高

建议分两步，而不是一步冲到最高。

### 推荐顺序

1. 先把当前 `115200` 路径跑稳
2. 再试 `230400`
3. 如果稳定，再试 `921600`

### 原因

- `115200` 最稳，适合 bring-up
- `230400` 往往是“明显变快、又相对稳”的折中点
- `921600` 虽然更快，但要求 USB-UART 链路、线材、驱动和板端都更稳定

## 九、除了提波特率，还能怎么提速

可以，但优先级排在“先提高波特率”之后。

### 1. 降低 JPEG 分辨率

如果后续网页预览不追求高分辨率，就可以让抓拍用更小尺寸，图会更快回来。

### 2. 调高 JPEG 压缩率

图片更小，传输更快，但预览清晰度会下降。

### 3. 降低自动预览频率

比如不要每几百毫秒抓一张，而是 1 秒抓一张，体验反而更稳定。

### 4. 后续切到网络链路

最终如果还是想更接近官方 `ImageTool` 的体验，网络链路肯定会比 UART 更适合连续预览。

## 十、当前结论

### 关于参数

- 现在网页里的 8 个参数，已经能完成第一阶段的基础调参
- 但它们不是君正公开 SDK 的全部能力

### 关于更多 API

- 公开 SDK 里还有很多能用的接口
- 对你这个项目，下一批最值得做的是：
  - `AntiFlicker`
  - `Expr`
  - `AeFreeze`
  - `FrontCrop`
  - `Mask`

### 关于畸变矫正

- 目前没在公开 `imp_isp.h` 里看到明确的公开畸变矫正接口
- 现阶段更现实的办法是先用安装位置、裁剪、mask、ROI 来规避边缘畸变带来的影响

### 关于刷新率

- 提高波特率一定会改善当前串口 JPEG 预览速度
- 但它不是唯一瓶颈
- 当前最推荐的实际策略是：
  - 先从 `115200` 升到 `230400`
  - 稳定后再试 `921600`
