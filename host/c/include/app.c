#include "app.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "mixer.h"
#include "proto.h"
#include "usbdev.h"

#define USB_VID     0x303A

/*
 * Both variants speak the same protocol. The bus is searched for both at once
 * and the first entry here that is attached wins, so the controller takes
 * precedence and the dongle is the fallback.
 */
static const struct {
    uint16_t    pid;
    const char *name;
} USB_VARIANTS[] = {
    { 0x4001, "IOMeeter Controller" },
    { 0x4002, "IOMeeter Dongle" },
};

#define USB_VARIANT_COUNT ((int)(sizeof(USB_VARIANTS) / sizeof(USB_VARIANTS[0])))

#define SEND_TIMEOUT_MS  500

/* Near-zero so a frame is never held up waiting on the device. */
#define POLL_TIMEOUT_MS  1

/*
 * A moving fader sends faster than the frame rate. Draining only a few per
 * frame lets a backlog build, which shows up as the interface lagging behind
 * the physical control; the per-packet work is small enough to take many.
 */
#define POLL_MAX_PACKETS 32

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

    snprintf(a->config_path, sizeof(a->config_path), "%s", path);
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

    snprintf(a->config_path, sizeof(a->config_path), "%s", path);
    app_log(a, APP_LOG_EVENT, "loaded %s", path);
    a->config_dirty = true;     /* mirror it into the working config.json */
    return true;
}

bool app_config_reload(app_t *a)
{
    char path[CONFIG_PATH_MAX];

    /* app_config_load_from writes config_path, so it cannot be read from
       the struct while that call is in progress. */
    snprintf(path, sizeof(path), "%s",
             a->config_path[0] ? a->config_path : APP_CONFIG_PATH);

    bool ok = app_config_load_from(a, path);
    if (ok) {
        for (int i = 0; i < APP_FADER_COUNT; i++) {
            app_apply_volume(a, i);
        }
    }
    return ok;
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

    snprintf(a->config_path, sizeof(a->config_path), "%s", APP_CONFIG_PATH);
    app_config_load(a);

    if (mixer_init()) {
        app_log(a, APP_LOG_EVENT, "mixer ready");
    } else {
        app_log(a, APP_LOG_ERROR, "volume control disabled: %s",
                mixer_last_error());
    }

    app_log(a, APP_LOG_EVENT, "not connected");
}

