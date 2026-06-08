/* SPDX-License-Identifier: LGPL-3.0-or-later */
#ifndef STATION_COMPILATION
# define STATION_COMPILATION
#endif
#define G_LOG_DOMAIN "Station-Updates"
#include "station-updates.h"

#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <stdlib.h>

/* A selectable release channel: an id (e.g. "beta") and the prerelease labels it
 * accepts. A stable release qualifies for every channel; a prerelease qualifies
 * only where its tag's label (the part after '-', e.g. "beta" in "v2.1-beta3")
 * starts with one of the channel's labels. */
typedef struct
{
  char  *id;
  GStrv  labels;
} Channel;

struct _StationUpdates
{
  GObject       parent_instance;
  char         *repo;            /* "owner/name" */
  char         *current;         /* current version, no leading 'v' */
  char         *channel;         /* active channel id; "stable" = releases only */
  GPtrArray    *channels;        /* (element-type Channel), in registration order */
  GCancellable *cancel;
};

G_DEFINE_FINAL_TYPE (StationUpdates, station_updates, G_TYPE_OBJECT)

enum {
  SIG_AVAILABLE, SIG_UP_TO_DATE, SIG_FAILED,
  SIG_DL_PROGRESS, SIG_DOWNLOADED, SIG_DL_FAILED,
  N_SIGNALS
};
static guint signals[N_SIGNALS];

static void
emit_failed (StationUpdates *self, const char *reason)
{
  g_warning ("update check failed: %s", reason);
  g_signal_emit (self, signals[SIG_FAILED], 0, reason);
}

static void
channel_free (gpointer p)
{
  Channel *c = p;
  g_free (c->id);
  g_strfreev (c->labels);
  g_free (c);
}

static Channel *
find_channel (StationUpdates *self, const char *id)
{
  if (id == NULL)
    return NULL;
  for (guint i = 0; i < self->channels->len; i++)
    {
      Channel *c = g_ptr_array_index (self->channels, i);
      if (g_ascii_strcasecmp (c->id, id) == 0)
        return c;
    }
  return NULL;
}

/* The prerelease label of a version (no leading 'v'): the part after the first
 * '-', e.g. "beta3" for "2.1.0-beta3", or "" for a stable "2.1.0". */
static const char *
prerelease_label (const char *version)
{
  const char *d = strchr (version, '-');
  return d ? d + 1 : "";
}

/* Does a release qualify for the active channel? Stable releases always do;
 * a prerelease does only when its label matches the channel. An unregistered
 * active id is taken as a single accepted label, so set_channel("beta") works
 * without an explicit add_channel. */
static gboolean
channel_qualifies (StationUpdates *self, gboolean is_prerelease, const char *label)
{
  if (!is_prerelease || label[0] == '\0')
    return TRUE;
  const char *id = self->channel;
  if (id == NULL || *id == '\0' || g_ascii_strcasecmp (id, "stable") == 0)
    return FALSE;
  Channel *c = find_channel (self, id);
  if (c == NULL)
    return g_ascii_strncasecmp (label, id, strlen (id)) == 0;
  for (guint i = 0; c->labels != NULL && c->labels[i] != NULL; i++)
    if (*c->labels[i] != '\0'
        && g_ascii_strncasecmp (label, c->labels[i], strlen (c->labels[i])) == 0)
      return TRUE;
  return FALSE;
}

static const char *
strip_v (const char *s)
{
  return (s != NULL && (s[0] == 'v' || s[0] == 'V')) ? s + 1 : s;
}

/* Compare "MAJOR.MINOR.PATCH[-prerelease]": >0 if a is newer, 0 equal, <0 older.
 * A release outranks a prerelease of the same core; prereleases compare lexically. */
