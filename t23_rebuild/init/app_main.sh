#!/bin/sh

set -e

# -----------------------------------------------------------------------------
# app_main.sh
#
# 角色:
#   这是 T23 rebuild 的应用层入口脚本，负责决定当前进入哪种“应用模式”：
#   1. camera_diag
#   2. isp_uartd
#   3. isp_bridge
#
# 设计目标:
#   - 同一份镜像可以切换不同诊断模式
#   - 同一份镜像也可以切换“应用类型”
#   - 默认不依赖 data 分区
#   - 输出日志尽量简单清晰
# -----------------------------------------------------------------------------

# 如果存在外部模式配置文件，则先把它加载进来。
# 这样做可以在不改镜像脚本的前提下，通过 /tmp 临时切换模式；
# 如果 /tmp 没放配置，则回退到 /system/init/app_mode.conf。
APP_MODE_FILE=/tmp/app_mode.conf
find "$APP_MODE_FILE" >/dev/null 2>&1 || APP_MODE_FILE=/system/init/app_mode.conf
if [ -f "$APP_MODE_FILE" ]; then
	# shellcheck disable=SC1090
	. "$APP_MODE_FILE"
fi

# APP_MODE 决定运行哪个主程序。
# camera_diag:
#   继续沿用原来的相机诊断流程
# isp_uartd:
#   自动进入串口 ISP 调参模式，适合直接配合浏览器页面使用
# isp_bridge:
#   进入“UART 控制 + SPI 传图”的桥接模式，供 C3 WiFi 方案使用
APP_MODE=${APP_MODE:-camera_diag}

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

# isp_uartd 模式使用调试串口，默认仍然走 ttyS1。
ISP_UART_DEV=${ISP_UART_DEV:-${UART_DEV:-/dev/ttyS1}}
ISP_UART_BAUD=${ISP_UART_BAUD:-${UART_BAUD:-115200}}

# isp_bridge 模式使用连接到 C3 的控制串口。
# 根据当前硬件图，T23 pin70/71 对应的是 UART0，因此这里默认走 ttyS0。
BRIDGE_UART_DEV=${BRIDGE_UART_DEV:-/dev/ttyS0}
BRIDGE_UART_BAUD=${BRIDGE_UART_BAUD:-115200}

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
echo "app mode   : ${APP_MODE}"
echo "camera mode: ${CAMERA_MODE}"
echo "output dir : ${TARGET_DIR}"
echo --------------------

case "$APP_MODE" in
	"camera_diag")
		# 真正启动 T23 相机诊断程序。
		exec /system/bin/t23_camera_diag ${CAMERA_MODE}
		;;
	"isp_uartd")
		# 自动切到串口 ISP 调参模式。
		exec /system/init/start_isp_uartd.sh "${ISP_UART_DEV}" "${ISP_UART_BAUD}"
		;;
	"isp_bridge")
		# 自动切到 C3 桥接模式：控制命令走 UART，图像数据走 SPI。
		exec /system/bin/t23_isp_bridge --port "${BRIDGE_UART_DEV}" --baud "${BRIDGE_UART_BAUD}"
		;;
	*)
		echo "err: unsupported APP_MODE=${APP_MODE}"
		exit 1
		;;
esac
