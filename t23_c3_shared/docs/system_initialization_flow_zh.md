# 系统初始化流程说明

## 文档目的

这份文档面向第一次接手项目的人，目标不是只讲“系统大概怎么启动”，而是把下面四类信息都讲清楚：

- 初始化的总体顺序
- 每一步对应到哪个文件、哪个函数
- 函数之间是谁调用谁
- 出问题时优先看哪一级

当前文档覆盖三个工程：

- `t23_rebuild`
- `c3_rebuild`
- `t23_c3_shared`

## 一、系统当前的整体初始化顺序

现阶段系统的推荐初始化顺序如下：

1. T23 上电，BootROM -> SPL -> U-Boot -> Linux
2. Linux 启动 BusyBox `init`
3. rootfs 中的 `rcS` 挂载 `/system`
4. `rcS` 调用 `/system/init/app_init.sh`
5. `app_init.sh` 调用 `/system/init/start.sh`
6. `start.sh` 识别 sensor，加载 `sinfo.ko`、`tx-isp-t23.ko`、sensor 驱动
7. `start.sh` 调用 `/system/init/app_main.sh`
8. `app_main.sh` 启动 `/system/bin/t23_camera_diag`
9. `t23_camera_diag` 依次验证：
   - `isp-only`
   - `framesource`
   - `jpeg`
10. T23 本地图像链路稳定后，再运行 `/system/bin/t23_spi_diag`
11. ESP32-C3 启动 `c3_rebuild` 固件，进入 SPI slave 等待状态
12. C3 准备好固定回包后拉高 `Data Ready`
13. T23 通过 `xfer-dr` 发起一次 SPI 传输
14. 确认 T23 收到固定回包后，再进入后续协议开发

一句话概括当前策略：

- 先把 T23 单机跑通
- 再把 SPI 电气层跑通
- 再做 T23<->C3 协议
- 最后再做 WiFi 和 ImageTool

## 二、工程职责划分

### 1. `t23_rebuild`

职责：

- T23 启动链
- sensor / ISP / framesource / JPEG
- SPI 主机诊断
- 打包 T23 flash 镜像

关键文件：

- `t23_rebuild/init/app_init.sh`
- `t23_rebuild/init/start.sh`
- `t23_rebuild/init/app_main.sh`
- `t23_rebuild/app/camera/src/main.c`
- `t23_rebuild/app/camera/src/camera_common.c`
- `t23_rebuild/app/spi_diag/src/main.c`
- `t23_rebuild/scripts/package_flash_image.sh`

### 2. `c3_rebuild`

职责：

- ESP32-C3 最小 SPI slave 固件
- `Data Ready` 输出
- 后续 WiFi / 协议 / 业务逻辑扩展

关键文件：

- `c3_rebuild/main/main.c`
- `c3_rebuild/scripts/idf_build.sh`
- `c3_rebuild/scripts/idf_flash.sh`
- `c3_rebuild/scripts/manual_flash.sh`

### 3. `t23_c3_shared`

职责：

- 引脚定义
- 固定回包协议
- 未来帧头和协议说明
- 项目文档

关键文件：

- `t23_c3_shared/include/t23_c3_protocol.h`
- `t23_c3_shared/docs/pinmap.md`
- `t23_c3_shared/docs/protocol.md`
- `t23_c3_shared/docs/test_vectors.md`

## 三、T23 初始化流程

## 3.1 Boot 阶段

### 顺序

1. BootROM
2. SPL
3. U-Boot
4. Linux kernel (`uImage`)

### 这一层证明什么

- Flash 分区布局正确
- 镜像可读
- 内核可解压、可启动

### 典型日志

```text
U-Boot SPL ...
U-Boot ...
## Booting kernel from Legacy Image ...
Starting kernel ...
```

### 如果这层失败

优先排查：

- 镜像打包错误
- Flash 烧写错误
- U-Boot / kernel 地址不匹配

这时不要先怀疑应用层代码。

## 3.2 Linux 内核阶段

### 内核当前与本项目相关的初始化点

- 串口控制台
- I2C
- SSI/SPI 控制器
- VPU
- MTD 分区
- rootfs 挂载

### 关键日志

