/*
 * Vertical fader widget.
 *
 * A pill-shaped gradient track with a circular knob, and the label set
 * vertically inside the lower part of the track.
 *
 * Clay lays out nothing but the bounding box: the gradient, the circle and the
 * rotated label are all drawn straight to raylib, the same way the colour wheel
 * is. Interaction reads the box Clay recorded on the previous frame, so
 * fader_interact() must run before Clay_BeginLayout() and fader_draw() after
 * the render pass.
 */

#ifndef FADER_H
#define FADER_H

#include <stdbool.h>

#include "clay.h"

/* Reserved by Clay, per fader. The track is narrower and centred within it. */
#define FADER_SLOT_WIDTH   52

/*
 * The design height, and the floor it may be squeezed to so the strip still
 * fits a small window. The drawing follows whatever box Clay hands over, so
 * anything between the two works.
 */
#define FADER_TRACK_HEIGHT 325     /* 250 raised by 30% */
#define FADER_MIN_HEIGHT   180

/* The label sits inside the track, so a slot is just the track. */
#define FADER_SLOT_HEIGHT  FADER_TRACK_HEIGHT

/**
 * Called whenever a fader's value changes.
 *
 * @param id    The id passed to fader_interact().
 * @param value The new value.
 * @param user  The pointer handed to fader_interact(), untouched.
 */
typedef void (*fader_change_cb_t)(int id, int value, void *user);

/**
 * Update @p value from the pointer, if this fader owns the current drag.
 *
 * A press inside a fader captures it until the button is released, so dragging
 * past a neighbour never moves that neighbour. Only one fader can be active at
 * a time.
 *
 * @param id        Identifies this fader; must be unique and stable per frame.
 * @param box       Bounding box Clay recorded for the slot.
 * @param value     In and out, clamped to 0..max.
 * @param max       Top of the range.
 * @param on_change Invoked when the value changes. May be NULL.
 * @param user      Passed through to @p on_change.
 * @return true if the value moved.
 */
bool fader_interact(int id, Clay_BoundingBox box, int *value, int max,
                    fader_change_cb_t on_change, void *user);

/* Ids passed to fader_interact() and fader_draw() must be below this. */
#define FADER_MAX_IDS 8

/**
 * Draw one fader.
 *
 * The knob eases toward @p value rather than snapping to it, so a hardware
 * fader sending slower than the frame rate still reads as continuous motion.
 * One being dragged with the pointer is drawn exactly where the pointer is,
 * since easing there would feel like lag.
 *
 * @param id     Same id given to fader_interact().
 * @param label  Text set vertically inside the bottom of the track.
 * @param font   Font used for the label.
 * @param device True while the hardware fader is driving this one, which
 *               recolours it; pointer dragging leaves it in its normal colour.
 */
void fader_draw(int id, Clay_BoundingBox box, int value, int max,
                const char *label, void *font, bool device);

#endif /* FADER_H */
