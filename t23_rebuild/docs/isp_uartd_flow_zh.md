# `t23_isp_uartd` 代码流程说明

## 一、这个程序解决什么问题

`t23_isp_uartd` 的目标是：在不依赖 C3 网络、不依赖官方 `ImageTool` 的前提下，先把 T23 的 ISP 参数调节链路打通。

它的基本工作模式是：

```text
浏览器页面
  -> Web Serial
  -> UART(COM3/COM8 对应的 ttyS1)
  -> t23_isp_uartd
  -> IMP_ISP_Tuning_* API
  -> 参数立即生效
  -> 再通过 SNAP 抓回 JPEG 预览
```

## 二、主流程从哪里开始

入口函数：

- `main()` in [main.c](/home/kuan/T23-C3-Project/t23_rebuild/app/isp_uartd/src/main.c)

主流程顺序：

1. `parse_args()`
   解析命令行里的串口设备和波特率
2. `signal(SIGINT, ...)` / `signal(SIGTERM, ...)`
   注册退出信号，方便程序被正常结束
3. `startup_pipeline()`
   初始化持续运行的 camera/ISP/JPEG 通路
4. `open_serial_port()`
   打开串口并设置成 raw 模式
5. `send_line(serial_fd, "READY T23_ISP_UARTD 1")`
   通知 PC 端“守护进程已经上线”
6. `while (g_running) { read_line(); process_command(); }`
   进入命令循环
7. `shutdown_pipeline()`
   程序退出时按反向顺序释放资源

## 三、初始化 pipeline 的函数调用顺序

函数：

- `startup_pipeline()`

内部顺序：

1. `make_sensor_cfg()`
   生成 `sample_sensor_cfg_t`
2. `sample_system_init(sensor_cfg)`
   初始化 IMP system 和 sensor 相关基础环境
3. `sample_framesource_init()`
   初始化 framesource
4. `create_groups()`
   创建 encoder group
5. `sample_jpeg_init()`
   初始化 JPEG 编码通道
6. `bind_jpeg_channels()`
   把 framesource 和 JPEG encoder 绑定起来
7. `sample_framesource_streamon()`
   正式开流

这样做完之后，程序就具备两种能力：

- 可以直接读写 ISP 参数
- 可以随时调用 `capture_jpeg_once()` 抓一张新的 JPEG

## 四、协议命令是怎么处理的

函数：

- `read_line()`
- `process_command()`

### 1. `read_line()`

作用：

- 从串口按“行”读取一条命令
- 自动忽略 `\r`
- 遇到 `\n` 认为一条命令结束

为什么这样设计：

- 文本协议最适合早期 bring-up
- 便于在浏览器日志里直接看到人类可读的命令和响应

### 2. `process_command()`

它负责解析并分发这些命令：

- `PING`
- `HELP`
- `GET ALL`
- `GET <PARAM>`
- `SET <PARAM> <VALUE>`
- `SNAP`

处理顺序大致是：

1. `strtok()` 把命令拆成 token
2. `strtoupper()` 统一转大写
3. 根据命令类型分支：
   - `PING` -> 回 `PONG`
   - `GET ALL` -> `send_all_values()`
   - `GET X` -> 查表后调用对应 `get_value()`
   - `SET X V` -> 查表后调用 `set_value()`，然后回读一次
   - `SNAP` -> `handle_snap()`

## 五、参数查表机制

核心数据结构：

- `isp_param_desc_t`
- `g_params[]`

`g_params[]` 把三类东西绑在一起：

1. 协议名
   例如 `BRIGHTNESS`
2. 读函数
   例如 `get_brightness_value()`
3. 写函数
   例如 `set_brightness_value()`

所以当网页发：

```text
SET BRIGHTNESS 140
```

程序会：

1. `find_param("BRIGHTNESS")`
2. 找到对应描述项
3. 执行 `set_brightness_value(140)`
4. 再执行 `get_brightness_value(&value)`
5. 回：

```text
VAL BRIGHTNESS 140
OK SET BRIGHTNESS
```

## 六、JPEG 抓拍是怎么走的

相关函数：

- `handle_snap()`
- `capture_jpeg_once()`

### 1. `handle_snap()`

作用：

- 是协议层对 `SNAP` 命令的处理入口

行为：

1. 调用 `capture_jpeg_once()`
2. 成功后先发文本头：

```text
JPEG <length>
```

3. 然后紧跟原始 JPEG 二进制数据

### 2. `capture_jpeg_once()`

作用：

- 从已打开的 JPEG 编码通路里抓一帧图像

调用顺序：

1. 找到启用的 channel
2. `IMP_Encoder_StartRecvPic()`
3. `IMP_Encoder_PollingStream()`
4. `IMP_Encoder_GetStream()`
5. 把 `stream.pack[]` 里的多个 pack 拼成连续 JPEG
6. `IMP_Encoder_ReleaseStream()`
7. `IMP_Encoder_StopRecvPic()`

为什么要拼 `pack`：

- 底层一帧 JPEG 不一定就是一块连续内存
- 所以必须把多个 pack 拷贝到 `g_jpeg_buf` 里，前端才能按长度一次性接收

## 七、程序启动成功的关键标志

最关键的一条串口输出是：

```text
READY T23_ISP_UARTD 1
```

来源：

- `main()` 中的 `send_line(serial_fd, "READY T23_ISP_UARTD 1")`

这条日志意味着：

1. `startup_pipeline()` 已经成功
2. `open_serial_port()` 已经成功
3. 网页端现在可以开始发 `PING / GET / SET / SNAP`

## 八、程序退出时怎么收尾

函数：

- `shutdown_pipeline()`

顺序与初始化相反：

1. `sample_framesource_streamoff()`
2. `unbind_jpeg_channels()`
3. `sample_encoder_exit()`
4. `destroy_groups()`
5. `sample_framesource_exit()`
6. `sample_system_exit()`

这样可以避免：

- JPEG 编码器残留
- bind 关系残留
- framesource 未释放
- 再次启动时出现资源冲突

## 九、第一次阅读这份源码时建议怎么看

建议顺序：

1. 先看 `main()`
2. 再看 `startup_pipeline()`
3. 再看 `process_command()`
4. 再看 `handle_snap()` / `capture_jpeg_once()`
5. 最后再看 `g_params[]` 和每个 `get_*/set_*` 函数

这样最容易先建立“整体框架”，再去看细节。
