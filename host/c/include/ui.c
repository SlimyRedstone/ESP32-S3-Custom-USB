#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fader.h"
#include "filedialog.h"
#include "instance.h"
#include "mixer.h"
#include "respath.h"
#include "tray.h"

#include "clay.h"
#include "clay_raylib_renderer.h"
#include "raylib.h"

/* --------------------------------------------------------------- theme --- */

#define COL(r, g, b, a) ((Clay_Color){ (float)(r), (float)(g), (float)(b), (float)(a) })

static const Clay_Color C_BG        = COL(0x0f, 0x0f, 0x0f, 255);
static const Clay_Color C_CARD      = COL(0x20, 0x20, 0x20, 255);
static const Clay_Color C_FIELD     = COL(0x0f, 0x0f, 0x0f, 255);
static const Clay_Color C_LINE      = COL(0x4f, 0x4f, 0x4f, 255);

/* Container outlines used to be drawn in C_LINE, which read as a grid of
   thin lines over the whole window. Kept as a named colour rather than
   deleting every border, so one value brings them back. */
static const Clay_Color C_BORDER    = COL(0x00, 0x00, 0x00, 0);
static const Clay_Color C_SELECT    = COL(0x2d, 0x4f, 0x8f, 255);
static const Clay_Color C_FG        = COL(0xe7, 0xe9, 0xee, 255);
static const Clay_Color C_MUTED     = COL(0x9a, 0xa1, 0xb1, 255);
static const Clay_Color C_ACCENT    = COL(0x5b, 0x8c, 0xff, 255);
static const Clay_Color C_ACCENT_HI = COL(0x7a, 0xa4, 0xff, 255);
static const Clay_Color C_OK        = COL(0x4e, 0xcb, 0x84, 255);
static const Clay_Color C_WARN      = COL(0xe8, 0xa3, 0x4a, 255);
static const Clay_Color C_RX        = COL(0xc7, 0x9b, 0xff, 255);
static const Clay_Color C_TRANSPARENT = COL(0, 0, 0, 0);

enum { FONT_BODY = 0, FONT_MONO = 1, FONT_COUNT = 2 };

static bool s_menu_open;

/*
 * An app removal requested this frame. Deferred rather than applied on the
 * spot: the click is detected while the list is being laid out, and deleting
 * there would shift the array under the loop still walking it.
 */
static int s_remove_slider = -1;
static int s_remove_index = -1;

static int  s_rename = -1;
static char s_rename_buf[CONFIG_NAME_MAX];

/*
 * Invoked by the fader library whenever a value changes. Nothing is sent to the
 * device yet, so this only records the movement; wiring a command in means
 * replacing the body.
 */
static void ui_on_fader_change(int id, int value, void *user)
{
    app_t *app = (app_t *)user;
    if (id < 0 || id >= APP_FADER_COUNT) {
        return;
    }

    /* Deliberately not marked dirty: a moving fader must not rewrite the file.
       Names and app lists still do, and the current values ride along with the
       next save those trigger. */
    app->slider_pending[id] = true;
    app_send_slider(app, id, value);
    app_apply_volume(app, id);
}

/* A fader column is wider than its track so filenames have room underneath. */
#define FADER_COLUMN_W  84
#define APP_ROW_H       16
#define APP_LIST_ROWS   5
#define APP_LIST_H      (APP_ROW_H * APP_LIST_ROWS)

#define UI_PI        3.14159265358979323846f
#define WHEEL_PIXELS 200        /* texture resolution of the colour wheel */
#define LOG_ROW_H    16

static Font s_fonts[FONT_COUNT];

/* Refresh rate the loop is running at, for the debug counter to judge by. */
static int s_target_fps = UI_FALLBACK_FPS;

/*
 * Height the faders get. The design heights are quoted for a full-size track,
 * so whatever they allot beyond it is the rest of the interface; the strip
 * takes what is left of the real window and is clamped to a usable range.
 */
static int ui_fader_height(bool debug)
{
    int chrome = UI_WINDOW_HEIGHT_FOR(debug) - FADER_TRACK_HEIGHT;
    int available = GetScreenHeight() - chrome;

    if (available > FADER_TRACK_HEIGHT) {
        available = FADER_TRACK_HEIGHT;
    }
    if (available < FADER_MIN_HEIGHT) {
        available = FADER_MIN_HEIGHT;
    }
    return available;
}

static Texture2D s_wheel;
static float     s_wheel_val = -1.0f;

static char *s_focus;
static size_t s_focus_cap;

/*
 * Clay stores a pointer to text, not a copy, and only reads it when the frame
 * is rendered. Each field therefore needs storage that stays untouched for the
 * rest of the frame; one shared buffer made every field show the same string.
 */
#define TEXT_SLOTS 4
static char s_text_slots[TEXT_SLOTS][APP_CONFIG_MAX + 2];
static int  s_text_slot;

/* --------------------------------------------------------------- helpers -- */

/* Clay_String over a runtime buffer. CLAY_STRING only accepts literals. */
static Clay_String dyn(const char *text)
{
    Clay_String s;
    s.isStaticallyAllocated = false;
    s.length = (int32_t)strlen(text);
    s.chars = text;
    return s;
}

static void HandleClayErrors(Clay_ErrorData errorData)
{
    fprintf(stderr, "clay: %.*s\n", (int)errorData.errorText.length,
            errorData.errorText.chars);
}

static Clay_Color mix(Clay_Color base, Clay_Color hi, bool on)
{
    return on ? hi : base;
}

/* ------------------------------------------------------------- widgets --- */

/*
 * Clay reports hover for the element being declared, so a click is "hovered
 * while the mouse was released this frame".
 */
static bool clicked(bool enabled)
{
    return enabled && Clay_Hovered() && IsMouseButtonReleased(MOUSE_BUTTON_LEFT);
}

static bool ui_button(Clay_ElementId id, const char *label, bool primary,
                      bool enabled, bool grow)
{
    bool hit = false;

    Clay_Color fill = primary ? C_ACCENT : C_CARD;
    Clay_Color text = primary ? C_FG : C_FG;
    if (!enabled) {
        fill = C_LINE;
        text = C_MUTED;
    }

    CLAY(id, {
        .layout = {
            .sizing = { .width = grow ? CLAY_SIZING_GROW(0) : CLAY_SIZING_FIT(0),
                        .height = CLAY_SIZING_FIXED(30) },
            .padding = { 12, 12, 7, 7 },
            .childAlignment = { CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER },
        },
        .backgroundColor = mix(fill, primary ? C_ACCENT_HI : C_LINE,
                               enabled && Clay_Hovered()),
        .cornerRadius = CLAY_CORNER_RADIUS(8),
        .border = { .color = C_BORDER, .width = { 1, 1, 1, 1 } },
    }) {
        hit = clicked(enabled);
        CLAY_TEXT(dyn(label), CLAY_TEXT_CONFIG({
            .fontId = FONT_BODY, .fontSize = 15, .textColor = text }));
    }
    return hit;
}

static bool ui_checkbox(Clay_ElementId id, const char *label, bool *value)
{
    bool hit = false;

    CLAY(id, {
        .layout = {
            .sizing = { .height = CLAY_SIZING_FIXED(22) },
            .childGap = 8,
            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
        },
    }) {
        hit = clicked(true);

        CLAY_AUTO_ID({
            .layout = { .sizing = { CLAY_SIZING_FIXED(14), CLAY_SIZING_FIXED(14) } },
            .backgroundColor = *value ? C_ACCENT : C_FIELD,
            .cornerRadius = CLAY_CORNER_RADIUS(4),
            .border = { .color = C_BORDER, .width = { 1, 1, 1, 1 } },
        }) {}

        CLAY_TEXT(dyn(label), CLAY_TEXT_CONFIG({
            .fontId = FONT_BODY, .fontSize = 13, .textColor = C_MUTED }));
    }

    if (hit) {
        *value = !*value;
    }
    return hit;
}

