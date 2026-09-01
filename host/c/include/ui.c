#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fader.h"
#include "filedialog.h"
#include "tray.h"

#include "clay.h"
#include "clay_raylib_renderer.h"
#include "raylib.h"

/* --------------------------------------------------------------- theme --- */

#define COL(r, g, b, a) ((Clay_Color){ (float)(r), (float)(g), (float)(b), (float)(a) })

static const Clay_Color C_BG        = COL(0x14, 0x16, 0x1a, 255);
static const Clay_Color C_CARD      = COL(0x1c, 0x1f, 0x25, 255);
static const Clay_Color C_FIELD     = COL(0x14, 0x16, 0x1a, 255);
static const Clay_Color C_LINE      = COL(0x2b, 0x30, 0x39, 255);
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

    app->config_dirty = true;
    app->slider_pending[id] = true;
    app_send_slider(app, id, value);
}

#define UI_PI        3.14159265358979323846f
#define WHEEL_PIXELS 200        /* texture resolution of the colour wheel */
#define LOG_ROW_H    16

static Font s_fonts[FONT_COUNT];

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
        .border = { .color = C_LINE, .width = { 1, 1, 1, 1 } },
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
            .border = { .color = C_LINE, .width = { 1, 1, 1, 1 } },
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
        .border = { .color = focused ? C_ACCENT : C_LINE, .width = { 1, 1, 1, 1 } },
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
static void ui_wheel_interact(app_t *app, Clay_BoundingBox box, bool *changed)
{
    if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        return;
    }

    Vector2 m = GetMousePosition();
    float cx = box.x + box.width / 2.0f;
    float cy = box.y + box.height / 2.0f;
    float radius = fminf(box.width, box.height) / 2.0f;

    float dx = m.x - cx;
    float dy = m.y - cy;
    float dist = sqrtf(dx * dx + dy * dy);

    if (dist > radius && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        return;
    }
    if (dist > radius * 1.6f) {
        return;
    }

    float angle = atan2f(dy, dx) * 180.0f / UI_PI + 90.0f;
    if (angle < 0.0f) {
        angle += 360.0f;
    }

    app->hue = angle;
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
        .border = { .color = C_LINE, .width = { 1, 1, 1, 1 } },              \
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
#define MENU_ITEMS    2

