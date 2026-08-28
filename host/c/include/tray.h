/*
 * Window icon and system tray.
 *
 * Implemented against the platform shell, so this header deliberately exposes
 * no windows.h and no raylib types: those two cannot share a translation unit,
 * because windows.h defines Rectangle, CloseWindow and ShowCursor.
 *
 * The window handle crosses the boundary as an opaque pointer, which is what
 * raylib's GetWindowHandle() returns.
 *
 * Windows is implemented. Elsewhere every call is a no-op and tray_available()
 * reports false, because a Linux tray needs a StatusNotifier/AppIndicator
 * connection rather than a handful of shell calls.
 */

#ifndef TRAY_H
#define TRAY_H

#include <stdbool.h>

/**
 * Attach to the window and load @p icon_path as its title-bar and taskbar icon.
 *
 * @param window_handle Native handle, from raylib's GetWindowHandle().
 * @param icon_path     .ico file, relative to the working directory.
 * @param tooltip       Text shown when hovering the tray icon.
 * @return false if the platform has no tray support, or setup failed.
 */
bool tray_init(void *window_handle, const char *icon_path, const char *tooltip);

/** Release the tray icon. Safe to call when tray_init() failed. */
void tray_shutdown(void);

/** True when the platform supports the tray and setup succeeded. */
bool tray_available(void);

/** Hide the window and place an icon in the notification area. */
void tray_minimize(void);

/** Bring the window back and remove the tray icon. */
void tray_restore(void);

/** True while the window is hidden in the tray. */
bool tray_is_minimized(void);

/**
 * Show a balloon notification from the tray icon.
 *
 * Only meaningful while the window is hidden; ignored otherwise, and on
 * platforms without tray support.
 */
void tray_notify(const char *title, const char *text);

/**
 * Service the tray's message queue. Call once per frame.
 *
 * @return true if the user asked to quit from the tray menu.
 */
bool tray_poll(void);

#endif /* TRAY_H */