/*
 * Text field. Clay only lays it out; the caret and editing are handled here
 * against the focused buffer.
 */
static void ui_text_field(Clay_ElementId id, char *buffer, size_t cap,
                          int font, bool *submitted)
{
    bool focused = (s_focus == buffer);

    CLAY(id, {
        .layout = {
            .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(30) },
            .padding = { 10, 10, 6, 6 },
            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
        },
        .backgroundColor = C_FIELD,
        .cornerRadius = CLAY_CORNER_RADIUS(8),
        .border = { .color = focused ? C_ACCENT : C_BORDER, .width = { 1, 1, 1, 1 } },
    }) {
        if (Clay_Hovered() && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            s_focus = buffer;
            s_focus_cap = cap;
        }

        char *shown = s_text_slots[s_text_slot % TEXT_SLOTS];
        s_text_slot++;
        snprintf(shown, APP_CONFIG_MAX + 2, "%s%s", buffer,
                 (focused && fmodf((float)GetTime(), 1.0f) < 0.5f) ? "_" : "");

        CLAY_TEXT(dyn(shown), CLAY_TEXT_CONFIG({
            .fontId = font, .fontSize = 14,
            .textColor = buffer[0] ? C_FG : C_MUTED }));
    }

    if (submitted) {
        *submitted = focused && IsKeyPressed(KEY_ENTER);
    }
}

static void ui_pump_text_input(void)
{
    if (s_focus == NULL) {
        return;
    }

    size_t len = strlen(s_focus);

    for (int c = GetCharPressed(); c > 0; c = GetCharPressed()) {
        if (c >= 32 && c < 127 && len + 1 < s_focus_cap) {
            s_focus[len++] = (char)c;
            s_focus[len] = '\0';
        }
    }

    /* Repeat on hold, so deleting a long value is not one press per character. */
    if ((IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) && len > 0) {
        s_focus[len - 1] = '\0';
    }
    if (IsKeyPressed(KEY_ESCAPE)) {
        s_focus = NULL;
    }
}

/* --------------------------------------------------------- colour wheel -- */

/* HSV disc: hue around the circumference, saturation along the radius. */
static void ui_rebuild_wheel(const app_t *app)
{
    if (fabsf(app->val - s_wheel_val) < 0.004f && s_wheel.id != 0) {
        return;
    }
    s_wheel_val = app->val;

    Image img = GenImageColor(WHEEL_PIXELS, WHEEL_PIXELS, BLANK);
    Color *px = (Color *)img.data;

    const float centre = WHEEL_PIXELS / 2.0f;
    const float radius = centre - 1.0f;

    for (int y = 0; y < WHEEL_PIXELS; y++) {
        for (int x = 0; x < WHEEL_PIXELS; x++) {
            float dx = (float)x - centre;
            float dy = (float)y - centre;
            float dist = sqrtf(dx * dx + dy * dy);
            Color *p = &px[y * WHEEL_PIXELS + x];

            if (dist > radius) {
                *p = BLANK;
                continue;
            }

            float angle = atan2f(dy, dx) * 180.0f / UI_PI + 90.0f;
            if (angle < 0.0f) {
                angle += 360.0f;
            }

            uint8_t r, g, b;
            app_hsv_to_rgb(angle, fminf(dist / radius, 1.0f), app->val, &r, &g, &b);

            p->r = r;
            p->g = g;
            p->b = b;
            /* Feather the rim so the edge is not jagged. */
            p->a = (dist > radius - 1.0f)
                 ? (unsigned char)((radius - dist) * 255.0f)
                 : 255;
        }
    }

    if (s_wheel.id != 0) {
        UnloadTexture(s_wheel);
    }
    s_wheel = LoadTextureFromImage(img);
    UnloadImage(img);
}

/*
 * Hit-test and marker are done outside Clay: the layout supplies the box, the
 * pointer maths happens here.
 */
/*
 * The wheel is drawn as a disc inside a square element, so the hit test has to
 * be the circle rather than the box. A press is only accepted within the disc,
 * and that press then owns the drag: without the ownership flag a press in a
 * corner would be rejected on its first frame but accepted on the next, because
 * IsMouseButtonPressed is only true once.
 */
static bool s_wheel_active;

static void ui_wheel_interact(app_t *app, Clay_BoundingBox box, bool *changed)
{
    if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        s_wheel_active = false;
        return;
    }

    Vector2 m = GetMousePosition();
    float cx = box.x + box.width / 2.0f;
    float cy = box.y + box.height / 2.0f;
    float radius = fminf(box.width, box.height) / 2.0f;

    float dx = m.x - cx;
    float dy = m.y - cy;
    float dist = sqrtf(dx * dx + dy * dy);

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && dist <= radius) {
        s_wheel_active = true;
    }
    if (!s_wheel_active) {
        return;
    }

    float angle = atan2f(dy, dx) * 180.0f / UI_PI + 90.0f;
    if (angle < 0.0f) {
        angle += 360.0f;
    }

    app->hue = angle;
    /* Saturation is clamped at the rim, so a drag may leave the disc and keep
       tracking the hue, which is how a colour wheel is expected to behave. */
    app->sat = fminf(dist / radius, 1.0f);
    app_sync_hex(app);
    *changed = true;
}

static void ui_draw_wheel_marker(const app_t *app, Clay_BoundingBox box)
{
    float cx = box.x + box.width / 2.0f;
    float cy = box.y + box.height / 2.0f;
    float radius = fminf(box.width, box.height) / 2.0f;
    float angle = (app->hue - 90.0f) * UI_PI / 180.0f;

    Vector2 at = { cx + app->sat * radius * cosf(angle),
                   cy + app->sat * radius * sinf(angle) };

    DrawCircleV(at, 7.0f, WHITE);
    DrawCircleV(at, 5.0f, (Color){ 0, 0, 0, 160 });

    uint8_t r, g, b;
    app_hsv_to_rgb(app->hue, app->sat, app->val, &r, &g, &b);
    DrawCircleV(at, 4.0f, (Color){ r, g, b, 255 });
}

/* -------------------------------------------------------------- slider --- */

/*
 * Interaction only. The element is declared inside the layout; this reads the
 * box Clay recorded last frame, so it must run before Clay_BeginLayout().
 */
static bool ui_slider(Clay_ElementId id, float *value)
{
    Clay_ElementData data = Clay_GetElementData(id);
    if (!data.found || !IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        return false;
    }

    Vector2 m = GetMousePosition();
    Clay_BoundingBox b = data.boundingBox;

    bool inside = m.x >= b.x - 6.0f && m.x <= b.x + b.width + 6.0f &&
                  m.y >= b.y - 8.0f && m.y <= b.y + b.height + 8.0f;
    if (!inside) {
        return false;
    }

    float t = (m.x - b.x) / (b.width > 1.0f ? b.width : 1.0f);
    t = fmaxf(0.0f, fminf(1.0f, t));

    if (fabsf(t - *value) <= 0.001f) {
        return false;
    }
    *value = t;
    return true;
}

static void ui_draw_slider_knob(Clay_ElementId id, float value)
{
    Clay_ElementData data = Clay_GetElementData(id);
    if (!data.found) {
        return;
    }

    Clay_BoundingBox b = data.boundingBox;
    float x = b.x + value * b.width;
    float y = b.y + b.height / 2.0f;

    DrawRectangleRounded((Rectangle){ b.x, y - 2.5f, value * b.width, 5.0f },
                         1.0f, 4, (Color){ 0x5b, 0x8c, 0xff, 255 });
    DrawCircle((int)x, (int)y, 7.0f, WHITE);
}

