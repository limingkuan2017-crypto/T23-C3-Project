#ifndef T23_C3_PROTOCOL_H
#define T23_C3_PROTOCOL_H

#include <stdint.h>

/*
 * Shared header for both sides of the T23<->C3 link.
 *
 * Stage 1 is intentionally tiny: fixed-response SPI only. The enum and frame
 * header reserve the shape of the future request/response protocol so both
 * projects grow from the same source of truth.
 */
#define T23_C3_PROTO_VERSION 0x0001u

/*
 * Stage 1 bring-up:
 * ESP32-C3 always replies with a fixed 4-byte pattern so the T23 side
 * can verify full-duplex SPI and the wiring on MISO.
 */
#define T23_C3_SPI_TEST_LEN 4u
#define T23_C3_SPI_TEST_RESP0 0x5Au
#define T23_C3_SPI_TEST_RESP1 0xA5u
#define T23_C3_SPI_TEST_RESP2 0xEEu
#define T23_C3_SPI_TEST_RESP3 0xDDu

/*
 * Reserved frame types for the next stage once fixed-response SPI is stable.
 */
typedef enum {
    T23_C3_FRAME_TYPE_NOP = 0x00,
    T23_C3_FRAME_TYPE_PING = 0x01,
    T23_C3_FRAME_TYPE_ACK = 0x02,
    T23_C3_FRAME_TYPE_RGB_CHUNK = 0x10,
    T23_C3_FRAME_TYPE_JPEG_CHUNK = 0x11,
    T23_C3_FRAME_TYPE_WIFI_STATUS = 0x20,
    T23_C3_FRAME_TYPE_ERROR = 0x7F,
} t23_c3_frame_type_t;

typedef struct {
    uint8_t magic0;
    uint8_t magic1;
    uint8_t version;
    uint8_t type;
    uint16_t seq;
    uint16_t payload_len;
} t23_c3_frame_header_t;

#endif
