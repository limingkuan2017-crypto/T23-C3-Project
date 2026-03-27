# T23 Rebuild Learning Path

## Purpose

This note is written for a first-time reader of the `t23_rebuild` project.
Its goal is not to explain every line at once, but to help you build a stable
mental model in a safe order.

If you try to start with `camera_common.c` directly, it will feel large and
messy. The better approach is:

1. understand the startup chain
2. understand the small diagnostic entry points
3. only then read the large vendor-derived helper file

## Recommended Order

Read the project in this order:

1. `init/app_init.sh`
2. `init/start.sh`
3. `init/app_main.sh`
4. `app/camera/src/main.c`
5. `app/camera/include/camera_common.h`
6. `app/camera/src/camera_common.c`
7. `app/spi_diag/src/main.c`
8. `scripts/package_flash_image.sh`

## Stage 1: Understand How The Program Starts

### File: `init/app_init.sh`

Focus on:

- why this script exists at all
- how it becomes the first rebuild entry point under `/system/init`
- where it jumps next

Questions to answer after reading:

- Which script is the real startup entry for the rebuild app?
- What log line proves the script was executed?

Expected answer:

- The log `app_init.sh start` proves control reached the rebuild startup chain.

### File: `init/start.sh`

Focus on:

- how sensor detection is triggered
- how `tx-isp` and the sensor module are loaded
- when `app_main.sh` is called

Functions / shell blocks to notice:

- `check_return()`

Questions to answer after reading:

- Where does the sensor name come from?
- At what point is `tx-isp-t23.ko` inserted?
- What happens if one of the critical commands fails?

### File: `init/app_main.sh`

Focus on:

- how the camera mode is chosen
- why `/tmp` is used as the output directory
- how `t23_camera_diag` is launched

Questions to answer after reading:

- Which mode is the default if none is given?
- Why are JPEG files not written to the flash partition during bring-up?

## Stage 2: Understand The Small Camera Entry Program

### File: `app/camera/src/main.c`

This is the most important beginner-friendly C file in the project.

Read these functions in order:

1. `parse_mode()`
2. `make_sensor_cfg()`
3. `create_groups()`
4. `bind_jpeg_channels()`
5. `main()`

### What each function is doing

#### `parse_mode()`

Purpose:

- convert command-line text into an internal diagnostic mode

Important idea:

- the same binary can test different layers of the media pipeline

Modes:

- `isp-only`
- `framesource`
- `jpeg`

#### `make_sensor_cfg()`

Purpose:

- convert compile-time sensor macros into a runtime structure

Important idea:

- `camera_common.h` contains static configuration
- `make_sensor_cfg()` turns that static configuration into data that can be
  passed to helper functions

#### `create_groups()`

Purpose:

- create encoder groups before using the JPEG encoder

Important idea:

- IMP graph objects often need to exist before binding is possible

#### `bind_jpeg_channels()`

Purpose:

- connect the frame-source side to the JPEG encoder side

Important idea:

- this is one of the steps that turns independent components into a working
  media pipeline

#### `main()`

Purpose:

- drive the whole diagnostic sequence

The most important thing to learn from `main()` is the call order.

### Camera call order by mode

#### `isp-only`

```text
main
  -> parse_mode
  -> make_sensor_cfg
  -> sample_system_init
  -> sample_system_exit
```

What this proves:

- sensor + ISP + IMP core init works

#### `framesource`

```text
main
  -> parse_mode
  -> make_sensor_cfg
  -> sample_system_init
  -> sample_framesource_init
  -> sample_framesource_streamon
  -> sleep
  -> sample_framesource_streamoff
  -> sample_framesource_exit
  -> sample_system_exit
```

What this proves:

- raw video path works after ISP init

#### `jpeg`

```text
main
  -> parse_mode
  -> make_sensor_cfg
  -> sample_system_init
  -> sample_framesource_init
  -> create_groups
  -> sample_jpeg_init
  -> bind_jpeg_channels
  -> sample_framesource_streamon
  -> sample_get_jpeg_snap
  -> sample_framesource_streamoff
  -> unbind_jpeg_channels
  -> sample_encoder_exit
  -> destroy_groups
  -> sample_framesource_exit
  -> sample_system_exit
```

