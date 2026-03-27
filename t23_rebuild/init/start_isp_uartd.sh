#!/bin/sh

set -e

# -----------------------------------------------------------------------------
# start_isp_uartd.sh
#
# 角色:
#   把当前 T23 板子切换到“串口 ISP 调参模式”。
#
# 使用场景:
#   1. 板子已经正常启动到 Linux。
#   2. 你准备把当前 UART 控制台口临时交给 t23_isp_uartd 使用。
#   3. PC 端随后会通过浏览器 Web Serial 或串口工具接管同一条串口。
#
# 重要提醒:
#   - 这个脚本默认使用 /dev/ttyS1，因为当前平台日志显示 ttyS1 是控制台口。
#   - 如果 ttyS1 同时还在刷 kernel log / login 提示，调参协议会被干扰。
#   - 所以脚本会尽量压低 printk，并停止 getty / login，给调参进程让路。
#   - 运行后，当前串口终端最好关闭，再由浏览器或串口工具重新打开该 COM 口。
# -----------------------------------------------------------------------------

UART_DEV=${1:-/dev/ttyS1}
UART_BAUD=${2:-921600}

echo "start_isp_uartd.sh: dev=${UART_DEV} baud=${UART_BAUD}"

# 降低内核往控制台刷日志的级别，避免把串口调参协议污染掉。
if [ -w /proc/sys/kernel/printk ]; then
	echo "1 1 1 1" > /proc/sys/kernel/printk || true
fi

# 某些 rootfs 会通过 busybox init / getty 反复占用控制台口。
# 这里尽量把常见的占用者停掉；如果系统里没有这些进程，忽略即可。
killall getty 2>/dev/null || true
killall login 2>/dev/null || true

# 给串口从“人机交互模式”切换到“协议通道模式”一点缓冲时间。
sleep 1

killall t23_isp_uartd 2>/dev/null || true

echo "start_isp_uartd.sh: launching /system/bin/t23_isp_uartd in background"
echo "start_isp_uartd.sh: log file -> /tmp/isp_uartd.log"
echo "start_isp_uartd.sh: pid file -> /tmp/isp_uartd.pid"
echo "start_isp_uartd.sh: after this point, close the serial terminal and reopen COM3 from the browser UI"

rm -f /tmp/isp_uartd.log /tmp/isp_uartd.pid

# 某些精简 rootfs 没有 nohup，所以这里直接用后台进程方式启动。
# 当前脚本本身运行在系统启动链里，不依赖交互式 shell，因此这样足够稳定。
/system/bin/t23_isp_uartd --port "${UART_DEV}" --baud "${UART_BAUD}" \
	>/tmp/isp_uartd.log 2>&1 </dev/null &
echo $! > /tmp/isp_uartd.pid

retry=0
while [ "$retry" -lt 5 ]; do
	# 这里只检查进程是否还活着，而不强依赖串口是否已经收到首条命令。
	# 这样做的原因是：
	# 1. ISP pipeline 初始化本身可能就要几秒
	# 2. 浏览器端连接还没发生时，守护进程本来就应该处于“空闲等待命令”状态
	if kill -0 "$(cat /tmp/isp_uartd.pid 2>/dev/null)" 2>/dev/null; then
		echo "start_isp_uartd.sh: daemon started successfully"
		exit 0
	fi
	retry=$((retry + 1))
	sleep 1
done

echo "start_isp_uartd.sh: daemon failed to start"
if [ -f /tmp/isp_uartd.log ]; then
	echo "start_isp_uartd.sh: last log lines:"
	tail -20 /tmp/isp_uartd.log || true
fi
exit 1
