/*
 * Composite USB device: a CDC-ACM serial port plus a vendor-specific interface
 * carrying a small text protocol on raw bulk endpoints.
 *
 * Everything USB lives behind this header -- descriptors, the BOS / MS OS 2.0
 * blocks that bind WinUSB on Windows, TinyUSB installation, endpoint FIFO
 * handling and command dispatch. Callers supply callbacks and call
 * usb_proto_start().
 *
 * Wire protocol, host -> device. One JSON object per bulk transfer:
 *
 *   {"set":{"led":"ABCDEF"}}                 dispatched to on_led_command
 *   {"set":{"message":"This is a test"}}     dispatched to on_text_message
 *   {"set":{"config":{ ... }}}               dispatched to on_config_set
 *   {"get":"led"}          ->  {"led":"ABCDEF"}
 *   {"get":"config"}       ->  {"config":{ ... }}
 *
 * A "set" object may carry several keys at once; all recognised ones are
 * applied. Every "set" is answered with {"ok":true} or, on failure,
 * {"ok":false,"error":"..."}.
 *
 * Transfers larger than one 64-byte packet are reassembled, so both requests
 * and replies may exceed the endpoint size. A payload that never parses is
 * handed to on_raw_packet, or answered with an error object if none is set.
 *
 * Wire protocol, device -> host, outside of replies:
 *   0x5A followed by a little-endian uint32 counter, once per second while idle
 *                  (when heartbeat is enabled).
 *   {"interrupt":{"gpio":0,"state":0,"message":"..."}}
 *                  a GPIO interrupt report, sent unprompted
 *   anything else  asynchronous events queued with usb_proto_send_event().
 *
 * Target: ESP32-S3 full-speed USB-OTG, esp_tinyusb 2.x on ESP-IDF v6.0.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Handler for "LED.RRGGBB". @p rgb is packed 0xRRGGBB. */
typedef esp_err_t (*usb_proto_led_cb_t)(uint32_t rgb);

/** Handler for "MSG.<text>", quotes already stripped. @p text is NUL terminated. */
typedef void (*usb_proto_msg_cb_t)(const char *text, size_t len);

/** Handler for a payload that is not valid JSON. */
typedef void (*usb_proto_raw_cb_t)(const uint8_t *data, size_t len);

/** Reports the current LED colour for {"get":"led"}. */
typedef uint32_t (*usb_proto_led_query_cb_t)(void);

/**
 * Serialises the configuration for {"get":"config"}.
 *
 * Must return a newly allocated JSON object string; usb_proto releases it with
 * free(). Returning NULL yields an error reply.
 */
typedef char *(*usb_proto_config_get_cb_t)(void);

/** Applies a JSON object from {"set":{"config":{...}}}. */
typedef esp_err_t (*usb_proto_config_set_cb_t)(const char *json);

typedef struct {
    /* Command handlers. A NULL handler falls back to the built-in behaviour
       described beside each field. */
    usb_proto_led_cb_t on_led_command;   /*!< NULL: reply with an error */
    usb_proto_msg_cb_t on_text_message;  /*!< NULL: print on CDC and to the log */
    usb_proto_raw_cb_t on_raw_packet;    /*!< NULL: echo back 0xA5 + payload */

    usb_proto_led_query_cb_t  on_led_query;   /*!< NULL: {"get":"led"} errors    */
    usb_proto_config_get_cb_t on_config_get;  /*!< NULL: {"get":"config"} errors */
    usb_proto_config_set_cb_t on_config_set;  /*!< NULL: config writes error     */

    bool heartbeat;   /*!< Emit the 1 Hz counter packet while idle */
    bool cdc_echo;    /*!< Echo back whatever is typed at the CDC port */

    /*
     * Status LED. When enabled, on_led_command is also driven on state changes:
     * led_connected once the host configures the device, then a brief flash of
     * led_receive per incoming packet before returning to the idle colour.
     *
     * An explicit LED.RRGGBB command replaces the idle colour, so a colour set
     * by the host survives subsequent receive flashes instead of being reverted.
     *
     * The boot colour is not listed here: it applies before the USB stack is
     * running, so the caller sets it directly.
     */
    bool     status_led;
    uint32_t led_connected;  /*!< packed 0xRRGGBB */
    uint32_t led_receive;    /*!< packed 0xRRGGBB */

    /* USB identity. NULL strings keep the built-in defaults. */
    const char *manufacturer;
    const char *product;
    const char *serial;
    uint16_t    vid;
    uint16_t    pid;

    /*
     * Windows caches MS OS descriptor results per VID/PID/bcdDevice under
     * HKLM\SYSTEM\CurrentControlSet\Control\usbflags and never re-queries a
     * triple it has already seen. Bump this whenever the descriptors change,
     * or Windows keeps the stale answer and refuses to bind WinUSB.
     */
    uint16_t bcd_device;
} usb_proto_config_t;

#define USB_PROTO_DEFAULT_CONFIG() ((usb_proto_config_t){ \
    .heartbeat  = true,                                   \
    .cdc_echo   = true,                                   \
    .vid        = 0x303A,                                 \
    .pid        = 0x4001,                                 \
    .bcd_device = 0x0102,                                 \
})

/**
 * @brief Install TinyUSB, bring up both functions and start the dispatch task.
 */
esp_err_t usb_proto_start(const usb_proto_config_t *config);

/**
 * @brief Tear everything down again.
 */
esp_err_t usb_proto_stop(void);

/**
 * @brief True once the host has configured the vendor interface.
 */
bool usb_proto_vendor_mounted(void);

/**
 * @brief Send raw bytes on the vendor bulk IN endpoint.
 *
 * Bounded internally, so a host that has stopped reading cannot wedge the
 * caller. Returns ESP_ERR_TIMEOUT if the transmit FIFO never drained.
 *
 * TinyUSB's vendor FIFOs carry no mutex. The component's own dispatch task is
 * one writer already, so call this from one task only, or serialise it
 * yourself.
 */
esp_err_t usb_proto_vendor_send(const uint8_t *data, size_t len);

/** Largest asynchronous event payload, in bytes. */
#define USB_PROTO_EVENT_MAX 192

/**
 * @brief Queue bytes to be sent on the vendor IN endpoint by the dispatch task.
 *
 * Safe to call from any task, unlike usb_proto_vendor_send(), because the
 * actual write still happens on the single writer. Returns ESP_ERR_NO_MEM if
 * the outbound queue is full, or ESP_ERR_INVALID_SIZE if @p len exceeds
 * USB_PROTO_EVENT_MAX. Not callable from an ISR.
 */
esp_err_t usb_proto_send_event(const void *data, size_t len);

/**
 * @brief Report a GPIO interrupt to the host.
 *
 * Queues one JSON object:
 * @code
 * {"interrupt":{"gpio":0,"state":0,"message":"Button Triggered"}}
 * @endcode
 *
 * @param gpio    Pin that triggered.
 * @param level   Pin level at the moment it triggered.
 * @param message NUL-terminated string placed in the "message" field, or NULL
 *                for an empty one. Escaped on the way out, so any text is safe.
 *
 * Matches button_cb_t, so a button can be wired directly to a USB event:
 * @code
 * button_init(&cfg, usb_proto_send_interrupt_cb, "Button Triggered");
 * @endcode
 */
void usb_proto_send_interrupt_cb(int gpio, int level, void *message);

/**
 * @brief Write text to the CDC serial port.
 *
 * Note that the host discards device-to-host serial data unless something has
 * the COM port open at that moment.
 */
esp_err_t usb_proto_cdc_print(const char *text, size_t len);

#ifdef __cplusplus
}
#endif