static int
cmp_version (const char *a, const char *b)
{
  char *ad = g_strdup (a), *bd = g_strdup (b);
  char *ap = strchr (ad, '-'), *bp = strchr (bd, '-');
  if (ap) *ap++ = '\0';
  if (bp) *bp++ = '\0';

  char **an = g_strsplit (ad, ".", -1);
  char **bn = g_strsplit (bd, ".", -1);
  guint n = MAX (g_strv_length (an), g_strv_length (bn));
  int result = 0;
  for (guint i = 0; i < n && result == 0; i++)
    {
      long av = (i < g_strv_length (an)) ? strtol (an[i], NULL, 10) : 0;
      long bv = (i < g_strv_length (bn)) ? strtol (bn[i], NULL, 10) : 0;
      if (av != bv)
        result = (av > bv) ? 1 : -1;
    }
  g_strfreev (an);
  g_strfreev (bn);
  if (result == 0)
    {
      gboolean a_pre = (ap && *ap), b_pre = (bp && *bp);
      /* "devel" (an untagged snapshot) is older than any tagged prerelease of the
       * same core, which is older than the release; so a -devel build is offered
       * every beta/rc, and a beta is offered the final. */
      gboolean a_dev = (a_pre && g_str_has_prefix (ap, "devel"));
      gboolean b_dev = (b_pre && g_str_has_prefix (bp, "devel"));
      if (a_pre && !b_pre)      result = -1;
      else if (!a_pre && b_pre) result = 1;
      else if (a_dev && !b_dev) result = -1;
      else if (!a_dev && b_dev) result = 1;
      else                      result = g_strcmp0 (ap ? ap : "", bp ? bp : "");
    }
  g_free (ad);
  g_free (bd);
  return result;
}

/* Parse the GitHub releases JSON body and emit "available" for the newest
 * applicable release that is newer than self->current. Runs on the main thread. */
static void
parse_and_emit (StationUpdates *self, const char *body, gsize len)
{
  JsonParser *parser = json_parser_new ();
  GError *err = NULL;
  if (!json_parser_load_from_data (parser, body, (gssize) len, &err))
    {
      char *r = g_strdup_printf ("malformed JSON: %s", err ? err->message : "?");
      emit_failed (self, r);
      g_free (r);
      g_clear_error (&err);
      g_object_unref (parser);
      return;
    }
  JsonNode *root = json_parser_get_root (parser);
  if (root == NULL || !JSON_NODE_HOLDS_ARRAY (root))
    {
      emit_failed (self, "unexpected response (not a releases array)");
      g_object_unref (parser);
      return;
    }
  JsonArray *arr = json_node_get_array (root);
  guint count = json_array_get_length (arr);

  /* Pick the highest-versioned release that qualifies for the active channel;
   * GitHub lists by publish time, which isn't version order, so scan them all.
   * The chosen strings point into the parser and stay valid until it is freed. */
  const char *best = NULL, *best_url = "", *best_notes = "";
  for (guint i = 0; i < count; i++)
    {
      JsonObject *o = json_array_get_object_element (arr, i);
      if (json_object_get_boolean_member_with_default (o, "draft", FALSE))
        continue;
      const char *tag = json_object_has_member (o, "tag_name")
                          ? json_object_get_string_member (o, "tag_name") : NULL;
      if (tag == NULL || tag[0] == '\0')
        continue;
      const char *ver = strip_v (tag);
      gboolean pre = json_object_get_boolean_member_with_default (o, "prerelease", FALSE);
      if (!channel_qualifies (self, pre, prerelease_label (ver)))
        continue;
      if (best == NULL || cmp_version (ver, best) > 0)
        {
          best = ver;
          /* _with_default returns "" when the member is absent OR JSON null — a
           * release with empty notes has body: null, and the plain getter would
           * return NULL, which then trips the non-nullable signal arg. */
          best_url = json_object_get_string_member_with_default (o, "html_url", "");
          best_notes = json_object_get_string_member_with_default (o, "body", "");
        }
    }

  if (best != NULL && cmp_version (best, self->current) > 0)
    {
      g_message ("update available: %s (current %s, channel %s)",
                 best, self->current, self->channel);
      g_signal_emit (self, signals[SIG_AVAILABLE], 0, best, best_url, best_notes);
    }
  else
    {
      g_debug ("up to date: best applicable %s, current %s, channel %s",
               best ? best : "(none)", self->current, self->channel);
      g_signal_emit (self, signals[SIG_UP_TO_DATE], 0);
    }
  g_object_unref (parser);
}

