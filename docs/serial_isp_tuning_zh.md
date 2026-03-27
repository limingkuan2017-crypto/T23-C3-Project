# 串口 ISP 调参路线说明

## 一、为什么先走串口，不先走官方 ImageTool

官方 `ImageTool` 当前的问题不是“界面不好用”，而是它要求：

1. T23 侧存在兼容的 `ImageToolServer`
2. PC 能通过 `IP + port` 连到 T23

这意味着如果继续走官方路径，你当前至少还要先解决：

- C3 WiFi 协议栈
- T23/C3 网络转发
- T23 最终的 IP 地址获取
- 官方工具与当前镜像的兼容性

这条链太长，不适合当前阶段。

当前阶段更合理的目标是：

- 先让 T23 本地 ISP 参数可调
- 先能立即看到参数变化带来的效果
- 先把“调参闭环”做出来

所以我们先走：

```text
PC 浏览器界面
  -> 串口
  -> T23 串口调参守护进程
  -> IMP_ISP_Tuning_* API
  -> ISP 参数立即生效
```

## 二、PC 端要不要做 GUI

要，而且已经按这个方向搭好了第一版。

当前推荐是：

- 不做桌面原生 GUI
- 先做浏览器页面
- 浏览器通过 Web Serial 直接打开 Windows COM 口

这样做的优点：

- 开发快
- 不依赖 C3
- 不需要先写 Windows 原生程序
- 页面调试方便
- 后面如果要升级成更复杂工具，前端可以继续复用

## 三、实时预览难不难

### 1. 网络式实时预览

如果用 IP 网络传图，难度较低，但前提是网络链路先通。

你当前还没有这条链，所以暂时不适合。

### 2. 串口式实时预览

严格意义上的“高帧率实时预览”不适合串口，因为串口带宽有限。

但“低帧率连续快照预览”是可行的。

当前路线采用：

- T23 通过 `SNAP` 命令抓一帧 JPEG
- 串口把 JPEG 回传给浏览器
- 浏览器显示图像
- 前端可以按固定间隔轮询

在 `921600` 波特率下，这种预览不是视频流，但足够用来调 ISP。

它更像：

- 1~3 FPS 的连续调参预览

这对 ISP bring-up 已经很有价值。

## 四、COM8 和 COM3 怎么选

### 1. COM8

你说的 COM8 是烧录 T23 的 USB 线。

当前不推荐把它作为第一版调参通道，原因是：

- 它首先是烧录链路
- 运行态是否稳定暴露为可交互串口，不确定
- 很可能需要额外的 USB gadget / CDC ACM 配置
- 这条路会把“串口调参”问题变成“USB 设备功能配置”问题

所以第一版不建议走 COM8。

### 2. COM3

COM3 是当前更现实的选择，因为它已经是 UART log 口。

优点：

- 现成可见
- 调试方便
- 不用先解决 USB 设备侧协议

缺点：

- 它现在还承担 kernel log / login / shell 输出
- 如果同一条口同时跑日志和调参协议，容易互相干扰

### 3. 当前推荐

第一版先用 COM3，但注意：

1. 优先调高波特率到 `921600`
2. 调参期间尽量避免大量日志刷屏
3. 最好不要让登录 shell 和调参协议长期共用同一时段
4. 推荐先通过串口终端登录一次，然后执行：

```sh
/system/init/start_isp_uartd.sh /dev/ttyS1 921600
```

5. 执行后关闭串口终端，再让浏览器 Web Serial 接管对应的 Windows `COM3`

后面如果效果不错，再考虑：

- 给调参单独腾一条 UART
- 或者等 C3 网络通了，再升级成网络版

## 五、当前已经搭好的组成部分

### 1. T23 串口调参守护进程

目录：

- `t23_rebuild/app/isp_uartd`

当前角色：

- 自己初始化 ISP / framesource / JPEG
- 打开 UART
- 接收串口命令
- 调用 `IMP_ISP_Tuning_*` 接口改参数
- 支持抓拍 JPEG
- 配套启动脚本:
  - `t23_rebuild/init/start_isp_uartd.sh`

### 2. PC 浏览器页面

目录：

- `pc_tuner/web_serial_isp_tuner`

当前角色：

- 通过 Web Serial 打开 COM 口
- 发送 `GET / SET / SNAP`
- 调整滑块
- 显示 JPEG 快照

## 六、第一版支持哪些参数

优先选择了“官方 SDK 已经提供直接 get/set 接口”的参数：

- `BRIGHTNESS`
- `CONTRAST`
- `SHARPNESS`
- `SATURATION`
- `AE_COMP`
- `DPC`
- `DRC`
- `AWB_CT`

这样第一版最容易稳定。

## 七、当前协议是什么样

PC -> T23：

```text
PING
GET ALL
GET BRIGHTNESS
SET BRIGHTNESS 140
SET AE_COMP 180
SNAP
```

T23 -> PC：

```text
PONG
VAL BRIGHTNESS 128
OK SET BRIGHTNESS
ERR unknown-parameter
JPEG 25466
<raw jpeg bytes>
```

## 八、当前路线的现实判断

### 可做

- 串口调参
- 浏览器滑块调参
- 串口抓拍预览

### 暂时不建议

- 先复刻官方 ImageTool
- 先走 COM8 USB 交互
- 先做高帧率实时视频预览

## 九、下一阶段建议

当前建议按这个顺序推进：

1. 把 `t23_isp_uartd` 和 `start_isp_uartd.sh` 放到镜像里
2. 上板后先执行：

```sh
/system/init/start_isp_uartd.sh /dev/ttyS1 921600
```

3. 在 PC 上启动网页：

```sh
cd ~/T23-C3-Project/pc_tuner/web_serial_isp_tuner
python3 -m http.server 8080
```

4. 在 Windows 浏览器里打开 `http://localhost:8080`
5. 先验证 `Refresh Values`
6. 再验证单个 `SET`
7. 再验证 `Capture Snapshot`
8. 最后再开 `Auto Preview`
9. 稳定后再决定是否继续做：
   - 更复杂参数
   - 参数保存/加载
   - 最终网络版
