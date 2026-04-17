# T23-C3-Project

这个仓库是当前 `T23N + ESP32-C3` 方案的统一工程入口。

如果你现在只想快速理解项目，不要再分散读很多文档，直接按下面顺序看：

1. [项目总指南](/home/kuan/T23-C3-Project/docs/project_guide_zh.md)
2. [校正算法详解](/home/kuan/T23-C3-Project/docs/rectification_algorithm_zh.md)
3. [Python/T23 算法同步说明](/home/kuan/T23-C3-Project/docs/python_rectification_sync_zh.md)
4. [T23 启动入口脚本](/home/kuan/T23-C3-Project/t23_rebuild/init/app_main.sh)
5. [T23 bridge 主程序](/home/kuan/T23-C3-Project/t23_rebuild/app/isp_bridge/src/main.c)
6. [C3 bridge 主程序](/home/kuan/T23-C3-Project/c3_rebuild/main/main.c)
7. [共享协议头](/home/kuan/T23-C3-Project/t23_c3_shared/include/t23_c3_protocol.h)

总指南里现在已经统一包含：

- 接口总览
- `DEBUG / RUN` 模式差异
- 关键调用顺序表
- 关键函数功能表
- 当前校正算法与灯带链路说明

如果你只关心这次和 Python 对齐后的校正实现，直接看这套“弱顶角拟合 + 内容优先、自适应比例 crop”的说明：

- [校正算法详解](/home/kuan/T23-C3-Project/docs/rectification_algorithm_zh.md)

## 目录说明

- [t23_rebuild](/home/kuan/T23-C3-Project/t23_rebuild)
  T23 侧应用、启动脚本、打包镜像
- [c3_rebuild](/home/kuan/T23-C3-Project/c3_rebuild)
  ESP32-C3 固件、网页服务、WiFi bridge
- [t23_c3_shared](/home/kuan/T23-C3-Project/t23_c3_shared)
  T23 与 C3 共用协议和边框提取数据结构
- [configs](/home/kuan/T23-C3-Project/configs)
  新旧硬件串口内核配置快照
- [third_party](/home/kuan/T23-C3-Project/third_party)
  官方 SDK 与供应商参考工程入口

## 最常用命令

### T23 打包

```sh
cd /home/kuan/T23-C3-Project/t23_rebuild
./scripts/package_flash_image.sh
```

### C3 刷机

```sh
cd /home/kuan/T23-C3-Project/c3_rebuild
source ./scripts/idf_env.sh
idf.py -p /dev/ttyUSB0 flash monitor
```

### 切换 T23 串口内核配置

```sh
cd /home/kuan/T23-C3-Project
./scripts/apply_t23_kernel_serial_profile.sh t23_new_hw
```

或：

```sh
./scripts/apply_t23_kernel_serial_profile.sh t23_vendor_hw
```
