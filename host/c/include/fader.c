#include "fader.h"

#include <math.h>

#include "raylib.h"

/* Track geometry within the reserved slot. The knob is wider than the track, so
   it still stands proud of it as on a real fader. */
#define KNOB_RADIUS   21.0f

/* The knob is a rectangle as wide as the old circle and half as tall. Travel
   reserves half its height at each end, so it reaches both ends of the track
   without overhanging either. */
#define KNOB_WIDTH    (KNOB_RADIUS * 2.0f)
#define KNOB_HEIGHT   KNOB_RADIUS
#define KNOB_BORDER   3.0f

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

/* Driven by the hardware fader rather than the pointer: #de123a, with the
   same light-top, deep-bottom, darker-edge relationship as the mint set. */
static const Color DEVICE_TOP    = { 240, 74,  106, 255 };
static const Color DEVICE_BOTTOM = { 222, 18,  58,  255 };
static const Color DEVICE_EDGE   = { 176, 14,  46,  255 };
static const Color DEVICE_KNOB   = { 222, 18,  58,  255 };
static const Color DEVICE_RING   = { 245, 120, 145, 255 };
static const Color DEVICE_LABEL  = { 70,  10,  22,  255 };

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

    /*
     * Travel is inset by half the knob, which is what keeps it inside the
     * track: at either extreme the knob's edge is flush with the end of the
     * track. Insetting by the full height instead left it a knob short of
     * both ends.
     */
    m.track_top = box.y + KNOB_HEIGHT / 2.0f;
    m.track_bottom = box.y + box.height - KNOB_HEIGHT / 2.0f;
    m.travel = m.track_bottom - m.track_top;

    if (m.travel < 1.0f) {
        m.travel = 1.0f;
    }
    return m;
}

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
                      mouse.y <= box.y + box.height;
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

/*
 * Smoothed knob positions, one per id. Negative means "no value yet", which
 * snaps on the first frame so the strip does not sweep up from zero at start.
 */
static float s_display[FADER_MAX_IDS] = {
    -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f
};

/* Time constant of the ease, in seconds. Short enough not to feel like lag. */
#define FADER_SMOOTH_TAU 0.010f

/* Framerate independent exponential approach to @p target. */
static float smoothed(int id, float target, bool snap)
{
    if (id < 0 || id >= FADER_MAX_IDS) {
        return target;
    }

    float *current = &s_display[id];

    if (snap || *current < 0.0f) {
        *current = target;
        return target;
    }

    float dt = GetFrameTime();
    if (dt <= 0.0f) {
        return *current;
    }

    /* 1 - exp(-dt/tau) lands in the same place whatever the frame rate, unlike
       a fixed fraction per frame. */
    float k = 1.0f - expf(-dt / FADER_SMOOTH_TAU);
    *current += (target - *current) * k;

    /* Stop creeping once it is closer than a pixel is worth. */
    if (fabsf(target - *current) < 0.0005f) {
        *current = target;
    }
    return *current;
}

void fader_draw(int id, Clay_BoundingBox box, int value, int max,
                const char *label, void *font, bool device)
{
    Font f = *(Font *)font;
    fader_metrics_t m = metrics_of(box);

    /* The filled half, the knob and the label all recolour together, so the
       source of the movement is obvious at a glance. */
    Color track_top    = device ? DEVICE_TOP    : TRACK_TOP;
    Color track_bottom = device ? DEVICE_BOTTOM : TRACK_BOTTOM;
    Color track_edge   = device ? DEVICE_EDGE   : TRACK_EDGE;
    Color knob_fill    = device ? DEVICE_KNOB   : KNOB_FILL;
    Color knob_ring    = device ? DEVICE_RING   : KNOB_RING;
    Color label_text   = device ? DEVICE_LABEL  : LABEL_TEXT;

    float half = TRACK_WIDTH / 2.0f;
    float track_top_y = box.y;
    float track_bottom_y = box.y + box.height;

    /* Knob position: 0 sits at the bottom of the travel. */
    float t = (max > 0) ? (float)value / (float)max : 0.0f;
    t = smoothed(id, t, s_active == id);
    float knob_y = m.track_bottom - t * m.travel;

    /*
     * The track is drawn in two pieces meeting at the knob: filled below it,
     * empty above. A square track needs no end caps, so each piece is one
     * gradient rectangle running the full width.
     */
    float split = fminf(fmaxf(knob_y, track_top_y), track_bottom_y);

    float empty_h = split - track_top_y;
    if (empty_h > 0.0f) {
        DrawRectangleGradientV((int)(m.centre_x - half), (int)track_top_y,
                               (int)TRACK_WIDTH, (int)empty_h,
                               EMPTY_TOP, EMPTY_BOTTOM);
        DrawLineEx((Vector2){ m.centre_x - half, track_top_y },
                   (Vector2){ m.centre_x - half, split }, 1.0f, EMPTY_EDGE);
        DrawLineEx((Vector2){ m.centre_x + half, track_top_y },
                   (Vector2){ m.centre_x + half, split }, 1.0f, EMPTY_EDGE);
    }

    float filled_h = track_bottom_y - split;
    if (filled_h > 0.0f) {
        DrawRectangleGradientV((int)(m.centre_x - half), (int)split,
                               (int)TRACK_WIDTH, (int)filled_h,
                               track_top, track_bottom);
        DrawLineEx((Vector2){ m.centre_x - half, split },
                   (Vector2){ m.centre_x - half, track_bottom_y }, 1.0f, track_edge);
        DrawLineEx((Vector2){ m.centre_x + half, split },
                   (Vector2){ m.centre_x + half, track_bottom_y }, 1.0f, track_edge);
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
    float label_centre_y = box.y + box.height - LABEL_INSET
                         - extent.x / 2.0f;

    draw_bold_pro(f, label,
                  (Vector2){ m.centre_x, label_centre_y },
                  (Vector2){ extent.x / 2.0f, extent.y / 2.0f },
                  -90.0f, LABEL_SIZE, label_text);

    Rectangle knob = { m.centre_x - KNOB_WIDTH / 2.0f,
                       knob_y - KNOB_HEIGHT / 2.0f,
                       KNOB_WIDTH, KNOB_HEIGHT };

    DrawRectangleRec(knob, knob_ring);
    DrawRectangleRec((Rectangle){ knob.x + KNOB_BORDER,
                                  knob.y + KNOB_BORDER,
                                  knob.width - 2.0f * KNOB_BORDER,
                                  knob.height - 2.0f * KNOB_BORDER }, knob_fill);
}
