#!/usr/bin/env bash
set -e

# -----------------------------------------------------------------------------
# build_camera.sh
#
# 角色:
#   构建 T23 侧 camera 诊断程序 `t23_camera_diag`。
#
# 为什么单独保留这个脚本:
#   这样用户不用记住 app/camera 子目录下的 make 命令，也方便后续被
#   package_flash_image.sh 复用。
# -----------------------------------------------------------------------------
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

# 进入 camera 应用目录并重新编译。
cd "$ROOT/app/camera"
make clean
make

echo
echo "Built: $ROOT/output/t23_camera_diag"
