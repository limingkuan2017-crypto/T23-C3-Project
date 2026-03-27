# Pin Mapping

New hardware mapping for T23N <-> ESP32-C3:

| Signal | T23N | ESP32-C3 | Direction |
|---|---:|---:|---|
| UART0_RXD | pin 71 | IO19 | C3 -> T23 |
| UART0_TXD | pin 70 | IO18 | T23 -> C3 |
| SPI_MISO | pin 85 | IO8 | C3 -> T23 |
| SPI_CLK | pin 82 | IO6 | T23 -> C3 |
| SPI_CS | pin 81 | IO5 | T23 -> C3 |
| SPI_MOSI | pin 86 | IO10 | T23 -> C3 |
| Data Ready | pin 68 / gpio53 | IO3 | C3 -> T23 |

Notes:
- T23 acts as the SPI master.
- ESP32-C3 acts as the SPI slave.
- `Data Ready` is driven by ESP32-C3 and read by T23 as an input.
- The old vendor handshake/reset usage is not reused in this bring-up path.
