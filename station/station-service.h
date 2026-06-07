/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#if !defined(STATION_INSIDE) && !defined(STATION_COMPILATION)
# error "Only <station.h> can be included directly."
#endif

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/* Controls a background helper process ("the daemon") and its login autostart.
 *
 * Lifecycle uses a pid file (<app_id>.pid in the user runtime dir): start()
 * spawns @daemon_argv detached and records its pid; is_active() checks the pid is
 * alive; stop() signals it. The daemon should also call
 * station_service_mark_running()/station_service_clear() so it is detected when
 * launched by autostart rather than by start().
 *
 * Autostart is native per OS: an XDG autostart .desktop (Linux), a LaunchAgent
 * plist (macOS), or an HKCU\…\Run value (Windows). */

#define STATION_TYPE_SERVICE (station_service_get_type ())
G_DECLARE_FINAL_TYPE (StationService, station_service, STATION, SERVICE, GObject)

/**
 * station_service_new:
 * @app_id: reverse-DNS id, used for the pid file and autostart entry
 * @daemon_argv: (array zero-terminated=1): command that runs the background helper
 */
StationService *station_service_new (const char         *app_id,
                                     const char * const *daemon_argv);

gboolean station_service_start         (StationService *self, GError **error);
void     station_service_stop          (StationService *self);
gboolean station_service_is_active     (StationService *self);

gboolean station_service_set_autostart (StationService *self,
                                        gboolean        enabled,
                                        GError        **error);
gboolean station_service_get_autostart (StationService *self);

/* Daemon-side: call at startup / shutdown so the controller can find it. */
void station_service_mark_running (const char *app_id);
void station_service_clear        (const char *app_id);

G_END_DECLS
