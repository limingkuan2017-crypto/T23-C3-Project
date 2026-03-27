#!/usr/bin/env bash
set -euo pipefail

# -----------------------------------------------------------------------------
# package_flash_image.sh
#
# 角色:
#   生成当前 T23 rebuild 工程的可烧录镜像。
#
# 当前策略:
#   1. 继续复用已知可启动的 vendor bootloader/kernel/rootfs
#   2. 用 rebuild 的 /system 内容替换 appfs
#   3. 同时输出:
#      - appfs.img
#      - kernel.img
#      - root.img
#      - 整片 flash 用的大镜像
#
# 为什么这样做:
#   当前阶段目标是稳定 bring-up，不是完全去 vendor 化。
#   所以先保证“能启动、能定位、能验证”，再逐步替换底层。
# -----------------------------------------------------------------------------
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
. "$ROOT/scripts/env.sh" >/dev/null
VENDOR_REF="$T23_VENDOR_REF"
OUT_DIR="$ROOT/_release/image_t23"
TMP_DIR="$OUT_DIR/.tmp"
SYSTEM_DIR="$TMP_DIR/system"
ROOTFS_DIR="$TMP_DIR/rootfs"
DRIVER_DIR="$VENDOR_REF/build/images/drivers"
SENSOR_DIR="$VENDOR_REF/build/images/sensor_settings_t23"
BIN_DIR="$ROOT/output"

UBOOT_PART_SIZE=$((256 * 1024))
KERNEL_PART_SIZE=$((2560 * 1024))
ROOTFS_PART_SIZE=$((2048 * 1024))
BACK_PART_SIZE=$((5120 * 1024))
DATA_PART_SIZE=$((1280 * 1024))
SIZE_16M=$((16384 * 1024))
APP_SYSTEMFS_16M=$((${SIZE_16M}-${UBOOT_PART_SIZE}-${KERNEL_PART_SIZE}-${ROOTFS_PART_SIZE}-${DATA_PART_SIZE}-${BACK_PART_SIZE}))

IMAGE_NAME=T23N_gcc540_uclibc_16M_camera_diag.img
TXT_NAME=T23N_gcc540_uclibc_16M_camera_diag.txt

# 1. 先构建所有 T23 侧诊断程序。
"$ROOT/scripts/build_camera.sh"

# 2. 清理临时目录，并构造 /system 目录骨架。
rm -rf "$TMP_DIR"
mkdir -p "$TMP_DIR" "$SYSTEM_DIR/bin" "$SYSTEM_DIR/init" "$SYSTEM_DIR/etc/sensor" "$SYSTEM_DIR/lib/modules"

# 3. 复制 bootloader/kernel，并重打包 rootfs。
cp "$VENDOR_REF/build/images/u-boot-with-spl.bin" "$TMP_DIR/"
cp "$VENDOR_REF/build/images/uImage" "$TMP_DIR/"
cp "$VENDOR_REF/build/images/uImage" "$TMP_DIR/kernel.img"

# vendor rootfs 默认会通过 busybox init 的 inittab 在 console 上 respawn getty。
# 这会不断抢占 ttyS1，导致我们的 Web Serial 调参协议收到 "Password:"。
# 所以这里在打包时把 getty 行注释掉，让 UART 在启动后真正空出来给 isp_uartd。
unsquashfs -d "$ROOTFS_DIR" "$VENDOR_REF/build/images/root-uclibc-toolchain540.squashfs" >/dev/null
sed -i 's@^console::respawn:/sbin/getty -L console 115200 vt100 # GENERIC_SERIAL@# console getty disabled by t23_rebuild package for ISP UART tuning@' \
	"$ROOTFS_DIR/etc/inittab"

# 这里仍然保留 vendor 的 rcS、基础工具链和系统目录结构，只做最小必要修改。
# 这样可以最大限度复用“已知可启动”的底座，同时把串口口子从 login 手里让出来。
mksquashfs "$ROOTFS_DIR" "$TMP_DIR/root-uclibc-toolchain540.squashfs" -noappend -comp xz >/dev/null
cp "$TMP_DIR/root-uclibc-toolchain540.squashfs" "$TMP_DIR/root.img"

