#include "filedialog.h"

#include <stdio.h>
#include <string.h>

/* Strip the last path component in place, leaving the directory. */
static void keep_directory(char *path)
{
    char *cut = NULL;

    for (char *p = path; *p; p++) {
        if (*p == '/' || *p == 0x5C) {   /* forward slash or backslash */
            cut = p;
        }
    }
    if (cut) {
        *cut = 0;
    }
}

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

static const char PROGRAM_FILTER[] =
    "Programs\0*.exe;*.com;*.bat\0"
    "All files\0*.*\0";

/* Directory holding the executable, so the dialogs start beside config.json
   rather than wherever the shell last left off. */
static bool program_directory(char *out, size_t cap)
{
    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)cap);
    if (n == 0 || n >= cap) {
        out[0] = 0;
        return false;
    }
    keep_directory(out);
    return out[0] != 0;
}

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

    char start[MAX_PATH];
    OPENFILENAMEA ofn;
    common_fields(&ofn, title, out, cap);
    if (program_directory(start, sizeof(start))) {
        ofn.lpstrInitialDir = start;
    }
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

    char start[MAX_PATH];
    OPENFILENAMEA ofn;
    common_fields(&ofn, title, out, cap);
    if (program_directory(start, sizeof(start))) {
        ofn.lpstrInitialDir = start;
    }
    ofn.Flags |= OFN_OVERWRITEPROMPT;

    return GetSaveFileNameA(&ofn) ? true : false;
}

bool filedialog_open_program(const char *title, char *out, size_t cap)
{
    if (cap == 0) {
        return false;
    }
    out[0] = 0;

    OPENFILENAMEA ofn;
    common_fields(&ofn, title, out, cap);
    ofn.lpstrFilter = PROGRAM_FILTER;
    ofn.lpstrDefExt = "exe";
    ofn.Flags |= OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;

    return GetOpenFileNameA(&ofn) ? true : false;
}

bool filedialog_available(void)
{
    return true;
}

#else /* !_WIN32 */

#include <stdlib.h>
#include <unistd.h>

/* Directory holding the executable, so the dialogs start beside config.json
   rather than in the working directory. */
static bool program_directory(char *out, size_t cap)
{
    ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    if (n <= 0) {
        out[0] = 0;
        return false;
    }
    out[n] = 0;
    keep_directory(out);
    return out[0] != 0;
}

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

    char command[640];
    char start[512];

    if (!program_directory(start, sizeof(start))) {
        snprintf(start, sizeof(start), ".");
    }

    if (have("zenity")) {
        /* A trailing slash makes zenity treat the value as a directory. */
        snprintf(command, sizeof(command),
                 "zenity --file-selection --title='%s' --filename='%s/' "
                 "--file-filter='JSON | *.json' --file-filter='All | *' 2>/dev/null",
                 title, start);
        return run_chooser(command, out, cap);
    }

    if (have("kdialog")) {
        snprintf(command, sizeof(command),
                 "kdialog --getopenfilename '%s' '*.json|JSON files' "
                 "--title '%s' 2>/dev/null", start, title);
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
    char command[640];
    char start[512];

    if (!program_directory(start, sizeof(start))) {
        snprintf(start, sizeof(start), ".");
    }

    if (have("zenity")) {
        snprintf(command, sizeof(command),
                 "zenity --file-selection --save --confirm-overwrite "
                 "--filename='%s/%s' --title='%s' "
                 "--file-filter='JSON | *.json' 2>/dev/null",
                 start, name, title);
        return run_chooser(command, out, cap);
    }

    if (have("kdialog")) {
        snprintf(command, sizeof(command),
                 "kdialog --getsavefilename '%s/%s' '*.json|JSON files' "
                 "--title '%s' 2>/dev/null", start, name, title);
        return run_chooser(command, out, cap);
    }

    fprintf(stderr, "no file chooser found; install zenity or kdialog\n");
    return false;
}

#endif /* _WIN32 */