/* ---- network fetch (worker thread) -------------------------------------- */
/* The worker returns the raw JSON body (a GBytes) on success, or fails the GTask
 * with a human-readable message. Two transports, because the GIO TLS backend
 * (glib-networking) is a separate module that the Android APK doesn't ship:
 *   - desktop: a GIO TLS socket speaks HTTP/1.0 to api.github.com and the headers
 *     are stripped here so the body GBytes is pure JSON;
 *   - Android: java.net does the HTTPS GET via the platform TLS stack + system CA
 *     and hands back the body directly (status checked Java-side). */

#ifdef __ANDROID__
/* station-android.c. http_get returns a g_malloc'd body (caller frees) + length,
 * or NULL with *error set; prime captures the JavaVM while on the main thread.
 * download streams the URL (java.net follows redirects) into @out, reporting
 * progress via station_updates_report_progress (self). */
extern void     station_updates_android_prime (void);
extern guint8 * station_updates_android_http_get (const char *url, gsize *out_len,
                                                  GError **error);
extern gboolean station_updates_android_download (const char *url, GOutputStream *out,
                                                  gpointer self, GCancellable *cancel,
                                                  GError **error);
#else
/* On Windows the GIO TLS backend (gnutls) loads but has no system CA store, so
 * verification fails with "Unacceptable TLS certificate". The app exports
 * SSL_CERT_FILE pointing at the bundled CA bundle (for the engine's own TLS);
 * reuse it here as the TLS connection's trust database at handshake time. Where
 * SSL_CERT_FILE is unset (Linux/macOS) the backend's system trust is used. */
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
#endif

static void
fetch_thread (GTask *task, gpointer src, gpointer task_data,
              GCancellable *cancellable)
{
  (void) src;
  const char *request = task_data;
  GError *err = NULL;

#ifdef __ANDROID__
  gsize blen = 0;
  guint8 *body = station_updates_android_http_get (request, &blen, &err);
  if (body == NULL)
    {
      g_task_return_error (task, err);
      return;
    }
  g_task_return_pointer (task, g_bytes_new_take (body, blen),
                         (GDestroyNotify) g_bytes_unref);
#else
  GSocketClient *client = g_socket_client_new ();
  g_socket_client_set_tls (client, TRUE);
  g_socket_client_set_timeout (client, 15);

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

  GSocketConnection *conn = g_socket_client_connect_to_host (
      client, "api.github.com", 443, cancellable, &err);
  if (conn == NULL)
    {
      g_object_unref (client);
      g_task_return_error (task, err);
      return;
    }

  GOutputStream *os = g_io_stream_get_output_stream (G_IO_STREAM (conn));
  if (!g_output_stream_write_all (os, request, strlen (request), NULL,
                                  cancellable, &err))
    {
      g_object_unref (conn);
      g_object_unref (client);
      g_task_return_error (task, err);
      return;
    }

  /* HTTP/1.0 + Connection: close → the body is delimited by EOF, so just read
   * everything; no chunked-transfer decoding needed. */
  GInputStream *is = g_io_stream_get_input_stream (G_IO_STREAM (conn));
  GByteArray *buf = g_byte_array_new ();
  guint8 tmp[8192];
  gssize n;
  while ((n = g_input_stream_read (is, tmp, sizeof tmp, cancellable, &err)) > 0)
    {
      /* Guard against an absurdly large response. */
      if (buf->len + n > (4u * 1024u * 1024u))
        break;
      g_byte_array_append (buf, tmp, (guint) n);
    }
  g_io_stream_close (G_IO_STREAM (conn), NULL, NULL);
  g_object_unref (conn);
  g_object_unref (client);

  if (n < 0)
    {
      g_byte_array_unref (buf);
      g_task_return_error (task, err);
      return;
    }

  /* Strip the HTTP headers and require a 200, so the body GBytes is pure JSON. */
  const char *data = (const char *) buf->data;
  const char *sep = g_strstr_len (data, (gssize) buf->len, "\r\n\r\n");
  if (sep == NULL)
    {
      g_byte_array_unref (buf);
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "no HTTP response headers");
      return;
    }
  gsize head_len = (gsize) (sep - data);
  if (g_strstr_len (data, (gssize) head_len, " 200 ") == NULL)
    {
      const char *eol = g_strstr_len (data, (gssize) head_len, "\r\n");
      char *status = g_strndup (data, eol ? (gsize) (eol - data) : head_len);
      g_byte_array_unref (buf);
      g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "HTTP error: %s", status);
      g_free (status);
      return;
    }
  GBytes *body = g_bytes_new (sep + 4, buf->len - head_len - 4);
  g_byte_array_unref (buf);
  g_task_return_pointer (task, body, (GDestroyNotify) g_bytes_unref);
