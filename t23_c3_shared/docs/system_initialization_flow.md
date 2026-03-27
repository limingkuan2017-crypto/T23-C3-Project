# System Initialization Flow

## Purpose

This document explains the initialization flow of the rebuilt T23N + ESP32-C3
system from power-on up to the current SPI bring-up stage.

It is written for a newcomer who needs to understand:

- what starts first
- which component depends on which earlier step
- where to look when a stage fails
- which parts are already implemented and which are still placeholders

## System Scope

The rebuilt system is split into three repositories:

- `t23_rebuild`
  T23 Linux side, camera/ISP/JPEG/SPI master
- `c3_rebuild`
  ESP32-C3 firmware side, SPI slave and later WiFi logic
- `t23_c3_shared`
  shared pin map, protocol notes and bring-up documents

## High-Level Initialization Order

At the current project stage, the intended order is:

1. T23 powers on and boots Linux
2. T23 root filesystem starts the minimal camera bring-up scripts
3. T23 detects the sensor and loads ISP-related kernel modules
4. T23 starts the diagnostic camera application
5. T23 validates local video path:
   - ISP only
   - frame source
   - JPEG snapshot
6. T23 validates SPI master-side interface
7. ESP32-C3 powers on and starts the minimal SPI slave firmware
8. ESP32-C3 raises `Data Ready` when its response buffer is ready
9. T23 performs a short fixed SPI transaction
10. Only after the above is stable do we introduce framed packets, image chunks,
    WiFi transfer and ImageTool connectivity

## Part 1: T23 Initialization Flow

### 1. Boot ROM / SPL / U-Boot

The T23N boot flow starts from:

1. Boot ROM
2. SPL
3. U-Boot
4. Linux kernel image (`uImage`)

What this stage proves:

- flash layout is readable
- the packaged image is structurally valid
- kernel can be loaded and decompressed

Typical evidence in serial log:

```text
U-Boot SPL ...
U-Boot ...
## Booting kernel from Legacy Image ...
Starting kernel ...
```

If boot stops before `Starting kernel ...`, the problem is below Linux userspace
and is not related to the camera application or SPI code.

### 2. Linux Kernel Initialization

After `Starting kernel ...`, Linux initializes:

- clocks
- memory
- UART console
- flash partitions
- I2C
- SSI/SPI controller
- VPU-related hardware blocks
- root filesystem mount

What matters most for this project:

- `jz-ssi.0` should register
- root squashfs should mount
- the board reaches user space

Typical evidence:

```text
JZ SSI Controller for SPI channel 0 driver register
soc_vpu probe success
VFS: Mounted root (squashfs filesystem) readonly on device ...
```

### 3. BusyBox init and rcS

Once Linux enters user space:

1. BusyBox `init` reads `/etc/inittab`
2. `/etc/init.d/rcS` runs
3. `rcS` mounts `/system` from the `appfs` partition
4. `rcS` launches `/system/init/app_init.sh`

This matters because the T23 application does not start directly from the root
filesystem. It starts from the writable `/system` partition that we pack into
the flash image.

### 4. app_init.sh

File:

- `/system/init/app_init.sh`

Current role:

- print a clear first log line
- jump immediately into the rebuild startup chain

Current behavior:

```sh
echo "app_init.sh start"
/system/init/start.sh
```

Typical evidence:

```text
app_init.sh start
```

If this line never appears, the `/system` partition content or the rootfs init
chain is wrong.

### 5. start.sh

File:

- `/system/init/start.sh`

Current role:

- discover the active sensor
- load the minimum ISP-related modules
- hand control to the diagnostic application launcher

Detailed steps:

1. load `sinfo.ko`
2. write `1` into `/proc/jz/sinfo/info`
3. read sensor name from `/proc/jz/sinfo/info`
4. parse `/system/init/start_param`
5. load `tx-isp-t23.ko`
6. load the matching sensor driver
7. call `/system/init/app_main.sh`

