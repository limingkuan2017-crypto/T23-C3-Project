# t23_rebuild

T23-side rebuild workspace for the new T23N + ESP32-C3 architecture.

## What this project is for

This project exists to make the T23 side understandable and testable without
depending on the old vendor application model.

It focuses on three layers:

1. local camera/ISP/JPEG bring-up
2. SPI master-side diagnostics
3. serial ISP tuning bring-up for early tuning without WiFi
4. later integration with ESP32-C3 and ImageTool

## Why it exists

The vendor project was built for a different hardware architecture:

- old model: T23 acted as the overall controller and C3 behaved more like a
  network sidecar
- new model: T23 handles image capture, C3 handles communication, and SPI is
  only a transport path

Because of that change, vendor code is treated as reference only.

## Important files

- `app/camera/src/main.c`
  staged camera diagnostic entry point
- `app/camera/include/camera_common.h`
  high-level camera constants, structures and helper declarations
- `app/camera/src/camera_common.c`
  vendor-derived media helper code used by the diagnostic app
- `app/spi_diag/src/main.c`
  T23-side SPI master diagnostic tool
- `app/isp_uartd/src/main.c`
  T23-side serial ISP tuning daemon used by the browser tuner
- `init/start.sh`
  minimal target startup chain
- `init/start_isp_uartd.sh`
  helper script that hands the UART console over to the ISP tuning daemon
- `scripts/package_flash_image.sh`
  creates the final flashable T23 image
- `docs/t23_runtime_flow.md`
  first document a new developer should read
- `docs/t23_function_guide_zh.md`
  beginner-oriented function guide
- `docs/t23_learning_path_zh.md`
  step-by-step reading order for a first-time developer

## External dependencies

This project expects external, non-repository dependencies to live under the
repository root:

- `../third_party/ingenic_t23_sdk`
- `../third_party/vendor_reference`

They are kept outside `t23_rebuild` so the repository structure stays portable
and the dependency boundary stays obvious.

## First useful commands

- Build diagnostics: `./scripts/build_camera.sh`
- Package flash image: `./scripts/package_flash_image.sh`
- On target:
  - `t23_camera_diag isp-only`
  - `t23_camera_diag framesource`
  - `t23_camera_diag jpeg`
  - `t23_spi_diag info`
  - `start_isp_uartd.sh /dev/ttyS1 921600`
