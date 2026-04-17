# T23-C3 项目总指南

## 1. 项目目标

本项目的目标是把原来的供应商 Demo 改造成适合当前硬件架构的新系统：

- `T23` 负责相机采集、ISP、JPEG 生成
- `ESP32-C3` 负责 WiFi 联网、网页服务、控制桥接
- `UART` 负责控制命令
- `SPI` 负责图像数据

当前已经跑通的能力：

- T23 相机、ISP、JPEG 链路
- T23 <-> C3 SPI 最小传输
- T23 <-> C3 UART 控制
- C3 连接路由器 `MK`
- 浏览器通过 C3 网页调整 ISP 参数并查看预览

当前重点不再是继续追“整张预览图绝对无延迟”，而是为下一阶段做准备：

1. 校准电视边框
2. 提取边框 ROI
3. 输出 50 个色块颜色

## 2. 仓库结构

```text
T23-C3-Project/
├─ README.md
├─ docs/
│  └─ project_guide_zh.md
├─ scripts/
├─ configs/
├─ t23_rebuild/
├─ c3_rebuild/
├─ t23_c3_shared/
└─ third_party/
```

各目录职责：

| 路径 | 作用 |
|---|---|
| `t23_rebuild/` | T23 侧应用、启动脚本、打包脚本 |
| `c3_rebuild/` | C3 固件、WiFi bridge、网页服务 |
| `t23_c3_shared/` | T23 和 C3 共用的协议头、共享数据结构 |
| `configs/` | 不同硬件版本使用的内核串口配置快照 |
| `third_party/` | SDK 和供应商参考工程入口 |

## 3. 外部依赖放哪里

外部依赖不提交到 Git，统一放在：

- [third_party/README.md](/home/kuan/T23-C3-Project/third_party/README.md)

实际需要：

- `third_party/ingenic_t23_sdk`
- `third_party/vendor_reference`

这两个入口是项目默认依赖位置。现在它们通常是软链接，分别指向主目录里的官方 SDK 和供应商参考工程。

## 4. 当前硬件链路

### 4.1 控制链路

- `C3 UART TX(IO19)` -> `T23 UART0 RX(PB19 / ttyS0)`
- `C3 UART RX(IO18)` <- `T23 UART0 TX(PB22 / ttyS0)`

说明：

- 由于当前样机硬件方向固定，C3 软件里已经按现有连线交换过 `TX/RX` 定义。
- 调试日志口仍然是 T23 的 `ttyS1`。

### 4.2 图像链路

- `T23 spidev0.0` 作为 SPI Master
- `C3 SPI2` 作为 SPI Slave
- `IO3 / GPIO53` 作为 `Data Ready`

### 4.3 网络链路

- C3 同时提供：
  - 一个设备配网热点：`T23-C3-Setup`
  - 一个已保存路由器的 `STA` 连接
- 网页里现在可以：
  - 扫描附近 WiFi
  - 选择 `SSID`
  - 输入密码并保存到 `NVS`
  - 忘记当前 WiFi，回到仅设备热点模式
- 如果 `NVS` 里没有用户保存过的 WiFi，当前仍会以旧默认值作为出厂回退：
  - `SSID = MK`
  - `PASS = 12345678`

## 5. 当前运行模式

T23 的应用模式由：

- [app_mode.conf](/home/kuan/T23-C3-Project/t23_rebuild/init/app_mode.conf)

控制。

支持 3 种模式：

| 模式 | 作用 |
|---|---|
| `camera_diag` | 本地相机/ISP/JPEG 诊断 |
| `isp_uartd` | PC 串口网页调参 |
| `isp_bridge` | 通过 C3 的 WiFi 网页调参 |

当前用于正式联调的模式是：

- `APP_MODE=isp_bridge`

## 6. 接口总览

为了避免后续查代码时在 UART、SPI、HTTP 三条链路之间来回跳，这里先把接口角色说清楚：

| 接口 | 所在链路 | 当前用途 |
|---|---|---|
| `T23 ttyS0 <-> C3 UART1` | 控制 / 运行结果 | `DEBUG` 下传 ASCII 命令；`RUN` 下由 T23 连续推送二进制色块帧 |
| `T23 spidev0.0 <-> C3 SPI2 slave` | 图像数据 | 仅在 `DEBUG` 下传普通 JPEG 和校正 JPEG |
| `C3 HTTP` | 网页调试入口 | ISP 参数、校准、模式切换、预览 |
| `C3 GPIO7` | 灯带数据 | 通过 `RMT` 输出 `SM16703SP3` 单线时序 |
| `C3 GPIO1` | 灯带电源使能 | 拉高后灯带上电 |

