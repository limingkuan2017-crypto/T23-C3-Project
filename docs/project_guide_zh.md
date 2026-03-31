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

- C3 以 `STA` 模式连接路由器
- 当前固定配置：
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

## 6. 当前图像规格

当前稳定预览尺寸已经恢复到：

- `640x320`

定义位置：

- [camera_common.h](/home/kuan/T23-C3-Project/t23_rebuild/app/camera/include/camera_common.h)

说明：

- 这不是传感器满分辨率
- 是当前调参和网页预览的稳定折中值
- 后面做边框提取时，实时链路可能不再传整张图，而是只传 ROI 或直接传 50 色块结果

## 7. 构建与烧录

### 7.1 T23

重新打包整包镜像：

```sh
cd /home/kuan/T23-C3-Project/t23_rebuild
./scripts/package_flash_image.sh
```

生成路径：

- [T23 整包镜像](/home/kuan/T23-C3-Project/t23_rebuild/_release/image_t23/T23N_gcc540_uclibc_16M_camera_diag.img)

### 7.2 C3

加载 ESP-IDF 环境：

```sh
cd /home/kuan/T23-C3-Project/c3_rebuild
source ./scripts/idf_env.sh
```

刷机并监视：

```sh
idf.py -p /dev/ttyUSB0 flash monitor
```

## 8. 成功启动时应该看到什么

### 8.1 T23

关键日志：

```text
sensor :sc2337p
@@@@ tx-isp-probe ok(...)
app mode   : isp_bridge
t23_isp_bridge start
serial device: /dev/ttyS0
```

### 8.2 C3

关键日志：

```text
T23 UART ready on UART1 tx=19 rx=18 baud=115200
SPI slave ready
WiFi got IP: 192.168.x.x
HTTP server started on port 80
```

## 9. 当前网页功能

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
| `DPC` | 坏点校正强度 |
| `DRC` | 动态范围压缩强度 |
| `AWB_CT` | 白平衡色温 |

说明：

- 参数控制链路已经稳定可用
- 预览仍然存在一定卡顿
- 这是当前整图 JPEG 方案的自然限制

## 10. 当前代码主流程

### 10.1 T23 启动脚本调用顺序

| 调用顺序 | 名称 | 功能 |
|---|---|---|
| 1 | `app_init.sh` | rootfs 用户态入口，打印 `app_init.sh start` 后进入 `start.sh` |
| 2 | `start.sh` | 加载 `sinfo.ko`、探测 sensor、读取 `start_param`、加载 `tx-isp` 和 sensor 驱动 |
| 3 | `app_main.sh` | 根据 `APP_MODE` 选择运行 `camera_diag / isp_uartd / isp_bridge` |
| 4 | `t23_isp_bridge` | 在 `isp_bridge` 模式下启动 T23 的控制桥和 JPEG 发送逻辑 |

### 10.2 T23 `isp_bridge` 关键函数顺序表

文件：

- [t23_rebuild/app/isp_bridge/src/main.c](/home/kuan/T23-C3-Project/t23_rebuild/app/isp_bridge/src/main.c)

| 调用顺序 | 函数名称 | 具体功能 |
|---|---|---|
| 1 | `main` | 程序入口，解析串口参数、注册信号、启动整条桥接流程 |
| 2 | `parse_args` | 解析 `--port` 和 `--baud` 参数 |
| 3 | `open_serial_port` | 打开 T23 侧控制串口，当前默认 `/dev/ttyS0` |
| 4 | `startup_pipeline` | 初始化 ISP、Framesource、JPEG Encoder，并让视频链跑起来 |
| 5 | `make_sensor_cfg` | 组装当前传感器配置结构体 |
| 6 | `sample_system_init` | 初始化 IMP 系统 |
| 7 | `sample_framesource_init` | 初始化视频源通道 |
| 8 | `create_groups` | 创建编码 group |
| 9 | `sample_jpeg_init` | 初始化 JPEG 编码通道 |
| 10 | `bind_jpeg_channels` | 绑定 Framesource 与 Encoder |
| 11 | `sample_framesource_streamon` | 打开视频流 |
| 12 | `start_jpeg_recv_once` | 启动 JPEG 接收，让编码器持续产出 JPEG |
| 13 | `read_line` | 从 UART 读一条来自 C3 的控制命令 |
| 14 | `handle_command_line` | 解析并分发 `PING / GET / SET / SNAP` |
| 15 | `handle_get_all` | 返回当前全部 ISP 参数 |
| 16 | `handle_get_param` | 返回单个 ISP 参数 |
| 17 | `handle_set_param` | 设置单个 ISP 参数 |
| 18 | `handle_snap` | 生成一张最新 JPEG，并准备通过 SPI 发送给 C3 |
| 19 | `capture_jpeg_once` | 从当前编码队列中取最新一帧 JPEG，丢弃旧帧 |
| 20 | `push_jpeg_over_spi` | 把一张 JPEG 按协议拆包通过 SPI 发送出去 |
| 21 | `ensure_transport_resources` | 打开并缓存 `spidev` 和 `Data Ready` 资源 |
| 22 | `push_one_spi_frame` | 发送一包 SPI 数据 |
| 23 | `sendf` / `send_line` | 通过 UART 把文本响应回给 C3 |
| 24 | `shutdown_pipeline` | 程序退出时关闭视频流、解绑并释放 IMP 资源 |

