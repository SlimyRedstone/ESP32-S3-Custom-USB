#include "tray.h"

#ifdef _WIN32

#include <windows.h>
#include <shellapi.h>
#include <stdio.h>

#define TRAY_MESSAGE   (WM_APP + 1)
#define TRAY_ICON_ID   1

#define MENU_SHOW      100
#define MENU_QUIT      101

#define TRAY_TITLE     "IOMeeter"

static HWND  s_app_window;      /* raylib's window */
static HWND  s_helper;          /* message-only window that owns the tray icon */
static HICON s_icon;
static NOTIFYICONDATAA s_nid;

static bool s_ready;
static bool s_minimized;
static bool s_quit_requested;

/* GLFW's own window procedure, chained to for everything we do not handle. */
static WNDPROC s_original_proc;

/*
 * The tray icon needs a window to deliver its notifications to, and raylib's
 * window belongs to GLFW. Subclassing that would mean intercepting GLFW's own
 * message handling, so a private message-only window is used instead: it costs
 * nothing and keeps the two event loops apart.
 */
static LRESULT CALLBACK tray_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (msg != TRAY_MESSAGE) {
        if (msg == WM_COMMAND) {
            switch (LOWORD(wparam)) {
            case MENU_SHOW: tray_restore(); return 0;
            case MENU_QUIT: s_quit_requested = true; return 0;
            default: break;
            }
        }
        return DefWindowProcA(hwnd, msg, wparam, lparam);
    }

    switch (LOWORD(lparam)) {
    case WM_LBUTTONDBLCLK:
    case WM_LBUTTONUP:
        tray_restore();
        break;

    case WM_RBUTTONUP: {
        HMENU menu = CreatePopupMenu();
        if (menu == NULL) {
            break;
        }
        AppendMenuA(menu, MF_STRING, MENU_SHOW, "Show " TRAY_TITLE);
        AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuA(menu, MF_STRING, MENU_QUIT, "Close " TRAY_TITLE);

        POINT at;
        GetCursorPos(&at);

        /* Required so the menu closes when focus moves elsewhere. */
        SetForegroundWindow(hwnd);
        TrackPopupMenu(menu, TPM_RIGHTBUTTON, at.x, at.y, 0, hwnd, NULL);
        PostMessageA(hwnd, WM_NULL, 0, 0);
        DestroyMenu(menu);
        break;
    }

    default:
        break;
    }
    return 0;
}

/*
 * The close button must not end the program: it hides to the notification area
 * instead, and only the tray menu really quits.
 *
 * raylib offers no way to clear the close flag once GLFW has set it, so WM_CLOSE
 * is intercepted before GLFW ever sees it.
 */
static LRESULT CALLBACK app_window_proc(HWND hwnd, UINT msg, WPARAM wparam,
                                        LPARAM lparam)
{
    if (msg == WM_CLOSE && s_ready) {
        tray_minimize();
        return 0;
    }
    return CallWindowProcA(s_original_proc, hwnd, msg, wparam, lparam);
}