### 6.1 T23 <-> C3 UART 协议

`DEBUG` 模式下，UART 走文本协议。当前常用命令：

| 命令 | 方向 | 作用 |
|---|---|---|
| `PING` | C3 -> T23 | 测试控制链是否在线 |
| `GET ALL` | C3 -> T23 | 读取全部 ISP 参数 |
| `SET <KEY> <VALUE>` | C3 -> T23 | 修改单个 ISP 参数 |
| `SNAP` | C3 -> T23 | 请求一张普通 JPEG，图像随后走 SPI |
| `CAL GET` | C3 -> T23 | 读取保存的 8 点校准数据 |
| `CAL SET ...` | C3 -> T23 | 写入 8 点校准数据 |
| `CAL SNAP` | C3 -> T23 | 请求一张 T23 内部校正后的 JPEG，图像随后走 SPI |
| `BLOCKS GET` | C3 -> T23 | 在 `DEBUG` 下请求一次边框色块结果 |
| `FRAME` | C3 -> T23 | 在 `DEBUG` 下请求“同一帧原图 + 色块” |
| `MODE GET / MODE SET ...` | 双向 | 查询或切换 `DEBUG / RUN` |
| `LAYOUT GET / LAYOUT SET ...` | 双向 | 查询或切换 `16X9 / 4X3` 布局 |

`RUN` 模式下，UART 不再走逐次请求文本协议，而是：

| 数据 | 方向 | 作用 |
|---|---|---|
| `t23_c3_run_blocks_frame_t` | T23 -> C3 | 高速二进制色块帧，供灯带实时刷新 |

### 6.2 T23 <-> C3 SPI 协议

SPI 现在只在 `DEBUG` 下使用，负责传图像数据：

| 帧类型 | 作用 |
|---|---|
| `RESP_JPEG_INFO` | 告知本次 JPEG 总长度 |
| `RESP_JPEG_DATA` | JPEG 数据分包 |

这也是当前 `RUN` 之所以更快的关键原因之一：`RUN` 已经不再依赖 SPI 传整图。

## 7. 当前图像规格

当前稳定预览尺寸已经恢复到：

- `640x320`

定义位置：

- [camera_common.h](/home/kuan/T23-C3-Project/t23_rebuild/app/camera/include/camera_common.h)

说明：

- 这不是传感器满分辨率
- 是当前调参和网页预览的稳定折中值
- 后面做边框提取时，实时链路可能不再传整张图，而是只传 ROI 或直接传 50 色块结果

## 8. 构建与烧录

### 8.1 T23

重新打包整包镜像：

```sh
cd /home/kuan/T23-C3-Project/t23_rebuild
./scripts/package_flash_image.sh
```

生成路径：

- [T23 整包镜像](/home/kuan/T23-C3-Project/t23_rebuild/_release/image_t23/T23N_gcc540_uclibc_16M_camera_diag.img)

### 8.2 C3

加载 ESP-IDF 环境：

```sh
cd /home/kuan/T23-C3-Project/c3_rebuild
source ./scripts/idf_env.sh
```

刷机并监视：

```sh
idf.py -p /dev/ttyUSB0 flash monitor
```

## 9. 成功启动时应该看到什么

### 9.1 T23

关键日志：

```text
sensor :sc2337p
@@@@ tx-isp-probe ok(...)
app mode   : isp_bridge
t23_isp_bridge start
serial device: /dev/ttyS0
```

### 9.2 C3

关键日志：

```text
T23 UART ready on UART1 tx=19 rx=18 baud=115200
SPI slave ready
WiFi got IP: 192.168.x.x
HTTP server started on port 80
```

## 10. 当前网页功能

当前网页由 C3 内嵌提供，访问：

```text
http://<C3-IP>/
```

当前支持的 ISP 参数：

| 参数 | 含义 |
|---|---|
| `BRIGHTNESS` | 亮度 |
| `CONTRAST` | 对比度 |
| `SHARPNESS` | 锐度 |
| `SATURATION` | 饱和度 |
| `AE_COMP` | 自动曝光补偿 |
| `DRC` | 动态范围压缩强度 |
| `AWB_CT` | 白平衡色温 |
| `HUE` | 色调 |

说明：

- 参数控制链路已经稳定可用
- 自动预览仍然存在一定卡顿
- 这是当前整图 JPEG 方案的自然限制
- `Save Startup Params` 会把当前 8 个 ISP 参数保存到 C3 的 `NVS`
  - 下次上电时，C3 会自动把这组已保存参数重新下发给 T23
  - 如果本次只是临时调参、没有点击保存，重启后仍回到 T23 默认参数
