# shared

这里放 T23 和 C3 当前仍在共同使用的协议与数据结构。

这个目录不是历史遗留物，当前仍被两侧直接引用：

- T23 `isp_bridge` 通过它定义 SPI 帧格式与 16x16 mosaic 协议
- C3 固件通过它解析同一套桥接协议与校准数据结构

当前最重要的两个头文件：

- [t23_c3_protocol.h](/home/kuan/T23-C3-Project/shared/include/t23_c3_protocol.h)
  当前 UART/SPI bridge 协议
- [t23_border_pipeline.h](/home/kuan/T23-C3-Project/shared/include/t23_border_pipeline.h)
  下一阶段边框提取和 50 色块的数据结构

项目说明统一看：

- [项目总指南](/home/kuan/T23-C3-Project/docs/project_guide_zh.md)