Typical evidence:

```text
sensor :sc2337p
ISP_PARAM=isp_clk=125000000
@@@@ tx-isp-probe ok(...)
app_main start
```

If sensor detect fails here, the likely causes are:

- I2C problem
- wrong sensor module
- wrong sensor power/reset sequence outside the visible app layer

### 6. app_main.sh

File:

- `/system/init/app_main.sh`

Current role:

- choose which T23 diagnostic mode to run
- optionally mount a writable data partition
- start `t23_camera_diag`

Current default:

- `CAMERA_MODE=framesource`
- outputs use `/tmp`

Typical evidence:

```text
app_main.sh start
camera mode: framesource
output dir : /tmp
```

### 7. t23_camera_diag

File:

- `t23_rebuild/app/camera/src/main.c`

Current role:

- provide a small, staged camera diagnostic entry point
- isolate failures by layer

Supported modes:

- `isp-only`
  proves sensor + ISP stack only
- `framesource`
  proves local video path
- `jpeg`
  proves JPEG encoder / VPU / codec path

#### Mode A: isp-only

Steps:

1. build runtime sensor configuration
2. call `sample_system_init()`
3. stop

What it proves:

- ISP can open
- sensor can be added and enabled
- `IMP_System_Init()` works

#### Mode B: framesource

Steps:

1. run all `isp-only` steps
2. call `sample_framesource_init()`
3. stream on briefly
4. stream off
5. clean shutdown

What it proves:

- local video pipeline is alive
- image frames can flow without involving JPEG encoding

Typical evidence:

```text
diag: sample_system_init ok
diag: sample_framesource_init ok
diag: framesource mode complete
```

#### Mode C: jpeg

Steps:

1. run all `framesource` prerequisites
2. create encoder groups
3. call `sample_jpeg_init()`
4. bind frame source to encoder
5. start streaming
6. call `sample_get_jpeg_snap()`
7. save snapshots into `/tmp`
8. clean shutdown

What it proves:

- VPU / JPEG / hwicodec path is alive
- encoded image output can actually be generated

Typical evidence:

```text
diag: sample_jpeg_init ok
diag: JPEG channels bound
diag: sample_get_jpeg_snap ret=0
diag: jpeg mode complete
```

## Part 2: T23 SPI Initialization Flow

### 1. Linux SPI Controller Availability

The SPI electrical bring-up on T23 depends on:

- kernel SSI driver loaded
- pinmux correct for SSI0
- `/dev/spidev0.0` present

Typical evidence:

```text
JZ SSI Controller for SPI channel 0 driver register
```

### 2. t23_spi_diag

File:

- `t23_rebuild/app/spi_diag/src/main.c`

Current role:

- prove T23 master-side SPI access works
- verify the new `Data Ready` line is treated as an input from C3

Commands:

- `info`
  opens `/dev/spidev0.0` and prints current configuration
- `read-dr`
  reads GPIO53
- `wait-dr <timeout_ms>`
  polls until `Data Ready` goes high
- `xfer <bytes...>`
  performs one full-duplex SPI transfer
- `xfer-dr <bytes...>`
  waits for `Data Ready`, then performs one transfer

What `xfer` proves:

- T23 can clock out MOSI/CLK/CS
- T23 samples MISO during the same transfer

What `xfer` does not prove on its own:

- C3 firmware correctness
- final packet protocol
- image streaming logic

Typical early result with no C3 firmware:

```text
tx: a5 5a 00 ff
rx: ff ff ff ff
```

This usually means:

- T23 transfer is happening
- `MISO` is floating or pulled high
- the slave is not yet actively responding

## Part 3: C3 Initialization Flow

### 1. ESP-IDF Build Environment

The C3 project uses:

- ESP-IDF 5.4.3
- project: `c3_rebuild`

Build helper:

- `c3_rebuild/scripts/idf_build.sh`

