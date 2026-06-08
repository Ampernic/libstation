/* SPDX-License-Identifier: LGPL-3.0-or-later */
#pragma once

#if !defined(STATION_INSIDE) && !defined(STATION_COMPILATION)
# error "Only <station.h> can be included directly."
#endif

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/**
 * StationOs:
 * @STATION_OS_OTHER: an unrecognised platform
 * @STATION_OS_LINUX: Linux (or another freedesktop.org desktop)
 * @STATION_OS_WINDOWS: Microsoft Windows
 * @STATION_OS_MACOS: Apple macOS
 * @STATION_OS_ANDROID: Google Android
 *
 * The host platform libstation was built for.
 */
typedef enum
{
  STATION_OS_OTHER,
  STATION_OS_LINUX,
  STATION_OS_WINDOWS,
  STATION_OS_MACOS,
  STATION_OS_ANDROID,
} StationOs;

/**
 * station_get_os:
 *
 * Returns: the platform this build of libstation targets.
 */
StationOs    station_get_os (void);

/**
 * station_os_to_string:
 * @os: a #StationOs
 *
 * Returns: (transfer none): a stable lowercase id ("linux", "windows", …).
 */
const char  *station_os_to_string (StationOs os);

/**
 * station_open_uri:
 * @uri: the URI to open (http(s), file, or a custom scheme like tg://)
 * @error: (nullable): return location for a #GError
 *
 * Opens @uri with the system's default handler, using the native mechanism for
 * the platform (ShellExecute, `open`, the default GAppInfo, …) — GIO alone does
 * not resolve custom schemes everywhere.
 *
 * Returns: %TRUE on success.
 */
gboolean     station_open_uri (const char *uri,
                               GError    **error);

/**
 * station_get_executable_path:
 *
 * Returns: (transfer full) (nullable): the absolute path of the running
 *   executable, or %NULL if it can't be determined. Free with g_free().
 */
char        *station_get_executable_path (void);

/**
 * station_get_pid:
 *
 * Returns: the numeric id of the current process (GetCurrentProcessId on
 *   Windows, getpid() elsewhere). Useful for pid-file based lifecycle where the
 *   native process-id, not GLib's #GPid handle, is needed.
 */
gint64       station_get_pid (void);

G_END_DECLS