/* ---------------------------------------------------------------- cards -- */

static void ui_card_title(const char *title)
{
    CLAY_AUTO_ID({ .layout = { .padding = { 0, 0, 0, 8 } } }) {
        CLAY_TEXT(dyn(title), CLAY_TEXT_CONFIG({
            .fontId = FONT_BODY, .fontSize = 13, .textColor = C_MUTED }));
    }
}

#define UI_CARD(id) CLAY(id, {                                              \
        .layout = {                                                          \
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },           \
            .padding = CLAY_PADDING_ALL(14),                                 \
            .childGap = 8,                                                   \
            .layoutDirection = CLAY_TOP_TO_BOTTOM,                           \
        },                                                                   \
        .backgroundColor = C_CARD,                                           \
        .cornerRadius = CLAY_CORNER_RADIUS(12),                              \
        .border = { .color = C_BORDER, .width = { 1, 1, 1, 1 } },              \
    })

static Clay_Color log_colour(app_log_kind_t kind)
{
    switch (kind) {
    case APP_LOG_TX:    return C_ACCENT;
    case APP_LOG_RX:    return C_RX;
    case APP_LOG_EVENT: return C_OK;
    case APP_LOG_ERROR:
    default:            return C_WARN;
    }
}

/* ---------------------------------------------------------------- menu --- */

#define MENU_WIDTH    190.0f
#define MENU_ITEM_H   32.0f
#define MENU_PAD      6.0f
#define MENU_ITEMS    3

static const char *const s_menu_items[MENU_ITEMS] = {
    "Save config", "Load config", "Reload config",
};

static Rectangle ui_menu_rect(Clay_BoundingBox button)
{
    return (Rectangle){
        button.x,
        button.y + button.height + 6.0f,
        MENU_WIDTH,
        MENU_ITEMS * MENU_ITEM_H + 2.0f * MENU_PAD,
    };
}

static int ui_menu_hit(Rectangle panel)
{
    Vector2 mouse = GetMousePosition();
    if (!CheckCollisionPointRec(mouse, panel)) {
        return -1;
    }

    float local = mouse.y - (panel.y + MENU_PAD);
    if (local < 0.0f) {
        return -1;
    }

    int index = (int)(local / MENU_ITEM_H);
    return (index >= 0 && index < MENU_ITEMS) ? index : -1;
}

/*
 * Drawn with raylib after everything else. Clay renders before the colour
 * wheel marker, the slider knob and the faders, so a Clay-drawn panel ended up
 * underneath them and their pointer handling ignored it.
 */
static void ui_draw_menu(Rectangle panel)
{
    int hovered = ui_menu_hit(panel);

    DrawRectangleRounded(panel, 0.12f, 8, (Color){ 0x2b, 0x30, 0x39, 255 });

    Rectangle inner = { panel.x + 1.0f, panel.y + 1.0f,
                        panel.width - 2.0f, panel.height - 2.0f };
    DrawRectangleRounded(inner, 0.12f, 8, (Color){ 0x1c, 0x1f, 0x25, 255 });

    for (int i = 0; i < MENU_ITEMS; i++) {
        Rectangle row = {
            panel.x + MENU_PAD,
            panel.y + MENU_PAD + (float)i * MENU_ITEM_H,
            panel.width - 2.0f * MENU_PAD,
            MENU_ITEM_H,
        };

        if (i == hovered) {
            DrawRectangleRounded(row, 0.25f, 8, (Color){ 0x5b, 0x8c, 0xff, 255 });
        }

        Font font = s_fonts[FONT_BODY];
        float size = 15.0f;
        Vector2 extent = MeasureTextEx(font, s_menu_items[i], size, 0.0f);

        DrawTextEx(font, s_menu_items[i],
                   (Vector2){ row.x + 10.0f,
                              row.y + (row.height - extent.y) / 2.0f },
                   size, 0.0f, (Color){ 0xe7, 0xe9, 0xee, 255 });
    }
}

static void ui_add_app(app_t *app, int slider)
{
    char path[CONFIG_PATH_MAX];

    if (!filedialog_open_program("Choose an application", path, sizeof(path))) {
        if (!filedialog_available()) {
            app_log(app, APP_LOG_ERROR,
                    "no file chooser available (install zenity or kdialog)");
        }
        return;
    }

    if (!config_add_app(&app->sliders[slider], path)) {
        app_log(app, APP_LOG_ERROR, "already listed, or the slider is full");
        return;
    }

    app->config_dirty = true;

    int last = app->sliders[slider].app_count - 1;
    app_log(app, APP_LOG_EVENT, "%s controls %s",
            app->sliders[slider].name, app->sliders[slider].apps[last].name);
    app_apply_volume(app, slider);
}

static void ui_menu_save_config(app_t *app)
{
    char path[512];

    if (!filedialog_save("Save configuration", "config.json", path, sizeof(path))) {
        if (!filedialog_available()) {
            app_log(app, APP_LOG_ERROR,
                    "no file chooser available (install zenity or kdialog)");
        }
        return;
    }
    app_config_save_as(app, path);
}

/* Re-reads whichever file the configuration last came from, so an edit made
   outside the program can be picked up without restarting it. */
static void ui_menu_reload_config(app_t *app)
{
    app_config_reload(app);
}

static void ui_menu_load_config(app_t *app)
{
    char path[512];

    if (!filedialog_open("Load configuration", path, sizeof(path))) {
        if (!filedialog_available()) {
            app_log(app, APP_LOG_ERROR,
                    "no file chooser available (install zenity or kdialog)");
        }
        return;
    }
    app_config_load_from(app, path);
}

/*
 * Rename editor. Typing is fed by ui_pump_text_input() through s_focus; this
 * only deals with finishing or abandoning the edit.
 */
static void ui_update_rename(app_t *app)
{
    if (s_rename < 0) {
        return;
    }

    if (IsKeyPressed(KEY_ENTER)) {
        if (s_rename_buf[0] != (char)0) {
            snprintf(app->sliders[s_rename].name, CONFIG_NAME_MAX, "%s",
                     s_rename_buf);
            app->config_dirty = true;
            app_log(app, APP_LOG_EVENT, "renamed fader %d to %s",
                    s_rename, s_rename_buf);
        }
        s_rename = -1;
        s_focus = NULL;
        return;
    }

    /* Escape abandons the edit. ui_pump_text_input() also drops focus on
       escape, which is harmless here. */
    if (IsKeyPressed(KEY_ESCAPE)) {
        s_rename = -1;
        s_focus = NULL;
    }
}

static void ui_draw_rename(const app_t *app, Clay_BoundingBox box)
{
    (void)app;

    Font font = s_fonts[FONT_BODY];
    float size = 15.0f;

    char shown[CONFIG_NAME_MAX + 2];
    snprintf(shown, sizeof(shown), "%s%s", s_rename_buf,
             (fmodf((float)GetTime(), 1.0f) < 0.5f) ? "_" : "");

    Vector2 extent = MeasureTextEx(font, shown, size, 0.0f);
    float width = fmaxf(extent.x + 20.0f, 150.0f);
    float height = 34.0f;

    float x = box.x + box.width / 2.0f - width / 2.0f;
    float y = box.y - height - 8.0f;

    if (x < 6.0f) {
        x = 6.0f;
    }
    if (x + width > (float)GetScreenWidth() - 6.0f) {
        x = (float)GetScreenWidth() - width - 6.0f;
    }
    if (y < 6.0f) {
        y = box.y + 6.0f;
    }

    Rectangle rect = { x, y, width, height };
    DrawRectangleRounded(rect, 0.25f, 8, (Color){ 0x5b, 0x8c, 0xff, 255 });

    Rectangle inner = { x + 1.0f, y + 1.0f, width - 2.0f, height - 2.0f };
    DrawRectangleRounded(inner, 0.25f, 8, (Color){ 0x14, 0x16, 0x1a, 255 });

    DrawTextEx(font, shown,
               (Vector2){ x + 10.0f, y + (height - extent.y) / 2.0f },
               size, 0.0f, (Color){ 0xe7, 0xe9, 0xee, 255 });
}

