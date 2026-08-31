#include "app.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jsoncmd.h"
#include "proto.h"
#include "usbdev.h"

#define USB_VID     0x303A
#define USB_PID     0x4001

#define SEND_TIMEOUT_MS  500

/* Near-zero so a frame is never held up waiting on the device. */
#define POLL_TIMEOUT_MS  1

/* Bound the work per frame in case the device is chatty. */
#define POLL_MAX_PACKETS 8

void app_hsv_to_rgb(float h, float s, float v, uint8_t *r, uint8_t *g, uint8_t *b)
{
    float c = v * s;
    float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    float m = v - c;
    float rf = 0, gf = 0, bf = 0;

    if      (h <  60) { rf = c; gf = x; }
    else if (h < 120) { rf = x; gf = c; }
    else if (h < 180) { gf = c; bf = x; }
    else if (h < 240) { gf = x; bf = c; }
    else if (h < 300) { rf = x; bf = c; }
    else              { rf = c; bf = x; }

    *r = (uint8_t)lroundf((rf + m) * 255.0f);
    *g = (uint8_t)lroundf((gf + m) * 255.0f);
    *b = (uint8_t)lroundf((bf + m) * 255.0f);
}

void app_rgb_to_hsv(uint8_t r8, uint8_t g8, uint8_t b8, float *h, float *s, float *v)
{
    float r = r8 / 255.0f, g = g8 / 255.0f, b = b8 / 255.0f;
    float max = fmaxf(r, fmaxf(g, b));
    float min = fminf(r, fminf(g, b));
    float d = max - min;

    float hue = 0.0f;
    if (d > 0.0f) {
        if      (max == r) hue = 60.0f * fmodf((g - b) / d, 6.0f);
        else if (max == g) hue = 60.0f * (((b - r) / d) + 2.0f);
        else               hue = 60.0f * (((r - g) / d) + 4.0f);
    }
    if (hue < 0.0f) {
        hue += 360.0f;
    }

    *h = hue;
    *s = (max <= 0.0f) ? 0.0f : d / max;
    *v = max;
}

