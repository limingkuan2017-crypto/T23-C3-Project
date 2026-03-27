# T23 启动日志逐段解析

## 说明

这份文档针对你当前已经跑通的这份启动日志，按阶段解释：

- 这行日志大概属于哪个阶段
- 是谁打印的
- 是否能从我们当前仓库精确定位到函数/脚本

需要先说明一件事：

- `U-Boot`、Linux 内核大量初始化日志、以及部分闭源驱动日志，我们无法从当前仓库精确定位到具体源码函数
- 对这些内容，文档会标成：
  - `来源：推断`
- 对我们自己现在能看到源码的部分，会标成：
  - `来源：精确可定位`

## 一、U-Boot SPL 阶段

### 日志

```text
U-Boot SPL 2013.07-H20250508a (Dec 10 2025 - 07:24:26)
Board info: T23N
apll_freq = 1188000000
mpll_freq = 1200000000
sdram init start
DDR clk rate 600000000
DDR_PAR of eFuse: 00000000 00000000
sdram init finished
image entry point: 0x80100000
```

### 解释

- 这是第一阶段 bootloader，也就是 `SPL`
- 它的职责是先把 DDR 初始化好，再把后续完整 U-Boot 拉起来

### 来源

- 来源：推断
- 当前仓库没有这部分源码
- 属于 vendor/U-Boot SPL 阶段

## 二、U-Boot 主阶段

### 日志

```text
U-Boot 2013.07 (Dec 10 2025 - 07:24:26)
Board: ISVP (Ingenic XBurst T23 SoC)
DRAM: 64 MiB
...
SF: Detected ZB25VQ128
```

### 解释

- 这是完整 U-Boot 阶段
- 主要完成：
  - 识别 SPI NOR flash
  - 读取 boot 环境
  - 读取 kernel 镜像

### 来源

- 来源：推断
- 当前仓库没有完整 U-Boot 源码

## 三、OTA 标志检查阶段

### 日志

```text
fsload 0x81f7b0a8 upGradeFlag
### JFFS2 loading 'upGradeFlag' to 0x81f7b0a8
Scanning JFFS2 FS:  done.
find_inode failed for name=upGradeFlag
load: Failed to find inode
### JFFS2 LOAD ERROR<0> for upGradeFlag!
===================no ota task======================
```

### 解释

- bootloader 在检查一个 OTA 升级标志文件 `upGradeFlag`
- 没找到，所以走普通启动流程

### 来源

- 来源：推断
- 当前仓库没有这段 bootloader 应用逻辑源码

## 四、读取 kernel 镜像并跳转

### 日志

```text
SF: 2621440 bytes @ 0x40000 Read: OK
## Booting kernel from Legacy Image at 80600000 ...
Image Name:   Linux-3.10.14__isvp_pike_1.0__
...
Verifying Checksum ... OK
Uncompressing Kernel Image ... OK
Starting kernel ...
```

### 解释

- U-Boot 已经把 `uImage` 从 flash 读到内存
- 校验通过后，开始解压并跳入 Linux 内核入口

### 来源

- 来源：推断
- 属于 U-Boot 标准启动阶段

## 五、Linux 内核早期初始化

### 日志

示例：

```text
[    0.000000] Initializing cgroup subsys cpu
[    0.000000] Linux version 3.10.14__isvp_pike_1.0__ ...
[    0.000000] CPU0 revision is: 00d00100 (Ingenic Xburst)
[    0.000000] Determined physical RAM map:
[    0.000000] Kernel command line: console=ttyS1,115200n8 ...
```

### 解释

- 这些是 Linux kernel 自己打印的早期初始化日志
- 这里最重要的一条是：

```text
Kernel command line: console=ttyS1,115200n8 ...
```

它说明：

- 当前控制台串口是 `ttyS1`
- 默认内核控制台波特率是 `115200`

### 来源

- 来源：推断
- 属于 Linux 内核自身初始化打印

## 六、内核驱动初始化阶段

### 日志

示例：

```text
[    0.134860] jz-dma jz-dma: JZ SoC DMA initialized
[    0.156150] i2c-gpio i2c-gpio.1: using pins 57 (SDA) and 58 (SCL)
[    0.336982] jz-uart.1: ttyS1 at MMIO 0x10031000 (irq = 58) is a uart1
[    0.399550] JZ SFC Controller for SFC channel 0 driver register
[    0.478411] JZ SSI Controller for SPI channel 0 driver register
[    0.536205] soc_vpu probe success,version:1.0.0-03203fd46d
```