void app_shutdown(app_t *a)
{
    app_config_save(a);
    mixer_shutdown();

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

    uint16_t pids[USB_VARIANT_COUNT];
    for (int i = 0; i < USB_VARIANT_COUNT; i++) {
        pids[i] = USB_VARIANTS[i].pid;
    }

    if (usbdev_open_any(a->dev, USB_VID, pids, USB_VARIANT_COUNT) != 0) {
        /* Retried on a timer, so this must not fill the log with one line per
           attempt while nothing is plugged in. */
        if (!a->connect_failure_logged) {
            a->connect_failure_logged = true;
            app_log(a, APP_LOG_ERROR, "no IOMeeter device found");
        }
        return false;
    }

    a->connect_failure_logged = false;

    snprintf(a->device_name, sizeof(a->device_name), "%s", "IOMeeter");
    for (int i = 0; i < USB_VARIANT_COUNT; i++) {
        if (USB_VARIANTS[i].pid == a->dev->pid) {
            snprintf(a->device_name, sizeof(a->device_name), "%s",
                     USB_VARIANTS[i].name);
            break;
        }
    }

    a->connected = true;

    /* Anything half-received from a previous session is meaningless now. */
    proto_framer_reset(&a->framer);

    app_log(a, APP_LOG_EVENT, "connected: %s (%04X:%04X)",
            a->device_name, USB_VID, a->dev->pid);
    app_log(a, APP_LOG_EVENT, "interface %d, OUT 0x%02X, IN 0x%02X",
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
    a->device_name[0] = '\0';
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
    proto_kind_t kind = proto_classify(data, len);

    if (kind == PROTO_EVENT) {
        app_log(a, APP_LOG_EVENT, "<- %.*s", len, (const char *)data);
        return;
    }

    /* Parsed once, up front. The packet is not NUL terminated, hence the
       length-taking entry point. */
    cJSON *root = cJSON_ParseWithLength((const char *)data, (size_t)len);
    if (root == NULL) {
        app_log(a, APP_LOG_ERROR, "unparsable packet: %.*s", len,
                (const char *)data);
        return;
    }

    if (kind == PROTO_INTERRUPT) {
        app_log(a, APP_LOG_EVENT, "<- %.*s", len, (const char *)data);

        const cJSON *report = cJSON_GetObjectItemCaseSensitive(root, "interrupt");
        const cJSON *message = cJSON_GetObjectItemCaseSensitive(report, "message");

        if (cJSON_IsString(message) && message->valuestring &&
            message->valuestring[0]) {
            app_notify(a, message->valuestring);
        } else {
            app_notify(a, "interrupt");
        }

        cJSON_Delete(root);
        return;
    }

    /* {"get":{"slider":{"id":N}}} -- the device is asking for a slider's state.
       Checked first because a get and a set both carry a "slider" object. */
    const cJSON *get = cJSON_GetObjectItemCaseSensitive(root, "get");
    if (get != NULL) {
        const cJSON *slider = cJSON_GetObjectItemCaseSensitive(get, "slider");
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(slider, "id");

        if (cJSON_IsNumber(id)) {
            app_reply_slider(a, id->valueint);
        } else {
            app_log(a, APP_LOG_ERROR, "unsupported get: %.*s", len,
                    (const char *)data);
        }

        cJSON_Delete(root);
        return;
    }

    app_log(a, APP_LOG_RX, "<- %.*s", len, (const char *)data);

    /* Commands arrive wrapped in "set"; answers to a get come back bare, so
       both shapes are accepted. */
    const cJSON *body = cJSON_GetObjectItemCaseSensitive(root, "set");
    if (body == NULL) {
        body = root;
    }

    /* {"set":{"slider":{"id":N,"value":V}}} -- the device moved one of its own
       sliders, so mirror it and let the UI lock the pointer out briefly. */
    const cJSON *slider = cJSON_GetObjectItemCaseSensitive(body, "slider");
    if (slider != NULL) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(slider, "id");
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(slider, "value");

        if (cJSON_IsNumber(id) && cJSON_IsNumber(value) &&
            id->valueint >= 0 && id->valueint < APP_FADER_COUNT) {
            a->sliders[id->valueint].value = value->valueint;
            a->slider_extern_seq++;
            a->slider_extern_at[id->valueint]++;
            /* Coalesced: a physical fader sends far faster than the frame rate,
               and each push reaches the audio session graph. */
            a->slider_volume_dirty[id->valueint] = true;
        }

        cJSON_Delete(root);
        return;
    }

    const cJSON *led = cJSON_GetObjectItemCaseSensitive(body, "led");
    if (cJSON_IsString(led) && led->valuestring) {
        unsigned rgb;
        if (sscanf(led->valuestring, "%6x", &rgb) == 1) {
            app_set_rgb(a, rgb);
        }

        cJSON_Delete(root);
        return;
    }

    const cJSON *config = cJSON_GetObjectItemCaseSensitive(body, "config");
    if (config != NULL) {
        char *text = cJSON_PrintUnformatted(config);
        if (text != NULL) {
            snprintf(a->config, sizeof(a->config), "%s", text);
            cJSON_free(text);
        }
    }

    cJSON_Delete(root);
}

/* One volume push per slider per poll, however many packets arrived. */
static void apply_dirty_volumes(app_t *a)
{
    for (int i = 0; i < APP_FADER_COUNT; i++) {
        if (a->slider_volume_dirty[i]) {
            a->slider_volume_dirty[i] = false;
            app_apply_volume(a, i);
        }
    }
}

/* Trampoline: the framer hands back one message at a time. */
static void app_on_message(void *user, const unsigned char *msg, int len)
{
    app_handle_packet((app_t *)user, msg, len);
}

void app_poll(app_t *a)
{
    if (!a->connected) {
        return;
    }

    /* An early return below must not strand a pending change. */
    apply_dirty_volumes(a);

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
            /* One read is not one message: WinUSB in particular returns
               several concatenated, and can split a long one in half. */
            proto_framer_push(&a->framer, buf, len, app_on_message, a);
        }
    }

    apply_dirty_volumes(a);
}

void app_set_led(app_t *a)
{
    char json[64];
    snprintf(json, sizeof(json), "{\"set\":{\"led\":\"%06X\"}}",
             (unsigned)app_rgb(a));
    app_send_json(a, json);
}

