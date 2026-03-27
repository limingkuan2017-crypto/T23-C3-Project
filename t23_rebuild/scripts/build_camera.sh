#!/usr/bin/env bash
set -e

# -----------------------------------------------------------------------------
# build_camera.sh
#
# 角色:
#   构建当前 T23 bring-up 阶段最常用的三个用户态工具:
#   1. t23_camera_diag
#   2. t23_spi_diag
#   3. t23_isp_uartd
#
# 为什么继续沿用这个脚本名:
#   早期仓库已经在多个地方引用了 build_camera.sh。
#   现在虽然它构建的内容不再只有 camera，但保留原名字能减少已有脚本改动。
# -----------------------------------------------------------------------------
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

# 1. 构建 camera 诊断程序。
cd "$ROOT/app/camera"
make clean
make

# 2. 构建 SPI 诊断程序。
cd "$ROOT/app/spi_diag"
make clean
make

# 3. 构建串口 ISP 调参守护进程。
cd "$ROOT/app/isp_uartd"
make clean
make

echo
echo "Built: $ROOT/output/t23_camera_diag"
echo "Built: $ROOT/output/t23_spi_diag"
echo "Built: $ROOT/output/t23_isp_uartd"