- `Restore T23 Defaults` 会把参数恢复到 T23 上电后的原始 ISP 默认值
  - 这组默认值由 C3 在首次成功连接 T23、且尚未覆盖启动参数时自动抓取并保存
- `Restore Saved Params` 会一键恢复你之前通过 `Save Startup Params` 保存过的那组参数
- `Capture Snapshot` 不是电脑截图，而是立即向 T23 请求一张新的预览帧
  - 它会同时刷新左侧原图预览和右侧边框色块预览
  - 如果已经开启 `Auto Preview`，手动点一次通常不会有很明显的额外变化
- `LED Install` 只影响物理灯带方向映射，不改变逻辑边框预览图
- `LED RGB Order` 用于适配不同灯带芯片或成品控制板的颜色通道顺序，当前支持：
  - `RGB`
  - `RBG`
  - `GRB`
  - `GBR`
  - `BRG`
  - `BGR`
- `LED Color Calibration` 用于补偿“网页颜色看起来对，但实际灯带偏色”的情况
  - 现在支持 6 个参数：
    - `R Gain / G Gain / B Gain`
    - `R Gamma / G Gamma / B Gamma`
  - `Gain` 主要解决三色亮度比例不一致，当前范围是 `0.00x ~ 1.00x`
    - `1.00x` 表示该颜色不衰减
    - `0.50x` 表示该颜色减半
    - `0.00x` 表示关闭该颜色通道
    - 当前版本不再允许 `>1.00x` 的增强，目的是让“颜色校正”语义更清晰，避免把校正和特效增强混在一起
  - `Gamma` 主要解决低亮度到中亮度区间的视觉线性不一致
    - `Gamma = 1.00` 表示线性
    - `Gamma > 1.00` 会压暗暗部，让低亮度更稳，但太大时会显得发闷
    - `Gamma < 1.00` 会抬亮暗部，让低亮度更容易显色，但太小时会发灰发雾
  - 调整滑块后会立刻作用到真实灯带
  - 点击 `Save LED Calibration` 后会把这组参数保存到 C3 的 `NVS`
  - 下次上电会自动恢复
  - 面板里的测试按钮用于直接检查灯带发色是否正确，当前支持：
    - `White`
    - `Red`
    - `Green`
    - `Blue`
    - `Cyan = Green + Blue`
    - `Magenta = Red + Blue`
    - `Yellow = Red + Green`
  - `Return To Live` 会退出测试色，回到实时边框色块输出
- `Enable Install Guide` 是一个持久化开关：
  - 打开时，允许修改安装方向，并让灯带点亮“一条短边蓝色 + 紧接着的一条长边红色”作为安装引导
  - 关闭时，安装方向被锁定并保存到 C3 的 `NVS`
  - 下次上电如果该开关保持关闭，系统会直接使用已保存的安装方向，不再进入安装引导步骤
- `DEBUG` 模式下灯带也会随当前边框色块结果更新，方便在调 ISP 和做校准时直接观察灯带响应
- 网页顶部现在采用“标题说明在上、控制面板在下”的两栏布局：
  - 左侧面板是 `WiFi`
  - 右侧面板是运行模式、边框布局、安装方向和调试按钮
  - 两个面板宽度对半，避免控件集中在右上角而发生重叠
- `Border Calibration` 当前已经支持：
  - 左侧显示真实校准抓拍图
  - 拖动 8 个点并保存到 T23
  - 点位顺序是 `TL / TM / TR / RM / BR / BM / BL / LM`
  - 页面默认点、水平锁和对称锁已经和 Python 调参工具对齐
  - 页面里已经移除 `Load Saved Points` 按钮；已保存校准会在页面初始化时自动读取
  - 右侧显示由 T23 内部完成 fixed fisheye + 8 点拉正后回传的真实校正预览图

## 11. 当前校正算法说明

当前版本已经不是旧的“10 点 Coons Patch 曲面插值”，而是：

- fixed fisheye + 8 点标定
- 在去畸变坐标系里拟合四条边
- 用单个 `homography` 拉成规则矩形
- 再在 rectified 图里选满足 `TM` 顶部约束的 `16:9 crop`

当前详细实现、关键函数职责、以及为什么它能和 Python 调参工具对齐，已经单独整理在：

- [校正算法详解](/home/kuan/T23-C3-Project/docs/rectification_algorithm_zh.md)

这一节只保留最关键的结论：

