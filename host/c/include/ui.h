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

/* Starting size, and also the floor: the cards stop being readable below it. */
#define UI_WINDOW_WIDTH   1000
#define UI_WINDOW_HEIGHT  600

/* Floor for the frame rate when the monitor refreshes more slowly than this. */
#define UI_MIN_FPS        90

/**
 * Open the window, run the event loop until it is closed, then clean up.
 * Returns the process exit status.
 */
int ui_run(app_t *app);

#endif /* UI_H */
