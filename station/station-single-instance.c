/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef STATION_COMPILATION
# define STATION_COMPILATION
#endif
#define G_LOG_DOMAIN "Station"
#include "station-single-instance.h"

#ifdef G_OS_WIN32
#include <windows.h>

static StationActivateFunc g_cb = NULL;
static UINT                g_msg = 0;

static gboolean
invoke_cb (gpointer d)
{
  (void) d;
  if (g_cb != NULL)
    g_cb ();
  return G_SOURCE_REMOVE;
}

/* Runs during GTK's Win32 message pump (the GDK event source dispatches it), so
 * marshal onto an idle to reach the GLib loop cleanly. */
static LRESULT CALLBACK
wndproc (HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
  if (g_msg != 0 && msg == g_msg)
    {
      g_idle_add (invoke_cb, NULL);
      return 0;
    }
  return DefWindowProcW (hwnd, msg, wp, lp);
}

gboolean
station_single_instance_acquire (const char *app_id, StationActivateFunc on_activate)
{
  g_return_val_if_fail (app_id != NULL, TRUE);

  char *name = g_strconcat ("station-si-", app_id, NULL);
  wchar_t *wname = (wchar_t *) g_utf8_to_utf16 (name, -1, NULL, NULL, NULL);
  g_free (name);
  if (wname == NULL)
    return TRUE;   /* can't name the lock → don't block startup */

  /* Same string → same message id and lock name in every process. */
  g_msg = RegisterWindowMessageW (wname);
  HANDLE mutex = CreateMutexW (NULL, TRUE, wname);
  gboolean already = (mutex != NULL && GetLastError () == ERROR_ALREADY_EXISTS);

  if (already)
    {
      /* A primary is running: find its hidden window (class == @wname) and ask it
       * to come forward, then tell the caller to bow out. */
      HWND prior = FindWindowW (wname, NULL);
      if (prior != NULL)
        PostMessageW (prior, g_msg, 0, 0);
      if (mutex != NULL)
        CloseHandle (mutex);
      g_free (wname);
      return FALSE;
    }

  /* Primary: keep the mutex for the process lifetime and stand up a hidden,
   * never-shown top-level window so later instances can find us by class. */
  g_cb = on_activate;

  WNDCLASSW wc = { 0 };
  wc.lpfnWndProc = wndproc;
  wc.hInstance = GetModuleHandleW (NULL);
  wc.lpszClassName = wname;   /* RegisterClassW copies it into the atom table */
  RegisterClassW (&wc);
  CreateWindowExW (0, wname, wname, WS_OVERLAPPED, 0, 0, 0, 0,
                   NULL, NULL, wc.hInstance, NULL);
  /* wname must outlive the class registration; leak it (one tiny string, lives as
   * long as the process, which is exactly the window/class lifetime). */
  return TRUE;
}

#else /* !G_OS_WIN32 */

gboolean
station_single_instance_acquire (const char *app_id, StationActivateFunc on_activate)
{
  (void) app_id;
  (void) on_activate;
  return TRUE;   /* GApplication already enforces uniqueness here */
}

#endif
