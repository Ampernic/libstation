/* SPDX-License-Identifier: LGPL-3.0-or-later */
#ifndef STATION_COMPILATION
# define STATION_COMPILATION
#endif
#define G_LOG_DOMAIN "Station-Updates"
#include "station-updates.h"
#include "station-http-private.h"
#include "station-updates-schema-private.h"
#include "station-minisign-private.h"

#include <gio/gio.h>
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
  GObject               parent_instance;
  char                 *repo;       /* "owner/name" */
  char                 *current;    /* current version, no leading 'v' */
  char                 *channel;    /* active channel id; "stable" = releases only */
  GPtrArray            *channels;   /* (element-type Channel), in registration order */
  StationReleaseSchema *schema;     /* describes the release source (forge/custom) */
  StationRelease       *available;  /* the release last reported via "available", retained
                                     * so a verified download can resolve url+digest by name */
  char                 *notes_section; /* if set, emit only the notes block under this
                                        * heading (a localized "What's new"); NULL = all */
  GCancellable         *cancel;
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

/* Case-insensitive (Unicode-aware) heading match, so a passed "Что нового"
 * matches the body's heading regardless of case. */
static gboolean
heading_matches (const char *a, const char *b)
{
  char *fa = g_utf8_casefold (a, -1);
  char *fb = g_utf8_casefold (b, -1);
  gboolean eq = (g_strcmp0 (fa, fb) == 0);
  g_free (fa);
  g_free (fb);
  return eq;
}

/* Return the Markdown block under the heading @heading: the lines after the first
 * heading whose text matches, up to the next heading of the same or higher level.
 * Lets a consumer show only one section (e.g. a localized "What's new"); passing a
 * locale-specific heading also selects the right language. The whole body is
 * returned when @heading is unset or not found. Caller frees. */
static char *
extract_notes_section (const char *body, const char *heading)
{
  if (body == NULL)
    return NULL;
  if (heading == NULL || *heading == '\0')
    return g_strdup (body);

  char **lines = g_strsplit (body, "\n", -1);
  GString *out = NULL;
  int want_level = 0;
  for (int i = 0; lines[i] != NULL; i++)
    {
      const char *l = lines[i];
      int level = 0;
      while (l[level] == '#')
        level++;
      gboolean is_heading = (level > 0 && level <= 6 && l[level] == ' ');

      if (out == NULL)
        {
          if (is_heading)
            {
              char *htext = g_strstrip (g_strdup (l + level + 1));
              if (heading_matches (htext, heading))
                {
                  out = g_string_new (NULL);
                  want_level = level;
                }
              g_free (htext);
            }
          continue;
        }
      if (is_heading && level <= want_level)
        break;                       /* next section of >= rank: stop */
      g_string_append (out, l);
      g_string_append_c (out, '\n');
    }
  g_strfreev (lines);

  if (out == NULL)
    return g_strdup (body);          /* heading not found: show everything */
  char *s = g_string_free (out, FALSE);
  return g_strchomp (s);
}

/* Parse the response per the configured schema and emit "available" for the
 * newest applicable release that is newer than self->current. Main thread. */
