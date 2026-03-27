#include <inttypes.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_slave.h"
#include "esp_err.h"
#include "esp_log.h"

#include "t23_c3_protocol.h"

#define TAG "c3_spi_diag"

#define C3_SPI_HOST SPI2_HOST

#define PIN_NUM_MOSI 10
#define PIN_NUM_MISO 8
#define PIN_NUM_CLK 6
#define PIN_NUM_CS 5
#define PIN_NUM_DATA_READY 3

/*
 * ESP32-C3 side SPI bring-up firmware.
 *
 * Current scope:
 * - behave as a deterministic SPI slave
 * - assert Data Ready when a response buffer is queued
 * - return a fixed 4-byte response so T23 can verify MISO wiring
 *
 * WiFi, LED logic and forwarding are intentionally out of scope for this
 * stage. We want the electrical link verified before higher-level behavior.
 */
static void post_setup_cb(spi_slave_transaction_t *trans)
{
    (void)trans;
    /* Tell T23 that the slave response buffer is ready to be clocked out. */
    gpio_set_level(PIN_NUM_DATA_READY, 1);
}

static void post_trans_cb(spi_slave_transaction_t *trans)
{
    (void)trans;
    /* Lower the ready signal after the transaction has finished. */
    gpio_set_level(PIN_NUM_DATA_READY, 0);
}

static void prepare_fixed_response(uint8_t *tx_buf, size_t len)
{
    size_t i;

    /* Fill the whole DMA buffer so unexpected extra bytes are obvious. */
    for (i = 0; i < len; ++i) {
        tx_buf[i] = 0xFF;
    }

    if (len >= T23_C3_SPI_TEST_LEN) {
        tx_buf[0] = T23_C3_SPI_TEST_RESP0;
        tx_buf[1] = T23_C3_SPI_TEST_RESP1;
        tx_buf[2] = T23_C3_SPI_TEST_RESP2;
        tx_buf[3] = T23_C3_SPI_TEST_RESP3;
    }
}

void app_main(void)
{
    esp_err_t ret;
    uint8_t *tx_buf;
    uint8_t *rx_buf;
    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 32,
    };
    spi_slave_interface_config_t slvcfg = {
        .mode = 0,
        .spics_io_num = PIN_NUM_CS,
        .queue_size = 1,
        .flags = 0,
        .post_setup_cb = post_setup_cb,
        .post_trans_cb = post_trans_cb,
    };
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << PIN_NUM_DATA_READY,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    spi_slave_transaction_t t;

    ESP_ERROR_CHECK(gpio_config(&io_conf));
    ESP_ERROR_CHECK(gpio_set_level(PIN_NUM_DATA_READY, 0));

    gpio_set_pull_mode(PIN_NUM_MOSI, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(PIN_NUM_CLK, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(PIN_NUM_CS, GPIO_PULLUP_ONLY);

    ret = spi_slave_initialize(C3_SPI_HOST, &buscfg, &slvcfg, SPI_DMA_CH_AUTO);
    ESP_ERROR_CHECK(ret);

    tx_buf = spi_bus_dma_memory_alloc(C3_SPI_HOST, 32, 0);
    rx_buf = spi_bus_dma_memory_alloc(C3_SPI_HOST, 32, 0);
    assert(tx_buf && rx_buf);

    ESP_LOGI(TAG, "SPI slave ready");
    ESP_LOGI(TAG, "pins: mosi=%d miso=%d clk=%d cs=%d dr=%d",
             PIN_NUM_MOSI, PIN_NUM_MISO, PIN_NUM_CLK, PIN_NUM_CS, PIN_NUM_DATA_READY);
    ESP_LOGI(TAG, "fixed response: %02X %02X %02X %02X",
             T23_C3_SPI_TEST_RESP0,
             T23_C3_SPI_TEST_RESP1,
             T23_C3_SPI_TEST_RESP2,
             T23_C3_SPI_TEST_RESP3);

    while (1) {
        memset(rx_buf, 0, 32);
        prepare_fixed_response(tx_buf, 32);
        memset(&t, 0, sizeof(t));
        /* Stage-1 contract: always return the same 4-byte test pattern. */
        t.length = T23_C3_SPI_TEST_LEN * 8;
        t.tx_buffer = tx_buf;
        t.rx_buffer = rx_buf;

        ret = spi_slave_transmit(C3_SPI_HOST, &t, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "spi_slave_transmit failed: %s", esp_err_to_name(ret));
            continue;
        }

        ESP_LOGI(TAG, "rx: %02X %02X %02X %02X",
                 rx_buf[0], rx_buf[1], rx_buf[2], rx_buf[3]);
    }
}
