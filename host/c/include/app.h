/*
 * Application state shared between the GUI and the USB transport.
 *
 * Holds the connection, the colour currently being edited, the text buffers the
 * UI types into, and a ring buffer of traffic. The USB side is polled with a
 * near-zero timeout so a frame never blocks on the device.
 */

#ifndef APP_H
#define APP_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Deliberately no #include "usbdev.h": that pulls in libusb.h, which on Windows
 * pulls in windows.h, whose Rectangle/CloseWindow/ShowCursor collide with
 * raylib. The UI includes this header, so the device stays behind a pointer.
 */
typedef struct usbdev usbdev_t;

#define APP_LOG_CAPACITY   256
#define APP_LOG_TEXT_MAX   200

#define APP_MESSAGE_MAX    192
#define APP_CONFIG_MAX     512
#define APP_HEX_MAX        8

typedef enum {
    APP_LOG_TX,        /*!< something we sent            */
    APP_LOG_RX,        /*!< a reply                      */
    APP_LOG_EVENT,     /*!< unprompted device traffic    */
    APP_LOG_ERROR,
} app_log_kind_t;

typedef struct {
    app_log_kind_t kind;
    char           text[APP_LOG_TEXT_MAX];
} app_log_entry_t;

typedef struct {
    usbdev_t *dev;
    bool      connected;

    /* Colour being edited, kept as HSV so the wheel and the brightness slider
       stay independent. */
    float hue;          /*!< 0..360 */
    float sat;          /*!< 0..1   */
    float val;          /*!< 0..1   */

    char message[APP_MESSAGE_MAX];
    char config[APP_CONFIG_MAX];
    char hex[APP_HEX_MAX];

    bool show_heartbeats;
    bool live_send;
    unsigned long heartbeats;

    /* Ring buffer; oldest entry is dropped once it fills. */
    app_log_entry_t log[APP_LOG_CAPACITY];
    int             log_count;
    int             log_first;

    /* Rises on every appended line. log_count stops changing once the ring is
       full, so it cannot be used to detect new traffic. */
    unsigned long   log_seq;

    /* Most recent notification. The UI watches notice_seq for changes and runs
       its own timer, so nothing here depends on a clock. */
    char            notice[APP_LOG_TEXT_MAX];
    unsigned long   notice_seq;
} app_t;

void app_init(app_t *a);
void app_shutdown(app_t *a);

bool app_connect(app_t *a);
void app_disconnect(app_t *a);

/** Drain anything waiting on the IN endpoint. Call once per frame. */
void app_poll(app_t *a);

/** Send a raw JSON document and log it. */
void app_send_json(app_t *a, const char *json);

void app_set_led(app_t *a);
void app_get(app_t *a, const char *what);
void app_send_message(app_t *a);
void app_set_config(app_t *a);

/** Raise a notification for the UI to surface. */
void app_notify(app_t *a, const char *text);

/** Append one line to the log, printf style. */
void app_log(app_t *a, app_log_kind_t kind, const char *fmt, ...);
void app_log_clear(app_t *a);

/** Oldest-first access to the log ring. */
const app_log_entry_t *app_log_at(const app_t *a, int index);

/** Current colour packed as 0xRRGGBB. */
uint32_t app_rgb(const app_t *a);

/** Adopt a packed colour, e.g. one reported by the device. */
void app_set_rgb(app_t *a, uint32_t rgb);

/** Refresh the hex text field from the current colour. */
void app_sync_hex(app_t *a);

void app_hsv_to_rgb(float h, float s, float v, uint8_t *r, uint8_t *g, uint8_t *b);
void app_rgb_to_hsv(uint8_t r, uint8_t g, uint8_t b, float *h, float *s, float *v);

#endif /* APP_H */
