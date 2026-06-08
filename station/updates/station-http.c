/* SPDX-License-Identifier: LGPL-3.0-or-later */
#ifndef STATION_COMPILATION
# define STATION_COMPILATION
#endif
#define G_LOG_DOMAIN "Station-Http"
#include "station-http-private.h"

/* The GIO TLS transport is desktop-only; the Android APK ships no glib-networking
 * and uses the java.net backend instead, so this whole module is empty there. */
#ifndef __ANDROID__

#include <string.h>
#include <stdlib.h>

/* Split "https://host[:port]/path"; only https. */
static gboolean
parse_https_url (const char *url, char **host, guint16 *port, char **path)
{
  if (!g_str_has_prefix (url, "https://"))
    return FALSE;
  const char *p = url + 8;
  const char *slash = strchr (p, '/');
  const char *hostend = slash ? slash : p + strlen (p);
  const char *colon = memchr (p, ':', (gsize) (hostend - p));
  *port = 443;
  if (colon != NULL)
    {
      *host = g_strndup (p, (gsize) (colon - p));
      *port = (guint16) atoi (colon + 1);
    }
  else
    *host = g_strndup (p, (gsize) (hostend - p));
  *path = g_strdup (slash ? slash : "/");
  return **host != '\0';
}

/* On Windows the GIO TLS backend (gnutls) loads but has no system CA store, so
 * verification fails with "Unacceptable TLS certificate". The app exports
 * SSL_CERT_FILE pointing at the bundled CA bundle; reuse it here as the TLS
 * connection's trust database at handshake time. Where SSL_CERT_FILE is unset
 * (Linux/macOS) the backend's system trust is used. */
static void
apply_ca_file (GSocketClient *client, GSocketClientEvent ev,
               GSocketConnectable *connectable, GIOStream *connection,
               gpointer user_data)
{
  (void) client; (void) connectable;
  if (ev == G_SOCKET_CLIENT_TLS_HANDSHAKING && G_IS_TLS_CONNECTION (connection))
    g_tls_connection_set_database (G_TLS_CONNECTION (connection),
                                   G_TLS_DATABASE (user_data));
}

static GSocketClient *
make_tls_client (guint timeout)
{
  GSocketClient *client = g_socket_client_new ();
  g_socket_client_set_tls (client, TRUE);
  g_socket_client_set_timeout (client, timeout);

  const char *ca_file = g_getenv ("SSL_CERT_FILE");
  if (ca_file != NULL && *ca_file != '\0')
    {
      GTlsDatabase *ca_db = g_tls_file_database_new (ca_file, NULL);
      if (ca_db != NULL)
        {
          /* Tie the database's lifetime to the client so every return path frees
           * it; the handler holds only a borrowed pointer. */
          g_signal_connect (client, "event", G_CALLBACK (apply_ca_file), ca_db);
          g_object_set_data_full (G_OBJECT (client), "station-ca-db", ca_db,
                                  g_object_unref);
        }
    }
  return client;
}

/* One HTTP/1.0 GET of @url. On 2xx, streams the body to @out (respecting
 * @max_bytes when > 0) and reports progress; *redirect stays NULL. On 3xx, sets
 * *redirect to the Location. Otherwise FALSE + @error. */
