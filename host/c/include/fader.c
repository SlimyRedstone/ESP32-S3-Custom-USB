#include "fader.h"

#include <math.h>

#include "raylib.h"

/* Track geometry within the reserved slot. The knob is wider than the track, so
   it still stands proud of it as on a real fader. */
#define KNOB_RADIUS   21.0f
#define TRACK_WIDTH   22.0f

/* Distance from the bottom of the track to the end of the label. */
#define LABEL_INSET   16.0f

/* Label type, scaled up 50% from its original 14. */
#define LABEL_BASE    14.0f
#define LABEL_SIZE    (LABEL_BASE * 1.5f)

/*
 * There is no bold face among the loaded fonts, so weight is faked by drawing
 * the text several times a fraction of a pixel apart.
 */
#define BOLD_OFFSET   0.7f

/* Mint palette taken from the reference: a light track, a deeper knob, and a
   pale ring so the knob reads as raised against it. */
static const Color TRACK_TOP    = { 168, 230, 190, 255 };
static const Color TRACK_BOTTOM = { 118, 198, 152, 255 };
static const Color TRACK_EDGE   = { 96,  170, 128, 255 };

/* Above the knob the track reads as unfilled. */
static const Color EMPTY_TOP    = { 84,  92,  104, 255 };
static const Color EMPTY_BOTTOM = { 68,  76,  88,  255 };
static const Color EMPTY_EDGE   = { 54,  60,  70,  255 };
static const Color KNOB_FILL    = { 108, 190, 145, 255 };
static const Color KNOB_RING    = { 176, 236, 200, 255 };
static const Color LABEL_TEXT   = { 32,  62,  46,  255 };

/*
 * Which fader owns the current drag, or FADER_NONE.
 *
 * Without this every fader within reach of the pointer responded to the same
 * drag, because each one only tested its own distance from the cursor. Capture
 * on press and release on button-up is what keeps a drag to one control.
 */
#define FADER_NONE (-1)
static int s_active = FADER_NONE;

/*
 * Geometry shared by interaction and drawing, so a click always lands where the
 * knob is painted.
 */
typedef struct {
    float centre_x;
    float track_top;      /*!< y of the topmost travel position    */
    float track_bottom;   /*!< y of the bottommost travel position */
    float travel;         /*!< distance between the two            */
} fader_metrics_t;

static fader_metrics_t metrics_of(Clay_BoundingBox box)
{
    fader_metrics_t m;

    m.centre_x = box.x + box.width / 2.0f;

    /* The knob must stay inside the track, so travel is inset by its radius. */
    m.track_top = box.y + KNOB_RADIUS;
    m.track_bottom = box.y + FADER_TRACK_HEIGHT - KNOB_RADIUS;
    m.travel = m.track_bottom - m.track_top;

    if (m.travel < 1.0f) {
        m.travel = 1.0f;
    }
    return m;
}

/* Draw text several times with sub-pixel offsets to imitate a bold face. */
static void draw_bold_pro(Font font, const char *text, Vector2 position,
                          Vector2 origin, float rotation, float size, Color tint)
{
    static const Vector2 nudges[] = {
        { 0.0f, 0.0f },
        { BOLD_OFFSET, 0.0f },
        { 0.0f, BOLD_OFFSET },
        { BOLD_OFFSET, BOLD_OFFSET },
    };

    for (int i = 0; i < (int)(sizeof(nudges) / sizeof(nudges[0])); i++) {
        Vector2 at = { position.x + nudges[i].x, position.y + nudges[i].y };
        DrawTextPro(font, text, at, origin, rotation, size, 0.0f, tint);
    }
}

bool fader_interact(int id, Clay_BoundingBox box, int *value, int max,
                    fader_change_cb_t on_change, void *user)
{
    /* Button up ends any drag this fader owned. */
    if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        if (s_active == id) {
            s_active = FADER_NONE;
        }
        return false;
    }

    fader_metrics_t m = metrics_of(box);
    Vector2 mouse = GetMousePosition();

    /* Claim the drag only on the press, and only inside this slot. */
    if (s_active == FADER_NONE && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        bool inside = mouse.x >= box.x && mouse.x <= box.x + box.width &&
                      mouse.y >= box.y &&
                      mouse.y <= box.y + FADER_TRACK_HEIGHT;
        if (inside) {
            s_active = id;
        }
    }

    if (s_active != id) {
        return false;
    }

    /*
     * Once captured, horizontal position no longer matters: the pointer is
     * free to wander while the value follows its height.
     *
     * Screen y grows downwards; the fader reads the other way round.
     */
    float t = (m.track_bottom - mouse.y) / m.travel;
    t = fmaxf(0.0f, fminf(1.0f, t));

    int next = (int)lroundf(t * (float)max);
    if (next == *value) {
        return false;
    }

    *value = next;
    if (on_change) {
        on_change(id, next, user);
    }
    return true;
}

