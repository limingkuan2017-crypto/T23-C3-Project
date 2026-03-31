#ifndef T23_C3_PROTOCOL_H
#define T23_C3_PROTOCOL_H

#include <stdint.h>

/*
 * Shared protocol between:
 * - T23 Linux userspace (SPI master)
 * - ESP32-C3 firmware (SPI slave + WiFi server)
 *
 * Transport model:
 * - one fixed-size SPI transaction always carries one frame from each side
 * - T23 data goes over MOSI
 * - C3 data goes over MISO
 * - C3 raises Data Ready when a transaction buffer is queued and ready
 */

#define T23_C3_PROTO_VERSION 0x0002u

#define T23_C3_FRAME_MAGIC0 0x54u /* 'T' */
#define T23_C3_FRAME_MAGIC1 0x43u /* 'C' */

#define T23_C3_SPI_FRAME_SIZE 1024u

typedef enum {
    T23_C3_FRAME_TYPE_NOP = 0x00,

    /* C3 -> T23 commands */
    T23_C3_FRAME_TYPE_CMD_PING = 0x01,
    T23_C3_FRAME_TYPE_CMD_GET_ALL = 0x02,
    T23_C3_FRAME_TYPE_CMD_GET_PARAM = 0x03,
    T23_C3_FRAME_TYPE_CMD_SET_PARAM = 0x04,
    T23_C3_FRAME_TYPE_CMD_SNAP = 0x05,

    /* T23 -> C3 responses */
    T23_C3_FRAME_TYPE_RESP_PONG = 0x40,
    T23_C3_FRAME_TYPE_RESP_PARAM = 0x41,
    T23_C3_FRAME_TYPE_RESP_DONE = 0x42,
    T23_C3_FRAME_TYPE_RESP_ERROR = 0x7f,
    T23_C3_FRAME_TYPE_RESP_JPEG_INFO = 0x50,
    T23_C3_FRAME_TYPE_RESP_JPEG_DATA = 0x51,
} t23_c3_frame_type_t;

typedef enum {
    T23_C3_PARAM_NONE = 0,
    T23_C3_PARAM_BRIGHTNESS = 1,
    T23_C3_PARAM_CONTRAST = 2,
    T23_C3_PARAM_SHARPNESS = 3,
    T23_C3_PARAM_SATURATION = 4,
    T23_C3_PARAM_AE_COMP = 5,
    T23_C3_PARAM_DPC = 6,
    T23_C3_PARAM_DRC = 7,
    T23_C3_PARAM_AWB_CT = 8,
} t23_c3_param_id_t;

typedef enum {
    T23_C3_STATUS_OK = 0,
    T23_C3_STATUS_PENDING = 1,
    T23_C3_STATUS_BAD_PARAM = 2,
    T23_C3_STATUS_BAD_RANGE = 3,
    T23_C3_STATUS_BUSY = 4,
    T23_C3_STATUS_INTERNAL = 5,
} t23_c3_status_t;

typedef struct __attribute__((packed)) {
    uint8_t magic0;
    uint8_t magic1;
    uint8_t version;
    uint8_t type;
    uint8_t param_id;
    uint8_t status;
    uint16_t seq;
    uint16_t payload_len;
    uint32_t total_len;
    uint32_t offset;
} t23_c3_frame_header_t;

#define T23_C3_FRAME_PAYLOAD_MAX \
    (T23_C3_SPI_FRAME_SIZE - (uint32_t)sizeof(t23_c3_frame_header_t))

typedef struct __attribute__((packed)) {
    t23_c3_frame_header_t hdr;
    uint8_t payload[T23_C3_FRAME_PAYLOAD_MAX];
} t23_c3_frame_t;

typedef struct __attribute__((packed)) {
    int32_t value;
} t23_c3_value_payload_t;

typedef struct __attribute__((packed)) {
    uint32_t jpeg_len;
} t23_c3_jpeg_info_payload_t;

/*
 * Stage-1 bring-up compatibility pattern.
 * Keeping the old constant makes it easier to compare old SPI diag logs.
 */
#define T23_C3_SPI_TEST_LEN 4u
#define T23_C3_SPI_TEST_RESP0 0x5Au
#define T23_C3_SPI_TEST_RESP1 0xA5u
#define T23_C3_SPI_TEST_RESP2 0xEEu
#define T23_C3_SPI_TEST_RESP3 0xDDu

#endif