```text
JZ SSI Controller for SPI channel 0 driver register
soc_vpu probe success
VFS: Mounted root (squashfs filesystem) readonly on device ...
```

### 如果这层失败

优先排查：

- kernel 配置
- pinmux / driver
- rootfs 镜像

## 3.3 BusyBox init -> rcS -> /system

### 真实调用链

```text
/etc/inittab
  -> /etc/init.d/rcS
     -> mount /system
     -> /system/init/app_init.sh
```

### 说明

T23 不是直接从 rootfs 启动你的 camera 程序，而是：

- rootfs 先起来
- rootfs 再挂载 `/system`
- `/system` 里的脚本再启动 rebuild 应用

这也是为什么 T23 的打包脚本要特别关注 `/system` 分区内容。

## 3.4 `app_init.sh`

文件：

- `t23_rebuild/init/app_init.sh`

### 当前内容和作用

逻辑极小，只做一件事：把启动权交给真正的 T23 初始化脚本。

### 调用顺序

```text
rcS
  -> app_init.sh
     -> start.sh
```

### 当前可见命令

```sh
echo "app_init.sh start"
/system/init/start.sh
```

### 功能说明

- 打出一个明确的“应用启动入口已执行”的日志
- 避免 rootfs 和应用启动链混在一起难以定位

### 关键日志

```text
app_init.sh start
```

### 如果这层失败

优先排查：

- `/system` 分区是否正确生成
- rootfs 的 `rcS` 是否真的执行了 `/system/init/app_init.sh`

## 3.5 `start.sh`

文件：

- `t23_rebuild/init/start.sh`

### 脚本级调用顺序

```text
app_init.sh
  -> start.sh
     -> insmod sinfo.ko
     -> echo 1 > /proc/jz/sinfo/info
     -> cat /proc/jz/sinfo/info
     -> 读取 /system/init/start_param
     -> insmod tx-isp-t23.ko
     -> insmod sensor_${SENSOR}_t23.ko
     -> /system/init/app_main.sh
```

### 当前脚本中的关键逻辑点

#### `check_return()`

作用：

- 统一检查上一条命令是否执行成功
- 一旦失败就打印错误并退出

意义：

- 保证启动链不会“假成功”

#### `insmod sinfo.ko`

作用：

- 加载 sensor 信息识别模块

#### `echo 1 > /proc/jz/sinfo/info`

作用：

- 触发 sensor 探测

#### `cat /proc/jz/sinfo/info`

作用：

- 读取当前识别到的 sensor 名字

#### 读取 `start_param`

作用：

- 根据 sensor 名称决定 ISP 参数和 sensor 参数

#### `insmod tx-isp-t23.ko`

作用：

- 加载 T23 ISP 主驱动模块

#### `insmod sensor_${SENSOR}_t23.ko`

作用：

- 加载与当前 sensor 名称匹配的 sensor 驱动模块

#### `/system/init/app_main.sh`

作用：

- 将控制权交给应用层入口

### 关键日志

```text
sensor :sc2337p
ISP_PARAM=isp_clk=125000000
SENSOR_PARAM=
@@@@ tx-isp-probe ok(...)
app_main start
```

### 如果这层失败

#### sensor 识别失败

优先排查：

- I2C 通路
- sensor 供电 / reset 时序
- sensor 驱动名是否与识别结果一致

#### `tx-isp-t23.ko` 加载失败

优先排查：

- module 与 kernel 是否匹配
- 参数是否错误

## 3.6 `app_main.sh`

文件：

- `t23_rebuild/init/app_main.sh`

### 脚本级调用顺序

```text
start.sh
  -> app_main.sh
     -> 解析 CAMERA_MODE
     -> 解析 USE_DATA_PARTITION
     -> 选择输出目录
     -> /system/bin/t23_camera_diag ${CAMERA_MODE}
```

### 当前关键变量

#### `CAMERA_MODE`

作用：

- 决定运行哪种 T23 诊断模式

当前支持：

- `isp-only`
- `framesource`
- `jpeg`

默认值：

- `framesource`

#### `USE_DATA_PARTITION`

作用：

- 决定是否尝试挂载 `/dev/mtdblock5`

当前默认：

- `0`

设计原因：

