#include "sm16703sp3.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_check.h"
#include "esp_log.h"

#define TAG "sm16703sp3"

#define SM16703SP3_RMT_RESOLUTION_HZ 8000000
#define SM16703SP3_MEM_BLOCK_SYMBOLS 64
#define SM16703SP3_QUEUE_DEPTH 4

typedef struct {
    rmt_encoder_t base;
    rmt_encoder_t *bytes_encoder;
    rmt_encoder_t *copy_encoder;
    int state;
    rmt_symbol_word_t reset_code;
} sm16703sp3_encoder_t;

static rmt_channel_handle_t g_led_chan;
static rmt_encoder_handle_t g_led_encoder;
static uint8_t g_zero_buffer[SM16703SP3_MAX_LEDS * 3];
static size_t g_led_count;
static int g_initialized;

/*
 * Encode one LED frame plus a trailing reset pulse. Timing values are chosen
 * from the SM16703SP3 datasheet's typical ranges so the full strip latches
 * reliably.
 */
static size_t sm16703sp3_encode(rmt_encoder_t *encoder,
                                rmt_channel_handle_t channel,
                                const void *primary_data,
                                size_t data_size,
                                rmt_encode_state_t *ret_state)
{
    sm16703sp3_encoder_t *led_encoder = __containerof(encoder, sm16703sp3_encoder_t, base);
    rmt_encode_state_t session_state = RMT_ENCODING_RESET;
    rmt_encode_state_t state = RMT_ENCODING_RESET;
    size_t encoded_symbols = 0;

    switch (led_encoder->state) {
    case 0:
        encoded_symbols += led_encoder->bytes_encoder->encode(led_encoder->bytes_encoder,
                                                              channel,
                                                              primary_data,
                                                              data_size,
                                                              &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            led_encoder->state = 1;
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            state |= RMT_ENCODING_MEM_FULL;
            goto out;
        }
        /* fall through */
    case 1:
        encoded_symbols += led_encoder->copy_encoder->encode(led_encoder->copy_encoder,
                                                             channel,
                                                             &led_encoder->reset_code,
                                                             sizeof(led_encoder->reset_code),
                                                             &session_state);
        if (session_state & RMT_ENCODING_COMPLETE) {
            led_encoder->state = RMT_ENCODING_RESET;
            state |= RMT_ENCODING_COMPLETE;
        }
        if (session_state & RMT_ENCODING_MEM_FULL) {
            state |= RMT_ENCODING_MEM_FULL;
            goto out;
        }
    }

out:
    *ret_state = state;
    return encoded_symbols;
}

static esp_err_t sm16703sp3_del_encoder(rmt_encoder_t *encoder)
{
    sm16703sp3_encoder_t *led_encoder = __containerof(encoder, sm16703sp3_encoder_t, base);

    if (led_encoder->bytes_encoder) {
        rmt_del_encoder(led_encoder->bytes_encoder);
    }
    if (led_encoder->copy_encoder) {
        rmt_del_encoder(led_encoder->copy_encoder);
    }
    free(led_encoder);
    return ESP_OK;
}

static esp_err_t sm16703sp3_reset_encoder(rmt_encoder_t *encoder)
{
    sm16703sp3_encoder_t *led_encoder = __containerof(encoder, sm16703sp3_encoder_t, base);

    rmt_encoder_reset(led_encoder->bytes_encoder);
    rmt_encoder_reset(led_encoder->copy_encoder);
    led_encoder->state = RMT_ENCODING_RESET;
    return ESP_OK;
}

