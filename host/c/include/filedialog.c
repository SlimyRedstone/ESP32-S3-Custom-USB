#include "filedialog.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32

#include <windows.h>
#include <commdlg.h>

/*
 * Two filters, JSON first. The list is a run of NUL-terminated pairs closed by
 * an empty string, which is why it cannot be written as a plain literal.
 */
static const char FILTER[] =
    "JSON files\0*.json\0"
    "All files\0*.*\0";

static void common_fields(OPENFILENAMEA *ofn, const char *title,
                          char *out, size_t cap)
{
    ZeroMemory(ofn, sizeof(*ofn));
    ofn->lStructSize = sizeof(*ofn);
    ofn->hwndOwner = GetActiveWindow();
    ofn->lpstrFilter = FILTER;
    ofn->nFilterIndex = 1;
    ofn->lpstrFile = out;
    ofn->nMaxFile = (DWORD)cap;
    ofn->lpstrTitle = title;
    ofn->lpstrDefExt = "json";

    /*
     * OFN_NOCHANGEDIR matters: without it the dialog leaves the process in
     * whichever directory the user browsed to, and the interface then fails to
     * find resources/ on the next font or icon load.
     */
    ofn->Flags = OFN_NOCHANGEDIR | OFN_EXPLORER;
}

bool filedialog_open(const char *title, char *out, size_t cap)
{
    if (cap == 0) {
        return false;
    }
    out[0] = '\0';

    OPENFILENAMEA ofn;
    common_fields(&ofn, title, out, cap);
    ofn.Flags |= OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

    return GetOpenFileNameA(&ofn) ? true : false;
}

bool filedialog_save(const char *title, const char *suggested,
                     char *out, size_t cap)
{
    if (cap == 0) {
        return false;
    }
    snprintf(out, cap, "%s", suggested ? suggested : "");

    OPENFILENAMEA ofn;
    common_fields(&ofn, title, out, cap);
    ofn.Flags |= OFN_OVERWRITEPROMPT;

    return GetSaveFileNameA(&ofn) ? true : false;
}

bool filedialog_available(void)
{
    return true;
}

#else /* !_WIN32 */

#include <stdlib.h>

/* Ask the shell whether a chooser exists, quietly. */
static bool have(const char *tool)
{
    char probe[64];
    snprintf(probe, sizeof(probe), "command -v %s >/dev/null 2>&1", tool);
    return system(probe) == 0;
}

/* Run a chooser and read the single path it prints. */
static bool run_chooser(const char *command, char *out, size_t cap)
{
    FILE *pipe = popen(command, "r");
    if (pipe == NULL) {
        return false;
    }

    bool ok = (fgets(out, (int)cap, pipe) != NULL);
    int status = pclose(pipe);

    if (!ok || status != 0) {
        out[0] = '\0';
        return false;
    }

    out[strcspn(out, "\n")] = '\0';
    return out[0] != '\0';
}

bool filedialog_available(void)
{
    return have("zenity") || have("kdialog");
}

bool filedialog_open(const char *title, char *out, size_t cap)
{
    if (cap == 0) {
        return false;
    }
    out[0] = '\0';

    char command[512];

    if (have("zenity")) {
        snprintf(command, sizeof(command),
                 "zenity --file-selection --title='%s' "
                 "--file-filter='JSON | *.json' --file-filter='All | *' 2>/dev/null",
                 title);
        return run_chooser(command, out, cap);
    }

    if (have("kdialog")) {
        snprintf(command, sizeof(command),
                 "kdialog --getopenfilename . '*.json|JSON files' "
                 "--title '%s' 2>/dev/null", title);
        return run_chooser(command, out, cap);
    }

    fprintf(stderr, "no file chooser found; install zenity or kdialog\n");
    return false;
}

bool filedialog_save(const char *title, const char *suggested,
                     char *out, size_t cap)
{
    if (cap == 0) {
        return false;
    }
    out[0] = '\0';

    const char *name = suggested ? suggested : "config.json";
    char command[512];

    if (have("zenity")) {
        snprintf(command, sizeof(command),
                 "zenity --file-selection --save --confirm-overwrite "
                 "--filename='%s' --title='%s' "
                 "--file-filter='JSON | *.json' 2>/dev/null",
                 name, title);
        return run_chooser(command, out, cap);
    }

    if (have("kdialog")) {
        snprintf(command, sizeof(command),
                 "kdialog --getsavefilename '%s' '*.json|JSON files' "
                 "--title '%s' 2>/dev/null", name, title);
        return run_chooser(command, out, cap);
    }

    fprintf(stderr, "no file chooser found; install zenity or kdialog\n");
    return false;
}

#endif /* _WIN32 */
