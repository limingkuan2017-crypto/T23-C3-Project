# Protocol Plan

## Stage 1: Fixed Response

Goal:
- prove that SPI is working in both directions
- prove that `Data Ready` is wired and readable by T23

Behavior:
- ESP32-C3 prepares a 4-byte response: `5A A5 EE DD`
- ESP32-C3 drives `Data Ready` high when the transaction buffer is ready
- T23 waits for `Data Ready`
- T23 performs a 4-byte SPI transfer
- ESP32-C3 lowers `Data Ready` after the transaction completes

Expected T23 command:

```sh
/system/bin/t23_spi_diag wait-dr 3000
/system/bin/t23_spi_diag xfer-dr 00 00 00 00
```

Expected T23 receive bytes:

```text
rx: 5a a5 ee dd
```

## Stage 2: Ping/Ack

Goal:
- verify deterministic request/response without image payloads

Plan:
- define a small fixed header
- carry `seq`
- return `ACK`

## Stage 3: Data Streaming

Goal:
- split RGB/JPEG payloads into chunks over SPI
- let C3 feed WiFi and LED logic independently

Plan:
- `JPEG_CHUNK` or `RGB_CHUNK`
- bounded payload length
- CRC or checksum on each frame