### 解释

- 这些是内核里各个驱动 probe 成功时的日志
- 当前项目后续最相关的是：
  - `ttyS1`：串口控制台/调参口
  - `SSI`：后续 SPI 主机链路
  - `soc_vpu`：JPEG 编码相关硬件

### 来源

- 来源：推断
- 其中部分驱动源码不可见，属于内核/闭源模块阶段

## 七、rootfs 的 `rcS` 启动阶段

### 日志

```text
mdev is ok......
net.core.wmem_max = 26214400
net.core.wmem_default = 26214400
```

### 解释

- 这几行来自 rootfs 的 `rcS`

### 来源

- 来源：精确可定位
- 文件：vendor rootfs 的 `/etc/init.d/rcS`
- 说明：这个文件不在当前仓库源码树里，而是在打包分析时从 vendor 的 squashfs rootfs 中解包得到
- 位置说明：
  - `mdev is ok......`
    来自：
    ```sh
    /sbin/mdev -s && echo "mdev is ok......"
    ```
  - `net.core.wmem_max = 26214400`
  - `net.core.wmem_default = 26214400`
    来自：
    ```sh
    sysctl -w net.core.wmem_max=26214400
    sysctl -w net.core.wmem_default=26214400
    ```

## 八、`app_init.sh` 入口

### 日志

```text
app_init.sh start
```

### 解释

- 说明 rootfs 已经挂载 `/system`
- 并且已经开始执行我们 rebuild 自己的启动链

### 来源

- 来源：精确可定位
- 文件：[app_init.sh](/home/kuan/T23-C3-Project/t23_rebuild/init/app_init.sh)
- 打印位置：
  - 直接来自：
    ```sh
    echo "app_init.sh start"
    ```

## 九、sensor 探测与 ISP 主驱动加载阶段

### 日志

示例：

```text
[    1.651811] i2c i2c-0: i2c_jz_irq 441, I2C transfer error, ABORT interrupt
...
info: success sensor find : sc2337p
sensor :sc2337p
/system/init/start_param
ISP_PARAM=isp_clk=125000000
SENSOR_PARAM=
@@@@ tx-isp-probe ok(version H20241028a), compiler date=Oct 28 2024 @@@@@
```

### 解释

- 前面的 `i2c ... ABORT` 是 sensor 探测过程中的尝试读寄存器
- 后面 `success sensor find : sc2337p` 表示最终探测成功
- `sensor :sc2337p`、`/system/init/start_param`、`ISP_PARAM=...`、`SENSOR_PARAM=...` 是我们启动脚本打印的
- `@@@@ tx-isp-probe ok ...` 是 `tx-isp-t23.ko` 模块加载成功后的日志

### 来源

- `sensor :sc2337p`
  - 来源：精确可定位
  - 文件：[start.sh](/home/kuan/T23-C3-Project/t23_rebuild/init/start.sh)
  - 打印代码：
    ```sh
    echo "${SENSOR_INFO}"
    ```
- `/system/init/start_param`
  - 来源：精确可定位
  - 文件：[start.sh](/home/kuan/T23-C3-Project/t23_rebuild/init/start.sh)
  - 打印代码：
    ```sh
    echo "${PARAM_PATH}"
    ```
- `ISP_PARAM=...`
  - 来源：精确可定位
  - 文件：[start.sh](/home/kuan/T23-C3-Project/t23_rebuild/init/start.sh)
  - 打印代码：
    ```sh
    echo "ISP_PARAM=${ISP_PARAM}"
    ```
- `SENSOR_PARAM=...`
  - 来源：精确可定位
  - 文件：[start.sh](/home/kuan/T23-C3-Project/t23_rebuild/init/start.sh)
  - 打印代码：
    ```sh
    echo "SENSOR_PARAM=${SENSOR_PARAM}"
    ```
- `info: success sensor find : sc2337p`
  - 来源：推断
  - 来自 `sinfo`/sensor 探测链
- `@@@@ tx-isp-probe ok ...`
  - 来源：推断
  - 来自 `tx-isp-t23.ko`

## 十、应用层入口切换阶段

### 日志

```text
app_main start
--------------------
app_main.sh start
app mode   : isp_uartd
camera mode: framesource
output dir : /tmp
--------------------
```

### 解释

- `app_main start`
  表示 `start.sh` 已经准备把控制权交给应用层入口
- `app_main.sh start`
  表示 `app_main.sh` 已经实际运行
- `app mode   : isp_uartd`
  表示当前镜像已切到“串口调参模式”