### 10.3 C3 `bridge` 关键函数顺序表

文件：

- [c3_rebuild/main/main.c](/home/kuan/T23-C3-Project/c3_rebuild/main/main.c)

| 调用顺序 | 函数名称 | 具体功能 |
|---|---|---|
| 1 | `app_main` | C3 固件入口，初始化 NVS、网络、UART、SPI、HTTP |
| 2 | `init_wifi_sta` | 以 STA 模式连接路由器 |
| 3 | `wifi_event_handler` | 处理 WiFi 连接、断开、获取 IP 等事件 |
| 4 | `init_t23_uart` | 初始化连接 T23 的 UART1 |
| 5 | `init_spi_slave` | 初始化 SPI Slave 和 `Data Ready` 引脚 |
| 6 | `start_http_server` | 启动 HTTP 服务器并注册网页和 API 路由 |
| 7 | `index_handler` | 返回网页 HTML |
| 8 | `styles_handler` | 返回网页 CSS |
| 9 | `app_js_handler` | 返回网页 JS |
| 10 | `ping_handler` | 调用 `/api/ping`，测试 T23 控制链 |
| 11 | `params_handler` | 调用 `/api/params`，读取全部 ISP 参数 |
| 12 | `set_handler` | 调用 `/api/set`，修改单个 ISP 参数 |
| 13 | `snap_handler` | 调用 `/api/snap`，抓一张最新 JPEG |
| 14 | `bridge_ping` | 通过 UART 向 T23 发 `PING` |
| 15 | `bridge_get_all` | 通过 UART 向 T23 发 `GET ALL` 并组装 JSON |
| 16 | `bridge_set_param` | 通过 UART 向 T23 发 `SET key value` |
| 17 | `bridge_fetch_snapshot_locked` | 通过 UART 发 `SNAP`，再通过 SPI 收完整 JPEG |
| 18 | `spi_receive_frame` | 接收单包 SPI 数据 |
| 19 | `uart_send_line` | 向 T23 发送一条 UART 文本命令 |
| 20 | `uart_read_line` | 读取一条来自 T23 的 UART 文本响应 |

## 11. 当前最重要的共享头文件

### 11.1 SPI / 控制协议

- [t23_c3_protocol.h](/home/kuan/T23-C3-Project/t23_c3_shared/include/t23_c3_protocol.h)

作用：

- 定义 UART/SPI bridge 使用的参数 ID
- 定义 SPI 固定帧格式
- 定义 JPEG 分包头

### 11.2 边框提取数据结构

- [t23_border_pipeline.h](/home/kuan/T23-C3-Project/t23_c3_shared/include/t23_border_pipeline.h)

作用：

- 定义边框提取未来会用到的共享数据结构
- 包括：
  - 预览 / 校准 / 运行模式
  - 电视四角坐标
  - 边框几何信息
  - 50 色块结果

## 12. 之后的真正目标：边框提取

后续不建议继续把主要精力放在“整张 JPEG 看起来像视频”上，而是应该切到更接近产品形态的链路：

1. 保留当前整图预览，用于 ISP 调参
2. 增加一次性的校准抓拍
3. 网页上手动选四个电视角点
4. 保存四点标定结果
5. 做透视矫正
6. 提取电视边框 ROI
7. 按固定顺序输出 50 个色块

建议的 50 色块分布：

- 上边：16
- 右边：9
- 下边：16
- 左边：9

总数正好 50。

## 13. 当前最常用的命令

### 13.1 T23 打包

```sh
cd /home/kuan/T23-C3-Project/t23_rebuild
./scripts/package_flash_image.sh
```

### 13.2 C3 刷机

```sh
cd /home/kuan/T23-C3-Project/c3_rebuild
source ./scripts/idf_env.sh
idf.py -p /dev/ttyUSB0 flash monitor
```

### 13.3 切换 T23 串口内核配置

```sh
cd /home/kuan/T23-C3-Project
./scripts/apply_t23_kernel_serial_profile.sh t23_new_hw
```

或：

```sh
./scripts/apply_t23_kernel_serial_profile.sh t23_vendor_hw
```

## 14. 你现在最应该看哪些文件

如果只看 6 个文件，建议按这个顺序：

1. [README.md](/home/kuan/T23-C3-Project/README.md)
2. [project_guide_zh.md](/home/kuan/T23-C3-Project/docs/project_guide_zh.md)
3. [app_main.sh](/home/kuan/T23-C3-Project/t23_rebuild/init/app_main.sh)
4. [t23_isp_bridge main.c](/home/kuan/T23-C3-Project/t23_rebuild/app/isp_bridge/src/main.c)
5. [c3 bridge main.c](/home/kuan/T23-C3-Project/c3_rebuild/main/main.c)
6. [t23_c3_protocol.h](/home/kuan/T23-C3-Project/t23_c3_shared/include/t23_c3_protocol.h)