- 早期 bring-up 不应依赖 data 分区是否可写

#### `TARGET_DIR`

当前默认：

- `/tmp`

作用：

- 存放 JPEG 等测试输出

### 为什么当前默认不用 `/system/flash`

因为早期阶段最重要的是把 camera / jpeg / spi 跑通，而不是先处理 data 分区格式化问题。

### 关键日志

```text
app_main.sh start
camera mode: framesource
output dir : /tmp
```

## 3.7 `t23_camera_diag` 总调用链

文件：

- `t23_rebuild/app/camera/src/main.c`

### 主调用链

```text
main()
  -> parse_mode()
  -> make_sensor_cfg()
  -> sample_system_init()
  -> [if isp-only] sample_system_exit()
  -> sample_framesource_init()
  -> [if framesource]
       sample_framesource_streamon()
       sleep()
       sample_framesource_streamoff()
       sample_framesource_exit()
       sample_system_exit()
  -> [if jpeg]
       create_groups()
       sample_jpeg_init()
       bind_jpeg_channels()
       sample_framesource_streamon()
       sleep()
       sample_get_jpeg_snap()
       sample_framesource_streamoff()
       unbind_jpeg_channels()
       sample_encoder_exit()
       destroy_groups()
       sample_framesource_exit()
       sample_system_exit()
```

## 3.8 `t23_camera_diag` 函数说明

### `parse_mode(int argc, char *argv[])`

文件：

- `t23_rebuild/app/camera/src/main.c`

作用：

- 解析命令行参数，决定当前跑哪种模式

输入：

- `isp-only`
- `framesource`
- `jpeg`

默认：

- `framesource`

### `make_sensor_cfg(void)`

作用：

- 根据 `camera_common.h` 中当前启用的 sensor 宏，构造统一的 `sample_sensor_cfg_t`

意义：

- 保证三种模式使用同一套 sensor 配置

### `create_groups(void)`

作用：

- 为启用的 channel 创建 encoder group

为什么需要：

- JPEG encoder channel 创建前必须先有 group

### `bind_jpeg_channels(void)`

作用：

- 将 framesource 输出绑定到 JPEG encoder 输入

### `destroy_groups(void)` / `unbind_jpeg_channels(void)`

作用：

- 对应清理 group 和 bind 关系

### `main(int argc, char *argv[])`

作用：

- 输出诊断头信息
- 调用底层 `sample_*` 函数
- 在日志中明确告诉你卡在哪个阶段

## 3.9 `camera_common.c` 当前真正用到的函数

文件：

- `t23_rebuild/app/camera/src/camera_common.c`

这个文件很大，但当前项目真正关键的只有下面这些函数。

### `sample_system_init(sample_sensor_cfg_t sensor_cfg)`

当前调用顺序：

```text
sample_system_init()
  -> IMP_ISP_Open()
  -> IMP_ISP_AddSensor()
  -> IMP_ISP_EnableSensor()
  -> IMP_System_Init()
  -> IMP_ISP_EnableTuning()
  -> 若干 tuning 参数设置
```

作用：

- 建立最底层的 sensor + ISP + IMP system 运行环境

如果它失败，优先怀疑：

- sensor
- tx-isp
- libimp
- kernel/module/media stack 不匹配

### `sample_system_exit()`

作用：

- 按逆序关闭系统资源

调用顺序：

```text
sample_system_exit()
  -> IMP_System_Exit()
  -> IMP_ISP_DisableSensor()
  -> IMP_ISP_DelSensor()
  -> IMP_ISP_DisableTuning()
  -> IMP_ISP_Close()
```

### `sample_framesource_init()`

作用：

- 创建 framesource channel
- 设置每个 channel 的属性

调用顺序：

```text
sample_framesource_init()
  -> IMP_FrameSource_CreateChn()
  -> IMP_FrameSource_SetChnAttr()
```

### `sample_framesource_streamon()`

作用：

- 打开启用的 framesource channel

### `sample_framesource_streamoff()`

作用：

- 关闭启用的 framesource channel

### `sample_framesource_exit()`

作用：

- 销毁 framesource channel

### `sample_jpeg_init()`

作用：

- 创建 JPEG encoder channel
- 将 JPEG channel 注册到对应 group

