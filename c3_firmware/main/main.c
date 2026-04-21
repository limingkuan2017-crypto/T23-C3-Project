#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_slave.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "sm16703sp3.h"
#include "t23_border_pipeline.h"
#include "t23_c3_protocol.h"

#define TAG "c3_bridge"

#define WIFI_DEFAULT_SSID "MK"
#define WIFI_DEFAULT_PASS "12345678"
#define WIFI_AP_SSID "T23-C3-Setup"
#define WIFI_AP_PASS "12345678"
#define WIFI_SCAN_MAX_RESULTS 20
#define WIFI_SSID_MAX_LEN 32
#define WIFI_PASS_MAX_LEN 64

#define C3_SPI_HOST SPI2_HOST
#define PIN_NUM_MOSI 10
#define PIN_NUM_MISO 8
#define PIN_NUM_CLK 6
#define PIN_NUM_CS 5
#define PIN_NUM_DATA_READY 3

#define LED_POWER_EN_GPIO 1
#define LED_STRIP_GPIO 7
#define LED_STRIP_COUNT 50

#define T23_UART_PORT UART_NUM_1
#define T23_UART_TX 19
#define T23_UART_RX 18
#define T23_UART_BAUD 115200

#define UART_LINE_MAX 256
#define JPEG_MAX_SIZE (128 * 1024)
#define HIRES_JPEG_MAX_SIZE (256 * 1024)
#define JPEG_MIN_SIZE (32 * 1024)
#define PREVIEW_REFRESH_MS 200
#define ISP_SAVE_MAGIC 0x49535031u
#define LED_MAP_MAGIC 0x4c4d4150u
#define LED_MAP_VERSION 0x01u
#define RUN_SPI_POLL_TIMEOUT_MS 60

typedef struct {
    const char *name;
    t23_c3_param_id_t id;
} bridge_param_t;

typedef enum {
    C3_MODE_DEBUG = 0,
    C3_MODE_RUN = 1,
} c3_mode_t;

typedef struct {
    const char *name;
    int top_blocks;
    int right_blocks;
    int bottom_blocks;
    int left_blocks;
} border_layout_desc_t;

typedef enum {
    LED_INSTALL_LEFT_TOP = 0,
    LED_INSTALL_LEFT_BOTTOM = 1,
    LED_INSTALL_RIGHT_TOP = 2,
    LED_INSTALL_RIGHT_BOTTOM = 3,
} led_install_mode_t;

typedef enum {
    LED_STRIP_PROFILE_5M = 0,
    LED_STRIP_PROFILE_38M = 1,
} led_strip_profile_t;

typedef struct {
    const char *name;
    const char *label;
    sm16703sp3_color_order_t order;
} led_color_order_desc_t;

typedef enum {
    LOGICAL_EDGE_TOP = 0,
    LOGICAL_EDGE_RIGHT = 1,
    LOGICAL_EDGE_BOTTOM = 2,
    LOGICAL_EDGE_LEFT = 3,
} logical_edge_t;

typedef struct {
    logical_edge_t edge;
    int reverse;
} led_install_segment_t;

typedef struct {
    const char *name;
    const char *label;
    led_install_segment_t segments[4];
} led_install_desc_t;

typedef struct {
    const char *name;
    const char *label;
    int segment_lengths[4];
    int led_count;
} led_strip_profile_desc_t;

typedef struct {
    char layout_name[16];
    int block_count;
    int top_blocks;
    int right_blocks;
    int bottom_blocks;
    int left_blocks;
    int image_w;
    int image_h;
    int rect_left;
    int rect_top;
    int rect_right;
    int rect_bottom;
    int thickness;
    uint8_t colors[T23_BORDER_BLOCK_COUNT_MAX][3];
    uint8_t valid[T23_BORDER_BLOCK_COUNT_MAX];
    int ready;
} border_blocks_cache_t;

typedef struct {
    uint8_t rgb[T23_C3_PREVIEW_MOSAIC_RGB_LEN];
    int ready;
} preview_mosaic_cache_t;

typedef struct {
    uint8_t x;
    uint8_t y;
} led_map_cell_t;

typedef struct {
    uint32_t magic;
    uint8_t version;
    uint8_t top_count;
    uint8_t right_count;
    uint8_t bottom_count;
    uint8_t left_count;
    led_map_cell_t cells[LED_STRIP_COUNT];
} led_mapping_profile_t;

typedef struct {
    uint32_t magic;
    int32_t values[13];
} saved_isp_params_t;

typedef enum {
    LED_TEST_MODE_LIVE = 0,
    LED_TEST_MODE_WHITE = 1,
    LED_TEST_MODE_RED = 2,
    LED_TEST_MODE_GREEN = 3,
    LED_TEST_MODE_BLUE = 4,
    LED_TEST_MODE_CYAN = 5,
    LED_TEST_MODE_MAGENTA = 6,
    LED_TEST_MODE_YELLOW = 7,
    LED_TEST_MODE_CUSTOM = 8,
} led_test_mode_t;

static const bridge_param_t g_params[] = {
    { "BRIGHTNESS", T23_C3_PARAM_BRIGHTNESS },
    { "CONTRAST", T23_C3_PARAM_CONTRAST },
    { "SHARPNESS", T23_C3_PARAM_SHARPNESS },
    { "SATURATION", T23_C3_PARAM_SATURATION },
    { "AE_COMP", T23_C3_PARAM_AE_COMP },
    { "DRC", T23_C3_PARAM_DRC },
    { "AWB_CT", T23_C3_PARAM_AWB_CT },
    { "HUE", T23_C3_PARAM_HUE },
};

static SemaphoreHandle_t g_bridge_lock;
static uint8_t *g_spi_tx_buf;
static uint8_t *g_spi_rx_buf;
static uint8_t *g_latest_jpeg;
static size_t g_latest_jpeg_capacity;
static char g_latest_border_json[4096];
static int g_latest_border_json_valid;
static volatile c3_mode_t g_c3_mode = C3_MODE_DEBUG;
static int g_install_setup_active = 1;
static char g_wifi_sta_ssid[WIFI_SSID_MAX_LEN + 1];
static char g_wifi_sta_pass[WIFI_PASS_MAX_LEN + 1];
static char g_wifi_sta_ip[16];
static int g_wifi_sta_configured = 1;
static int g_wifi_sta_connected = 0;
static const border_layout_desc_t g_layout_16x9 = { "16X9", 16, 9, 16, 9 };
static const border_layout_desc_t g_layout_4x3 = { "4X3", 4, 3, 4, 3 };
static const border_layout_desc_t *g_current_layout = &g_layout_16x9;
static const led_strip_profile_desc_t g_strip_profile_5m = {
    "5M", "5m (9 short / 16 long)", { 9, 16, 9, 16 }, 50
};
static const led_strip_profile_desc_t g_strip_profile_38m = {
    "3P8M", "3.8m (7 short / 12 long)", { 7, 12, 7, 12 }, 38
};
static border_blocks_cache_t g_latest_blocks;
static preview_mosaic_cache_t g_latest_preview_mosaic;
static preview_mosaic_cache_t g_latest_runtime_mosaic;
static led_mapping_profile_t g_led_map_5m;
static led_mapping_profile_t g_led_map_38m;
static const led_install_desc_t g_install_left_top = {
    "LEFT_TOP", "Left short -> Top long",
    { { LOGICAL_EDGE_LEFT, 0 }, { LOGICAL_EDGE_TOP, 0 }, { LOGICAL_EDGE_RIGHT, 0 }, { LOGICAL_EDGE_BOTTOM, 0 } }
};
static const led_install_desc_t g_install_left_bottom = {
    "LEFT_BOTTOM", "Left short -> Bottom long",
    { { LOGICAL_EDGE_LEFT, 1 }, { LOGICAL_EDGE_BOTTOM, 1 }, { LOGICAL_EDGE_RIGHT, 1 }, { LOGICAL_EDGE_TOP, 1 } }
};
static const led_install_desc_t g_install_right_top = {
    "RIGHT_TOP", "Right short -> Top long",
    { { LOGICAL_EDGE_RIGHT, 1 }, { LOGICAL_EDGE_TOP, 1 }, { LOGICAL_EDGE_LEFT, 1 }, { LOGICAL_EDGE_BOTTOM, 1 } }
};
static const led_install_desc_t g_install_right_bottom = {
    "RIGHT_BOTTOM", "Right short -> Bottom long",
    { { LOGICAL_EDGE_RIGHT, 0 }, { LOGICAL_EDGE_BOTTOM, 0 }, { LOGICAL_EDGE_LEFT, 0 }, { LOGICAL_EDGE_TOP, 0 } }
};
static const led_install_desc_t *g_install_mode = &g_install_left_top;
static const led_color_order_desc_t g_color_order_rgb = { "RGB", "RGB", SM16703SP3_ORDER_RGB };
static const led_color_order_desc_t g_color_order_rbg = { "RBG", "RBG", SM16703SP3_ORDER_RBG };
static const led_color_order_desc_t g_color_order_grb = { "GRB", "GRB", SM16703SP3_ORDER_GRB };
static const led_color_order_desc_t g_color_order_gbr = { "GBR", "GBR", SM16703SP3_ORDER_GBR };
static const led_color_order_desc_t g_color_order_brg = { "BRG", "BRG", SM16703SP3_ORDER_BRG };
static const led_color_order_desc_t g_color_order_bgr = { "BGR", "BGR", SM16703SP3_ORDER_BGR };
static const led_color_order_desc_t *g_led_color_order = &g_color_order_rgb;
static const led_strip_profile_desc_t *g_led_strip_profile = &g_strip_profile_5m;
static saved_isp_params_t g_saved_isp_params;
static int g_saved_isp_params_valid = 0;
static saved_isp_params_t g_default_isp_params;
static int g_default_isp_params_valid = 0;
static led_test_mode_t g_led_test_mode = LED_TEST_MODE_LIVE;
static uint8_t g_led_custom_rgb[3] = { 139, 118, 58 };
static int g_led_power_enabled = 1;

static esp_err_t spi_receive_frame(t23_c3_frame_t *frame, int timeout_ms);
static esp_err_t wifi_apply_sta_config(void);
static esp_err_t show_install_guide_pattern(void);
static esp_err_t refresh_led_output_from_state(void);
static void set_common_headers(httpd_req_t *req);

static const char *c3_mode_name(c3_mode_t mode)
{
    return (mode == C3_MODE_RUN) ? "RUN" : "DEBUG";
}

static const led_strip_profile_desc_t *find_strip_profile_desc(const char *name)
{
    if (name == NULL) {
        return &g_strip_profile_5m;
    }
    if (strcmp(name, g_strip_profile_38m.name) == 0) {
        return &g_strip_profile_38m;
    }
    return &g_strip_profile_5m;
}

static const led_install_desc_t *find_install_mode_desc(const char *name)
{
    if (name == NULL) {
        return &g_install_left_top;
    }
    if (strcmp(name, g_install_left_bottom.name) == 0) {
        return &g_install_left_bottom;
    }
    if (strcmp(name, g_install_right_top.name) == 0) {
        return &g_install_right_top;
    }
    if (strcmp(name, g_install_right_bottom.name) == 0) {
        return &g_install_right_bottom;
    }
    return &g_install_left_top;
}

static uint8_t strip_profile_to_u8(const led_strip_profile_desc_t *desc)
{
    if (desc == &g_strip_profile_38m) {
        return (uint8_t)LED_STRIP_PROFILE_38M;
    }
    return (uint8_t)LED_STRIP_PROFILE_5M;
}

static const led_strip_profile_desc_t *strip_profile_from_u8(uint8_t value)
{
    switch (value) {
    case LED_STRIP_PROFILE_38M:
        return &g_strip_profile_38m;
    case LED_STRIP_PROFILE_5M:
    default:
        return &g_strip_profile_5m;
    }
}

static const led_color_order_desc_t *find_color_order_desc(const char *name)
{
    if (name == NULL) {
        return &g_color_order_rgb;
    }
    if (strcmp(name, g_color_order_rbg.name) == 0) {
        return &g_color_order_rbg;
    }
    if (strcmp(name, g_color_order_grb.name) == 0) {
        return &g_color_order_grb;
    }
    if (strcmp(name, g_color_order_gbr.name) == 0) {
        return &g_color_order_gbr;
    }
    if (strcmp(name, g_color_order_brg.name) == 0) {
        return &g_color_order_brg;
    }
    if (strcmp(name, g_color_order_bgr.name) == 0) {
        return &g_color_order_bgr;
    }
    return &g_color_order_rgb;
}

static uint8_t install_mode_to_u8(const led_install_desc_t *desc)
{
    if (desc == &g_install_left_bottom) {
        return (uint8_t)LED_INSTALL_LEFT_BOTTOM;
    }
    if (desc == &g_install_right_top) {
        return (uint8_t)LED_INSTALL_RIGHT_TOP;
    }
    if (desc == &g_install_right_bottom) {
        return (uint8_t)LED_INSTALL_RIGHT_BOTTOM;
    }
    return (uint8_t)LED_INSTALL_LEFT_TOP;
}

static const led_install_desc_t *install_mode_from_u8(uint8_t value)
{
    switch (value) {
    case LED_INSTALL_LEFT_BOTTOM:
        return &g_install_left_bottom;
    case LED_INSTALL_RIGHT_TOP:
        return &g_install_right_top;
    case LED_INSTALL_RIGHT_BOTTOM:
        return &g_install_right_bottom;
    case LED_INSTALL_LEFT_TOP:
    default:
        return &g_install_left_top;
    }
}

static uint8_t color_order_to_u8(const led_color_order_desc_t *desc)
{
    return (uint8_t)(desc ? desc->order : SM16703SP3_ORDER_RGB);
}

static const char *led_test_mode_name(led_test_mode_t mode)
{
    switch (mode) {
    case LED_TEST_MODE_CUSTOM:
        return "CUSTOM";
    case LED_TEST_MODE_WHITE:
        return "WHITE";
    case LED_TEST_MODE_RED:
        return "RED";
    case LED_TEST_MODE_GREEN:
        return "GREEN";
    case LED_TEST_MODE_BLUE:
        return "BLUE";
    case LED_TEST_MODE_CYAN:
        return "CYAN";
    case LED_TEST_MODE_MAGENTA:
        return "MAGENTA";
    case LED_TEST_MODE_YELLOW:
        return "YELLOW";
    case LED_TEST_MODE_LIVE:
    default:
        return "LIVE";
    }
}

static led_test_mode_t led_test_mode_from_name(const char *name)
{
    if (name == NULL) {
        return LED_TEST_MODE_LIVE;
    }
    if (strcmp(name, "CUSTOM") == 0) {
        return LED_TEST_MODE_CUSTOM;
    }
    if (strcmp(name, "WHITE") == 0) {
        return LED_TEST_MODE_WHITE;
    }
    if (strcmp(name, "RED") == 0) {
        return LED_TEST_MODE_RED;
    }
    if (strcmp(name, "GREEN") == 0) {
        return LED_TEST_MODE_GREEN;
    }
    if (strcmp(name, "BLUE") == 0) {
        return LED_TEST_MODE_BLUE;
    }
    if (strcmp(name, "CYAN") == 0) {
        return LED_TEST_MODE_CYAN;
    }
    if (strcmp(name, "MAGENTA") == 0) {
        return LED_TEST_MODE_MAGENTA;
    }
    if (strcmp(name, "YELLOW") == 0) {
        return LED_TEST_MODE_YELLOW;
    }
    return LED_TEST_MODE_LIVE;
}

static uint8_t clamp_u8(int value)
{
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return (uint8_t)value;
}

static const led_color_order_desc_t *color_order_from_u8(uint8_t value)
{
    switch ((sm16703sp3_color_order_t)value) {
    case SM16703SP3_ORDER_RBG:
        return &g_color_order_rbg;
    case SM16703SP3_ORDER_GRB:
        return &g_color_order_grb;
    case SM16703SP3_ORDER_GBR:
        return &g_color_order_gbr;
    case SM16703SP3_ORDER_BRG:
        return &g_color_order_brg;
    case SM16703SP3_ORDER_BGR:
        return &g_color_order_bgr;
    case SM16703SP3_ORDER_RGB:
    default:
        return &g_color_order_rgb;
    }
}

static void get_strip_logical_counts(const led_strip_profile_desc_t *strip,
                                     int *top_count,
                                     int *right_count,
                                     int *bottom_count,
                                     int *left_count)
{
    int short_count = g_strip_profile_5m.segment_lengths[0];
    int long_count = g_strip_profile_5m.segment_lengths[1];

    if (strip != NULL) {
        short_count = strip->segment_lengths[0];
        long_count = strip->segment_lengths[1];
    }

    *top_count = long_count;
    *right_count = short_count;
    *bottom_count = long_count;
    *left_count = short_count;
}

static led_mapping_profile_t *led_mapping_profile_from_strip(const led_strip_profile_desc_t *strip)
{
    if (strip == &g_strip_profile_38m) {
        return &g_led_map_38m;
    }
    return &g_led_map_5m;
}

static led_mapping_profile_t *led_mapping_profile_mut_from_name(const char *name)
{
    if (name != NULL && strcmp(name, g_strip_profile_38m.name) == 0) {
        return &g_led_map_38m;
    }
    return &g_led_map_5m;
}

static void default_mapping_cell_for_edge(logical_edge_t edge,
                                          int index,
                                          int count,
                                          led_map_cell_t *cell_out)
{
    int pos = 0;

    if (cell_out == NULL) {
        return;
    }
    if (count > 1) {
        pos = (index * (int)(T23_C3_PREVIEW_MOSAIC_WIDTH - 1)) / (count - 1);
    }

    switch (edge) {
    case LOGICAL_EDGE_TOP:
        cell_out->x = (uint8_t)pos;
        cell_out->y = 0;
        break;
    case LOGICAL_EDGE_RIGHT:
        cell_out->x = (uint8_t)(T23_C3_PREVIEW_MOSAIC_WIDTH - 1);
        cell_out->y = (uint8_t)pos;
        break;
    case LOGICAL_EDGE_BOTTOM:
        cell_out->x = (uint8_t)((T23_C3_PREVIEW_MOSAIC_WIDTH - 1) - pos);
        cell_out->y = (uint8_t)(T23_C3_PREVIEW_MOSAIC_HEIGHT - 1);
        break;
    case LOGICAL_EDGE_LEFT:
    default:
        cell_out->x = 0;
        cell_out->y = (uint8_t)((T23_C3_PREVIEW_MOSAIC_HEIGHT - 1) - pos);
        break;
    }
}

static void reset_led_mapping_profile(led_mapping_profile_t *profile, const led_strip_profile_desc_t *strip)
{
    int top_count = 0;
    int right_count = 0;
    int bottom_count = 0;
    int left_count = 0;
    int offset = 0;
    int i;

    if (profile == NULL) {
        return;
    }

    get_strip_logical_counts(strip, &top_count, &right_count, &bottom_count, &left_count);
    memset(profile, 0, sizeof(*profile));
    profile->magic = LED_MAP_MAGIC;
    profile->version = LED_MAP_VERSION;
    profile->top_count = (uint8_t)top_count;
    profile->right_count = (uint8_t)right_count;
    profile->bottom_count = (uint8_t)bottom_count;
    profile->left_count = (uint8_t)left_count;

    for (i = 0; i < top_count; ++i) {
        default_mapping_cell_for_edge(LOGICAL_EDGE_TOP, i, top_count, &profile->cells[offset + i]);
    }
    offset += top_count;
    for (i = 0; i < right_count; ++i) {
        default_mapping_cell_for_edge(LOGICAL_EDGE_RIGHT, i, right_count, &profile->cells[offset + i]);
    }
    offset += right_count;
    for (i = 0; i < bottom_count; ++i) {
        default_mapping_cell_for_edge(LOGICAL_EDGE_BOTTOM, i, bottom_count, &profile->cells[offset + i]);
    }
    offset += bottom_count;
    for (i = 0; i < left_count; ++i) {
        default_mapping_cell_for_edge(LOGICAL_EDGE_LEFT, i, left_count, &profile->cells[offset + i]);
    }
}

static int validate_led_mapping_profile(led_mapping_profile_t *profile, const led_strip_profile_desc_t *strip)
{
    int top_count = 0;
    int right_count = 0;
    int bottom_count = 0;
    int left_count = 0;
    int total = 0;
    int i;

    if (profile == NULL || strip == NULL) {
        return 0;
    }

    get_strip_logical_counts(strip, &top_count, &right_count, &bottom_count, &left_count);
    total = top_count + right_count + bottom_count + left_count;
    if (profile->magic != LED_MAP_MAGIC ||
        profile->version != LED_MAP_VERSION ||
        profile->top_count != top_count ||
        profile->right_count != right_count ||
        profile->bottom_count != bottom_count ||
        profile->left_count != left_count) {
        return 0;
    }

    for (i = 0; i < total; ++i) {
        if (profile->cells[i].x >= T23_C3_PREVIEW_MOSAIC_WIDTH ||
            profile->cells[i].y >= T23_C3_PREVIEW_MOSAIC_HEIGHT) {
            return 0;
        }
    }
    return 1;
}

static const char *led_mapping_nvs_key(const led_strip_profile_desc_t *strip)
{
    if (strip == &g_strip_profile_38m) {
        return "ledmap38m";
    }
    return "ledmap5m";
}

static void load_one_led_mapping_profile(nvs_handle_t nvs,
                                         led_mapping_profile_t *profile,
                                         const led_strip_profile_desc_t *strip)
{
    size_t blob_size = sizeof(*profile);

    reset_led_mapping_profile(profile, strip);
    if (nvs == 0) {
        return;
    }
    if (nvs_get_blob(nvs, led_mapping_nvs_key(strip), profile, &blob_size) != ESP_OK ||
        blob_size != sizeof(*profile) ||
        !validate_led_mapping_profile(profile, strip)) {
        reset_led_mapping_profile(profile, strip);
    }
}

static void load_led_mapping_profiles(void)
{
    nvs_handle_t nvs = 0;

    if (nvs_open("c3cfg", NVS_READONLY, &nvs) != ESP_OK) {
        load_one_led_mapping_profile(0, &g_led_map_5m, &g_strip_profile_5m);
        load_one_led_mapping_profile(0, &g_led_map_38m, &g_strip_profile_38m);
        return;
    }

    load_one_led_mapping_profile(nvs, &g_led_map_5m, &g_strip_profile_5m);
    load_one_led_mapping_profile(nvs, &g_led_map_38m, &g_strip_profile_38m);
    nvs_close(nvs);
}

static esp_err_t save_led_mapping_profile(const led_strip_profile_desc_t *strip)
{
    nvs_handle_t nvs = 0;
    const led_mapping_profile_t *profile = led_mapping_profile_from_strip(strip);
    esp_err_t ret;

    if (profile == NULL || strip == NULL || !validate_led_mapping_profile((led_mapping_profile_t *)profile, strip)) {
        return ESP_ERR_INVALID_ARG;
    }

    ret = nvs_open("c3cfg", NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_blob(nvs, led_mapping_nvs_key(strip), profile, sizeof(*profile));
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
}

static void load_install_config(void)
{
    nvs_handle_t nvs = 0;
    uint8_t mode_value = (uint8_t)LED_INSTALL_LEFT_TOP;
    uint8_t active = 1;
    uint8_t color_order = (uint8_t)SM16703SP3_ORDER_RGB;
    uint8_t strip_profile = (uint8_t)LED_STRIP_PROFILE_5M;

    if (nvs_open("c3cfg", NVS_READONLY, &nvs) != ESP_OK) {
        g_install_mode = &g_install_left_top;
        g_install_setup_active = 1;
        return;
    }

    if (nvs_get_u8(nvs, "install_mode", &mode_value) == ESP_OK) {
        g_install_mode = install_mode_from_u8(mode_value);
    } else {
        g_install_mode = &g_install_left_top;
    }

    if (nvs_get_u8(nvs, "install_active", &active) == ESP_OK) {
        g_install_setup_active = active ? 1 : 0;
    } else {
        g_install_setup_active = 1;
    }

    if (nvs_get_u8(nvs, "led_rgb_order", &color_order) == ESP_OK) {
        g_led_color_order = color_order_from_u8(color_order);
    } else {
        g_led_color_order = &g_color_order_rgb;
    }

    if (nvs_get_u8(nvs, "led_strip_pf", &strip_profile) == ESP_OK) {
        g_led_strip_profile = strip_profile_from_u8(strip_profile);
    } else {
        g_led_strip_profile = &g_strip_profile_5m;
    }

    nvs_close(nvs);
}

static esp_err_t save_install_config(void)
{
    nvs_handle_t nvs = 0;
    esp_err_t ret = nvs_open("c3cfg", NVS_READWRITE, &nvs);

    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_u8(nvs, "install_mode", install_mode_to_u8(g_install_mode));
    if (ret == ESP_OK) {
        ret = nvs_set_u8(nvs, "install_active", g_install_setup_active ? 1 : 0);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_u8(nvs, "led_rgb_order", color_order_to_u8(g_led_color_order));
    }
    if (ret == ESP_OK) {
        ret = nvs_set_u8(nvs, "led_strip_pf", strip_profile_to_u8(g_led_strip_profile));
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }

    nvs_close(nvs);
    return ret;
}

static void load_wifi_config(void)
{
    nvs_handle_t nvs = 0;
    size_t len = 0;
    uint8_t configured = 1;

    snprintf(g_wifi_sta_ssid, sizeof(g_wifi_sta_ssid), "%s", WIFI_DEFAULT_SSID);
    snprintf(g_wifi_sta_pass, sizeof(g_wifi_sta_pass), "%s", WIFI_DEFAULT_PASS);
    g_wifi_sta_configured = 1;

    if (nvs_open("c3cfg", NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }

    len = sizeof(g_wifi_sta_ssid);
    if (nvs_get_str(nvs, "wifi_ssid", g_wifi_sta_ssid, &len) != ESP_OK) {
        snprintf(g_wifi_sta_ssid, sizeof(g_wifi_sta_ssid), "%s", WIFI_DEFAULT_SSID);
    }

    len = sizeof(g_wifi_sta_pass);
    if (nvs_get_str(nvs, "wifi_pass", g_wifi_sta_pass, &len) != ESP_OK) {
        snprintf(g_wifi_sta_pass, sizeof(g_wifi_sta_pass), "%s", WIFI_DEFAULT_PASS);
    }

    if (nvs_get_u8(nvs, "wifi_cfg", &configured) == ESP_OK) {
        g_wifi_sta_configured = configured ? 1 : 0;
    }

    nvs_close(nvs);
}

static esp_err_t save_wifi_config(void)
{
    nvs_handle_t nvs = 0;
    esp_err_t ret = nvs_open("c3cfg", NVS_READWRITE, &nvs);

    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_str(nvs, "wifi_ssid", g_wifi_sta_ssid);
    if (ret == ESP_OK) {
        ret = nvs_set_str(nvs, "wifi_pass", g_wifi_sta_pass);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_u8(nvs, "wifi_cfg", g_wifi_sta_configured ? 1 : 0);
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }

    nvs_close(nvs);
    return ret;
}

static void load_isp_params_blob(const char *key, saved_isp_params_t *params, int *valid_flag)
{
    nvs_handle_t nvs = 0;
    size_t len = sizeof(*params);

    memset(params, 0, sizeof(*params));
    *valid_flag = 0;

    if (nvs_open("c3cfg", NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }

    if (nvs_get_blob(nvs, key, params, &len) == ESP_OK &&
        len == sizeof(*params) &&
        params->magic == ISP_SAVE_MAGIC) {
        *valid_flag = 1;
    }

    nvs_close(nvs);
}

static esp_err_t save_isp_params_blob(const char *key, const saved_isp_params_t *params)
{
    nvs_handle_t nvs = 0;
    esp_err_t ret = nvs_open("c3cfg", NVS_READWRITE, &nvs);

    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_blob(nvs, key, params, sizeof(*params));
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }

    nvs_close(nvs);
    return ret;
}

static void load_saved_isp_params(void)
{
    load_isp_params_blob("isp_params", &g_saved_isp_params, &g_saved_isp_params_valid);
}

static void load_default_isp_params(void)
{
    load_isp_params_blob("isp_defaults", &g_default_isp_params, &g_default_isp_params_valid);
}

static esp_err_t save_saved_isp_params(void)
{
    return save_isp_params_blob("isp_params", &g_saved_isp_params);
}

static esp_err_t save_default_isp_params(void)
{
    return save_isp_params_blob("isp_defaults", &g_default_isp_params);
}

static const border_layout_desc_t *find_layout_desc(const char *name)
{
    if (name != NULL && strcmp(name, g_layout_4x3.name) == 0) {
        return &g_layout_4x3;
    }
    return &g_layout_16x9;
}

static void init_led_power_enable(void)
{
    const gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << LED_POWER_EN_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&io_conf));
    ESP_ERROR_CHECK(gpio_set_level(LED_POWER_EN_GPIO, 1));
    g_led_power_enabled = 1;
    ESP_LOGI(TAG, "LED power enabled on GPIO%d", LED_POWER_EN_GPIO);
}

static void set_led_power_enabled(int enabled)
{
    if (!enabled) {
        (void)sm16703sp3_clear();
    }
    g_led_power_enabled = enabled ? 1 : 0;
    gpio_set_level(LED_POWER_EN_GPIO, g_led_power_enabled);
}

static esp_err_t show_install_guide_pattern(void);
static void refresh_led_strip_from_latest_blocks_if_allowed(void);

static void reset_blocks_cache(border_blocks_cache_t *cache)
{
    memset(cache, 0, sizeof(*cache));
    snprintf(cache->layout_name, sizeof(cache->layout_name), "%s", g_layout_16x9.name);
    cache->top_blocks = g_layout_16x9.top_blocks;
    cache->right_blocks = g_layout_16x9.right_blocks;
    cache->bottom_blocks = g_layout_16x9.bottom_blocks;
    cache->left_blocks = g_layout_16x9.left_blocks;
    cache->block_count = cache->top_blocks + cache->right_blocks + cache->bottom_blocks + cache->left_blocks;
}

/*
 * Copy the latest block result into a single in-memory cache used by both the
 * web UI and the LED runtime path. Centralizing this copy keeps DEBUG and RUN
 * modes consistent.
 */
static void store_latest_blocks(const char *layout_name,
                                int block_count,
                                int top_blocks,
                                int right_blocks,
                                int bottom_blocks,
                                int left_blocks,
                                int image_w,
                                int image_h,
                                int rect_left,
                                int rect_top,
                                int rect_right,
                                int rect_bottom,
                                int thickness,
                                const int blocks[T23_BORDER_BLOCK_COUNT_MAX][3],
                                const int got_blocks[T23_BORDER_BLOCK_COUNT_MAX])
{
    int i;

    reset_blocks_cache(&g_latest_blocks);
    snprintf(g_latest_blocks.layout_name, sizeof(g_latest_blocks.layout_name), "%s", layout_name ? layout_name : g_layout_16x9.name);
    g_latest_blocks.block_count = block_count;
    g_latest_blocks.top_blocks = top_blocks;
    g_latest_blocks.right_blocks = right_blocks;
    g_latest_blocks.bottom_blocks = bottom_blocks;
    g_latest_blocks.left_blocks = left_blocks;
    g_latest_blocks.image_w = image_w;
    g_latest_blocks.image_h = image_h;
    g_latest_blocks.rect_left = rect_left;
    g_latest_blocks.rect_top = rect_top;
    g_latest_blocks.rect_right = rect_right;
    g_latest_blocks.rect_bottom = rect_bottom;
    g_latest_blocks.thickness = thickness;

    for (i = 0; i < block_count && i < (int)T23_BORDER_BLOCK_COUNT_MAX; ++i) {
        g_latest_blocks.colors[i][0] = got_blocks[i] ? (uint8_t)blocks[i][0] : 0;
        g_latest_blocks.colors[i][1] = got_blocks[i] ? (uint8_t)blocks[i][1] : 0;
        g_latest_blocks.colors[i][2] = got_blocks[i] ? (uint8_t)blocks[i][2] : 0;
        g_latest_blocks.valid[i] = got_blocks[i] ? 1 : 0;
    }
    g_latest_blocks.ready = 1;
    refresh_led_strip_from_latest_blocks_if_allowed();
}

