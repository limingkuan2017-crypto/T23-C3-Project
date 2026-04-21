# c3_firmware

这里是 ESP32-C3 侧工程。

当前它负责：

- 连接 WiFi 路由器
- 提供网页端
- 通过 UART 控制 T23
- 通过 SPI 从 T23 接收 JPEG 与 16x16 rectified mosaic

如果要看代码，不建议单独找旧说明，直接读：

1. [项目总指南](/home/kuan/T23-C3-Project/docs/project_guide_zh.md)
2. [C3 bridge 主程序](/home/kuan/T23-C3-Project/c3_firmware/main/main.c)
3. [共享协议头](/home/kuan/T23-C3-Project/shared/include/t23_c3_protocol.h)

## 最常用命令

```sh
cd /home/kuan/T23-C3-Project/c3_firmware
source ./scripts/idf_env.sh
idf.py -p /dev/ttyUSB0 flash monitor
```