调用顺序：

```text
sample_jpeg_init()
  -> IMP_Encoder_CreateChn()
  -> IMP_Encoder_RegisterChn()
```

如果这里失败，而 `framesource` 正常，优先怀疑：

- VPU
- hwicodec
- JPEG encoder 路径

### `sample_get_jpeg_snap()`

作用：

- 启动 JPEG 接收
- 轮询获取 JPEG stream
- 保存到 `/tmp/snap-*.jpg`

调用顺序：

```text
sample_get_jpeg_snap()
  -> IMP_Encoder_StartRecvPic()
  -> IMP_Encoder_PollingStream()
  -> IMP_Encoder_GetStream()
  -> save_stream()
  -> IMP_Encoder_ReleaseStream()
  -> IMP_Encoder_StopRecvPic()
```

### `sample_encoder_exit()`

作用：

- 销毁 encoder channel
- 解除注册

## 四、T23 SPI 初始化流程

## 4.1 `t23_spi_diag` 总调用链

文件：

- `t23_rebuild/app/spi_diag/src/main.c`

### 主调用链

```text
main()
  -> [info] spi_print_info()
           -> spi_open_configure()
  -> [read-dr] read_data_ready_value()
              -> setup_data_ready_input()
              -> ensure_gpio_exported()
              -> write_text_file()
  -> [wait-dr] wait_data_ready_high()
              -> read_data_ready_value()
  -> [xfer] spi_transfer_bytes()
           -> spi_open_configure()
           -> ioctl(SPI_IOC_MESSAGE)
  -> [xfer-dr] wait_data_ready_high()
              -> spi_transfer_bytes()
```

## 4.2 `t23_spi_diag` 关键函数说明

### `ensure_gpio_exported(int gpio)`

作用：

- 确保 GPIO 已经在 sysfs 中导出

### `write_text_file(const char *path, const char *value)`

作用：

- 向 sysfs 文件写值

### `setup_data_ready_input(void)`

作用：

- 将 GPIO53 配置为输入

注意：

- 新硬件中 `Data Ready` 是 `C3 -> T23`
- 所以 T23 只能读，不能驱动

### `read_data_ready_value(int *value)`

作用：

- 读取当前 `Data Ready` 电平

### `wait_data_ready_high(int timeout_ms)`

作用：

- 在一定时间内轮询等待 `Data Ready` 变高

### `spi_open_configure(int *fd_out)`

作用：

- 打开 `/dev/spidev0.0`
- 配置 mode、bits、speed

当前配置：

- mode = `SPI_MODE_0`
- bits = `8`
- speed = `10000000`

### `spi_print_info(void)`

作用：

- 读取并打印当前 SPI 配置

### `spi_transfer_bytes(const uint8_t *tx, size_t len)`

作用：

- 发起一次全双工 SPI 传输
- 同时打印 `tx` 和 `rx`

意义：

- 最适合用来验证主机侧 SPI 是否真的在工作

## 五、C3 初始化流程

## 5.1 `c3_rebuild` 固件总调用链

文件：

- `c3_rebuild/main/main.c`

### 主调用链

```text
app_main()
  -> gpio_config()
  -> gpio_set_level(PIN_NUM_DATA_READY, 0)
  -> gpio_set_pull_mode()
  -> spi_slave_initialize()
  -> spi_bus_dma_memory_alloc(tx_buf)
  -> spi_bus_dma_memory_alloc(rx_buf)
  -> while(1)
       -> memset(rx_buf)
       -> prepare_fixed_response(tx_buf, 32)
       -> memset(&t)
       -> spi_slave_transmit()
            -> post_setup_cb()
            -> post_trans_cb()
       -> ESP_LOGI(rx data)
```

## 5.2 C3 关键函数说明

### `post_setup_cb(spi_slave_transaction_t *trans)`

作用：

- 当 SPI slave 事务已经准备好、等待主机取走时，拉高 `Data Ready`

意义：

- 通知 T23：“我已经准备好回应了”

### `post_trans_cb(spi_slave_transaction_t *trans)`

作用：

- 当本次事务完成后，拉低 `Data Ready`

意义：

- 告诉 T23：“这次数据已经传完了”