1. 页面交互点现在固定是 8 个，不再使用 `TML / TMR`
2. `TL / TR` 只是顶边的弱参考，真正主导顶边的是 `BL/LM` 与 `BR/RM` 外推出的预测顶角
3. `TM` 经过 homography 后得到 `tm_rect_y`，用于限制最终 crop 的顶部位置
4. rectified 预览和后续边框取色共用同一套 rectification model 和 crop
5. crop 搜索已经同时解决：
   - 顶部黑块
   - 下身被过度裁掉
   - 在 T23 上因候选框统计过重导致的 `HTTP 500`

### 11.7 当前边框色块划分规则

当前第一版边框划分已经支持两种布局：

- `16X9`
  - 上边：16 块
  - 右边：9 块
  - 下边：16 块
  - 左边：9 块
- `4X3`
  - 上边：4 块
  - 右边：3 块
  - 下边：4 块
  - 左边：3 块

总数分别是：

- `16 + 9 + 16 + 9 = 50`
- `4 + 3 + 4 + 3 = 14`

当前顺序按顺时针定义，两种布局都遵循同一规则：

1. `0 ~ 15`：上边，从左到右
2. `16 ~ 24`：右边，从上到下
3. `25 ~ 40`：下边，从右到左
4. `41 ~ 49`：左边，从下到上

网页右侧的 `Border Average Preview` 还补了一条显示规则：

- `16X9`：按四边全部块数完整显示
- `4X3`：角块在视觉上按“上下边共用”处理
  - 顶部看起来是 4 块
  - 底部看起来是 4 块
  - 左右边各只显示中间 1 块

这样做的目的不是改变底层块数据，而是让 `4X3` 的预览更符合“横向 4、纵向 3、角落共用”的直觉。

这样定义的目的，是让边框块在逻辑上围成一圈，后面更容易直接映射到灯带顺序。网页上的布局选择会通过 `C3 -> UART -> T23` 切换底层算法，而不是只改变前端显示。

### 11.8 当前平均颜色算法

当前颜色算法是最简单、最稳定的第一版：

1. 先得到 T23 内部的“16:9 校正图”
2. 取电视区域的外接矩形
3. 在这个矩形上定义一圈固定厚度的边框采样带
4. 再按当前布局切成一圈矩形块
5. 对每个块内所有像素分别计算 `R / G / B` 的平均值

当前使用的是普通均值：

- `R_avg = sum(R) / N`
- `G_avg = sum(G) / N`
- `B_avg = sum(B) / N`

优点：

- 简单
- 稳定
- 易于调试
- 和后面灯带输出直接对应

后面如果你觉得需要，可以继续升级为：

- 去高光/去异常值的裁剪均值
- 中值滤波
- 时间上的平滑滤波
## 12. 当前代码主流程

### 12.1 T23 启动脚本调用顺序

| 调用顺序 | 名称 | 功能 |
|---|---|---|
| 1 | `app_init.sh` | rootfs 用户态入口，打印 `app_init.sh start` 后进入 `start.sh` |
| 2 | `start.sh` | 加载 `sinfo.ko`、探测 sensor、读取 `start_param`、加载 `tx-isp` 和 sensor 驱动 |
| 3 | `app_main.sh` | 根据 `APP_MODE` 选择运行 `camera_diag / isp_uartd / isp_bridge` |
| 4 | `t23_isp_bridge` | 在 `isp_bridge` 模式下启动 T23 的控制桥和 JPEG 发送逻辑 |

### 12.2 T23 `isp_bridge` 关键函数顺序表

文件：

- [t23_rebuild/app/isp_bridge/src/main.c](/home/kuan/T23-C3-Project/t23_rebuild/app/isp_bridge/src/main.c)

