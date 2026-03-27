# T23 Rebuild 函数阅读手册

## 文档目的

这份文档专门给初学者使用，帮助你在阅读 `t23_rebuild` 代码时知道：

- 先看哪个文件
- 每个文件里有哪些关键函数
- 函数之间是什么调用关系
- 这些函数各自负责什么

它不是替代源码，而是帮助你“带着地图读源码”。

## 推荐阅读顺序

建议按下面顺序阅读：

1. `init/app_init.sh`
2. `init/start.sh`
3. `init/app_main.sh`
4. `app/camera/src/main.c`
5. `app/camera/src/camera_common.c`
6. `app/spi_diag/src/main.c`
7. `scripts/package_flash_image.sh`

原因是：

- 先理解启动链
- 再理解 camera 主程序
- 再理解底层媒体函数
- 最后再理解 SPI 和打包脚本

## 一、启动脚本函数/逻辑说明

## 1. `init/app_init.sh`

### 作用

- 作为 rebuild 应用的最外层入口

### 关键逻辑

- 打印 `app_init.sh start`
- 调用 `/system/init/start.sh`

### 阅读重点

这是“应用启动链有没有执行到”的标志点。

## 2. `init/start.sh`

### 作用

- 识别 sensor
- 加载 ISP 驱动
- 加载 sensor 驱动
- 启动 app_main.sh

### 脚本中的关键函数

#### `check_return()`

作用：

- 检查上一条命令是否成功
- 失败时立即退出

这是脚本里唯一真正意义上的 shell 函数。

### 阅读重点

按顺序理解下面几步：

1. `insmod sinfo.ko`
2. `echo 1 >/proc/jz/sinfo/info`
3. `cat /proc/jz/sinfo/info`
4. 读取 `start_param`
5. `insmod tx-isp-t23.ko`
6. `insmod sensor_${SENSOR}_t23.ko`
7. 调用 `app_main.sh`

## 3. `init/app_main.sh`

### 作用

- 决定 camera 诊断模式
- 设置输出目录
- 启动 `t23_camera_diag`

### 阅读重点

重点理解这几个变量：

- `CAMERA_MODE`
- `USE_DATA_PARTITION`
- `TARGET_DIR`

## 二、camera 主入口 `app/camera/src/main.c`

这个文件是当前最适合初学者先读懂的 C 文件，因为它短，而且把整个 camera bring-up 分成了清晰的几个阶段。

## 1. `usage(const char *prog)`

### 作用

- 打印命令行帮助

### 什么时候会执行

- 参数不合法时

## 2. `make_sensor_cfg(void)`

### 作用

- 根据 `camera_common.h` 中定义的 sensor 宏，构造一个运行时的 `sample_sensor_cfg_t`

### 你应该重点理解什么

- 为什么要把宏配置整理成结构体
- 这个结构体后面会传给 `sample_system_init()`

## 3. `create_groups(void)`

### 作用

- 为每个启用的 channel 创建 encoder group

### 为什么需要它

- JPEG encoder channel 初始化前必须先有 group

## 4. `destroy_groups(void)`

### 作用

- 销毁 `create_groups()` 创建出来的 group

## 5. `bind_jpeg_channels(void)`

### 作用

- 将 framesource channel 绑定到 JPEG encoder channel

### 对应清理函数

- `unbind_jpeg_channels(void)`

## 6. `parse_mode(int argc, char *argv[])`

### 作用

- 解析当前程序运行模式

### 支持的模式

- `isp-only`
- `framesource`
- `jpeg`

### 默认值

- `framesource`

## 7. `main(int argc, char *argv[])`

### 作用

- 整个 camera 诊断程序的总入口

### 调用顺序

```text
main
  -> parse_mode
  -> make_sensor_cfg
  -> sample_system_init
  -> [isp-only 结束]
  -> sample_framesource_init
  -> [framesource 模式]
       sample_framesource_streamon
       sleep
       sample_framesource_streamoff
       sample_framesource_exit
       sample_system_exit
  -> [jpeg 模式]
       create_groups
       sample_jpeg_init
       bind_jpeg_channels
       sample_framesource_streamon
       sample_get_jpeg_snap
       sample_framesource_streamoff
       unbind_jpeg_channels
       sample_encoder_exit
       destroy_groups
       sample_framesource_exit
       sample_system_exit
```

### 阅读建议