/* --------------------------------------------------------- notification -- */

#define TOAST_SECONDS   4.0
#define TOAST_FADE      0.8

static unsigned long s_toast_seen;
static double        s_toast_until;

/*
 * Drawn with raylib rather than Clay: it floats above the finished layout and
 * fades, neither of which the layout pass is involved in.
 */
static void ui_draw_toast(const app_t *app)
{
    if (GetTime() >= s_toast_until || app->notice[0] == '\0') {
        return;
    }

    double remaining = s_toast_until - GetTime();
    float alpha = (remaining < TOAST_FADE) ? (float)(remaining / TOAST_FADE) : 1.0f;

    Font font = s_fonts[FONT_BODY];
    const char *title = "Interrupt";
    Vector2 title_size = MeasureTextEx(font, title, 14.0f, 0.0f);
    Vector2 text_size = MeasureTextEx(font, app->notice, 17.0f, 0.0f);

    float width = fmaxf(title_size.x, text_size.x) + 28.0f;
    float height = 58.0f;
    float x = (float)GetScreenWidth() - width - 18.0f;
    float y = (float)GetScreenHeight() - height - 18.0f;

    Rectangle box = { x, y, width, height };

    /*
     * The border is an outer rounded rect with the fill inset by a pixel.
     * DrawRectangleRoundedLines is avoided on purpose: raylib 5.5 dropped its
     * lineThick parameter and moved it to DrawRectangleRoundedLinesEx, so the
     * call does not compile against both 5.0 and 5.5.
     */
    DrawRectangleRounded(box, 0.18f, 8,
                         Fade((Color){ 0x5b, 0x8c, 0xff, 255 }, alpha));

    Rectangle inner = { box.x + 1.0f, box.y + 1.0f,
                        box.width - 2.0f, box.height - 2.0f };
    DrawRectangleRounded(inner, 0.18f, 8,
                         Fade((Color){ 0x1c, 0x1f, 0x25, 255 }, alpha));

    DrawTextEx(font, title, (Vector2){ x + 14.0f, y + 9.0f }, 14.0f, 0.0f,
               Fade((Color){ 0x9a, 0xa1, 0xb1, 255 }, alpha));
    DrawTextEx(font, app->notice, (Vector2){ x + 14.0f, y + 29.0f }, 17.0f, 0.0f,
               Fade((Color){ 0xe7, 0xe9, 0xee, 255 }, alpha));
}

/*
 * Keep the newest line in view, but stop doing so once the user scrolls up, so
 * reading back through the traffic is not fought by every arriving packet.
 * Scrolling back to the bottom re-arms it.
 */
static void ui_autoscroll_log(const app_t *app, Clay_ElementId id)
{
    static unsigned long seen_seq = 0;
    static bool stick = true;

    Clay_ScrollContainerData sc = Clay_GetScrollContainerData(id);
    if (!sc.found || sc.scrollPosition == NULL) {
        return;
    }

    float overflow = sc.contentDimensions.height - sc.scrollContainerDimensions.height;
    if (overflow < 0.0f) {
        overflow = 0.0f;
    }

    /* Clay scrolls with a negative offset, so the bottom sits at -overflow. */
    bool at_bottom = (-sc.scrollPosition->y) >= overflow - 4.0f;

    if (app->log_seq != seen_seq) {
        seen_seq = app->log_seq;
        if (stick) {
            sc.scrollPosition->y = -overflow;
            return;     /* just moved it; judge the position again next frame */
        }
    }

    stick = at_bottom;
}

/* ------------------------------------------------------- log selection --- */

#define LOG_LINE_PITCH  (LOG_ROW_H + 1.0f)   /* row height plus the childGap */
#define LOG_PAD         8.0f
#define LOG_BAR_W       8.0f

/* Inclusive range of selected lines; -1 when nothing is selected. */
static int   s_log_from = -1;
static int   s_log_to   = -1;
static bool  s_log_selecting;
static bool  s_bar_dragging;
static float s_bar_grab;        /* pointer offset inside the thumb, in pixels */

static bool log_line_selected(int index)
{
    if (s_log_from < 0 || s_log_to < 0) {
        return false;
    }

    int lo = (s_log_from < s_log_to) ? s_log_from : s_log_to;
    int hi = (s_log_from < s_log_to) ? s_log_to : s_log_from;
    return index >= lo && index <= hi;
}

/* Geometry of the scrollbar track, or a zero-width rectangle when the content
   fits and no bar is needed. */
static Rectangle log_bar_track(Clay_BoundingBox box, float overflow)
{
    if (overflow <= 0.0f) {
        return (Rectangle){ 0, 0, 0, 0 };
    }
    return (Rectangle){ box.x + box.width - LOG_BAR_W - 4.0f,
                        box.y + 4.0f,
                        LOG_BAR_W,
                        box.height - 8.0f };
}

static Rectangle log_bar_thumb(Rectangle track, float overflow,
                               float content_h, float view_h, float scroll_y)
{
    float ratio = (content_h > 0.0f) ? view_h / content_h : 1.0f;
    float h = track.height * ratio;

    if (h < 24.0f) {
        h = 24.0f;
    }
    if (h > track.height) {
        h = track.height;
    }

    float progress = (overflow > 0.0f) ? (-scroll_y) / overflow : 0.0f;
    progress = fminf(fmaxf(progress, 0.0f), 1.0f);

    return (Rectangle){ track.x, track.y + progress * (track.height - h),
                        track.width, h };
}

/* Copy the selected lines, oldest first, one per line. */
static void ui_log_copy(const app_t *app)
{
    if (s_log_from < 0 || app->log_count <= 0) {
        return;
    }

    int lo = (s_log_from < s_log_to) ? s_log_from : s_log_to;
    int hi = (s_log_from < s_log_to) ? s_log_to : s_log_from;

    if (lo < 0) {
        lo = 0;
    }
    if (hi >= app->log_count) {
        hi = app->log_count - 1;
    }

    size_t need = 1;
    for (int i = lo; i <= hi; i++) {
        need += strlen(app_log_at(app, i)->text) + 1;
    }

    char *text = malloc(need);
    if (text == NULL) {
        return;
    }
    text[0] = '\0';

    for (int i = lo; i <= hi; i++) {
        strcat(text, app_log_at(app, i)->text);
        if (i < hi) {
            strcat(text, "\n");
        }
    }

    SetClipboardText(text);
    free(text);
}

/*
 * Scrollbar dragging and line selection, both against the box Clay recorded
 * last frame. Runs before Clay_BeginLayout(), so the highlight drawn this
 * frame reflects the pointer as it is now.
 */