void app_send_slider(app_t *a, int id, int value)
{
    char json[96];
    snprintf(json, sizeof(json),
             "{\"set\":{\"slider\":{\"id\":%d,\"value\":%d}}}", id, value);
    app_send_json(a, json);
}

/*
 * Faders are linear but loudness is not: a straight 0..1 mapping puts almost
 * all of the audible change in the bottom of the travel. Cubing approximates
 * the taper the system mixer applies, so the fader feels even.
 */
static float slider_to_gain(int value, int max)
{
    float t = (max > 0) ? (float)value / (float)max : 0.0f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t * t * t;
}

void app_apply_volume(app_t *a, int id)
{
    if (id < 0 || id >= APP_FADER_COUNT || !mixer_available()) {
        return;
    }

    float gain = slider_to_gain(a->sliders[id].value, APP_FADER_MAX);
    int matched = 0;

    for (int i = 0; i < a->sliders[id].app_count; i++) {
        if (mixer_set_volume(a->sliders[id].apps[i].name, gain)) {
            matched++;
        }
    }

    /*
     * An application that is not playing anything has no session to set, which
     * is normal. A fader whose whole list matches nothing is the usual reason
     * it appears to do nothing at all, so that is worth saying once.
     */
    bool unmatched = (a->sliders[id].app_count > 0 && matched == 0);

    if (unmatched && !a->slider_unmatched[id]) {
        app_log(a, APP_LOG_ERROR,
                "%s: no audio session matches its applications",
                a->sliders[id].name);
    }
    a->slider_unmatched[id] = unmatched;
}

/* Serialise @p root, send it, and dispose of it either way. */
static void app_send_cjson(app_t *a, cJSON *root, const char *what)
{
    char *text = (root != NULL) ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);

    if (text == NULL) {
        app_log(a, APP_LOG_ERROR, "could not build the %s command", what);
        return;
    }

    app_send_json(a, text);
    cJSON_free(text);
}

void app_reply_slider(app_t *a, int id)
{
    if (id < 0 || id >= APP_FADER_COUNT) {
        app_log(a, APP_LOG_ERROR, "slider %d out of range", id);
        return;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *set = (root != NULL) ? cJSON_AddObjectToObject(root, "set") : NULL;
    cJSON *slider = (set != NULL) ? cJSON_AddObjectToObject(set, "slider") : NULL;

    if (slider == NULL ||
        cJSON_AddNumberToObject(slider, "id", id) == NULL ||
        cJSON_AddNumberToObject(slider, "value", a->sliders[id].value) == NULL ||
        cJSON_AddStringToObject(slider, "name", a->sliders[id].name) == NULL ||
        cJSON_AddBoolToObject(slider, "update", a->slider_pending[id]) == NULL) {
        cJSON_Delete(root);
        root = NULL;
    }

    app_send_cjson(a, root, "slider");
    a->slider_pending[id] = false;
}

void app_get(app_t *a, const char *what)
{
    cJSON *root = cJSON_CreateObject();

    if (root != NULL && cJSON_AddStringToObject(root, "get", what) == NULL) {
        cJSON_Delete(root);
        root = NULL;
    }
    app_send_cjson(a, root, "get");
}

void app_send_message(app_t *a)
{
    if (a->message[0] == '\0') {
        return;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *set = (root != NULL) ? cJSON_AddObjectToObject(root, "set") : NULL;

    if (set == NULL || cJSON_AddStringToObject(set, "message", a->message) == NULL) {
        cJSON_Delete(root);
        root = NULL;
    }
    app_send_cjson(a, root, "message");
}

void app_set_config(app_t *a)
{
    if (a->config[0] == '\0') {
        app_log(a, APP_LOG_ERROR, "nothing to send");
        return;
    }

    /* a->config holds the document the device sent back, so it is re-parsed
       rather than spliced in as text. */
    cJSON *config = cJSON_Parse(a->config);
    if (config == NULL) {
        app_log(a, APP_LOG_ERROR, "the stored config is not valid JSON");
        return;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *set = (root != NULL) ? cJSON_AddObjectToObject(root, "set") : NULL;

    if (set == NULL || !cJSON_AddItemToObject(set, "config", config)) {
        cJSON_Delete(config);
        cJSON_Delete(root);
        root = NULL;
    }
    app_send_cjson(a, root, "config");
}