bool tray_init(void *window_handle, const char *icon_path, const char *tooltip)
{
    s_app_window = (HWND)window_handle;

    /* LR_LOADFROMFILE keeps this independent of the executable's resources,
       so the .ico simply ships beside the binary. */
    s_icon = (HICON)LoadImageA(NULL, icon_path, IMAGE_ICON, 0, 0,
                               LR_LOADFROMFILE | LR_DEFAULTSIZE | LR_SHARED);
    if (s_icon == NULL) {
        fprintf(stderr, "tray: could not load %s (%lu)\n",
                icon_path, (unsigned long)GetLastError());
    }

    if (s_icon && s_app_window) {
        SendMessageA(s_app_window, WM_SETICON, ICON_SMALL, (LPARAM)s_icon);
        SendMessageA(s_app_window, WM_SETICON, ICON_BIG, (LPARAM)s_icon);
    }

    WNDCLASSEXA wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = tray_wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "IOMeeterTray";

    if (RegisterClassExA(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    s_helper = CreateWindowExA(0, wc.lpszClassName, "", 0, 0, 0, 0, 0,
                               HWND_MESSAGE, NULL, wc.hInstance, NULL);
    if (s_helper == NULL) {
        return false;
    }

    ZeroMemory(&s_nid, sizeof(s_nid));
    s_nid.cbSize = sizeof(s_nid);
    s_nid.hWnd = s_helper;
    s_nid.uID = TRAY_ICON_ID;
    s_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    s_nid.uCallbackMessage = TRAY_MESSAGE;
    s_nid.hIcon = s_icon ? s_icon : LoadIconA(NULL, IDI_APPLICATION);
    snprintf(s_nid.szTip, sizeof(s_nid.szTip), "%s", tooltip ? tooltip : "");

    if (!Shell_NotifyIconA(NIM_ADD, &s_nid)) {
        DestroyWindow(s_helper);
        s_helper = NULL;
        return false;
    }

    if (s_app_window) {
        s_original_proc = (WNDPROC)SetWindowLongPtrA(s_app_window, GWLP_WNDPROC,
                                                     (LONG_PTR)app_window_proc);
    }

    s_ready = true;
    return true;
}

void tray_shutdown(void)
{
    if (s_app_window && s_original_proc) {
        SetWindowLongPtrA(s_app_window, GWLP_WNDPROC, (LONG_PTR)s_original_proc);
        s_original_proc = NULL;
    }
    if (s_ready) {
        Shell_NotifyIconA(NIM_DELETE, &s_nid);
    }
    s_minimized = false;

    if (s_helper) {
        DestroyWindow(s_helper);
        s_helper = NULL;
    }
    s_ready = false;
}

bool tray_available(void)
{
    return s_ready;
}

void tray_minimize(void)
{
    if (!s_ready || s_minimized) {
        return;
    }

    /* The icon is already registered, so this only hides the window. */
    ShowWindow(s_app_window, SW_HIDE);
    s_minimized = true;
}

void tray_restore(void)
{
    if (!s_minimized) {
        return;
    }
    s_minimized = false;

    ShowWindow(s_app_window, SW_SHOW);
    ShowWindow(s_app_window, SW_RESTORE);
    SetForegroundWindow(s_app_window);
}

bool tray_is_minimized(void)
{
    return s_minimized;
}

void tray_notify(const char *title, const char *text)
{
    /* The balloon hangs off the tray icon, so there is nothing to show it from
       unless the icon is currently registered. */
    if (!s_ready || !s_minimized) {
        return;
    }

    NOTIFYICONDATAA balloon = s_nid;
    balloon.uFlags = NIF_INFO;
    balloon.dwInfoFlags = NIIF_INFO;
    snprintf(balloon.szInfoTitle, sizeof(balloon.szInfoTitle), "%s",
             title ? title : "");
    snprintf(balloon.szInfo, sizeof(balloon.szInfo), "%s", text ? text : "");

    Shell_NotifyIconA(NIM_MODIFY, &balloon);
}

bool tray_poll(void)
{
    if (!s_ready) {
        return false;
    }

    /* Our helper window is not pumped by GLFW, so drain it here. */
    MSG msg;
    while (PeekMessageA(&msg, s_helper, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    bool quit = s_quit_requested;
    s_quit_requested = false;
    return quit;
}

#else /* !_WIN32 */

/*
 * No tray here. A Linux implementation would speak StatusNotifierItem over
 * D-Bus, or fall back to libappindicator, neither of which belongs in a file
 * this size. The window icon is set by the UI through raylib instead.
 */

bool tray_init(void *window_handle, const char *icon_path, const char *tooltip)
{
    (void)window_handle;
    (void)icon_path;
    (void)tooltip;
    return false;
}

void tray_shutdown(void)      {}
void tray_notify(const char *title, const char *text) { (void)title; (void)text; }
bool tray_available(void)     { return false; }
void tray_minimize(void)      {}
void tray_restore(void)       {}
bool tray_is_minimized(void)  { return false; }
bool tray_poll(void)          { return false; }

#endif /* _WIN32 */
