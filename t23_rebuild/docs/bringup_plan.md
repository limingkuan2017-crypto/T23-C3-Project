# Bring-up plan

## Phase 1: T23 camera-only

Target:
- sensor detect OK
- tx-isp init OK
- sample_video_init or equivalent local video init OK
- no dependency on ESP32-C3, SPI or ImageTool

## Phase 2: SPI electrical + driver bring-up

Target:
- verify SSI master configuration
- verify Data Ready GPIO
- verify short fixed-size transfers to C3

## Phase 3: T23 <-> C3 protocol

Target:
- define packet header
- ACK/retry rules
- image chunk transport

## Phase 4: ImageTool connectivity

Target:
- stable IP path
- stable port exposure
- ISP service reachable from host tool
