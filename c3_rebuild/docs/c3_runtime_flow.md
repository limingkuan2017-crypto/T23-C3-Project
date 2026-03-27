# C3 Runtime Flow

## Purpose

This document explains the current ESP32-C3 firmware used for SPI bring-up.

## Current Scope

The firmware does only one thing:

- acts as an SPI slave
- raises `Data Ready` when a transaction buffer is prepared
- returns a fixed 4-byte response to T23

This is intentional. WiFi, LED logic and protocol forwarding are postponed until
the T23<->C3 electrical link is proven stable.

## app_main Flow

1. Configure `Data Ready` as an output on IO3.
2. Configure the SPI slave pins:
   - MOSI IO10
   - MISO IO8
   - CLK IO6
   - CS IO5
3. Initialize the ESP-IDF SPI slave driver.
4. Allocate DMA-safe TX/RX buffers.
5. Queue a slave transaction with a fixed response:
   - `5A A5 EE DD`
6. Raise `Data Ready` in `post_setup_cb`.
7. Lower `Data Ready` in `post_trans_cb`.
8. Log the received 4 bytes after each transaction.

## Key Functions

- `post_setup_cb()`
  Signals to T23 that the slave buffer is ready.
- `post_trans_cb()`
  Clears the ready signal after the transfer is complete.
- `prepare_fixed_response()`
  Fills the transmit buffer with the known test pattern.
- `app_main()`
  Main firmware loop.

## Build and Flash Scripts

- `scripts/idf_env.sh`
  Pins the ESP-IDF environment.
- `scripts/idf_build.sh`
  Builds the project for `esp32c3`.
- `scripts/idf_flash.sh`
  For adapters that can control boot/reset automatically.
- `scripts/manual_flash.sh`
  For serial links that require pressing `BOOT` and `EN` manually.