static void ui_log_interact(const app_t *app)
{
    if (!app->debug) {
        return;
    }

    Clay_ElementData panel = Clay_GetElementData(CLAY_ID("LogScroll"));
    Clay_ScrollContainerData sc = Clay_GetScrollContainerData(CLAY_ID("LogScroll"));

    if (!panel.found || !sc.found || sc.scrollPosition == NULL) {
        return;
    }

    Clay_BoundingBox box = panel.boundingBox;
    float content_h = sc.contentDimensions.height;
    float view_h = sc.scrollContainerDimensions.height;
    float overflow = content_h - view_h;

    if (overflow < 0.0f) {
        overflow = 0.0f;
    }

    Vector2 mouse = GetMousePosition();
    Rectangle track = log_bar_track(box, overflow);
    Rectangle thumb = log_bar_thumb(track, overflow, content_h, view_h,
                                    sc.scrollPosition->y);

    /* --- scrollbar ---------------------------------------------------- */
    if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        s_bar_dragging = false;
    }

    if (track.width > 0.0f && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
        CheckCollisionPointRec(mouse, track)) {
        if (CheckCollisionPointRec(mouse, thumb)) {
            s_bar_dragging = true;
            s_bar_grab = mouse.y - thumb.y;
        } else {
            /* Clicking the track jumps so the thumb centres on the pointer. */
            float span = track.height - thumb.height;
            float progress = (span > 0.0f)
                           ? (mouse.y - track.y - thumb.height / 2.0f) / span
                           : 0.0f;
            progress = fminf(fmaxf(progress, 0.0f), 1.0f);
            sc.scrollPosition->y = -progress * overflow;
        }
        return;         /* the press belongs to the bar, not to the text */
    }

    if (s_bar_dragging) {
        float span = track.height - thumb.height;
        float progress = (span > 0.0f)
                       ? (mouse.y - s_bar_grab - track.y) / span
                       : 0.0f;
        progress = fminf(fmaxf(progress, 0.0f), 1.0f);
        sc.scrollPosition->y = -progress * overflow;
        return;
    }

    /* --- selection ----------------------------------------------------- */
    bool inside = CheckCollisionPointRec(mouse, (Rectangle){
        box.x, box.y, box.width, box.height });

    /* Row tops are laid out from the padded top, shifted by the scroll. */
    float local = mouse.y - box.y - LOG_PAD - sc.scrollPosition->y;
    int line = (int)(local / LOG_LINE_PITCH);

    if (line < 0) {
        line = 0;
    }
    if (line >= app->log_count) {
        line = app->log_count - 1;
    }

    if (inside && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        if (app->log_count > 0) {
            s_log_selecting = true;
            s_log_from = line;
            s_log_to = line;
        } else {
            s_log_from = s_log_to = -1;
        }
    } else if (s_log_selecting && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        s_log_to = line;
    }

    if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        s_log_selecting = false;
    }

    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);

    if (ctrl && IsKeyPressed(KEY_A) && app->log_count > 0) {
        s_log_from = 0;
        s_log_to = app->log_count - 1;
    }
    if (ctrl && IsKeyPressed(KEY_C)) {
        ui_log_copy(app);
    }
}

/*
 * Frame rate, for judging whether the interface is keeping up with the panel.
 * Tinted against the refresh rate so a drop is visible without reading it.
 */
static void ui_draw_fps(const app_t *app)
{
    if (!app->debug) {
        return;
    }

    int fps = GetFPS();

    char text[32];
    snprintf(text, sizeof(text), "%d FPS", fps);

    const float size = 14.0f;
    Vector2 measured = MeasureTextEx(s_fonts[FONT_MONO], text, size, 0.0f);
    Rectangle box = { 6.0f, 6.0f, measured.x + 12.0f, measured.y + 8.0f };

    Color tint;
    if (fps >= (s_target_fps * 9) / 10) {
        tint = (Color){ 0x4e, 0xcb, 0x84, 255 };        /* holding the clock */
    } else if (fps >= s_target_fps / 2) {
        tint = (Color){ 0xe8, 0xa3, 0x4a, 255 };        /* slipping */
    } else {
        tint = (Color){ 0xe8, 0x6a, 0x5a, 255 };        /* struggling */
    }

    /* Its own backing, since it floats over whatever is underneath. */
    DrawRectangleRounded(box, 0.35f, 6, (Color){ 0x00, 0x00, 0x00, 170 });
    DrawTextEx(s_fonts[FONT_MONO], text,
               (Vector2){ box.x + 6.0f, box.y + 4.0f }, size, 0.0f, tint);
}

/* Drawn after Clay, so the bar sits over the text rather than under it. */
static void ui_log_draw_scrollbar(const app_t *app)
{
    if (!app->debug) {
        return;
    }

    Clay_ElementData panel = Clay_GetElementData(CLAY_ID("LogScroll"));
    Clay_ScrollContainerData sc = Clay_GetScrollContainerData(CLAY_ID("LogScroll"));

    if (!panel.found || !sc.found || sc.scrollPosition == NULL) {
        return;
    }

    float content_h = sc.contentDimensions.height;
    float view_h = sc.scrollContainerDimensions.height;
    float overflow = content_h - view_h;

    if (overflow <= 0.0f) {
        return;         /* everything fits, so no bar */
    }

    Rectangle track = log_bar_track(panel.boundingBox, overflow);
    Rectangle thumb = log_bar_thumb(track, overflow, content_h, view_h,
                                    sc.scrollPosition->y);

    Vector2 mouse = GetMousePosition();
    bool hot = s_bar_dragging || CheckCollisionPointRec(mouse, track);

    DrawRectangleRounded(track, 1.0f, 6, (Color){ 0x2a, 0x2a, 0x2a, 255 });
    DrawRectangleRounded(thumb, 1.0f, 6,
                         hot ? (Color){ 0x8a, 0x8a, 0x8a, 255 }
                             : (Color){ 0x6a, 0x6a, 0x6a, 255 });
}

/* ----------------------------------------------------------------- run --- */

