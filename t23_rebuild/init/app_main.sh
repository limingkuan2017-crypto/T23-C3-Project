#!/bin/sh

set -e

# -----------------------------------------------------------------------------
# app_main.sh
#
# 角色:
#   这是 T23 rebuild 的应用层入口脚本，负责决定运行哪种诊断模式，并启动
#   /system/bin/t23_camera_diag。
#
# 设计目标:
#   - 同一份镜像可以切换不同诊断模式
#   - 默认不依赖 data 分区
#   - 输出日志尽量简单清晰
# -----------------------------------------------------------------------------

# CAMERA_MODE 控制 camera 诊断程序运行哪种模式。
# 不传时默认使用 framesource，因为它最适合做第一阶段验证。
CAMERA_MODE=${CAMERA_MODE:-framesource}

# USE_DATA_PARTITION=1 时，尝试挂载 /dev/mtdblock5 到 /system/flash。
# 默认关闭，因为早期 bring-up 更关心 camera 和 jpeg 是否工作，
# 不希望被数据分区问题干扰。
USE_DATA_PARTITION=${USE_DATA_PARTITION:-0}

# TARGET_DIR 是逻辑上的“输出目录”。
# 当前诊断程序主要把 JPEG 保存到 /tmp，所以这里默认也是 /tmp。
TARGET_DIR=${TARGET_DIR:-/tmp}

# 无论后面是否切换分区，先保证目标目录存在。
mkdir -p "$TARGET_DIR"

if [ "$USE_DATA_PARTITION" = "1" ]; then
	TARGET_DIR="/system/flash"
	mkdir -p "$TARGET_DIR"
	# 可写 flash 存储在早期 bring-up 阶段是可选项，因此挂载失败时不终止，
	# 而是回退到 /tmp，避免影响 camera / jpeg 诊断。
	if ! mount -t jffs2 /dev/mtdblock5 "$TARGET_DIR"; then
		echo "warn: mount /dev/mtdblock5 failed, falling back to /tmp"
		TARGET_DIR="/tmp"
		mkdir -p "$TARGET_DIR"
	fi
fi

echo --------------------
echo "app_main.sh start"
echo "camera mode: ${CAMERA_MODE}"
echo "output dir : ${TARGET_DIR}"
echo --------------------

# 真正启动 T23 诊断程序。
/system/bin/t23_camera_diag ${CAMERA_MODE}
