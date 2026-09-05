/*
 * Clay + raylib front end, laid out to mirror host/web/index.html.
 *
 * Header with a connection badge, a NeoPixel card holding the colour wheel and
 * brightness, a message card, a config editor, and a traffic log across the
 * bottom.
 */

#ifndef UI_H
#define UI_H

#include "app.h"

/*
 * Starting size, and also the floor: the cards stop being readable below it.
 * The traffic console only exists when "debug" is set in config.json, so the
 * window is shorter without it.
 */
#define UI_WINDOW_WIDTH         1130
#define UI_WINDOW_HEIGHT_DEBUG  777
#define UI_WINDOW_HEIGHT_PLAIN  630

#define UI_WINDOW_HEIGHT_FOR(debug)     ((debug) ? UI_WINDOW_HEIGHT_DEBUG : UI_WINDOW_HEIGHT_PLAIN)

/* Floor for the frame rate when the monitor refreshes more slowly than this. */
/* The interface runs at the monitor's refresh rate. Used only when the driver
   does not report one. */
#define UI_FALLBACK_FPS   60

/* Minimum the layout stays usable at. */
#define UI_WINDOW_MIN_WIDTH   850
#define UI_WINDOW_MIN_HEIGHT  550

/**
 * Open the window, run the event loop until it is closed, then clean up.
 * Returns the process exit status.
 */
int ui_run(app_t *app);

#endif /* UI_H */
