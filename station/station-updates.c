/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef STATION_COMPILATION
# define STATION_COMPILATION
#endif
#include "station-updates.h"

#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <stdlib.h>

struct _StationUpdates
{
  GObject       parent_instance;
  char         *repo;            /* "owner/name" */
  char         *current;         /* current version, no leading 'v' */
  gboolean      include_pre;
  GCancellable *cancel;
};

G_DEFINE_FINAL_TYPE (StationUpdates, station_updates, G_TYPE_OBJECT)

enum { SIG_AVAILABLE, N_SIGNALS };
static guint signals[N_SIGNALS];

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
      if (a_pre && !b_pre)      result = -1;
      else if (!a_pre && b_pre) result = 1;
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
      g_debug ("station updates: parse failed: %s", err ? err->message : "?");
      g_clear_error (&err);
      g_object_unref (parser);
      return;
    }
  JsonNode *root = json_parser_get_root (parser);
  if (root == NULL || !JSON_NODE_HOLDS_ARRAY (root))
    {
      g_object_unref (parser);
      return;
    }
  JsonArray *arr = json_node_get_array (root);
  guint count = json_array_get_length (arr);
  for (guint i = 0; i < count; i++)
    {
      JsonObject *o = json_array_get_object_element (arr, i);
      if (json_object_get_boolean_member_with_default (o, "draft", FALSE))
        continue;
      if (!self->include_pre
          && json_object_get_boolean_member_with_default (o, "prerelease", FALSE))
        continue;
      const char *tag = json_object_has_member (o, "tag_name")
                          ? json_object_get_string_member (o, "tag_name") : NULL;
      if (tag == NULL || tag[0] == '\0')
        continue;
      if (cmp_version (strip_v (tag), self->current) > 0)
        {
          const char *url = json_object_has_member (o, "html_url")
                              ? json_object_get_string_member (o, "html_url") : "";
          const char *notes = json_object_has_member (o, "body")
                                ? json_object_get_string_member (o, "body") : "";
          g_signal_emit (self, signals[SIG_AVAILABLE], 0, strip_v (tag), url, notes);
        }
      break; /* newest applicable release decided the outcome */
    }
  g_object_unref (parser);
}

/* ---- network fetch (worker thread, sync GIO) ---------------------------- */

static void
fetch_thread (GTask *task, gpointer src, gpointer task_data,
              GCancellable *cancellable)
{
  (void) src;
  const char *request = task_data;
  GError *err = NULL;

  GSocketClient *client = g_socket_client_new ();
  g_socket_client_set_tls (client, TRUE);
  g_socket_client_set_timeout (client, 15);
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
  g_task_return_pointer (task, g_byte_array_free_to_bytes (buf),
                         (GDestroyNotify) g_bytes_unref);
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
      g_debug ("station updates: check failed: %s", err ? err->message : "?");
      g_clear_error (&err);
      return;
    }
  gsize len = 0;
  const char *data = g_bytes_get_data (bytes, &len);
  /* Split off the HTTP headers; require a 200 status. */
  const char *sep = g_strstr_len (data, (gssize) len, "\r\n\r\n");
  if (sep != NULL)
    {
      gsize head_len = (gsize) (sep - data);
      if (g_strstr_len (data, (gssize) head_len, " 200 ") != NULL)
        parse_and_emit (self, sep + 4, len - head_len - 4);
    }
  g_bytes_unref (bytes);
}

void
station_updates_check (StationUpdates *self, gboolean include_prerelease)
{
  g_return_if_fail (STATION_IS_UPDATES (self));
  self->include_pre = include_prerelease;

  char *request = g_strdup_printf (
      "GET /repos/%s/releases?per_page=15 HTTP/1.0\r\n"
      "Host: api.github.com\r\n"
      "User-Agent: libstation\r\n"
      "Accept: application/vnd.github+json\r\n"
      "Connection: close\r\n\r\n",
      self->repo);

  GTask *task = g_task_new (self, self->cancel, fetch_done, NULL);
  g_task_set_task_data (task, request, g_free);
  g_task_run_in_thread (task, fetch_thread);
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

static void
station_updates_finalize (GObject *object)
{
  StationUpdates *self = STATION_UPDATES (object);
  g_cancellable_cancel (self->cancel);
  g_clear_object (&self->cancel);
  g_clear_pointer (&self->repo, g_free);
  g_clear_pointer (&self->current, g_free);
  G_OBJECT_CLASS (station_updates_parent_class)->finalize (object);
}

static void
station_updates_class_init (StationUpdatesClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = station_updates_finalize;
  signals[SIG_AVAILABLE] = g_signal_new ("available",
      G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
      G_TYPE_NONE, 3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
}

static void
station_updates_init (StationUpdates *self)
{
  self->cancel = g_cancellable_new ();
}