/* Build the custom RMT encoder used by the strip driver. */
static esp_err_t sm16703sp3_new_encoder(rmt_encoder_handle_t *ret_encoder)
{
    esp_err_t ret = ESP_OK;
    sm16703sp3_encoder_t *led_encoder = NULL;
    rmt_bytes_encoder_config_t bytes_encoder_config = {
        .bit0 = {
            .level0 = 1,
            .duration0 = 2,
            .level1 = 0,
            .duration1 = 8,
        },
        .bit1 = {
            .level0 = 1,
            .duration0 = 7,
            .level1 = 0,
            .duration1 = 3,
        },
        .flags.msb_first = 1,
    };
    rmt_copy_encoder_config_t copy_encoder_config = {};

    ESP_RETURN_ON_FALSE(ret_encoder != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid encoder output");

    led_encoder = calloc(1, sizeof(*led_encoder));
    ESP_GOTO_ON_FALSE(led_encoder, ESP_ERR_NO_MEM, err, TAG, "no mem for encoder");

    led_encoder->base.encode = sm16703sp3_encode;
    led_encoder->base.del = sm16703sp3_del_encoder;
    led_encoder->base.reset = sm16703sp3_reset_encoder;

    ESP_GOTO_ON_ERROR(rmt_new_bytes_encoder(&bytes_encoder_config, &led_encoder->bytes_encoder), err, TAG, "bytes encoder failed");
    ESP_GOTO_ON_ERROR(rmt_new_copy_encoder(&copy_encoder_config, &led_encoder->copy_encoder), err, TAG, "copy encoder failed");

    led_encoder->reset_code = (rmt_symbol_word_t) {
        .level0 = 0,
        .duration0 = 1600,
        .level1 = 0,
        .duration1 = 1600,
    };

    *ret_encoder = &led_encoder->base;
    return ESP_OK;

err:
    if (led_encoder) {
        if (led_encoder->bytes_encoder) {
            rmt_del_encoder(led_encoder->bytes_encoder);
        }
        if (led_encoder->copy_encoder) {
            rmt_del_encoder(led_encoder->copy_encoder);
        }
        free(led_encoder);
    }
    return ret;
}

esp_err_t sm16703sp3_init(const sm16703sp3_config_t *config)
{
    rmt_tx_channel_config_t tx_chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = config ? config->gpio_num : GPIO_NUM_NC,
        .mem_block_symbols = SM16703SP3_MEM_BLOCK_SYMBOLS,
        .resolution_hz = SM16703SP3_RMT_RESOLUTION_HZ,
        .trans_queue_depth = SM16703SP3_QUEUE_DEPTH,
    };
    esp_err_t ret;

    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "missing config");
    ESP_RETURN_ON_FALSE(config->led_count > 0 && config->led_count <= SM16703SP3_MAX_LEDS,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "invalid led count");

    if (g_initialized) {
        return ESP_OK;
    }

    ret = rmt_new_tx_channel(&tx_chan_config, &g_led_chan);
    ESP_RETURN_ON_ERROR(ret, TAG, "new tx channel failed");
    ret = sm16703sp3_new_encoder(&g_led_encoder);
    ESP_RETURN_ON_ERROR(ret, TAG, "new encoder failed");
    ret = rmt_enable(g_led_chan);
    ESP_RETURN_ON_ERROR(ret, TAG, "enable channel failed");

    g_led_count = config->led_count;
    g_initialized = 1;
    memset(g_zero_buffer, 0, sizeof(g_zero_buffer));

    ESP_LOGI(TAG, "initialized on GPIO%d for %u LEDs", (int)config->gpio_num, (unsigned)g_led_count);
    return sm16703sp3_clear();
}

esp_err_t sm16703sp3_clear(void)
{
    if (!g_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    return sm16703sp3_show_rgb(g_zero_buffer, g_led_count);
}

/*
 * Send one complete RGB frame. The implementation emits an explicit leading
 * reset, then RGB data, then waits for the final low period generated by the
 * custom encoder before returning.
 */
esp_err_t sm16703sp3_show_rgb(const uint8_t *rgb, size_t led_count)
{
    static uint8_t rgb_ordered[SM16703SP3_MAX_LEDS * 3];
    rmt_transmit_config_t tx_config = {
        .loop_count = 0,
    };
    rmt_symbol_word_t reset_symbol = {
        .level0 = 0,
        .duration0 = 1600,
        .level1 = 0,
        .duration1 = 1600,
    };
    rmt_copy_encoder_config_t copy_encoder_config = {};
    rmt_encoder_handle_t reset_encoder = NULL;
    size_t i;

    ESP_RETURN_ON_FALSE(g_initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(rgb != NULL, ESP_ERR_INVALID_ARG, TAG, "missing rgb");
    ESP_RETURN_ON_FALSE(led_count > 0 && led_count <= g_led_count, ESP_ERR_INVALID_ARG, TAG, "invalid led count");

    for (i = 0; i < led_count; ++i) {
        rgb_ordered[i * 3 + 0] = rgb[i * 3 + 0];
        rgb_ordered[i * 3 + 1] = rgb[i * 3 + 1];
        rgb_ordered[i * 3 + 2] = rgb[i * 3 + 2];
    }

    ESP_RETURN_ON_ERROR(rmt_new_copy_encoder(&copy_encoder_config, &reset_encoder), TAG, "reset encoder failed");
    ESP_RETURN_ON_ERROR(rmt_transmit(g_led_chan, reset_encoder, &reset_symbol, sizeof(reset_symbol), &tx_config), TAG, "leading reset failed");
    ESP_RETURN_ON_ERROR(rmt_tx_wait_all_done(g_led_chan, portMAX_DELAY), TAG, "leading reset wait failed");
    rmt_del_encoder(reset_encoder);

    ESP_RETURN_ON_ERROR(rmt_transmit(g_led_chan, g_led_encoder, rgb_ordered, led_count * 3, &tx_config), TAG, "transmit failed");
    return rmt_tx_wait_all_done(g_led_chan, portMAX_DELAY);
}

esp_err_t sm16703sp3_fill_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    static uint8_t rgb[SM16703SP3_MAX_LEDS * 3];
    size_t i;

    ESP_RETURN_ON_FALSE(g_initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    for (i = 0; i < g_led_count; ++i) {
        rgb[i * 3 + 0] = r;
        rgb[i * 3 + 1] = g;
        rgb[i * 3 + 2] = b;
    }
    return sm16703sp3_show_rgb(rgb, g_led_count);
}