static esp_err_t send_preview_mosaic_json(httpd_req_t *req, const preview_mosaic_cache_t *cache)
{
    char chunk[96];
    unsigned int i;
    int written;

    if (cache == NULL || !cache->ready) {
        return ESP_ERR_INVALID_STATE;
    }

    set_common_headers(req);
    httpd_resp_set_type(req, "application/json");

    written = snprintf(chunk,
                       sizeof(chunk),
                       "{\"ok\":true,\"width\":%u,\"height\":%u,\"pixels\":[",
                       (unsigned int)T23_C3_PREVIEW_MOSAIC_WIDTH,
                       (unsigned int)T23_C3_PREVIEW_MOSAIC_HEIGHT);
    if (written < 0 || written >= (int)sizeof(chunk)) {
        return ESP_FAIL;
    }
    if (httpd_resp_send_chunk(req, chunk, HTTPD_RESP_USE_STRLEN) != ESP_OK) {
        return ESP_FAIL;
    }

    for (i = 0; i < T23_C3_PREVIEW_MOSAIC_RGB_LEN; ++i) {
        written = snprintf(chunk,
                           sizeof(chunk),
                           "%s%u",
                           (i == 0u) ? "" : ",",
                           (unsigned int)cache->rgb[i]);
        if (written < 0 || written >= (int)sizeof(chunk)) {
            return ESP_FAIL;
        }
        if (httpd_resp_send_chunk(req, chunk, HTTPD_RESP_USE_STRLEN) != ESP_OK) {
            return ESP_FAIL;
        }
    }

    if (httpd_resp_send_chunk(req, "]}", 2) != ESP_OK) {
        return ESP_FAIL;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static void get_mapping_edge_span(const led_mapping_profile_t *profile,
                                  logical_edge_t edge,
                                  int *src_offset,
                                  int *src_count)
{
    int top_count = profile != NULL ? profile->top_count : 0;
    int right_count = profile != NULL ? profile->right_count : 0;
    int bottom_count = profile != NULL ? profile->bottom_count : 0;
    int left_count = profile != NULL ? profile->left_count : 0;

    switch (edge) {
    case LOGICAL_EDGE_TOP:
        *src_offset = 0;
        *src_count = top_count;
        break;
    case LOGICAL_EDGE_RIGHT:
        *src_offset = top_count;
        *src_count = right_count;
        break;
    case LOGICAL_EDGE_BOTTOM:
        *src_offset = top_count + right_count;
        *src_count = bottom_count;
        break;
    case LOGICAL_EDGE_LEFT:
    default:
        *src_offset = top_count + right_count + bottom_count;
        *src_count = left_count;
        break;
    }
}

static void sample_mosaic_rgb(const preview_mosaic_cache_t *mosaic,
                              int x,
                              int y,
                              uint8_t *r,
                              uint8_t *g,
                              uint8_t *b)
{
    int idx = 0;

    *r = 0;
    *g = 0;
    *b = 0;
    if (mosaic == NULL || !mosaic->ready ||
        x < 0 || x >= (int)T23_C3_PREVIEW_MOSAIC_WIDTH ||
        y < 0 || y >= (int)T23_C3_PREVIEW_MOSAIC_HEIGHT) {
        return;
    }

    idx = (y * (int)T23_C3_PREVIEW_MOSAIC_WIDTH + x) * 3;
    *r = mosaic->rgb[idx + 0];
    *g = mosaic->rgb[idx + 1];
    *b = mosaic->rgb[idx + 2];
}

static const preview_mosaic_cache_t *active_live_mosaic_cache(void)
{
    if (g_c3_mode == C3_MODE_RUN && g_latest_runtime_mosaic.ready) {
        return &g_latest_runtime_mosaic;
    }
    if (g_latest_preview_mosaic.ready) {
        return &g_latest_preview_mosaic;
    }
    if (g_latest_runtime_mosaic.ready) {
        return &g_latest_runtime_mosaic;
    }
    return NULL;
}

static void expand_logical_segment(uint8_t *dst_led_rgb,
                                   int dst_offset,
                                   int dst_count,
                                   const uint8_t *src_logical_rgb,
                                   int src_offset,
                                   int src_count,
                                   int reverse)
{
    int i;

    if (dst_count <= 0 || src_count <= 0) {
        return;
    }

    for (i = 0; i < dst_count; ++i) {
        int src_i = (i * src_count) / dst_count;
        int logical_i = reverse ? (src_count - 1 - src_i) : src_i;
        int dst_base = (dst_offset + i) * 3;
        int src_base = (src_offset + logical_i) * 3;

        dst_led_rgb[dst_base + 0] = src_logical_rgb[src_base + 0];
        dst_led_rgb[dst_base + 1] = src_logical_rgb[src_base + 1];
        dst_led_rgb[dst_base + 2] = src_logical_rgb[src_base + 2];
    }
}

typedef struct {
    uint8_t pixel_span;
    uint8_t ic_span;
} led_region_group_t;

typedef struct {
    uint8_t group_count;
    led_region_group_t groups[6];
} led_edge_region_plan_t;

typedef struct {
    led_edge_region_plan_t horizontal;
    led_edge_region_plan_t vertical;
} led_strip_region_plan_t;

static const led_strip_region_plan_t g_region_plan_5m = {
    .horizontal = {
        6,
        {
            { 2, 2 }, { 2, 2 }, { 4, 4 }, { 4, 4 }, { 2, 2 }, { 2, 2 }
        }
    },
    .vertical = {
        5,
        {
            { 2, 1 }, { 3, 2 }, { 6, 3 }, { 3, 2 }, { 2, 1 }, { 0, 0 }
        }
    }
};

static const led_strip_region_plan_t g_region_plan_38m = {
    .horizontal = {
        6,
        {
            { 2, 1 }, { 2, 2 }, { 4, 3 }, { 4, 3 }, { 2, 2 }, { 2, 1 }
        }
    },
    .vertical = {
        5,
        {
            { 2, 1 }, { 3, 1 }, { 6, 3 }, { 3, 1 }, { 2, 1 }, { 0, 0 }
        }
    }
};

static const led_strip_region_plan_t *region_plan_for_strip(const led_strip_profile_desc_t *strip)
{
    if (strip == &g_strip_profile_38m) {
        return &g_region_plan_38m;
    }
    return &g_region_plan_5m;
}

static void accumulate_rect_rgb(const preview_mosaic_cache_t *mosaic,
                                uint32_t *sum_r,
                                uint32_t *sum_g,
                                uint32_t *sum_b,
                                uint32_t *count,
                                int x0,
                                int y0,
                                int w,
                                int h)
{
    int x;
    int y;

    if (w <= 0 || h <= 0) {
        return;
    }
    for (y = y0; y < y0 + h; ++y) {
        for (x = x0; x < x0 + w; ++x) {
            uint8_t r = 0;
            uint8_t g = 0;
            uint8_t b = 0;

            sample_mosaic_rgb(mosaic, x, y, &r, &g, &b);
            *sum_r += r;
            *sum_g += g;
            *sum_b += b;
            *count += 1;
        }
    }
}

static void fill_edge_group_broadcast(uint8_t *logical_rgb,
                                      int logical_offset,
                                      int logical_count,
                                      const led_edge_region_plan_t *plan,
                                      int is_horizontal,
                                      int is_bottom_or_left,
                                      const preview_mosaic_cache_t *mosaic)
{
    int pixel_offset = 0;
    int logical_cursor = is_bottom_or_left ? logical_count : 0;
    int group_i;

    if (logical_rgb == NULL || plan == NULL || mosaic == NULL || !mosaic->ready) {
        return;
    }

    for (group_i = 0; group_i < plan->group_count; ++group_i) {
        const led_region_group_t *group = &plan->groups[group_i];
        uint32_t sum_r = 0;
        uint32_t sum_g = 0;
        uint32_t sum_b = 0;
        uint32_t count = 0;
        uint8_t out_r = 0;
        uint8_t out_g = 0;
        uint8_t out_b = 0;
        int inner_start = 0;
        int inner_end = 0;
        int ic_i;

        if (group->pixel_span == 0 || group->ic_span == 0) {
            continue;
        }

        if (is_horizontal) {
            accumulate_rect_rgb(mosaic, &sum_r, &sum_g, &sum_b, &count,
                                pixel_offset,
                                is_bottom_or_left ? ((int)T23_C3_PREVIEW_MOSAIC_HEIGHT - 1) : 0,
                                group->pixel_span,
                                1);
            inner_start = pixel_offset < 1 ? 1 : pixel_offset;
            inner_end = (pixel_offset + group->pixel_span - 1) > ((int)T23_C3_PREVIEW_MOSAIC_WIDTH - 2)
                            ? ((int)T23_C3_PREVIEW_MOSAIC_WIDTH - 2)
                            : (pixel_offset + group->pixel_span - 1);
            if (inner_end >= inner_start) {
                accumulate_rect_rgb(mosaic, &sum_r, &sum_g, &sum_b, &count,
                                    inner_start,
                                    is_bottom_or_left ? ((int)T23_C3_PREVIEW_MOSAIC_HEIGHT - 2) : 1,
                                    inner_end - inner_start + 1,
                                    1);
            }
        } else {
            accumulate_rect_rgb(mosaic, &sum_r, &sum_g, &sum_b, &count,
                                is_bottom_or_left ? 0 : ((int)T23_C3_PREVIEW_MOSAIC_WIDTH - 1),
                                pixel_offset,
                                1,
                                group->pixel_span);
            inner_start = pixel_offset < 1 ? 1 : pixel_offset;
            inner_end = (pixel_offset + group->pixel_span - 1) > ((int)T23_C3_PREVIEW_MOSAIC_HEIGHT - 2)
                            ? ((int)T23_C3_PREVIEW_MOSAIC_HEIGHT - 2)
                            : (pixel_offset + group->pixel_span - 1);
            if (inner_end >= inner_start) {
                accumulate_rect_rgb(mosaic, &sum_r, &sum_g, &sum_b, &count,
                                    is_bottom_or_left ? 1 : ((int)T23_C3_PREVIEW_MOSAIC_WIDTH - 2),
                                    inner_start,
                                    1,
                                    inner_end - inner_start + 1);
            }
        }

        if (count > 0) {
            out_r = (uint8_t)(sum_r / count);
            out_g = (uint8_t)(sum_g / count);
            out_b = (uint8_t)(sum_b / count);
        }

        if (is_bottom_or_left) {
            logical_cursor -= group->ic_span;
        }
        for (ic_i = 0; ic_i < group->ic_span; ++ic_i) {
            int logical_index = logical_offset + logical_cursor + ic_i;

            if (logical_index < 0 || logical_index >= LED_STRIP_COUNT) {
                continue;
            }
            logical_rgb[logical_index * 3 + 0] = out_r;
            logical_rgb[logical_index * 3 + 1] = out_g;
            logical_rgb[logical_index * 3 + 2] = out_b;
        }
        if (!is_bottom_or_left) {
            logical_cursor += group->ic_span;
        }
        pixel_offset += group->pixel_span;
    }
}

/*
 * Unified weighted 22-region mapping:
 * - Always sample the same 22 logical regions from the 16x16 mosaic.
 * - Long edges use 6 weighted regions with pixel spans 2/2/4/4/2/2.
 * - Short edges use 5 weighted regions with pixel spans 2/3/6/3/2.
 * - 5m broadcasts those regions to 16/9 physical ICs with 2/2/4/4/2/2 and
 *   1/2/3/2/1 allocation.
 * - 3.8m broadcasts the same 22 regions to 12/7 physical ICs with
 *   1/2/3/3/2/1 and 1/1/3/1/1 allocation.
 *
 * Sampling still uses a thin 2-pixel border band, but now each region average
 * is copied to the whole IC range for the active strip profile.
 */
static void build_fixed_logical_rgb_from_mosaic(const preview_mosaic_cache_t *mosaic,
                                                const led_strip_profile_desc_t *strip,
                                                uint8_t logical_rgb[LED_STRIP_COUNT * 3])
{
    const led_strip_region_plan_t *plan = region_plan_for_strip(strip);
    int top_count = 0;
    int right_count = 0;
    int bottom_count = 0;
    int left_count = 0;
    int top_offset = 0;
    int right_offset = 0;
    int bottom_offset = 0;
    int left_offset = 0;

    memset(logical_rgb, 0, LED_STRIP_COUNT * 3);
    if (mosaic == NULL || !mosaic->ready || strip == NULL || plan == NULL) {
        return;
    }

    get_strip_logical_counts(strip, &top_count, &right_count, &bottom_count, &left_count);
    right_offset = top_count;
    bottom_offset = top_count + right_count;
    left_offset = top_count + right_count + bottom_count;

    fill_edge_group_broadcast(logical_rgb, top_offset, top_count, &plan->horizontal, 1, 0, mosaic);
    fill_edge_group_broadcast(logical_rgb, right_offset, right_count, &plan->vertical, 0, 0, mosaic);
    fill_edge_group_broadcast(logical_rgb, bottom_offset, bottom_count, &plan->horizontal, 1, 1, mosaic);
    fill_edge_group_broadcast(logical_rgb, left_offset, left_count, &plan->vertical, 0, 1, mosaic);
}

static esp_err_t update_led_strip_from_mosaic(const preview_mosaic_cache_t *mosaic)
{
    uint8_t logical_rgb[LED_STRIP_COUNT * 3];
    uint8_t led_rgb[LED_STRIP_COUNT * 3];
    int dst_offset = 0;
    int seg;

    if (!g_led_power_enabled) {
        return ESP_OK;
    }
    if (mosaic == NULL || !mosaic->ready) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(led_rgb, 0, sizeof(led_rgb));
    build_fixed_logical_rgb_from_mosaic(mosaic, g_led_strip_profile, logical_rgb);

    for (seg = 0; seg < 4; ++seg) {
        int src_offset = 0;
        int src_count = 0;
        int dst_count = g_led_strip_profile->segment_lengths[seg];

        get_mapping_edge_span(led_mapping_profile_from_strip(g_led_strip_profile),
                              g_install_mode->segments[seg].edge,
                              &src_offset,
                              &src_count);
        expand_logical_segment(led_rgb,
                               dst_offset,
                               dst_count,
                               logical_rgb,
                               src_offset,
                               src_count,
                               g_install_mode->segments[seg].reverse);
        dst_offset += dst_count;
    }

    return sm16703sp3_show_rgb(led_rgb, LED_STRIP_COUNT);
}

static esp_err_t show_led_test_pattern(led_test_mode_t mode)
{
    uint8_t led_rgb[LED_STRIP_COUNT * 3];
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    int i;

    if (!g_led_power_enabled) {
        return ESP_OK;
    }

    switch (mode) {
    case LED_TEST_MODE_CUSTOM:
        r = g_led_custom_rgb[0];
        g = g_led_custom_rgb[1];
        b = g_led_custom_rgb[2];
        break;
    case LED_TEST_MODE_WHITE:
        r = 255;
        g = 255;
        b = 255;
        break;
    case LED_TEST_MODE_RED:
        r = 255;
        break;
    case LED_TEST_MODE_GREEN:
        g = 255;
        break;
    case LED_TEST_MODE_BLUE:
        b = 255;
        break;
    case LED_TEST_MODE_CYAN:
        g = 255;
        b = 255;
        break;
    case LED_TEST_MODE_MAGENTA:
        r = 255;
        b = 255;
        break;
    case LED_TEST_MODE_YELLOW:
        r = 255;
        g = 255;
        break;
    case LED_TEST_MODE_LIVE:
    default:
        return ESP_ERR_INVALID_STATE;
    }

    for (i = 0; i < LED_STRIP_COUNT; ++i) {
        led_rgb[i * 3 + 0] = r;
        led_rgb[i * 3 + 1] = g;
        led_rgb[i * 3 + 2] = b;
    }
    return sm16703sp3_show_rgb(led_rgb, LED_STRIP_COUNT);
}

static esp_err_t refresh_led_output_from_state(void)
{
    const preview_mosaic_cache_t *mosaic = NULL;

    if (!g_led_power_enabled) {
        return ESP_OK;
    }
    if (g_install_setup_active) {
        return show_install_guide_pattern();
    }
    if (g_led_test_mode != LED_TEST_MODE_LIVE) {
        return show_led_test_pattern(g_led_test_mode);
    }
    mosaic = active_live_mosaic_cache();
    if (mosaic != NULL) {
        return update_led_strip_from_mosaic(mosaic);
    }
    return sm16703sp3_clear();
}

static void refresh_led_strip_from_latest_blocks_if_allowed(void)
{
    if (!g_led_power_enabled) {
        return;
    }
    if (g_c3_mode != C3_MODE_DEBUG) {
        return;
    }

    if (refresh_led_output_from_state() != ESP_OK) {
        ESP_LOGW(TAG, "DEBUG LED refresh skipped");
    }
}

static esp_err_t show_install_guide_pattern(void)
{
    uint8_t led_rgb[LED_STRIP_COUNT * 3];
    int dst_offset = 0;
    int seg;

    if (!g_led_power_enabled) {
        return ESP_OK;
    }

    memset(led_rgb, 0, sizeof(led_rgb));
    for (seg = 0; seg < 4; ++seg) {
        uint8_t r = 0;
        uint8_t g = 0;
        uint8_t b = 0;
        int segment_length = g_led_strip_profile->segment_lengths[seg];
        int i;

        /* Highlight only the selected installation start pair:
         * segment 0 = short edge (blue), segment 1 = following long edge (red).
         * Remaining segments stay off so customers can identify orientation quickly.
         */
        if (seg == 0) {
            b = 255;
        } else if (seg == 1) {
            r = 255;
        }

        for (i = 0; i < segment_length; ++i) {
            led_rgb[(dst_offset + i) * 3 + 0] = r;
            led_rgb[(dst_offset + i) * 3 + 1] = g;
            led_rgb[(dst_offset + i) * 3 + 2] = b;
        }
        dst_offset += segment_length;
    }
    ESP_LOGI(TAG,
             "LED install guide: selected short edge blue, selected long edge red (%s, %s)",
             g_install_mode->name,
             g_led_strip_profile->name);
    return sm16703sp3_show_rgb(led_rgb, LED_STRIP_COUNT);
}

static const char g_index_html[] =
    "<!doctype html>\n"
    "<html lang=\"en\">\n"
    "<head>\n"
    "  <meta charset=\"utf-8\">\n"
    "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
    "  <title>T23 ISP WiFi Tuner</title>\n"
    "  <link rel=\"stylesheet\" href=\"/styles.css\">\n"
    "</head>\n"
    "<body>\n"
    "  <main class=\"layout\">\n"
    "    <section class=\"panel panel--hero\">\n"
    "      <div class=\"hero-copy\">\n"
    "        <p class=\"eyebrow\">T23 ISP Tuning</p>\n"
    "        <h1>WiFi Web Tuner</h1>\n"
    "        <p class=\"subtitle\">This page is served by the ESP32-C3. Control commands go to T23 over UART, and JPEG snapshots come back over SPI.</p>\n"
    "      </div>\n"
    "      <div class=\"hero-panels\">\n"
    "        <div class=\"wifi-card\">\n"
    "          <div class=\"section-header section-header--tight\"><h3>WiFi</h3><span id=\"wifiStatusBadge\" class=\"preview-status\">Loading WiFi...</span></div>\n"
    "          <p id=\"wifiHint\" class=\"subtitle\">Fallback AP: " WIFI_AP_SSID " / " WIFI_AP_PASS "</p>\n"
    "          <div class=\"button-row\">\n"
    "            <label for=\"wifiScanSelect\">Nearby WiFi</label>\n"
    "            <select id=\"wifiScanSelect\"></select>\n"
    "            <button id=\"wifiScanBtn\">Scan WiFi</button>\n"
    "          </div>\n"
    "          <div class=\"button-row\">\n"
    "            <label for=\"wifiSsidInput\">SSID</label>\n"
    "            <input id=\"wifiSsidInput\" type=\"text\" maxlength=\"32\" placeholder=\"Select or enter SSID\">\n"
    "            <label for=\"wifiPassInput\">Key</label>\n"
    "            <input id=\"wifiPassInput\" type=\"password\" maxlength=\"64\" placeholder=\"Enter password\">\n"
    "          </div>\n"
    "          <div class=\"button-row\">\n"
    "            <button id=\"wifiSaveBtn\">Save WiFi</button>\n"
    "            <button id=\"wifiForgetBtn\">Forget WiFi</button>\n"
    "          </div>\n"
    "        </div>\n"
    "        <div class=\"connection-card\">\n"
    "          <div class=\"status-row\"><span class=\"status-dot connected\"></span><span id=\"statusText\">Connected to C3 bridge</span><span id=\"modeBadge\" class=\"preview-status\">Mode: DEBUG</span></div>\n"
    "          <div class=\"button-row\">\n"
    "          <label for=\"stripProfileSelect\">LED Strip</label>\n"
    "          <select id=\"stripProfileSelect\">\n"
    "            <option value=\"5M\">5m</option>\n"
    "            <option value=\"3P8M\">3.8m</option>\n"
    "          </select>\n"
    "          </div>\n"
    "          <div class=\"button-row\">\n"
    "          <label for=\"installModeSelect\">LED Install</label>\n"
    "          <select id=\"installModeSelect\">\n"
    "            <option value=\"LEFT_TOP\">Left short -> Top long</option>\n"
    "            <option value=\"LEFT_BOTTOM\">Left short -> Bottom long</option>\n"
    "            <option value=\"RIGHT_TOP\">Right short -> Top long</option>\n"
    "            <option value=\"RIGHT_BOTTOM\">Right short -> Bottom long</option>\n"
    "          </select>\n"
    "          <button id=\"showInstallGuideBtn\">Enable Install Guide</button>\n"
    "          <button id=\"ledPowerBtn\">Turn LED Strip Off</button>\n"
    "          </div>\n"
    "          <div class=\"button-row\">\n"
    "          <label for=\"colorOrderSelect\">LED RGB Order</label>\n"
    "          <select id=\"colorOrderSelect\">\n"
    "            <option value=\"RGB\">RGB</option>\n"
    "            <option value=\"RBG\">RBG</option>\n"
    "            <option value=\"GRB\">GRB</option>\n"
    "            <option value=\"GBR\">GBR</option>\n"
    "            <option value=\"BRG\">BRG</option>\n"
    "            <option value=\"BGR\">BGR</option>\n"
    "          </select>\n"
    "          </div>\n"
    "          <div class=\"button-row\">\n"
    "          <button id=\"snapBtn\">Save Raw Snapshot</button>\n"
    "          <button id=\"saveSnapBtn\">Save Rectified Preview</button>\n"
    "          <button id=\"saveHiResSnapBtn\">Save 16x16 Mosaic</button>\n"
    "          <button id=\"autoPreviewBtn\">Start Auto Preview</button>\n"
    "          <button id=\"enterRunBtn\">Enter Run Mode</button>\n"
    "          <button id=\"returnDebugBtn\">Return To Debug Mode</button>\n"
    "          </div>\n"
    "        </div>\n"
    "      </div>\n"
    "    </section>\n"
    "    <section class=\"panel\" id=\"ispPanel\">\n"
    "      <div class=\"section-header\"><h2>ISP Controls</h2><div class=\"button-row\"><button id=\"restoreDefaultBtn\">Restore T23 Defaults</button><button id=\"restoreSavedBtn\">Restore Saved Params</button><button id=\"saveParamsBtn\">Save Startup Params</button></div></div>\n"
    "      <div class=\"grid\" id=\"paramGrid\"></div>\n"
    "    </section>\n"
    "    <section class=\"panel panel--preview\" id=\"previewPanel\">\n"
    "      <div class=\"section-header\">\n"
    "        <h2>Preview</h2>\n"
    "        <span id=\"previewStatus\" class=\"preview-status\">Idle</span>\n"
    "      </div>\n"
    "      <div class=\"preview-compare\">\n"
    "        <div class=\"preview-pane\" id=\"originalPane\">\n"
          "          <h3>Original Stream</h3>\n"
          "          <div class=\"preview-wrap\"><img id=\"previewImage\" alt=\"JPEG preview will appear here\"></div>\n"
          "        </div>\n"
    "        <div class=\"preview-pane\">\n"
    "          <h3 id=\"mosaicPaneTitle\">Rectified 16x16 Mosaic Preview</h3>\n"
    "          <div class=\"preview-wrap\"><canvas id=\"borderCanvas\" width=\"320\" height=\"320\"></canvas></div>\n"
    "        </div>\n"
    "      </div>\n"
    "    </section>\n"
    "    <section class=\"panel panel--calibration\" id=\"calibrationPanel\">\n"
    "      <div class=\"section-header\">\n"
    "        <h2>Border Calibration</h2>\n"
    "        <span class=\"preview-status\" id=\"calibrationStatus\">Idle</span>\n"
    "      </div>\n"
    "      <p class=\"subtitle\">Drag 8 points on the original full-FOV calibration image, then confirm the final rectified preview. The final rectification still uses the fixed fisheye lens parameters.</p>\n"
    "      <div class=\"section-header section-header--tight\">\n"
    "        <h3>Lens Parameters</h3>\n"
    "        <span class=\"preview-status\" id=\"lensStatus\">Lens preset idle</span>\n"
    "      </div>\n"
    "      <div class=\"button-row calibration-actions\">\n"
    "        <button id=\"updateLensBtn\">Update Lens Params</button>\n"
    "      </div>\n"
    "      <div class=\"button-row calibration-actions\">\n"
    "        <button id=\"loadCalibrationSnapBtn\">Load Calibration Snapshot</button>\n"
    "        <button id=\"resetCalibrationBtn\">Reset 8 Points</button>\n"
    "        <button id=\"saveCalibrationBtn\">Confirm 8 Points</button>\n"
    "        <button id=\"horizontalLockBtn\">Horizontal Lock: On</button>\n"
    "        <button id=\"symmetryLockBtn\">Symmetry Lock: Off</button>\n"
    "      </div>\n"
    "      <div class=\"calibration-grid\">\n"
    "        <div class=\"calibration-pane\">\n"
    "          <h3>Interactive Calibration</h3>\n"
    "          <canvas id=\"calibrationCanvas\" width=\"640\" height=\"320\"></canvas>\n"
    "          <div class=\"calibration-tools\">\n"
    "            <div class=\"magnifier-wrap\">\n"
    "              <canvas id=\"magnifierCanvas\" width=\"180\" height=\"180\"></canvas>\n"
    "            </div>\n"
    "            <p class=\"calibration-hint\">Drag points on the raw fisheye image. Use arrow keys for 1px nudges, Shift+Arrow for 5px nudges. When you release the mouse, the point will snap to the nearest strong edge.</p>\n"
    "          </div>\n"
    "        </div>\n"
    "        <div class=\"calibration-pane\">\n"
    "          <h3>Rectified Preview</h3>\n"
    "          <canvas id=\"rectifiedCanvas\" width=\"640\" height=\"320\"></canvas>\n"
    "        </div>\n"
    "      </div>\n"
    "    </section>\n"
    "    <section class=\"panel\" id=\"ledCalPanel\">\n"
    "      <div class=\"section-header\"><h2>LED Color Calibration</h2><span id=\"ledCalStatus\" class=\"preview-status\">Idle</span></div>\n"
    "      <p class=\"subtitle\">Pick a color from the panel or choose a white temperature. The UI uses measured anchor colors and interpolates between them before sending live RGB values to the strip.</p>\n"
    "      <div class=\"led-color-shell\">\n"
    "        <div class=\"led-color-readout\">\n"
    "          <span id=\"ledCurrentMode\" class=\"preview-status\">Mode: LIVE</span>\n"
    "          <span id=\"ledRgbValue\" class=\"preview-status\">RGB(139,118,58)</span>\n"
    "          <span id=\"ledHexValue\" class=\"preview-status\">#8B763A</span>\n"
    "        </div>\n"
    "        <div class=\"led-color-pad-wrap\">\n"
    "          <canvas id=\"ledColorCanvas\" width=\"720\" height=\"260\"></canvas>\n"
    "        </div>\n"
    "        <div class=\"led-swatch-row\" id=\"ledPresetRow\">\n"
    "          <button id=\"ledPresetBlueBtn\" class=\"led-swatch\" title=\"Blue\"></button>\n"
    "          <button id=\"ledPresetGreenBtn\" class=\"led-swatch\" title=\"Green\"></button>\n"
    "          <button id=\"ledPresetRedBtn\" class=\"led-swatch\" title=\"Red\"></button>\n"
    "          <button id=\"ledPresetCyanBtn\" class=\"led-swatch\" title=\"Cyan\"></button>\n"
    "          <button id=\"ledPresetYellowBtn\" class=\"led-swatch\" title=\"Yellow\"></button>\n"
    "          <button id=\"ledPresetMagentaBtn\" class=\"led-swatch\" title=\"Magenta\"></button>\n"
    "          <button id=\"ledPresetColorWhiteBtn\" class=\"led-chip-btn\">Color White / HEX</button>\n"
    "        </div>\n"
    "        <div class=\"led-white-card\">\n"
    "          <div class=\"section-header section-header--tight\"><h3>White Temperature</h3><span id=\"ledWhiteLabel\" class=\"preview-status\">4200K</span></div>\n"
    "          <input id=\"ledWhiteTempSlider\" class=\"led-white-slider\" type=\"range\" min=\"2700\" max=\"6500\" step=\"10\" value=\"4200\">\n"
    "          <div class=\"led-swatch-row led-swatch-row--compact\">\n"
    "            <button id=\"ledPresetWarmBtn\" class=\"led-chip-btn\">2700K</button>\n"
    "            <button id=\"ledPresetNeutralBtn\" class=\"led-chip-btn\">4200K</button>\n"
    "            <button id=\"ledPresetCoolBtn\" class=\"led-chip-btn\">6500K</button>\n"
    "            <button id=\"ledTestLiveBtn\">Return To Live</button>\n"
    "          </div>\n"
    "        </div>\n"
    "        <div class=\"led-mapping-card\">\n"
    "          <div class=\"section-header section-header--tight\"><h3>LED Mapping Editor</h3><span id=\"ledMapStatus\" class=\"preview-status\">Profile: 5M</span></div>\n"
    "          <p class=\"subtitle\">RUN mode now uses the 16x16 rectified mosaic as its only live source. Click a logical LED slot, then click a cell in the grid to remap where that LED samples color.</p>\n"
    "          <div class=\"button-row led-mapping-actions\">\n"
    "            <button id=\"ledMapResetBtn\">Reset Mapping</button>\n"
    "            <button id=\"ledMapSaveBtn\">Save Mapping</button>\n"
    "            <span id=\"ledMapSelection\" class=\"preview-status\">TOP 0 -> (0,0)</span>\n"
    "          </div>\n"
    "          <div class=\"led-mapping-grid\">\n"
    "            <div class=\"led-mapping-slots\" id=\"ledMapSlots\"></div>\n"
    "            <div class=\"led-mapping-canvas-wrap\"><canvas id=\"ledMapCanvas\" width=\"420\" height=\"420\"></canvas></div>\n"
    "          </div>\n"
    "        </div>\n"
    "      </div>\n"
    "    </section>\n"
    "    <section class=\"panel\" id=\"logPanel\">\n"
    "      <div class=\"section-header\"><h2>Log</h2><button id=\"clearLogBtn\">Clear</button></div>\n"
    "      <pre id=\"logBox\" class=\"log-box\"></pre>\n"
    "    </section>\n"
    "  </main>\n"
    "  <script src=\"/app.js\"></script>\n"
    "</body>\n"
    "</html>\n";

static const char g_styles_css[] =
    ":root {\n"
    "  --bg: #eef1e8;\n"
    "  --panel: #fffdf7;\n"
    "  --ink: #1e2a24;\n"
    "  --muted: #5b6b61;\n"
    "  --accent: #0d6b57;\n"
    "  --accent-2: #d26a2e;\n"
    "  --line: #d9dfd5;\n"
    "  --good: #14804a;\n"
    "  --bad: #b83b2d;\n"
    "  --shadow: 0 14px 34px rgba(19, 43, 33, 0.08);\n"
    "}\n"
    "* { box-sizing: border-box; }\n"
    "body {\n"
    "  margin: 0;\n"
    "  font-family: \"Segoe UI\", \"PingFang SC\", \"Microsoft YaHei\", sans-serif;\n"
    "  color: var(--ink);\n"
    "  background: radial-gradient(circle at top right, rgba(210, 106, 46, 0.12), transparent 28%), radial-gradient(circle at top left, rgba(13, 107, 87, 0.12), transparent 32%), var(--bg);\n"
    "}\n"
    ".layout { width: min(1200px, calc(100vw - 32px)); margin: 20px auto 40px; display: grid; gap: 18px; }\n"
    ".panel { background: var(--panel); border: 1px solid var(--line); border-radius: 18px; box-shadow: var(--shadow); padding: 20px; }\n"
    ".panel--hero { display: grid; gap: 20px; }\n"
    ".hero-copy { display: grid; gap: 8px; }\n"
    ".eyebrow { margin: 0 0 8px; color: var(--accent-2); font-weight: 700; letter-spacing: 0.08em; text-transform: uppercase; font-size: 12px; }\n"
    "h1, h2 { margin: 0; }\n"
    "h1 { font-size: clamp(28px, 4vw, 42px); }\n"
    ".subtitle { color: var(--muted); line-height: 1.6; }\n"
    ".hero-panels { display:grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 16px; align-items: stretch; }\n"
    ".connection-card, .wifi-card { border: 1px solid var(--line); border-radius: 16px; padding: 16px; background: linear-gradient(180deg, #ffffff, #f4f8f2); height: 100%; }\n"
    ".status-row, .button-row, .section-header { display: flex; align-items: center; gap: 12px; }\n"
    ".section-header { justify-content: space-between; margin-bottom: 16px; }\n"
    ".section-header--tight { margin-bottom: 8px; }\n"
    ".wifi-card h3 { margin: 0; font-size: 18px; }\n"
    ".status-dot { width: 12px; height: 12px; border-radius: 50%; background: var(--bad); box-shadow: 0 0 0 5px rgba(184, 59, 45, 0.12); }\n"
    ".status-dot.connected { background: var(--good); box-shadow: 0 0 0 5px rgba(20, 128, 74, 0.12); }\n"
    ".grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(240px, 1fr)); gap: 14px; }\n"
    ".param-card { border: 1px solid var(--line); border-radius: 14px; padding: 14px; background: #fff; }\n"
    ".param-card header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 8px; }\n"
    ".param-card label { font-weight: 700; }\n"
    ".param-card .value { color: var(--accent); font-weight: 700; }\n"
    ".param-card input[type=\"range\"] { width: 100%; }\n"
    ".param-actions { display: flex; gap: 8px; margin-top: 10px; }\n"
    "button, select, input { border-radius: 10px; border: 1px solid var(--line); padding: 10px 12px; font: inherit; }\n"
    "button { cursor: pointer; background: #fff; }\n"
    "button:hover:enabled { border-color: var(--accent); }\n"
    "button:disabled { opacity: 0.5; cursor: not-allowed; }\n"
    ".preview-wrap { border: 1px dashed var(--line); border-radius: 16px; min-height: 280px; background: repeating-linear-gradient(45deg, rgba(13, 107, 87, 0.03), rgba(13, 107, 87, 0.03) 16px, rgba(210, 106, 46, 0.03) 16px, rgba(210, 106, 46, 0.03) 32px); display: flex; align-items: center; justify-content: center; overflow: hidden; }\n"
    ".preview-compare { display:grid; grid-template-columns: repeat(auto-fit, minmax(320px, 1fr)); gap: 16px; }\n"
    ".preview-pane h3 { margin: 0 0 10px; font-size: 18px; }\n"
    ".calibration-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(320px, 1fr)); gap: 16px; }\n"
    ".calibration-pane h3 { margin: 0 0 10px; font-size: 18px; }\n"
    ".calibration-pane canvas { width: 100%; height: auto; border: 1px dashed var(--line); border-radius: 16px; background: #fbfcf8; display: block; }\n"
    ".calibration-actions { margin-bottom: 16px; flex-wrap: wrap; }\n"
    ".calibration-tools { display: grid; grid-template-columns: 180px 1fr; gap: 14px; margin-top: 14px; align-items: start; }\n"
    ".magnifier-wrap canvas { width: 180px; height: 180px; border: 1px solid var(--line); border-radius: 14px; background: #0f1713; display: block; }\n"
    ".calibration-hint { margin: 0; color: var(--muted); line-height: 1.6; }\n"
    ".rectify-tune-card { margin-bottom: 16px; }\n"
    ".preview-status { display: inline-flex; align-items: center; min-height: 36px; padding: 6px 12px; border-radius: 999px; border: 1px solid var(--line); background: #f7faf4; color: var(--muted); font-weight: 700; }\n"
    ".preview-status.is-busy { color: var(--accent); border-color: rgba(13, 107, 87, 0.3); background: rgba(13, 107, 87, 0.08); }\n"
    ".preview-status.is-good { color: var(--good); border-color: rgba(20, 128, 74, 0.25); background: rgba(20, 128, 74, 0.08); }\n"
    ".preview-status.is-bad { color: var(--bad); border-color: rgba(184, 59, 45, 0.25); background: rgba(184, 59, 45, 0.08); }\n"
    ".led-color-shell { display: grid; gap: 16px; }\n"
    ".led-color-readout { display: flex; flex-wrap: wrap; gap: 10px; }\n"
    ".led-color-pad-wrap { border: 1px solid var(--line); border-radius: 26px; background: linear-gradient(180deg, rgba(255,255,255,0.92), rgba(243,248,255,0.95)); padding: 14px; box-shadow: inset 0 1px 0 rgba(255,255,255,0.7); }\n"
    "#ledColorCanvas { width: 100%; height: auto; border-radius: 20px; display: block; cursor: crosshair; }\n"
    ".led-swatch-row { display: flex; flex-wrap: wrap; gap: 14px; align-items: center; }\n"
    ".led-swatch-row--compact { gap: 10px; }\n"
    ".led-swatch { width: 58px; height: 58px; border-radius: 50%; border: 3px solid rgba(255,255,255,0.92); box-shadow: 0 10px 20px rgba(18,32,25,0.14); padding: 0; }\n"
    ".led-swatch:hover:enabled, .led-chip-btn:hover:enabled { transform: translateY(-1px); }\n"
    ".led-chip-btn { min-width: 112px; border-radius: 999px; border: 2px solid rgba(46,125,214,0.55); background: #fff; color: #2470c9; font-weight: 700; }\n"
    ".led-white-card { border: 1px solid var(--line); border-radius: 20px; padding: 16px; background: linear-gradient(180deg, #ffffff, #f7fbff); }\n"
    ".led-white-slider { -webkit-appearance: none; appearance: none; width: 100%; height: 24px; border-radius: 999px; padding: 0; border: 1px solid rgba(36,112,201,0.18); background: linear-gradient(90deg, #ffd8a6 0%, #fff8ea 45%, #eef6ff 100%); box-shadow: inset 0 1px 3px rgba(18,32,25,0.14); }\n"
    ".led-white-slider::-webkit-slider-thumb { -webkit-appearance: none; appearance: none; width: 28px; height: 28px; border-radius: 50%; border: 4px solid #fff; background: #eb6a89; box-shadow: 0 4px 14px rgba(18,32,25,0.18); cursor: pointer; }\n"
    ".led-white-slider::-moz-range-thumb { width: 28px; height: 28px; border-radius: 50%; border: 4px solid #fff; background: #eb6a89; box-shadow: 0 4px 14px rgba(18,32,25,0.18); cursor: pointer; }\n"
    ".led-white-slider::-moz-range-track { height: 24px; border-radius: 999px; background: linear-gradient(90deg, #ffd8a6 0%, #fff8ea 45%, #eef6ff 100%); }\n"
    ".led-mapping-card { display: none; border: 1px solid var(--line); border-radius: 20px; padding: 16px; background: linear-gradient(180deg, #ffffff, #f7fbff); }\n"
    ".led-mapping-actions { margin-bottom: 14px; flex-wrap: wrap; }\n"
    ".led-mapping-grid { display: grid; grid-template-columns: minmax(220px, 320px) minmax(260px, 1fr); gap: 16px; align-items: start; }\n"
    ".led-mapping-slots { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 10px; }\n"
    ".led-map-slot { display: flex; justify-content: space-between; align-items: center; gap: 10px; width: 100%; border-radius: 14px; border: 1px solid var(--line); background: #fff; padding: 10px 12px; text-align: left; }\n"
    ".led-map-slot.is-selected { border-color: rgba(36,112,201,0.75); box-shadow: inset 0 0 0 1px rgba(36,112,201,0.18), 0 10px 18px rgba(36,112,201,0.10); }\n"
    ".led-map-slot-code { font-weight: 700; color: var(--ink); }\n"
    ".led-map-slot-coord { color: var(--muted); font-size: 13px; }\n"
    ".led-mapping-canvas-wrap { border: 1px solid var(--line); border-radius: 18px; padding: 12px; background: #fff; }\n"
    "#ledMapCanvas { width: 100%; height: auto; display: block; border-radius: 12px; background: #101814; cursor: crosshair; }\n"
    ".is-hidden { display: none !important; }\n"
    "#previewImage, #borderCanvas { max-width: 100%; max-height: 560px; display: block; }\n"
    ".log-box { min-height: 180px; max-height: 300px; overflow: auto; padding: 14px; background: #122019; color: #cce7db; border-radius: 14px; white-space: pre-wrap; }\n"
    "@media (max-width: 980px) { .hero-panels { grid-template-columns: 1fr; } }\n"
    "@media (max-width: 820px) { .button-row { flex-wrap: wrap; } .calibration-tools { grid-template-columns: 1fr; } .magnifier-wrap canvas { width: min(220px, 100%); height: auto; aspect-ratio: 1 / 1; } .led-mapping-grid { grid-template-columns: 1fr; } .led-mapping-slots { grid-template-columns: 1fr; } }\n";

static const char g_app_js[] =
    "const PARAMS=[\n"
    " {key:'BRIGHTNESS',label:'Brightness',min:0,max:255,step:1},\n"
    " {key:'CONTRAST',label:'Contrast',min:0,max:255,step:1},\n"
    " {key:'SHARPNESS',label:'Sharpness',min:0,max:255,step:1},\n"
    " {key:'SATURATION',label:'Saturation',min:0,max:255,step:1},\n"
    " {key:'AE_COMP',label:'AE Compensation',min:90,max:250,step:1},\n"
    " {key:'DRC',label:'DRC Strength',min:0,max:255,step:1},\n"
    " {key:'AWB_CT',label:'AWB Color Temp',min:1500,max:12000,step:10},\n"
    " {key:'HUE',label:'Hue',min:0,max:255,step:1}\n"
    "];\n"
    "const LED_COLOR_MODEL={displayWhite:[255,255,255],colorWhite:[139,118,58],whiteAnchors:[{k:2700,rgb:[255,160,15]},{k:4200,rgb:[255,169,55]},{k:6500,rgb:[245,171,93]}],hueAnchors:[{h:0,rgb:[255,0,0]},{h:60,rgb:[255,199,0]},{h:120,rgb:[0,255,0]},{h:180,rgb:[0,255,179]},{h:240,rgb:[0,0,255]},{h:300,rgb:[255,0,199]},{h:360,rgb:[255,0,0]}],presets:{RED:[255,0,0],GREEN:[0,255,0],BLUE:[0,0,255],CYAN:[0,255,179],YELLOW:[255,199,0],MAGENTA:[255,0,199]}};\n"
    "const POINT_LABELS=['TL','TM','TR','RM','BR','BM','BL','LM'];\n"
    "const ui={snapBtn:document.getElementById('snapBtn'),saveSnapBtn:document.getElementById('saveSnapBtn'),saveHiResSnapBtn:document.getElementById('saveHiResSnapBtn'),autoPreviewBtn:document.getElementById('autoPreviewBtn'),enterRunBtn:document.getElementById('enterRunBtn'),returnDebugBtn:document.getElementById('returnDebugBtn'),modeBadge:document.getElementById('modeBadge'),statusText:document.getElementById('statusText'),wifiStatusBadge:document.getElementById('wifiStatusBadge'),wifiHint:document.getElementById('wifiHint'),wifiScanSelect:document.getElementById('wifiScanSelect'),wifiScanBtn:document.getElementById('wifiScanBtn'),wifiSsidInput:document.getElementById('wifiSsidInput'),wifiPassInput:document.getElementById('wifiPassInput'),wifiSaveBtn:document.getElementById('wifiSaveBtn'),wifiForgetBtn:document.getElementById('wifiForgetBtn'),stripProfileSelect:document.getElementById('stripProfileSelect'),installModeSelect:document.getElementById('installModeSelect'),colorOrderSelect:document.getElementById('colorOrderSelect'),showInstallGuideBtn:document.getElementById('showInstallGuideBtn'),ledPowerBtn:document.getElementById('ledPowerBtn'),ispPanel:document.getElementById('ispPanel'),previewPanel:document.getElementById('previewPanel'),mosaicPaneTitle:document.getElementById('mosaicPaneTitle'),saveParamsBtn:document.getElementById('saveParamsBtn'),restoreDefaultBtn:document.getElementById('restoreDefaultBtn'),restoreSavedBtn:document.getElementById('restoreSavedBtn'),ledCalPanel:document.getElementById('ledCalPanel'),ledCalStatus:document.getElementById('ledCalStatus'),ledCurrentMode:document.getElementById('ledCurrentMode'),ledRgbValue:document.getElementById('ledRgbValue'),ledHexValue:document.getElementById('ledHexValue'),ledColorCanvas:document.getElementById('ledColorCanvas'),ledWhiteLabel:document.getElementById('ledWhiteLabel'),ledWhiteTempSlider:document.getElementById('ledWhiteTempSlider'),ledPresetBlueBtn:document.getElementById('ledPresetBlueBtn'),ledPresetGreenBtn:document.getElementById('ledPresetGreenBtn'),ledPresetRedBtn:document.getElementById('ledPresetRedBtn'),ledPresetCyanBtn:document.getElementById('ledPresetCyanBtn'),ledPresetYellowBtn:document.getElementById('ledPresetYellowBtn'),ledPresetMagentaBtn:document.getElementById('ledPresetMagentaBtn'),ledPresetColorWhiteBtn:document.getElementById('ledPresetColorWhiteBtn'),ledPresetWarmBtn:document.getElementById('ledPresetWarmBtn'),ledPresetNeutralBtn:document.getElementById('ledPresetNeutralBtn'),ledPresetCoolBtn:document.getElementById('ledPresetCoolBtn'),ledTestLiveBtn:document.getElementById('ledTestLiveBtn'),ledMapStatus:document.getElementById('ledMapStatus'),ledMapResetBtn:document.getElementById('ledMapResetBtn'),ledMapSaveBtn:document.getElementById('ledMapSaveBtn'),ledMapSelection:document.getElementById('ledMapSelection'),ledMapSlots:document.getElementById('ledMapSlots'),ledMapCanvas:document.getElementById('ledMapCanvas'),originalPane:document.getElementById('originalPane'),calibrationPanel:document.getElementById('calibrationPanel'),logPanel:document.getElementById('logPanel'),clearLogBtn:document.getElementById('clearLogBtn'),paramGrid:document.getElementById('paramGrid'),logBox:document.getElementById('logBox'),previewImage:document.getElementById('previewImage'),borderCanvas:document.getElementById('borderCanvas'),previewStatus:document.getElementById('previewStatus'),lensStatus:document.getElementById('lensStatus'),updateLensBtn:document.getElementById('updateLensBtn'),loadCalibrationSnapBtn:document.getElementById('loadCalibrationSnapBtn'),resetCalibrationBtn:document.getElementById('resetCalibrationBtn'),saveCalibrationBtn:document.getElementById('saveCalibrationBtn'),horizontalLockBtn:document.getElementById('horizontalLockBtn'),symmetryLockBtn:document.getElementById('symmetryLockBtn'),calibrationStatus:document.getElementById('calibrationStatus'),calibrationCanvas:document.getElementById('calibrationCanvas'),magnifierCanvas:document.getElementById('magnifierCanvas'),rectifiedCanvas:document.getElementById('rectifiedCanvas')};\n"
    "const state={autoPreviewTimer:null,autoPreviewBusy:false,previewBusy:false,runtimeBusy:false,mosaicBusy:false,blocksBusy:false,previewUrl:null,calibrationImage:null,rectifiedImage:null,previewMosaic:null,borderData:null,mode:'DEBUG',layout:'16X9',stripProfile:'5M',installMode:'LEFT_TOP',colorOrder:'RGB',installGuideActive:true,ledPowerEnabled:true,wifiConfigured:true,wifiConnected:false,wifiSsid:'',wifiIp:'',lastBlocksAt:0,lastMosaicAt:0,debugWarmUntil:0,defaultsLoaded:false,ledCal:{rGain:100,gGain:100,bGain:100,rGamma:100,gGamma:100,bGamma:100,testMode:'LIVE',customR:139,customG:118,customB:58},ledUi:{x:0.96,y:0.50,whiteTemp:4200,rgb:[139,118,58],source:'LIVE',bound:false,dragging:false,pendingTimer:null},mapping:{profile:'5M',label:'',topCount:0,rightCount:0,bottomCount:0,leftCount:0,cells:[],selectedEdge:'TOP',selectedIndex:0,bound:false},lens:{deviceValid:false,k1Scaled:0,k2Scaled:0,knewScale:1,fx:0,fy:0,cx:0,cy:0},calibration:{imageWidth:640,imageHeight:320,points:[]},dragIndex:-1,selectedIndex:0,hoverPos:null,calibrationImageData:null,calibrationSampleCanvas:null,horizontalLock:true,symmetryLock:false};\n"
    "function log(m){const ts=new Date().toLocaleTimeString();ui.logBox.textContent+=`[${ts}] ${m}\\n`;ui.logBox.scrollTop=ui.logBox.scrollHeight;}\n"
    "function setPreviewStatus(t,k='idle'){ui.previewStatus.textContent=t;ui.previewStatus.classList.remove('is-busy','is-good','is-bad');if(k==='busy')ui.previewStatus.classList.add('is-busy');else if(k==='good')ui.previewStatus.classList.add('is-good');else if(k==='bad')ui.previewStatus.classList.add('is-bad');}\n"
    "function setCalibrationStatus(t,k='idle'){ui.calibrationStatus.textContent=t;ui.calibrationStatus.classList.remove('is-busy','is-good','is-bad');if(k==='busy')ui.calibrationStatus.classList.add('is-busy');else if(k==='good')ui.calibrationStatus.classList.add('is-good');else if(k==='bad')ui.calibrationStatus.classList.add('is-bad');}\n"
    "function setLensStatus(t,k='idle'){if(!ui.lensStatus)return;ui.lensStatus.textContent=t;ui.lensStatus.classList.remove('is-busy','is-good','is-bad');if(k==='busy')ui.lensStatus.classList.add('is-busy');else if(k==='good')ui.lensStatus.classList.add('is-good');else if(k==='bad')ui.lensStatus.classList.add('is-bad');}\n"
    "function setLedCalStatus(t,k='idle'){ui.ledCalStatus.textContent=t;ui.ledCalStatus.classList.remove('is-busy','is-good','is-bad');if(k==='busy')ui.ledCalStatus.classList.add('is-busy');else if(k==='good')ui.ledCalStatus.classList.add('is-good');else if(k==='bad')ui.ledCalStatus.classList.add('is-bad');}\n"
    "function setLedMapStatus(t,k='idle'){if(!ui.ledMapStatus)return;ui.ledMapStatus.textContent=t;ui.ledMapStatus.classList.remove('is-busy','is-good','is-bad');if(k==='busy')ui.ledMapStatus.classList.add('is-busy');else if(k==='good')ui.ledMapStatus.classList.add('is-good');else if(k==='bad')ui.ledMapStatus.classList.add('is-bad');}\n"
    "function waitForImage(img){if(img.complete&&img.naturalWidth>0)return Promise.resolve();return new Promise((resolve,reject)=>{const onLoad=()=>{img.removeEventListener('load',onLoad);img.removeEventListener('error',onError);resolve();};const onError=(e)=>{img.removeEventListener('load',onLoad);img.removeEventListener('error',onError);reject(e);};img.addEventListener('load',onLoad,{once:true});img.addEventListener('error',onError,{once:true});});}\n"
    "function applyLayoutMeta(data){if(!data)return;const layout=data.layout||state.layout||'16X9';state.layout=layout;if(ui.layoutSelect&&ui.layoutSelect.value!==layout)ui.layoutSelect.value=layout;}\n"
    "function applyInstallModeMeta(data){if(!data)return;const installMode=data.installMode||state.installMode||'LEFT_TOP';const colorOrder=data.colorOrder||state.colorOrder||'RGB';const stripProfile=data.stripProfile||state.stripProfile||'5M';state.installMode=installMode;state.colorOrder=colorOrder;state.stripProfile=stripProfile;state.installGuideActive=typeof data.installGuideActive==='boolean'?data.installGuideActive:state.installGuideActive;if(ui.installModeSelect&&ui.installModeSelect.value!==installMode)ui.installModeSelect.value=installMode;if(ui.colorOrderSelect&&ui.colorOrderSelect.value!==colorOrder)ui.colorOrderSelect.value=colorOrder;if(ui.stripProfileSelect&&ui.stripProfileSelect.value!==stripProfile)ui.stripProfileSelect.value=stripProfile;if(ui.installModeSelect)ui.installModeSelect.disabled=!state.installGuideActive;if(ui.colorOrderSelect)ui.colorOrderSelect.disabled=!state.installGuideActive;if(ui.stripProfileSelect)ui.stripProfileSelect.disabled=!state.installGuideActive;if(ui.showInstallGuideBtn){ui.showInstallGuideBtn.textContent=state.installGuideActive?'Finish Install Guide':'Enable Install Guide';ui.showInstallGuideBtn.classList.toggle('is-good',state.installGuideActive);} }\n"
    "function applyLedPower(data){if(!data)return;state.ledPowerEnabled=!!data.enabled;if(ui.ledPowerBtn){ui.ledPowerBtn.textContent=state.ledPowerEnabled?'Turn LED Strip Off':'Turn LED Strip On';ui.ledPowerBtn.classList.toggle('is-good',state.ledPowerEnabled);}}\n"
    "function applyWifiStatus(data){if(!data)return;state.wifiConfigured=!!data.configured;state.wifiConnected=!!data.connected;state.wifiSsid=data.ssid||'';state.wifiIp=data.ip||'';if(ui.wifiSsidInput&&!ui.wifiSsidInput.matches(':focus'))ui.wifiSsidInput.value=state.wifiSsid;if(ui.wifiStatusBadge){ui.wifiStatusBadge.textContent=state.wifiConnected?`WiFi: ${state.wifiSsid} (${state.wifiIp||'IP pending'})`:(state.wifiConfigured?`Connecting to ${state.wifiSsid||'saved WiFi'}`:'Setup mode');ui.wifiStatusBadge.classList.remove('is-good','is-busy','is-bad');ui.wifiStatusBadge.classList.add(state.wifiConnected?'is-good':'is-busy');}if(ui.statusText)ui.statusText.textContent=state.wifiConnected?`Connected to ${state.wifiSsid}`:`Connected to C3 bridge (${data.apSsid})`;if(ui.wifiHint)ui.wifiHint.textContent=`Fallback AP: ${data.apSsid} / ${data.apPass}`;}\n"
    "function applyWifiScan(data){if(!ui.wifiScanSelect)return;ui.wifiScanSelect.innerHTML='';const placeholder=document.createElement('option');placeholder.value='';placeholder.textContent=data.results&&data.results.length?`Found ${data.results.length} network(s)`:'No networks found';ui.wifiScanSelect.appendChild(placeholder);for(const item of (data.results||[])){const opt=document.createElement('option');opt.value=item.ssid;opt.textContent=`${item.ssid} (${item.rssi} dBm)`;ui.wifiScanSelect.appendChild(opt);}ui.wifiScanSelect.value='';}\n"
    "function getLayoutMeta(d){const top=d.topBlocks||16,right=d.rightBlocks||9,bottom=d.bottomBlocks||16,left=d.leftBlocks||9;return{top,right,bottom,left,total:(d.blockCount||d.blocks?.length||0),layout:(d.layout||state.layout||'16X9')};}\n"
    "function setModeUi(mode){state.mode=mode;ui.modeBadge.textContent=`Mode: ${mode}`;const isRun=mode==='RUN';ui.originalPane.classList.toggle('is-hidden',isRun);ui.ispPanel.classList.toggle('is-hidden',isRun);ui.calibrationPanel.classList.toggle('is-hidden',isRun);ui.ledCalPanel.classList.toggle('is-hidden',isRun);ui.logPanel.classList.toggle('is-hidden',isRun);ui.snapBtn.disabled=isRun;ui.saveSnapBtn.disabled=isRun;ui.saveHiResSnapBtn.disabled=isRun;ui.autoPreviewBtn.disabled=isRun;ui.enterRunBtn.disabled=isRun;ui.returnDebugBtn.disabled=!isRun;if(ui.mosaicPaneTitle)ui.mosaicPaneTitle.textContent=isRun?'Fixed LED Partition Guide':'Rectified 16x16 Mosaic Preview';if(isRun){state.debugWarmUntil=0;setPreviewStatus('Run mode active: web preview polling disabled','good');drawPreviewMosaic();}else{state.debugWarmUntil=Date.now()+1200;setPreviewStatus('Debug mode active','good');drawPreviewMosaic();}}\n"
    "function renderParams(){ui.paramGrid.innerHTML='';for(const p of PARAMS){const card=document.createElement('article');card.className='param-card';card.innerHTML=`<header><label for=\"slider-${p.key}\">${p.label}</label><span class=\"value\" id=\"value-${p.key}\">-</span></header><input id=\"slider-${p.key}\" type=\"range\" min=\"${p.min}\" max=\"${p.max}\" step=\"${p.step}\" value=\"${p.min}\">`;ui.paramGrid.appendChild(card);const slider=document.getElementById(`slider-${p.key}`);const value=document.getElementById(`value-${p.key}`);slider.oninput=()=>{value.textContent=slider.value;};slider.onchange=()=>setParam(p.key,slider.value,true);p.slider=slider;p.valueLabel=value;}}\n"
    "function applyValues(data){for(const p of PARAMS){if(typeof data[p.key]!=='undefined'){p.slider.value=data[p.key];p.valueLabel.textContent=data[p.key];}}state.defaultsLoaded=true;}\n"
    "function applySingleParamValue(data){if(!data||!data.key)return;const p=PARAMS.find(x=>x.key===data.key);if(!p)return;p.slider.value=data.value;p.valueLabel.textContent=data.value;updateDefaultButtonState(p);}\n"
    "async function saveStartupParams(){setPreviewStatus('Saving startup ISP params...','busy');log('GET /api/params/save');try{await fetchJson('/api/params/save');setPreviewStatus('Startup ISP params saved','good');}catch(e){log(`ERR ${e.message}`);setPreviewStatus(`ERR ${e.message}`,'bad');}}\n"
    "async function restoreDefaultParams(){setPreviewStatus('Restoring T23 defaults...','busy');log('GET /api/params/restore_default');try{const data=await fetchJson('/api/params/restore_default');applyValues(data);setPreviewStatus('Restored T23 defaults','good');if(state.mode==='DEBUG')await refreshPreviewAndBlocks();}catch(e){log(`ERR ${e.message}`);setPreviewStatus(`ERR ${e.message}`,'bad');}}\n"
    "async function restoreSavedParams(){setPreviewStatus('Restoring saved startup params...','busy');log('GET /api/params/restore_saved');try{const data=await fetchJson('/api/params/restore_saved');applyValues(data);setPreviewStatus('Restored saved startup params','good');if(state.mode==='DEBUG')await refreshPreviewAndBlocks();}catch(e){log(`ERR ${e.message}`);setPreviewStatus(`ERR ${e.message}`,'bad');}}\n"
    "function lerpByte(a,b,t){return Math.round(a+(b-a)*t);}\n"
    "function mixRgb(a,b,t){return[lerpByte(a[0],b[0],t),lerpByte(a[1],b[1],t),lerpByte(a[2],b[2],t)];}\n"
    "function interpAnchors(value,anchors,key){const v=Number(value);for(let i=0;i<anchors.length-1;i++){const a=anchors[i],b=anchors[i+1];if(v<=b[key]){const span=(b[key]-a[key])||1;const t=clamp((v-a[key])/span,0,1);return mixRgb(a.rgb,b.rgb,t);}}return anchors[anchors.length-1].rgb.slice();}\n"
    "function interpHueRgb(h){let hue=Number(h)%360;if(hue<0)hue+=360;return interpAnchors(hue,LED_COLOR_MODEL.hueAnchors,'h');}\n"
    "function interpWhiteRgb(k){return interpAnchors(clamp(Number(k),2700,6500),LED_COLOR_MODEL.whiteAnchors,'k');}\n"
    "function padOutputRgb(x,y){const hue=clamp(Number(x),0,1)*360;const sat=Math.pow(clamp(1-Number(y),0,1),0.92);return mixRgb(LED_COLOR_MODEL.colorWhite,interpHueRgb(hue),sat);}\n"
    "function padDisplayRgb(x,y){const hue=clamp(Number(x),0,1)*360;const sat=Math.pow(clamp(1-Number(y),0,1),0.92);return mixRgb(LED_COLOR_MODEL.displayWhite,interpHueRgb(hue),sat);}\n"
    "function parseHexRgb(input){const raw=String(input||'').trim().replace(/^#/,'');if(!/^[0-9a-fA-F]{6}$/.test(raw))return null;return[parseInt(raw.slice(0,2),16),parseInt(raw.slice(2,4),16),parseInt(raw.slice(4,6),16)];}\n"
    "function rgbHex(rgb){return'#'+rgb.map(v=>Math.round(clamp(v,0,255)).toString(16).padStart(2,'0')).join('').toUpperCase();}\n"
    "function updateLedReadout(){const rgb=state.ledUi.rgb||[0,0,0];if(ui.ledRgbValue)ui.ledRgbValue.textContent=`RGB(${rgb.map(v=>Math.round(v)).join(',')})`;if(ui.ledHexValue)ui.ledHexValue.textContent=rgbHex(rgb);if(ui.ledCurrentMode)ui.ledCurrentMode.textContent=`Mode: ${state.ledUi.source}`;if(ui.ledWhiteLabel)ui.ledWhiteLabel.textContent=`${Math.round(state.ledUi.whiteTemp)}K`;}\n"
    "function drawLedColorCanvas(){const c=ui.ledColorCanvas;if(!c)return;const ctx=c.getContext('2d');const w=c.width,h=c.height;const image=ctx.createImageData(w,h);for(let x=0;x<w;x++){for(let y=0;y<h;y++){const rgb=padDisplayRgb(x/(w-1),y/(h-1));const idx=(y*w+x)*4;image.data[idx]=rgb[0];image.data[idx+1]=rgb[1];image.data[idx+2]=rgb[2];image.data[idx+3]=255;}}ctx.putImageData(image,0,0);const px=state.ledUi.x*(w-1),py=state.ledUi.y*(h-1);ctx.save();ctx.lineWidth=5;ctx.strokeStyle='rgba(255,255,255,0.96)';ctx.beginPath();ctx.arc(px,py,16,0,Math.PI*2);ctx.stroke();ctx.lineWidth=2;ctx.strokeStyle='rgba(22,36,29,0.55)';ctx.stroke();ctx.restore();}\n"
    "async function queueLedCustomColor(rgb,source){state.ledUi.rgb=rgb.map(v=>Math.round(clamp(v,0,255)));state.ledUi.source=source||'CUSTOM';updateLedReadout();if(state.ledUi.pendingTimer)clearTimeout(state.ledUi.pendingTimer);state.ledUi.pendingTimer=setTimeout(async()=>{const[r,g,b]=state.ledUi.rgb;const url=`/api/led_cal/test?mode=CUSTOM&r=${r}&g=${g}&b=${b}`;log(`GET ${url}`);setLedCalStatus(`Applying ${state.ledUi.source}...`,'busy');try{const data=await fetchJson(url);state.ledCal={...state.ledCal,...data};setLedCalStatus(`LED mode: ${state.ledUi.source}`,'good');}catch(e){log(`ERR ${e.message}`);setLedCalStatus(`ERR ${e.message}`,'bad');}},60);}\n"
    "function setLedColorSelection(h,s,label){const hue=((Number(h)%360)+360)%360;const sat=clamp(Number(s),0,1);state.ledUi.x=hue/360;state.ledUi.y=1-sat;drawLedColorCanvas();queueLedCustomColor(padOutputRgb(state.ledUi.x,state.ledUi.y),label||'CUSTOM COLOR');}\n"
    "function setLedWhiteTemp(k){const kelvin=Math.round(clamp(Number(k),2700,6500));state.ledUi.whiteTemp=kelvin;if(ui.ledWhiteTempSlider&&Number(ui.ledWhiteTempSlider.value)!==kelvin)ui.ledWhiteTempSlider.value=kelvin;updateLedReadout();queueLedCustomColor(interpWhiteRgb(kelvin),`WHITE ${kelvin}K`);}\n"
    "function bindLedColorControls(){if(state.ledUi.bound)return;state.ledUi.bound=true;const updateFromEvent=evt=>{const rect=ui.ledColorCanvas.getBoundingClientRect();const x=clamp((evt.clientX-rect.left)/rect.width,0,1),y=clamp((evt.clientY-rect.top)/rect.height,0,1);state.ledUi.x=x;state.ledUi.y=y;drawLedColorCanvas();queueLedCustomColor(padOutputRgb(x,y),'CUSTOM COLOR');};if(ui.ledColorCanvas){ui.ledColorCanvas.onpointerdown=evt=>{state.ledUi.dragging=true;ui.ledColorCanvas.setPointerCapture(evt.pointerId);updateFromEvent(evt);};ui.ledColorCanvas.onpointermove=evt=>{if(state.ledUi.dragging)updateFromEvent(evt);};ui.ledColorCanvas.onpointerup=evt=>{state.ledUi.dragging=false;try{ui.ledColorCanvas.releasePointerCapture(evt.pointerId);}catch(_){}};}if(ui.ledWhiteTempSlider)ui.ledWhiteTempSlider.oninput=()=>setLedWhiteTemp(ui.ledWhiteTempSlider.value);if(ui.ledPresetBlueBtn)ui.ledPresetBlueBtn.onclick=()=>queueLedCustomColor(LED_COLOR_MODEL.presets.BLUE.slice(),'BLUE');if(ui.ledPresetGreenBtn)ui.ledPresetGreenBtn.onclick=()=>queueLedCustomColor(LED_COLOR_MODEL.presets.GREEN.slice(),'GREEN');if(ui.ledPresetRedBtn)ui.ledPresetRedBtn.onclick=()=>queueLedCustomColor(LED_COLOR_MODEL.presets.RED.slice(),'RED');if(ui.ledPresetCyanBtn)ui.ledPresetCyanBtn.onclick=()=>queueLedCustomColor(LED_COLOR_MODEL.presets.CYAN.slice(),'CYAN');if(ui.ledPresetYellowBtn)ui.ledPresetYellowBtn.onclick=()=>queueLedCustomColor(LED_COLOR_MODEL.presets.YELLOW.slice(),'YELLOW');if(ui.ledPresetMagentaBtn)ui.ledPresetMagentaBtn.onclick=()=>queueLedCustomColor(LED_COLOR_MODEL.presets.MAGENTA.slice(),'MAGENTA');if(ui.ledPresetColorWhiteBtn)ui.ledPresetColorWhiteBtn.onclick=()=>{const input=window.prompt('Enter hex color (#RRGGBB). Leave empty to use Color White.',rgbHex(state.ledUi.rgb||LED_COLOR_MODEL.colorWhite));if(input===null)return;const text=String(input).trim();if(!text){queueLedCustomColor(LED_COLOR_MODEL.colorWhite.slice(),'COLOR WHITE');return;}const rgb=parseHexRgb(text);if(!rgb){setLedCalStatus('HEX format must be #RRGGBB','bad');return;}queueLedCustomColor(rgb,`HEX ${rgbHex(rgb)}`);};if(ui.ledPresetWarmBtn)ui.ledPresetWarmBtn.onclick=()=>setLedWhiteTemp(2700);if(ui.ledPresetNeutralBtn)ui.ledPresetNeutralBtn.onclick=()=>setLedWhiteTemp(4200);if(ui.ledPresetCoolBtn)ui.ledPresetCoolBtn.onclick=()=>setLedWhiteTemp(6500);}\n"
    "function renderLedCal(){bindLedColorControls();if(ui.ledPresetBlueBtn)ui.ledPresetBlueBtn.style.background='rgb(0,0,255)';if(ui.ledPresetGreenBtn)ui.ledPresetGreenBtn.style.background='rgb(0,255,0)';if(ui.ledPresetRedBtn)ui.ledPresetRedBtn.style.background='rgb(255,0,0)';if(ui.ledPresetCyanBtn)ui.ledPresetCyanBtn.style.background='rgb(0,255,179)';if(ui.ledPresetYellowBtn)ui.ledPresetYellowBtn.style.background='rgb(255,199,0)';if(ui.ledPresetMagentaBtn)ui.ledPresetMagentaBtn.style.background='rgb(255,0,199)';if(ui.ledPresetColorWhiteBtn)ui.ledPresetColorWhiteBtn.style.background='#ffffff';if(ui.ledPresetColorWhiteBtn)ui.ledPresetColorWhiteBtn.style.color='#2470c9';drawLedColorCanvas();updateLedReadout();}\n"
    "function applyLedCal(data){if(!data)return;state.ledCal={...state.ledCal,...data};if(typeof data.customR!=='undefined'&&typeof data.customG!=='undefined'&&typeof data.customB!=='undefined'){state.ledUi.rgb=[Number(data.customR)||0,Number(data.customG)||0,Number(data.customB)||0];}if(data.testMode==='LIVE'){state.ledUi.source='LIVE';}else if(data.testMode==='CUSTOM'&&state.ledUi.source==='LIVE'){state.ledUi.source='CUSTOM';}renderLedCal();}\n"
    "async function refreshLedCal(){setLedCalStatus('Refreshing LED color controls...','busy');log('GET /api/led_cal');try{const data=await fetchJson('/api/led_cal');applyLedCal(data);setLedCalStatus(`LED mode: ${data.testMode||'LIVE'}`,'good');}catch(e){log(`ERR ${e.message}`);setLedCalStatus(`ERR ${e.message}`,'bad');}}\n"
    "async function setLedTest(mode){log(`GET /api/led_cal/test?mode=${mode}`);setLedCalStatus(`Switching to ${mode}...`,'busy');try{const data=await fetchJson(`/api/led_cal/test?mode=${encodeURIComponent(mode)}`);if(mode==='LIVE')state.ledUi.source='LIVE';applyLedCal(data);setLedCalStatus(`LED test mode: ${data.testMode}`,'good');}catch(e){log(`ERR ${e.message}`);setLedCalStatus(`ERR ${e.message}`,'bad');}}\n"
    "function makeDefaultCalibration(w,h){return{imageWidth:w,imageHeight:h,points:[{x:Math.round(w*0.02),y:Math.round(h*0.42)},{x:Math.round(w*0.50),y:Math.round(h*0.23)},{x:Math.round(w*0.98),y:Math.round(h*0.42)},{x:Math.round(w*0.90),y:Math.round(h*0.54)},{x:Math.round(w*0.80),y:Math.round(h*0.65)},{x:Math.round(w*0.50),y:Math.round(h*0.64)},{x:Math.round(w*0.18),y:Math.round(h*0.64)},{x:Math.round(w*0.08),y:Math.round(h*0.54)}]};}\n"
    "function ensureCalibrationPoints(){if(!state.calibration.points||state.calibration.points.length!==8){state.calibration=makeDefaultCalibration(state.calibration.imageWidth||640,state.calibration.imageHeight||320);}}\n"
    "function drawCalibrationCanvas(){const c=ui.calibrationCanvas,ctx=c.getContext('2d');ctx.clearRect(0,0,c.width,c.height);ctx.fillStyle='#f8fbf5';ctx.fillRect(0,0,c.width,c.height);if(state.calibrationImage){ctx.drawImage(state.calibrationImage,0,0,c.width,c.height);}ctx.strokeStyle='rgba(13,107,87,0.85)';ctx.lineWidth=3;ensureCalibrationPoints();ctx.beginPath();state.calibration.points.forEach((p,i)=>{if(i===0)ctx.moveTo(p.x,p.y);else ctx.lineTo(p.x,p.y);});ctx.closePath();ctx.stroke();if(state.hoverPos){ctx.save();ctx.strokeStyle='rgba(255,255,255,0.6)';ctx.lineWidth=1;ctx.beginPath();ctx.moveTo(state.hoverPos.x,0);ctx.lineTo(state.hoverPos.x,c.height);ctx.moveTo(0,state.hoverPos.y);ctx.lineTo(c.width,state.hoverPos.y);ctx.stroke();ctx.restore();}ctx.font='12px sans-serif';state.calibration.points.forEach((p,i)=>{ctx.beginPath();ctx.fillStyle=i===state.selectedIndex?'#0d6b57':'#d26a2e';ctx.arc(p.x,p.y,i===state.selectedIndex?8:7,0,Math.PI*2);ctx.fill();ctx.lineWidth=2;ctx.strokeStyle='rgba(255,255,255,0.9)';ctx.stroke();ctx.fillStyle='#132b21';ctx.fillText(POINT_LABELS[i],p.x+10,p.y-10);});drawMagnifier();}\n"
    "function drawRectifiedGuide(){const c=ui.rectifiedCanvas,ctx=c.getContext('2d');const pad=40,w=c.width,h=c.height;const left=pad,right=w-pad,top=pad,bottom=h-pad,cx=Math.round((left+right)/2),cy=Math.round((top+bottom)/2);ctx.clearRect(0,0,w,h);ctx.fillStyle='#f8fbf5';ctx.fillRect(0,0,w,h);if(state.rectifiedImage){const img=state.rectifiedImage;const scale=Math.min(w/img.width,h/img.height);const dw=Math.round(img.width*scale),dh=Math.round(img.height*scale);const dx=Math.round((w-dw)/2),dy=Math.round((h-dh)/2);ctx.drawImage(img,dx,dy,dw,dh);ctx.strokeStyle='rgba(13,107,87,0.45)';ctx.lineWidth=2;ctx.strokeRect(dx,dy,dw,dh);ctx.fillStyle='#5b6b61';ctx.fillText('Rectified preview rendered by T23',pad,h-12);return;}ctx.strokeStyle='rgba(13,107,87,0.85)';ctx.lineWidth=3;ctx.strokeRect(left,top,right-left,bottom-top);const pts=[[left,top],[cx,top],[right,top],[right,cy],[right,bottom],[cx,bottom],[left,bottom],[left,cy]];ctx.font='12px sans-serif';pts.forEach((p,i)=>{ctx.beginPath();ctx.fillStyle='#0d6b57';ctx.arc(p[0],p[1],7,0,Math.PI*2);ctx.fill();ctx.fillStyle='#132b21';ctx.fillText(POINT_LABELS[i],p[0]+10,p[1]-10);});ctx.fillStyle='#5b6b61';ctx.fillText('Rectified preview will appear here after calibration confirm',pad,h-12);}\n"
    "function runGuidePalette(){return['#cdecb8','#dcb6e6','#b7deef','#f2d6c4','#f8efb7','#f7c8d9','#c9d6f5','#bfe7d0'];}\n"
    "function runGuideConfig(){const common={top:[2,2,4,4,2,2],right:[2,3,6,3,2],bottom:[2,2,4,4,2,2],left:[2,3,6,3,2],topColors:[0,1,2,3,4,5],rightColors:[1,2,3,4,5],bottomColors:[0,1,2,3,4,5],leftColors:[5,4,3,2,1]};if(state.stripProfile==='3P8M')return{label:'3.8m weighted 22-region guide',...common};return{label:'5m weighted 22-region guide',...common};}\n"
    "function paintGuideHorizontal(grid,w,h,y,innerY,spans,colors,palette){let start=0;spans.forEach((span,i)=>{const fill=palette[(colors[i]??i)%palette.length];for(let dx=0;dx<span;dx++){const x=start+dx;if(x<0||x>=w)continue;grid[y*w+x]=fill;if(innerY>=0&&innerY<h&&x>0&&x<w-1)grid[innerY*w+x]=fill;}start+=span;});}\n"
    "function paintGuideVertical(grid,w,h,x,innerX,spans,colors,palette){let start=0;spans.forEach((span,i)=>{const fill=palette[(colors[i]??i)%palette.length];for(let dy=0;dy<span;dy++){const y=start+dy;if(y<0||y>=h)continue;grid[y*w+x]=fill;if(innerX>=0&&innerX<w&&y>0&&y<h-1)grid[y*w+innerX]=fill;}start+=span;});}\n"
    "function drawRunPartitionGuide(){const c=ui.borderCanvas,ctx=c.getContext('2d');const w=16,h=16,palette=runGuidePalette(),cfg=runGuideConfig();const pad=16,size=Math.min(c.width,c.height)-pad*2,cell=size/16,drawW=cell*w,drawH=cell*h,dx=Math.round((c.width-drawW)/2),dy=Math.round((c.height-drawH)/2);const grid=new Array(w*h).fill('#f1f3ef');paintGuideHorizontal(grid,w,h,0,1,cfg.top,cfg.topColors,palette);paintGuideVertical(grid,w,h,w-1,w-2,cfg.right,cfg.rightColors,palette);paintGuideHorizontal(grid,w,h,h-1,h-2,cfg.bottom,cfg.bottomColors,palette);paintGuideVertical(grid,w,h,0,1,cfg.left,cfg.leftColors,palette);ctx.clearRect(0,0,c.width,c.height);ctx.fillStyle='#101814';ctx.fillRect(0,0,c.width,c.height);ctx.strokeStyle='rgba(255,255,255,0.16)';ctx.lineWidth=1;for(let y=0;y<h;y++){for(let x=0;x<w;x++){ctx.fillStyle=grid[y*w+x];ctx.fillRect(dx+x*cell,dy+y*cell,Math.ceil(cell),Math.ceil(cell));ctx.strokeRect(dx+x*cell,dy+y*cell,Math.ceil(cell),Math.ceil(cell));}}ctx.strokeStyle='rgba(255,255,255,0.35)';ctx.lineWidth=2;ctx.strokeRect(dx,dy,drawW,drawH);ctx.fillStyle='#cce7db';ctx.font='12px sans-serif';ctx.fillText(cfg.label,16,c.height-12);}\n"
    "function drawPreviewMosaic(){if(state.mode==='RUN'){drawRunPartitionGuide();return;}const c=ui.borderCanvas,ctx=c.getContext('2d');ctx.clearRect(0,0,c.width,c.height);ctx.fillStyle='#101814';ctx.fillRect(0,0,c.width,c.height);if(!state.previewMosaic||!state.previewMosaic.pixels){ctx.fillStyle='#cce7db';ctx.font='16px sans-serif';ctx.fillText('16x16 rectified mosaic will appear here',20,30);return;}const w=state.previewMosaic.width||16,h=state.previewMosaic.height||16,pixels=state.previewMosaic.pixels||[];const pad=16,size=Math.min(c.width,c.height)-pad*2;const cell=size/Math.max(w,h);const drawW=cell*w,drawH=cell*h,dx=Math.round((c.width-drawW)/2),dy=Math.round((c.height-drawH)/2);ctx.strokeStyle='rgba(255,255,255,0.2)';ctx.lineWidth=1;for(let y=0;y<h;y++){for(let x=0;x<w;x++){const idx=(y*w+x)*3;ctx.fillStyle=`rgb(${pixels[idx]||0},${pixels[idx+1]||0},${pixels[idx+2]||0})`;ctx.fillRect(dx+x*cell,dy+y*cell,Math.ceil(cell),Math.ceil(cell));ctx.strokeRect(dx+x*cell,dy+y*cell,Math.ceil(cell),Math.ceil(cell));}}ctx.strokeStyle='rgba(255,255,255,0.35)';ctx.lineWidth=2;ctx.strokeRect(dx,dy,drawW,drawH);ctx.fillStyle='#cce7db';ctx.font='12px sans-serif';ctx.fillText('16x16 mosaic sampled from T23 rectified preview',16,c.height-12);}\n"
    "function edgeShortName(edge){return edge==='TOP'?'T':edge==='RIGHT'?'R':edge==='BOTTOM'?'B':'L';}\n"
    "function mappingCellsForEdge(edge){return(state.mapping.cells||[]).filter(item=>item.edge===edge);}\n"
    "function selectedMappingCell(){return(state.mapping.cells||[]).find(item=>item.edge===state.mapping.selectedEdge&&Number(item.index)===Number(state.mapping.selectedIndex))||null;}\n"
    "function updateLedMapSelection(){const cell=selectedMappingCell();if(!ui.ledMapSelection)return;if(!cell){ui.ledMapSelection.textContent='Select a logical LED';return;}ui.ledMapSelection.textContent=`${cell.edge} ${cell.index} -> (${cell.x},${cell.y})`;}\n"
    "function renderLedMappingSlots(){if(!ui.ledMapSlots)return;const edges=['TOP','RIGHT','BOTTOM','LEFT'];ui.ledMapSlots.innerHTML='';edges.forEach(edge=>{mappingCellsForEdge(edge).forEach(cell=>{const btn=document.createElement('button');btn.className='led-map-slot';if(state.mapping.selectedEdge===cell.edge&&Number(state.mapping.selectedIndex)===Number(cell.index))btn.classList.add('is-selected');btn.innerHTML=`<span class=\"led-map-slot-code\">${edgeShortName(cell.edge)}${String(cell.index).padStart(2,'0')}</span><span class=\"led-map-slot-coord\">(${cell.x},${cell.y})</span>`;btn.onclick=()=>{state.mapping.selectedEdge=cell.edge;state.mapping.selectedIndex=Number(cell.index);renderLedMappingSlots();drawLedMappingCanvas();updateLedMapSelection();};ui.ledMapSlots.appendChild(btn);});});updateLedMapSelection();}\n"
    "function drawLedMappingCanvas(){const c=ui.ledMapCanvas;if(!c)return;const ctx=c.getContext('2d');ctx.clearRect(0,0,c.width,c.height);ctx.fillStyle='#101814';ctx.fillRect(0,0,c.width,c.height);const mosaic=state.previewMosaic;if(!mosaic||!mosaic.pixels){ctx.fillStyle='#cce7db';ctx.font='16px sans-serif';ctx.fillText('Load preview mosaic to edit mapping',20,30);return;}const w=mosaic.width||16,h=mosaic.height||16,pixels=mosaic.pixels||[];const pad=18,size=Math.min(c.width,c.height)-pad*2;const cell=size/Math.max(w,h);const drawW=cell*w,drawH=cell*h,dx=Math.round((c.width-drawW)/2),dy=Math.round((c.height-drawH)/2);ctx.strokeStyle='rgba(255,255,255,0.18)';ctx.lineWidth=1;for(let y=0;y<h;y++){for(let x=0;x<w;x++){const idx=(y*w+x)*3;ctx.fillStyle=`rgb(${pixels[idx]||0},${pixels[idx+1]||0},${pixels[idx+2]||0})`;ctx.fillRect(dx+x*cell,dy+y*cell,Math.ceil(cell),Math.ceil(cell));ctx.strokeRect(dx+x*cell,dy+y*cell,Math.ceil(cell),Math.ceil(cell));}}ctx.strokeStyle='rgba(255,255,255,0.35)';ctx.lineWidth=2;ctx.strokeRect(dx,dy,drawW,drawH);(state.mapping.cells||[]).forEach((item,order)=>{const cx=dx+(Number(item.x)+0.5)*cell,cy=dy+(Number(item.y)+0.5)*cell;const selected=state.mapping.selectedEdge===item.edge&&Number(state.mapping.selectedIndex)===Number(item.index);ctx.beginPath();ctx.fillStyle=selected?'#f4ff5e':'rgba(13,107,87,0.88)';ctx.arc(cx,cy,selected?8:5,0,Math.PI*2);ctx.fill();ctx.lineWidth=selected?3:2;ctx.strokeStyle='rgba(255,255,255,0.92)';ctx.stroke();if(selected){ctx.strokeStyle='rgba(255,255,255,0.96)';ctx.lineWidth=3;ctx.strokeRect(dx+Number(item.x)*cell,dy+Number(item.y)*cell,Math.ceil(cell),Math.ceil(cell));}if(order<12){ctx.fillStyle='rgba(255,255,255,0.9)';ctx.font='11px sans-serif';ctx.fillText(`${edgeShortName(item.edge)}${item.index}`,cx+8,cy-8);}});ctx.fillStyle='#cce7db';ctx.font='12px sans-serif';ctx.fillText('Click a cell to move the selected logical LED sample point',16,c.height-12);}\n"
    "function bindLedMappingEditor(){if(state.mapping.bound)return;state.mapping.bound=true;if(ui.ledMapCanvas){ui.ledMapCanvas.onclick=evt=>{const mosaic=state.previewMosaic;if(!mosaic||!mosaic.pixels)return;const rect=ui.ledMapCanvas.getBoundingClientRect();const w=mosaic.width||16,h=mosaic.height||16;const pad=18,size=Math.min(ui.ledMapCanvas.width,ui.ledMapCanvas.height)-pad*2;const cell=size/Math.max(w,h);const drawW=cell*w,drawH=cell*h,dx=Math.round((ui.ledMapCanvas.width-drawW)/2),dy=Math.round((ui.ledMapCanvas.height-drawH)/2);const x=Math.floor((((evt.clientX-rect.left)*(ui.ledMapCanvas.width/rect.width))-dx)/cell);const y=Math.floor((((evt.clientY-rect.top)*(ui.ledMapCanvas.height/rect.height))-dy)/cell);if(x<0||x>=w||y<0||y>=h)return;setLedMappingCell(x,y);};}if(ui.ledMapResetBtn)ui.ledMapResetBtn.onclick=resetLedMapping;if(ui.ledMapSaveBtn)ui.ledMapSaveBtn.onclick=saveLedMapping;}\n"
    "function applyLedMapping(data){if(!data)return;const cells=Array.isArray(data.cells)?data.cells:[];state.mapping={...state.mapping,profile:data.profile||state.stripProfile,label:data.label||'',topCount:Number(data.topCount)||0,rightCount:Number(data.rightCount)||0,bottomCount:Number(data.bottomCount)||0,leftCount:Number(data.leftCount)||0,cells:cells.map(item=>({edge:item.edge,index:Number(item.index)||0,x:Number(item.x)||0,y:Number(item.y)||0})),bound:state.mapping.bound};const selectedExists=state.mapping.cells.some(item=>item.edge===state.mapping.selectedEdge&&Number(item.index)===Number(state.mapping.selectedIndex));if(!selectedExists&&state.mapping.cells.length){state.mapping.selectedEdge=state.mapping.cells[0].edge;state.mapping.selectedIndex=Number(state.mapping.cells[0].index)||0;}renderLedMappingSlots();drawLedMappingCanvas();setLedMapStatus(`Profile: ${state.mapping.profile}`,'good');}\n"
    "async function refreshLedMapping(){const profile=state.stripProfile||'5M';setLedMapStatus(`Loading ${profile} mapping...`,'busy');log(`GET /api/led_mapping?profile=${profile}`);try{const data=await fetchJson(`/api/led_mapping?profile=${encodeURIComponent(profile)}`);applyLedMapping(data);}catch(e){log(`ERR ${e.message}`);setLedMapStatus(`ERR ${e.message}`,'bad');}}\n"
    "async function setLedMappingCell(x,y){const cell=selectedMappingCell();if(!cell){setLedMapStatus('Select a logical LED first','bad');return;}const profile=state.stripProfile||state.mapping.profile||'5M';const url=`/api/led_mapping/set?profile=${encodeURIComponent(profile)}&edge=${encodeURIComponent(cell.edge)}&index=${cell.index}&x=${x}&y=${y}`;log(`GET ${url}`);setLedMapStatus(`Updating ${cell.edge} ${cell.index}...`,'busy');try{const data=await fetchJson(url);applyLedMapping(data);setLedMapStatus(`Mapped ${cell.edge} ${cell.index} -> (${x},${y})`,'good');}catch(e){log(`ERR ${e.message}`);setLedMapStatus(`ERR ${e.message}`,'bad');}}\n"
    "async function resetLedMapping(){const profile=state.stripProfile||state.mapping.profile||'5M';log(`GET /api/led_mapping/reset?profile=${profile}`);setLedMapStatus(`Resetting ${profile} mapping...`,'busy');try{const data=await fetchJson(`/api/led_mapping/reset?profile=${encodeURIComponent(profile)}`);applyLedMapping(data);setLedMapStatus(`Reset ${profile} mapping`,'good');}catch(e){log(`ERR ${e.message}`);setLedMapStatus(`ERR ${e.message}`,'bad');}}\n"
    "async function saveLedMapping(){const profile=state.stripProfile||state.mapping.profile||'5M';log(`GET /api/led_mapping/save?profile=${profile}`);setLedMapStatus(`Saving ${profile} mapping...`,'busy');try{const data=await fetchJson(`/api/led_mapping/save?profile=${encodeURIComponent(profile)}`);applyLedMapping(data);setLedMapStatus(`Saved ${profile} mapping`,'good');}catch(e){log(`ERR ${e.message}`);setLedMapStatus(`ERR ${e.message}`,'bad');}}\n"
    "function canvasPos(evt){const rect=ui.calibrationCanvas.getBoundingClientRect();const sx=ui.calibrationCanvas.width/rect.width,sy=ui.calibrationCanvas.height/rect.height;return{x:(evt.clientX-rect.left)*sx,y:(evt.clientY-rect.top)*sy};}\n"
    "function pickPoint(pos){ensureCalibrationPoints();for(let i=0;i<state.calibration.points.length;i++){const p=state.calibration.points[i];const dx=p.x-pos.x,dy=p.y-pos.y;if((dx*dx+dy*dy)<=225)return i;}return -1;}\n"
    "function clamp(v,min,max){return Math.max(min,Math.min(max,v));}\n"
    "function getSymmetryCenterX(){const pts=state.calibration.points;return Math.round((pts[1].x+pts[5].x)/2);}\n"
    "function applyHorizontalConstraint(){if(!state.horizontalLock)return;const pts=state.calibration.points;[[0,2],[7,3],[6,4]].forEach(([a,b])=>{const y=Math.round((pts[a].y+pts[b].y)/2);pts[a]={...pts[a],y:y};pts[b]={...pts[b],y:y};});}\n"
    "function applySymmetryConstraint(){if(!state.symmetryLock)return;const pts=state.calibration.points;const maxX=state.calibration.imageWidth-1;const centerX=(pts[1].x+pts[5].x)/2;[[0,2],[7,3],[6,4]].forEach(([l,r])=>{const dist=0.5*((centerX-pts[l].x)+(pts[r].x-centerX));pts[l]={...pts[l],x:Math.round(clamp(centerX-dist,0,maxX))};pts[r]={...pts[r],x:Math.round(clamp(centerX+dist,0,maxX))};});}\n"
    "function getCalibrationImageData(){if(!state.calibrationImage)return null;if(state.calibrationImageData)return state.calibrationImageData;const canvas=state.calibrationSampleCanvas||document.createElement('canvas');canvas.width=state.calibrationImage.width;canvas.height=state.calibrationImage.height;const ctx=canvas.getContext('2d',{willReadFrequently:true});ctx.drawImage(state.calibrationImage,0,0,canvas.width,canvas.height);state.calibrationSampleCanvas=canvas;state.calibrationImageData=ctx.getImageData(0,0,canvas.width,canvas.height);return state.calibrationImageData;}\n"
    "function getActiveCalibrationPos(){ensureCalibrationPoints();if(state.dragIndex>=0)return state.calibration.points[state.dragIndex];if(state.hoverPos)return state.hoverPos;if(state.selectedIndex>=0&&state.calibration.points[state.selectedIndex])return state.calibration.points[state.selectedIndex];return {x:state.calibration.imageWidth/2,y:state.calibration.imageHeight/2};}\n"
    "function drawMagnifier(){const c=ui.magnifierCanvas,ctx=c.getContext('2d');ctx.clearRect(0,0,c.width,c.height);ctx.fillStyle='#101814';ctx.fillRect(0,0,c.width,c.height);if(!state.calibrationImage){ctx.fillStyle='#cce7db';ctx.font='14px sans-serif';ctx.fillText('Load calibration image',16,28);return;}const pos=getActiveCalibrationPos();const srcSize=24;const sx=clamp(Math.round(pos.x-srcSize/2),0,Math.max(0,state.calibrationImage.width-srcSize));const sy=clamp(Math.round(pos.y-srcSize/2),0,Math.max(0,state.calibrationImage.height-srcSize));ctx.imageSmoothingEnabled=false;ctx.drawImage(state.calibrationImage,sx,sy,srcSize,srcSize,0,0,c.width,c.height);ctx.imageSmoothingEnabled=true;ctx.strokeStyle='rgba(255,255,255,0.95)';ctx.lineWidth=1.5;ctx.beginPath();ctx.moveTo(c.width/2,0);ctx.lineTo(c.width/2,c.height);ctx.moveTo(0,c.height/2);ctx.lineTo(c.width,c.height/2);ctx.stroke();ctx.strokeStyle='rgba(13,107,87,0.95)';ctx.lineWidth=2.5;ctx.strokeRect(1,1,c.width-2,c.height-2);ctx.fillStyle='#cce7db';ctx.font='12px sans-serif';const label=state.selectedIndex>=0?POINT_LABELS[state.selectedIndex]:'Cursor';ctx.fillText(`${label}: ${Math.round(pos.x)}, ${Math.round(pos.y)}`,10,c.height-12);}\n"
    "function sampleLuma(img,x,y){const w=img.width,h=img.height;const ix=clamp(x,0,w-1),iy=clamp(y,0,h-1);const idx=(iy*w+ix)*4;const d=img.data;return d[idx]*0.299+d[idx+1]*0.587+d[idx+2]*0.114;}\n"
    "function edgeStrengthAt(img,x,y){const gx=-sampleLuma(img,x-1,y-1)-2*sampleLuma(img,x-1,y)-sampleLuma(img,x-1,y+1)+sampleLuma(img,x+1,y-1)+2*sampleLuma(img,x+1,y)+sampleLuma(img,x+1,y+1);const gy=-sampleLuma(img,x-1,y-1)-2*sampleLuma(img,x,y-1)-sampleLuma(img,x+1,y-1)+sampleLuma(img,x-1,y+1)+2*sampleLuma(img,x,y+1)+sampleLuma(img,x+1,y+1);return Math.abs(gx)+Math.abs(gy);}\n"
    "function snapCalibrationPoint(index){const img=getCalibrationImageData();if(!img||index<0)return;const p=state.calibration.points[index];let bestX=p.x,bestY=p.y,bestScore=-1;const radius=8;for(let dy=-radius;dy<=radius;dy++){for(let dx=-radius;dx<=radius;dx++){const x=Math.round(clamp(p.x+dx,1,img.width-2));const y=Math.round(clamp(p.y+dy,1,img.height-2));const dist=Math.abs(dx)+Math.abs(dy);const score=edgeStrengthAt(img,x,y)-dist*10;if(score>bestScore){bestScore=score;bestX=x;bestY=y;}}}state.calibration.points[index]={x:bestX,y:bestY};}\n"
    "function moveCalibrationPoint(index,dx,dy){if(index<0)return;const maxX=state.calibration.imageWidth-1,maxY=state.calibration.imageHeight-1;const p=state.calibration.points[index];state.calibration.points[index]={x:Math.round(clamp(p.x+dx,0,maxX)),y:Math.round(clamp(p.y+dy,0,maxY))};applyHorizontalConstraint();applySymmetryConstraint();drawCalibrationCanvas();drawRectifiedGuide();}\n"
    "function updateHorizontalLockButton(){if(!ui.horizontalLockBtn)return;ui.horizontalLockBtn.textContent=`Horizontal Lock: ${state.horizontalLock?'On':'Off'}`;ui.horizontalLockBtn.classList.toggle('is-good',state.horizontalLock);}\n"
    "function toggleHorizontalLock(){state.horizontalLock=!state.horizontalLock;applyHorizontalConstraint();applySymmetryConstraint();drawCalibrationCanvas();drawRectifiedGuide();setCalibrationStatus(state.horizontalLock?'Horizontal lock enabled':'Horizontal lock disabled','good');updateHorizontalLockButton();}\n"
    "function updateSymmetryLockButton(){if(!ui.symmetryLockBtn)return;ui.symmetryLockBtn.textContent=`Symmetry Lock: ${state.symmetryLock?'On':'Off'}`;ui.symmetryLockBtn.classList.toggle('is-good',state.symmetryLock);}\n"
    "function toggleSymmetryLock(){state.symmetryLock=!state.symmetryLock;applyHorizontalConstraint();applySymmetryConstraint();drawCalibrationCanvas();drawRectifiedGuide();setCalibrationStatus(state.symmetryLock?'Symmetry lock enabled':'Symmetry lock disabled','good');updateSymmetryLockButton();}\n"
    "function bindCalibrationCanvas(){ui.calibrationCanvas.onpointerdown=(evt)=>{const pos=canvasPos(evt);state.hoverPos=pos;state.dragIndex=pickPoint(pos);if(state.dragIndex>=0){state.selectedIndex=state.dragIndex;ui.calibrationCanvas.setPointerCapture(evt.pointerId);}drawCalibrationCanvas();};ui.calibrationCanvas.onpointermove=(evt)=>{const pos=canvasPos(evt);state.hoverPos=pos;if(state.dragIndex<0){drawCalibrationCanvas();return;}const maxX=state.calibration.imageWidth-1,maxY=state.calibration.imageHeight-1;state.calibration.points[state.dragIndex]={x:Math.round(clamp(pos.x,0,maxX)),y:Math.round(clamp(pos.y,0,maxY))};applyHorizontalConstraint();applySymmetryConstraint();drawCalibrationCanvas();drawRectifiedGuide();};ui.calibrationCanvas.onpointerup=(evt)=>{state.dragIndex=-1;try{ui.calibrationCanvas.releasePointerCapture(evt.pointerId);}catch(_){};};ui.calibrationCanvas.onpointerleave=()=>{state.hoverPos=null;drawCalibrationCanvas();};window.addEventListener('keydown',(evt)=>{if(!ui.calibrationPanel||ui.calibrationPanel.classList.contains('is-hidden'))return;const active=['ArrowUp','ArrowDown','ArrowLeft','ArrowRight'];if(!active.includes(evt.key))return;ensureCalibrationPoints();const step=evt.shiftKey?5:1;let dx=0,dy=0;if(evt.key==='ArrowLeft')dx=-step;else if(evt.key==='ArrowRight')dx=step;else if(evt.key==='ArrowUp')dy=-step;else if(evt.key==='ArrowDown')dy=step;evt.preventDefault();moveCalibrationPoint(state.selectedIndex,dx,dy);});}\n"
    "async function loadCalibrationSnapshot(){setCalibrationStatus('Loading calibration image...','busy');try{const r=await fetch(`/api/calibration/snapshot?t=${Date.now()}`,{cache:'no-store'});if(!r.ok)throw new Error(`HTTP ${r.status}`);const blob=await r.blob();const img=new Image();await new Promise((resolve,reject)=>{img.onload=resolve;img.onerror=reject;img.src=URL.createObjectURL(blob);});state.calibrationImage=img;state.calibrationImageData=null;state.calibrationSampleCanvas=null;ui.calibrationCanvas.width=img.width;ui.calibrationCanvas.height=img.height;if(!state.calibration.points||state.calibration.imageWidth!==img.width||state.calibration.imageHeight!==img.height){state.calibration=makeDefaultCalibration(img.width,img.height);}drawCalibrationCanvas();drawRectifiedGuide();setCalibrationStatus('Calibration image ready','good');}catch(e){log(`ERR ${e.message}`);setCalibrationStatus(`ERR ${e.message}`,'bad');}}\n"
    "async function loadRectifiedPreview(){setCalibrationStatus('Loading rectified preview...','busy');log(`GET /api/calibration/rectified?t=${Date.now()}`);try{const r=await fetch(`/api/calibration/rectified?t=${Date.now()}`,{cache:'no-store'});if(!r.ok)throw new Error(`HTTP ${r.status}`);const blob=await r.blob();const img=new Image();await new Promise((resolve,reject)=>{img.onload=resolve;img.onerror=reject;img.src=URL.createObjectURL(blob);});state.rectifiedImage=img;drawRectifiedGuide();setCalibrationStatus('Rectified preview updated','good');}catch(e){log(`ERR ${e.message}`);setCalibrationStatus(`ERR ${e.message}`,'bad');}}\n"
    "async function loadCalibration(){setCalibrationStatus('Loading saved points...','busy');log('GET /api/calibration');try{const data=await fetchJson('/api/calibration');state.calibration={imageWidth:data.imageWidth,imageHeight:data.imageHeight,points:data.points};applyHorizontalConstraint();applySymmetryConstraint();if(!state.calibrationImage)await loadCalibrationSnapshot();else{ui.calibrationCanvas.width=state.calibration.imageWidth;ui.calibrationCanvas.height=state.calibration.imageHeight;drawCalibrationCanvas();drawRectifiedGuide();}await loadRectifiedPreview();}catch(e){log(`ERR ${e.message}`);setCalibrationStatus(`ERR ${e.message}`,'bad');}}\n"
    "function resetCalibration(){const w=(state.calibrationImage&&state.calibrationImage.width)||state.calibration.imageWidth||640;const h=(state.calibrationImage&&state.calibrationImage.height)||state.calibration.imageHeight||320;state.calibration=makeDefaultCalibration(w,h);applyHorizontalConstraint();applySymmetryConstraint();state.rectifiedImage=null;drawCalibrationCanvas();drawRectifiedGuide();setCalibrationStatus('Calibration points reset','good');}\n"
    "function buildCalibrationQuery(){ensureCalibrationPoints();applyHorizontalConstraint();applySymmetryConstraint();const q=new URLSearchParams();q.set('iw',state.calibration.imageWidth);q.set('ih',state.calibration.imageHeight);state.calibration.points.forEach((p,i)=>{q.set(`x${i}`,p.x);q.set(`y${i}`,p.y);});return q;}\n"
    "function formatLensK1(v){return (Number(v||0)/1000000).toFixed(6);}\n"
    "function formatLensFloat(v,s=3){return Number(v||0).toFixed(s);}\n"
    "function applyLensProfile(data){if(!data)return;state.lens=data;const parts=[`fx ${formatLensFloat(data.fx,3)}`,`fy ${formatLensFloat(data.fy,3)}`,`cx ${formatLensFloat(data.cx,3)}`,`cy ${formatLensFloat(data.cy,3)}`,`k1 ${formatLensK1(data.k1Scaled)}`,`k2 ${formatLensK1(data.k2Scaled)}`,`scale ${formatLensFloat(data.knewScale,3)}x`];setLensStatus(`Lens preset active: ${parts.join(' | ')}`,'good');}\n"
    "async function refreshLensProfile(){log('GET /api/lens_profile');try{const data=await fetchJson('/api/lens_profile');applyLensProfile(data);}catch(e){log(`ERR ${e.message}`);setLensStatus(`ERR ${e.message}`,'bad');}}\n"
    "async function saveCalibration(){const q=buildCalibrationQuery();const url=`/api/calibration/set?${q.toString()}`;setCalibrationStatus('Saving calibration...','busy');log(`GET ${url}`);try{const data=await fetchJson(url);state.calibration={imageWidth:data.imageWidth,imageHeight:data.imageHeight,points:data.points};applyHorizontalConstraint();applySymmetryConstraint();drawCalibrationCanvas();state.rectifiedImage=null;drawRectifiedGuide();await loadRectifiedPreview();}catch(e){log(`ERR ${e.message}`);setCalibrationStatus(`ERR ${e.message}`,'bad');}}\n"
    "async function fetchJson(url){const r=await fetch(url,{cache:'no-store'});if(!r.ok)throw new Error(`HTTP ${r.status}`);return r.json();}\n"
    "async function refreshWiFiStatus(){log('GET /api/wifi');try{const data=await fetchJson('/api/wifi');applyWifiStatus(data);}catch(e){log(`ERR ${e.message}`);if(ui.wifiStatusBadge){ui.wifiStatusBadge.textContent=`ERR ${e.message}`;ui.wifiStatusBadge.classList.remove('is-good','is-busy');ui.wifiStatusBadge.classList.add('is-bad');}}}\n"
    "async function scanWiFi(){log('GET /api/wifi/scan');try{const data=await fetchJson('/api/wifi/scan');applyWifiScan(data);setPreviewStatus('WiFi scan complete','good');}catch(e){log(`ERR ${e.message}`);setPreviewStatus(`ERR ${e.message}`,'bad');}}\n"
    "async function saveWiFi(){const ssid=(ui.wifiSsidInput.value||'').trim();const key=ui.wifiPassInput.value||'';if(!ssid){setPreviewStatus('Enter SSID first','bad');return;}const url=`/api/wifi/save?ssid=${encodeURIComponent(ssid)}&key=${encodeURIComponent(key)}`;log(`GET /api/wifi/save?ssid=${ssid}`);try{const data=await fetchJson(url);applyWifiStatus(data);ui.wifiPassInput.value='';setPreviewStatus(`WiFi saved: ${ssid}`,'good');}catch(e){log(`ERR ${e.message}`);setPreviewStatus(`ERR ${e.message}`,'bad');}}\n"
    "async function forgetWiFi(){log('GET /api/wifi/forget');try{const data=await fetchJson('/api/wifi/forget');applyWifiStatus(data);ui.wifiPassInput.value='';setPreviewStatus('Saved WiFi cleared; fallback AP only','good');}catch(e){log(`ERR ${e.message}`);setPreviewStatus(`ERR ${e.message}`,'bad');}}\n"
    "async function refreshLayout(){log('GET /api/layout');try{const data=await fetchJson('/api/layout');applyLayoutMeta(data);}catch(e){log(`ERR ${e.message}`);}}\n"
    "async function refreshInstallMode(){log('GET /api/install_mode');try{const data=await fetchJson('/api/install_mode');applyInstallModeMeta(data);}catch(e){log(`ERR ${e.message}`);}}\n"
    "async function setStripProfile(value){log(`GET /api/led_strip/set?value=${value}`);try{const data=await fetchJson(`/api/led_strip/set?value=${encodeURIComponent(value)}`);applyInstallModeMeta(data);if(state.mode==='RUN')drawPreviewMosaic();setPreviewStatus(`LED strip updated: ${data.stripLabel}`,'good');}catch(e){log(`ERR ${e.message}`);setPreviewStatus(`ERR ${e.message}`,'bad');}}\n"
    "async function setInstallMode(value){log(`GET /api/install_mode/set?value=${value}`);try{const data=await fetchJson(`/api/install_mode/set?value=${encodeURIComponent(value)}`);applyInstallModeMeta(data);setPreviewStatus(`Install mapping updated: ${data.label}`,'good');}catch(e){log(`ERR ${e.message}`);setPreviewStatus(`ERR ${e.message}`,'bad');}}\n"
    "async function setColorOrder(value){log(`GET /api/color_order/set?value=${value}`);try{const data=await fetchJson(`/api/color_order/set?value=${encodeURIComponent(value)}`);applyInstallModeMeta(data);setPreviewStatus(`LED color order updated: ${data.colorOrder}`,'good');}catch(e){log(`ERR ${e.message}`);setPreviewStatus(`ERR ${e.message}`,'bad');}}\n"
    "async function toggleInstallGuide(){const active=state.installGuideActive?0:1;log(`GET /api/install_guide?active=${active}`);try{const data=await fetchJson(`/api/install_guide?active=${active}`);applyInstallModeMeta(data);setPreviewStatus(state.installGuideActive?'Install guide active: choose direction now':'Install guide locked: saved for next boot','good');}catch(e){log(`ERR ${e.message}`);setPreviewStatus(`ERR ${e.message}`,'bad');}}\n"
    "async function refreshLedPower(){log('GET /api/led_power');try{const data=await fetchJson('/api/led_power');applyLedPower(data);}catch(e){log(`ERR ${e.message}`);}}\n"
    "async function toggleLedPower(){const enabled=state.ledPowerEnabled?0:1;log(`GET /api/led_power/set?enabled=${enabled}`);try{const data=await fetchJson(`/api/led_power/set?enabled=${enabled}`);applyLedPower(data);setPreviewStatus(state.ledPowerEnabled?'LED strip power enabled':'LED strip power disabled','good');}catch(e){log(`ERR ${e.message}`);setPreviewStatus(`ERR ${e.message}`,'bad');}}\n"
    "async function refreshMode(){log('GET /api/mode');try{const data=await fetchJson('/api/mode');setModeUi(data.mode||'DEBUG');}catch(e){log(`ERR ${e.message}`);setPreviewStatus(`ERR ${e.message}`,'bad');}}\n"
    "async function setMode(mode){if(state.autoPreviewTimer){clearInterval(state.autoPreviewTimer);state.autoPreviewTimer=null;ui.autoPreviewBtn.textContent='Start Auto Preview';}log(`GET /api/mode/set?value=${mode}`);setPreviewStatus(`Switching to ${mode} mode...`,'busy');try{const data=await fetchJson(`/api/mode/set?value=${encodeURIComponent(mode)}`);setModeUi(data.mode||mode);}catch(e){log(`ERR ${e.message}`);setPreviewStatus(`ERR ${e.message}`,'bad');}}\n"
    "async function refreshValues(){setPreviewStatus('Refreshing parameters...','busy');log('GET /api/params');try{const data=await fetchJson('/api/params');applyValues(data);setPreviewStatus('Parameters refreshed','good');}catch(e){log(`ERR ${e.message}`);setPreviewStatus(`ERR ${e.message}`,'bad');}}\n"
    "async function pingT23(){log('GET /api/ping');try{await fetchJson('/api/ping');setPreviewStatus('T23 online','good');}catch(e){log(`ERR ${e.message}`);setPreviewStatus(`ERR ${e.message}`,'bad');}}\n"
    "async function setParam(key,value,autoSnap){log(`GET /api/set?key=${key}&value=${value}`);try{const data=await fetchJson(`/api/set?key=${encodeURIComponent(key)}&value=${encodeURIComponent(value)}`);applySingleParamValue(data);setPreviewStatus(`Applied ${key}`,'good');if(autoSnap){await refreshPreviewAndBlocks();}}catch(e){log(`ERR ${e.message}`);setPreviewStatus(`ERR ${e.message}`,'bad');}}\n"
    "async function refreshBorderBlocks(){if(state.blocksBusy)return false;state.blocksBusy=true;log('GET /api/border_blocks');try{state.borderData=await fetchJson('/api/border_blocks');applyLayoutMeta(state.borderData);state.lastBlocksAt=Date.now();return true;}catch(e){log(`ERR ${e.message}`);return false;}finally{state.blocksBusy=false;}}\n"
    "async function refreshRuntimeMosaic(){if(state.runtimeBusy)return;state.runtimeBusy=true;log('GET /api/runtime_mosaic');try{const data=await fetchJson('/api/runtime_mosaic');if(data&&data.pixels&&data.pixels.length){state.previewMosaic=data;drawPreviewMosaic();drawLedMappingCanvas();setPreviewStatus('Run mode mosaic updated','good');}else setPreviewStatus('Run mode warming up...','busy');}catch(e){log(`ERR ${e.message}`);if(String(e.message).includes('HTTP 500'))setPreviewStatus('Run mode warming up...','busy');else setPreviewStatus(`ERR ${e.message}`,'bad');}finally{state.runtimeBusy=false;}}\n"
    "async function captureSnapshot(){if(state.previewBusy)return false;state.previewBusy=true;setPreviewStatus('Capturing snapshot...','busy');const url=`/api/snap?t=${Date.now()}`;log(`GET ${url}`);try{const r=await fetch(url,{cache:'no-store'});if(!r.ok)throw new Error(`HTTP ${r.status}`);const blob=await r.blob();if(state.previewUrl)URL.revokeObjectURL(state.previewUrl);state.previewUrl=URL.createObjectURL(blob);ui.previewImage.src=state.previewUrl;await waitForImage(ui.previewImage);setPreviewStatus('Preview updated','good');return true;}catch(e){log(`ERR ${e.message}`);setPreviewStatus(`ERR ${e.message}`,'bad');return false;}finally{state.previewBusy=false;}}\n"
    "async function refreshPreviewMosaic(){if(state.mosaicBusy)return false;state.mosaicBusy=true;log('GET /api/preview_mosaic');try{state.previewMosaic=await fetchJson('/api/preview_mosaic');state.lastMosaicAt=Date.now();drawPreviewMosaic();drawLedMappingCanvas();return true;}catch(e){log(`ERR ${e.message}`);setPreviewStatus(`ERR ${e.message}`,'bad');return false;}finally{state.mosaicBusy=false;}}\n"
    "function makeSnapshotFilename(kind='raw',ext='jpg'){const d=new Date();const p=n=>String(n).padStart(2,'0');return `t23_${kind}_${d.getFullYear()}${p(d.getMonth()+1)}${p(d.getDate())}_${p(d.getHours())}${p(d.getMinutes())}${p(d.getSeconds())}.${ext}`;}\n"
    "function downloadBlob(blob,name){const href=URL.createObjectURL(blob);const a=document.createElement('a');a.href=href;a.download=name;document.body.appendChild(a);a.click();a.remove();setTimeout(()=>URL.revokeObjectURL(href),1000);}\n"
    "function buildMosaicPngBlob(){const w=(state.previewMosaic&&state.previewMosaic.width)||16,h=(state.previewMosaic&&state.previewMosaic.height)||16,pixels=(state.previewMosaic&&state.previewMosaic.pixels)||[];const canvas=document.createElement('canvas');canvas.width=w;canvas.height=h;const ctx=canvas.getContext('2d');const image=ctx.createImageData(w,h);for(let i=0;i<w*h;i++){image.data[i*4+0]=pixels[i*3+0]||0;image.data[i*4+1]=pixels[i*3+1]||0;image.data[i*4+2]=pixels[i*3+2]||0;image.data[i*4+3]=255;}ctx.putImageData(image,0,0);return new Promise(resolve=>canvas.toBlob(resolve,'image/png'));}\n"
    "async function saveRawSnapshotToLocal(){if(state.mode==='RUN'){setPreviewStatus('Run mode does not provide web preview snapshots','bad');return;}const ok=await captureSnapshot();if(!ok||!state.previewUrl)return;const r=await fetch(state.previewUrl);downloadBlob(await r.blob(),makeSnapshotFilename('raw','jpg'));setPreviewStatus('Raw snapshot saved to local download folder','good');}\n"
    "async function saveRectifiedPreviewToLocal(){if(state.mode==='RUN'){setPreviewStatus('Run mode does not provide web preview snapshots','bad');return;}setPreviewStatus('Capturing rectified preview...','busy');const url=`/api/calibration/rectified?t=${Date.now()}`;log(`GET ${url}`);try{const r=await fetch(url,{cache:'no-store'});if(!r.ok)throw new Error(`HTTP ${r.status}`);downloadBlob(await r.blob(),makeSnapshotFilename('rectified','jpg'));setPreviewStatus('Rectified preview saved to local download folder','good');}catch(e){log(`ERR ${e.message}`);setPreviewStatus(`ERR ${e.message}`,'bad');}}\n"
    "async function saveMosaicSnapshotToLocal(){if(state.mode==='RUN'){setPreviewStatus('Run mode does not provide web preview snapshots','bad');return;}if(!state.previewMosaic)await refreshPreviewMosaic();if(!state.previewMosaic){setPreviewStatus('Mosaic preview not ready','bad');return;}const blob=await buildMosaicPngBlob();if(!blob){setPreviewStatus('Failed to encode mosaic preview','bad');return;}downloadBlob(blob,makeSnapshotFilename('mosaic_16x16','png'));setPreviewStatus('16x16 mosaic saved to local download folder','good');}\n"
    "async function refreshPreviewAndBlocks(){if(state.autoPreviewBusy)return;state.autoPreviewBusy=true;try{const ok=await captureSnapshot();const now=Date.now();if(ok&&state.mode!=='RUN'&&now>=state.debugWarmUntil){if((now-state.lastMosaicAt)>900)void refreshPreviewMosaic();if((now-state.lastBlocksAt)>1400)void refreshBorderBlocks();}}finally{state.autoPreviewBusy=false;}}\n"
    "function startAutoPreview(){if(state.mode==='RUN'){setPreviewStatus('Run mode disables web preview for lowest latency','good');return;}if(state.autoPreviewTimer){clearInterval(state.autoPreviewTimer);state.autoPreviewTimer=null;ui.autoPreviewBtn.textContent='Start Auto Preview';setPreviewStatus('Auto preview stopped');return;}const runner=()=>{void refreshPreviewAndBlocks();};state.debugWarmUntil=Math.max(state.debugWarmUntil||0,Date.now()+800);runner();state.autoPreviewTimer=setInterval(runner,280);ui.autoPreviewBtn.textContent='Stop Auto Preview';setPreviewStatus('Auto preview running','good');}\n"
    "async function bootUi(){await refreshMode();if(state.mode==='RUN'){await refreshInstallMode();await refreshLedPower();await refreshWiFiStatus();drawPreviewMosaic();return;}await refreshLayout();await refreshInstallMode();await refreshLedPower();await refreshLedCal();await refreshLensProfile();await refreshWiFiStatus();await scanWiFi();await refreshValues();await refreshPreviewAndBlocks();await loadCalibration();}\n"
    "ui.snapBtn.onclick=saveRawSnapshotToLocal;ui.saveSnapBtn.onclick=saveRectifiedPreviewToLocal;ui.saveHiResSnapBtn.onclick=saveMosaicSnapshotToLocal;ui.autoPreviewBtn.onclick=startAutoPreview;ui.enterRunBtn.onclick=()=>setMode('RUN');ui.returnDebugBtn.onclick=()=>setMode('DEBUG');ui.wifiScanSelect.onchange=()=>{if(ui.wifiScanSelect.value)ui.wifiSsidInput.value=ui.wifiScanSelect.value;};ui.wifiScanBtn.onclick=scanWiFi;ui.wifiSaveBtn.onclick=saveWiFi;ui.wifiForgetBtn.onclick=forgetWiFi;if(ui.stripProfileSelect)ui.stripProfileSelect.onchange=()=>setStripProfile(ui.stripProfileSelect.value);if(ui.installModeSelect)ui.installModeSelect.onchange=()=>setInstallMode(ui.installModeSelect.value);if(ui.colorOrderSelect)ui.colorOrderSelect.onchange=()=>setColorOrder(ui.colorOrderSelect.value);if(ui.showInstallGuideBtn)ui.showInstallGuideBtn.onclick=toggleInstallGuide;if(ui.ledPowerBtn)ui.ledPowerBtn.onclick=toggleLedPower;if(ui.ledTestLiveBtn)ui.ledTestLiveBtn.onclick=()=>setLedTest('LIVE');ui.saveParamsBtn.onclick=saveStartupParams;ui.restoreDefaultBtn.onclick=restoreDefaultParams;ui.restoreSavedBtn.onclick=restoreSavedParams;ui.clearLogBtn.onclick=()=>{ui.logBox.textContent='';};ui.updateLensBtn.onclick=refreshLensProfile;ui.loadCalibrationSnapBtn.onclick=loadCalibrationSnapshot;ui.resetCalibrationBtn.onclick=resetCalibration;ui.saveCalibrationBtn.onclick=saveCalibration;ui.horizontalLockBtn.onclick=toggleHorizontalLock;ui.symmetryLockBtn.onclick=toggleSymmetryLock;renderParams();renderLedCal();bindCalibrationCanvas();updateHorizontalLockButton();updateSymmetryLockButton();resetCalibration();drawRectifiedGuide();drawPreviewMosaic();bootUi().catch(()=>{});setInterval(()=>{if(state.mode!=='RUN')refreshWiFiStatus();},5000);\n";

static void post_setup_cb(spi_slave_transaction_t *trans)
{
    (void)trans;
    gpio_set_level(PIN_NUM_DATA_READY, 1);
}

static void post_trans_cb(spi_slave_transaction_t *trans)
{
    (void)trans;
    gpio_set_level(PIN_NUM_DATA_READY, 0);
}

static void set_common_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
}

static void json_escape_copy(char *dst, size_t dst_size, const char *src)
{
    size_t di = 0;
    size_t si = 0;

    if (dst_size == 0) {
        return;
    }

    while (src != NULL && src[si] != '\0' && di + 1 < dst_size) {
        char ch = src[si++];

        if ((ch == '\\' || ch == '"') && di + 2 < dst_size) {
            dst[di++] = '\\';
            dst[di++] = ch;
        } else if ((unsigned char)ch >= 0x20) {
            dst[di++] = ch;
        }
    }

    dst[di] = '\0';
}

static esp_err_t send_json(httpd_req_t *req, const char *json)
{
    set_common_headers(req);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t send_error_json(httpd_req_t *req, const char *message, httpd_err_code_t code)
{
    char buf[192];

    snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", message);
    set_common_headers(req);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send_err(req, code, buf);
}

static const bridge_param_t *find_param(const char *name)
{
    size_t i;

    for (i = 0; i < sizeof(g_params) / sizeof(g_params[0]); ++i) {
        if (strcmp(g_params[i].name, name) == 0) {
            return &g_params[i];
        }
    }

    return NULL;
}

static void uart_flush_rx(void)
{
    uart_flush_input(T23_UART_PORT);
}

static esp_err_t uart_send_line(const char *line)
{
    int len = (int)strlen(line);

    ESP_LOGI(TAG, "UART >> %s", line);

    if (uart_write_bytes(T23_UART_PORT, line, len) < 0) {
        return ESP_FAIL;
    }
    if (uart_write_bytes(T23_UART_PORT, "\n", 1) < 0) {
        return ESP_FAIL;
    }
    return uart_wait_tx_done(T23_UART_PORT, pdMS_TO_TICKS(1000));
}

static int uart_read_line(char *buf, size_t buf_size, int timeout_ms)
{
    size_t len = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    while (len + 1 < buf_size) {
        char ch;
        int ret;
        TickType_t now = xTaskGetTickCount();

        if (now >= deadline) {
            break;
        }

        ret = uart_read_bytes(T23_UART_PORT, &ch, 1, pdMS_TO_TICKS(20));
        if (ret <= 0) {
            continue;
        }
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            break;
        }
        buf[len++] = ch;
    }

    buf[len] = '\0';
    if (len > 0) {
        ESP_LOGI(TAG, "UART << %s", buf);
    }
    return (int)len;
}

static esp_err_t bridge_receive_runtime_mosaic_frame_locked(void)
{
    t23_c3_frame_t frame;
    esp_err_t ret = spi_receive_frame(&frame, RUN_SPI_POLL_TIMEOUT_MS);

    if (ret != ESP_OK) {
        return ret;
    }
    if (frame.hdr.magic0 != T23_C3_FRAME_MAGIC0 || frame.hdr.magic1 != T23_C3_FRAME_MAGIC1) {
        ESP_LOGW(TAG, "runtime SPI bad frame magic: %02X %02X", frame.hdr.magic0, frame.hdr.magic1);
        return ESP_FAIL;
    }
    if (frame.hdr.type != T23_C3_FRAME_TYPE_RESP_MOSAIC_RGB ||
        frame.hdr.payload_len != T23_C3_PREVIEW_MOSAIC_RGB_LEN ||
        frame.hdr.total_len != T23_C3_PREVIEW_MOSAIC_RGB_LEN ||
        frame.hdr.offset != 0) {
        ESP_LOGW(TAG, "runtime SPI unexpected frame type=%u len=%u total=%" PRIu32 " offset=%" PRIu32,
                 frame.hdr.type,
                 frame.hdr.payload_len,
                 frame.hdr.total_len,
                 frame.hdr.offset);
        return ESP_FAIL;
    }

    memcpy(g_latest_runtime_mosaic.rgb, frame.payload, sizeof(g_latest_runtime_mosaic.rgb));
    g_latest_runtime_mosaic.ready = 1;
    return ESP_OK;
}

static esp_err_t init_t23_uart(void)
{
    const uart_config_t uart_cfg = {
        .baud_rate = T23_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(T23_UART_PORT, 4096, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(T23_UART_PORT, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(T23_UART_PORT, T23_UART_TX, T23_UART_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    ESP_LOGI(TAG, "T23 UART ready on UART%d tx=%d rx=%d baud=%d",
             (int)T23_UART_PORT, T23_UART_TX, T23_UART_RX, T23_UART_BAUD);
    return ESP_OK;
}

static esp_err_t bridge_ping(void)
{
    char line[UART_LINE_MAX];
    int len;

    uart_flush_rx();
    ESP_ERROR_CHECK(uart_send_line("PING"));

    len = uart_read_line(line, sizeof(line), 1000);
    if (len <= 0) {
        ESP_LOGW(TAG, "PING timeout waiting for UART response");
        return ESP_ERR_TIMEOUT;
    }
    if (strcmp(line, "PONG") != 0) {
        ESP_LOGW(TAG, "unexpected ping response: %s", line);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t bridge_get_mode(c3_mode_t *mode_out)
{
    char line[UART_LINE_MAX];
    int len;

    uart_flush_rx();
    ESP_ERROR_CHECK(uart_send_line("MODE GET"));

    len = uart_read_line(line, sizeof(line), 1000);
    if (len <= 0) {
        ESP_LOGW(TAG, "MODE GET timeout waiting for UART response");
        return ESP_ERR_TIMEOUT;
    }
    if (strcmp(line, "MODE RUN") == 0) {
        *mode_out = C3_MODE_RUN;
        return ESP_OK;
    }
    if (strcmp(line, "MODE DEBUG") == 0) {
        *mode_out = C3_MODE_DEBUG;
        return ESP_OK;
    }

    ESP_LOGW(TAG, "unexpected mode response: %s", line);
    return ESP_FAIL;
}

static esp_err_t bridge_set_mode(c3_mode_t mode, char *json_buf, size_t json_buf_size)
{
    char cmd[32];
    char line[UART_LINE_MAX];
    const char *target = c3_mode_name(mode);
    int len;

    uart_flush_rx();
    snprintf(cmd, sizeof(cmd), "MODE SET %s", target);
    ESP_ERROR_CHECK(uart_send_line(cmd));

    while (1) {
        len = uart_read_line(line, sizeof(line), 1500);
        if (len <= 0) {
            ESP_LOGW(TAG, "MODE SET timeout waiting for UART response");
            return ESP_ERR_TIMEOUT;
        }
        if (strncmp(line, "MODE ", 5) == 0) {
            continue;
        }
        if (strcmp(line, "OK MODE SET RUN") == 0) {
            g_c3_mode = C3_MODE_RUN;
            snprintf(json_buf, json_buf_size, "{\"ok\":true,\"mode\":\"RUN\"}");
            return ESP_OK;
        }
        if (strcmp(line, "OK MODE SET DEBUG") == 0) {
            g_c3_mode = C3_MODE_DEBUG;
            snprintf(json_buf, json_buf_size, "{\"ok\":true,\"mode\":\"DEBUG\"}");
            return ESP_OK;
        }
        if (strncmp(line, "ERR ", 4) == 0) {
            ESP_LOGW(TAG, "MODE SET failed: %s", line);
            return ESP_FAIL;
        }
    }
}

static esp_err_t bridge_get_layout(char *json_buf, size_t json_buf_size)
{
    char line[UART_LINE_MAX];
    char layout_name[16];
    int block_count = 0;
    int top_blocks = 0;
    int right_blocks = 0;
    int bottom_blocks = 0;
    int left_blocks = 0;
    int len;

    uart_flush_rx();
    ESP_ERROR_CHECK(uart_send_line("LAYOUT GET"));

    len = uart_read_line(line, sizeof(line), 1000);
    if (len <= 0) {
        ESP_LOGW(TAG, "LAYOUT GET timeout waiting for UART response");
        return ESP_ERR_TIMEOUT;
    }
    if (sscanf(line,
               "LAYOUT %15s %d %d %d %d %d",
               layout_name,
               &block_count,
               &top_blocks,
               &right_blocks,
               &bottom_blocks,
               &left_blocks) != 6) {
        ESP_LOGW(TAG, "unexpected layout response: %s", line);
        return ESP_FAIL;
    }

    g_current_layout = find_layout_desc(layout_name);
    snprintf(json_buf,
             json_buf_size,
             "{\"ok\":true,\"layout\":\"%s\",\"blockCount\":%d,\"topBlocks\":%d,\"rightBlocks\":%d,\"bottomBlocks\":%d,\"leftBlocks\":%d}",
             layout_name,
             block_count,
             top_blocks,
             right_blocks,
             bottom_blocks,
             left_blocks);
    return ESP_OK;
}

static esp_err_t bridge_set_layout(const char *layout_name, char *json_buf, size_t json_buf_size)
{
    char cmd[32];
    char line[UART_LINE_MAX];
    int block_count = 0;
    int top_blocks = 0;
    int right_blocks = 0;
    int bottom_blocks = 0;
    int left_blocks = 0;
    int len;

    uart_flush_rx();
    snprintf(cmd, sizeof(cmd), "LAYOUT SET %s", layout_name);
    ESP_ERROR_CHECK(uart_send_line(cmd));

    while (1) {
        len = uart_read_line(line, sizeof(line), 1500);
        if (len <= 0) {
            ESP_LOGW(TAG, "LAYOUT SET timeout waiting for UART response");
            return ESP_ERR_TIMEOUT;
        }
        if (sscanf(line,
                   "LAYOUT %15s %d %d %d %d %d",
                   cmd,
                   &block_count,
                   &top_blocks,
                   &right_blocks,
                   &bottom_blocks,
                   &left_blocks) == 6) {
            g_current_layout = find_layout_desc(cmd);
            continue;
        }
        if (strncmp(line, "OK LAYOUT SET ", 14) == 0) {
            snprintf(json_buf,
                     json_buf_size,
                     "{\"ok\":true,\"layout\":\"%s\",\"blockCount\":%d,\"topBlocks\":%d,\"rightBlocks\":%d,\"bottomBlocks\":%d,\"leftBlocks\":%d}",
                     g_current_layout->name,
                     block_count,
                     top_blocks,
                     right_blocks,
                     bottom_blocks,
                     left_blocks);
            return ESP_OK;
        }
        if (strncmp(line, "ERR ", 4) == 0) {
            ESP_LOGW(TAG, "LAYOUT SET failed: %s", line);
            return ESP_FAIL;
        }
    }
}

static esp_err_t bridge_get_all_values(int *values, int *have_value)
{
    char line[UART_LINE_MAX];
    int len;
    size_t i;

    memset(values, 0, sizeof(int) * (sizeof(g_params) / sizeof(g_params[0])));
    memset(have_value, 0, sizeof(int) * (sizeof(g_params) / sizeof(g_params[0])));

    uart_flush_rx();
    ESP_ERROR_CHECK(uart_send_line("GET ALL"));

    while (1) {
        char key[32];
        int value;

        len = uart_read_line(line, sizeof(line), 1500);
        if (len <= 0) {
            ESP_LOGW(TAG, "GET ALL timeout waiting for UART response");
            return ESP_ERR_TIMEOUT;
        }
        if (strcmp(line, "OK GET ALL") == 0) {
            break;
        }
        if (sscanf(line, "VAL %31s %d", key, &value) == 2) {
            for (i = 0; i < sizeof(g_params) / sizeof(g_params[0]); ++i) {
                if (strcmp(g_params[i].name, key) == 0) {
                    values[i] = value;
                    have_value[i] = 1;
                    break;
                }
            }
            continue;
        }
        ESP_LOGW(TAG, "GET ALL unexpected line: %s", line);
    }

    return ESP_OK;
}

static esp_err_t bridge_get_all(char *json_buf, size_t json_buf_size)
{
    int values[sizeof(g_params) / sizeof(g_params[0])] = {0};
    int have_value[sizeof(g_params) / sizeof(g_params[0])] = {0};
    size_t used = 0;
    size_t i;

    if (bridge_get_all_values(values, have_value) != ESP_OK) {
        return ESP_FAIL;
    }

    used += (size_t)snprintf(json_buf + used, json_buf_size - used, "{\"ok\":true");
    for (i = 0; i < sizeof(g_params) / sizeof(g_params[0]); ++i) {
        if (!have_value[i]) {
            continue;
        }
        used += (size_t)snprintf(json_buf + used,
                                 json_buf_size - used,
                                 ",\"%s\":%d",
                                 g_params[i].name,
                                 values[i]);
    }
    snprintf(json_buf + used, json_buf_size - used, "}");
    return ESP_OK;
}

static esp_err_t bridge_set_param(const char *key, int value, char *json_buf, size_t json_buf_size)
{
    char line[UART_LINE_MAX];
    char cmd[64];
    int actual_value = value;
    int len;

    uart_flush_rx();
    snprintf(cmd, sizeof(cmd), "SET %s %d", key, value);
    ESP_ERROR_CHECK(uart_send_line(cmd));

    while (1) {
        char echoed_key[32];
        int echoed_value;

        len = uart_read_line(line, sizeof(line), 1500);
        if (len <= 0) {
            ESP_LOGW(TAG, "SET timeout waiting for UART response");
            return ESP_ERR_TIMEOUT;
        }

        if (sscanf(line, "VAL %31s %d", echoed_key, &echoed_value) == 2) {
            if (strcmp(echoed_key, key) == 0) {
                actual_value = echoed_value;
            }
            continue;
        }

        if (strncmp(line, "OK SET ", 7) == 0) {
            snprintf(json_buf,
                     json_buf_size,
                     "{\"ok\":true,\"key\":\"%s\",\"value\":%d}",
                     key,
                     actual_value);
            return ESP_OK;
        }

        if (strncmp(line, "ERR ", 4) == 0) {
            ESP_LOGW(TAG, "SET failed: %s", line);
            return ESP_FAIL;
        }
    }
}

static esp_err_t capture_current_isp_params_to_slot(saved_isp_params_t *slot, int *valid_flag)
{
    int values[sizeof(g_params) / sizeof(g_params[0])] = {0};
    int have_value[sizeof(g_params) / sizeof(g_params[0])] = {0};
    size_t i;
    esp_err_t ret = bridge_get_all_values(values, have_value);

    if (ret != ESP_OK) {
        return ret;
    }

    memset(slot, 0, sizeof(*slot));
    slot->magic = ISP_SAVE_MAGIC;

    for (i = 0; i < sizeof(g_params) / sizeof(g_params[0]); ++i) {
        slot->values[i] = have_value[i] ? values[i] : 0;
    }
    *valid_flag = 1;
    return ESP_OK;
}

static esp_err_t apply_isp_params_to_t23(const saved_isp_params_t *params)
{
    char json[96];
    size_t i;

    if (params == NULL || params->magic != ISP_SAVE_MAGIC) {
        return ESP_OK;
    }

    for (i = 0; i < sizeof(g_params) / sizeof(g_params[0]); ++i) {
        esp_err_t ret = bridge_set_param(g_params[i].name, (int)params->values[i], json, sizeof(json));

        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "failed to apply saved ISP param %s", g_params[i].name);
            return ret;
        }
    }

    ESP_LOGI(TAG, "saved ISP params applied to T23");
    return ESP_OK;
}

static esp_err_t apply_saved_isp_params_to_t23(void)
{
    if (!g_saved_isp_params_valid) {
        return ESP_OK;
    }
    return apply_isp_params_to_t23(&g_saved_isp_params);
}

static esp_err_t bridge_get_lens_status(int *valid_out,
                                        int32_t *fx_scaled_out,
                                        int32_t *fy_scaled_out,
                                        int32_t *cx_scaled_out,
                                        int32_t *cy_scaled_out,
                                        int32_t *k1_scaled_out,
                                        int32_t *k2_scaled_out,
                                        int32_t *knew_scaled_out)
{
    char line[UART_LINE_MAX];
    int valid = 0;
    int32_t fx_scaled = 0;
    int32_t fy_scaled = 0;
    int32_t cx_scaled = 0;
    int32_t cy_scaled = 0;
    int32_t k1_scaled = 0;
    int32_t k2_scaled = 0;
    int32_t knew_scaled = 0;
    int len;

    uart_flush_rx();
    ESP_ERROR_CHECK(uart_send_line("LENS GET"));

    while (1) {
        len = uart_read_line(line, sizeof(line), 1500);
        if (len <= 0) {
            ESP_LOGW(TAG, "LENS GET timeout waiting for UART response");
            return ESP_ERR_TIMEOUT;
        }
        if (sscanf(line,
                   "LENS PRESET %d %" SCNd32 " %" SCNd32 " %" SCNd32 " %" SCNd32 " %" SCNd32 " %" SCNd32 " %" SCNd32,
                   &valid,
                   &fx_scaled,
                   &fy_scaled,
                   &cx_scaled,
                   &cy_scaled,
                   &k1_scaled,
                   &k2_scaled,
                   &knew_scaled) == 8) {
            continue;
        }
        if (strcmp(line, "OK LENS GET") == 0) {
            *valid_out = valid ? 1 : 0;
            *fx_scaled_out = fx_scaled;
            *fy_scaled_out = fy_scaled;
            *cx_scaled_out = cx_scaled;
            *cy_scaled_out = cy_scaled;
            *k1_scaled_out = k1_scaled;
            *k2_scaled_out = k2_scaled;
            *knew_scaled_out = knew_scaled;
            return ESP_OK;
        }
        if (strncmp(line, "ERR ", 4) == 0) {
            return ESP_FAIL;
        }
    }
}

static esp_err_t capture_and_save_current_isp_params(char *json_buf, size_t json_buf_size)
{
    esp_err_t ret = capture_current_isp_params_to_slot(&g_saved_isp_params, &g_saved_isp_params_valid);

    if (ret != ESP_OK) {
        return ret;
    }

    ret = save_saved_isp_params();
    if (ret != ESP_OK) {
        return ret;
    }

    snprintf(json_buf, json_buf_size, "{\"ok\":true,\"saved\":true}");
    return ESP_OK;
}

static esp_err_t bridge_get_border_blocks(char *json_buf, size_t json_buf_size)
{
    char line[UART_LINE_MAX];
    char layout_name[16] = "16X9";
    int image_w = 640;
    int image_h = 320;
    int rect_left = 0;
    int rect_top = 0;
    int rect_right = 639;
    int rect_bottom = 319;
    int thickness = 8;
    int top_blocks = g_current_layout->top_blocks;
    int right_blocks = g_current_layout->right_blocks;
    int bottom_blocks = g_current_layout->bottom_blocks;
    int left_blocks = g_current_layout->left_blocks;
    int block_count = top_blocks + right_blocks + bottom_blocks + left_blocks;
    int blocks[T23_BORDER_BLOCK_COUNT_MAX][3];
    int got_blocks[T23_BORDER_BLOCK_COUNT_MAX] = {0};
    int len;
    int i;
    size_t used = 0;

    memset(blocks, 0, sizeof(blocks));
    uart_flush_rx();
    ESP_ERROR_CHECK(uart_send_line("BLOCKS GET"));

    while (1) {
        int idx;
        int r;
        int g;
        int b;

        len = uart_read_line(line, sizeof(line), 3000);
        if (len <= 0) {
            ESP_LOGW(TAG, "BLOCKS GET timeout waiting for UART response");
            return ESP_ERR_TIMEOUT;
        }
        if (sscanf(line, "BLOCKS SIZE %d %d", &image_w, &image_h) == 2) {
            continue;
        }
        if (sscanf(line,
                   "BLOCKS META %15s %d %d %d %d %d",
                   layout_name,
                   &block_count,
                   &top_blocks,
                   &right_blocks,
                   &bottom_blocks,
                   &left_blocks) == 6) {
            if (block_count < 0 || block_count > (int)T23_BORDER_BLOCK_COUNT_MAX) {
                ESP_LOGW(TAG, "BLOCKS META invalid count: %d", block_count);
                return ESP_FAIL;
            }
            g_current_layout = find_layout_desc(layout_name);
            continue;
        }
        if (sscanf(line, "BLOCKS RECT %d %d %d %d %d", &rect_left, &rect_top, &rect_right, &rect_bottom, &thickness) == 5) {
            continue;
        }
        if (sscanf(line, "BLOCK %d %d %d %d", &idx, &r, &g, &b) == 4) {
            if (idx >= 0 && idx < block_count) {
                blocks[idx][0] = r;
                blocks[idx][1] = g;
                blocks[idx][2] = b;
                got_blocks[idx] = 1;
            }
            continue;
        }
        if (strcmp(line, "OK BLOCKS GET") == 0) {
            break;
        }
        if (strncmp(line, "ERR ", 4) == 0) {
            ESP_LOGW(TAG, "BLOCKS GET failed: %s", line);
            return ESP_FAIL;
        }
    }

    used += (size_t)snprintf(json_buf + used,
                             json_buf_size - used,
                             "{\"ok\":true,\"layout\":\"%s\",\"blockCount\":%d,\"topBlocks\":%d,\"rightBlocks\":%d,\"bottomBlocks\":%d,\"leftBlocks\":%d,\"imageWidth\":%d,\"imageHeight\":%d,\"rect\":{\"left\":%d,\"top\":%d,\"right\":%d,\"bottom\":%d,\"thickness\":%d},\"blocks\":[",
                             layout_name,
                             block_count,
                             top_blocks,
                             right_blocks,
                             bottom_blocks,
                             left_blocks,
                             image_w,
                             image_h,
                             rect_left,
                             rect_top,
                             rect_right,
                             rect_bottom,
                             thickness);

    for (i = 0; i < block_count; ++i) {
        used += (size_t)snprintf(json_buf + used,
                                 json_buf_size - used,
                                 "%s{\"index\":%d,\"r\":%d,\"g\":%d,\"b\":%d}",
                                 (i == 0) ? "" : ",",
                                 i,
                                 got_blocks[i] ? blocks[i][0] : 0,
                                 got_blocks[i] ? blocks[i][1] : 0,
                                 got_blocks[i] ? blocks[i][2] : 0);
    }
    snprintf(json_buf + used, json_buf_size - used, "]}");
    store_latest_blocks(layout_name,
                        block_count,
                        top_blocks,
                        right_blocks,
                        bottom_blocks,
                        left_blocks,
                        image_w,
                        image_h,
                        rect_left,
                        rect_top,
                        rect_right,
                        rect_bottom,
                        thickness,
                        blocks,
                        got_blocks);
    return ESP_OK;
}

static esp_err_t build_border_blocks_json(char *json_buf,
                                          size_t json_buf_size,
                                          const char *layout_name,
                                          int block_count,
                                          int top_blocks,
                                          int right_blocks,
                                          int bottom_blocks,
                                          int left_blocks,
                                          int image_w,
                                          int image_h,
                                          int rect_left,
                                          int rect_top,
                                          int rect_right,
                                          int rect_bottom,
                                          int thickness,
                                          const int blocks[T23_BORDER_BLOCK_COUNT_MAX][3],
                                          const int got_blocks[T23_BORDER_BLOCK_COUNT_MAX])
{
    size_t used = 0;
    int i;

    used += (size_t)snprintf(json_buf + used,
                             json_buf_size - used,
                             "{\"ok\":true,\"layout\":\"%s\",\"blockCount\":%d,\"topBlocks\":%d,\"rightBlocks\":%d,\"bottomBlocks\":%d,\"leftBlocks\":%d,\"imageWidth\":%d,\"imageHeight\":%d,\"rect\":{\"left\":%d,\"top\":%d,\"right\":%d,\"bottom\":%d,\"thickness\":%d},\"blocks\":[",
                             layout_name,
                             block_count,
                             top_blocks,
                             right_blocks,
                             bottom_blocks,
                             left_blocks,
                             image_w,
                             image_h,
                             rect_left,
                             rect_top,
                             rect_right,
                             rect_bottom,
                             thickness);

    for (i = 0; i < block_count; ++i) {
        used += (size_t)snprintf(json_buf + used,
                                 json_buf_size - used,
                                 "%s{\"index\":%d,\"r\":%d,\"g\":%d,\"b\":%d}",
                                 (i == 0) ? "" : ",",
                                 i,
                                 got_blocks[i] ? blocks[i][0] : 0,
                                 got_blocks[i] ? blocks[i][1] : 0,
                                 got_blocks[i] ? blocks[i][2] : 0);
        if (used >= json_buf_size) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (snprintf(json_buf + used, json_buf_size - used, "]}") >= (int)(json_buf_size - used)) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static int copy_latest_border_json(char *json_buf, size_t json_buf_size)
{
    int valid = 0;

    if (xSemaphoreTake(g_bridge_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (g_latest_border_json_valid) {
            snprintf(json_buf, json_buf_size, "%s", g_latest_border_json);
            valid = 1;
        }
        xSemaphoreGive(g_bridge_lock);
    }

    return valid;
}

static esp_err_t bridge_fetch_frame_locked(uint8_t *jpeg_buf,
                                           size_t jpeg_buf_size,
                                           size_t *jpeg_len_out)
{
    char line[UART_LINE_MAX];
    char layout_name[16] = "16X9";
    int image_w = 640;
    int image_h = 320;
    int rect_left = 0;
    int rect_top = 0;
    int rect_right = 639;
    int rect_bottom = 319;
    int thickness = 8;
    int top_blocks = g_current_layout->top_blocks;
    int right_blocks = g_current_layout->right_blocks;
    int bottom_blocks = g_current_layout->bottom_blocks;
    int left_blocks = g_current_layout->left_blocks;
    int block_count = top_blocks + right_blocks + bottom_blocks + left_blocks;
    int blocks[T23_BORDER_BLOCK_COUNT_MAX][3];
    int got_blocks[T23_BORDER_BLOCK_COUNT_MAX] = {0};
    int len;
    uint32_t jpeg_len = 0;
    uint32_t received = 0;

    memset(blocks, 0, sizeof(blocks));
    *jpeg_len_out = 0;
    uart_flush_rx();
    ESP_ERROR_CHECK(uart_send_line("FRAME"));

    while (1) {
        int idx;
        int r;
        int g;
        int b;

        len = uart_read_line(line, sizeof(line), 3000);
        if (len <= 0) {
            ESP_LOGW(TAG, "FRAME timeout waiting for UART response");
            return ESP_ERR_TIMEOUT;
        }
        if (sscanf(line, "BLOCKS SIZE %d %d", &image_w, &image_h) == 2) {
            continue;
        }
        if (sscanf(line,
                   "BLOCKS META %15s %d %d %d %d %d",
                   layout_name,
                   &block_count,
                   &top_blocks,
                   &right_blocks,
                   &bottom_blocks,
                   &left_blocks) == 6) {
            if (block_count < 0 || block_count > (int)T23_BORDER_BLOCK_COUNT_MAX) {
                ESP_LOGW(TAG, "FRAME invalid block count: %d", block_count);
                return ESP_FAIL;
            }
            g_current_layout = find_layout_desc(layout_name);
            continue;
        }
        if (sscanf(line, "BLOCKS RECT %d %d %d %d %d", &rect_left, &rect_top, &rect_right, &rect_bottom, &thickness) == 5) {
            continue;
        }
        if (sscanf(line, "BLOCK %d %d %d %d", &idx, &r, &g, &b) == 4) {
            if (idx >= 0 && idx < block_count) {
                blocks[idx][0] = r;
                blocks[idx][1] = g;
                blocks[idx][2] = b;
                got_blocks[idx] = 1;
            }
            continue;
        }
        if (strcmp(line, "OK BLOCKS GET") == 0) {
            continue;
        }
        if (sscanf(line, "SNAP OK %" SCNu32, &jpeg_len) == 1) {
            break;
        }
        if (strncmp(line, "ERR ", 4) == 0) {
            ESP_LOGW(TAG, "FRAME failed: %s", line);
            return ESP_FAIL;
        }
        ESP_LOGW(TAG, "FRAME unexpected UART line: %s", line);
    }

    if (jpeg_len == 0 || jpeg_len > jpeg_buf_size) {
        ESP_LOGW(TAG, "FRAME invalid jpeg length: %" PRIu32, jpeg_len);
        return ESP_FAIL;
    }

    while (received < jpeg_len) {
        t23_c3_frame_t frame;
        esp_err_t ret = spi_receive_frame(&frame, 3000);

        if (ret != ESP_OK) {
            return ret;
        }
        if (frame.hdr.magic0 != T23_C3_FRAME_MAGIC0 || frame.hdr.magic1 != T23_C3_FRAME_MAGIC1) {
            ESP_LOGW(TAG, "FRAME bad SPI magic: %02X %02X", frame.hdr.magic0, frame.hdr.magic1);
            return ESP_FAIL;
        }
        if (frame.hdr.type == T23_C3_FRAME_TYPE_RESP_JPEG_INFO) {
            continue;
        }
        if (frame.hdr.type != T23_C3_FRAME_TYPE_RESP_JPEG_DATA) {
            ESP_LOGW(TAG, "FRAME unexpected SPI type: %u", frame.hdr.type);
            return ESP_FAIL;
        }
        if (frame.hdr.payload_len > T23_C3_FRAME_PAYLOAD_MAX) {
            return ESP_FAIL;
        }
        if (received + frame.hdr.payload_len > jpeg_len) {
            return ESP_FAIL;
        }

        memcpy(jpeg_buf + received, frame.payload, frame.hdr.payload_len);
        received += frame.hdr.payload_len;
    }

    if (build_border_blocks_json(g_latest_border_json,
                                 sizeof(g_latest_border_json),
                                 layout_name,
                                 block_count,
                                 top_blocks,
                                 right_blocks,
                                 bottom_blocks,
                                 left_blocks,
                                 image_w,
                                 image_h,
                                 rect_left,
                                 rect_top,
                                 rect_right,
                                 rect_bottom,
                                 thickness,
                                 blocks,
                                 got_blocks) != ESP_OK) {
        g_latest_border_json_valid = 0;
        return ESP_ERR_NO_MEM;
    }

    g_latest_border_json_valid = 1;
    store_latest_blocks(layout_name,
                        block_count,
                        top_blocks,
                        right_blocks,
                        bottom_blocks,
                        left_blocks,
                        image_w,
                        image_h,
                        rect_left,
                        rect_top,
                        rect_right,
                        rect_bottom,
                        thickness,
                        blocks,
                        got_blocks);
    *jpeg_len_out = received;
    return ESP_OK;
}

static esp_err_t bridge_get_calibration(char *json_buf, size_t json_buf_size)
{
    char line[UART_LINE_MAX];
    int width = 640;
    int height = 320;
    int pts[T23_BORDER_POINT_COUNT * 2] = {0};
    int len;
    int i;

    uart_flush_rx();
    ESP_ERROR_CHECK(uart_send_line("CAL GET"));

    while (1) {
        len = uart_read_line(line, sizeof(line), 1500);
        if (len <= 0) {
            ESP_LOGW(TAG, "CAL GET timeout waiting for UART response");
            return ESP_ERR_TIMEOUT;
        }
        if (strncmp(line, "CAL SIZE ", 9) == 0) {
            if (sscanf(line + 9, "%d %d", &width, &height) != 2) {
                return ESP_FAIL;
            }
            continue;
        }
        if (strncmp(line, "CAL POINTS ", 11) == 0) {
            char points_copy[256];
            char *token = NULL;
            char *saveptr = NULL;

            snprintf(points_copy, sizeof(points_copy), "%s", line + 11);
            for (i = 0; i < (int)(T23_BORDER_POINT_COUNT * 2); ++i) {
                token = strtok_r((i == 0) ? points_copy : NULL, " ", &saveptr);
                if (token == NULL) {
                    return ESP_FAIL;
                }
                pts[i] = atoi(token);
            }
            continue;
        }
        if (strcmp(line, "OK CAL GET") == 0) {
            break;
        }
        if (strncmp(line, "ERR ", 4) == 0) {
            return ESP_FAIL;
        }
    }

    snprintf(json_buf, json_buf_size, "{\"ok\":true,\"imageWidth\":%d,\"imageHeight\":%d,\"points\":[", width, height);
    for (i = 0; i < (int)T23_BORDER_POINT_COUNT; ++i) {
        char tmp[48];

        snprintf(tmp, sizeof(tmp), "%s{\"x\":%d,\"y\":%d}", (i == 0) ? "" : ",", pts[i * 2], pts[i * 2 + 1]);
        strncat(json_buf, tmp, json_buf_size - strlen(json_buf) - 1);
    }
    strncat(json_buf, "]}", json_buf_size - strlen(json_buf) - 1);
    return ESP_OK;
}

static esp_err_t bridge_set_calibration(const char *query, char *json_buf, size_t json_buf_size)
{
    char cmd[512];
    char value[16];
    int iw = 0;
    int ih = 0;
    int pts[T23_BORDER_POINT_COUNT * 2];
    int i;
    size_t len = 0;

    memset(pts, 0, sizeof(pts));
    if (httpd_query_key_value(query, "iw", value, sizeof(value)) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    iw = atoi(value);
    if (httpd_query_key_value(query, "ih", value, sizeof(value)) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    ih = atoi(value);
    for (i = 0; i < (int)T23_BORDER_POINT_COUNT; ++i) {
        char keyx[8];
        char keyy[8];

        snprintf(keyx, sizeof(keyx), "x%d", i);
        snprintf(keyy, sizeof(keyy), "y%d", i);
        if (httpd_query_key_value(query, keyx, value, sizeof(value)) != ESP_OK) {
            return ESP_ERR_INVALID_ARG;
        }
        pts[i * 2] = atoi(value);
        if (httpd_query_key_value(query, keyy, value, sizeof(value)) != ESP_OK) {
            return ESP_ERR_INVALID_ARG;
        }
        pts[i * 2 + 1] = atoi(value);
    }

    len = (size_t)snprintf(cmd, sizeof(cmd), "CAL SET %d %d", iw, ih);
    for (i = 0; i < (int)(T23_BORDER_POINT_COUNT * 2) && len + 16 < sizeof(cmd); ++i) {
        len += (size_t)snprintf(cmd + len, sizeof(cmd) - len, " %d", pts[i]);
    }

    uart_flush_rx();
    ESP_ERROR_CHECK(uart_send_line(cmd));

    while (1) {
        int len = uart_read_line(value, sizeof(value), 1500);

        if (len <= 0) {
            ESP_LOGW(TAG, "CAL SET timeout waiting for UART response");
            return ESP_ERR_TIMEOUT;
        }
        if (strcmp(value, "OK CAL SET") == 0) {
            break;
        }
        if (strncmp(value, "ERR ", 4) == 0) {
            return ESP_FAIL;
        }
    }

    return bridge_get_calibration(json_buf, json_buf_size);
}

static esp_err_t init_spi_slave(void)
{
    const spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = T23_C3_SPI_FRAME_SIZE,
    };
    const spi_slave_interface_config_t slvcfg = {
        .mode = 0,
        .spics_io_num = PIN_NUM_CS,
        .queue_size = 1,
        .flags = 0,
        .post_setup_cb = post_setup_cb,
        .post_trans_cb = post_trans_cb,
    };
    const gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << PIN_NUM_DATA_READY,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&io_conf));
    ESP_ERROR_CHECK(gpio_set_level(PIN_NUM_DATA_READY, 0));

    gpio_set_pull_mode(PIN_NUM_MOSI, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(PIN_NUM_CLK, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(PIN_NUM_CS, GPIO_PULLUP_ONLY);

    ESP_ERROR_CHECK(spi_slave_initialize(C3_SPI_HOST, &buscfg, &slvcfg, SPI_DMA_CH_AUTO));

    g_spi_tx_buf = spi_bus_dma_memory_alloc(C3_SPI_HOST, T23_C3_SPI_FRAME_SIZE, 0);
    g_spi_rx_buf = spi_bus_dma_memory_alloc(C3_SPI_HOST, T23_C3_SPI_FRAME_SIZE, 0);
    assert(g_spi_tx_buf && g_spi_rx_buf);

    ESP_LOGI(TAG, "SPI slave ready");
    ESP_LOGI(TAG, "pins: mosi=%d miso=%d clk=%d cs=%d dr=%d",
             PIN_NUM_MOSI, PIN_NUM_MISO, PIN_NUM_CLK, PIN_NUM_CS, PIN_NUM_DATA_READY);
    return ESP_OK;
}

static esp_err_t spi_receive_frame(t23_c3_frame_t *frame, int timeout_ms)
{
    spi_slave_transaction_t t;
    esp_err_t ret;

    memset(g_spi_tx_buf, 0, T23_C3_SPI_FRAME_SIZE);
    memset(g_spi_rx_buf, 0, T23_C3_SPI_FRAME_SIZE);
    memset(&t, 0, sizeof(t));
    t.length = T23_C3_SPI_FRAME_SIZE * 8;
    t.tx_buffer = g_spi_tx_buf;
    t.rx_buffer = g_spi_rx_buf;

    ret = spi_slave_transmit(C3_SPI_HOST, &t, pdMS_TO_TICKS(timeout_ms));
    if (ret != ESP_OK) {
        return ret;
    }

    memcpy(frame, g_spi_rx_buf, sizeof(*frame));
    return ESP_OK;
}

static esp_err_t bridge_fetch_snapshot_locked_ex(const char *command,
                                                 const char *ok_prefix,
                                                 uint8_t *jpeg_buf,
                                                 size_t jpeg_buf_size,
                                                 size_t *jpeg_len_out)
{
    char line[UART_LINE_MAX];
    int len;
    uint32_t jpeg_len = 0;
    uint32_t received = 0;

    *jpeg_len_out = 0;
    uart_flush_rx();
    ESP_ERROR_CHECK(uart_send_line(command));

    while (1) {
        len = uart_read_line(line, sizeof(line), 3000);
        if (len <= 0) {
            ESP_LOGW(TAG, "%s timeout waiting for UART response", command);
            return ESP_ERR_TIMEOUT;
        }
        if (sscanf(line, ok_prefix, &jpeg_len) == 1) {
            break;
        }
        if (strncmp(line, "ERR ", 4) == 0) {
            ESP_LOGW(TAG, "%s failed: %s", command, line);
            return ESP_FAIL;
        }
        ESP_LOGW(TAG, "%s unexpected UART line: %s", command, line);
    }

    if (jpeg_len == 0 || jpeg_len > jpeg_buf_size) {
        ESP_LOGW(TAG, "invalid jpeg length: %" PRIu32, jpeg_len);
        return ESP_FAIL;
    }

    while (received < jpeg_len) {
        t23_c3_frame_t frame;
        esp_err_t ret = spi_receive_frame(&frame, 3000);

        if (ret != ESP_OK) {
            return ret;
        }
        if (frame.hdr.magic0 != T23_C3_FRAME_MAGIC0 || frame.hdr.magic1 != T23_C3_FRAME_MAGIC1) {
            ESP_LOGW(TAG, "SPI bad frame magic: %02X %02X", frame.hdr.magic0, frame.hdr.magic1);
            return ESP_FAIL;
        }
        if (frame.hdr.type == T23_C3_FRAME_TYPE_RESP_JPEG_INFO) {
            continue;
        }
        if (frame.hdr.type != T23_C3_FRAME_TYPE_RESP_JPEG_DATA) {
            ESP_LOGW(TAG, "SPI unexpected frame type: %u", frame.hdr.type);
            return ESP_FAIL;
        }
        if (frame.hdr.payload_len > T23_C3_FRAME_PAYLOAD_MAX) {
            return ESP_FAIL;
        }
        if (received + frame.hdr.payload_len > jpeg_len) {
            return ESP_FAIL;
        }

        memcpy(jpeg_buf + received, frame.payload, frame.hdr.payload_len);
        received += frame.hdr.payload_len;
    }

    *jpeg_len_out = received;
    return ESP_OK;
}

static esp_err_t bridge_fetch_snapshot_locked(uint8_t *jpeg_buf, size_t jpeg_buf_size, size_t *jpeg_len_out)
{
    return bridge_fetch_snapshot_locked_ex("SNAP", "SNAP OK %" SCNu32, jpeg_buf, jpeg_buf_size, jpeg_len_out);
}

static esp_err_t bridge_fetch_hires_snapshot_locked(uint8_t *jpeg_buf, size_t jpeg_buf_size, size_t *jpeg_len_out)
{
    return bridge_fetch_snapshot_locked_ex("SNAP HIRES", "SNAP HIRES OK %" SCNu32, jpeg_buf, jpeg_buf_size, jpeg_len_out);
}

static esp_err_t bridge_fetch_rectified_locked(uint8_t *jpeg_buf, size_t jpeg_buf_size, size_t *jpeg_len_out)
{
    return bridge_fetch_snapshot_locked_ex("CAL SNAP", "CAL SNAP OK %" SCNu32, jpeg_buf, jpeg_buf_size, jpeg_len_out);
}

static esp_err_t bridge_fetch_preview_mosaic_locked(uint8_t *rgb_buf,
                                                    size_t rgb_buf_size,
                                                    size_t *rgb_len_out)
{
    char line[UART_LINE_MAX];
    int width = 0;
    int height = 0;
    uint32_t data_len = 0;
    int len;

    if (rgb_len_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *rgb_len_out = 0;

    uart_flush_rx();
    ESP_ERROR_CHECK(uart_send_line("CAL MOSAIC"));

    while (1) {
        len = uart_read_line(line, sizeof(line), 3000);
        if (len <= 0) {
            ESP_LOGW(TAG, "CAL MOSAIC timeout waiting for UART response");
            return ESP_ERR_TIMEOUT;
        }
        if (sscanf(line, "CAL MOSAIC OK %d %d %" SCNu32, &width, &height, &data_len) == 3) {
            break;
        }
        if (strncmp(line, "ERR ", 4) == 0) {
            ESP_LOGW(TAG, "CAL MOSAIC failed: %s", line);
            return ESP_FAIL;
        }
        ESP_LOGW(TAG, "CAL MOSAIC unexpected UART line: %s", line);
    }

    if (width != (int)T23_C3_PREVIEW_MOSAIC_WIDTH ||
        height != (int)T23_C3_PREVIEW_MOSAIC_HEIGHT ||
        data_len != T23_C3_PREVIEW_MOSAIC_RGB_LEN ||
        data_len > rgb_buf_size) {
        ESP_LOGW(TAG, "CAL MOSAIC invalid payload: %dx%d len=%" PRIu32, width, height, data_len);
        return ESP_FAIL;
    }

    {
        t23_c3_frame_t frame;
        esp_err_t ret = spi_receive_frame(&frame, 3000);

        if (ret != ESP_OK) {
            return ret;
        }
        if (frame.hdr.magic0 != T23_C3_FRAME_MAGIC0 || frame.hdr.magic1 != T23_C3_FRAME_MAGIC1) {
            ESP_LOGW(TAG, "CAL MOSAIC bad SPI magic: %02X %02X", frame.hdr.magic0, frame.hdr.magic1);
            return ESP_FAIL;
        }
        if (frame.hdr.type != T23_C3_FRAME_TYPE_RESP_MOSAIC_RGB ||
            frame.hdr.payload_len != data_len ||
            frame.hdr.total_len != data_len) {
            ESP_LOGW(TAG, "CAL MOSAIC unexpected SPI frame type=%u len=%u total=%" PRIu32,
                     frame.hdr.type,
                     frame.hdr.payload_len,
                     frame.hdr.total_len);
            return ESP_FAIL;
        }
        memcpy(rgb_buf, frame.payload, data_len);
    }

    memcpy(g_latest_preview_mosaic.rgb, rgb_buf, data_len);
    g_latest_preview_mosaic.ready = 1;
    *rgb_len_out = data_len;
    return ESP_OK;
}

static esp_err_t bridge_fetch_calibration_snapshot_locked(uint8_t *jpeg_buf, size_t jpeg_buf_size, size_t *jpeg_len_out)
{
    return bridge_fetch_snapshot_locked_ex("SNAP", "SNAP OK %" SCNu32, jpeg_buf, jpeg_buf_size, jpeg_len_out);
}

static size_t choose_preview_capacity(void)
{
    static const size_t candidates[] = {
        128 * 1024,
        96 * 1024,
        64 * 1024,
        48 * 1024,
        32 * 1024,
    };
    size_t i;

    for (i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        uint8_t *buf = malloc(candidates[i]);

        if (buf != NULL) {
            g_latest_jpeg = buf;
            g_latest_jpeg_capacity = candidates[i];
            return candidates[i];
        }
    }

    return 0;
}

/*
 * Background task for RUN mode. While RUN is active it continuously receives
 * 16x16 runtime mosaics from T23 and immediately refreshes the LED strip.
 * DEBUG mode parks this task so the web UI can keep using the slower
 * request/response bridge safely.
 */
static void runtime_blocks_task(void *arg)
{
    (void)arg;

    while (1) {
        if (g_c3_mode == C3_MODE_RUN) {
            if (xSemaphoreTake(g_bridge_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
                esp_err_t ret = bridge_receive_runtime_mosaic_frame_locked();
                int mosaic_ready = 0;

                if (ret == ESP_OK) {
                    mosaic_ready = g_latest_runtime_mosaic.ready;
                } else {
                    if (ret != ESP_ERR_TIMEOUT) {
                        ESP_LOGW(TAG, "runtime frame receive failed: %s", esp_err_to_name(ret));
                    }
                }
                xSemaphoreGive(g_bridge_lock);

                if (mosaic_ready || g_led_test_mode != LED_TEST_MODE_LIVE || g_install_setup_active) {
                    esp_err_t led_ret = refresh_led_output_from_state();
                    if (led_ret != ESP_OK) {
                        ESP_LOGW(TAG, "LED update failed: %s", esp_err_to_name(led_ret));
                    }
                }
            }
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static esp_err_t bridge_stream_snapshot(httpd_req_t *req)
{
    size_t jpeg_len = 0;
    esp_err_t ret;

    if (g_latest_jpeg == NULL || g_latest_jpeg_capacity == 0) {
        return ESP_FAIL;
    }

    /*
     * Web preview only needs the latest original JPEG. Avoid the heavier FRAME
     * bridge path here because auto-preview already fetches border blocks and
     * the 16x16 mosaic separately. Using plain SNAP keeps DEBUG mode more
     * responsive, especially right after switching back from RUN.
     */
    ret = bridge_fetch_snapshot_locked(g_latest_jpeg, g_latest_jpeg_capacity, &jpeg_len);
    if (ret != ESP_OK || jpeg_len == 0) {
        return ESP_FAIL;
    }

    set_common_headers(req);
    httpd_resp_set_type(req, "image/jpeg");
    return httpd_resp_send(req, (const char *)g_latest_jpeg, jpeg_len);
}

static esp_err_t bridge_stream_snapshot_hires(httpd_req_t *req)
{
    uint8_t *jpeg_buf = NULL;
    size_t jpeg_len = 0;
    esp_err_t ret;

    jpeg_buf = malloc(HIRES_JPEG_MAX_SIZE);
    if (jpeg_buf == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ret = bridge_fetch_hires_snapshot_locked(jpeg_buf, HIRES_JPEG_MAX_SIZE, &jpeg_len);
    if (ret == ESP_OK && jpeg_len > 0) {
        set_common_headers(req);
        httpd_resp_set_type(req, "image/jpeg");
        ret = httpd_resp_send(req, (const char *)jpeg_buf, jpeg_len);
    } else {
        ret = ESP_FAIL;
    }

    free(jpeg_buf);
    return ret;
}

static esp_err_t bridge_stream_rectified(httpd_req_t *req)
{
    size_t jpeg_len = 0;
    esp_err_t ret;

    if (g_latest_jpeg == NULL || g_latest_jpeg_capacity == 0) {
        return ESP_FAIL;
    }

    ret = bridge_fetch_rectified_locked(g_latest_jpeg, g_latest_jpeg_capacity, &jpeg_len);
    if (ret != ESP_OK || jpeg_len == 0) {
        return ESP_FAIL;
    }

    set_common_headers(req);
    httpd_resp_set_type(req, "image/jpeg");
    return httpd_resp_send(req, (const char *)g_latest_jpeg, jpeg_len);
}

static esp_err_t bridge_stream_calibration_snapshot(httpd_req_t *req)
{
    size_t jpeg_len = 0;
    esp_err_t ret;

    if (g_latest_jpeg == NULL || g_latest_jpeg_capacity == 0) {
        return ESP_FAIL;
    }

    ret = bridge_fetch_calibration_snapshot_locked(g_latest_jpeg, g_latest_jpeg_capacity, &jpeg_len);
    if (ret != ESP_OK || jpeg_len == 0) {
        return ESP_FAIL;
    }

    set_common_headers(req);
    httpd_resp_set_type(req, "image/jpeg");
    return httpd_resp_send(req, (const char *)g_latest_jpeg, jpeg_len);
}

static esp_err_t root_handler(httpd_req_t *req)
{
    set_common_headers(req);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, g_index_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t app_js_handler(httpd_req_t *req)
{
    set_common_headers(req);
    httpd_resp_set_type(req, "application/javascript; charset=utf-8");
    return httpd_resp_send(req, g_app_js, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t styles_handler(httpd_req_t *req)
{
    set_common_headers(req);
    httpd_resp_set_type(req, "text/css; charset=utf-8");
    return httpd_resp_send(req, g_styles_css, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t ping_handler(httpd_req_t *req)
{
    esp_err_t ret;

    xSemaphoreTake(g_bridge_lock, portMAX_DELAY);
    ret = bridge_ping();
    xSemaphoreGive(g_bridge_lock);

    if (ret != ESP_OK) {
        return send_error_json(req, "ping failed", HTTPD_500_INTERNAL_SERVER_ERROR);
    }
    return send_json(req, "{\"ok\":true,\"pong\":true}");
}

static esp_err_t mode_get_handler(httpd_req_t *req)
{
    char json[64];

    snprintf(json, sizeof(json), "{\"ok\":true,\"mode\":\"%s\"}", c3_mode_name(g_c3_mode));
    return send_json(req, json);
}

static esp_err_t mode_set_handler(httpd_req_t *req)
{
    char query[64];
    char value[16];
    char json[64];
    c3_mode_t target;
    esp_err_t ret;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return send_error_json(req, "missing mode query", HTTPD_400_BAD_REQUEST);
    }
    if (httpd_query_key_value(query, "value", value, sizeof(value)) != ESP_OK) {
        return send_error_json(req, "missing mode value", HTTPD_400_BAD_REQUEST);
    }

    if (strcmp(value, "RUN") == 0) {
        target = C3_MODE_RUN;
    } else if (strcmp(value, "DEBUG") == 0) {
        target = C3_MODE_DEBUG;
    } else {
        return send_error_json(req, "unknown mode", HTTPD_400_BAD_REQUEST);
    }

    xSemaphoreTake(g_bridge_lock, portMAX_DELAY);
    ret = bridge_set_mode(target, json, sizeof(json));
    xSemaphoreGive(g_bridge_lock);
    g_latest_border_json_valid = 0;

    if (ret != ESP_OK) {
        return send_error_json(req, "mode set failed", HTTPD_500_INTERNAL_SERVER_ERROR);
    }
    if (target == C3_MODE_RUN) {
        reset_blocks_cache(&g_latest_blocks);
        memset(&g_latest_runtime_mosaic, 0, sizeof(g_latest_runtime_mosaic));
        /*
         * RUN mode must always follow the live camera mosaic. If we keep the
         * previous manual test color or install-guide state around, those
         * override the runtime mosaic path and make the strip look "stuck".
         */
        g_led_test_mode = LED_TEST_MODE_LIVE;
        if (g_install_setup_active) {
            g_install_setup_active = 0;
            ret = save_install_config();
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "failed to persist install guide disable for RUN: %s", esp_err_to_name(ret));
            }
        }
    } else {
        /*
         * When leaving RUN, keep the last runtime mosaic alive until fresh
         * DEBUG preview data arrives. This avoids the strip flashing off while
         * auto-preview is just starting back up.
         */
        refresh_led_output_from_state();
    }
    return send_json(req, json);
}

static esp_err_t layout_get_handler(httpd_req_t *req)
{
    esp_err_t ret;
    char json[128];

    xSemaphoreTake(g_bridge_lock, portMAX_DELAY);
    ret = bridge_get_layout(json, sizeof(json));
    xSemaphoreGive(g_bridge_lock);

    if (ret != ESP_OK) {
        return send_error_json(req, "layout get failed", HTTPD_500_INTERNAL_SERVER_ERROR);
    }
    return send_json(req, json);
}

static esp_err_t layout_set_handler(httpd_req_t *req)
{
    char query[64];
    char value[16];
    char json[128];
    esp_err_t ret;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return send_error_json(req, "missing layout query", HTTPD_400_BAD_REQUEST);
    }
    if (httpd_query_key_value(query, "value", value, sizeof(value)) != ESP_OK) {
        return send_error_json(req, "missing layout value", HTTPD_400_BAD_REQUEST);
    }
    if (strcmp(value, "16X9") != 0 && strcmp(value, "4X3") != 0) {
        return send_error_json(req, "unknown layout", HTTPD_400_BAD_REQUEST);
    }

    xSemaphoreTake(g_bridge_lock, portMAX_DELAY);
    ret = bridge_set_layout(value, json, sizeof(json));
    xSemaphoreGive(g_bridge_lock);
    g_latest_border_json_valid = 0;

    if (ret != ESP_OK) {
        return send_error_json(req, "layout set failed", HTTPD_500_INTERNAL_SERVER_ERROR);
    }
    return send_json(req, json);
}

static esp_err_t install_mode_get_handler(httpd_req_t *req)
{
    char json[320];

    snprintf(json,
             sizeof(json),
             "{\"ok\":true,\"installMode\":\"%s\",\"label\":\"%s\",\"colorOrder\":\"%s\",\"stripProfile\":\"%s\",\"stripLabel\":\"%s\",\"installGuideActive\":%s}",
             g_install_mode->name,
             g_install_mode->label,
             g_led_color_order->name,
             g_led_strip_profile->name,
             g_led_strip_profile->label,
             g_install_setup_active ? "true" : "false");
    return send_json(req, json);
}

static esp_err_t install_mode_set_handler(httpd_req_t *req)
{
    char query[96];
    char value[32];
    char json[320];
    const led_install_desc_t *desc;
    esp_err_t ret;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return send_error_json(req, "missing install mode query", HTTPD_400_BAD_REQUEST);
    }
    if (httpd_query_key_value(query, "value", value, sizeof(value)) != ESP_OK) {
        return send_error_json(req, "missing install mode value", HTTPD_400_BAD_REQUEST);
    }

    desc = find_install_mode_desc(value);
    if (strcmp(desc->name, value) != 0) {
        return send_error_json(req, "unknown install mode", HTTPD_400_BAD_REQUEST);
    }
    if (!g_install_setup_active) {
        return send_error_json(req, "install guide locked", HTTPD_400_BAD_REQUEST);
    }

    g_install_mode = desc;
    ret = save_install_config();
    if (ret != ESP_OK) {
        return send_error_json(req, "install mode save failed", HTTPD_500_INTERNAL_SERVER_ERROR);
    }
    ret = show_install_guide_pattern();
    if (ret != ESP_OK) {
        return send_error_json(req, "install guide failed", HTTPD_500_INTERNAL_SERVER_ERROR);
    }

    snprintf(json,
             sizeof(json),
             "{\"ok\":true,\"installMode\":\"%s\",\"label\":\"%s\",\"colorOrder\":\"%s\",\"stripProfile\":\"%s\",\"stripLabel\":\"%s\",\"installGuideActive\":%s}",
             g_install_mode->name,
             g_install_mode->label,
             g_led_color_order->name,
             g_led_strip_profile->name,
             g_led_strip_profile->label,
             g_install_setup_active ? "true" : "false");
    return send_json(req, json);
}

static esp_err_t color_order_set_handler(httpd_req_t *req)
{
    char query[96];
    char value[16];
    char json[320];
    const led_color_order_desc_t *desc;
    esp_err_t ret;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return send_error_json(req, "missing color order query", HTTPD_400_BAD_REQUEST);
    }
    if (httpd_query_key_value(query, "value", value, sizeof(value)) != ESP_OK) {
        return send_error_json(req, "missing color order value", HTTPD_400_BAD_REQUEST);
    }
    if (!g_install_setup_active) {
        return send_error_json(req, "install guide locked", HTTPD_400_BAD_REQUEST);
    }

    desc = find_color_order_desc(value);
    if (strcmp(desc->name, value) != 0) {
        return send_error_json(req, "unknown color order", HTTPD_400_BAD_REQUEST);
    }

    g_led_color_order = desc;
    ret = sm16703sp3_set_color_order(g_led_color_order->order);
    if (ret != ESP_OK) {
        return send_error_json(req, "apply color order failed", HTTPD_500_INTERNAL_SERVER_ERROR);
    }
    ret = save_install_config();
    if (ret != ESP_OK) {
        return send_error_json(req, "save color order failed", HTTPD_500_INTERNAL_SERVER_ERROR);
    }
    ret = show_install_guide_pattern();
    if (ret != ESP_OK) {
        return send_error_json(req, "install guide failed", HTTPD_500_INTERNAL_SERVER_ERROR);
    }

    snprintf(json,
             sizeof(json),
             "{\"ok\":true,\"installMode\":\"%s\",\"label\":\"%s\",\"colorOrder\":\"%s\",\"stripProfile\":\"%s\",\"stripLabel\":\"%s\",\"installGuideActive\":%s}",
             g_install_mode->name,
             g_install_mode->label,
             g_led_color_order->name,
             g_led_strip_profile->name,
             g_led_strip_profile->label,
             g_install_setup_active ? "true" : "false");
    return send_json(req, json);
}

static esp_err_t install_guide_handler(httpd_req_t *req)
{
    char query[96];
    char value[8];
    char json[320];
    esp_err_t ret;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "active", value, sizeof(value)) == ESP_OK) {
        g_install_setup_active = (strcmp(value, "1") == 0 || strcasecmp(value, "true") == 0) ? 1 : 0;
    } else {
        g_install_setup_active = g_install_setup_active ? 0 : 1;
    }

    ret = save_install_config();
    if (ret != ESP_OK) {
        return send_error_json(req, "install guide save failed", HTTPD_500_INTERNAL_SERVER_ERROR);
    }

    if (g_install_setup_active) {
        ret = show_install_guide_pattern();
        if (ret != ESP_OK) {
            return send_error_json(req, "install guide failed", HTTPD_500_INTERNAL_SERVER_ERROR);
        }
    } else {
        ret = refresh_led_output_from_state();
        if (ret != ESP_OK) {
            return send_error_json(req, "install guide clear failed", HTTPD_500_INTERNAL_SERVER_ERROR);
        }
    }

    snprintf(json,
             sizeof(json),
             "{\"ok\":true,\"installMode\":\"%s\",\"label\":\"%s\",\"colorOrder\":\"%s\",\"stripProfile\":\"%s\",\"stripLabel\":\"%s\",\"installGuideActive\":%s}",
             g_install_mode->name,
             g_install_mode->label,
             g_led_color_order->name,
             g_led_strip_profile->name,
             g_led_strip_profile->label,
             g_install_setup_active ? "true" : "false");
    return send_json(req, json);
}

static esp_err_t strip_profile_set_handler(httpd_req_t *req)
{
    char query[96];
    char value[16];
    char json[320];
    const led_strip_profile_desc_t *desc;
    esp_err_t ret;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return send_error_json(req, "missing strip profile query", HTTPD_400_BAD_REQUEST);
    }
    if (httpd_query_key_value(query, "value", value, sizeof(value)) != ESP_OK) {
        return send_error_json(req, "missing strip profile value", HTTPD_400_BAD_REQUEST);
    }
    if (!g_install_setup_active) {
        return send_error_json(req, "install guide locked", HTTPD_400_BAD_REQUEST);
    }

    desc = find_strip_profile_desc(value);
    if (strcmp(desc->name, value) != 0) {
        return send_error_json(req, "unknown strip profile", HTTPD_400_BAD_REQUEST);
    }

    g_led_strip_profile = desc;
    ret = save_install_config();
    if (ret != ESP_OK) {
        return send_error_json(req, "strip profile save failed", HTTPD_500_INTERNAL_SERVER_ERROR);
    }
    ret = refresh_led_output_from_state();
    if (ret != ESP_OK) {
        return send_error_json(req, "strip profile apply failed", HTTPD_500_INTERNAL_SERVER_ERROR);
    }

    snprintf(json,
             sizeof(json),
             "{\"ok\":true,\"installMode\":\"%s\",\"label\":\"%s\",\"colorOrder\":\"%s\",\"stripProfile\":\"%s\",\"stripLabel\":\"%s\",\"installGuideActive\":%s}",
             g_install_mode->name,
             g_install_mode->label,
             g_led_color_order->name,
             g_led_strip_profile->name,
             g_led_strip_profile->label,
             g_install_setup_active ? "true" : "false");
    return send_json(req, json);
}

static void build_led_calibration_json(char *json, size_t json_size)
{
    snprintf(json,
             json_size,
             "{\"ok\":true,\"testMode\":\"%s\",\"customR\":%u,\"customG\":%u,\"customB\":%u}",
             led_test_mode_name(g_led_test_mode),
             (unsigned)g_led_custom_rgb[0],
             (unsigned)g_led_custom_rgb[1],
             (unsigned)g_led_custom_rgb[2]);
}

static esp_err_t led_calibration_get_handler(httpd_req_t *req)
{
    char json[192];

    build_led_calibration_json(json, sizeof(json));
    return send_json(req, json);
}

static esp_err_t led_calibration_test_handler(httpd_req_t *req)
{
    char query[128];
    char value[16];
    char rgb_value[8];
    char json[192];
    esp_err_t ret;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return send_error_json(req, "missing led test query", HTTPD_400_BAD_REQUEST);
    }
    if (httpd_query_key_value(query, "mode", value, sizeof(value)) != ESP_OK) {
        return send_error_json(req, "missing led test mode", HTTPD_400_BAD_REQUEST);
    }

    g_led_test_mode = led_test_mode_from_name(value);
    if (strcmp(value, "LIVE") != 0 && strcmp(led_test_mode_name(g_led_test_mode), value) != 0) {
        return send_error_json(req, "unknown led test mode", HTTPD_400_BAD_REQUEST);
    }
    if (g_led_test_mode == LED_TEST_MODE_CUSTOM) {
        int ch;
        const char *keys[3] = { "r", "g", "b" };

        for (ch = 0; ch < 3; ++ch) {
            if (httpd_query_key_value(query, keys[ch], rgb_value, sizeof(rgb_value)) != ESP_OK) {
                return send_error_json(req, "missing custom rgb value", HTTPD_400_BAD_REQUEST);
            }
            g_led_custom_rgb[ch] = clamp_u8(atoi(rgb_value));
        }
    }

    ret = refresh_led_output_from_state();
    if (ret != ESP_OK) {
        return send_error_json(req, "led test apply failed", HTTPD_500_INTERNAL_SERVER_ERROR);
    }

    build_led_calibration_json(json, sizeof(json));
    return send_json(req, json);
}

static esp_err_t wifi_status_handler(httpd_req_t *req)
{
    char ssid[128];
    char ip[32];
    char json[512];

    json_escape_copy(ssid, sizeof(ssid), g_wifi_sta_ssid);
    json_escape_copy(ip, sizeof(ip), g_wifi_sta_ip);
    snprintf(json,
             sizeof(json),
             "{\"ok\":true,\"configured\":%s,\"connected\":%s,\"ssid\":\"%s\",\"ip\":\"%s\",\"apSsid\":\"%s\",\"apPass\":\"%s\"}",
             g_wifi_sta_configured ? "true" : "false",
             g_wifi_sta_connected ? "true" : "false",
             ssid,
             ip,
             WIFI_AP_SSID,
             WIFI_AP_PASS);
    return send_json(req, json);
}

static esp_err_t wifi_scan_handler(httpd_req_t *req)
{
    wifi_scan_config_t scan_cfg = { 0 };
    wifi_ap_record_t records[WIFI_SCAN_MAX_RESULTS];
    uint16_t count = WIFI_SCAN_MAX_RESULTS;
    char json[4096];
    size_t len = 0;
    uint16_t i;
    esp_err_t ret;

    memset(records, 0, sizeof(records));
    ret = esp_wifi_scan_start(&scan_cfg, true);
    if (ret != ESP_OK) {
        return send_error_json(req, "wifi scan failed", HTTPD_500_INTERNAL_SERVER_ERROR);
    }
    ret = esp_wifi_scan_get_ap_records(&count, records);
    if (ret != ESP_OK) {
        return send_error_json(req, "wifi scan read failed", HTTPD_500_INTERNAL_SERVER_ERROR);
    }

    len += snprintf(json + len, sizeof(json) - len, "{\"ok\":true,\"results\":[");
    for (i = 0; i < count && len + 96 < sizeof(json); ++i) {
        char ssid[96];

        json_escape_copy(ssid, sizeof(ssid), (const char *)records[i].ssid);
        len += snprintf(json + len,
                        sizeof(json) - len,
                        "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d}",
                        (i == 0) ? "" : ",",
                        ssid,
                        records[i].rssi,
                        records[i].authmode);
    }
    snprintf(json + len, sizeof(json) - len, "]}");
    return send_json(req, json);
}

static esp_err_t wifi_save_handler(httpd_req_t *req)
{
    char query[256];
    char ssid[WIFI_SSID_MAX_LEN + 1];
    char key[WIFI_PASS_MAX_LEN + 1];
    esp_err_t ret;

    memset(ssid, 0, sizeof(ssid));
    memset(key, 0, sizeof(key));
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return send_error_json(req, "missing wifi query", HTTPD_400_BAD_REQUEST);
    }
    if (httpd_query_key_value(query, "ssid", ssid, sizeof(ssid)) != ESP_OK || ssid[0] == '\0') {
        return send_error_json(req, "missing ssid", HTTPD_400_BAD_REQUEST);
    }
    httpd_query_key_value(query, "key", key, sizeof(key));

    snprintf(g_wifi_sta_ssid, sizeof(g_wifi_sta_ssid), "%s", ssid);
    snprintf(g_wifi_sta_pass, sizeof(g_wifi_sta_pass), "%s", key);
    g_wifi_sta_configured = 1;
    g_wifi_sta_connected = 0;
    g_wifi_sta_ip[0] = '\0';

    ret = save_wifi_config();
    if (ret != ESP_OK) {
        return send_error_json(req, "wifi save failed", HTTPD_500_INTERNAL_SERVER_ERROR);
    }
    ret = wifi_apply_sta_config();
    if (ret != ESP_OK) {
        return send_error_json(req, "wifi apply failed", HTTPD_500_INTERNAL_SERVER_ERROR);
    }
    esp_wifi_disconnect();
    esp_wifi_connect();
    return wifi_status_handler(req);
}

static esp_err_t wifi_forget_handler(httpd_req_t *req)
{
    esp_err_t ret;

    g_wifi_sta_ssid[0] = '\0';
    g_wifi_sta_pass[0] = '\0';
    g_wifi_sta_ip[0] = '\0';
    g_wifi_sta_connected = 0;
    g_wifi_sta_configured = 0;
    ret = save_wifi_config();
    if (ret != ESP_OK) {
        return send_error_json(req, "wifi forget failed", HTTPD_500_INTERNAL_SERVER_ERROR);
    }
    esp_wifi_disconnect();
    return wifi_status_handler(req);
}

static esp_err_t params_handler(httpd_req_t *req)
{
    esp_err_t ret;
    char json[512];

    xSemaphoreTake(g_bridge_lock, portMAX_DELAY);
    ret = bridge_get_all(json, sizeof(json));
    xSemaphoreGive(g_bridge_lock);

    if (ret != ESP_OK) {
        return send_error_json(req, "get_all failed", HTTPD_500_INTERNAL_SERVER_ERROR);
    }
    return send_json(req, json);
}

static esp_err_t params_save_handler(httpd_req_t *req)
{
    char json[128];
    esp_err_t ret;

    xSemaphoreTake(g_bridge_lock, portMAX_DELAY);
    ret = capture_and_save_current_isp_params(json, sizeof(json));
    xSemaphoreGive(g_bridge_lock);

    if (ret != ESP_OK) {
        return send_error_json(req, "params save failed", HTTPD_500_INTERNAL_SERVER_ERROR);
    }
    return send_json(req, json);
}

static esp_err_t params_restore_default_handler(httpd_req_t *req)
{
    char json[512];
    esp_err_t ret;

    if (!g_default_isp_params_valid) {
        return send_error_json(req, "default params not captured yet", HTTPD_500_INTERNAL_SERVER_ERROR);
    }

    xSemaphoreTake(g_bridge_lock, portMAX_DELAY);
    ret = apply_isp_params_to_t23(&g_default_isp_params);
    if (ret == ESP_OK) {
        ret = bridge_get_all(json, sizeof(json));
    }
    xSemaphoreGive(g_bridge_lock);

    if (ret != ESP_OK) {
        return send_error_json(req, "restore default params failed", HTTPD_500_INTERNAL_SERVER_ERROR);
    }
    return send_json(req, json);
}

static esp_err_t params_restore_saved_handler(httpd_req_t *req)
{
    char json[512];
    esp_err_t ret;

    if (!g_saved_isp_params_valid) {
        return send_error_json(req, "saved startup params not found", HTTPD_500_INTERNAL_SERVER_ERROR);
    }

    xSemaphoreTake(g_bridge_lock, portMAX_DELAY);
    ret = apply_isp_params_to_t23(&g_saved_isp_params);
    if (ret == ESP_OK) {
        ret = bridge_get_all(json, sizeof(json));
    }
    xSemaphoreGive(g_bridge_lock);

    if (ret != ESP_OK) {
        return send_error_json(req, "restore saved params failed", HTTPD_500_INTERNAL_SERVER_ERROR);
    }
    return send_json(req, json);
}

static esp_err_t set_handler(httpd_req_t *req)
{
    char query[128];
    char key[32];
    char value_str[16];
    char json[128];
    const bridge_param_t *param;
    int value;
    esp_err_t ret;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return send_error_json(req, "missing query", HTTPD_400_BAD_REQUEST);
    }
    if (httpd_query_key_value(query, "key", key, sizeof(key)) != ESP_OK) {
        return send_error_json(req, "missing key", HTTPD_400_BAD_REQUEST);
    }
    if (httpd_query_key_value(query, "value", value_str, sizeof(value_str)) != ESP_OK) {
        return send_error_json(req, "missing value", HTTPD_400_BAD_REQUEST);
    }

    param = find_param(key);
    if (param == NULL) {
        return send_error_json(req, "unknown parameter", HTTPD_400_BAD_REQUEST);
    }

    value = atoi(value_str);

    xSemaphoreTake(g_bridge_lock, portMAX_DELAY);
    ret = bridge_set_param(param->name, value, json, sizeof(json));
    xSemaphoreGive(g_bridge_lock);
    g_latest_border_json_valid = 0;

    if (ret != ESP_OK) {
        return send_error_json(req, "set failed", HTTPD_500_INTERNAL_SERVER_ERROR);
    }
    return send_json(req, json);
}

static esp_err_t snap_handler(httpd_req_t *req)
{
    esp_err_t ret;

    xSemaphoreTake(g_bridge_lock, portMAX_DELAY);
    ret = bridge_stream_snapshot(req);
    xSemaphoreGive(g_bridge_lock);

    if (ret != ESP_OK) {
        return send_error_json(req, "snap failed", HTTPD_500_INTERNAL_SERVER_ERROR);
    }
    return ESP_OK;
}

static esp_err_t snap_hires_handler(httpd_req_t *req)
{
    esp_err_t ret;

    xSemaphoreTake(g_bridge_lock, portMAX_DELAY);
    ret = bridge_stream_snapshot_hires(req);
    xSemaphoreGive(g_bridge_lock);

    if (ret != ESP_OK) {
        return send_error_json(req, "hires snap failed", HTTPD_500_INTERNAL_SERVER_ERROR);
    }
    return ESP_OK;
}

static esp_err_t calibration_get_handler(httpd_req_t *req)
{
    esp_err_t ret;
    char json[512];

    xSemaphoreTake(g_bridge_lock, portMAX_DELAY);
    ret = bridge_get_calibration(json, sizeof(json));
    xSemaphoreGive(g_bridge_lock);

    if (ret != ESP_OK) {
        return send_error_json(req, "calibration get failed", HTTPD_500_INTERNAL_SERVER_ERROR);
    }
    return send_json(req, json);
}

static esp_err_t calibration_set_handler(httpd_req_t *req)
{
    char query[512];
    char json[512];
    esp_err_t ret;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return send_error_json(req, "missing calibration query", HTTPD_400_BAD_REQUEST);
    }

    xSemaphoreTake(g_bridge_lock, portMAX_DELAY);
    ret = bridge_set_calibration(query, json, sizeof(json));
    xSemaphoreGive(g_bridge_lock);
    g_latest_border_json_valid = 0;

    if (ret != ESP_OK) {
        return send_error_json(req, "calibration set failed", HTTPD_500_INTERNAL_SERVER_ERROR);
    }
    return send_json(req, json);
}

static esp_err_t calibration_rectified_handler(httpd_req_t *req)
{
    esp_err_t ret;

    xSemaphoreTake(g_bridge_lock, portMAX_DELAY);
    ret = bridge_stream_rectified(req);
    xSemaphoreGive(g_bridge_lock);

    if (ret != ESP_OK) {
        return send_error_json(req, "calibration rectified failed", HTTPD_500_INTERNAL_SERVER_ERROR);
    }
    return ESP_OK;
}

static esp_err_t preview_mosaic_handler(httpd_req_t *req)
{
    uint8_t rgb[T23_C3_PREVIEW_MOSAIC_RGB_LEN];
    preview_mosaic_cache_t cache = {0};
    size_t rgb_len = 0;
    esp_err_t ret;

    xSemaphoreTake(g_bridge_lock, portMAX_DELAY);
    ret = bridge_fetch_preview_mosaic_locked(rgb, sizeof(rgb), &rgb_len);
    if (ret == ESP_OK && rgb_len == T23_C3_PREVIEW_MOSAIC_RGB_LEN) {
        memcpy(cache.rgb, rgb, sizeof(cache.rgb));
        cache.ready = 1;
    }
    xSemaphoreGive(g_bridge_lock);

    if (ret != ESP_OK) {
        return send_error_json(req, "preview mosaic failed", HTTPD_500_INTERNAL_SERVER_ERROR);
    }
    refresh_led_strip_from_latest_blocks_if_allowed();
    return send_preview_mosaic_json(req, &cache);
}

static esp_err_t calibration_snapshot_handler(httpd_req_t *req)
{
    esp_err_t ret;

    xSemaphoreTake(g_bridge_lock, portMAX_DELAY);
    ret = bridge_stream_calibration_snapshot(req);
    xSemaphoreGive(g_bridge_lock);

    if (ret != ESP_OK) {
        return send_error_json(req, "calibration snapshot failed", HTTPD_500_INTERNAL_SERVER_ERROR);
    }
    return ESP_OK;
}

static esp_err_t lens_profile_get_handler(httpd_req_t *req)
{
    char json[320];
    int valid = 0;
    int32_t fx_scaled = 0;
    int32_t fy_scaled = 0;
    int32_t cx_scaled = 0;
    int32_t cy_scaled = 0;
    int32_t k1_scaled = 0;
    int32_t k2_scaled = 0;
    int32_t knew_scaled = 0;
    esp_err_t ret;

    xSemaphoreTake(g_bridge_lock, portMAX_DELAY);
    ret = bridge_get_lens_status(&valid,
                                 &fx_scaled,
                                 &fy_scaled,
                                 &cx_scaled,
                                 &cy_scaled,
                                 &k1_scaled,
                                 &k2_scaled,
                                 &knew_scaled);
    xSemaphoreGive(g_bridge_lock);
    if (ret != ESP_OK) {
        return send_error_json(req, "lens profile get failed", HTTPD_500_INTERNAL_SERVER_ERROR);
    }

    snprintf(json,
             sizeof(json),
             "{\"ok\":true,\"deviceValid\":%s,\"fx\":%.3f,\"fy\":%.3f,\"cx\":%.3f,\"cy\":%.3f,\"k1Scaled\":%" PRId32 ",\"k2Scaled\":%" PRId32 ",\"knewScale\":%.3f}",
             valid ? "true" : "false",
             (double)fx_scaled / 1000.0,
             (double)fy_scaled / 1000.0,
             (double)cx_scaled / 1000.0,
             (double)cy_scaled / 1000.0,
             k1_scaled,
             k2_scaled,
             (double)knew_scaled / 1000.0);
    return send_json(req, json);
}

static void build_led_power_json(char *json, size_t json_size)
{
    snprintf(json,
             json_size,
             "{\"ok\":true,\"enabled\":%s}",
             g_led_power_enabled ? "true" : "false");
}

static esp_err_t led_power_get_handler(httpd_req_t *req)
{
    char json[64];

    build_led_power_json(json, sizeof(json));
    return send_json(req, json);
}

static esp_err_t led_power_set_handler(httpd_req_t *req)
{
    char query[64];
    char enabled_str[8];
    char json[64];
    int enabled = 1;
    esp_err_t ret = ESP_OK;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return send_error_json(req, "missing led power query", HTTPD_400_BAD_REQUEST);
    }
    if (httpd_query_key_value(query, "enabled", enabled_str, sizeof(enabled_str)) != ESP_OK) {
        return send_error_json(req, "missing led power value", HTTPD_400_BAD_REQUEST);
    }

    enabled = atoi(enabled_str) ? 1 : 0;
    set_led_power_enabled(enabled);
    if (g_led_power_enabled) {
        ret = refresh_led_output_from_state();
    }
    if (ret != ESP_OK) {
        return send_error_json(req, "led power apply failed", HTTPD_500_INTERNAL_SERVER_ERROR);
    }

    build_led_power_json(json, sizeof(json));
    return send_json(req, json);
}

static const char *logical_edge_name(logical_edge_t edge)
{
    switch (edge) {
    case LOGICAL_EDGE_TOP:
        return "TOP";
    case LOGICAL_EDGE_RIGHT:
        return "RIGHT";
    case LOGICAL_EDGE_BOTTOM:
        return "BOTTOM";
    case LOGICAL_EDGE_LEFT:
    default:
        return "LEFT";
    }
}

static int logical_edge_from_name(const char *name, logical_edge_t *edge_out)
{
    if (name == NULL || edge_out == NULL) {
        return 0;
    }
    if (strcmp(name, "TOP") == 0) {
        *edge_out = LOGICAL_EDGE_TOP;
        return 1;
    }
    if (strcmp(name, "RIGHT") == 0) {
        *edge_out = LOGICAL_EDGE_RIGHT;
        return 1;
    }
    if (strcmp(name, "BOTTOM") == 0) {
        *edge_out = LOGICAL_EDGE_BOTTOM;
        return 1;
    }
    if (strcmp(name, "LEFT") == 0) {
        *edge_out = LOGICAL_EDGE_LEFT;
        return 1;
    }
    return 0;
}

static const led_strip_profile_desc_t *resolve_mapping_strip_profile(const char *query)
{
    char value[16];

    if (query != NULL && httpd_query_key_value(query, "profile", value, sizeof(value)) == ESP_OK) {
        const led_strip_profile_desc_t *desc = find_strip_profile_desc(value);

        if (strcmp(desc->name, value) == 0) {
            return desc;
        }
        return NULL;
    }
    return g_led_strip_profile;
}

static esp_err_t build_led_mapping_json(char *json, size_t json_size, const led_strip_profile_desc_t *strip)
{
    const led_mapping_profile_t *profile = led_mapping_profile_from_strip(strip);
    size_t used = 0;
    int offset = 0;
    int counts[4];
    logical_edge_t edges[4] = {
        LOGICAL_EDGE_TOP,
        LOGICAL_EDGE_RIGHT,
        LOGICAL_EDGE_BOTTOM,
        LOGICAL_EDGE_LEFT
    };
    int e;

    if (json == NULL || json_size == 0 || profile == NULL || strip == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    counts[0] = profile->top_count;
    counts[1] = profile->right_count;
    counts[2] = profile->bottom_count;
    counts[3] = profile->left_count;
    used += (size_t)snprintf(json + used,
                             json_size - used,
                             "{\"ok\":true,\"profile\":\"%s\",\"label\":\"%s\",\"topCount\":%u,\"rightCount\":%u,\"bottomCount\":%u,\"leftCount\":%u,\"cells\":[",
                             strip->name,
                             strip->label,
                             (unsigned)profile->top_count,
                             (unsigned)profile->right_count,
                             (unsigned)profile->bottom_count,
                             (unsigned)profile->left_count);
    if (used >= json_size) {
        return ESP_ERR_NO_MEM;
    }

    for (e = 0; e < 4; ++e) {
        int i;

        for (i = 0; i < counts[e]; ++i) {
            const led_map_cell_t *cell = &profile->cells[offset + i];

            used += (size_t)snprintf(json + used,
                                     json_size - used,
                                     "%s{\"edge\":\"%s\",\"index\":%d,\"x\":%u,\"y\":%u}",
                                     (offset == 0 && i == 0) ? "" : ",",
                                     logical_edge_name(edges[e]),
                                     i,
                                     (unsigned)cell->x,
                                     (unsigned)cell->y);
            if (used >= json_size) {
                return ESP_ERR_NO_MEM;
            }
        }
        offset += counts[e];
    }

    if (snprintf(json + used, json_size - used, "]}") >= (int)(json_size - used)) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t led_mapping_get_handler(httpd_req_t *req)
{
    char query[96];
    char json[4096];
    const led_strip_profile_desc_t *strip = g_led_strip_profile;
    esp_err_t ret;

    if (httpd_req_get_url_query_len(req) > 0 &&
        httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        strip = resolve_mapping_strip_profile(query);
    } else {
        strip = g_led_strip_profile;
    }
    if (strip == NULL) {
        return send_error_json(req, "unknown strip profile", HTTPD_400_BAD_REQUEST);
    }

    ret = build_led_mapping_json(json, sizeof(json), strip);
    if (ret != ESP_OK) {
        return send_error_json(req, "mapping json failed", HTTPD_500_INTERNAL_SERVER_ERROR);
    }
    return send_json(req, json);
}

static esp_err_t led_mapping_set_handler(httpd_req_t *req)
{
    char query[128];
    char edge_name[16];
    char value[16];
    char json[4096];
    const led_strip_profile_desc_t *strip = NULL;
    led_mapping_profile_t *profile = NULL;
    logical_edge_t edge = LOGICAL_EDGE_TOP;
    int edge_offset = 0;
    int edge_count = 0;
    int index = 0;
    int x = 0;
    int y = 0;
    esp_err_t ret;

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return send_error_json(req, "missing mapping query", HTTPD_400_BAD_REQUEST);
    }
    strip = resolve_mapping_strip_profile(query);
    if (strip == NULL) {
        return send_error_json(req, "unknown strip profile", HTTPD_400_BAD_REQUEST);
    }
    profile = led_mapping_profile_mut_from_name(strip->name);
    if (profile == NULL) {
        return send_error_json(req, "mapping profile missing", HTTPD_500_INTERNAL_SERVER_ERROR);
    }
    if (httpd_query_key_value(query, "edge", edge_name, sizeof(edge_name)) != ESP_OK ||
        !logical_edge_from_name(edge_name, &edge)) {
        return send_error_json(req, "invalid edge", HTTPD_400_BAD_REQUEST);
    }
    if (httpd_query_key_value(query, "index", value, sizeof(value)) != ESP_OK) {
        return send_error_json(req, "missing index", HTTPD_400_BAD_REQUEST);
    }
    index = atoi(value);
    if (httpd_query_key_value(query, "x", value, sizeof(value)) != ESP_OK) {
        return send_error_json(req, "missing x", HTTPD_400_BAD_REQUEST);
    }
    x = atoi(value);
    if (httpd_query_key_value(query, "y", value, sizeof(value)) != ESP_OK) {
        return send_error_json(req, "missing y", HTTPD_400_BAD_REQUEST);
    }
    y = atoi(value);

    get_mapping_edge_span(profile, edge, &edge_offset, &edge_count);
    if (index < 0 || index >= edge_count ||
        x < 0 || x >= (int)T23_C3_PREVIEW_MOSAIC_WIDTH ||
        y < 0 || y >= (int)T23_C3_PREVIEW_MOSAIC_HEIGHT) {
        return send_error_json(req, "mapping coordinates out of range", HTTPD_400_BAD_REQUEST);
    }

    profile->cells[edge_offset + index].x = (uint8_t)x;
    profile->cells[edge_offset + index].y = (uint8_t)y;

    if (strip == g_led_strip_profile) {
        ret = refresh_led_output_from_state();
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            return send_error_json(req, "mapping apply failed", HTTPD_500_INTERNAL_SERVER_ERROR);
        }
    }

    ret = build_led_mapping_json(json, sizeof(json), strip);
    if (ret != ESP_OK) {
        return send_error_json(req, "mapping json failed", HTTPD_500_INTERNAL_SERVER_ERROR);
    }
    return send_json(req, json);
}

static esp_err_t led_mapping_reset_handler(httpd_req_t *req)
{
    char query[96];
    char json[4096];
    const led_strip_profile_desc_t *strip = g_led_strip_profile;
    led_mapping_profile_t *profile = NULL;
    esp_err_t ret;

    if (httpd_req_get_url_query_len(req) > 0 &&
        httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        strip = resolve_mapping_strip_profile(query);
    }
    if (strip == NULL) {
        return send_error_json(req, "unknown strip profile", HTTPD_400_BAD_REQUEST);
    }

    profile = led_mapping_profile_mut_from_name(strip->name);
    reset_led_mapping_profile(profile, strip);

    if (strip == g_led_strip_profile) {
        ret = refresh_led_output_from_state();
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            return send_error_json(req, "mapping apply failed", HTTPD_500_INTERNAL_SERVER_ERROR);
        }
    }

    ret = build_led_mapping_json(json, sizeof(json), strip);
    if (ret != ESP_OK) {
        return send_error_json(req, "mapping json failed", HTTPD_500_INTERNAL_SERVER_ERROR);
    }
    return send_json(req, json);
}

static esp_err_t led_mapping_save_handler(httpd_req_t *req)
{
    char query[96];
    char json[4096];
    const led_strip_profile_desc_t *strip = g_led_strip_profile;
    esp_err_t ret;

    if (httpd_req_get_url_query_len(req) > 0 &&
        httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        strip = resolve_mapping_strip_profile(query);
    }
    if (strip == NULL) {
        return send_error_json(req, "unknown strip profile", HTTPD_400_BAD_REQUEST);
    }

    ret = save_led_mapping_profile(strip);
    if (ret != ESP_OK) {
        return send_error_json(req, "mapping save failed", HTTPD_500_INTERNAL_SERVER_ERROR);
    }
    ret = build_led_mapping_json(json, sizeof(json), strip);
    if (ret != ESP_OK) {
        return send_error_json(req, "mapping json failed", HTTPD_500_INTERNAL_SERVER_ERROR);
    }
    return send_json(req, json);
}

static esp_err_t border_blocks_handler(httpd_req_t *req)
{
    esp_err_t ret;
    char json[4096];

    if (copy_latest_border_json(json, sizeof(json))) {
        return send_json(req, json);
    }

    xSemaphoreTake(g_bridge_lock, portMAX_DELAY);
    ret = bridge_get_border_blocks(json, sizeof(json));
    if (ret == ESP_OK) {
        snprintf(g_latest_border_json, sizeof(g_latest_border_json), "%s", json);
        g_latest_border_json_valid = 1;
    } else {
        g_latest_border_json_valid = 0;
    }
    xSemaphoreGive(g_bridge_lock);

    if (ret != ESP_OK) {
        return send_error_json(req, "border blocks failed", HTTPD_500_INTERNAL_SERVER_ERROR);
    }
    return send_json(req, json);
}

/*
 * Lightweight RUN status endpoint. The webpage can poll this for a human
 * friendly summary, but RUN mode itself no longer depends on web polling for
 * LED updates.
 */
static esp_err_t runtime_mosaic_handler(httpd_req_t *req)
{
    preview_mosaic_cache_t cache = {0};

    xSemaphoreTake(g_bridge_lock, portMAX_DELAY);
    memcpy(&cache, &g_latest_runtime_mosaic, sizeof(cache));
    xSemaphoreGive(g_bridge_lock);
    if (!cache.ready) {
        return send_json(req, "{\"ok\":true,\"warming\":true,\"width\":16,\"height\":16,\"pixels\":[]}");
    }
    return send_preview_mosaic_json(req, &cache);
}

static esp_err_t start_http_server(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t app_js = {
        .uri = "/app.js",
        .method = HTTP_GET,
        .handler = app_js_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t styles = {
        .uri = "/styles.css",
        .method = HTTP_GET,
        .handler = styles_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t ping = {
        .uri = "/api/ping",
        .method = HTTP_GET,
        .handler = ping_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t layout_get = {
        .uri = "/api/layout",
        .method = HTTP_GET,
        .handler = layout_get_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t layout_set = {
        .uri = "/api/layout/set",
        .method = HTTP_GET,
        .handler = layout_set_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t install_mode_get = {
        .uri = "/api/install_mode",
        .method = HTTP_GET,
        .handler = install_mode_get_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t install_mode_set = {
        .uri = "/api/install_mode/set",
        .method = HTTP_GET,
        .handler = install_mode_set_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t strip_profile_set = {
        .uri = "/api/led_strip/set",
        .method = HTTP_GET,
        .handler = strip_profile_set_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t color_order_set = {
        .uri = "/api/color_order/set",
        .method = HTTP_GET,
        .handler = color_order_set_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t install_guide = {
        .uri = "/api/install_guide",
        .method = HTTP_GET,
        .handler = install_guide_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t led_cal_get = {
        .uri = "/api/led_cal",
        .method = HTTP_GET,
        .handler = led_calibration_get_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t led_cal_test = {
        .uri = "/api/led_cal/test",
        .method = HTTP_GET,
        .handler = led_calibration_test_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t wifi_status = {
        .uri = "/api/wifi",
        .method = HTTP_GET,
        .handler = wifi_status_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t wifi_scan = {
        .uri = "/api/wifi/scan",
        .method = HTTP_GET,
        .handler = wifi_scan_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t wifi_save = {
        .uri = "/api/wifi/save",
        .method = HTTP_GET,
        .handler = wifi_save_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t wifi_forget = {
        .uri = "/api/wifi/forget",
        .method = HTTP_GET,
        .handler = wifi_forget_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t params = {
        .uri = "/api/params",
        .method = HTTP_GET,
        .handler = params_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t params_save = {
        .uri = "/api/params/save",
        .method = HTTP_GET,
        .handler = params_save_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t params_restore_default = {
        .uri = "/api/params/restore_default",
        .method = HTTP_GET,
        .handler = params_restore_default_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t params_restore_saved = {
        .uri = "/api/params/restore_saved",
        .method = HTTP_GET,
        .handler = params_restore_saved_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t set = {
        .uri = "/api/set",
        .method = HTTP_GET,
        .handler = set_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t snap = {
        .uri = "/api/snap",
        .method = HTTP_GET,
        .handler = snap_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t snap_hires = {
        .uri = "/api/snap_hires",
        .method = HTTP_GET,
        .handler = snap_hires_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t mode_get = {
        .uri = "/api/mode",
        .method = HTTP_GET,
        .handler = mode_get_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t mode_set = {
        .uri = "/api/mode/set",
        .method = HTTP_GET,
        .handler = mode_set_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t calibration_get = {
        .uri = "/api/calibration",
        .method = HTTP_GET,
        .handler = calibration_get_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t calibration_set = {
        .uri = "/api/calibration/set",
        .method = HTTP_GET,
        .handler = calibration_set_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t calibration_snapshot = {
        .uri = "/api/calibration/snapshot",
        .method = HTTP_GET,
        .handler = calibration_snapshot_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t calibration_rectified = {
        .uri = "/api/calibration/rectified",
        .method = HTTP_GET,
        .handler = calibration_rectified_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t preview_mosaic = {
        .uri = "/api/preview_mosaic",
        .method = HTTP_GET,
        .handler = preview_mosaic_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t lens_profile_get = {
        .uri = "/api/lens_profile",
        .method = HTTP_GET,
        .handler = lens_profile_get_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t led_power_get = {
        .uri = "/api/led_power",
        .method = HTTP_GET,
        .handler = led_power_get_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t led_power_set = {
        .uri = "/api/led_power/set",
        .method = HTTP_GET,
        .handler = led_power_set_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t led_mapping_get = {
        .uri = "/api/led_mapping",
        .method = HTTP_GET,
        .handler = led_mapping_get_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t led_mapping_set = {
        .uri = "/api/led_mapping/set",
        .method = HTTP_GET,
        .handler = led_mapping_set_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t led_mapping_reset = {
        .uri = "/api/led_mapping/reset",
        .method = HTTP_GET,
        .handler = led_mapping_reset_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t led_mapping_save = {
        .uri = "/api/led_mapping/save",
        .method = HTTP_GET,
        .handler = led_mapping_save_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t border_blocks = {
        .uri = "/api/border_blocks",
        .method = HTTP_GET,
        .handler = border_blocks_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t runtime_mosaic = {
        .uri = "/api/runtime_mosaic",
        .method = HTTP_GET,
        .handler = runtime_mosaic_handler,
        .user_ctx = NULL,
    };
    config.stack_size = 10240;
    config.max_uri_handlers = 60;

    ESP_ERROR_CHECK(httpd_start(&server, &config));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &app_js));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &styles));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ping));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &layout_get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &layout_set));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &install_mode_get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &install_mode_set));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &strip_profile_set));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &color_order_set));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &install_guide));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &led_cal_get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &led_cal_test));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &wifi_status));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &wifi_scan));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &wifi_save));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &wifi_forget));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &params));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &params_save));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &params_restore_default));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &params_restore_saved));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &set));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &snap));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &snap_hires));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &mode_get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &mode_set));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &calibration_get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &calibration_set));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &calibration_snapshot));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &calibration_rectified));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &preview_mosaic));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &lens_profile_get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &led_power_get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &led_power_set));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &led_mapping_get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &led_mapping_set));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &led_mapping_reset));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &led_mapping_save));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &border_blocks));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &runtime_mosaic));

    ESP_LOGI(TAG, "HTTP server started on port %d", config.server_port);
    return ESP_OK;
}

