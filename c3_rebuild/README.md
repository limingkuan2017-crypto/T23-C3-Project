# c3_rebuild

ESP32-C3 rebuild project for the new T23N + C3 architecture.

## Current role

At the current stage, this project is intentionally small and deterministic.
It exists only to prove that the new T23<->C3 SPI wiring works.

The firmware currently:

- runs as an SPI slave
- drives `Data Ready` on IO3
- returns a fixed 4-byte pattern to T23

## Why it is separate

This project uses a different toolchain and different runtime model from T23.
Keeping it separate avoids mixing Linux userspace code with ESP-IDF firmware.

Shared definitions live in:

- `../t23_c3_shared`
- ESP-IDF is expected through `IDF_PATH` or the local default
  `~/.espressif/v5.4.3/esp-idf`

## Important files

- `main/main.c`
  SPI slave test firmware entry point
- `scripts/idf_build.sh`
  build helper
- `scripts/idf_flash.sh`
  automatic flash helper
- `scripts/manual_flash.sh`
  manual download-mode helper
- `docs/c3_runtime_flow.md`
  first document a new developer should read

## Build

```sh
cd <repo>/c3_rebuild
./scripts/idf_build.sh
```

## Flash

Automatic reset path:

```sh
cd <repo>/c3_rebuild
./scripts/idf_flash.sh /dev/ttyACM0
```

Manual boot/download path:

```sh
cd <repo>/c3_rebuild
./scripts/manual_flash.sh /dev/ttyACM0
```
