# T23-C3 项目总指南

## 1. 当前仓库是什么

这个仓库已经整理成当前产品方案对应的统一工程，不再沿用早期“临时 rebuild 工程”的目录命名。

现在顶层结构是：

```text
T23-C3-Project/
├─ README.md
├─ docs/
├─ scripts/
├─ configs/
├─ pc_tuner/
├─ t23_firmware/
├─ c3_firmware/
├─ shared/
└─ third_party/
```

各目录职责：

| 路径 | 作用 |
|---|---|
| `t23_firmware/` | T23 侧应用、启动脚本、打包镜像 |
| `c3_firmware/` | ESP32-C3 固件、网页服务、LED 输出 |
| `shared/` | T23 与 C3 共用协议、8 点校准数据结构 |
| `pc_tuner/` | Python 调参参考实现、桌面调试工具 |
| `docs/` | 当前工程说明文档 |
| `scripts/` | 仓库级辅助脚本 |
| `configs/` | 不同硬件版本使用的配置快照 |
| `third_party/` | 外部 SDK 与供应商参考工程入口 |

`shared/` 不是冗余目录，当前仍被 T23 和 C3 两边直接引用，所以保留并改成了更直白的名字。

## 2. 当前系统分工

### 2.1 T23

- 负责相机采集与 ISP
- 负责抓拍 JPEG
- 负责 8 点边框校准后的真实几何校正
- 在 `RUN` 模式下直接把当前画面压缩成 `16x16` rectified mosaic
- 通过 SPI 把图像数据或 mosaic 发送给 C3

主入口文件：

- [t23_firmware/init/app_main.sh](/home/kuan/T23-C3-Project/t23_firmware/init/app_main.sh)
- [t23_firmware/app/isp_bridge/src/main.c](/home/kuan/T23-C3-Project/t23_firmware/app/isp_bridge/src/main.c)

### 2.2 C3

- 负责 WiFi 与网页服务
- 负责把网页操作转成 T23 控制命令
- 负责接收 T23 返回的 JPEG 与 `16x16` mosaic
- 负责把 `16x16` mosaic 映射成灯带颜色
- 负责 LED 电源开关与运行时灯带刷新

主入口文件：

- [c3_firmware/main/main.c](/home/kuan/T23-C3-Project/c3_firmware/main/main.c)

### 2.3 Python / PC 调试工具

- 负责桌面侧标定与行为对照
- 当前与 T23 校正算法保持一致

参考入口：

- [pc_tuner/python_calibration/distortion calibration.py](/home/kuan/T23-C3-Project/pc_tuner/python_calibration/distortion%20calibration.py)
- [pc_tuner/python_rectification_reference.py](/home/kuan/T23-C3-Project/pc_tuner/python_rectification_reference.py)

## 3. 当前两种工作模式

### 3.1 DEBUG 模式

用途：

- ISP 调参
- 原图抓拍
- 边框校准
- rectified 预览
- 16x16 mosaic 预览

链路特点：

- UART 走文本命令
- SPI 按需返回 JPEG 或 `RESP_MOSAIC_RGB`
- 网页允许交互和预览

### 3.2 RUN 模式

用途：

- 低延迟灯带控制

链路特点：

- T23 不再等待网页逐次请求
- T23 每帧直接抓当前图像并生成 `16x16` rectified mosaic
- T23 通过 SPI 连续发送 `RESP_MOSAIC_RGB`
- C3 后台任务接收 mosaic 并立刻刷新灯带
- 网页只保留最小必要状态，不再参与实时链路

## 4. 当前真实数据流

### 4.1 DEBUG 预览链路

1. 浏览器访问 C3 网页
2. C3 通过 UART 向 T23 发送 `SNAP`、`CAL SNAP`、`CAL MOSAIC` 等命令
3. T23 抓图并在本地完成编码或校正
4. T23 通过 SPI 返回 JPEG 或 `16x16` mosaic
5. C3 再通过 HTTP 回给浏览器

### 4.2 RUN 灯带链路

1. 用户先在 `DEBUG` 完成 8 点边框校准
2. 切换到 `RUN`
3. T23 每帧直接从当前画面生成 `16x16` rectified mosaic
4. C3 后台任务持续接收最新 mosaic
5. C3 根据当前灯带型号做固定分区平均与映射
6. C3 通过 `SM16703SP3` 驱动输出到灯带

## 5. 当前协议与共享头

### 5.1 共享协议头

- [shared/include/t23_c3_protocol.h](/home/kuan/T23-C3-Project/shared/include/t23_c3_protocol.h)

当前最重要的内容：

- `T23_C3_FRAME_TYPE_RESP_JPEG_INFO`
- `T23_C3_FRAME_TYPE_RESP_JPEG_DATA`
- `T23_C3_FRAME_TYPE_RESP_MOSAIC_RGB`
- `T23_C3_PREVIEW_MOSAIC_WIDTH/HEIGHT = 16`

