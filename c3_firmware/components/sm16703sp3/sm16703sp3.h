#pragma once

#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SM16703SP3_MAX_LEDS 50

typedef struct {
    gpio_num_t gpio_num;
    size_t led_count;
} sm16703sp3_config_t;

typedef enum {
    SM16703SP3_ORDER_RGB = 0,
    SM16703SP3_ORDER_RBG = 1,
    SM16703SP3_ORDER_GRB = 2,
    SM16703SP3_ORDER_GBR = 3,
    SM16703SP3_ORDER_BRG = 4,
    SM16703SP3_ORDER_BGR = 5,
} sm16703sp3_color_order_t;

/* Initialize the RMT-based SM16703SP3 output driver on the selected GPIO. */
esp_err_t sm16703sp3_init(const sm16703sp3_config_t *config);
/* Turn all configured LEDs off. */
esp_err_t sm16703sp3_clear(void);
/* Change logical RGB-to-wire byte order for strips with non-RGB channel layouts. */
esp_err_t sm16703sp3_set_color_order(sm16703sp3_color_order_t order);
/* Send one full RGB frame to the strip using RGB byte order. */
esp_err_t sm16703sp3_show_rgb(const uint8_t *rgb, size_t led_count);
/* Fill the entire strip with one solid color. */
esp_err_t sm16703sp3_fill_rgb(uint8_t r, uint8_t g, uint8_t b);

#ifdef __cplusplus
}
#endif