- `camera mode: framesource`
  这个值在当前模式下不重要，只是配置文件里仍然保留了 camera_diag 的默认值

### 来源

- `app_main start`
  - 来源：精确可定位
  - 文件：[start.sh](/home/kuan/T23-C3-Project/t23_rebuild/init/start.sh)
  - 打印代码：
    ```sh
    echo "app_main start"
    ```
- `app_main.sh start`
  - 来源：精确可定位
  - 文件：[app_main.sh](/home/kuan/T23-C3-Project/t23_rebuild/init/app_main.sh)
- `app mode   : isp_uartd`
  - 来源：精确可定位
  - 文件：[app_main.sh](/home/kuan/T23-C3-Project/t23_rebuild/init/app_main.sh)
- `camera mode: framesource`
  - 来源：精确可定位
  - 文件：[app_main.sh](/home/kuan/T23-C3-Project/t23_rebuild/init/app_main.sh)
- `output dir : /tmp`
  - 来源：精确可定位
  - 文件：[app_main.sh](/home/kuan/T23-C3-Project/t23_rebuild/init/app_main.sh)

## 十一、串口调参守护进程启动阶段

### 日志

```text
start_isp_uartd.sh: dev=/dev/ttyS1 baud=115200
start_isp_uartd.sh: launching /system/bin/t23_isp_uartd in background
start_isp_uartd.sh: log file -> /tmp/isp_uartd.log
start_isp_uartd.sh: pid file -> /tmp/isp_uartd.pid
start_isp_uartd.sh: after this point, close the serial terminal and reopen COM3 from the browser UI
start_isp_uartd.sh: daemon started successfully
```

### 解释

- 这一段说明：
  - `app_main.sh` 已经切到了 `start_isp_uartd.sh`
  - 守护进程准备占用 `/dev/ttyS1`
  - 它的后台日志会写到 `/tmp/isp_uartd.log`
  - 最后一条 `daemon started successfully` 是关键成功标志

### 来源

- 来源：精确可定位
- 文件：[start_isp_uartd.sh](/home/kuan/T23-C3-Project/t23_rebuild/init/start_isp_uartd.sh)
- 每条都来自脚本里的 `echo`

## 十二、守护进程 READY 阶段

### 日志

```text
READY T23_ISP_UARTD 1
```

### 解释

- 这是浏览器端最重要的“板端已上线”标志
- 看到这条说明：
  - `t23_isp_uartd` 进程已启动
  - ISP pipeline 已成功初始化
  - 串口已经成功打开
  - 网页现在可以发 `PING / GET ALL / SET / SNAP`

### 来源

- 来源：精确可定位
- 文件：[main.c](/home/kuan/T23-C3-Project/t23_rebuild/app/isp_uartd/src/main.c)
- 函数：
  - `main()`
- 打印代码：
  - `send_line(serial_fd, "READY T23_ISP_UARTD 1");`

## 十三、这份 log 里哪些是“我们自己最该关注的关键成功点”

建议优先盯住这几条：

1. `app_init.sh start`
   说明 `/system` 的 rebuild 启动链已经被执行
2. `sensor :sc2337p`
   说明 sensor 探测成功
3. `@@@@ tx-isp-probe ok(...)`
   说明 ISP 主驱动加载成功
4. `app mode   : isp_uartd`
   说明镜像确实进入串口调参模式
5. `start_isp_uartd.sh: daemon started successfully`
   说明后台守护进程已经启动成功
6. `READY T23_ISP_UARTD 1`
   说明网页现在可以真正连上协议服务

## 十四、如果以后再遇到问题，应该先看哪一条

- 如果卡在 boot 前面：
  看 U-Boot / kernel 是否正常启动
- 如果看不到 `app_init.sh start`：
  看 `/system` 是否挂载成功
- 如果看不到 `sensor :sc2337p`：
  看 sensor 探测和驱动加载
- 如果看不到 `app mode   : isp_uartd`：
  看 [app_mode.conf](/home/kuan/T23-C3-Project/t23_rebuild/init/app_mode.conf)
- 如果看不到 `daemon started successfully`：
  看 [start_isp_uartd.sh](/home/kuan/T23-C3-Project/t23_rebuild/init/start_isp_uartd.sh) 和 `/tmp/isp_uartd.log`
- 如果看不到 `READY T23_ISP_UARTD 1`：
  看 [main.c](/home/kuan/T23-C3-Project/t23_rebuild/app/isp_uartd/src/main.c) 的 `main()` 是否走完初始化