static void boot_isp_sync_task(void *arg)
{
    int attempt;

    (void)arg;
    for (attempt = 0; attempt < 10; ++attempt) {
        esp_err_t ret;

        vTaskDelay(pdMS_TO_TICKS(1000));
        xSemaphoreTake(g_bridge_lock, portMAX_DELAY);
        ret = ESP_OK;
        if (!g_default_isp_params_valid) {
            ret = capture_current_isp_params_to_slot(&g_default_isp_params, &g_default_isp_params_valid);
            if (ret == ESP_OK) {
                ret = save_default_isp_params();
                if (ret == ESP_OK) {
                    ESP_LOGI(TAG, "captured T23 default ISP params");
                }
            }
        }
        if (ret == ESP_OK && g_saved_isp_params_valid) {
            ret = apply_saved_isp_params_to_t23();
        }
        xSemaphoreGive(g_bridge_lock);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "boot ISP sync complete");
            vTaskDelete(NULL);
            return;
        }
    }

    ESP_LOGW(TAG, "boot ISP sync gave up after retries");
    vTaskDelete(NULL);
}

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (g_wifi_sta_configured && g_wifi_sta_ssid[0] != '\0') {
            ESP_LOGI(TAG, "WiFi STA start, connecting to SSID \"%s\"", g_wifi_sta_ssid);
            esp_wifi_connect();
        } else {
            ESP_LOGI(TAG, "WiFi STA start, waiting for WiFi setup");
        }
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG, "WiFi AP ready: SSID \"%s\"", WIFI_AP_SSID);
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        g_wifi_sta_connected = 0;
        g_wifi_sta_ip[0] = '\0';
        ESP_LOGW(TAG, "WiFi disconnected, retrying");
        if (g_wifi_sta_configured && g_wifi_sta_ssid[0] != '\0') {
            esp_wifi_connect();
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        g_wifi_sta_connected = 1;
        snprintf(g_wifi_sta_ip, sizeof(g_wifi_sta_ip), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "WiFi got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

static esp_err_t wifi_apply_sta_config(void)
{
    wifi_config_t wifi_cfg = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false,
            },
        },
    };
    size_t ssid_len = strnlen(g_wifi_sta_ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    size_t pass_len = strnlen(g_wifi_sta_pass, sizeof(wifi_cfg.sta.password) - 1);

    memcpy(wifi_cfg.sta.ssid, g_wifi_sta_ssid, ssid_len);
    wifi_cfg.sta.ssid[ssid_len] = '\0';
    memcpy(wifi_cfg.sta.password, g_wifi_sta_pass, pass_len);
    wifi_cfg.sta.password[pass_len] = '\0';
    return esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
}

