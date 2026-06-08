/* SPDX-License-Identifier: LGPL-3.0-or-later */
#pragma once

#if !defined(STATION_INSIDE) && !defined(STATION_COMPILATION)
# error "Only <station.h> can be included directly."
#endif

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/* A line-delimited local control channel between a background process (server)
 * and front-ends (clients). Transport is native per OS: a Unix socket on
 * Linux/macOS, a loopback TCP port + auth token on Windows. The protocol on top
 * (e.g. JSON status/commands) is the caller's; libstation just moves lines.
 *
 * The endpoint is derived from a @name (use your app id): a <name>.sock under the
 * user runtime dir on POSIX, or a <name>.control endpoint file under the user
 * config dir on Windows. */

#define STATION_TYPE_CONTROL_SERVER (station_control_server_get_type ())
G_DECLARE_FINAL_TYPE (StationControlServer, station_control_server,
                      STATION, CONTROL_SERVER, GObject)

StationControlServer *station_control_server_new       (const char *name);
gboolean              station_control_server_start     (StationControlServer *self,
                                                        GError              **error);
void                  station_control_server_stop      (StationControlServer *self);
/* Send a line to every connected client. */
void                  station_control_server_broadcast (StationControlServer *self,
                                                        const char           *line);
/* Signal "command" (const char *line): a line received from any client. */

#define STATION_TYPE_CONTROL_CLIENT (station_control_client_get_type ())
G_DECLARE_FINAL_TYPE (StationControlClient, station_control_client,
                      STATION, CONTROL_CLIENT, GObject)

StationControlClient *station_control_client_new   (const char *name);
/* Begin connecting (and auto-reconnecting) to the server. */
void                  station_control_client_start (StationControlClient *self);
void                  station_control_client_stop  (StationControlClient *self);
/* Send a line to the server (no-op if not connected). */
void                  station_control_client_send  (StationControlClient *self,
                                                    const char           *line);
/* Signals: "message" (const char *line), "connected" (gboolean connected). */

G_END_DECLS