void fader_draw(Clay_BoundingBox box, int value, int max, const char *label,
                void *font)
{
    Font f = *(Font *)font;
    fader_metrics_t m = metrics_of(box);

    float half = TRACK_WIDTH / 2.0f;
    float cap_r = half;
    float top_cap_y = box.y + cap_r;
    float bottom_cap_y = box.y + FADER_TRACK_HEIGHT - cap_r;

    /* Knob position: 0 sits at the bottom of the travel. */
    float t = (max > 0) ? (float)value / (float)max : 0.0f;
    float knob_y = m.track_bottom - t * m.travel;

    /*
     * The track is drawn in two pieces meeting at the knob: filled below it,
     * empty above. Clamping to the caps keeps the split inside the straight
     * section; the knob is wider than the track, so the sliver of "wrong"
     * colour beyond either clamp is hidden underneath it.
     */
    float split = fminf(fmaxf(knob_y, top_cap_y), bottom_cap_y);

    /*
     * A pill with a vertical gradient: an end cap at each extreme plus a
     * gradient rectangle between them. DrawRectangleRounded cannot take a
     * gradient and a gradient rectangle has no rounded ends, so the shape is
     * built from both.
     */
    DrawCircleV((Vector2){ m.centre_x, top_cap_y }, cap_r, EMPTY_TOP);
    DrawCircleV((Vector2){ m.centre_x, bottom_cap_y }, cap_r, TRACK_BOTTOM);

    float empty_h = split - top_cap_y;
    if (empty_h > 0.0f) {
        DrawRectangleGradientV((int)(m.centre_x - half), (int)top_cap_y,
                               (int)TRACK_WIDTH, (int)empty_h,
                               EMPTY_TOP, EMPTY_BOTTOM);
        DrawLineEx((Vector2){ m.centre_x - half, top_cap_y },
                   (Vector2){ m.centre_x - half, split }, 1.0f, EMPTY_EDGE);
        DrawLineEx((Vector2){ m.centre_x + half, top_cap_y },
                   (Vector2){ m.centre_x + half, split }, 1.0f, EMPTY_EDGE);
    }

    float filled_h = bottom_cap_y - split;
    if (filled_h > 0.0f) {
        DrawRectangleGradientV((int)(m.centre_x - half), (int)split,
                               (int)TRACK_WIDTH, (int)filled_h,
                               TRACK_TOP, TRACK_BOTTOM);
        DrawLineEx((Vector2){ m.centre_x - half, split },
                   (Vector2){ m.centre_x - half, bottom_cap_y }, 1.0f, TRACK_EDGE);
        DrawLineEx((Vector2){ m.centre_x + half, split },
                   (Vector2){ m.centre_x + half, bottom_cap_y }, 1.0f, TRACK_EDGE);
    }

    /*
     * Label inside the bottom of the track, rotated a quarter turn because the
     * track is far narrower than the text. Drawn before the knob so that a
     * fader near zero covers the label rather than colliding with it.
     *
     * DrawTextPro turns about the origin passed to it, so putting the origin at
     * the text's midpoint keeps it centred on the position regardless of angle.
     *
     * -90 rather than 90: the letters' tops face left and the text reads from
     * the bottom upwards.
     */
    Vector2 extent = MeasureTextEx(f, label, LABEL_SIZE, 0.0f);
    float label_centre_y = box.y + FADER_TRACK_HEIGHT - LABEL_INSET
                         - extent.x / 2.0f;

    draw_bold_pro(f, label,
                  (Vector2){ m.centre_x, label_centre_y },
                  (Vector2){ extent.x / 2.0f, extent.y / 2.0f },
                  -90.0f, LABEL_SIZE, LABEL_TEXT);

    Vector2 knob = { m.centre_x, knob_y };

    DrawCircleV(knob, KNOB_RADIUS, KNOB_RING);
    DrawCircleV(knob, KNOB_RADIUS - 3.0f, KNOB_FILL);
}
