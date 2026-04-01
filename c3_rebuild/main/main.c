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
#define JPEG_MIN_SIZE (32 * 1024)
#define PREVIEW_REFRESH_MS 200

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

static const bridge_param_t g_params[] = {
    { "BRIGHTNESS", T23_C3_PARAM_BRIGHTNESS },
    { "CONTRAST", T23_C3_PARAM_CONTRAST },
    { "SHARPNESS", T23_C3_PARAM_SHARPNESS },
    { "SATURATION", T23_C3_PARAM_SATURATION },
    { "AE_COMP", T23_C3_PARAM_AE_COMP },
    { "DPC", T23_C3_PARAM_DPC },
    { "DRC", T23_C3_PARAM_DRC },
    { "AWB_CT", T23_C3_PARAM_AWB_CT },
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
static border_blocks_cache_t g_latest_blocks;
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

static esp_err_t spi_receive_frame(t23_c3_frame_t *frame, int timeout_ms);
static esp_err_t wifi_apply_sta_config(void);

static const char *c3_mode_name(c3_mode_t mode)
{
    return (mode == C3_MODE_RUN) ? "RUN" : "DEBUG";
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

static void load_install_config(void)
{
    nvs_handle_t nvs = 0;
    uint8_t mode_value = (uint8_t)LED_INSTALL_LEFT_TOP;
    uint8_t active = 1;

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
    ESP_LOGI(TAG, "LED power enabled on GPIO%d", LED_POWER_EN_GPIO);
}

static esp_err_t show_install_guide_pattern(void);

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
}

static uint16_t run_blocks_checksum16(const uint8_t *data, size_t len)
{
    uint32_t sum = 0;
    size_t i;

    for (i = 0; i < len; ++i) {
        sum = (sum + data[i]) & 0xffffu;
    }

    return (uint16_t)sum;
}

/*
 * Convert the cached block result into the JSON shape expected by the web UI.
 * This keeps HTTP handlers lightweight and avoids recomputing layout metadata
 * on every request.
 */
static esp_err_t build_border_blocks_json_from_cache(char *json_buf,
                                                     size_t json_buf_size,
                                                     const border_blocks_cache_t *cache)
{
    size_t used = 0;
    int i;

    if (!cache->ready || cache->block_count <= 0) {
        return ESP_ERR_INVALID_STATE;
    }

    used += (size_t)snprintf(json_buf + used,
                             json_buf_size - used,
                             "{\"ok\":true,\"layout\":\"%s\",\"blockCount\":%d,\"topBlocks\":%d,\"rightBlocks\":%d,\"bottomBlocks\":%d,\"leftBlocks\":%d,\"imageWidth\":%d,\"imageHeight\":%d,\"rect\":{\"left\":%d,\"top\":%d,\"right\":%d,\"bottom\":%d,\"thickness\":%d},\"blocks\":[",
                             cache->layout_name,
                             cache->block_count,
                             cache->top_blocks,
                             cache->right_blocks,
                             cache->bottom_blocks,
                             cache->left_blocks,
                             cache->image_w,
                             cache->image_h,
                             cache->rect_left,
                             cache->rect_top,
                             cache->rect_right,
                             cache->rect_bottom,
                             cache->thickness);

    for (i = 0; i < cache->block_count; ++i) {
        used += (size_t)snprintf(json_buf + used,
                                 json_buf_size - used,
                                 "%s{\"index\":%d,\"r\":%u,\"g\":%u,\"b\":%u}",
                                 (i == 0) ? "" : ",",
                                 i,
                                 cache->valid[i] ? cache->colors[i][0] : 0,
                                 cache->valid[i] ? cache->colors[i][1] : 0,
                                 cache->valid[i] ? cache->colors[i][2] : 0);
        if (used >= json_buf_size) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (snprintf(json_buf + used, json_buf_size - used, "]}") >= (int)(json_buf_size - used)) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static void expand_block_segment(uint8_t *led_rgb,
                                 int dst_offset,
                                 int dst_count,
                                 const border_blocks_cache_t *cache,
                                 int src_offset,
                                 int src_count,
                                 int reverse)
{
    int i;

    if (dst_count <= 0 || src_count <= 0) {
        return;
    }

    for (i = 0; i < dst_count; ++i) {
        int sample_i = reverse ? (dst_count - 1 - i) : i;
        int src_index = src_offset + ((sample_i * src_count) / dst_count);
        uint8_t r = 0;
        uint8_t g = 0;
        uint8_t b = 0;

        if (src_index >= 0 && src_index < cache->block_count && cache->valid[src_index]) {
            r = cache->colors[src_index][0];
            g = cache->colors[src_index][1];
            b = cache->colors[src_index][2];
        }

        led_rgb[(dst_offset + i) * 3 + 0] = r;
        led_rgb[(dst_offset + i) * 3 + 1] = g;
        led_rgb[(dst_offset + i) * 3 + 2] = b;
    }
}

static void get_edge_span(const border_blocks_cache_t *cache,
                          logical_edge_t edge,
                          int *src_offset,
                          int *src_count)
{
    switch (edge) {
    case LOGICAL_EDGE_TOP:
        *src_offset = 0;
        *src_count = cache->top_blocks;
        break;
    case LOGICAL_EDGE_RIGHT:
        *src_offset = cache->top_blocks;
        *src_count = cache->right_blocks;
        break;
    case LOGICAL_EDGE_BOTTOM:
        *src_offset = cache->top_blocks + cache->right_blocks;
        *src_count = cache->bottom_blocks;
        break;
    case LOGICAL_EDGE_LEFT:
    default:
        *src_offset = cache->top_blocks + cache->right_blocks + cache->bottom_blocks;
        *src_count = cache->left_blocks;
        break;
    }
}

/*
 * Expand the logical border layout into the fixed 50-pixel physical LED strip
 * order and push the result to the SM16703SP3 driver.
 */
static esp_err_t update_led_strip_from_cache(const border_blocks_cache_t *cache)
{
    uint8_t led_rgb[LED_STRIP_COUNT * 3];
    static const int segment_lengths[4] = { 9, 16, 9, 16 };
    int dst_offset = 0;
    int seg;

    if (!cache->ready || cache->block_count <= 0) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(led_rgb, 0, sizeof(led_rgb));
    for (seg = 0; seg < 4; ++seg) {
        int src_offset = 0;
        int src_count = 0;

        get_edge_span(cache, g_install_mode->segments[seg].edge, &src_offset, &src_count);
        expand_block_segment(led_rgb,
                             dst_offset,
                             segment_lengths[seg],
                             cache,
                             src_offset,
                             src_count,
                             g_install_mode->segments[seg].reverse);
        dst_offset += segment_lengths[seg];
    }
    return sm16703sp3_show_rgb(led_rgb, LED_STRIP_COUNT);
}

static esp_err_t show_install_guide_pattern(void)
{
    static const int segment_lengths[4] = { 9, 16, 9, 16 };
    uint8_t led_rgb[LED_STRIP_COUNT * 3];
    int dst_offset = 0;
    int seg;

    memset(led_rgb, 0, sizeof(led_rgb));
    for (seg = 0; seg < 4; ++seg) {
        uint8_t r = 0;
        uint8_t g = 0;
        uint8_t b = 0;
        int i;

        /* Highlight only the selected installation start pair:
         * segment 0 = short edge (blue), segment 1 = following long edge (red).
         * Remaining segments stay off so customers can identify orientation quickly.
         */
        if (seg == 0 && segment_lengths[seg] == 9) {
            b = 255;
        } else if (seg == 1 && segment_lengths[seg] == 16) {
            r = 255;
        }

        for (i = 0; i < segment_lengths[seg]; ++i) {
            led_rgb[(dst_offset + i) * 3 + 0] = r;
            led_rgb[(dst_offset + i) * 3 + 1] = g;
            led_rgb[(dst_offset + i) * 3 + 2] = b;
        }
        dst_offset += segment_lengths[seg];
    }
    ESP_LOGI(TAG, "LED install guide: selected short edge blue, selected long edge red (%s)", g_install_mode->name);
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
    "          <label for=\"layoutSelect\">Border Layout</label>\n"
    "          <select id=\"layoutSelect\">\n"
    "            <option value=\"16X9\">16 x 9</option>\n"
    "            <option value=\"4X3\">4 x 3</option>\n"
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
    "          </div>\n"
    "          <div class=\"button-row\">\n"
    "          <button id=\"snapBtn\">Capture Snapshot</button>\n"
    "          <button id=\"autoPreviewBtn\">Start Auto Preview</button>\n"
    "          <button id=\"enterRunBtn\">Enter Run Mode</button>\n"
    "          <button id=\"returnDebugBtn\">Return To Debug Mode</button>\n"
    "          </div>\n"
    "        </div>\n"
    "      </div>\n"
    "    </section>\n"
    "    <section class=\"panel\" id=\"ispPanel\">\n"
    "      <div class=\"section-header\"><h2>ISP Controls</h2></div>\n"
    "      <div class=\"grid\" id=\"paramGrid\"></div>\n"
    "    </section>\n"
    "    <section class=\"panel panel--preview\">\n"
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
    "          <h3>Logical Border Average Preview</h3>\n"
    "          <div class=\"preview-wrap\"><canvas id=\"borderCanvas\" width=\"640\" height=\"320\"></canvas></div>\n"
    "        </div>\n"
    "      </div>\n"
    "    </section>\n"
    "    <section class=\"panel panel--calibration\" id=\"calibrationPanel\">\n"
    "      <div class=\"section-header\">\n"
    "        <h2>Border Calibration</h2>\n"
    "        <span class=\"preview-status\" id=\"calibrationStatus\">Idle</span>\n"
    "      </div>\n"
    "      <p class=\"subtitle\">Drag 8 points around the TV border: 4 corners and 4 edge midpoints. Click confirm to store the calibration in T23 and fetch a real rectified preview rendered inside T23.</p>\n"
    "      <div class=\"button-row calibration-actions\">\n"
    "        <button id=\"loadCalibrationSnapBtn\">Load Calibration Snapshot</button>\n"
    "        <button id=\"loadCalibrationBtn\">Load Saved Points</button>\n"
    "        <button id=\"resetCalibrationBtn\">Reset 8 Points</button>\n"
    "        <button id=\"saveCalibrationBtn\">Confirm 8 Points</button>\n"
    "      </div>\n"
    "      <div class=\"calibration-grid\">\n"
    "        <div class=\"calibration-pane\">\n"
    "          <h3>Interactive Calibration</h3>\n"
    "          <canvas id=\"calibrationCanvas\" width=\"640\" height=\"320\"></canvas>\n"
    "        </div>\n"
    "        <div class=\"calibration-pane\">\n"
    "          <h3>Rectified Preview</h3>\n"
    "          <canvas id=\"rectifiedCanvas\" width=\"640\" height=\"320\"></canvas>\n"
    "        </div>\n"
    "      </div>\n"
    "    </section>\n"
    "    <section class=\"panel\">\n"
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
    ".preview-status { display: inline-flex; align-items: center; min-height: 36px; padding: 6px 12px; border-radius: 999px; border: 1px solid var(--line); background: #f7faf4; color: var(--muted); font-weight: 700; }\n"
    ".preview-status.is-busy { color: var(--accent); border-color: rgba(13, 107, 87, 0.3); background: rgba(13, 107, 87, 0.08); }\n"
    ".preview-status.is-good { color: var(--good); border-color: rgba(20, 128, 74, 0.25); background: rgba(20, 128, 74, 0.08); }\n"
    ".preview-status.is-bad { color: var(--bad); border-color: rgba(184, 59, 45, 0.25); background: rgba(184, 59, 45, 0.08); }\n"
    ".is-hidden { display: none !important; }\n"
    "#previewImage, #borderCanvas { max-width: 100%; max-height: 560px; display: block; }\n"
    ".log-box { min-height: 180px; max-height: 300px; overflow: auto; padding: 14px; background: #122019; color: #cce7db; border-radius: 14px; white-space: pre-wrap; }\n"
    "@media (max-width: 980px) { .hero-panels { grid-template-columns: 1fr; } }\n"
    "@media (max-width: 820px) { .button-row { flex-wrap: wrap; } }\n";

static const char g_app_js[] =
    "const PARAMS=[\n"
    " {key:'BRIGHTNESS',label:'Brightness',min:0,max:255,step:1},\n"
    " {key:'CONTRAST',label:'Contrast',min:0,max:255,step:1},\n"
    " {key:'SHARPNESS',label:'Sharpness',min:0,max:255,step:1},\n"
    " {key:'SATURATION',label:'Saturation',min:0,max:255,step:1},\n"
    " {key:'AE_COMP',label:'AE Compensation',min:90,max:250,step:1},\n"
    " {key:'DPC',label:'DPC Strength',min:0,max:255,step:1},\n"
    " {key:'DRC',label:'DRC Strength',min:0,max:255,step:1},\n"
    " {key:'AWB_CT',label:'AWB Color Temp',min:1500,max:12000,step:10}\n"
    "];\n"
    "const POINT_LABELS=['TL','TM','TR','RM','BR','BM','BL','LM'];\n"
    "const ui={snapBtn:document.getElementById('snapBtn'),autoPreviewBtn:document.getElementById('autoPreviewBtn'),enterRunBtn:document.getElementById('enterRunBtn'),returnDebugBtn:document.getElementById('returnDebugBtn'),modeBadge:document.getElementById('modeBadge'),statusText:document.getElementById('statusText'),wifiStatusBadge:document.getElementById('wifiStatusBadge'),wifiHint:document.getElementById('wifiHint'),wifiScanSelect:document.getElementById('wifiScanSelect'),wifiScanBtn:document.getElementById('wifiScanBtn'),wifiSsidInput:document.getElementById('wifiSsidInput'),wifiPassInput:document.getElementById('wifiPassInput'),wifiSaveBtn:document.getElementById('wifiSaveBtn'),wifiForgetBtn:document.getElementById('wifiForgetBtn'),layoutSelect:document.getElementById('layoutSelect'),installModeSelect:document.getElementById('installModeSelect'),showInstallGuideBtn:document.getElementById('showInstallGuideBtn'),ispPanel:document.getElementById('ispPanel'),originalPane:document.getElementById('originalPane'),calibrationPanel:document.getElementById('calibrationPanel'),clearLogBtn:document.getElementById('clearLogBtn'),paramGrid:document.getElementById('paramGrid'),logBox:document.getElementById('logBox'),previewImage:document.getElementById('previewImage'),borderCanvas:document.getElementById('borderCanvas'),previewStatus:document.getElementById('previewStatus'),loadCalibrationSnapBtn:document.getElementById('loadCalibrationSnapBtn'),loadCalibrationBtn:document.getElementById('loadCalibrationBtn'),resetCalibrationBtn:document.getElementById('resetCalibrationBtn'),saveCalibrationBtn:document.getElementById('saveCalibrationBtn'),calibrationStatus:document.getElementById('calibrationStatus'),calibrationCanvas:document.getElementById('calibrationCanvas'),rectifiedCanvas:document.getElementById('rectifiedCanvas')};\n"
    "const state={autoPreviewTimer:null,previewBusy:false,runtimeBusy:false,previewUrl:null,calibrationImage:null,rectifiedImage:null,borderData:null,mode:'DEBUG',layout:'16X9',installMode:'LEFT_TOP',installGuideActive:true,wifiConfigured:true,wifiConnected:false,wifiSsid:'',wifiIp:'',lastBlocksAt:0,defaultsLoaded:false,calibration:{imageWidth:640,imageHeight:320,points:[]},dragIndex:-1};\n"
    "function log(m){const ts=new Date().toLocaleTimeString();ui.logBox.textContent+=`[${ts}] ${m}\\n`;ui.logBox.scrollTop=ui.logBox.scrollHeight;}\n"
    "function setPreviewStatus(t,k='idle'){ui.previewStatus.textContent=t;ui.previewStatus.classList.remove('is-busy','is-good','is-bad');if(k==='busy')ui.previewStatus.classList.add('is-busy');else if(k==='good')ui.previewStatus.classList.add('is-good');else if(k==='bad')ui.previewStatus.classList.add('is-bad');}\n"
    "function setCalibrationStatus(t,k='idle'){ui.calibrationStatus.textContent=t;ui.calibrationStatus.classList.remove('is-busy','is-good','is-bad');if(k==='busy')ui.calibrationStatus.classList.add('is-busy');else if(k==='good')ui.calibrationStatus.classList.add('is-good');else if(k==='bad')ui.calibrationStatus.classList.add('is-bad');}\n"
    "function waitForImage(img){if(img.complete&&img.naturalWidth>0)return Promise.resolve();return new Promise((resolve,reject)=>{const onLoad=()=>{img.removeEventListener('load',onLoad);img.removeEventListener('error',onError);resolve();};const onError=(e)=>{img.removeEventListener('load',onLoad);img.removeEventListener('error',onError);reject(e);};img.addEventListener('load',onLoad,{once:true});img.addEventListener('error',onError,{once:true});});}\n"
    "function applyLayoutMeta(data){if(!data)return;const layout=data.layout||state.layout||'16X9';state.layout=layout;if(ui.layoutSelect&&ui.layoutSelect.value!==layout)ui.layoutSelect.value=layout;}\n"
    "function applyInstallModeMeta(data){if(!data)return;const installMode=data.installMode||state.installMode||'LEFT_TOP';state.installMode=installMode;state.installGuideActive=typeof data.installGuideActive==='boolean'?data.installGuideActive:state.installGuideActive;if(ui.installModeSelect&&ui.installModeSelect.value!==installMode)ui.installModeSelect.value=installMode;if(ui.installModeSelect)ui.installModeSelect.disabled=!state.installGuideActive; if(ui.showInstallGuideBtn){ui.showInstallGuideBtn.textContent=state.installGuideActive?'Finish Install Guide':'Enable Install Guide'; ui.showInstallGuideBtn.classList.toggle('is-good',state.installGuideActive);} }\n"
    "function applyWifiStatus(data){if(!data)return;state.wifiConfigured=!!data.configured;state.wifiConnected=!!data.connected;state.wifiSsid=data.ssid||'';state.wifiIp=data.ip||'';if(ui.wifiSsidInput&&!ui.wifiSsidInput.matches(':focus'))ui.wifiSsidInput.value=state.wifiSsid;if(ui.wifiStatusBadge){ui.wifiStatusBadge.textContent=state.wifiConnected?`WiFi: ${state.wifiSsid} (${state.wifiIp||'IP pending'})`:(state.wifiConfigured?`Connecting to ${state.wifiSsid||'saved WiFi'}`:'Setup mode');ui.wifiStatusBadge.classList.remove('is-good','is-busy','is-bad');ui.wifiStatusBadge.classList.add(state.wifiConnected?'is-good':'is-busy');}if(ui.statusText)ui.statusText.textContent=state.wifiConnected?`Connected to ${state.wifiSsid}`:`Connected to C3 bridge (${data.apSsid})`;if(ui.wifiHint)ui.wifiHint.textContent=`Fallback AP: ${data.apSsid} / ${data.apPass}`;}\n"
    "function applyWifiScan(data){if(!ui.wifiScanSelect)return;ui.wifiScanSelect.innerHTML='';const placeholder=document.createElement('option');placeholder.value='';placeholder.textContent=data.results&&data.results.length?`Found ${data.results.length} network(s)`:'No networks found';ui.wifiScanSelect.appendChild(placeholder);for(const item of (data.results||[])){const opt=document.createElement('option');opt.value=item.ssid;opt.textContent=`${item.ssid} (${item.rssi} dBm)`;ui.wifiScanSelect.appendChild(opt);}ui.wifiScanSelect.value='';}\n"
    "function getLayoutMeta(d){const top=d.topBlocks||16,right=d.rightBlocks||9,bottom=d.bottomBlocks||16,left=d.leftBlocks||9;return{top,right,bottom,left,total:(d.blockCount||d.blocks?.length||0),layout:(d.layout||state.layout||'16X9')};}\n"
    "function setModeUi(mode){state.mode=mode;ui.modeBadge.textContent=`Mode: ${mode}`;const isRun=mode==='RUN';ui.originalPane.classList.toggle('is-hidden',isRun);ui.ispPanel.classList.toggle('is-hidden',isRun);ui.calibrationPanel.classList.toggle('is-hidden',isRun);ui.snapBtn.disabled=isRun;ui.autoPreviewBtn.disabled=isRun;ui.enterRunBtn.disabled=isRun;ui.returnDebugBtn.disabled=!isRun;if(isRun){setPreviewStatus('Run mode active: LED output only','good');}else{setPreviewStatus('Debug mode active','good');}}\n"
    "function renderParams(){ui.paramGrid.innerHTML='';for(const p of PARAMS){const card=document.createElement('article');card.className='param-card';card.innerHTML=`<header><label for=\"slider-${p.key}\">${p.label}</label><span class=\"value\" id=\"value-${p.key}\">-</span></header><input id=\"slider-${p.key}\" type=\"range\" min=\"${p.min}\" max=\"${p.max}\" step=\"${p.step}\" value=\"${p.min}\"><div class=\"param-actions\"><button id=\"default-${p.key}\">Restore Default</button></div>`;ui.paramGrid.appendChild(card);const slider=document.getElementById(`slider-${p.key}`);const value=document.getElementById(`value-${p.key}`);const defaultBtn=document.getElementById(`default-${p.key}`);defaultBtn.onclick=()=>restoreDefault(p);slider.oninput=()=>{value.textContent=slider.value;};slider.onchange=()=>setParam(p.key,slider.value,true);p.slider=slider;p.valueLabel=value;p.defaultBtn=defaultBtn;updateDefaultButtonState(p);}}\n"
    "function updateDefaultButtonState(p){if(!p.defaultBtn)return;const hasDefault=typeof p.defaultValue!=='undefined';p.defaultBtn.disabled=!hasDefault;}\n"
    "function applyValues(data){for(const p of PARAMS){if(typeof data[p.key]!=='undefined'){p.slider.value=data[p.key];p.valueLabel.textContent=data[p.key];if(!state.defaultsLoaded||typeof p.defaultValue==='undefined'){p.defaultValue=data[p.key];}updateDefaultButtonState(p);}}state.defaultsLoaded=true;}\n"
    "function applySingleParamValue(data){if(!data||!data.key)return;const p=PARAMS.find(x=>x.key===data.key);if(!p)return;p.slider.value=data.value;p.valueLabel.textContent=data.value;updateDefaultButtonState(p);}\n"
    "async function restoreDefault(p){if(typeof p.defaultValue==='undefined')return;await setParam(p.key,p.defaultValue,true);}\n"
    "function makeDefaultCalibration(w,h){const left=Math.round(w/8),right=Math.round(w-left),top=Math.round(h/8),bottom=Math.round(h-top),cx=Math.round(w/2),cy=Math.round(h/2);return{imageWidth:w,imageHeight:h,points:[{x:left,y:top},{x:cx,y:top},{x:right,y:top},{x:right,y:cy},{x:right,y:bottom},{x:cx,y:bottom},{x:left,y:bottom},{x:left,y:cy}]};}\n"
    "function ensureCalibrationPoints(){if(!state.calibration.points||state.calibration.points.length!==8){state.calibration=makeDefaultCalibration(state.calibration.imageWidth||640,state.calibration.imageHeight||320);}}\n"
    "function drawCalibrationCanvas(){const c=ui.calibrationCanvas,ctx=c.getContext('2d');ctx.clearRect(0,0,c.width,c.height);ctx.fillStyle='#f8fbf5';ctx.fillRect(0,0,c.width,c.height);if(state.calibrationImage){ctx.drawImage(state.calibrationImage,0,0,c.width,c.height);}ctx.strokeStyle='rgba(13,107,87,0.85)';ctx.lineWidth=3;ensureCalibrationPoints();ctx.beginPath();state.calibration.points.forEach((p,i)=>{if(i===0)ctx.moveTo(p.x,p.y);else ctx.lineTo(p.x,p.y);});ctx.closePath();ctx.stroke();ctx.font='12px sans-serif';state.calibration.points.forEach((p,i)=>{ctx.beginPath();ctx.fillStyle='#d26a2e';ctx.arc(p.x,p.y,7,0,Math.PI*2);ctx.fill();ctx.fillStyle='#132b21';ctx.fillText(POINT_LABELS[i],p.x+10,p.y-10);});}\n"
    "function drawRectifiedGuide(){const c=ui.rectifiedCanvas,ctx=c.getContext('2d');const pad=40,w=c.width,h=c.height;const left=pad,right=w-pad,top=pad,bottom=h-pad,cx=Math.round((left+right)/2),cy=Math.round((top+bottom)/2);ctx.clearRect(0,0,w,h);ctx.fillStyle='#f8fbf5';ctx.fillRect(0,0,w,h);if(state.rectifiedImage){const img=state.rectifiedImage;const scale=Math.min(w/img.width,h/img.height);const dw=Math.round(img.width*scale),dh=Math.round(img.height*scale);const dx=Math.round((w-dw)/2),dy=Math.round((h-dh)/2);ctx.drawImage(img,dx,dy,dw,dh);ctx.strokeStyle='rgba(13,107,87,0.45)';ctx.lineWidth=2;ctx.strokeRect(dx,dy,dw,dh);ctx.fillStyle='#5b6b61';ctx.fillText('Rectified preview rendered by T23',pad,h-12);return;}ctx.strokeStyle='rgba(13,107,87,0.85)';ctx.lineWidth=3;ctx.strokeRect(left,top,right-left,bottom-top);const pts=[[left,top],[cx,top],[right,top],[right,cy],[right,bottom],[cx,bottom],[left,bottom],[left,cy]];ctx.font='12px sans-serif';pts.forEach((p,i)=>{ctx.beginPath();ctx.fillStyle='#0d6b57';ctx.arc(p[0],p[1],7,0,Math.PI*2);ctx.fill();ctx.fillStyle='#132b21';ctx.fillText(POINT_LABELS[i],p[0]+10,p[1]-10);});ctx.fillStyle='#5b6b61';ctx.fillText('Rectified preview will appear here after calibration confirm',pad,h-12);}\n"
    "function drawBorderBlocks(){const c=ui.borderCanvas,ctx=c.getContext('2d');ctx.clearRect(0,0,c.width,c.height);ctx.fillStyle='#101814';ctx.fillRect(0,0,c.width,c.height);if(!state.borderData){ctx.fillStyle='#cce7db';ctx.font='16px sans-serif';ctx.fillText('Border average preview will appear here',20,30);return;}const d=state.borderData,m=getLayoutMeta(d);const margin=16,outerW=c.width-margin*2,outerH=c.height-margin*2;const l=margin,t=margin,r=l+outerW,b=t+outerH;const topCell=Math.max(1,outerW/Math.max(1,m.top));const sideCell=Math.max(1,outerH/Math.max(1,m.right));const th=Math.max(8,Math.min(Math.round(Math.min(topCell,sideCell)*0.85),Math.round(Math.min(outerW,outerH)*0.42)));const innerTop=t+th,innerBottom=b-th,innerLeft=l+th,innerRight=r-th;let idx=0;for(let i=0;i<m.top&&idx<d.blocks.length;i++,idx++){const x0=Math.round(l+outerW*i/m.top),x1=Math.round(l+outerW*(i+1)/m.top);const c0=d.blocks[idx];ctx.fillStyle=`rgb(${c0.r},${c0.g},${c0.b})`;ctx.fillRect(x0,t,Math.max(1,x1-x0),th);}const rightStart=idx;if(m.layout==='4X3'&&m.right===3&&rightStart+1<d.blocks.length){const c0=d.blocks[rightStart+1];ctx.fillStyle=`rgb(${c0.r},${c0.g},${c0.b})`;ctx.fillRect(r-th,innerTop,th,Math.max(1,innerBottom-innerTop));idx+=m.right;}else{for(let i=0;i<m.right&&idx<d.blocks.length;i++,idx++){const y0=Math.round(innerTop+(innerBottom-innerTop)*i/m.right),y1=Math.round(innerTop+(innerBottom-innerTop)*(i+1)/m.right);const c0=d.blocks[idx];ctx.fillStyle=`rgb(${c0.r},${c0.g},${c0.b})`;ctx.fillRect(r-th,y0,th,Math.max(1,y1-y0));}}for(let i=0;i<m.bottom&&idx<d.blocks.length;i++,idx++){const x0=Math.round(l+outerW*(m.bottom-1-i)/m.bottom),x1=Math.round(l+outerW*(m.bottom-i)/m.bottom);const c0=d.blocks[idx];ctx.fillStyle=`rgb(${c0.r},${c0.g},${c0.b})`;ctx.fillRect(x0,b-th,Math.max(1,x1-x0),th);}const leftStart=idx;if(m.layout==='4X3'&&m.left===3&&leftStart+1<d.blocks.length){const c0=d.blocks[leftStart+1];ctx.fillStyle=`rgb(${c0.r},${c0.g},${c0.b})`;ctx.fillRect(l,innerTop,th,Math.max(1,innerBottom-innerTop));idx+=m.left;}else{for(let i=0;i<m.left&&idx<d.blocks.length;i++,idx++){const y0=Math.round(innerTop+(innerBottom-innerTop)*(m.left-1-i)/m.left),y1=Math.round(innerTop+(innerBottom-innerTop)*(m.left-i)/m.left);const c0=d.blocks[idx];ctx.fillStyle=`rgb(${c0.r},${c0.g},${c0.b})`;ctx.fillRect(l,y0,th,Math.max(1,y1-y0));}}ctx.fillStyle='#101814';ctx.fillRect(innerLeft,innerTop,Math.max(1,innerRight-innerLeft),Math.max(1,innerBottom-innerTop));ctx.strokeStyle='rgba(255,255,255,0.35)';ctx.lineWidth=2;ctx.strokeRect(l,t,Math.max(1,outerW),Math.max(1,outerH));ctx.fillStyle='#cce7db';ctx.font='12px sans-serif';ctx.fillText(`${m.total} block average colors (${m.layout}) rendered from T23 output`,16,c.height-12);}\n"
    "function canvasPos(evt){const rect=ui.calibrationCanvas.getBoundingClientRect();const sx=ui.calibrationCanvas.width/rect.width,sy=ui.calibrationCanvas.height/rect.height;return{x:(evt.clientX-rect.left)*sx,y:(evt.clientY-rect.top)*sy};}\n"
    "function pickPoint(pos){ensureCalibrationPoints();for(let i=0;i<state.calibration.points.length;i++){const p=state.calibration.points[i];const dx=p.x-pos.x,dy=p.y-pos.y;if((dx*dx+dy*dy)<=225)return i;}return -1;}\n"
    "function clamp(v,min,max){return Math.max(min,Math.min(max,v));}\n"
    "function bindCalibrationCanvas(){ui.calibrationCanvas.onpointerdown=(evt)=>{const pos=canvasPos(evt);state.dragIndex=pickPoint(pos);if(state.dragIndex>=0){ui.calibrationCanvas.setPointerCapture(evt.pointerId);}};ui.calibrationCanvas.onpointermove=(evt)=>{if(state.dragIndex<0)return;const pos=canvasPos(evt);const maxX=state.calibration.imageWidth-1,maxY=state.calibration.imageHeight-1;state.calibration.points[state.dragIndex]={x:Math.round(clamp(pos.x,0,maxX)),y:Math.round(clamp(pos.y,0,maxY))};drawCalibrationCanvas();drawRectifiedGuide();};ui.calibrationCanvas.onpointerup=(evt)=>{state.dragIndex=-1;try{ui.calibrationCanvas.releasePointerCapture(evt.pointerId);}catch(_){};};ui.calibrationCanvas.onpointerleave=()=>{};}\n"
    "async function loadCalibrationSnapshot(){setCalibrationStatus('Loading snapshot...','busy');try{const r=await fetch(`/api/snap?t=${Date.now()}`,{cache:'no-store'});if(!r.ok)throw new Error(`HTTP ${r.status}`);const blob=await r.blob();const img=new Image();await new Promise((resolve,reject)=>{img.onload=resolve;img.onerror=reject;img.src=URL.createObjectURL(blob);});state.calibrationImage=img;ui.calibrationCanvas.width=img.width;ui.calibrationCanvas.height=img.height;if(!state.calibration.points||state.calibration.imageWidth!==img.width||state.calibration.imageHeight!==img.height){state.calibration=makeDefaultCalibration(img.width,img.height);}drawCalibrationCanvas();drawRectifiedGuide();setCalibrationStatus('Calibration snapshot ready','good');}catch(e){log(`ERR ${e.message}`);setCalibrationStatus(`ERR ${e.message}`,'bad');}}\n"
    "async function loadRectifiedPreview(){setCalibrationStatus('Loading rectified preview...','busy');log(`GET /api/calibration/rectified?t=${Date.now()}`);try{const r=await fetch(`/api/calibration/rectified?t=${Date.now()}`,{cache:'no-store'});if(!r.ok)throw new Error(`HTTP ${r.status}`);const blob=await r.blob();const img=new Image();await new Promise((resolve,reject)=>{img.onload=resolve;img.onerror=reject;img.src=URL.createObjectURL(blob);});state.rectifiedImage=img;drawRectifiedGuide();setCalibrationStatus('Rectified preview updated','good');}catch(e){log(`ERR ${e.message}`);setCalibrationStatus(`ERR ${e.message}`,'bad');}}\n"
    "async function loadCalibration(){setCalibrationStatus('Loading saved points...','busy');log('GET /api/calibration');try{const data=await fetchJson('/api/calibration');state.calibration={imageWidth:data.imageWidth,imageHeight:data.imageHeight,points:data.points};if(!state.calibrationImage)await loadCalibrationSnapshot();else{ui.calibrationCanvas.width=state.calibration.imageWidth;ui.calibrationCanvas.height=state.calibration.imageHeight;drawCalibrationCanvas();drawRectifiedGuide();}await loadRectifiedPreview();}catch(e){log(`ERR ${e.message}`);setCalibrationStatus(`ERR ${e.message}`,'bad');}}\n"
    "function resetCalibration(){const w=(state.calibrationImage&&state.calibrationImage.width)||state.calibration.imageWidth||640;const h=(state.calibrationImage&&state.calibrationImage.height)||state.calibration.imageHeight||320;state.calibration=makeDefaultCalibration(w,h);state.rectifiedImage=null;drawCalibrationCanvas();drawRectifiedGuide();setCalibrationStatus('Calibration points reset','good');}\n"
    "async function saveCalibration(){ensureCalibrationPoints();const q=new URLSearchParams();q.set('iw',state.calibration.imageWidth);q.set('ih',state.calibration.imageHeight);state.calibration.points.forEach((p,i)=>{q.set(`x${i}`,p.x);q.set(`y${i}`,p.y);});const url=`/api/calibration/set?${q.toString()}`;setCalibrationStatus('Saving calibration...','busy');log(`GET ${url}`);try{const data=await fetchJson(url);state.calibration={imageWidth:data.imageWidth,imageHeight:data.imageHeight,points:data.points};drawCalibrationCanvas();state.rectifiedImage=null;drawRectifiedGuide();await loadRectifiedPreview();}catch(e){log(`ERR ${e.message}`);setCalibrationStatus(`ERR ${e.message}`,'bad');}}\n"
    "async function fetchJson(url){const r=await fetch(url,{cache:'no-store'});if(!r.ok)throw new Error(`HTTP ${r.status}`);return r.json();}\n"
    "async function refreshWiFiStatus(){log('GET /api/wifi');try{const data=await fetchJson('/api/wifi');applyWifiStatus(data);}catch(e){log(`ERR ${e.message}`);if(ui.wifiStatusBadge){ui.wifiStatusBadge.textContent=`ERR ${e.message}`;ui.wifiStatusBadge.classList.remove('is-good','is-busy');ui.wifiStatusBadge.classList.add('is-bad');}}}\n"
    "async function scanWiFi(){log('GET /api/wifi/scan');try{const data=await fetchJson('/api/wifi/scan');applyWifiScan(data);setPreviewStatus('WiFi scan complete','good');}catch(e){log(`ERR ${e.message}`);setPreviewStatus(`ERR ${e.message}`,'bad');}}\n"
    "async function saveWiFi(){const ssid=(ui.wifiSsidInput.value||'').trim();const key=ui.wifiPassInput.value||'';if(!ssid){setPreviewStatus('Enter SSID first','bad');return;}const url=`/api/wifi/save?ssid=${encodeURIComponent(ssid)}&key=${encodeURIComponent(key)}`;log(`GET /api/wifi/save?ssid=${ssid}`);try{const data=await fetchJson(url);applyWifiStatus(data);ui.wifiPassInput.value='';setPreviewStatus(`WiFi saved: ${ssid}`,'good');}catch(e){log(`ERR ${e.message}`);setPreviewStatus(`ERR ${e.message}`,'bad');}}\n"
    "async function forgetWiFi(){log('GET /api/wifi/forget');try{const data=await fetchJson('/api/wifi/forget');applyWifiStatus(data);ui.wifiPassInput.value='';setPreviewStatus('Saved WiFi cleared; fallback AP only','good');}catch(e){log(`ERR ${e.message}`);setPreviewStatus(`ERR ${e.message}`,'bad');}}\n"
    "async function refreshLayout(){log('GET /api/layout');try{const data=await fetchJson('/api/layout');applyLayoutMeta(data);}catch(e){log(`ERR ${e.message}`);}}\n"
    "async function refreshInstallMode(){log('GET /api/install_mode');try{const data=await fetchJson('/api/install_mode');applyInstallModeMeta(data);}catch(e){log(`ERR ${e.message}`);}}\n"
    "async function setLayout(value){log(`GET /api/layout/set?value=${value}`);try{const data=await fetchJson(`/api/layout/set?value=${encodeURIComponent(value)}`);applyLayoutMeta(data);drawBorderBlocks();if(state.mode==='RUN')await refreshRuntimeBlocks();else await refreshPreviewAndBlocks();}catch(e){log(`ERR ${e.message}`);setPreviewStatus(`ERR ${e.message}`,'bad');}}\n"
    "async function setInstallMode(value){log(`GET /api/install_mode/set?value=${value}`);try{const data=await fetchJson(`/api/install_mode/set?value=${encodeURIComponent(value)}`);applyInstallModeMeta(data);setPreviewStatus(`Install mapping updated: ${data.label}`,'good');}catch(e){log(`ERR ${e.message}`);setPreviewStatus(`ERR ${e.message}`,'bad');}}\n"
    "async function toggleInstallGuide(){const active=state.installGuideActive?0:1;log(`GET /api/install_guide?active=${active}`);try{const data=await fetchJson(`/api/install_guide?active=${active}`);applyInstallModeMeta(data);setPreviewStatus(state.installGuideActive?'Install guide active: choose direction now':'Install guide locked: saved for next boot','good');}catch(e){log(`ERR ${e.message}`);setPreviewStatus(`ERR ${e.message}`,'bad');}}\n"
    "async function refreshMode(){log('GET /api/mode');try{const data=await fetchJson('/api/mode');setModeUi(data.mode||'DEBUG');}catch(e){log(`ERR ${e.message}`);setPreviewStatus(`ERR ${e.message}`,'bad');}}\n"
    "async function setMode(mode){log(`GET /api/mode/set?value=${mode}`);try{const data=await fetchJson(`/api/mode/set?value=${encodeURIComponent(mode)}`);setModeUi(data.mode||mode);if(state.autoPreviewTimer){clearInterval(state.autoPreviewTimer);state.autoPreviewTimer=null;ui.autoPreviewBtn.textContent='Start Auto Preview';}}catch(e){log(`ERR ${e.message}`);setPreviewStatus(`ERR ${e.message}`,'bad');}}\n"
    "async function refreshValues(){setPreviewStatus('Refreshing parameters...','busy');log('GET /api/params');try{const data=await fetchJson('/api/params');applyValues(data);setPreviewStatus('Parameters refreshed','good');}catch(e){log(`ERR ${e.message}`);setPreviewStatus(`ERR ${e.message}`,'bad');}}\n"
    "async function pingT23(){log('GET /api/ping');try{await fetchJson('/api/ping');setPreviewStatus('T23 online','good');}catch(e){log(`ERR ${e.message}`);setPreviewStatus(`ERR ${e.message}`,'bad');}}\n"
    "async function setParam(key,value,autoSnap){log(`GET /api/set?key=${key}&value=${value}`);try{const data=await fetchJson(`/api/set?key=${encodeURIComponent(key)}&value=${encodeURIComponent(value)}`);applySingleParamValue(data);setPreviewStatus(`Applied ${key}`,'good');if(autoSnap){await refreshPreviewAndBlocks();}}catch(e){log(`ERR ${e.message}`);setPreviewStatus(`ERR ${e.message}`,'bad');}}\n"
    "async function refreshBorderBlocks(){log('GET /api/border_blocks');try{state.borderData=await fetchJson('/api/border_blocks');applyLayoutMeta(state.borderData);state.lastBlocksAt=Date.now();drawBorderBlocks();}catch(e){log(`ERR ${e.message}`);}}\n"
    "async function refreshRuntimeBlocks(){if(state.runtimeBusy)return;state.runtimeBusy=true;log('GET /api/runtime_blocks');try{state.borderData=await fetchJson('/api/runtime_blocks');applyLayoutMeta(state.borderData);drawBorderBlocks();setPreviewStatus('Run mode blocks updated','good');}catch(e){log(`ERR ${e.message}`);if(String(e.message).includes('HTTP 500'))setPreviewStatus('Run mode warming up...','busy');else setPreviewStatus(`ERR ${e.message}`,'bad');}finally{state.runtimeBusy=false;}}\n"
    "async function captureSnapshot(){if(state.previewBusy)return false;state.previewBusy=true;setPreviewStatus('Capturing snapshot...','busy');const url=`/api/snap?t=${Date.now()}`;log(`GET ${url}`);try{const r=await fetch(url,{cache:'no-store'});if(!r.ok)throw new Error(`HTTP ${r.status}`);const blob=await r.blob();if(state.previewUrl)URL.revokeObjectURL(state.previewUrl);state.previewUrl=URL.createObjectURL(blob);ui.previewImage.src=state.previewUrl;await waitForImage(ui.previewImage);setPreviewStatus('Preview updated','good');return true;}catch(e){log(`ERR ${e.message}`);setPreviewStatus(`ERR ${e.message}`,'bad');return false;}finally{state.previewBusy=false;}}\n"
    "async function refreshPreviewAndBlocks(){const ok=await captureSnapshot();if(ok)await refreshBorderBlocks();}\n"
    "function startAutoPreview(){if(state.mode==='RUN'){setPreviewStatus('Run mode disables web preview for lowest latency','good');return;}if(state.autoPreviewTimer){clearInterval(state.autoPreviewTimer);state.autoPreviewTimer=null;ui.autoPreviewBtn.textContent='Start Auto Preview';setPreviewStatus('Auto preview stopped');return;}const runner=refreshPreviewAndBlocks;runner();state.autoPreviewTimer=setInterval(()=>runner(),280);ui.autoPreviewBtn.textContent='Stop Auto Preview';setPreviewStatus('Auto preview running','good');}\n"
    "ui.snapBtn.onclick=refreshPreviewAndBlocks;ui.autoPreviewBtn.onclick=startAutoPreview;ui.enterRunBtn.onclick=()=>setMode('RUN');ui.returnDebugBtn.onclick=()=>setMode('DEBUG');ui.wifiScanSelect.onchange=()=>{if(ui.wifiScanSelect.value)ui.wifiSsidInput.value=ui.wifiScanSelect.value;};ui.wifiScanBtn.onclick=scanWiFi;ui.wifiSaveBtn.onclick=saveWiFi;ui.wifiForgetBtn.onclick=forgetWiFi;ui.layoutSelect.onchange=()=>setLayout(ui.layoutSelect.value);ui.installModeSelect.onchange=()=>setInstallMode(ui.installModeSelect.value);ui.showInstallGuideBtn.onclick=toggleInstallGuide;ui.clearLogBtn.onclick=()=>{ui.logBox.textContent='';};ui.loadCalibrationSnapBtn.onclick=loadCalibrationSnapshot;ui.loadCalibrationBtn.onclick=loadCalibration;ui.resetCalibrationBtn.onclick=resetCalibration;ui.saveCalibrationBtn.onclick=saveCalibration;renderParams();bindCalibrationCanvas();resetCalibration();drawRectifiedGuide();drawBorderBlocks();refreshMode().then(refreshLayout).then(refreshInstallMode).then(refreshWiFiStatus).then(scanWiFi).then(()=>state.mode==='RUN'?refreshRuntimeBlocks():refreshValues().then(refreshPreviewAndBlocks).then(loadCalibration)).catch(()=>{});setInterval(()=>refreshWiFiStatus(),5000);\n";

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

static int uart_read_exact(uint8_t *buf, size_t len, int timeout_ms)
{
    size_t received = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    while (received < len) {
        TickType_t now = xTaskGetTickCount();
        int remain_ms;
        int ret;

        if (now >= deadline) {
            break;
        }

        remain_ms = (int)((deadline - now) * portTICK_PERIOD_MS);
        if (remain_ms < 1) {
            remain_ms = 1;
        }

        ret = uart_read_bytes(T23_UART_PORT, buf + received, len - received, pdMS_TO_TICKS(remain_ms > 20 ? 20 : remain_ms));
        if (ret > 0) {
            received += (size_t)ret;
        }
    }

    return (int)received;
}

/*
 * Receive one high-speed RUN frame from T23. Frames are binary and fixed-size,
 * so the C3 can update LEDs without any ASCII parsing in the hot path.
 */
static esp_err_t bridge_receive_runtime_blocks_frame_locked(void)
{
    t23_c3_run_blocks_frame_t frame;
    uint8_t first;
    uint8_t second;

    while (1) {
        if (uart_read_exact(&first, 1, 150) != 1) {
            return ESP_ERR_TIMEOUT;
        }
        if (first != T23_C3_RUN_MAGIC0) {
            continue;
        }
        if (uart_read_exact(&second, 1, 50) != 1) {
            return ESP_ERR_TIMEOUT;
        }
        if (second != T23_C3_RUN_MAGIC1) {
            continue;
        }

        memset(&frame, 0, sizeof(frame));
        frame.magic0 = first;
        frame.magic1 = second;
        if (uart_read_exact(((uint8_t *)&frame) + 2, sizeof(frame) - 2, 100) != (int)(sizeof(frame) - 2)) {
            return ESP_ERR_TIMEOUT;
        }

        if (frame.version != T23_C3_RUN_STREAM_VERSION) {
            continue;
        }
        if (frame.block_count == 0 || frame.block_count > T23_BORDER_BLOCK_COUNT_MAX) {
            continue;
        }
        if (run_blocks_checksum16((const uint8_t *)&frame, sizeof(frame) - sizeof(frame.checksum)) != frame.checksum) {
            ESP_LOGW(TAG, "runtime frame checksum mismatch");
            continue;
        }

        {
            int blocks[T23_BORDER_BLOCK_COUNT_MAX][3];
            int valid[T23_BORDER_BLOCK_COUNT_MAX];
            int i;
            const border_layout_desc_t *layout = (frame.layout == T23_BORDER_LAYOUT_4X3) ? &g_layout_4x3 : &g_layout_16x9;

            for (i = 0; i < frame.block_count; ++i) {
                blocks[i][0] = frame.blocks[i].r;
                blocks[i][1] = frame.blocks[i].g;
                blocks[i][2] = frame.blocks[i].b;
                valid[i] = 1;
            }

            store_latest_blocks(layout->name,
                                frame.block_count,
                                frame.top_blocks,
                                frame.right_blocks,
                                frame.bottom_blocks,
                                frame.left_blocks,
                                0,
                                0,
                                0,
                                0,
                                0,
                                0,
                                0,
                                blocks,
                                valid);
            g_current_layout = layout;
            if (build_border_blocks_json_from_cache(g_latest_border_json, sizeof(g_latest_border_json), &g_latest_blocks) == ESP_OK) {
                g_latest_border_json_valid = 1;
            } else {
                g_latest_border_json_valid = 0;
            }
        }

        return ESP_OK;
    }
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

static esp_err_t bridge_get_all(char *json_buf, size_t json_buf_size)
{
    char line[UART_LINE_MAX];
    int values[sizeof(g_params) / sizeof(g_params[0])] = {0};
    int have_value[sizeof(g_params) / sizeof(g_params[0])] = {0};
    size_t used = 0;
    int len;
    size_t i;

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
    int pts[16] = {0};
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
            if (sscanf(line + 11,
                       "%d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d",
                       &pts[0], &pts[1], &pts[2], &pts[3], &pts[4], &pts[5], &pts[6], &pts[7],
                       &pts[8], &pts[9], &pts[10], &pts[11], &pts[12], &pts[13], &pts[14], &pts[15]) != 16) {
                return ESP_FAIL;
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
    for (i = 0; i < 8; ++i) {
        char tmp[48];

        snprintf(tmp, sizeof(tmp), "%s{\"x\":%d,\"y\":%d}", (i == 0) ? "" : ",", pts[i * 2], pts[i * 2 + 1]);
        strncat(json_buf, tmp, json_buf_size - strlen(json_buf) - 1);
    }
    strncat(json_buf, "]}", json_buf_size - strlen(json_buf) - 1);
    return ESP_OK;
}

static esp_err_t bridge_set_calibration(const char *query, char *json_buf, size_t json_buf_size)
{
    char cmd[256];
    char value[16];
    int iw = 0;
    int ih = 0;
    int pts[16];
    int i;

    memset(pts, 0, sizeof(pts));
    if (httpd_query_key_value(query, "iw", value, sizeof(value)) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    iw = atoi(value);
    if (httpd_query_key_value(query, "ih", value, sizeof(value)) != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    ih = atoi(value);
    for (i = 0; i < 8; ++i) {
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

    snprintf(cmd,
             sizeof(cmd),
             "CAL SET %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d",
             iw, ih,
             pts[0], pts[1], pts[2], pts[3], pts[4], pts[5], pts[6], pts[7],
             pts[8], pts[9], pts[10], pts[11], pts[12], pts[13], pts[14], pts[15]);

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

static esp_err_t bridge_fetch_rectified_locked(uint8_t *jpeg_buf, size_t jpeg_buf_size, size_t *jpeg_len_out)
{
    return bridge_fetch_snapshot_locked_ex("CAL SNAP", "CAL SNAP OK %" SCNu32, jpeg_buf, jpeg_buf_size, jpeg_len_out);
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
 * block frames from T23 and immediately refreshes the LED strip. DEBUG mode
 * parks this task so the web UI can keep using the slower request/response
 * bridge safely.
 */
static void runtime_blocks_task(void *arg)
{
    (void)arg;

    while (1) {
        if (g_c3_mode == C3_MODE_RUN) {
            if (xSemaphoreTake(g_bridge_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
                esp_err_t ret = bridge_receive_runtime_blocks_frame_locked();
                border_blocks_cache_t cache_copy;

                if (ret == ESP_OK) {
                    memcpy(&cache_copy, &g_latest_blocks, sizeof(cache_copy));
                } else {
                    if (ret != ESP_ERR_TIMEOUT) {
                        ESP_LOGW(TAG, "runtime frame receive failed: %s", esp_err_to_name(ret));
                    }
                    reset_blocks_cache(&cache_copy);
                }
                xSemaphoreGive(g_bridge_lock);

                if (cache_copy.ready) {
                    esp_err_t led_ret = update_led_strip_from_cache(&cache_copy);
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

    ret = bridge_fetch_frame_locked(g_latest_jpeg, g_latest_jpeg_capacity, &jpeg_len);
    if (ret != ESP_OK) {
        g_latest_border_json_valid = 0;
        ret = bridge_fetch_snapshot_locked(g_latest_jpeg, g_latest_jpeg_capacity, &jpeg_len);
    }
    if (ret != ESP_OK || jpeg_len == 0) {
        return ESP_FAIL;
    }

    set_common_headers(req);
    httpd_resp_set_type(req, "image/jpeg");
    return httpd_resp_send(req, (const char *)g_latest_jpeg, jpeg_len);
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
    reset_blocks_cache(&g_latest_blocks);

    if (ret != ESP_OK) {
        return send_error_json(req, "mode set failed", HTTPD_500_INTERNAL_SERVER_ERROR);
    }
    if (target == C3_MODE_DEBUG) {
        sm16703sp3_clear();
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
    char json[192];

    snprintf(json,
             sizeof(json),
             "{\"ok\":true,\"installMode\":\"%s\",\"label\":\"%s\",\"installGuideActive\":%s}",
             g_install_mode->name,
             g_install_mode->label,
             g_install_setup_active ? "true" : "false");
    return send_json(req, json);
}

static esp_err_t install_mode_set_handler(httpd_req_t *req)
{
    char query[96];
    char value[32];
    char json[192];
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
             "{\"ok\":true,\"installMode\":\"%s\",\"label\":\"%s\",\"installGuideActive\":%s}",
             g_install_mode->name,
             g_install_mode->label,
             g_install_setup_active ? "true" : "false");
    return send_json(req, json);
}

static esp_err_t install_guide_handler(httpd_req_t *req)
{
    char query[96];
    char value[8];
    char json[192];
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
        ret = sm16703sp3_clear();
        if (ret != ESP_OK) {
            return send_error_json(req, "install guide clear failed", HTTPD_500_INTERNAL_SERVER_ERROR);
        }
    }

    snprintf(json,
             sizeof(json),
             "{\"ok\":true,\"installMode\":\"%s\",\"label\":\"%s\",\"installGuideActive\":%s}",
             g_install_mode->name,
             g_install_mode->label,
             g_install_setup_active ? "true" : "false");
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
    char query[256];
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
static esp_err_t runtime_blocks_handler(httpd_req_t *req)
{
    char json[4096];

    if (!copy_latest_border_json(json, sizeof(json))) {
        snprintf(json, sizeof(json), "{\"ok\":true,\"warming\":true,\"layout\":\"%s\",\"blockCount\":0,\"blocks\":[]}", g_current_layout->name);
    }

    return send_json(req, json);
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
    httpd_uri_t install_guide = {
        .uri = "/api/install_guide",
        .method = HTTP_GET,
        .handler = install_guide_handler,
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
    httpd_uri_t calibration_rectified = {
        .uri = "/api/calibration/rectified",
        .method = HTTP_GET,
        .handler = calibration_rectified_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t border_blocks = {
        .uri = "/api/border_blocks",
        .method = HTTP_GET,
        .handler = border_blocks_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t runtime_blocks = {
        .uri = "/api/runtime_blocks",
        .method = HTTP_GET,
        .handler = runtime_blocks_handler,
        .user_ctx = NULL,
    };
    config.stack_size = 10240;
    config.max_uri_handlers = 24;

    ESP_ERROR_CHECK(httpd_start(&server, &config));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &root));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &app_js));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &styles));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ping));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &layout_get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &layout_set));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &install_mode_get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &install_mode_set));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &install_guide));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &wifi_status));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &wifi_scan));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &wifi_save));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &wifi_forget));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &params));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &set));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &snap));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &mode_get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &mode_set));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &calibration_get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &calibration_set));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &calibration_rectified));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &border_blocks));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &runtime_blocks));

    ESP_LOGI(TAG, "HTTP server started on port %d", config.server_port);
    return ESP_OK;
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
    load_wifi_config();

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
    ESP_LOGI(TAG,
             "Install config loaded: mode=%s active=%d",
             g_install_mode->name,
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
    xTaskCreate(runtime_blocks_task, "runtime_blocks", 12288, NULL, 5, NULL);
}
