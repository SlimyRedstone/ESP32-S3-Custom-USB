/*
 * Composite USB device on an ESP32-S3: a CDC-ACM serial port plus a
 * vendor-specific interface carrying a small text protocol.
 *
 * All USB work lives in the usb_proto component, LED work in neopixel, the
 * GPIO interrupt in button, and the JSON settings in config. This file only
 * declares the hardware and wires the pieces together.
 *
 * Commands, sent as JSON to the vendor bulk OUT endpoint:
 *   {"set":{"led":"ABCDEF"}}              set the NeoPixel
 *   {"set":{"message":"a test"}}          print on the CDC serial port
 *   {"set":{"config":{ ... }}}            merge into /config.json and save
 *   {"get":"led"}     -> {"led":"ABCDEF"}
 *   {"get":"config"}  -> {"config":{ ... }}
 *
 * Unsolicited, on the vendor bulk IN endpoint:
 *   "Button Triggered"  once per GPIO0 press
 *   0x5A + uint32       heartbeat counter, once per second while idle
 *
 * LED status colours come from /config.json on the SPIFFS volume.
 *
 * USB-OTG port only: GPIO19 = D-, GPIO20 = D+.
 */

#include "esp_err.h"

#include "button.h"
#include "config.h"
#include "neopixel.h"
#include "usb_proto.h"

#define NEOPIXEL_GPIO   21
#define BUTTON_GPIO     0       /* BOOT button: switch to GND against a pull-up */

#define BUTTON_PACKET   "Button Triggered"

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
    usb_cfg.product        = "Custom USB Protocol";
    usb_cfg.status_led     = true;
    usb_cfg.led_connected  = config_led_color(LED_STATE_CONNECTED);
    usb_cfg.led_receive    = config_led_color(LED_STATE_RECEIVE);
    ESP_ERROR_CHECK(usb_proto_start(&usb_cfg));

    /* Pressing the button queues BUTTON_PACKET on the vendor IN endpoint. */
    const button_config_t btn_cfg = BUTTON_DEFAULT_CONFIG(BUTTON_GPIO);
    ESP_ERROR_CHECK(button_init(&btn_cfg, usb_proto_send_event_cb, BUTTON_PACKET));
}
