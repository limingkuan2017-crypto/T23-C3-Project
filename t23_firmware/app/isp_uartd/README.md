# app/isp_uartd

Serial ISP tuning daemon for the T23 firmware project.

## Why this app exists

The official ImageTool expects an IP address and a socket-based server, but the
current rebuild goal is to start ISP tuning before the C3 networking path is
finished.

This app provides a simpler path:

1. T23 initializes sensor + ISP + framesource + JPEG
2. T23 opens a UART device
3. A PC-side browser page talks to T23 over serial
4. ISP parameters are adjusted immediately through `IMP_ISP_Tuning_*` APIs

## Current protocol

The daemon accepts line-based ASCII commands terminated by `\n`.

Examples:

- `PING`
- `GET ALL`
- `GET BRIGHTNESS`
- `SET BRIGHTNESS 128`
- `SET AE_COMP 180`
- `SNAP`

Normal responses are text lines:

- `PONG`
- `VAL BRIGHTNESS 128`
- `OK SET BRIGHTNESS`
- `ERR unknown-parameter`

For snapshots:

1. the daemon sends `JPEG <length>\n`
2. the daemon sends raw JPEG bytes of exactly that length

## Recommended serial path

Current recommendation is to use the UART debug port, not the USB burn port.

- preferred bring-up path: COM3 / `/dev/ttyS1` class debug UART
- not recommended for MVP: COM8 burn/programming USB path

## Important caution

If the same UART is also used as the Linux console, kernel logs and login
prompts can interfere with the tuning protocol. For the cleanest result:

1. keep kernel logs quiet
2. avoid interactive login on that port during tuning
3. later consider moving the tuning daemon to a dedicated UART