| 调用顺序 | 函数名称 | 具体功能 |
|---|---|---|
| 1 | `main` | 程序入口，解析参数并决定进入 `DEBUG` 轮询还是 `RUN` 流模式 |
| 2 | `parse_args` | 解析 `--port` 和 `--baud` 参数 |
| 3 | `open_serial_port` | 打开控制串口 `/dev/ttyS0`，配置为非阻塞 |
| 4 | `startup_pipeline` | 初始化 T23 图像链：IMP 系统、视频源、JPEG 编码器 |
| 5 | `make_sensor_cfg` | 组装当前传感器配置 |
| 6 | `sample_system_init` | 初始化 IMP 系统 |
| 7 | `sample_framesource_init` | 初始化视频源通道 |
| 8 | `create_groups` | 创建编码 group |
| 9 | `sample_jpeg_init` | 初始化 JPEG 编码通道 |
| 10 | `bind_jpeg_channels` | 绑定 Framesource 与 Encoder |
| 11 | `sample_framesource_streamon` | 打开视频流 |
| 12 | `start_jpeg_recv_once` | 让编码器持续接收图片，后续随取随用 |
| 13 | `poll_serial_commands_nonblocking` | 非阻塞轮询 DEBUG 命令，避免卡住运行循环 |
| 14 | `process_command` | 分发 `PING / GET / SET / SNAP / CAL / MODE / LAYOUT / FRAME / BLOCKS GET` |
| 15 | `handle_snap` | 抓一张普通 JPEG，并通过 SPI 发给 C3 |
| 16 | `handle_cal_snap` | 生成真实校正预览 JPEG，并通过 SPI 发给 C3 |
| 17 | `handle_frame_with_blocks` | 在同一帧上同时产出原图 JPEG 和边框色块，供 DEBUG 同步显示 |
| 18 | `handle_blocks_get` | 在 DEBUG 下按请求返回一次文本色块结果 |
| 19 | `capture_jpeg_once` | 从编码队列里取最新 JPEG，并丢掉旧帧 |
| 20 | `compute_border_blocks_from_calibration` | 运行时色块核心算法：用反映射直接从原图取样平均，不再先生成整张校正图 |
| 21 | `rectify_jpeg_from_calibration` | 仅给 DEBUG 校准预览使用，生成真实的校正后 JPEG |
| 22 | `send_run_blocks_frame` | 把一帧色块结果打包成二进制高速帧 |
| 23 | `build_runtime_blocks_frame_from_current` | 生成当前 RUN 色块帧 |
| 24 | `run_mode_loop` | RUN 主循环，持续流式推送二进制色块帧 |
| 25 | `push_jpeg_over_spi` | 按共享协议分包发送 JPEG |
| 26 | `sendf / send_line` | 通过 UART 回文本响应 |
| 27 | `shutdown_pipeline` | 关闭视频流并释放 IMP 资源 |

### 12.3 C3 `bridge` 关键函数顺序表

文件：

- [c3_rebuild/main/main.c](/home/kuan/T23-C3-Project/c3_rebuild/main/main.c)

| 调用顺序 | 函数名称 | 具体功能 |
|---|---|---|
| 1 | `app_main` | C3 固件入口，初始化 NVS、WiFi、UART、SPI、HTTP、灯带 |
| 2 | `init_led_power_enable` | 拉高 `GPIO1`，给灯带上电 |
| 3 | `sm16703sp3_init` | 初始化 `GPIO7` 上的 RMT 灯带输出 |
| 4 | `run_led_self_test` | 上电自检灯带颜色顺序 |
| 5 | `init_wifi_sta` | 以 STA 模式连接路由器 |
| 6 | `wifi_event_handler` | 处理 WiFi 连接、断开、获取 IP |
| 7 | `init_t23_uart` | 初始化连接 T23 的 UART1 |
| 8 | `init_spi_slave` | 初始化 SPI Slave，仅供 DEBUG 图像链使用 |
| 9 | `start_http_server` | 启动网页和所有 API 路由 |
| 10 | `runtime_blocks_task` | RUN 后台任务，持续接收 T23 推送的二进制色块帧并刷新灯带 |
| 11 | `bridge_receive_runtime_blocks_frame_locked` | 解析一帧二进制 RUN 色块帧 |
| 12 | `store_latest_blocks` | 把最新色块统一保存到缓存，供网页和灯带复用 |
| 13 | `update_led_strip_from_cache` | 把逻辑色块扩展成 50 颗实体灯珠并发送到 SM16703SP3 |
| 14 | `root_handler / app_js_handler / styles_handler` | 返回网页 HTML/JS/CSS |
| 15 | `mode_get_handler / mode_set_handler` | 查询或切换 `DEBUG / RUN` |
| 16 | `layout_get_handler / layout_set_handler` | 查询或切换 `16X9 / 4X3` |
| 17 | `params_handler / set_handler / params_save_handler` | 读取、设置并保存 ISP 参数 |
| 18 | `load_saved_isp_params / save_saved_isp_params` | 从 NVS 读取或写入启动参数 |
| 19 | `boot_isp_sync_task` | 上电后把已保存 ISP 参数自动回放到 T23 |
| 20 | `snap_handler` | 走 DEBUG 链抓普通 JPEG |
| 21 | `calibration_get_handler / calibration_set_handler / calibration_rectified_handler` | 校准相关接口 |
| 22 | `border_blocks_handler` | 在 DEBUG 下返回一次色块 JSON |
| 23 | `runtime_blocks_handler` | 在 RUN 下返回轻量状态/缓存结果，网页不再依赖它驱动灯带 |
| 24 | `bridge_fetch_frame_locked` | DEBUG 同帧抓图和色块同步链路 |
| 25 | `bridge_fetch_snapshot_locked_ex` | 按命令类型请求 JPEG 并通过 SPI 收图 |
| 26 | `uart_send_line / uart_read_line` | DEBUG 文本协议的 UART 发送与接收 |