static gboolean
http_get_once (const char *url, GOutputStream *out,
               StationHttpProgress progress, gpointer pdata, gint64 max_bytes,
               char **redirect, GCancellable *cancel, GError **error)
{
  *redirect = NULL;
  char *host = NULL, *path = NULL;
  guint16 port = 443;
  if (!parse_https_url (url, &host, &port, &path))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "bad URL");
      return FALSE;
    }

  GSocketClient *client = make_tls_client (30);
  GSocketConnection *conn = g_socket_client_connect_to_host (client, host, port, cancel, error);
  if (conn == NULL)
    { g_object_unref (client); g_free (host); g_free (path); return FALSE; }

  char *req = g_strdup_printf (
      "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: libstation\r\n"
      "Accept: application/json, */*\r\nConnection: close\r\n\r\n", path, host);
  GOutputStream *os = g_io_stream_get_output_stream (G_IO_STREAM (conn));
  gboolean wrote = g_output_stream_write_all (os, req, strlen (req), NULL, cancel, error);
  g_free (req);
  if (!wrote)
    { g_object_unref (conn); g_object_unref (client); g_free (host); g_free (path); return FALSE; }

  GInputStream *is = g_io_stream_get_input_stream (G_IO_STREAM (conn));

  /* Read the header block; any body bytes trailing the blank line are kept. */
  GString *head = g_string_new (NULL);
  char body0[65536];
  gsize body0_len = 0;
  guint8 tmp[8192];
  gboolean have_sep = FALSE;
  gssize n = 0;
  while (!have_sep && (n = g_input_stream_read (is, tmp, sizeof tmp, cancel, error)) > 0)
    {
      g_string_append_len (head, (char *) tmp, n);
      const char *sep = g_strstr_len (head->str, head->len, "\r\n\r\n");
      if (sep != NULL)
        {
          have_sep = TRUE;
          gsize hlen = (gsize) (sep - head->str) + 4;
          body0_len = MIN (head->len - hlen, sizeof body0);
          memcpy (body0, head->str + hlen, body0_len);
          g_string_truncate (head, hlen);
        }
    }
  if (!have_sep)
    {
      if (error != NULL && *error == NULL)
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "no HTTP headers");
      g_string_free (head, TRUE);
      g_object_unref (conn); g_object_unref (client); g_free (host); g_free (path);
      return FALSE;
    }

  int status = (head->len > 12 && strncmp (head->str, "HTTP/1.", 7) == 0)
                 ? atoi (head->str + 9) : 0;
  char *lc = g_ascii_strdown (head->str, head->len);

  if (status >= 300 && status < 400)
    {
      const char *loc = strstr (lc, "\r\nlocation:");
      if (loc != NULL)
        {
          const char *val = head->str + (loc - lc) + 11;
          while (*val == ' ') val++;
          const char *eol = strstr (val, "\r\n");
          *redirect = g_strndup (val, eol ? (gsize) (eol - val) : strlen (val));
        }
      g_free (lc);
      g_string_free (head, TRUE);
      g_object_unref (conn); g_object_unref (client); g_free (host); g_free (path);
      if (*redirect == NULL)
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "redirect without Location");
      return *redirect != NULL;
    }
  if (status != 200)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "HTTP status %d", status);
      g_free (lc);
      g_string_free (head, TRUE);
      g_object_unref (conn); g_object_unref (client); g_free (host); g_free (path);
      return FALSE;
    }

  gint64 total = -1;
  const char *cl = strstr (lc, "\r\ncontent-length:");
  if (cl != NULL) total = g_ascii_strtoll (head->str + (cl - lc) + 17, NULL, 10);
  g_free (lc);
  g_string_free (head, TRUE);

  gboolean ok = TRUE;
  gint64 got = 0, last_report = 0;
  if (body0_len > 0)
    { ok = g_output_stream_write_all (out, body0, body0_len, NULL, cancel, error); got += body0_len; }
  while (ok && (n = g_input_stream_read (is, tmp, sizeof tmp, cancel, error)) > 0)
    {
      if (max_bytes > 0 && got + n > max_bytes)
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "response too large");
          ok = FALSE;
          break;
        }
      ok = g_output_stream_write_all (out, tmp, n, NULL, cancel, error);
      got += n;
      if (progress != NULL && got - last_report >= 65536)
        { progress (pdata, got, total); last_report = got; }
    }
  if (ok && n < 0) ok = FALSE;
  if (ok && progress != NULL) progress (pdata, got, total > 0 ? total : got);

  g_object_unref (conn); g_object_unref (client); g_free (host); g_free (path);
  return ok;
}

/* Follow up to 6 redirects of @url, streaming each successful body to @out. */
static gboolean
http_get_follow (const char *url, GOutputStream *out,
                 StationHttpProgress progress, gpointer pdata, gint64 max_bytes,
                 GCancellable *cancel, GError **error)
{
  char *cur = g_strdup (url);
  for (int hop = 0; hop < 6; hop++)
    {
      char *redirect = NULL;
      gboolean ok = http_get_once (cur, out, progress, pdata, max_bytes,
                                   &redirect, cancel, error);
      if (!ok || redirect == NULL) { g_free (redirect); g_free (cur); return ok; }
      g_free (cur);
      cur = redirect;
    }
  g_free (cur);
  if (error != NULL && *error == NULL)
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "too many redirects");
  return FALSE;
}

gboolean
station_http_get_to_stream (const char *url, GOutputStream *out,
                            StationHttpProgress progress, gpointer progress_data,
                            GCancellable *cancel, GError **error)
{
  g_return_val_if_fail (url != NULL && out != NULL, FALSE);
  return http_get_follow (url, out, progress, progress_data, -1, cancel, error);
}

GBytes *
station_http_get_bytes (const char *url, gsize max_bytes,
                        GCancellable *cancel, GError **error)
{
  g_return_val_if_fail (url != NULL, NULL);
  GMemoryOutputStream *mem = G_MEMORY_OUTPUT_STREAM (g_memory_output_stream_new_resizable ());
  gboolean ok = http_get_follow (url, G_OUTPUT_STREAM (mem), NULL, NULL,
                                 (gint64) max_bytes, cancel, error);
  if (!ok)
    { g_object_unref (mem); return NULL; }
  g_output_stream_close (G_OUTPUT_STREAM (mem), NULL, NULL);
  GBytes *body = g_memory_output_stream_steal_as_bytes (mem);
  g_object_unref (mem);
  return body;
}

#endif /* !__ANDROID__ */
