/*
 * Minimal WS2812 / NeoPixel wrapper over espressif/led_strip.
 *
 * Kept separate from usb_proto so the USB component carries no LED knowledge
 * and can be reused on boards that have no addressable LED.
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int      gpio;   /*!< GPIO driving the strip's data line */
    uint32_t count;  /*!< Number of LEDs on the strip */
} neopixel_config_t;

#define NEOPIXEL_DEFAULT_CONFIG(gpio_num) ((neopixel_config_t){ \
    .gpio  = (gpio_num),                                       \
    .count = 1,                                                \
})

/**
 * @brief Bring up the strip and clear it.
 */
esp_err_t neopixel_init(const neopixel_config_t *config);

/**
 * @brief Set every LED to one packed 0xRRGGBB colour.
 *
 * The signature matches usb_proto_led_cb_t so it can be handed straight to
 * usb_proto_config_t::on_led_command.
 */
esp_err_t neopixel_set_rgb(uint32_t rgb);

/**
 * @brief Last colour passed to neopixel_set_rgb(), packed 0xRRGGBB.
 *
 * Reports what was requested, not what the strip physically shows; a failed
 * refresh still updates it.
 */
uint32_t neopixel_get_rgb(void);

/**
 * @brief Turn every LED off.
 */
esp_err_t neopixel_clear(void);

/**
 * @brief Release the RMT channel.
 */
esp_err_t neopixel_deinit(void);

#ifdef __cplusplus
}
#endif
