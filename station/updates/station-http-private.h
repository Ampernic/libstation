/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Internal HTTP/1.0-over-GIO-TLS transport, shared by the update check (fetch a
 * JSON body) and the asset download (stream to a file with progress). It speaks
 * plain HTTP/1.0 + "Connection: close" over a GIO TLS socket, so there is no
 * HTTP-library dependency. The Android build ships its own java.net transport
 * (the GIO TLS backend is not in the APK), so this module is compiled out there.
 *
 * TLS trust: if SSL_CERT_FILE is set (the app points it at a bundled CA bundle on
 * Windows, where gnutls has no system store) it is used as the trust database;
 * otherwise the platform system trust applies. Not a public/installed header. */
#pragma once

#include <gio/gio.h>

G_BEGIN_DECLS

/* Download progress sink: @got bytes received of @total (@total < 0 = unknown). */
typedef void (*StationHttpProgress) (gpointer user_data, gint64 got, gint64 total);

/* GET @url over HTTPS, following a few redirects, streaming the response body to
 * @out and reporting progress. FALSE + @error on any non-2xx / network error. */
gboolean station_http_get_to_stream (const char *url, GOutputStream *out,
                                     StationHttpProgress progress, gpointer progress_data,
                                     GCancellable *cancel, GError **error);

/* GET @url over HTTPS (following redirects) and return the whole body as #GBytes,
 * refusing a body larger than @max_bytes. NULL + @error on failure. */
GBytes *station_http_get_bytes (const char *url, gsize max_bytes,
                                GCancellable *cancel, GError **error);

G_END_DECLS
