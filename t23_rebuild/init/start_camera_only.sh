#!/bin/sh

set -e

# -----------------------------------------------------------------------------
# start_camera_only.sh
#
# 角色:
#   这是一个“单次直接拉起 camera bring-up”的辅助脚本。
#
# 为什么保留:
#   主启动链现在已经是:
#       app_init.sh -> start.sh -> app_main.sh -> t23_camera_diag
#   但在调试时，有时我们只想快速验证 sensor/ISP/framesource，
#   不想经过完整主链，这个脚本就很有用。
# -----------------------------------------------------------------------------
APP=/system/bin/t23_camera_diag
SINFO_KO_PATH=/lib/modules
SENSOR_DRV_PATH=/lib/modules
ISP_DRV_PATH=/lib/modules

#
# 函数: check_return
# 作用:
#   检查上一条命令是否成功，失败就退出。
#
check_return()
{
	if [ $? -ne 0 ] ; then
		echo "err: $1"
		exit 1
	fi
}

# 加载 sensor 信息探测模块。
lsmod | grep "sinfo" >/dev/null 2>&1 || insmod ${SINFO_KO_PATH/%\//}/sinfo.ko
check_return "insmod sinfo"

# 触发 sensor 探测。
echo 1 >/proc/jz/sinfo/info
check_return "start sinfo"

# 读取当前探测到的 sensor。
SENSOR_INFO=`cat /proc/jz/sinfo/info`
check_return "get sensor type"
echo "${SENSOR_INFO}"

# 从探测结果中提取 sensor 名称，并准备默认 ISP 参数。
SENSOR=${SENSOR_INFO#*:}
ISP_PARAM="isp_clk=125000000"
SENSOR_PARAM=

# 加载 tx-isp 主驱动。
echo "loading tx-isp for ${SENSOR}"
lsmod | grep "tx_isp" >/dev/null 2>&1 || insmod ${ISP_DRV_PATH/%\//}/tx-isp-t23.ko ${ISP_PARAM}
check_return "insmod tx-isp-t23"

# 加载对应的 sensor 驱动。
echo "loading sensor driver ${SENSOR}"
lsmod | grep "${SENSOR}" >/dev/null 2>&1 || insmod ${SENSOR_DRV_PATH/%\//}/sensor_${SENSOR}_t23.ko ${SENSOR_PARAM}
check_return "insmod sensor driver"

# 这个脚本固定跑 framesource，因为它最适合用来确认
# “sensor + ISP + 本地图像链路”是否已经正常。
echo "running ${APP} framesource"
${APP} framesource