What this proves:

- JPEG/VPU path works in addition to the raw video path

## Stage 3: Understand The Shared Media Helper Layer

### File: `app/camera/include/camera_common.h`

Read this file before `camera_common.c`.

Focus on:

- active sensor selection macros
- resolution macros
- channel enable macros
- `struct chn_conf`
- `sample_sensor_cfg_t`
- helper function declarations

Questions to answer after reading:

- Which sensor is currently active?
- What resolution is used for bring-up?
- Which helper function should be called first?

### File: `app/camera/src/camera_common.c`

This file is large because it comes from a vendor sample and contains more
capabilities than the current minimum bring-up path needs.

Do not try to read it top-to-bottom in one pass.

Instead, focus on these functions first:

1. `sample_system_init()`
2. `sample_system_exit()`
3. `sample_framesource_init()`
4. `sample_framesource_streamon()`
5. `sample_framesource_streamoff()`
6. `sample_jpeg_init()`
7. `sample_get_jpeg_snap()`
8. `sample_encoder_exit()`

### What to learn from each one

#### `sample_system_init()`

Learn:

- where ISP and sensor attachment actually happen
- where `IMP_System_Init()` sits in the sequence

If this fails, think about:

- sensor wiring
- `tx-isp`
- kernel / module / library compatibility

#### `sample_framesource_init()`

Learn:

- how channels are created
- how frame-source attributes are applied

If this fails after `sample_system_init()` succeeds, the issue is more likely in
the video path than in basic sensor bring-up.

#### `sample_jpeg_init()`

Learn:

- how JPEG encoder channels are created
- how those channels register into a group

This is the key boundary:

- `framesource` works, but `sample_jpeg_init()` fails:
  think encoder/VPU side
- `sample_system_init()` fails:
  think sensor/ISP side

#### `sample_get_jpeg_snap()`

Learn:

- how JPEG capture starts
- how streams are polled and read
- where files are written

## Stage 4: Understand SPI Diagnostics

### File: `app/spi_diag/src/main.c`

This file is much smaller and ideal for learning basic Linux user-space SPI.

Read in this order:

1. `usage()`
2. `setup_data_ready_input()`
3. `read_data_ready_value()`
4. `wait_data_ready_high()`
5. `spi_open_configure()`
6. `spi_transfer_bytes()`
7. `main()`

Key ideas:

- T23 is the SPI master
- `Data Ready` is an input on T23 in the new architecture
- `info`, `read-dr`, `wait-dr`, `xfer`, and `xfer-dr` each verify a different
  piece of the transport layer

## Stage 5: Understand Packaging

### File: `scripts/package_flash_image.sh`

Read this last, after you understand the runtime chain.

Focus on:

- where binaries come from
- how `/system` is assembled
- how `appfs.img` is generated
- how the final flash image is assembled

Questions to answer after reading:

- Which files in `/system` come from `t23_rebuild`?
- Which base pieces are still copied from the vendor reference?

## A Good Three-Day Reading Plan

### Day 1

Read:

- `app_init.sh`
- `start.sh`
- `app_main.sh`
- `main.c`

Goal:

- understand the end-to-end bring-up sequence without drowning in details

### Day 2

Read:

- `camera_common.h`
- `sample_system_init()`
- `sample_framesource_init()`
- `sample_jpeg_init()`
- `sample_get_jpeg_snap()`

Goal:

- understand how the camera pipeline is built inside IMP

### Day 3

Read:

- `spi_diag/main.c`
- `package_flash_image.sh`

Goal:

- understand transport diagnostics and image packaging

## What To Ignore At First

If you are just getting comfortable with the project, do not try to deeply
understand these on the first pass:

- OSD helpers
- all vendor-preserved sensor branches
- every stream-dump variant in `camera_common.c`

They are useful later, but they are not required to understand the current
bring-up path.