static const char *const s_menu_items[MENU_ITEMS] = {
    "Save config", "Load config",
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
    Clay_Initialize(arena,
                    (Clay_Dimensions){ UI_WINDOW_WIDTH, UI_WINDOW_HEIGHT },
                    (Clay_ErrorHandler){ HandleClayErrors, NULL });

    Clay_Raylib_Initialize(UI_WINDOW_WIDTH, UI_WINDOW_HEIGHT,
                           "Custom USB Protocol",
                           FLAG_VSYNC_HINT | FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);

    /* The layout stops being readable below its design size, so make that the
       floor rather than letting the cards collapse. */
    SetWindowMinSize(UI_WINDOW_WIDTH, UI_WINDOW_HEIGHT);

    /*
     * Follow the monitor, but never drop below UI_MIN_FPS. Vsync cannot exceed
     * the refresh rate, so on a slower panel it has to be turned off for the
     * higher target to have any effect.
     */
    int refresh_hz = GetMonitorRefreshRate(GetCurrentMonitor());
    if (refresh_hz <= 0) {
        refresh_hz = 60;        /* driver did not report one */
    }

    int target_fps = (refresh_hz >= UI_MIN_FPS) ? refresh_hz : UI_MIN_FPS;
    if (target_fps > refresh_hz) {
        ClearWindowState(FLAG_VSYNC_HINT);
    }
    SetTargetFPS(target_fps);

    /*
     * Window icon. raylib has no .ico loader, so the PNG beside it is what gets
     * used here; the .ico is still the better source on Windows and tray_init()
     * overrides this with it below.
     *
     * On Linux this is the only thing that sets an icon at all, since the tray
     * layer is a stub there.
     */
    Image icon = LoadImage("resources/icon.png");
    if (icon.data) {
        ImageFormat(&icon, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
        SetWindowIcon(icon);
        UnloadImage(icon);
    } else {
        fprintf(stderr, "could not load resources/icon.png\n");
    }

    /* On Windows this also owns the tray, and replaces the icon above with the
       multi-resolution .ico through the shell. */
    tray_init(GetWindowHandle(), "resources/icon.ico", "Custom USB Protocol");

    s_fonts[FONT_BODY] = LoadFontEx("resources/Roboto-Regular.ttf", 32, NULL, 0);
    s_fonts[FONT_MONO] = LoadFontEx("resources/RobotoMono-Medium.ttf", 32, NULL, 0);
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

    bool quit = false;

    while (!WindowShouldClose() && !quit) {
        quit = tray_poll();

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
        bool colour_changed = false;
        Clay_ElementData wheel_data = Clay_GetElementData(wheel_id);
        if (wheel_data.found && !menu_blocks) {
            ui_wheel_interact(app, wheel_data.boundingBox, &colour_changed);
        }
        if (app->slider_extern_seq != slider_extern_seen) {
            slider_extern_seen = app->slider_extern_seq;
            slider_locked_until = GetTime() + 0.5;
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
                .padding = CLAY_PADDING_ALL(14),
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
                    .border = { .color = C_LINE, .width = { 1, 1, 1, 1 } },
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
                    CLAY_TEXT(CLAY_STRING("Custom USB Protocol"),
                              CLAY_TEXT_CONFIG({ .fontId = FONT_BODY, .fontSize = 20,
                                                 .textColor = C_FG }));
                    CLAY_TEXT(CLAY_STRING("ESP32-S3 - vendor interface over libusb"),
                              CLAY_TEXT_CONFIG({ .fontId = FONT_BODY, .fontSize = 13,
                                                 .textColor = C_MUTED }));
                }

                CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_GROW(0) } } }) {}

                CLAY_AUTO_ID({
                    .layout = { .padding = { 10, 10, 6, 6 } },
                    .cornerRadius = CLAY_CORNER_RADIUS(999),
                    .border = { .color = C_LINE, .width = { 1, 1, 1, 1 } },
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
                                    .border = { .color = C_LINE, .width = { 1, 1, 1, 1 } },
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
                            .sizing = { CLAY_SIZING_FIT(0),
                                        CLAY_SIZING_FIXED(FADER_SLOT_HEIGHT) },
                            .childGap = 6,
                        },
                    }) {
                        for (int i = 0; i < APP_FADER_COUNT; i++) {
                            CLAY(CLAY_IDI("Fader", i), {
                                .layout = { .sizing = {
                                    CLAY_SIZING_FIXED(FADER_SLOT_WIDTH),
                                    CLAY_SIZING_FIXED(FADER_SLOT_HEIGHT) } },
                            }) {}
                        }
                    }
                }

                CLAY_AUTO_ID({
                    .layout = {
                        .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0) },
                        .childGap = 12,
                        .layoutDirection = CLAY_TOP_TO_BOTTOM,
                    },
                }) {
                    UI_CARD(CLAY_ID("MessageCard")) {
                        ui_card_title("MESSAGE");

                        bool submitted = false;
                        ui_text_field(CLAY_ID("MessageField"), app->message,
                                      APP_MESSAGE_MAX, FONT_BODY, &submitted);

                        if (ui_button(CLAY_ID("SendMsg"), "Send", true,
                                      app->connected, true) || submitted) {
                            app_send_message(app);
                        }
                    }

                    UI_CARD(CLAY_ID("ConfigCard")) {
                        ui_card_title("DEVICE CONFIG");

                        ui_text_field(CLAY_ID("ConfigField"), app->config,
                                      APP_CONFIG_MAX, FONT_MONO, NULL);

                        CLAY_AUTO_ID({ .layout = { .childGap = 8 } }) {
                            if (ui_button(CLAY_ID("GetCfg"), "Get", false,
                                          app->connected, false)) {
                                app_get(app, "config");
                            }
                            if (ui_button(CLAY_ID("SetCfg"), "Set", true,
                                          app->connected, true)) {
                                app_set_config(app);
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
                        CLAY_AUTO_ID({ .layout = { .sizing = {
                                           CLAY_SIZING_GROW(0),
                                           CLAY_SIZING_FIXED(LOG_ROW_H) } } }) {
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

        Clay_RenderCommandArray commands = Clay_EndLayout();

        /* Needs this frame's content height, so it runs after the layout. */
        if (app->debug) {
            ui_autoscroll_log(app, CLAY_ID("LogScroll"));
        }

        BeginDrawing();
        ClearBackground((Color){ 0x14, 0x16, 0x1a, 255 });
        Clay_Raylib_Render(commands);

        Clay_ElementData wheel_now = Clay_GetElementData(wheel_id);
        if (wheel_now.found) {
            ui_draw_wheel_marker(app, wheel_now.boundingBox);
        }
        ui_draw_slider_knob(slider_id, app->val);

        for (int i = 0; i < APP_FADER_COUNT; i++) {
            Clay_ElementData slot = Clay_GetElementData(CLAY_IDI("Fader", i));
            if (slot.found) {
                fader_draw(slot.boundingBox, app->sliders[i].value,
                           APP_FADER_MAX, app->sliders[i].name,
                           &s_fonts[FONT_BODY]);
            }
        }

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
