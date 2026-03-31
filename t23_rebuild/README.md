# t23_rebuild

这里是 T23 侧工程。

如果要理解当前程序，不建议再翻很多旧文档，直接看：

1. [项目总指南](/home/kuan/T23-C3-Project/docs/project_guide_zh.md)
2. [app_main.sh](/home/kuan/T23-C3-Project/t23_rebuild/init/app_main.sh)
3. [isp_bridge 主程序](/home/kuan/T23-C3-Project/t23_rebuild/app/isp_bridge/src/main.c)
4. [camera_common.h](/home/kuan/T23-C3-Project/t23_rebuild/app/camera/include/camera_common.h)

## 当前职责

- 相机采集
- ISP 调参
- JPEG 生成
- 通过 UART 接收 C3 控制命令
- 通过 SPI 向 C3 发送图像数据

## 当前最常用命令

```sh
cd /home/kuan/T23-C3-Project/t23_rebuild
./scripts/package_flash_image.sh
```

生成镜像：

- [T23N_gcc540_uclibc_16M_camera_diag.img](/home/kuan/T23-C3-Project/t23_rebuild/_release/image_t23/T23N_gcc540_uclibc_16M_camera_diag.img)
