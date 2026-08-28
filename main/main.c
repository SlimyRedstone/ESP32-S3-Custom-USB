#include "esp_err.h"

#include "button.h"
#include "config.h"
#include "neopixel.h"
#include "usb_proto.h"

#define NEOPIXEL_GPIO   21
#define BUTTON_GPIO     0       /* BOOT button: switch to GND against a pull-up */

/* Placed in the "message" field of the interrupt report. */
#define BUTTON_MESSAGE  "Button Triggered"

void app_main(void) {
    /* Mounts SPIFFS and loads /config.json, writing defaults on first boot. */
    ESP_ERROR_CHECK(config_init());

    const neopixel_config_t led_cfg = NEOPIXEL_DEFAULT_CONFIG(NEOPIXEL_GPIO);
    ESP_ERROR_CHECK(neopixel_init(&led_cfg));

    /* Booting, USB not ready yet. */
    ESP_ERROR_CHECK(neopixel_set_rgb(config_led_color(LED_STATE_BOOT)));

    usb_proto_config_t usb_cfg = USB_PROTO_DEFAULT_CONFIG();
    usb_cfg.on_led_command = neopixel_set_rgb;
    usb_cfg.on_led_query   = neopixel_get_rgb;
    usb_cfg.on_config_get  = config_to_json;
    usb_cfg.on_config_set  = config_from_json;
    usb_cfg.product        = "S3 Custom USB Dongle";
    usb_cfg.status_led     = true;
    usb_cfg.led_connected  = config_led_color(LED_STATE_CONNECTED);
    usb_cfg.led_receive    = config_led_color(LED_STATE_RECEIVE);
    ESP_ERROR_CHECK(usb_proto_start(&usb_cfg));

    /* A press queues {"interrupt":{"gpio":..,"state":..,"message":..}}. */
    const button_config_t btn_cfg = BUTTON_DEFAULT_CONFIG(BUTTON_GPIO);
    ESP_ERROR_CHECK(button_init(&btn_cfg, usb_proto_send_interrupt_cb, BUTTON_MESSAGE));
}
