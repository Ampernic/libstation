/* SPDX-License-Identifier: LGPL-3.0-or-later */
#ifndef STATION_COMPILATION
# define STATION_COMPILATION
#endif
#include "station-platform.h"

#ifdef G_OS_WIN32
# ifndef UNICODE
#  define UNICODE
# endif
# include <windows.h>
# include <shellapi.h>
#else
# include <unistd.h>
# if defined(__APPLE__)
#  include <stdlib.h>      /* realpath, free */
#  include <mach-o/dyld.h>
#  include <limits.h>
# endif
#endif

StationOs
station_get_os (void)
{
#if defined(__ANDROID__)
  return STATION_OS_ANDROID;
#elif defined(G_OS_WIN32)
  return STATION_OS_WINDOWS;
#elif defined(__APPLE__)
  return STATION_OS_MACOS;
#elif defined(__linux__)
  return STATION_OS_LINUX;
#else
  return STATION_OS_OTHER;
#endif
}

const char *
station_os_to_string (StationOs os)
{
  switch (os)
    {
    case STATION_OS_LINUX:   return "linux";
    case STATION_OS_WINDOWS: return "windows";
    case STATION_OS_MACOS:   return "macos";
    case STATION_OS_ANDROID: return "android";
    case STATION_OS_OTHER:
    default:                 return "other";
    }
}

gboolean
station_open_uri (const char *uri,
                  GError    **error)
{
  g_return_val_if_fail (uri != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

#if defined(G_OS_WIN32)
  wchar_t *w = (wchar_t *) g_utf8_to_utf16 (uri, -1, NULL, NULL, error);
  if (w == NULL)
    return FALSE;
  HINSTANCE rc = ShellExecuteW (NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL);
  g_free (w);
  if ((INT_PTR) rc <= 32)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "ShellExecute failed (%ld) for %s", (long) (INT_PTR) rc, uri);
      return FALSE;
    }
  return TRUE;
#elif defined(__APPLE__)
  /* `open` resolves every scheme via Launch Services without linking Cocoa. */
  const char *argv[] = { "/usr/bin/open", uri, NULL };
  GSubprocess *p = g_subprocess_newv (argv, G_SUBPROCESS_FLAGS_NONE, error);
  if (p == NULL)
    return FALSE;
  g_object_unref (p);
  return TRUE;
#elif defined(__ANDROID__)
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                       "station_open_uri: Android backend not implemented yet");
  return FALSE;
#else
  /* Linux/other freedesktop: the default GAppInfo (xdg) handles registered
   * schemes, including custom ones with an installed .desktop handler. */
  return g_app_info_launch_default_for_uri (uri, NULL, error);
#endif
}

char *
station_get_executable_path (void)
{
#if defined(G_OS_WIN32)
  wchar_t buf[32768];
  DWORD n = GetModuleFileNameW (NULL, buf, G_N_ELEMENTS (buf));
  if (n == 0 || n >= G_N_ELEMENTS (buf))
    return NULL;
  return g_utf16_to_utf8 ((const gunichar2 *) buf, -1, NULL, NULL, NULL);
#elif defined(__APPLE__)
  char buf[PATH_MAX];
  uint32_t size = sizeof (buf);
  if (_NSGetExecutablePath (buf, &size) != 0)
    return NULL;
  /* realpath() returns malloc'd memory; the documented contract is "free with
   * g_free", so copy it into a g_free-able buffer and release the original. */
  char *real = realpath (buf, NULL);   /* resolve symlinks/.. */
  char *out = g_strdup (real ? real : buf);
  free (real);
  return out;
#elif defined(__linux__) && !defined(__ANDROID__)
  return g_file_read_link ("/proc/self/exe", NULL);
#else
  return NULL;   /* Android: /proc/self/exe is app_process, not useful */
#endif
}

gint64
station_get_pid (void)
{
#if defined(G_OS_WIN32)
  return (gint64) GetCurrentProcessId ();
#else
  return (gint64) getpid ();
#endif
}
