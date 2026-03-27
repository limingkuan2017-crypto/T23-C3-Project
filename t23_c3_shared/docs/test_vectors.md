# Test Vectors

## T23 Only

```sh
/system/bin/t23_spi_diag info
/system/bin/t23_spi_diag read-dr
/system/bin/t23_spi_diag xfer a5 5a 00 ff
```

Expected:
- `info` succeeds
- `read-dr` is typically `0` before C3 firmware is active
- `xfer` may return all `ff` if no slave data is being driven

## T23 + C3 Fixed Response

```sh
/system/bin/t23_spi_diag wait-dr 3000
/system/bin/t23_spi_diag xfer-dr 00 00 00 00
```

Expected:

```text
data-ready: high
tx: 00 00 00 00
rx: 5a a5 ee dd
transfer-bytes: 4
```
