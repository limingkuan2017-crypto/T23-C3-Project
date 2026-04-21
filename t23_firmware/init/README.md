# init

Startup scripts for the new hardware architecture.

Planned split:
- start_camera_only.sh
- start_spi_bridge.sh
- start_imagetool.sh

The old vendor start.sh mixes camera bring-up, ESP32 network behavior and legacy assumptions.
This project will keep those phases separated.