uint32_t app_rgb(const app_t *a)
{
    uint8_t r, g, b;
    app_hsv_to_rgb(a->hue, a->sat, a->val, &r, &g, &b);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

void app_set_rgb(app_t *a, uint32_t rgb)
{
    app_rgb_to_hsv((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF,
                   &a->hue, &a->sat, &a->val);
    app_sync_hex(a);
}

void app_sync_hex(app_t *a)
{
    snprintf(a->hex, sizeof(a->hex), "%06X", (unsigned)app_rgb(a));
}

void app_log(app_t *a, app_log_kind_t kind, const char *fmt, ...)
{
    int slot;
    if (a->log_count < APP_LOG_CAPACITY) {
        slot = (a->log_first + a->log_count) % APP_LOG_CAPACITY;
        a->log_count++;
    } else {
        slot = a->log_first;
        a->log_first = (a->log_first + 1) % APP_LOG_CAPACITY;
    }

    a->log[slot].kind = kind;
    a->log_seq++;

    va_list args;
    va_start(args, fmt);
    vsnprintf(a->log[slot].text, APP_LOG_TEXT_MAX, fmt, args);
    va_end(args);
}

void app_config_load(app_t *a)
{
    if (config_load(APP_CONFIG_PATH, a->sliders, APP_FADER_COUNT, &a->debug)) {
        app_log(a, APP_LOG_EVENT, "loaded %s", APP_CONFIG_PATH);
    } else {
        /* Absent or unreadable: start from the defaults and write them out so
           the file exists for editing. */
        app_log(a, APP_LOG_EVENT, "no %s, using defaults", APP_CONFIG_PATH);
        a->config_dirty = true;
    }
}

void app_config_save(app_t *a)
{
    if (!a->config_dirty) {
        return;
    }
    a->config_dirty = false;

    if (!config_save(APP_CONFIG_PATH, a->sliders, APP_FADER_COUNT, a->debug)) {
        app_log(a, APP_LOG_ERROR, "could not write %s", APP_CONFIG_PATH);
    }
}

bool app_config_save_as(app_t *a, const char *path)
{
    if (!config_save(path, a->sliders, APP_FADER_COUNT, a->debug)) {
        app_log(a, APP_LOG_ERROR, "could not write %s", path);
        return false;
    }

    app_log(a, APP_LOG_EVENT, "saved %s", path);
    return true;
}

bool app_config_load_from(app_t *a, const char *path)
{
    if (!config_load(path, a->sliders, APP_FADER_COUNT, &a->debug)) {
        /* config_load has already filled in the defaults, so the strip stays
           usable; only report that the file was not understood. */
        app_log(a, APP_LOG_ERROR, "could not read %s, defaults applied", path);
        a->config_dirty = true;
        return false;
    }

    app_log(a, APP_LOG_EVENT, "loaded %s", path);
    a->config_dirty = true;     /* mirror it into the working config.json */
    return true;
}

void app_notify(app_t *a, const char *text)
{
    snprintf(a->notice, sizeof(a->notice), "%s", text ? text : "");
    a->notice_seq++;
}

void app_log_clear(app_t *a)
{
    a->log_count = 0;
    a->log_first = 0;
    a->log_seq++;
}

const app_log_entry_t *app_log_at(const app_t *a, int index)
{
    if (index < 0 || index >= a->log_count) {
        return NULL;
    }
    return &a->log[(a->log_first + index) % APP_LOG_CAPACITY];
}

void app_init(app_t *a)
{
    memset(a, 0, sizeof(*a));

    a->hue = 0.0f;
    a->sat = 1.0f;
    a->val = 1.0f;
    a->live_send = false;
    a->debug = CONFIG_DEBUG_DEFAULT;

    snprintf(a->message, sizeof(a->message), "This is a test");
    app_sync_hex(a);

    app_config_load(a);

    app_log(a, APP_LOG_EVENT, "not connected");
}

void app_shutdown(app_t *a)
{
    app_config_save(a);

    if (a->connected) {
        app_disconnect(a);
    }
    free(a->dev);
    a->dev = NULL;
}

bool app_connect(app_t *a)
{
    if (a->connected) {
        return true;
    }

    if (a->dev == NULL) {
        a->dev = calloc(1, sizeof(*a->dev));
        if (a->dev == NULL) {
            app_log(a, APP_LOG_ERROR, "out of memory");
            return false;
        }
    }

    if (usbdev_open(a->dev, USB_VID, USB_PID) != 0) {
        app_log(a, APP_LOG_ERROR, "connect failed -- see the console for details");
        return false;
    }

    a->connected = true;
    app_log(a, APP_LOG_EVENT, "connected: interface %d, OUT 0x%02X, IN 0x%02X",
            a->dev->interface, a->dev->ep_out, a->dev->ep_in);
    return true;
}

void app_disconnect(app_t *a)
{
    if (!a->connected) {
        return;
    }
    usbdev_close(a->dev);
    a->connected = false;
    app_log(a, APP_LOG_EVENT, "disconnected");
}

void app_send_json(app_t *a, const char *json)
{
    if (!a->connected) {
        app_log(a, APP_LOG_ERROR, "not connected");
        return;
    }

    app_log(a, APP_LOG_TX, "-> %s", json);

    if (usbdev_send(a->dev, json, strlen(json), SEND_TIMEOUT_MS) != 0) {
        app_log(a, APP_LOG_ERROR, "send failed, dropping the connection");
        app_disconnect(a);
    }
}

static void app_handle_packet(app_t *a, const unsigned char *data, int len)
{
    switch (proto_classify(data, len)) {
    case PROTO_INTERRUPT: {
        app_log(a, APP_LOG_EVENT, "<- %.*s", len, (const char *)data);

        const char *message = jsoncmd_find_string(data, len, "message");
        if (message && message[0]) {
            app_notify(a, message);
        } else {
            app_notify(a, "interrupt");
        }
        return;
    }

    case PROTO_EVENT:
        app_log(a, APP_LOG_EVENT, "<- %.*s", len, (const char *)data);
        return;

    case PROTO_REPLY:
    default:
        break;
    }

    app_log(a, APP_LOG_RX, "<- %.*s", len, (const char *)data);

    const char *led = jsoncmd_find_string(data, len, "led");
    if (led) {
        unsigned rgb;
        if (sscanf(led, "%6x", &rgb) == 1) {
            app_set_rgb(a, rgb);
        }
        return;
    }

    const char *config = jsoncmd_find_object(data, len, "config");
    if (config) {
        size_t n = strlen(config);
        if (n >= sizeof(a->config)) {
            n = sizeof(a->config) - 1;
        }
        memcpy(a->config, config, n);
        a->config[n] = '\0';
    }
}

void app_poll(app_t *a)
{
    if (!a->connected) {
        return;
    }

    for (int i = 0; i < POLL_MAX_PACKETS; i++) {
        unsigned char buf[512];
        int len = 0;

        int rc = usbdev_recv(a->dev, buf, sizeof(buf), &len, POLL_TIMEOUT_MS);
        if (rc == LIBUSB_ERROR_TIMEOUT) {
            return;         /* nothing waiting */
        }
        if (rc == LIBUSB_ERROR_NO_DEVICE) {
            app_log(a, APP_LOG_ERROR, "device unplugged");
            app_disconnect(a);
            return;
        }
        if (rc != 0) {
            return;         /* transient; try again next frame */
        }
        if (len > 0) {
            app_handle_packet(a, buf, len);
        }
    }
}

void app_set_led(app_t *a)
{
    char json[64];
    snprintf(json, sizeof(json), "{\"set\":{\"led\":\"%06X\"}}",
             (unsigned)app_rgb(a));
    app_send_json(a, json);
}

void app_get(app_t *a, const char *what)
{
    char json[64];
    snprintf(json, sizeof(json), "{\"get\":\"%s\"}", what);
    app_send_json(a, json);
}

void app_send_message(app_t *a)
{
    if (a->message[0] == '\0') {
        return;
    }

    char json[APP_MESSAGE_MAX * 2 + 32];
    snprintf(json, sizeof(json), "{\"set\":{\"message\":\"");
    if (!jsoncmd_escape_append(json, sizeof(json), a->message) ||
        !jsoncmd_append(json, sizeof(json), "\"}}")) {
        app_log(a, APP_LOG_ERROR, "message is too long");
        return;
    }
    app_send_json(a, json);
}

void app_set_config(app_t *a)
{
    if (a->config[0] == '\0') {
        app_log(a, APP_LOG_ERROR, "nothing to send");
        return;
    }

    char json[APP_CONFIG_MAX + 32];
    int n = snprintf(json, sizeof(json), "{\"set\":{\"config\":%s}}", a->config);
    if (n < 0 || n >= (int)sizeof(json)) {
        app_log(a, APP_LOG_ERROR, "config is too long");
        return;
    }
    app_send_json(a, json);
}