int ui_run(app_t *app)
{
    uint64_t memorySize = Clay_MinMemorySize();
    void *memory = malloc(memorySize);
    if (memory == NULL) {
        fprintf(stderr, "out of memory setting up the interface\n");
        return 1;
    }

    Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(memorySize, memory);
    int window_h = UI_WINDOW_HEIGHT_FOR(app->debug);

    Clay_Initialize(arena,
                    (Clay_Dimensions){ UI_WINDOW_WIDTH, (float)window_h },
                    (Clay_ErrorHandler){ HandleClayErrors, NULL });

#ifndef _WIN32
    /*
     * X11 identifies a window by WM_CLASS, and that is what the panel matches
     * against StartupWMClass in IOMeeter.desktop to pair the window with its
     * launcher and icon. GLFW builds WM_CLASS from RESOURCE_NAME when that is
     * set and from the window title otherwise, so setting it here pins the
     * name even in a session that exports its own.
     */
    setenv("RESOURCE_NAME", "IOMeeter", 1);
#endif

    Clay_Raylib_Initialize(UI_WINDOW_WIDTH, window_h,
                           "IOMeeter",
                           FLAG_VSYNC_HINT | FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);

    /* The layout stops being readable below its design size, so make that the
       floor rather than letting the cards collapse. */
    SetWindowMinSize(UI_WINDOW_MIN_WIDTH, UI_WINDOW_MIN_HEIGHT);

    /*
     * Escape must not end the program: it cancels the rename editor, and with
     * the tray running the only real quit is the tray menu.
     */
    SetExitKey(KEY_NULL);

    /*
     * Run at the panel's refresh rate, with vsync left on. Presenting on the
     * refresh boundary is what makes motion even; a target above it can only
     * tear, and one below it drops frames.
     */
    s_target_fps = GetMonitorRefreshRate(GetCurrentMonitor());
    if (s_target_fps <= 0) {
        s_target_fps = UI_FALLBACK_FPS;     /* driver did not report one */
    }
    SetTargetFPS(s_target_fps);

    /*
     * Window icon. raylib has no .ico loader, so the PNG beside it is what gets
     * used here; the .ico is still the better source on Windows and tray_init()
     * overrides this with it below.
     *
     * On Linux this is the icon the panel shows for the window, and the only
     * one set at all, since the tray layer is a stub there.
     */
    char icon_path[512];

    if (respath_find("icon.png", icon_path, sizeof(icon_path))) {
        Image icon = LoadImage(icon_path);
        if (icon.data) {
            ImageFormat(&icon, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
            SetWindowIcon(icon);
            UnloadImage(icon);
        }
    } else {
        fprintf(stderr, "could not find icon.png\n");
    }

    /* On Windows this also owns the tray, and replaces the icon above with the
       multi-resolution .ico through the shell. */
    char tray_icon[512];
#ifdef _WIN32
    const char *tray_icon_file = "icon.ico";
#else
    const char *tray_icon_file = "icon.png";    /* AppIndicator cannot read .ico */
#endif
    if (!respath_find(tray_icon_file, tray_icon, sizeof(tray_icon))) {
        tray_icon[0] = 0;
    }
    tray_init(GetWindowHandle(), tray_icon, "IOMeeter");

    char font_path[512];

    if (respath_find("Roboto-Regular.ttf", font_path, sizeof(font_path))) {
        s_fonts[FONT_BODY] = LoadFontEx(font_path, 32, NULL, 0);
    }
    if (respath_find("RobotoMono-Medium.ttf", font_path, sizeof(font_path))) {
        s_fonts[FONT_MONO] = LoadFontEx(font_path, 32, NULL, 0);
    }
    for (int i = 0; i < FONT_COUNT; i++) {
        if (!s_fonts[i].glyphs) {
            s_fonts[i] = GetFontDefault();
        } else {
            SetTextureFilter(s_fonts[i].texture, TEXTURE_FILTER_BILINEAR);
        }
    }

    Clay_SetMeasureTextFunction(Raylib_MeasureText, s_fonts);
    Clay_Raylib_SetFonts(s_fonts, FONT_COUNT);

    Clay_ElementId wheel_id  = CLAY_ID("ColourWheel");
    Clay_ElementId slider_id = CLAY_ID("Brightness");

    double last_live_send = 0.0;
    double last_config_save = 0.0;

    /*
     * While the device is driving a slider, the pointer is locked out so the
     * two do not fight over the same value.
     */
    unsigned long slider_extern_seen = 0;
    double slider_locked_until = 0.0;

    /* Per fader, so only the one the device is actually moving recolours. */
    unsigned long slider_seen_at[APP_FADER_COUNT] = { 0 };
    double slider_device_until[APP_FADER_COUNT] = { 0.0 };

    /* Loading a configuration can flip "debug", which adds or removes the
       traffic console and so changes the height the layout needs. */
    bool debug_seen = app->debug;

    for (;;) {
        /*
         * On Windows the close button never gets this far: tray.c swallows
         * WM_CLOSE in the window procedure. Elsewhere it arrives here, and
         * WindowShouldClose() reports it exactly once because raylib clears
         * GLFW's flag as it reads it, so ignoring it simply keeps the loop
         * running. With a tray present that means hiding instead of quitting.
         */
        if (WindowShouldClose()) {
            if (!tray_available()) {
                break;
            }
            tray_minimize();
        }

        if (tray_poll()) {      /* "Close IOMeeter" from the tray menu */
            break;
        }

        /* Somebody launched IOMeeter again: surface instead of ignoring it. */
        if (instance_show_requested()) {
            tray_restore();
            instance_raise(GetWindowHandle());
        }

        if (tray_available() && !tray_is_minimized() && IsWindowMinimized()) {
            tray_minimize();
        }

        /* Hidden: no window to draw into, so idle instead of spinning. */
        if (tray_is_minimized()) {
            app_poll(app);
            if (app->notice_seq != s_toast_seen) {
                s_toast_seen = app->notice_seq;
                tray_notify("Interrupt", app->notice);
            }
            /* Nothing is drawn while hidden, and EndDrawing() is what normally
               pumps input, so drain the queue explicitly. */
            PollInputEvents();
            WaitTime(0.05);
            continue;
        }

        app_poll(app);

        if (app->debug != debug_seen) {
            debug_seen = app->debug;
            window_h = UI_WINDOW_HEIGHT_FOR(app->debug);
            SetWindowMinSize(UI_WINDOW_MIN_WIDTH, UI_WINDOW_MIN_HEIGHT);
            SetWindowSize(GetScreenWidth(), window_h);
        }

        if (app->notice_seq != s_toast_seen) {
            s_toast_seen = app->notice_seq;
            s_toast_until = GetTime() + TOAST_SECONDS;
            /* Hidden in the tray there is no window to draw the toast on, so
               hand it to the shell instead. */
            tray_notify("Interrupt", app->notice);
        }

        ui_pump_text_input();
        ui_update_rename(app);
        ui_rebuild_wheel(app);

        Clay_SetLayoutDimensions((Clay_Dimensions){
            (float)GetScreenWidth(), (float)GetScreenHeight() });

        Vector2 mouse = GetMousePosition();
        Clay_SetPointerState((Clay_Vector2){ mouse.x, mouse.y },
                             IsMouseButtonDown(MOUSE_BUTTON_LEFT));
        Clay_UpdateScrollContainers(true,
                                    (Clay_Vector2){ 0, GetMouseWheelMove() * 32 },
                                    GetFrameTime());

        /*
         * The menu is resolved first and, while open, swallows the pointer.
         * The wheel, sliders and faders all hit-test raw mouse coordinates
         * without knowing what is drawn above them, so without this a click on
         * a menu entry also dragged whatever sat underneath it.
         */
        Rectangle menu_panel = { 0 };
        bool menu_blocks = false;

        if (s_menu_open) {
            Clay_ElementData button = Clay_GetElementData(CLAY_ID("MenuButton"));
            if (button.found) {
                menu_panel = ui_menu_rect(button.boundingBox);
                Vector2 mouse = GetMousePosition();
                Clay_BoundingBox b = button.boundingBox;

                bool over_button = mouse.x >= b.x && mouse.x <= b.x + b.width &&
                                   mouse.y >= b.y && mouse.y <= b.y + b.height;
                menu_blocks = CheckCollisionPointRec(mouse, menu_panel) || over_button;

                if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    int item = ui_menu_hit(menu_panel);
                    if (item == 0) {
                        s_menu_open = false;
                        ui_menu_save_config(app);
                    } else if (item == 1) {
                        s_menu_open = false;
                        ui_menu_load_config(app);
                    } else if (item == 2) {
                        s_menu_open = false;
                        ui_menu_reload_config(app);
                    } else if (!over_button) {
                        s_menu_open = false;   /* clicked away */
                    }
                }
            } else {
                s_menu_open = false;   /* button not laid out yet */
            }
        }

        /* Interaction against the previous frame's boxes, before laying out
           the next one. */
        ui_log_interact(app);

        float fader_h = (float)ui_fader_height(app->debug);

        bool colour_changed = false;
        Clay_ElementData wheel_data = Clay_GetElementData(wheel_id);
        if (wheel_data.found && !menu_blocks) {
            ui_wheel_interact(app, wheel_data.boundingBox, &colour_changed);
        }
        if (app->slider_extern_seq != slider_extern_seen) {
            slider_extern_seen = app->slider_extern_seq;
            slider_locked_until = GetTime() + 0.5;
        }

        /* Held briefly past the last packet so the colour does not flicker
           between updates while the hardware fader is being moved. */
        for (int i = 0; i < APP_FADER_COUNT; i++) {
            if (app->slider_extern_at[i] != slider_seen_at[i]) {
                slider_seen_at[i] = app->slider_extern_at[i];
                slider_device_until[i] = GetTime() + 0.5;
            }
        }
        bool sliders_locked = GetTime() < slider_locked_until;

        for (int i = 0; i < APP_FADER_COUNT && !menu_blocks && !sliders_locked; i++) {
            Clay_ElementData slot = Clay_GetElementData(CLAY_IDI("Fader", i));
            if (!slot.found) {
                continue;
            }

            if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
                Vector2 m = GetMousePosition();
                Clay_BoundingBox b = slot.boundingBox;
                if (m.x >= b.x && m.x <= b.x + b.width &&
                    m.y >= b.y && m.y <= b.y + b.height) {
                    s_rename = i;
                    snprintf(s_rename_buf, sizeof(s_rename_buf), "%s",
                             app->sliders[i].name);
                    s_focus = s_rename_buf;         /* route typing here */
                    s_focus_cap = sizeof(s_rename_buf);
                }
            }

            fader_interact(i, slot.boundingBox, &app->sliders[i].value,
                           APP_FADER_MAX, ui_on_fader_change, app);
        }

        if (!menu_blocks && ui_slider(slider_id, &app->val)) {
            colour_changed = true;
            app_sync_hex(app);
        }

        if (colour_changed && app->live_send && app->connected) {
            /* Throttle so a drag does not flood the endpoint. */
            if (GetTime() - last_live_send > 0.06) {
                last_live_send = GetTime();
                app_set_led(app);
            }
        }

        s_text_slot = 0;    /* hand out fresh text storage each frame */
        Clay_BeginLayout();

        CLAY(CLAY_ID("Root"), {
            .layout = {
                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                .padding = { 15, 15, 20, 20 },      /* left, right, top, bottom */
                .childGap = 12,
                .layoutDirection = CLAY_TOP_TO_BOTTOM,
            },
            .backgroundColor = C_BG,
        }) {
            /* ---- header ---- */
            CLAY(CLAY_ID("Header"), {
                .layout = {
                    .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                    .childGap = 10,
                    .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                },
            }) {
                /* Hamburger. Three bars rather than a glyph, so it does not
                   depend on the loaded font having one. */
                CLAY(CLAY_ID("MenuButton"), {
                    .layout = {
                        .sizing = { CLAY_SIZING_FIXED(34), CLAY_SIZING_FIXED(30) },
                        .padding = CLAY_PADDING_ALL(8),
                        .childGap = 4,
                        .layoutDirection = CLAY_TOP_TO_BOTTOM,
                        .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                    },
                    .backgroundColor = (s_menu_open || Clay_Hovered())
                                     ? C_LINE : C_CARD,
                    .cornerRadius = CLAY_CORNER_RADIUS(8),
                    .border = { .color = C_BORDER, .width = { 1, 1, 1, 1 } },
                }) {
                    if (clicked(true)) {
                        s_menu_open = !s_menu_open;
                    }
                    for (int bar = 0; bar < 3; bar++) {
                        CLAY(CLAY_IDI("MenuBar", bar), {
                            .layout = { .sizing = { CLAY_SIZING_GROW(0),
                                                    CLAY_SIZING_FIXED(2) } },
                            .backgroundColor = C_FG,
                            .cornerRadius = CLAY_CORNER_RADIUS(1),
                        }) {}
                    }
                }

                CLAY_AUTO_ID({ .layout = { .layoutDirection = CLAY_TOP_TO_BOTTOM,
                                           .childGap = 2 } }) {
                    CLAY_TEXT(CLAY_STRING("IOMeeter"),
                              CLAY_TEXT_CONFIG({ .fontId = FONT_BODY, .fontSize = 20,
                                                 .textColor = C_FG }));
                }

                CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_GROW(0) } } }) {}

                CLAY_AUTO_ID({
                    .layout = { .padding = { 10, 10, 6, 6 } },
                    .cornerRadius = CLAY_CORNER_RADIUS(999),
                    .border = { .color = C_BORDER, .width = { 1, 1, 1, 1 } },
                }) {
                    CLAY_TEXT(dyn(app->connected ? "connected" : "disconnected"),
                              CLAY_TEXT_CONFIG({
                                  .fontId = FONT_MONO, .fontSize = 12,
                                  .textColor = app->connected ? C_OK : C_MUTED }));
                }

                if (app->connected) {
                    if (ui_button(CLAY_ID("Disconnect"), "Disconnect", false, true, false)) {
                        app_disconnect(app);
                    }
                } else {
                    if (ui_button(CLAY_ID("Connect"), "Connect", true, true, false)) {
                        app_connect(app);
                    }
                }
            }

            /* ---- two columns ---- */
            CLAY(CLAY_ID("Columns"), {
                .layout = {
                    .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                    .childGap = 12,
                },
            }) {
                UI_CARD(CLAY_ID("LedCard")) {
                    ui_card_title("NEOPIXEL");

                    CLAY_AUTO_ID({ .layout = { .childGap = 12 } }) {
                        CLAY(wheel_id, {
                            .layout = { .sizing = { CLAY_SIZING_FIXED(150),
                                                    CLAY_SIZING_FIXED(150) } },
                            .image = { .imageData = &s_wheel },
                            .backgroundColor = C_TRANSPARENT,
                        }) {}

                        CLAY_AUTO_ID({
                            .layout = {
                                .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                                .childGap = 8,
                                .layoutDirection = CLAY_TOP_TO_BOTTOM,
                            },
                        }) {
                            CLAY_TEXT(CLAY_STRING("Brightness"), CLAY_TEXT_CONFIG({
                                .fontId = FONT_BODY, .fontSize = 12,
                                .textColor = C_MUTED }));

                            /* Declared here; the value was read further up. */
                            CLAY(slider_id, {
                                .layout = { .sizing = { CLAY_SIZING_GROW(0),
                                                        CLAY_SIZING_FIXED(18) },
                                            .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
                            }) {
                                CLAY_AUTO_ID({
                                    .layout = { .sizing = { CLAY_SIZING_GROW(0),
                                                            CLAY_SIZING_FIXED(5) } },
                                    .backgroundColor = C_LINE,
                                    .cornerRadius = CLAY_CORNER_RADIUS(3),
                                }) {}
                            }

                            CLAY_AUTO_ID({ .layout = { .childGap = 8,
                                                       .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
                                uint32_t rgb = app_rgb(app);
                                CLAY_AUTO_ID({
                                    .layout = { .sizing = { CLAY_SIZING_FIXED(34),
                                                            CLAY_SIZING_FIXED(30) } },
                                    .backgroundColor = COL((rgb >> 16) & 0xFF,
                                                           (rgb >> 8) & 0xFF,
                                                           rgb & 0xFF, 255),
                                    .cornerRadius = CLAY_CORNER_RADIUS(8),
                                    .border = { .color = C_BORDER, .width = { 1, 1, 1, 1 } },
                                }) {}

                                ui_text_field(CLAY_ID("HexField"), app->hex,
                                              APP_HEX_MAX, FONT_MONO, NULL);
                            }

                            ui_checkbox(CLAY_ID("Live"), "Send while dragging",
                                        &app->live_send);

                            CLAY_AUTO_ID({ .layout = { .childGap = 8 } }) {
                                if (ui_button(CLAY_ID("SetLed"), "Set LED", true,
                                              app->connected, true)) {
                                    app_set_led(app);
                                }
                                if (ui_button(CLAY_ID("GetLed"), "Get", false,
                                              app->connected, false)) {
                                    app_get(app, "led");
                                }
                            }
                        }
                    }
                }

                /* Faders. Clay only reserves the slots; fader_draw paints
                   the gradient track, knob and rotated label afterwards. */
                UI_CARD(CLAY_ID("FaderCard")) {
                    ui_card_title("FADERS");

                    CLAY_AUTO_ID({
                        .layout = {
                            .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0) },
                            .childGap = 6,
                        },
                    }) {
                        for (int i = 0; i < APP_FADER_COUNT; i++) {
                            CLAY(CLAY_IDI("FaderCol", i), {
                                .layout = {
                                    .sizing = { CLAY_SIZING_FIXED(FADER_COLUMN_W),
                                                CLAY_SIZING_FIT(0) },
                                    .childGap = 6,
                                    .layoutDirection = CLAY_TOP_TO_BOTTOM,
                                    .childAlignment = { .x = CLAY_ALIGN_X_CENTER },
                                },
                            }) {
                                CLAY(CLAY_IDI("Fader", i), {
                                    .layout = { .sizing = {
                                        CLAY_SIZING_FIXED(FADER_SLOT_WIDTH),
                                        CLAY_SIZING_FIXED(fader_h) } },
                                }) {}

                                if (ui_button(CLAY_IDI("AddApp", i), "+",
                                              false, true, true)) {
                                    ui_add_app(app, i);
                                }

                                CLAY(CLAY_IDI("AppList", i), {
                                    .layout = {
                                        .sizing = { CLAY_SIZING_GROW(0),
                                                    CLAY_SIZING_FIXED(APP_LIST_H) },
                                        .padding = { 4, 4, 2, 2 },
                                        .layoutDirection = CLAY_TOP_TO_BOTTOM,
                                    },
                                    .backgroundColor = C_FIELD,
                                    .cornerRadius = CLAY_CORNER_RADIUS(6),
                                    /* Scrolls once the list outgrows the fixed
                                       height, i.e. past five entries. */
                                    .clip = { .vertical = true,
                                              .childOffset = Clay_GetScrollOffset() },
                                }) {
                                    for (int a = 0; a < app->sliders[i].app_count; a++) {
                                        int slot = i * CONFIG_APPS_MAX + a;

                                        CLAY(CLAY_IDI("AppRow", slot), {
                                            .layout = {
                                                .sizing = { CLAY_SIZING_GROW(0),
                                                            CLAY_SIZING_FIXED(APP_ROW_H) },
                                                .childGap = 2,
                                                .childAlignment = { .y = CLAY_ALIGN_Y_CENTER },
                                            },
                                        }) {
                                            CLAY_AUTO_ID({
                                                .layout = { .sizing = { CLAY_SIZING_GROW(0) } },
                                            }) {
                                                CLAY_TEXT(dyn(app->sliders[i].apps[a].name),
                                                    CLAY_TEXT_CONFIG({
                                                        .fontId = FONT_BODY,
                                                        .fontSize = 11,
                                                        .textColor = C_MUTED }));
                                            }

                                            CLAY(CLAY_IDI("AppDel", slot), {
                                                .layout = {
                                                    .sizing = { CLAY_SIZING_FIXED(13),
                                                                CLAY_SIZING_FIXED(13) },
                                                    .childAlignment = { CLAY_ALIGN_X_CENTER,
                                                                        CLAY_ALIGN_Y_CENTER },
                                                },
                                                .backgroundColor = Clay_Hovered()
                                                                 ? C_WARN : C_LINE,
                                                .cornerRadius = CLAY_CORNER_RADIUS(3),
                                            }) {
                                                if (clicked(true)) {
                                                    s_remove_slider = i;
                                                    s_remove_index = a;
                                                }
                                                /* "x" rather than a multiplication
                                                   sign: the fonts are loaded with
                                                   the default ASCII range only. */
                                                CLAY_TEXT(CLAY_STRING("x"),
                                                    CLAY_TEXT_CONFIG({
                                                        .fontId = FONT_BODY,
                                                        .fontSize = 11,
                                                        .textColor = C_FG }));
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            /* ---- traffic ----
               Declared only when "debug" is set in config.json; leaving it out
               of the layout hands its height back to the cards above. */
            if (app->debug) {
            UI_CARD(CLAY_ID("LogCard")) {
                CLAY_AUTO_ID({ .layout = { .childGap = 12,
                                           .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
                    CLAY_TEXT(CLAY_STRING("TRAFFIC"), CLAY_TEXT_CONFIG({
                        .fontId = FONT_BODY, .fontSize = 13, .textColor = C_MUTED }));

                    CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_GROW(0) } } }) {}

                    if (ui_button(CLAY_ID("ClearLog"), "Clear", false, true, false)) {
                        app_log_clear(app);
                    }
                }

                CLAY(CLAY_ID("LogScroll"), {
                    .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                                .padding = CLAY_PADDING_ALL(8),
                                .childGap = 1,
                                .layoutDirection = CLAY_TOP_TO_BOTTOM },
                    .backgroundColor = C_FIELD,
                    .cornerRadius = CLAY_CORNER_RADIUS(8),
                    .clip = { .vertical = true,
                              .childOffset = Clay_GetScrollOffset() },
                }) {
                    for (int i = 0; i < app->log_count; i++) {
                        const app_log_entry_t *entry = app_log_at(app, i);
                        CLAY_AUTO_ID({
                            .layout = { .sizing = { CLAY_SIZING_GROW(0),
                                                    CLAY_SIZING_FIXED(LOG_ROW_H) } },
                            .backgroundColor = log_line_selected(i)
                                             ? C_SELECT : C_TRANSPARENT,
                        }) {
                            CLAY_TEXT(dyn(entry->text), CLAY_TEXT_CONFIG({
                                .fontId = FONT_MONO, .fontSize = 12,
                                .textColor = log_colour(entry->kind) }));
                        }
                    }
                }
            }
            }   /* if (app->debug) */
        }

        if (app->config_dirty && GetTime() - last_config_save > 1.0) {
            last_config_save = GetTime();
            app_config_save(app);
        }

        if (s_remove_slider >= 0) {
            config_slider_t *slider = &app->sliders[s_remove_slider];
            if (s_remove_index < slider->app_count) {
                app_log(app, APP_LOG_EVENT, "%s no longer controls %s",
                        slider->name, slider->apps[s_remove_index].name);
                config_remove_app(slider, s_remove_index);
                app->config_dirty = true;
            }
            s_remove_slider = -1;
            s_remove_index = -1;
        }

        Clay_RenderCommandArray commands = Clay_EndLayout();

        /* Needs this frame's content height, so it runs after the layout. */
        if (app->debug) {
            ui_autoscroll_log(app, CLAY_ID("LogScroll"));
        }

        BeginDrawing();
        ClearBackground((Color){ 0x0f, 0x0f, 0x0f, 255 });
        Clay_Raylib_Render(commands);

        Clay_ElementData wheel_now = Clay_GetElementData(wheel_id);
        if (wheel_now.found) {
            ui_draw_wheel_marker(app, wheel_now.boundingBox);
        }
        ui_draw_slider_knob(slider_id, app->val);

        for (int i = 0; i < APP_FADER_COUNT; i++) {
            Clay_ElementData slot = Clay_GetElementData(CLAY_IDI("Fader", i));
            if (slot.found) {
                fader_draw(i, slot.boundingBox, app->sliders[i].value,
                           APP_FADER_MAX, app->sliders[i].name,
                           &s_fonts[FONT_BODY],
                           GetTime() < slider_device_until[i]);
            }
        }

        ui_log_draw_scrollbar(app);

        if (s_rename >= 0) {
            Clay_ElementData slot = Clay_GetElementData(CLAY_IDI("Fader", s_rename));
            if (slot.found) {
                ui_draw_rename(app, slot.boundingBox);
            }
        }

        ui_draw_toast(app);

        /* Last of all, so it covers the wheel marker, knob and faders. */
        if (s_menu_open && menu_panel.width > 0.0f) {
            ui_draw_menu(menu_panel);
        }

        ui_draw_fps(app);

        EndDrawing();
    }

    if (s_wheel.id != 0) {
        UnloadTexture(s_wheel);
    }
    tray_shutdown();
    Clay_Raylib_Close();
    free(memory);
    return 0;
}