### `prepare_fixed_response(uint8_t *tx_buf, size_t len)`

作用：

- 将当前测试阶段的固定回包写入 TX buffer

当前固定回包：

```text
5A A5 EE DD
```

### `app_main(void)`

作用：

- 初始化 GPIO
- 初始化 SPI slave
- 分配 DMA buffer
- 循环等待 T23 发起 SPI 事务

## 六、共享协议层初始化相关信息

文件：

- `t23_c3_shared/include/t23_c3_protocol.h`

### 当前真正生效的定义

#### `T23_C3_PROTO_VERSION`

作用：

- 共享协议版本号

#### `T23_C3_SPI_TEST_LEN`

作用：

- 当前固定回包测试长度

#### `T23_C3_SPI_TEST_RESP0~3`

作用：

- 当前固定回包的 4 个字节

### 当前保留但尚未正式启用的定义

#### `t23_c3_frame_type_t`

作用：

- 为后续 `PING / ACK / RGB_CHUNK / JPEG_CHUNK` 做预留

#### `t23_c3_frame_header_t`

作用：

- 为后续正式帧头做预留

当前状态：

- 已定义
- 还没有进入真正收发流程

## 七、当前推荐阅读顺序

建议第一次接手时按下面顺序看代码和文档：

1. `t23_c3_shared/docs/pinmap.md`
2. `t23_c3_shared/docs/system_initialization_flow_zh.md`
3. `t23_rebuild/init/app_init.sh`
4. `t23_rebuild/init/start.sh`
5. `t23_rebuild/init/app_main.sh`
6. `t23_rebuild/app/camera/src/main.c`
7. `t23_rebuild/app/camera/src/camera_common.c`
8. `t23_rebuild/app/spi_diag/src/main.c`
9. `c3_rebuild/main/main.c`
10. `t23_c3_shared/include/t23_c3_protocol.h`

## 八、当前最常见的故障定位顺序

### 情况 A：没有看到 `app_init.sh start`

先看：

1. `/system` 分区是否正确
2. rootfs 的 `rcS` 是否真正运行到 `/system/init/app_init.sh`

### 情况 B：sensor 识别失败

先看：

1. I2C
2. sensor 供电/reset
3. `sinfo.ko`
4. sensor 驱动名是否正确

### 情况 C：`sample_system_init()` 失败

先看：

1. tx-isp
2. sensor 驱动
3. libimp
4. kernel/module/media stack 是否匹配

### 情况 D：`framesource` 正常，`jpeg` 失败

先看：

1. `sample_jpeg_init()`
2. VPU
3. hwicodec
4. codec 相关兼容性

### 情况 E：`t23_spi_diag info` 失败

先看：

1. `/dev/spidev0.0`
2. pinmux
3. SPI 控制器驱动

### 情况 F：`xfer` 成功但 `rx` 全是 `ff`

通常表示：

- T23 主机侧已正常发出时钟和数据
- C3 还没有驱动 `MISO`
- 或 MISO 当前处于上拉 / 悬空状态

### 情况 G：C3 刷机时报 `No serial data received`

先看：

1. 串口是否真的是下载口
2. 是否进入下载模式
3. 是否需要手动 `BOOT + EN`

## 九、当前系统的代码级调用图

## 9.1 T23 启动调用图

```text
BusyBox init
  -> rcS
     -> /system/init/app_init.sh
        -> /system/init/start.sh
           -> /system/init/app_main.sh
              -> /system/bin/t23_camera_diag
```

## 9.2 T23 camera 诊断调用图

```text
main
  -> parse_mode
  -> make_sensor_cfg
  -> sample_system_init
  -> sample_framesource_init
  -> [jpeg path]
       create_groups
       sample_jpeg_init
       bind_jpeg_channels
       sample_get_jpeg_snap
       sample_encoder_exit
```

## 9.3 T23 SPI 诊断调用图

```text
main
  -> spi_print_info / read_data_ready_value / wait_data_ready_high / spi_transfer_bytes
```

## 9.4 C3 固件调用图

```text
app_main
  -> gpio_config
  -> spi_slave_initialize
  -> prepare_fixed_response
  -> spi_slave_transmit
     -> post_setup_cb
     -> post_trans_cb
```