This script:

1. exports the pinned IDF environment
2. sets target to `esp32c3`
3. runs `idf.py build`

### 2. C3 Firmware Entry

File:

- `c3_rebuild/main/main.c`

Current role:

- implement a deterministic SPI slave bring-up firmware

Detailed steps:

1. configure `Data Ready` on IO3 as output
2. configure SPI slave pins:
   - MOSI IO10
   - MISO IO8
   - CLK IO6
   - CS IO5
3. initialize `spi_slave`
4. allocate DMA-safe TX/RX buffers
5. fill TX buffer with a known fixed response
6. wait for T23 master transfer
7. raise/lower `Data Ready` around the queued transaction
8. log received bytes

### 3. Current Fixed-Response Contract

Shared definition:

- `t23_c3_shared/include/t23_c3_protocol.h`

Current stage-1 behavior:

- C3 always returns 4 bytes:

```text
5A A5 EE DD
```

This is deliberately simple because it makes wiring failures obvious before any
higher-level protocol is introduced.

## Part 4: Combined T23 + C3 Bring-Up Flow

Once both sides exist, the current intended combined initialization looks like:

1. T23 boots Linux and starts camera diagnostics
2. T23 proves `framesource` and `jpeg`
3. C3 firmware boots as SPI slave
4. C3 queues a fixed response
5. C3 raises `Data Ready`
6. T23 sees `Data Ready` high
7. T23 performs `xfer-dr`
8. T23 receives `5A A5 EE DD`
9. C3 logs the bytes sent by T23

Target test command on T23:

```sh
/system/bin/t23_spi_diag wait-dr 3000
/system/bin/t23_spi_diag xfer-dr 00 00 00 00
```

Expected result:

```text
data-ready: high
tx: 00 00 00 00
rx: 5a a5 ee dd
transfer-bytes: 4
```

## Part 5: Initialization Dependencies

The dependency chain is strict:

1. T23 kernel and rootfs must boot
2. T23 sensor/ISP path must work
3. T23 frame source path must work
4. T23 JPEG path should work
5. T23 SPI master diagnostics must work
6. C3 firmware must boot
7. C3 SPI slave path must work
8. T23<->C3 fixed-response transfer must work
9. only then should framed packets be introduced
10. only after framed packets are stable should WiFi/ImageTool transport be added

## Part 6: Common Failure Points

### Failure A: no `app_init.sh start`

Likely causes:

- `/system` partition content wrong
- rootfs startup chain not calling the rebuild init scripts

### Failure B: no sensor detected

Likely causes:

- I2C wiring or sensor power sequence issue
- wrong sensor driver or wrong sensor module name

### Failure C: `framesource` fails

Likely causes:

- ISP stack not fully initialized
- media stack mismatch between kernel/modules/libs

### Failure D: `jpeg` fails while `framesource` succeeds

Likely causes:

- codec/VPU/hwicodec mismatch
- incompatible media stack combination

### Failure E: `t23_spi_diag info` fails

Likely causes:

- no `/dev/spidev0.0`
- SPI pinmux not configured
- kernel SPI driver not available

### Failure F: C3 flash fails with `No serial data received`

Likely causes:

- wrong serial adapter
- chip not in download mode
- no working auto-reset lines
- need manual `BOOT` + `EN`

### Failure G: `xfer` works but `rx` is all `ff`

Likely causes:

- T23 master side is working
- C3 is not yet responding on MISO
- MISO is floating or pulled up

## Part 7: Files a New Developer Should Read First

Recommended order:

1. `t23_c3_shared/docs/pinmap.md`
2. `t23_c3_shared/docs/protocol.md`
3. `t23_c3_shared/docs/system_initialization_flow.md`
4. `t23_rebuild/docs/t23_runtime_flow.md`
5. `c3_rebuild/docs/c3_runtime_flow.md`
6. `t23_c3_shared/docs/project_handover.md`