# 4. 复制 rebuild 产物。
cp "$DRIVER_DIR"/*.ko "$SYSTEM_DIR/lib/modules/"
cp "$SENSOR_DIR"/* "$SYSTEM_DIR/etc/sensor/"
cp "$BIN_DIR/t23_camera_diag" "$SYSTEM_DIR/bin/"
cp "$BIN_DIR/t23_spi_diag" "$SYSTEM_DIR/bin/"
cp "$BIN_DIR/t23_isp_uartd" "$SYSTEM_DIR/bin/"

# /system/init 下面既包含 app_init.sh / app_main.sh 这样的启动脚本，
# 也包含 start_isp_uartd.sh / app_mode.conf 这样的调参模式配置。
cp "$ROOT/init/"* "$SYSTEM_DIR/init/"
chmod 755 "$SYSTEM_DIR/init/"*.sh

# 5. 创建 .system 哨兵文件。
#    vendor 的 rcS 会用它判断 /system 是否已经初始化完成。
touch "$SYSTEM_DIR/.system"

# 6. 生成 appfs.jffs2 镜像。
mkfs.jffs2 -o "$TMP_DIR/appfs.img" -r "$SYSTEM_DIR" -e 0x8000 -s 0x1000 -n -l -X zlib --pad="${APP_SYSTEMFS_16M}"
cp "$TMP_DIR/appfs.img" "$TMP_DIR/system.jffs2"

rm -f "$TMP_DIR/$IMAGE_NAME"

# 7. 预分配整片 flash 大小，保证最终输出和 16 MB flash 一致。
truncate -s "${SIZE_16M}" "$TMP_DIR/$IMAGE_NAME"

# 8. 按分区偏移把 bootloader / kernel / rootfs / system 依次写进整镜像。
dd if="$TMP_DIR/u-boot-with-spl.bin" of="$TMP_DIR/$IMAGE_NAME" obs=1 seek=0 conv=notrunc
dd if="$TMP_DIR/uImage" of="$TMP_DIR/$IMAGE_NAME" obs=${UBOOT_PART_SIZE} seek=1 conv=notrunc
dd if="$TMP_DIR/root-uclibc-toolchain540.squashfs" of="$TMP_DIR/$IMAGE_NAME" obs=$((${UBOOT_PART_SIZE}+${KERNEL_PART_SIZE})) seek=1 conv=notrunc
dd if="$TMP_DIR/system.jffs2" of="$TMP_DIR/$IMAGE_NAME" obs=$((${UBOOT_PART_SIZE}+${KERNEL_PART_SIZE}+${ROOTFS_PART_SIZE})) seek=1 conv=notrunc

# 9. 导出最终产物到 release 目录。
mkdir -p "$OUT_DIR"
cp "$TMP_DIR/appfs.img" "$OUT_DIR/"
cp "$TMP_DIR/kernel.img" "$OUT_DIR/"
cp "$TMP_DIR/root.img" "$OUT_DIR/"
cp "$TMP_DIR/$IMAGE_NAME" "$OUT_DIR/"

# 10. 生成一份简单文本清单，方便后续追踪产物信息。
cat > "$OUT_DIR/$TXT_NAME" <<EOF
project    : t23_rebuild
mode       : camera_diag
binary     : t23_camera_diag
sensor     : sc2337p
vendor_ref : $VENDOR_REF
drivers    : $(cd "$DRIVER_DIR" && ls *.ko | sed 's/.ko//g' | tr '\n' ' ')
rootfs     : repacked root-uclibc-toolchain540.squashfs (console getty disabled)
EOF

# 11. 打印输出结果。
echo
echo "Generated:"
echo "  $OUT_DIR/appfs.img"
echo "  $OUT_DIR/kernel.img"
echo "  $OUT_DIR/root.img"
echo "  $OUT_DIR/$IMAGE_NAME"
echo "  $OUT_DIR/$TXT_NAME"
