#!/bin/sh

set -e

# -----------------------------------------------------------------------------
# start.sh
#
# 角色:
#   这是 T23 rebuild 项目的核心启动脚本，负责把系统从“Linux 已进入
#   用户态”推进到“camera 诊断程序可运行”。
#
# 当前职责:
#   1. 加载 sinfo.ko
#   2. 探测当前 sensor
#   3. 读取 start_param 中的配置
#   4. 加载 tx-isp-t23.ko
#   5. 加载对应 sensor 驱动
#   6. 调用 app_main.sh
#
# 当前刻意不做的事:
#   - 不做旧供应商网络栈初始化
#   - 不做 C3 相关协议初始化
#   - 不做最终业务逻辑
# -----------------------------------------------------------------------------
SINFO_KO_PATH=/lib/modules
SENSOR_DRV_PATH=/lib/modules
ISP_DRV_PATH=/lib/modules

#
# 函数: check_return
# 作用:
#   检查上一条命令是否成功，失败就立即停止启动链。
#
# 为什么需要:
#   bring-up 阶段最怕“前面其实失败了，后面还继续跑”，那样日志会非常乱，
#   也很难定位第一现场。
#
# 参数:
#   $1 = 当前失败步骤的说明文字
#
check_return()
{
	if [ $? -ne 0 ] ;then
		echo "err: $1"
		exit 1
	fi
}

# 1. 加载 sinfo.ko，用于探测 sensor 类型。
lsmod | grep "sinfo" >/dev/null 2>&1 || insmod ${SINFO_KO_PATH/%\//}/sinfo.ko
check_return "insmod sinfo"

# 2. 触发 sensor 探测。
echo 1 >/proc/jz/sinfo/info
check_return "start sinfo"

# 3. 读取探测结果，格式通常是 "sensor:sc2337p"。
SENSOR_INFO=`cat /proc/jz/sinfo/info`
check_return "get sensor type"
echo "${SENSOR_INFO}"

# 4. 先尝试读取 /tmp/start_param，找不到再回退到 /system/init/start_param。
#    这样做是为了方便运行时临时覆盖参数，而不用每次重打镜像。
PARAM_PATH=/tmp/start_param
find /tmp/start_param >/dev/null 2>&1 || PARAM_PATH=/system/init/start_param
echo "${PARAM_PATH}"

# 5. 从 sensor 探测结果中提取 sensor 名称。
SENSOR=${SENSOR_INFO#*:}
ISP_PARAM="isp_clk=125000000"
SENSOR_PARAM=
START=0

#
# 6. 读取 start_param 文件。
#
# 文件格式是按 sensor 分段的，例如:
#   sensor:sc2337p
#   isp_param:isp_clk=125000000
#   sensor_param:
#
# 脚本会找到当前 sensor 对应的那一段，并提取 ISP_PARAM 和 SENSOR_PARAM。
#
while read str
do
	if [ "$str" = "" ]; then
		continue
	fi
	name=${str%:*}
	value=${str#*:}
	if [ ${START} = 0 ]; then
		if [ "$value" = "$SENSOR" ]; then
			START=1
		fi
	else
		case ${name} in
			"isp_param")
				ISP_PARAM=${value}
				;;
			"sensor_param")
				SENSOR_PARAM=${value}
				;;
			*)
				break
				;;
		esac
	fi
done < ${PARAM_PATH}

echo "ISP_PARAM=${ISP_PARAM}"
echo "SENSOR_PARAM=${SENSOR_PARAM}"

# 7. 加载 tx-isp 主驱动。
lsmod | grep "tx_isp" >/dev/null 2>&1 || insmod ${ISP_DRV_PATH/%\//}/tx-isp-t23.ko ${ISP_PARAM}
check_return "insmod tx-isp"

# 8. 根据探测到的 sensor 名称拼出 sensor 驱动文件名并加载。
#    例如:
#      SENSOR=sc2337p
#      -> sensor_sc2337p_t23.ko
lsmod | grep "${SENSOR}" >/dev/null 2>&1 || insmod ${SENSOR_DRV_PATH/%\//}/sensor_${SENSOR}_t23.ko ${SENSOR_PARAM}
check_return "insmod sensor driver"

# 9. 进入应用层入口。
echo "app_main start"
/system/init/app_main.sh
