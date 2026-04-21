# t23_spi_diag

Minimal SPI host-side diagnostic tool for the T23 firmware project.

Purpose:
- verify `/dev/spidev0.0` exists and can be configured
- perform deterministic SPI transfers from T23
- optionally watch the new `Data Ready` GPIO as an input from ESP32-C3

Examples:

```sh
/system/bin/t23_spi_diag info
/system/bin/t23_spi_diag xfer a5 5a 00 ff
/system/bin/t23_spi_diag wait-dr 3000
/system/bin/t23_spi_diag xfer-dr a5 5a 11 22
```