#endif
}

static void
fetch_done (GObject *src, GAsyncResult *res, gpointer user_data)
{
  (void) user_data;
  StationUpdates *self = STATION_UPDATES (src);
  GError *err = NULL;
  GBytes *bytes = g_task_propagate_pointer (G_TASK (res), &err);
  if (bytes == NULL)
    {
      emit_failed (self, err ? err->message : "network error");
      g_clear_error (&err);
      return;
    }
  gsize len = 0;
  const char *data = g_bytes_get_data (bytes, &len);
  parse_and_emit (self, data, len);
  g_bytes_unref (bytes);
}

void
station_updates_check (StationUpdates *self)
{
  g_return_if_fail (STATION_IS_UPDATES (self));
  g_debug ("checking %s for releases newer than %s on channel %s",
           self->repo, self->current, self->channel);

#ifdef __ANDROID__
  /* java.net needs a full URL; capture the JavaVM now, on the main thread. */
  station_updates_android_prime ();
  char *request = g_strdup_printf (
      "https://api.github.com/repos/%s/releases?per_page=15", self->repo);
#else
  char *request = g_strdup_printf (
      "GET /repos/%s/releases?per_page=15 HTTP/1.0\r\n"
      "Host: api.github.com\r\n"
      "User-Agent: libstation\r\n"
      "Accept: application/vnd.github+json\r\n"
      "Connection: close\r\n\r\n",
      self->repo);
#endif

  GTask *task = g_task_new (self, self->cancel, fetch_done, NULL);
  g_task_set_task_data (task, request, g_free);
  g_task_run_in_thread (task, fetch_thread);
  g_object_unref (task);
}

/* ---- asset download (worker thread) ------------------------------------- */

typedef struct { StationUpdates *self; double frac; } ProgressData;

static gboolean
emit_progress_main (gpointer p)
{
  ProgressData *d = p;
  g_signal_emit (d->self, signals[SIG_DL_PROGRESS], 0, d->frac);
  g_object_unref (d->self);
  g_free (d);
  return G_SOURCE_REMOVE;
}

/* Emit "download-progress" on the main thread. total<=0 → fraction -1 (unknown).
 * Called from the worker thread here and from station-android.c. */
void
station_updates_report_progress (gpointer self, gint64 got, gint64 total)
{
  ProgressData *d = g_new (ProgressData, 1);
  d->self = g_object_ref (self);
  d->frac = (total > 0) ? CLAMP ((double) got / (double) total, 0.0, 1.0) : -1.0;
  g_main_context_invoke (NULL, emit_progress_main, d);
}

#ifndef __ANDROID__
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

/* One HTTP/1.0 GET. 2xx: stream body to @out (with progress), *redirect=NULL.
 * 3xx: *redirect=Location. Else: FALSE + *error. */