先把 `main()` 的整体流程读顺，再去看 `camera_common.c` 里的底层函数。

## 三、媒体公共实现 `app/camera/src/camera_common.c`

这个文件来源于 vendor sample，比较大。你不用一次性全部吃透，当前阶段先重点理解下面这些函数。

## 1. `sample_system_init(sample_sensor_cfg_t sensor_cfg)`

### 作用

- 初始化 sensor、ISP、IMP System

### 核心调用顺序

```text
sample_system_init
  -> IMP_ISP_Open
  -> IMP_ISP_AddSensor
  -> IMP_ISP_EnableSensor
  -> IMP_System_Init
  -> IMP_ISP_EnableTuning
```

### 失败意味着什么

如果这里失败，问题通常还在：

- sensor
- tx-isp
- libimp
- kernel/module/media stack 兼容性

## 2. `sample_system_exit()`

### 作用

- 按逆序关闭 sensor / ISP / IMP system

### 核心调用顺序

```text
sample_system_exit
  -> IMP_System_Exit
  -> IMP_ISP_DisableSensor
  -> IMP_ISP_DelSensor
  -> IMP_ISP_DisableTuning
  -> IMP_ISP_Close
```

## 3. `sample_framesource_init()`

### 作用

- 创建 framesource channel
- 设置 channel 属性

### 核心调用顺序

```text
sample_framesource_init
  -> IMP_FrameSource_CreateChn
  -> IMP_FrameSource_SetChnAttr
```

## 4. `sample_framesource_streamon()`

### 作用

- 打开已经创建好的 framesource channel

## 5. `sample_framesource_streamoff()`

### 作用

- 关闭 framesource channel

## 6. `sample_framesource_exit()`

### 作用

- 销毁 framesource channel

## 7. `sample_jpeg_init()`

### 作用

- 创建 JPEG encoder channel
- 将 channel 注册到 group

### 核心调用顺序

```text
sample_jpeg_init
  -> IMP_Encoder_CreateChn
  -> IMP_Encoder_RegisterChn
```

### 为什么它很关键

因为它是“camera 基础链路”和“VPU/JPEG 编码链路”的分界点。

如果：

- `framesource` 正常
- 但 `sample_jpeg_init()` 失败

那就优先怀疑 VPU / hwicodec / encoder 路径，而不是 sensor。

## 8. `sample_get_jpeg_snap()`

### 作用

- 取一批 JPEG 图片并保存到磁盘

### 核心调用顺序

```text
sample_get_jpeg_snap
  -> IMP_Encoder_StartRecvPic
  -> IMP_Encoder_PollingStream
  -> IMP_Encoder_GetStream
  -> save_stream
  -> IMP_Encoder_ReleaseStream
  -> IMP_Encoder_StopRecvPic
```

### 当前输出位置

- `/tmp/snap-*.jpg`

## 9. `sample_encoder_exit(void)`

### 作用

- 销毁 encoder channel
- 解除注册

## 10. `sample_get_frame()`

### 作用

- 为每个启用 channel 创建线程，抓取原始 frame

### 它依赖哪个线程函数

- `get_frame(void *args)`

## 11. `get_frame(void *args)`

### 作用

- 从某个 framesource channel 取 frame
- 选择其中一帧写入文件

### 适合什么时候看

当你已经理解 `framesource` 基本流程，想继续理解“怎么把 raw frame 拿出来”时再看。

## 12. `sample_get_video_stream()`

### 作用

- 为每个 encoder channel 启动线程去取编码后的 stream

### 它依赖哪个线程函数

- `get_video_stream(void *args)`

## 13. `get_video_stream(void *args)`

### 作用

- 从编码器取出 H264/H265/JPEG stream
- 保存到文件

## 14. `save_stream(...)`

### 作用

- 把一个 IMP stream 写入已打开文件

## 15. `save_stream_by_name(...)`

### 作用

- 根据前缀 + 序号生成文件名，并保存 stream

## 16. `sample_get_video_stream_byfd()`

### 作用

- 使用 `select()` 等待 encoder 文件描述符
- 与 `sample_get_video_stream()` 相比，这是另一种收流方式

## 17. `sample_osd_init()` / `sample_osd_exit()`

### 作用

- 初始化和释放 OSD 区域

### 当前是否属于主 bring-up 路径

- 不是

### 为什么还保留