## 13. 关键函数索引

这一节不是按模块介绍，而是按“阅读和排错最常查的函数”整理，方便你直接搜索。

### 13.1 T23 `isp_bridge` 最常查函数

| 函数 | 功能 |
|---|---|
| `startup_pipeline` | 启动 T23 图像采集与 JPEG 编码链 |
| `capture_jpeg_once` | 取最新 JPEG，并尽量丢掉旧帧 |
| `compute_border_blocks_from_calibration` | RUN 模式下根据校准结果直接反映射取样并输出色块 |
| `rectify_jpeg_from_calibration` | DEBUG 校准预览用，生成真实校正图 |
| `handle_frame_with_blocks` | DEBUG 下一次性返回原图和色块，保证两边预览同帧 |
| `send_run_blocks_frame` | 发送 RUN 二进制色块帧 |
| `run_mode_loop` | RUN 持续流式主循环 |
| `process_command` | 所有 UART 文本命令的总入口 |

### 13.2 C3 `main` 最常查函数

| 函数 | 功能 |
|---|---|
| `runtime_blocks_task` | RUN 模式后台接收色块流并刷新灯带 |
| `bridge_receive_runtime_blocks_frame_locked` | 解析 T23 推送的二进制色块帧 |
| `store_latest_blocks` | 保存最新色块缓存，供网页/灯带复用 |
| `update_led_strip_from_cache` | 将逻辑布局扩展成 50 颗物理灯珠 |
| `apply_led_color_calibration` | 对最终输出到灯带的 RGB 再做 Gain/Gamma 校正 |
| `rebuild_led_color_lut` | 根据当前 Gain/Gamma 重建 8bit 查找表，避免热路径做浮点幂运算 |
| `refresh_led_output_from_state` | 统一处理实时色块、安装引导和测试色三种灯带输出状态 |
| `bridge_fetch_frame_locked` | DEBUG 下抓同一帧原图和色块 |
| `snap_handler` | 普通预览抓图接口 |
| `params_save_handler` | 把当前 8 个 ISP 参数保存为启动参数 |
| `boot_isp_sync_task` | 上电自动把已保存参数重新写入 T23 |
| `led_calibration_set_handler / led_calibration_save_handler / led_calibration_test_handler` | LED 颜色校正参数写入、保存和测试色切换 |
| `mode_set_handler` | `DEBUG / RUN` 切换入口 |
| `layout_set_handler` | `16X9 / 4X3` 布局切换入口 |

### 13.3 灯带驱动最常查函数

| 函数 | 功能 |
|---|---|
| `sm16703sp3_init` | 初始化 RMT 灯带驱动 |
| `sm16703sp3_show_rgb` | 发送一整帧 RGB 数据 |
| `sm16703sp3_fill_rgb` | 用于自检，整条灯带填充单色 |
| `sm16703sp3_clear` | 熄灭整条灯带 |

## 14. 当前最重要的共享头文件

### 14.1 SPI / 控制协议

- [t23_c3_protocol.h](/home/kuan/T23-C3-Project/t23_c3_shared/include/t23_c3_protocol.h)

作用：

- 定义 UART/SPI bridge 使用的参数 ID
- 定义 SPI 固定帧格式
- 定义 JPEG 分包头

### 14.2 边框提取数据结构

- [t23_border_pipeline.h](/home/kuan/T23-C3-Project/t23_c3_shared/include/t23_border_pipeline.h)

作用：

- 定义边框提取未来会用到的共享数据结构
- 包括：
  - 预览 / 校准 / 运行模式
  - 电视四角坐标
  - 边框几何信息
  - 50 色块结果

## 15. 之后的真正目标：边框提取

后续不建议继续把主要精力放在“整张 JPEG 看起来像视频”上，而是应该切到更接近产品形态的链路：

1. 保留当前整图预览，用于 ISP 调参
2. 增加一次性的校准抓拍
3. 网页上手动选择 8 个边框点
4. 保存 8 点标定结果
5. 在 T23 内部完成边框拉正 / 畸变补偿
6. 提取电视边框 ROI
7. 按固定顺序输出 50 个色块