### 5.2 边框与校准共享头

- [shared/include/t23_border_pipeline.h](/home/kuan/T23-C3-Project/shared/include/t23_border_pipeline.h)

当前最重要的内容：

- 8 个校准点定义：`TL/TM/TR/RM/BR/BM/BL/LM`
- `t23_border_calibration_t`
- `t23_rgb8_t`

说明：

- `16x9 / 4x3` 的旧布局常量仍保留在头文件里，主要用于兼容旧调试路径
- 当前产品主链路已经固定为 `16x16` mosaic，再由 C3 做灯带映射

## 6. 关键构建命令

### 6.1 打包 T23 镜像

```sh
cd /home/kuan/T23-C3-Project/t23_firmware
./scripts/package_flash_image.sh
```

输出路径：

- [t23_firmware/_release/image_t23/T23N_gcc540_uclibc_16M_camera_diag.img](/home/kuan/T23-C3-Project/t23_firmware/_release/image_t23/T23N_gcc540_uclibc_16M_camera_diag.img)

### 6.2 构建 / 烧录 C3

```sh
cd /home/kuan/T23-C3-Project/c3_firmware
source ./scripts/idf_env.sh
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

### 6.3 仓库级自检

```sh
cd /home/kuan/T23-C3-Project
./scripts/check_workspace.sh
./scripts/bootstrap.sh --check
```

## 7. 当前主要产物

### 7.1 T23

- `t23_camera_diag`
- `t23_isp_uartd`
- `t23_isp_bridge`
- `t23_spi_diag`

位置：

- [t23_firmware/output](/home/kuan/T23-C3-Project/t23_firmware/output)

### 7.2 C3

- `c3_firmware.bin`

位置：

- [c3_firmware/build/c3_firmware.bin](/home/kuan/T23-C3-Project/c3_firmware/build/c3_firmware.bin)

## 8. 当前最重要的函数

### 8.1 T23 校正与运行时输出

| 函数 | 作用 |
|---|---|
| `build_rectification_model` | 根据 8 点与固定鱼眼参数生成当前 rectification model |
| `finalize_rectification_crop` | 在完整内容优先的前提下计算最终有效 crop |
| `capture_jpeg_once` | 抓取当前 JPEG，用于 DEBUG 原图与校正预览 |
| `handle_cal_snap` | 生成真实 rectified JPEG 并回传给 C3 |
| `handle_cal_mosaic` | 生成当前 `16x16` rectified mosaic 并回传给 C3 |
| `build_preview_mosaic_from_nv12_buffer` | 直接从 NV12 帧构建 `16x16` mosaic，减少运行时开销 |
| `push_runtime_mosaic_from_current` | RUN 模式下抓当前帧并推送 mosaic |
| `run_mode_loop` | RUN 主循环，持续向 C3 推送最新 mosaic |

### 8.2 C3 网页桥接与灯带控制

| 函数 | 作用 |
|---|---|
| `bridge_receive_runtime_mosaic_frame_locked` | 接收 T23 RUN 模式推送的一帧 `16x16` mosaic |
| `runtime_blocks_task` | RUN 后台任务，持续接收 mosaic 并刷新灯带 |
| `preview_mosaic_handler` | DEBUG 下返回当前 `16x16` preview mosaic 给网页 |
| `border_blocks_handler` | DEBUG 下返回当前边框平均结果给网页 |
| `update_led_strip_from_mosaic` | 把 `16x16` mosaic 映射成当前灯带输出 |
| `refresh_led_output_from_state` | 统一处理 live、测试色、安装引导三种输出状态 |
| `led_power_set_handler` | 控制灯带总电源开关 |
| `mode_set_handler` | 切换 `DEBUG / RUN` 并维护模式相关缓存 |

## 9. 校正算法文档入口

如果你只想看几何校正本身，直接看：

- [校正算法详解](/home/kuan/T23-C3-Project/docs/rectification_algorithm_zh.md)

如果你要核对 Python 与 T23 的一致性，直接看：

- [Python/T23 算法同步说明](/home/kuan/T23-C3-Project/docs/python_rectification_sync_zh.md)

## 10. 目录整理后的约定

为了保持仓库长期可读，当前约定如下：

- 顶层目录只保留“功能名”，不再保留 `rebuild` 这类历史临时命名
- 共享协议统一放在 `shared/`
- 生成物仍放在各自工程目录内部的 `build/`、`output/`、`_release/`
- 本地 IDE 配置如 `.vscode/` 不进入仓库

这样做的目的很简单：

- 新同事一眼能看懂目录
- 构建脚本路径稳定
- 文档里的入口路径和实际工程一致
