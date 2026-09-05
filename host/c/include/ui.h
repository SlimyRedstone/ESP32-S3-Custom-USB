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
 * The heights the layout was designed around, quoted for a full-size fader
 * track. The window no longer opens at these -- it opens at the minimum below
 * -- but the difference between them and FADER_TRACK_HEIGHT is what everything
 * other than the strip needs, which is how ui_fader_height() shares out the
 * real window. The traffic console only exists when "debug" is set, hence two.
 */
#define UI_WINDOW_HEIGHT_DEBUG  777
#define UI_WINDOW_HEIGHT_PLAIN  630

#define UI_WINDOW_HEIGHT_FOR(debug)     ((debug) ? UI_WINDOW_HEIGHT_DEBUG : UI_WINDOW_HEIGHT_PLAIN)

/* The interface runs at the monitor's refresh rate. Used only when the driver
   does not report one. */
#define UI_FALLBACK_FPS   60

/* How often the bus is swept for a device while none is connected. */
#define UI_CONNECT_SCAN_SECONDS 5.0

/* Minimum the layout stays usable at, and the size it opens at. */
#define UI_WINDOW_MIN_WIDTH   850
#define UI_WINDOW_MIN_HEIGHT  550

/**
 * Open the window, run the event loop until it is closed, then clean up.
 * Returns the process exit status.
 */
int ui_run(app_t *app);

#endif /* UI_H */