static esp_err_t init_wifi_network(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t ap_cfg = {
        .ap = {
            .ssid_len = 0,
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .required = false,
            },
        },
    };

    memcpy(ap_cfg.ap.ssid, WIFI_AP_SSID, strlen(WIFI_AP_SSID));
    memcpy(ap_cfg.ap.password, WIFI_AP_PASS, strlen(WIFI_AP_PASS));
    ap_cfg.ap.ssid_len = strlen(WIFI_AP_SSID);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(wifi_apply_sta_config());
    ESP_ERROR_CHECK(esp_wifi_start());
    return ESP_OK;
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    c3_mode_t initial_mode = C3_MODE_DEBUG;

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    load_install_config();
    load_led_mapping_profiles();
    load_wifi_config();
    load_saved_isp_params();
    load_default_isp_params();
    g_bridge_lock = xSemaphoreCreateMutex();
    assert(g_bridge_lock != NULL);
    if (choose_preview_capacity() == 0) {
        ESP_LOGE(TAG, "failed to allocate preview cache");
        return;
    }
    ESP_LOGI(TAG, "preview cache allocated: %u bytes", (unsigned)g_latest_jpeg_capacity);

    init_wifi_network();
    init_t23_uart();
    init_spi_slave();
    init_led_power_enable();
    reset_blocks_cache(&g_latest_blocks);
    ESP_ERROR_CHECK(sm16703sp3_init(&(sm16703sp3_config_t) {
        .gpio_num = LED_STRIP_GPIO,
        .led_count = LED_STRIP_COUNT,
    }));
    ESP_ERROR_CHECK(sm16703sp3_set_color_order(g_led_color_order->order));
    ESP_LOGI(TAG,
             "Install config loaded: mode=%s colorOrder=%s active=%d",
             g_install_mode->name,
             g_led_color_order->name,
             g_install_setup_active);
    if (g_install_setup_active) {
        ESP_ERROR_CHECK(show_install_guide_pattern());
    } else {
        ESP_ERROR_CHECK(sm16703sp3_clear());
    }
    if (bridge_get_mode(&initial_mode) == ESP_OK) {
        g_c3_mode = initial_mode;
        ESP_LOGI(TAG, "Initial T23 mode: %s", c3_mode_name(g_c3_mode));
    } else {
        ESP_LOGW(TAG, "Failed to query initial T23 mode, defaulting to DEBUG");
        g_c3_mode = C3_MODE_DEBUG;
    }
    start_http_server();
    xTaskCreate(boot_isp_sync_task, "boot_isp_sync", 4096, NULL, 4, NULL);
    xTaskCreate(runtime_blocks_task, "runtime_blocks", 12288, NULL, 5, NULL);
}
