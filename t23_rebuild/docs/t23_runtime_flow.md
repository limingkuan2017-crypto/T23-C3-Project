# T23 Runtime Flow

## Purpose

This document explains how the rebuilt T23 software starts, what each script
does, and where to look when a stage fails.

## Boot Chain

1. Kernel boots and mounts the squashfs root filesystem.
2. BusyBox `init` runs `/etc/init.d/rcS` from the rootfs.
3. `rcS` mounts `/system` from the `appfs` partition.
4. `rcS` launches `/system/init/app_init.sh`.
5. `app_init.sh` calls `/system/init/start.sh`.
6. `start.sh`:
   - loads `sinfo.ko`
   - detects the active sensor
   - loads `tx-isp-t23.ko`
   - loads the matching sensor driver
   - launches `/system/init/app_main.sh`
7. `app_main.sh` starts `/system/bin/t23_camera_diag`.

## Diagnostic Modes

`t23_camera_diag` supports three modes:

- `isp-only`
  Use this when checking whether sensor detect and ISP init work at all.
- `framesource`
  Use this as the default bring-up test. It proves the local video path works.
- `jpeg`
  Use this to validate the encoder/VPU/hwicodec path and save JPEG snapshots.

## Function Guide

### app/camera/src/main.c

- `make_sensor_cfg()`
  Builds the runtime sensor configuration used by all diagnostic modes.
- `create_groups()`
  Creates encoder groups needed before JPEG channels can exist.
- `bind_jpeg_channels()`
  Binds frame source output into the JPEG encoder path.
- `main()`
  Entry point that runs one of the staged checks.

### app/camera/src/camera_common.c

This file is vendor-derived and large. For the current project, the most
important functions are:

- `sample_system_init()`
  Brings up ISP, sensor and IMP system.
- `sample_framesource_init()`
  Creates frame source channels above the ISP path.
- `sample_jpeg_init()`
  Creates JPEG encoder channels. Failures here usually point to VPU or codec
  issues, not sensor issues.
- `sample_get_jpeg_snap()`
  Saves JPEG snapshots into `/tmp`.

## Required Runtime Pieces

- `tx-isp-t23.ko`
- `sensor_sc2337p_t23.ko`
- `sinfo.ko`
- compatible `libimp`
- kernel/rootfs/module combination that matches the media stack

## Notes

- The data partition is optional during early bring-up.
- JPEG snapshots and test outputs are written to `/tmp`.
- The rebuild project deliberately avoids the old vendor T23-C3 network model.