static gboolean
http_get_once (StationUpdates *self, const char *url, GOutputStream *out,
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

  GSocketClient *client = g_socket_client_new ();
  g_socket_client_set_tls (client, TRUE);
  g_socket_client_set_timeout (client, 30);
  const char *ca_file = g_getenv ("SSL_CERT_FILE");
  if (ca_file != NULL && *ca_file != '\0')
    {
      GTlsDatabase *ca_db = g_tls_file_database_new (ca_file, NULL);
      if (ca_db != NULL)
        {
          g_signal_connect (client, "event", G_CALLBACK (apply_ca_file), ca_db);
          g_object_set_data_full (G_OBJECT (client), "station-ca-db", ca_db, g_object_unref);
        }
    }

  GSocketConnection *conn = g_socket_client_connect_to_host (client, host, port, cancel, error);
  if (conn == NULL)
    { g_object_unref (client); g_free (host); g_free (path); return FALSE; }

  char *req = g_strdup_printf (
      "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: libstation\r\n"
      "Accept: */*\r\nConnection: close\r\n\r\n", path, host);
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
      ok = g_output_stream_write_all (out, tmp, n, NULL, cancel, error);
      got += n;
      if (got - last_report >= 65536)
        { station_updates_report_progress (self, got, total); last_report = got; }
    }
  if (ok && n < 0) ok = FALSE;
  if (ok) station_updates_report_progress (self, got, total > 0 ? total : got);

  g_object_unref (conn); g_object_unref (client); g_free (host); g_free (path);
  return ok;
}

static gboolean
desktop_download (StationUpdates *self, const char *url, GOutputStream *out,
                  GCancellable *cancel, GError **error)
{
  char *cur = g_strdup (url);
  gboolean ok = FALSE;
  for (int hop = 0; hop < 6; hop++)
    {
      char *redirect = NULL;
      ok = http_get_once (self, cur, out, &redirect, cancel, error);
      if (!ok || redirect == NULL) { g_free (redirect); g_free (cur); return ok; }
      g_free (cur);
      cur = redirect;
    }
  g_free (cur);
  if (error != NULL && *error == NULL)
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "too many redirects");
  return FALSE;
}
#endif /* !__ANDROID__ */

typedef struct { char *url; char *dest; } DownloadReq;

static void
download_req_free (gpointer p)
{
  DownloadReq *r = p;
  g_free (r->url);
  g_free (r->dest);
  g_free (r);
}

static void
download_thread (GTask *task, gpointer src, gpointer task_data, GCancellable *cancel)
{
  StationUpdates *self = STATION_UPDATES (src);
  DownloadReq *req = task_data;
  GError *err = NULL;

  GFile *file = g_file_new_for_path (req->dest);
  GFileOutputStream *out = g_file_replace (file, NULL, FALSE,
                                           G_FILE_CREATE_REPLACE_DESTINATION, cancel, &err);
  if (out == NULL)
    { g_object_unref (file); g_task_return_error (task, err); return; }

#ifdef __ANDROID__
  gboolean ok = station_updates_android_download (req->url, G_OUTPUT_STREAM (out),
                                                  self, cancel, &err);
#else
  gboolean ok = desktop_download (self, req->url, G_OUTPUT_STREAM (out), cancel, &err);
#endif

  g_output_stream_close (G_OUTPUT_STREAM (out), NULL, NULL);
  g_object_unref (out);
  if (!ok)
    {
      g_file_delete (file, NULL, NULL);   /* no half-written file */
      g_object_unref (file);
      g_task_return_error (task, err);
      return;
    }
  g_object_unref (file);
  g_task_return_boolean (task, TRUE);
}

static void
download_done (GObject *src, GAsyncResult *res, gpointer user_data)
{
  (void) user_data;
  StationUpdates *self = STATION_UPDATES (src);
  DownloadReq *req = g_task_get_task_data (G_TASK (res));
  GError *err = NULL;
  if (g_task_propagate_boolean (G_TASK (res), &err))
    {
      g_message ("update downloaded to %s", req->dest);
      g_signal_emit (self, signals[SIG_DOWNLOADED], 0, req->dest);
    }
  else
    {
      g_warning ("update download failed: %s", err ? err->message : "?");
      g_signal_emit (self, signals[SIG_DL_FAILED], 0, err ? err->message : "?");
      g_clear_error (&err);
    }
}

