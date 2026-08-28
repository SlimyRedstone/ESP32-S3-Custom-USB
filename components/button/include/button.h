/*
 * Edge-triggered GPIO button with debouncing.
 *
 * The ISR only notifies a worker task, so the callback runs in task context and
 * may do real work -- including USB transfers, which are illegal from an ISR.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Fired once per debounced press, in task context.
 *
 * The signature matches usb_proto_send_event_cb(), so a button can be wired
 * straight to a USB event with no glue function.
 */
typedef void (*button_cb_t)(void *arg);

typedef struct {
    int      gpio;
    /*
     * true:  idle high via the internal pull-up, event on the falling edge.
     *        This is the wiring of the BOOT button on GPIO0 -- a switch to GND
     *        against an external pull-up.
     * false: idle low via the internal pull-down, event on the rising edge.
     */
    bool     active_low;
    uint32_t debounce_ms;
} button_config_t;

#define BUTTON_DEFAULT_CONFIG(gpio_num) ((button_config_t){ \
    .gpio        = (gpio_num),                              \
    .active_low  = true,                                    \
    .debounce_ms = 50,                                      \
})

/**
 * @brief Configure the pin, install the ISR and start the worker task.
 *
 * @param config  Pin and debounce settings.
 * @param cb      Called once per press.
 * @param arg     Passed through to @p cb untouched.
 */
esp_err_t button_init(const button_config_t *config, button_cb_t cb, void *arg);

/**
 * @brief Remove the ISR and stop the worker task.
 */
esp_err_t button_deinit(void);

#ifdef __cplusplus
}
#endif