static void
parse_and_emit (StationUpdates *self, const char *body, gsize len)
{
  GError *err = NULL;
  GPtrArray *releases = station_release_schema_parse (self->schema, body, len, &err);
  if (releases == NULL)
    {
      char *r = g_strdup_printf ("unexpected response: %s", err ? err->message : "?");
      emit_failed (self, r);
      g_free (r);
      g_clear_error (&err);
      return;
    }

  /* Sources list by publish time, not version order, so scan all and keep the
   * highest-versioned release that qualifies for the active channel. */
  StationRelease *best = NULL;
  const char *best_ver = NULL;   /* strip_v'd, points into best->version */
  for (guint i = 0; i < releases->len; i++)
    {
      StationRelease *rel = g_ptr_array_index (releases, i);
      const char *ver = strip_v (rel->version);
      if (!channel_qualifies (self, rel->prerelease, prerelease_label (ver)))
        continue;
      if (best == NULL || cmp_version (ver, best_ver) > 0)
        { best = rel; best_ver = ver; }
    }

  if (best != NULL && cmp_version (best_ver, self->current) > 0)
    {
      /* Retain it so download_checked() can resolve the asset url + digest by
       * name later, after the user acts on the notification. */
      g_clear_pointer (&self->available, station_release_free);
      self->available = station_release_copy (best);
      g_message ("update available: %s (current %s, channel %s)",
                 best_ver, self->current, self->channel);
      char *notes = extract_notes_section (best->notes, self->notes_section);
      g_signal_emit (self, signals[SIG_AVAILABLE], 0,
                     best_ver, best->page_url, notes);
      g_free (notes);
    }
  else
    {
      g_debug ("up to date: best applicable %s, current %s, channel %s",
               best_ver ? best_ver : "(none)", self->current, self->channel);
      g_signal_emit (self, signals[SIG_UP_TO_DATE], 0);
    }
  g_ptr_array_unref (releases);
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
#endif

static void
fetch_thread (GTask *task, gpointer src, gpointer task_data,
              GCancellable *cancellable)
{
  (void) src;
  const char *url = task_data;
  GError *err = NULL;

#ifdef __ANDROID__
  gsize blen = 0;
  guint8 *body = station_updates_android_http_get (url, &blen, &err);
  if (body == NULL)
    { g_task_return_error (task, err); return; }
  g_task_return_pointer (task, g_bytes_new_take (body, blen),
                         (GDestroyNotify) g_bytes_unref);
#else
  GBytes *body = station_http_get_bytes (url, 4u * 1024u * 1024u, cancellable, &err);
  if (body == NULL)
    { g_task_return_error (task, err); return; }
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
  /* java.net needs the JavaVM; capture it now, on the main thread. */
  station_updates_android_prime ();
#endif
  char *url = station_release_schema_build_url (self->schema, self->repo);

  GTask *task = g_task_new (self, self->cancel, fetch_done, NULL);
  g_task_set_task_data (task, url, g_free);
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


typedef struct
{
  char *url;
  char *dest;
  /* Verification (download_checked only; all NULL for a plain download):
   * expected_sha256 is a known hex digest; else checksums_url is fetched and its
   * line for @filename gives the digest. When sig_url + pubkey are set, the
   * fetched checksums are first authenticated with a minisign signature. */
  char *expected_sha256;
  char *checksums_url;
  char *filename;
  char *sig_url;
  char *pubkey;
} DownloadReq;

static void
download_req_free (gpointer p)
{
  DownloadReq *r = p;
  g_free (r->url);
  g_free (r->dest);
  g_free (r->expected_sha256);
  g_free (r->checksums_url);
  g_free (r->filename);
  g_free (r->sig_url);
  g_free (r->pubkey);
  g_free (r);
}

/* SHA-256 of a file as lowercase hex, or NULL on read error. */
static char *
sha256_file (const char *path)
{
  GFile *f = g_file_new_for_path (path);
  GFileInputStream *in = g_file_read (f, NULL, NULL);
  g_object_unref (f);
  if (in == NULL)
    return NULL;
  GChecksum *ck = g_checksum_new (G_CHECKSUM_SHA256);
  guint8 buf[65536];
  gssize n;
  while ((n = g_input_stream_read (G_INPUT_STREAM (in), buf, sizeof buf, NULL, NULL)) > 0)
    g_checksum_update (ck, buf, (gsize) n);
  g_object_unref (in);
  char *hex = (n < 0) ? NULL : g_strdup (g_checksum_get_string (ck));
  g_checksum_free (ck);
  return hex;
}

/* Find @filename's digest in a "HEX  name" / "HEX *name" checksums listing. */
static char *
sha256_from_sums (GBytes *sums, const char *filename)
{
  gsize len = 0;
  const char *data = g_bytes_get_data (sums, &len);
  char *text = g_strndup (data, len);
  char **lines = g_strsplit (text, "\n", -1);
  char *result = NULL;
  for (guint i = 0; lines[i] != NULL && result == NULL; i++)
    {
      char *line = g_strstrip (lines[i]);
      if (*line == '\0')
        continue;
      char **parts = g_strsplit_set (line, " \t", 2);
      if (parts[0] != NULL && parts[1] != NULL)
        {
          char *name = g_strstrip (parts[1]);
          if (*name == '*')   /* the "binary" marker */
            name++;
          if (g_strcmp0 (name, filename) == 0)
            result = g_ascii_strdown (parts[0], -1);
        }
      g_strfreev (parts);
    }
  g_strfreev (lines);
  g_free (text);
  return result;
}

/* Fetch a URL body (same transport as the check). */
static GBytes *
fetch_bytes (const char *url, GCancellable *cancel, GError **error)
{
#ifdef __ANDROID__
  gsize n = 0;
  guint8 *b = station_updates_android_http_get (url, &n, error);
  return b != NULL ? g_bytes_new_take (b, n) : NULL;
#else
  return station_http_get_bytes (url, 4u * 1024u * 1024u, cancel, error);
#endif
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
  gboolean ok = station_http_get_to_stream (req->url, G_OUTPUT_STREAM (out),
                                            station_updates_report_progress, self,
                                            cancel, &err);
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

  /* Verify SHA-256 when requested: use the known digest, else fetch the
   * checksums listing and look up @filename. A mismatch (or, when a checksum was
   * expected, an unresolvable one) deletes the file and fails. */
  char *expected = (req->expected_sha256 != NULL) ? g_strdup (req->expected_sha256) : NULL;
  if (expected == NULL && req->checksums_url != NULL)
    {
      GBytes *sums = fetch_bytes (req->checksums_url, cancel, &err);
      /* Authenticate the checksums with the minisign signature first, so a
       * compromised host can't serve a matching file + checksums pair. */
      if (sums != NULL && req->sig_url != NULL && req->pubkey != NULL)
        {
          GBytes *sig = fetch_bytes (req->sig_url, cancel, &err);
          gboolean sig_ok = (sig != NULL)
            && station_minisign_verify (req->pubkey, sums, sig, &err);
          g_clear_pointer (&sig, g_bytes_unref);
          if (!sig_ok)
            {
              g_bytes_unref (sums);
              g_file_delete (file, NULL, NULL);
              g_object_unref (file);
              if (err == NULL)
                g_set_error_literal (&err, G_IO_ERROR, G_IO_ERROR_FAILED,
                                     "checksums signature did not verify");
              g_task_return_error (task, err);
              return;
            }
          g_message ("update checksums signature verified (minisign)");
        }
      if (sums != NULL)
        { expected = sha256_from_sums (sums, req->filename); g_bytes_unref (sums); }
      if (expected == NULL)
        {
          g_file_delete (file, NULL, NULL);
          g_object_unref (file);
          if (err == NULL)
            g_set_error (&err, G_IO_ERROR, G_IO_ERROR_FAILED,
                         "no checksum for %s", req->filename);
          g_task_return_error (task, err);
          return;
        }
    }
  if (expected != NULL)
    {
      char *actual = sha256_file (req->dest);
      gboolean match = (actual != NULL && g_ascii_strcasecmp (actual, expected) == 0);
      g_free (actual);
      g_free (expected);
      if (!match)
        {
          g_file_delete (file, NULL, NULL);
          g_object_unref (file);
          g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                   "SHA-256 mismatch");
          return;
        }
      g_message ("update SHA-256 verified");
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

void
station_updates_download_checked (StationUpdates *self, const char *asset_name,
                                  const char *dest_path)
{
  g_return_if_fail (STATION_IS_UPDATES (self));
  g_return_if_fail (asset_name != NULL && dest_path != NULL);

  if (self->available == NULL || self->available->assets == NULL)
    {
      g_signal_emit (self, signals[SIG_DL_FAILED], 0, "no available release");
      return;
    }

  /* Resolve the named asset (and the checksums asset, if configured) by name in
   * the retained release. */
  StationAsset *asset = NULL, *sums = NULL, *sig = NULL;
  const char *sums_name = station_release_schema_get_checksums_asset (self->schema);
  const char *sig_name  = station_release_schema_get_signature_asset (self->schema);
  for (guint i = 0; i < self->available->assets->len; i++)
    {
      StationAsset *a = g_ptr_array_index (self->available->assets, i);
      if (g_strcmp0 (a->name, asset_name) == 0)
        asset = a;
      if (sums_name != NULL && g_strcmp0 (a->name, sums_name) == 0)
        sums = a;
      if (sig_name != NULL && g_strcmp0 (a->name, sig_name) == 0)
        sig = a;
    }
  if (asset == NULL || asset->url[0] == '\0')
    {
      char *r = g_strdup_printf ("asset not found: %s", asset_name);
      g_signal_emit (self, signals[SIG_DL_FAILED], 0, r);
      g_free (r);
      return;
    }

  DownloadReq *req = g_new0 (DownloadReq, 1);
  req->url = g_strdup (asset->url);
  req->dest = g_strdup (dest_path);
  req->filename = g_strdup (asset_name);
  /* Resolve how the download is verified, strongest first. If a public key is
   * configured a signed checksums asset is REQUIRED: fail closed rather than fall
   * back to an unsigned listing or an inline digest, both of which a compromised
   * release host controls (a downgrade attack otherwise strips the signature). */
  const char *pubkey = station_release_schema_get_public_key (self->schema);
  gboolean require_sig = (pubkey != NULL && *pubkey != '\0');

  if (require_sig)
    {
      gboolean have_sums = (sums != NULL && sums->url[0] != '\0');
      gboolean have_sig  = (sig  != NULL && sig->url[0]  != '\0');
      if (!have_sums || !have_sig)
        {
          char *r = g_strdup_printf ("refusing unsigned update: missing %s",
                                     !have_sums ? (sums_name ? sums_name : "checksums")
                                                : (sig_name ? sig_name : "signature"));
          g_signal_emit (self, signals[SIG_DL_FAILED], 0, r);
          g_free (r);
          download_req_free (req);
          return;
        }
      req->checksums_url = g_strdup (sums->url);
      req->sig_url = g_strdup (sig->url);
      req->pubkey  = g_strdup (pubkey);
    }
  else if (asset->digest != NULL && g_ascii_strncasecmp (asset->digest, "sha256:", 7) == 0)
    req->expected_sha256 = g_ascii_strdown (asset->digest + 7, -1);
  else if (sums != NULL && sums->url[0] != '\0')
    req->checksums_url = g_strdup (sums->url);
  else
    g_warning ("no checksum source for %s; downloading unverified", asset_name);

#ifdef __ANDROID__
  station_updates_android_prime ();
#endif
  g_debug ("downloading (checked) %s -> %s", req->url, dest_path);
  GTask *task = g_task_new (self, self->cancel, download_done, NULL);
  g_task_set_task_data (task, req, download_req_free);
  g_task_run_in_thread (task, download_thread);
  g_object_unref (task);
}

/* ---- GObject ------------------------------------------------------------ */

StationUpdates *
station_updates_new_with_schema (StationReleaseSchema *schema, const char *repo,
                                 const char *current_version)
{
  g_return_val_if_fail (STATION_IS_RELEASE_SCHEMA (schema), NULL);
  g_return_val_if_fail (repo != NULL, NULL);
  StationUpdates *self = g_object_new (STATION_TYPE_UPDATES, NULL);
  self->schema = g_object_ref (schema);
  self->repo = g_strdup (repo);
  self->current = g_strdup (strip_v (current_version != NULL ? current_version : ""));
  return self;
}

StationUpdates *
station_updates_new (const char *repo, const char *current_version)
{
  StationReleaseSchema *gh = station_release_schema_new_github ();
  StationUpdates *self = station_updates_new_with_schema (gh, repo, current_version);
  g_object_unref (gh);   /* new_with_schema took its own ref */
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

void
station_updates_set_notes_section (StationUpdates *self, const char *heading)
{
  g_return_if_fail (STATION_IS_UPDATES (self));
  g_free (self->notes_section);
  self->notes_section = (heading != NULL && *heading != '\0') ? g_strdup (heading) : NULL;
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
  g_clear_pointer (&self->available, station_release_free);
  g_clear_pointer (&self->notes_section, g_free);
  g_clear_object (&self->schema);
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