- 后续如果你要加 overlay / 调试信息叠加，这段代码是现成参考

## 四、SPI 诊断程序 `app/spi_diag/src/main.c`

这个文件也很适合初学者读，因为它短，而且只做一件事：验证 T23 主机侧 SPI。

## 1. `usage(const char *prog)`

### 作用

- 打印 SPI 诊断程序的帮助信息

## 2. `ensure_gpio_exported(int gpio)`

### 作用

- 如果某个 GPIO 还没在 sysfs 中导出，就先导出

## 3. `write_text_file(const char *path, const char *value)`

### 作用

- 往 sysfs 文件写字符串

### 为什么单独抽出来

- 避免导出 GPIO、设置 direction 时重复代码

## 4. `setup_data_ready_input(void)`

### 作用

- 把 `Data Ready` 配置为输入

### 当前重要结论

新硬件里：

- `Data Ready = C3 -> T23`

所以这里一定是输入，不是输出。

## 5. `read_data_ready_value(int *value)`

### 作用

- 读取 `Data Ready` 当前电平

## 6. `monotonic_ms(void)`

### 作用

- 读取单调时钟毫秒数

### 用途

- 给 `wait_data_ready_high()` 做超时控制

## 7. `wait_data_ready_high(int timeout_ms)`

### 作用

- 在超时时间内轮询等待 `Data Ready` 变高

## 8. `spi_open_configure(int *fd_out)`

### 作用

- 打开 `/dev/spidev0.0`
- 设置 mode / bits / speed

### 当前固定参数

- `SPI_MODE_0`
- `8 bits`
- `10 MHz`

## 9. `spi_print_info(void)`

### 作用

- 打印 SPI 当前配置

## 10. `parse_bytes(...)`

### 作用

- 把命令行输入的十六进制字节串解析成二进制数组

## 11. `dump_hex(...)`

### 作用

- 以十六进制打印缓冲区内容

## 12. `spi_transfer_bytes(...)`

### 作用

- 发起一次全双工 SPI 传输

### 它最重要的意义

- 这是验证“主机侧 SPI 确实在工作”的核心函数

## 13. `main(...)`

### 作用

- 解析命令行，决定执行哪种 SPI 自检命令

### 支持命令

- `info`
- `read-dr`
- `wait-dr`
- `xfer`
- `xfer-dr`

## 五、打包脚本 `scripts/package_flash_image.sh`

## 1. 作用

- 生成 T23 可烧录镜像

## 2. 推荐阅读方式

按这 10 步理解：

1. 先构建 `t23_camera_diag`
2. 再构建 `t23_spi_diag`
3. 创建临时 `/system` 目录
4. 复制 vendor 底层镜像
5. 复制 rebuild 的 bin、脚本和模块
6. 创建 `.system`
7. 生成 `appfs.img`
8. 生成整片 flash 镜像
9. 导出 release 文件
10. 生成文本清单

## 六、初学者最容易卡住的地方

### 1. 为什么 `main.c` 很短，但 `camera_common.c` 很大？

因为：

- `main.c` 是“流程调度器”
- `camera_common.c` 是“底层媒体函数实现”

阅读时不要反过来。先读 `main.c`，再读 `camera_common.c`。

### 2. 为什么 shell 脚本这么重要？

因为 T23 的真正启动顺序不是“程序一上电自己就跑”，而是：

- rootfs 启动脚本
- `/system` 启动脚本
- camera 诊断程序

如果不理解脚本链，就很容易搞不清“为什么程序没跑起来”。

### 3. 为什么有的函数现在看起来没被主流程使用？

因为 `camera_common.c` 继承自 vendor sample，里面保留了一些后续可能还会用到的能力，比如：

- OSD
- H264/H265 stream 获取
- 多种收流方式

当前主流程只需要其中一部分。

## 七、建议你的下一步阅读顺序

如果你要慢慢消化代码，我建议你明天开始按这个顺序看：

1. `init/app_init.sh`
2. `init/start.sh`
3. `init/app_main.sh`
4. `app/camera/src/main.c`
5. `app/camera/src/camera_common.c`
   先只看：
   - `sample_system_init`
   - `sample_framesource_init`
   - `sample_jpeg_init`
   - `sample_get_jpeg_snap`
6. `app/spi_diag/src/main.c`
7. `scripts/package_flash_image.sh`

这样节奏最平稳，也最不容易被大文件吓住。
