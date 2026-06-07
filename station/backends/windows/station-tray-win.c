/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Windows backend: Shell_NotifyIcon with a right-click menu built from the item
 * store. The tray lives in the GUI process (GDK pumps the thread's messages). */
#ifndef STATION_COMPILATION
# define STATION_COMPILATION
#endif
#ifndef UNICODE
# define UNICODE
#endif
#ifndef _UNICODE
# define _UNICODE
#endif
#include "station-tray-private.h"

#include <windows.h>
#include <shellapi.h>

#define TRAY_MSG (WM_APP + 1)

typedef struct
{
  HWND  hwnd;
  NOTIFYICONDATAW nid;
  HICON icon;
} Win;

static wchar_t *
to_w (const char *s)
{
  return (wchar_t *) g_utf8_to_utf16 (s ? s : "", -1, NULL, NULL, NULL);
}

static void
tray_enable_dark (HWND hwnd)
{
  HMODULE ux = LoadLibraryExW (L"uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (ux == NULL)
    return;
  typedef int (WINAPI * SetMode) (int);
  typedef BOOL (WINAPI * AllowWin) (HWND, BOOL);
  typedef void (WINAPI * Flush) (void);
  SetMode set_mode = (SetMode) (void *) GetProcAddress (ux, MAKEINTRESOURCEA (135));
  AllowWin allow = (AllowWin) (void *) GetProcAddress (ux, MAKEINTRESOURCEA (133));
  Flush flush = (Flush) (void *) GetProcAddress (ux, MAKEINTRESOURCEA (136));
  if (set_mode) set_mode (1);          /* AllowDark: follow the system theme */
  if (allow)    allow (hwnd, TRUE);
  if (flush)    flush ();
}

static void
tray_popup (StationTray *self)
{
  HMENU m = CreatePopupMenu ();
  guint n = _station_tray_item_count (self);
  for (guint i = 0; i < n; i++)
    {
      const StationTrayItem *it = _station_tray_item_at (self, i);
      if (it->id == 0)
        {
          AppendMenuW (m, MF_SEPARATOR, 0, NULL);
          continue;
        }
      wchar_t *w = to_w (it->label);
      AppendMenuW (m, MF_STRING | (it->enabled ? 0 : MF_GRAYED), it->id, w);
      g_free (w);
    }
  Win *win = self->backend;
  POINT p;
  GetCursorPos (&p);
  SetForegroundWindow (win->hwnd);   /* so the menu dismisses on click-away */
  int cmd = TrackPopupMenu (m, TPM_RIGHTBUTTON | TPM_RETURNCMD, p.x, p.y, 0,
                            win->hwnd, NULL);
  PostMessageW (win->hwnd, WM_NULL, 0, 0);
  DestroyMenu (m);
  if (cmd > 0)
    _station_tray_emit_activated (self, (guint) cmd);
}

static LRESULT CALLBACK
tray_wndproc (HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
  StationTray *self = (StationTray *) GetWindowLongPtrW (hwnd, GWLP_USERDATA);
  if (self != NULL && msg == TRAY_MSG)
    {
      if (LOWORD (lp) == WM_LBUTTONUP || LOWORD (lp) == WM_LBUTTONDBLCLK)
        _station_tray_emit_activate (self);
      else if (LOWORD (lp) == WM_RBUTTONUP || LOWORD (lp) == WM_CONTEXTMENU)
        tray_popup (self);
      return 0;
    }
  return DefWindowProcW (hwnd, msg, wp, lp);
}

static HICON
load_icon (StationTray *self)
{
  if (self->icon_file != NULL)
    {
      wchar_t *wf = to_w (self->icon_file);
      HICON ic = (HICON) LoadImageW (NULL, wf, IMAGE_ICON,
          GetSystemMetrics (SM_CXSMICON), GetSystemMetrics (SM_CYSMICON),
          LR_LOADFROMFILE | LR_DEFAULTSIZE);
      g_free (wf);
      if (ic != NULL)
        return ic;
    }
  /* Fall back to the exe's first embedded icon, then the generic app icon. */
  HICON ic = (HICON) LoadImageW (GetModuleHandleW (NULL), MAKEINTRESOURCEW (1),
      IMAGE_ICON, GetSystemMetrics (SM_CXSMICON), GetSystemMetrics (SM_CYSMICON),
      LR_DEFAULTCOLOR);
  return ic ? ic : LoadIconW (NULL, IDI_APPLICATION);
}

void
_station_tray_backend_init (StationTray *self)
{
  static const wchar_t *CLASS = L"StationTrayWindow";
  HINSTANCE inst = GetModuleHandleW (NULL);
  static gsize once = 0;
  if (g_once_init_enter (&once))
    {
      WNDCLASSEXW wc = { 0 };
      wc.cbSize = sizeof (wc);
      wc.lpfnWndProc = tray_wndproc;
      wc.hInstance = inst;
      wc.lpszClassName = CLASS;
      RegisterClassExW (&wc);
      g_once_init_leave (&once, 1);
    }

  Win *win = g_new0 (Win, 1);
  self->backend = win;
  win->hwnd = CreateWindowExW (0, CLASS, L"", 0, 0, 0, 0, 0,
                               HWND_MESSAGE, NULL, inst, NULL);
  if (win->hwnd == NULL)
    return;
  SetWindowLongPtrW (win->hwnd, GWLP_USERDATA, (LONG_PTR) self);
  tray_enable_dark (win->hwnd);

  win->icon = load_icon (self);
  win->nid.cbSize = sizeof (win->nid);
  win->nid.hWnd = win->hwnd;
  win->nid.uID = 1;
  win->nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  win->nid.uCallbackMessage = TRAY_MSG;
  win->nid.hIcon = win->icon;
  Shell_NotifyIconW (NIM_ADD, &win->nid);
}

void
_station_tray_backend_update (StationTray *self)
{
  Win *win = self->backend;
  if (win == NULL || win->hwnd == NULL)
    return;
  /* Refresh the icon + tooltip; the menu is rebuilt live on right-click. */
  HICON ic = load_icon (self);
  if (ic != NULL)
    {
      if (win->icon)
        DestroyIcon (win->icon);
      win->icon = ic;
      win->nid.hIcon = ic;
    }
  win->nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  wchar_t *tip = to_w (self->tooltip);
  wcsncpy (win->nid.szTip, tip, G_N_ELEMENTS (win->nid.szTip) - 1);
  win->nid.szTip[G_N_ELEMENTS (win->nid.szTip) - 1] = L'\0';
  g_free (tip);
  Shell_NotifyIconW (NIM_MODIFY, &win->nid);
}

void
_station_tray_backend_dispose (StationTray *self)
{
  Win *win = self->backend;
  if (win == NULL)
    return;
  if (win->hwnd != NULL)
    {
      Shell_NotifyIconW (NIM_DELETE, &win->nid);
      DestroyWindow (win->hwnd);
    }
  if (win->icon != NULL)
    DestroyIcon (win->icon);
  g_free (win);
  self->backend = NULL;
}