void
station_updates_download (StationUpdates *self, const char *url, const char *dest_path)
{
  g_return_if_fail (STATION_IS_UPDATES (self));
  g_return_if_fail (url != NULL && dest_path != NULL);
  g_debug ("downloading %s -> %s", url, dest_path);
#ifdef __ANDROID__
  station_updates_android_prime ();
#endif
  DownloadReq *req = g_new0 (DownloadReq, 1);
  req->url = g_strdup (url);
  req->dest = g_strdup (dest_path);

  GTask *task = g_task_new (self, self->cancel, download_done, NULL);
  g_task_set_task_data (task, req, download_req_free);
  g_task_run_in_thread (task, download_thread);
  g_object_unref (task);
}

/* ---- GObject ------------------------------------------------------------ */

StationUpdates *
station_updates_new (const char *repo, const char *current_version)
{
  g_return_val_if_fail (repo != NULL, NULL);
  StationUpdates *self = g_object_new (STATION_TYPE_UPDATES, NULL);
  self->repo = g_strdup (repo);
  self->current = g_strdup (strip_v (current_version != NULL ? current_version : ""));
  return self;
}

void
station_updates_add_channel (StationUpdates *self, const char *id,
                             const char *const *prerelease_labels)
{
  g_return_if_fail (STATION_IS_UPDATES (self));
  g_return_if_fail (id != NULL && *id != '\0');
  Channel *c = find_channel (self, id);
  if (c == NULL)
    {
      c = g_new0 (Channel, 1);
      c->id = g_strdup (id);
      g_ptr_array_add (self->channels, c);
    }
  else
    g_clear_pointer (&c->labels, g_strfreev);
  c->labels = prerelease_labels != NULL
                ? g_strdupv ((char **) prerelease_labels)
                : g_new0 (char *, 1);
}

void
station_updates_set_channel (StationUpdates *self, const char *id)
{
  g_return_if_fail (STATION_IS_UPDATES (self));
  g_free (self->channel);
  self->channel = g_strdup ((id != NULL && *id != '\0') ? id : "stable");
}

const char *
station_updates_get_channel (StationUpdates *self)
{
  g_return_val_if_fail (STATION_IS_UPDATES (self), NULL);
  return self->channel;
}

char **
station_updates_dup_channels (StationUpdates *self)
{
  g_return_val_if_fail (STATION_IS_UPDATES (self), NULL);
  GPtrArray *ids = g_ptr_array_new ();
  for (guint i = 0; i < self->channels->len; i++)
    {
      Channel *c = g_ptr_array_index (self->channels, i);
      g_ptr_array_add (ids, g_strdup (c->id));
    }
  g_ptr_array_add (ids, NULL);
  return (char **) g_ptr_array_free (ids, FALSE);
}

static void
station_updates_finalize (GObject *object)
{
  StationUpdates *self = STATION_UPDATES (object);
  g_cancellable_cancel (self->cancel);
  g_clear_object (&self->cancel);
  g_clear_pointer (&self->repo, g_free);
  g_clear_pointer (&self->current, g_free);
  g_clear_pointer (&self->channel, g_free);
  g_clear_pointer (&self->channels, g_ptr_array_unref);
  G_OBJECT_CLASS (station_updates_parent_class)->finalize (object);
}

static void
station_updates_class_init (StationUpdatesClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = station_updates_finalize;
  signals[SIG_AVAILABLE] = g_signal_new ("available",
      G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
      G_TYPE_NONE, 3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
  signals[SIG_UP_TO_DATE] = g_signal_new ("up-to-date",
      G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
      G_TYPE_NONE, 0);
  signals[SIG_FAILED] = g_signal_new ("failed",
      G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
      G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[SIG_DL_PROGRESS] = g_signal_new ("download-progress",
      G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
      G_TYPE_NONE, 1, G_TYPE_DOUBLE);
  signals[SIG_DOWNLOADED] = g_signal_new ("downloaded",
      G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
      G_TYPE_NONE, 1, G_TYPE_STRING);
  signals[SIG_DL_FAILED] = g_signal_new ("download-failed",
      G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
      G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
station_updates_init (StationUpdates *self)
{
  self->cancel = g_cancellable_new ();
  self->channel = g_strdup ("stable");
  self->channels = g_ptr_array_new_with_free_func (channel_free);
}
