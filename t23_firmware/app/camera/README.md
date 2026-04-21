# app/camera

This directory now contains the first clean-room bring-up app:

- `t23_camera_diag`

It is intentionally split into three modes:

- `isp-only`: only check sensor + ISP init
- `framesource`: check sensor + ISP + framesource stream path
- `jpeg`: add JPEG encoder to isolate VPU/hwicodec issues

Current source basis:

- build structure references the official Ingenic SDK
- sensor/IMP initialization code is copied from the vendor reference as a temporary snapshot

This lets us separate:

1. basic T23 camera bring-up
2. JPEG/VPU problems
3. future SPI/C3 integration
