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

#include "config.h"

#define APP_LOG_CAPACITY   256
#define APP_LOG_TEXT_MAX   200

#define APP_MESSAGE_MAX    192
#define APP_CONFIG_MAX     512
#define APP_HEX_MAX        8

/* Four faders, 12-bit like the DAC range they are meant to drive. */
#define APP_FADER_COUNT    4
#define APP_FADER_MAX      4095

/* Written beside the executable, next to resources/. */
#define APP_CONFIG_PATH    "config.json"

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

    /* Fader values (0..APP_FADER_MAX, bottom to top) and their names. */
    config_slider_t sliders[APP_FADER_COUNT];

    /* Set when a fader moves or is renamed; the UI flushes it periodically so
       a drag does not write the file on every frame. */
    bool config_dirty;

    /* From the "debug" key. When false the traffic console is hidden. */
    bool debug;

    /*
     * Rises each time the device reports a slider move. The UI watches it and
     * locks its own sliders for a moment, so a fader being moved on the device
     * is not fought by the pointer.
     */
    unsigned long slider_extern_seq;

    /*
     * Set when the user moves a fader, cleared once the device has been told.
     * Reported as "update" so the device knows whether the value it just asked
     * for is newer than the one it holds.
     */
    bool slider_pending[APP_FADER_COUNT];

    /* Set when an inbound update changed a value; the volume is pushed once at
       the end of the poll rather than per packet. */
    bool slider_volume_dirty[APP_FADER_COUNT];

    /* Latches once a fader's applications match no audio session, so the
       warning is written when that starts rather than on every frame. */
    bool slider_unmatched[APP_FADER_COUNT];

    bool live_send;

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

void app_poll(app_t *a);

void app_send_json(app_t *a, const char *json);

void app_set_led(app_t *a);

/**
 * Send {"set":{"slider":{"id":N,"value":V}}}.
 *
 * @param id    Slider index.
 * @param value New position.
 */
void app_send_slider(app_t *a, int id, int value);

/**
 * Answer {"get":{"slider":{"id":N}}} with the slider's full state.
 *
 * @param id Slider index; ignored if out of range.
 */
void app_reply_slider(app_t *a, int id);

/**
 * Push a slider's value to every application it controls.
 *
 * @param id Slider index; ignored if out of range.
 */
void app_apply_volume(app_t *a, int id);
void app_get(app_t *a, const char *what);
void app_send_message(app_t *a);
void app_set_config(app_t *a);

void app_config_load(app_t *a);

void app_config_save(app_t *a);

bool app_config_save_as(app_t *a, const char *path);

bool app_config_load_from(app_t *a, const char *path);

void app_notify(app_t *a, const char *text);

/** Append one line to the log, printf style. */
void app_log(app_t *a, app_log_kind_t kind, const char *fmt, ...);
void app_log_clear(app_t *a);

const app_log_entry_t *app_log_at(const app_t *a, int index);

uint32_t app_rgb(const app_t *a);

void app_set_rgb(app_t *a, uint32_t rgb);

void app_sync_hex(app_t *a);

void app_hsv_to_rgb(float h, float s, float v, uint8_t *r, uint8_t *g, uint8_t *b);
void app_rgb_to_hsv(uint8_t r, uint8_t g, uint8_t b, float *h, float *s, float *v);

#endif /* APP_H */