建议的 50 色块分布：

- 上边：16
- 右边：9
- 下边：16
- 左边：9

总数正好 50。

## 16. 当前灯带控制链路

当前灯带芯片是 `SM16703SP3`，控制方式按 `WS2811` 兼容时序处理。

实现位置：

- [sm16703sp3.h](/home/kuan/T23-C3-Project/c3_rebuild/components/sm16703sp3/sm16703sp3.h)
- [sm16703sp3.c](/home/kuan/T23-C3-Project/c3_rebuild/components/sm16703sp3/sm16703sp3.c)
- [c3 bridge main.c](/home/kuan/T23-C3-Project/c3_rebuild/main/main.c)

当前设计：

- 灯带数据输出由 `C3` 完成
- 输出引脚是 `IO7`
- 使用 `RMT` 单线发送，不占用已经给 `T23` 图像链使用的 `SPI2_HOST`
- 实体灯珠数量按 `50` 颗处理

为什么不用参考工程里的 `ws2811` SPI 写法：

- 参考实现：
  - [ws2811.h](/home/kuan/programs/esp32-i2s-sr/components/ws2811/ws2811.h)
  - [ws2811.c](/home/kuan/programs/esp32-i2s-sr/components/ws2811/ws2811.c)
- 它本质上是用 `SPI master` 伪造灯带波形
- 但当前项目里 `SPI2_HOST` 已经被 `T23 <-> C3` 图像链占用
- 所以本项目改成 `RMT` 输出，更适合当前架构

当前灯带刷新流程：

1. `T23` 在 `RUN` 模式下持续计算边框平均色
2. `T23` 通过 `UART0 -> C3 UART1` 持续推送紧凑二进制色块帧
3. `C3` 后台任务 `runtime_blocks_task()` 连续接收色块帧并更新缓存
4. `C3` 把色块展开成 50 颗物理灯珠颜色
5. `C3` 在发送前先做一次 LED 色彩校正：
   - 按当前 `R/G/B Gain`
   - 再按当前 `R/G/B Gamma`
   - 通过查找表把逻辑颜色映射成更适合当前灯带硬件的实际输出值
6. `C3` 通过 `IO7` 把数据发给 `SM16703SP3`

### 16.1 为什么网页颜色准，灯带颜色却可能不准

网页上的 `Border Average Preview` 只是把 T23 算出来的 RGB 数值直接画到浏览器画布上，它更接近标准显示器的 `sRGB` 呈现。

但真实灯带会额外受到这些因素影响：

1. LED 三色本身光强不平衡
   - 例如很多灯珠天然就是绿更亮、蓝更暗
2. 三色波长和电脑屏幕原色不完全一致
3. 驱动电流、供电压降、PCB 走线都会影响实际发光
4. 人眼对亮度不是线性感知，尤其低亮度区域最容易“看起来不对”

所以“网页颜色对”只说明取色算法是对的，不代表灯带一定会主观上看起来一样。

### 16.2 LED Color Calibration 的用途

`LED Color Calibration` 不是去改 T23 的取色结果，而是在 C3 即将输出到灯带之前做最后一级补偿。

它的目标是：

- 不改变上游取色算法
- 不改变边框布局和安装方向映射
- 只修正“这条具体灯带硬件”的显示偏差

这样有两个好处：

1. 风险小
   - T23 的图像、校正、取色、布局逻辑都不用改
2. 可移植
   - 将来换一批灯珠、换一块灯板，只要重新调一次 LED 校正参数即可

### 16.3 Gain 和 Gamma 分别调什么

#### Gain

`Gain` 可以理解成每个颜色通道的“放大倍数”。

- `R Gain` 大一点：红色整体更强
- `G Gain` 小一点：绿色整体被压低
- `B Gain` 大一点：蓝色整体更强

它主要影响：

- 白平衡
- 三原色相对强弱
- 中高亮区域的颜色比例

如果灯带整体偏绿，通常先降 `G Gain`；
如果偏紫，通常是红蓝太强或者绿太弱；
如果偏黄，通常是蓝太弱。

#### Gamma

`Gamma` 不主要用来改“哪个颜色更强”，而是改“这个颜色在暗部和中间亮度时怎么爬升”。

可以把它理解成：  
同样一个输入值，经过 gamma 后，低亮度区域是被压住，还是被提前抬起来。

##### 当 gamma = 1.0

这是线性关系：

- 输入多少，基本按比例输出多少

##### 当 gamma > 1.0

结果是“暗部更暗，中高亮变化更明显”。

直观效果：

