/*
 * Native file chooser.
 *
 * Like tray.[ch], this is kept in its own translation unit: the Windows
 * implementation needs windows.h, whose Rectangle/CloseWindow/ShowCursor
 * collide with raylib.
 *
 * Windows uses the common dialogs. Elsewhere it shells out to zenity or
 * kdialog, whichever is installed; if neither is, the call reports failure and
 * the caller should fall back to a fixed path.
 */

#ifndef FILEDIALOG_H
#define FILEDIALOG_H

#include <stdbool.h>
#include <stddef.h>

/**
 * Ask for an existing file to open.
 *
 * @param title Dialog caption.
 * @param out   Receives the chosen path.
 * @param cap   Size of @p out.
 * @return false if the user cancelled or no chooser is available.
 */
bool filedialog_open(const char *title, char *out, size_t cap);

/**
 * Ask where to write a file.
 *
 * @param suggested Filename offered by default, may be NULL.
 */
bool filedialog_save(const char *title, const char *suggested,
                     char *out, size_t cap);

bool filedialog_available(void);

#endif /* FILEDIALOG_H */
