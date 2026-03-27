# Project Handover Guide

## Overview

This rebuild effort splits the system into three independent parts:

- `t23_rebuild`
  T23 camera, ISP, JPEG and SPI master bring-up
- `c3_rebuild`
  ESP32-C3 SPI slave bring-up and future WiFi work
- `t23_c3_shared`
  Shared protocol and hardware mapping documents

This split is deliberate. The old vendor project mixed several assumptions that
no longer apply to the new hardware architecture.

## New Architecture

- T23 captures video and owns ISP-related work.
- ESP32-C3 runs its own communication stack.
- SPI is only the transport link between T23 and C3.
- `Data Ready` is driven by C3 and read by T23.

## Current Project Status

### T23

Completed:
- sensor detect
- ISP init
- frame source init
- JPEG snapshots
- SPI master-side diagnostics

Tools:
- `t23_camera_diag`
- `t23_spi_diag`

### C3

Completed:
- standalone ESP-IDF project
- SPI slave initialization
- fixed-response bring-up firmware

Pending:
- successful flashing on target hardware
- validated T23<->C3 fixed-response transaction

### Shared Protocol

Completed:
- pin map
- stage-1 fixed-response contract
- reserved frame header and frame type definitions

Pending:
- ping/ack framing
- chunked payload transport
- retry and timeout rules

## Recommended Bring-up Order

1. Verify T23 local camera path with `framesource`.
2. Verify T23 JPEG path with `jpeg`.
3. Verify T23 SPI master diagnostics with no C3 firmware.
4. Flash C3 fixed-response firmware.
5. Validate `Data Ready` and fixed SPI response.
6. Introduce framed packets.
7. Introduce image transport.
8. Introduce WiFi/ImageTool path.

## Required Components

### Hardware

- T23N board with sensor connected
- ESP32-C3 module
- SPI lines:
  - MOSI
  - MISO
  - CLK
  - CS
- `Data Ready` line from C3 to T23
- optional UART for C3 debug

### Software

- vendor-compatible T23 kernel/rootfs/modules/media stack
- Ingenic SDK
- ESP-IDF 5.4.3
- `usbipd-win` if flashing ESP32-C3 from WSL on Windows

## Common Failure Patterns

- sensor found, but jpeg fails
  Usually codec/VPU/media-stack mismatch
- SPI master transfer returns all `ff`
  T23 transmit is working, but C3 is not yet driving `MISO`
- C3 flash fails with "No serial data received"
  Usually wrong serial port or missing manual download mode

## Files Newcomers Should Read First

1. `t23_c3_shared/docs/pinmap.md`
2. `t23_c3_shared/docs/protocol.md`
3. `t23_rebuild/docs/t23_runtime_flow.md`
4. `c3_rebuild/docs/c3_runtime_flow.md`