- 低亮度更不容易亮起来
- 黑位附近更稳
- 暗部杂色会减少
- 但如果太大，画面会显得偏闷，细小颜色变化不容易出来

适合的情况：

- 低亮度时灯带发灰
- 暗部带一点脏绿、脏红
- 轻微亮度输入就显得太亮

##### 当 gamma < 1.0

结果是“暗部被抬亮，低亮度更容易显色”。

直观效果：

- 微弱颜色更容易被看见
- 暗部更活跃
- 但如果太小，黑位容易发雾，颜色显得浮

适合的情况：

- 低亮度几乎不发光
- 颜色只有到很高亮度才看得出来
- 想增强暗场下的存在感

### 16.4 一个实用的调参顺序

建议不要一开始就同时乱动 6 个参数，最稳的顺序是：

1. 先点 `White Test`
2. 先调 `R/G/B Gain`
   - 先把白色调到尽量中性，不偏红、不偏绿、不偏蓝
3. 再切 `Red / Green / Blue Test`
   - 看纯色有没有明显偏脏、偏淡
4. 最后再调 `R/G/B Gamma`
   - 只修低亮度到中亮度的主观观感

经验上：

- `Gain` 先决定“颜色比例”
- `Gamma` 再决定“亮度曲线”

如果一开始就先大幅改 gamma，通常会把问题越调越乱。

`RUN` 模式和 `DEBUG` 模式的主要区别：

- `DEBUG`
  - 仍然是网页优先的调试链路
  - `UART` 走 ASCII 命令协议
  - 支持 ISP 调参、抓拍、校准、校正图预览
- `RUN`
  - 不再走 `BLOCKS GET` 这种逐次请求-响应文本协议
  - `T23` 进入持续输出模式
  - `C3` 只负责接收色块流、缓存并直接刷新灯带
  - 网页默认不再实时拉取预览，避免影响最低延迟
  - 这样可以明显减少文本解析、往返等待和网页轮询带来的延迟

当前 `RUN` 模式下的色块算法已经不再是“每帧先生成整张校正图，再从校正图切块”，而是：

1. `DEBUG` 模式完成一次 8 点校准
2. `RUN` 模式每帧直接抓当前图像
3. 对每个目标色块区域，在逻辑上的校正矩形坐标里取样
4. 通过当前 rectification model 的逆映射，把这些采样点映射回原始图像
5. 直接在原始图像上做双线性采样并求平均色

这样保留了校准结果，但去掉了每帧整图校正的额外开销，更适合低延迟灯带控制。

布局映射规则：

- `16x9` 布局
  - 与 50 颗实体灯珠一一对应
  - 上 `16`、右 `9`、下 `16`、左 `9`
- `4x3` 布局
  - 仍然输出到同一条 50 灯珠物理分布
  - 只是把 `4/3/4/3` 的粗块颜色按比例展开到 `16/9/16/9`
  - 例如：
    - 上边 `4` 块会扩展到 `16` 颗灯
    - 右边 `3` 块会扩展到 `9` 颗灯

这意味着：

- `DEBUG` 模式下你可以继续看网页预览
- `RUN` 模式下即使网页不开，灯带也会持续刷新

## 17. 当前最常用的命令

### 17.1 T23 打包

```sh
cd /home/kuan/T23-C3-Project/t23_rebuild
./scripts/package_flash_image.sh
```

### 17.2 C3 刷机

```sh
cd /home/kuan/T23-C3-Project/c3_rebuild
source ./scripts/idf_env.sh
idf.py -p /dev/ttyUSB0 flash monitor
```

### 17.3 切换 T23 串口内核配置

```sh
cd /home/kuan/T23-C3-Project
./scripts/apply_t23_kernel_serial_profile.sh t23_new_hw
```

或：

```sh
./scripts/apply_t23_kernel_serial_profile.sh t23_vendor_hw
```

## 18. 你现在最应该看哪些文件

如果只看 6 个文件，建议按这个顺序：

1. [README.md](/home/kuan/T23-C3-Project/README.md)
2. [project_guide_zh.md](/home/kuan/T23-C3-Project/docs/project_guide_zh.md)
3. [app_main.sh](/home/kuan/T23-C3-Project/t23_rebuild/init/app_main.sh)
4. [t23_isp_bridge main.c](/home/kuan/T23-C3-Project/t23_rebuild/app/isp_bridge/src/main.c)
5. [c3 bridge main.c](/home/kuan/T23-C3-Project/c3_rebuild/main/main.c)
6. [t23_c3_protocol.h](/home/kuan/T23-C3-Project/t23_c3_shared/include/t23_c3_protocol.h)
