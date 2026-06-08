/* SPDX-License-Identifier: LGPL-3.0-or-later */
#ifndef STATION_COMPILATION
# define STATION_COMPILATION
#endif
#define G_LOG_DOMAIN "Station-Updates"
#include "station-updates-schema-private.h"

#include <json-glib/json-glib.h>
#include <string.h>

struct _StationReleaseSchema
{
  GObject  parent_instance;
  char    *host;            /* {host} substitution */
  char    *url_template;
  char    *releases_path;   /* JsonPath to the releases array */
  char    *version_field;
  char    *notes_field;
  char    *page_url_field;
  char    *prerelease_field;
};

G_DEFINE_FINAL_TYPE (StationReleaseSchema, station_release_schema, G_TYPE_OBJECT)

void
station_release_free (StationRelease *r)
{
  if (r == NULL)
    return;
  g_free (r->version);
  g_free (r->notes);
  g_free (r->page_url);
  g_free (r);
}

/* ---- setters ------------------------------------------------------------- */

#define SETTER(name, field)                                                   \
  void station_release_schema_set_##name (StationReleaseSchema *self,         \
                                          const char *v)                      \
  {                                                                           \
    g_return_if_fail (STATION_IS_RELEASE_SCHEMA (self));                      \
    g_free (self->field);                                                     \
    self->field = g_strdup (v);                                              \
  }
SETTER (url,        url_template)
SETTER (releases,   releases_path)
SETTER (version,    version_field)
SETTER (notes,      notes_field)
SETTER (page_url,   page_url_field)
SETTER (prerelease, prerelease_field)
#undef SETTER

/* ---- presets ------------------------------------------------------------- */

StationReleaseSchema *
station_release_schema_new (void)
{
  return g_object_new (STATION_TYPE_RELEASE_SCHEMA, NULL);
}

StationReleaseSchema *
station_release_schema_new_github (void)
{
  StationReleaseSchema *s = station_release_schema_new ();
  s->host = g_strdup ("api.github.com");
  station_release_schema_set_url (s, "https://{host}/repos/{repo}/releases?per_page=15");
  return s;
}

/* Gitea and Forgejo share GitHub's release JSON shape under /api/v1. */
StationReleaseSchema *
station_release_schema_new_gitea (const char *host)
{
  g_return_val_if_fail (host != NULL && *host != '\0', NULL);
  StationReleaseSchema *s = station_release_schema_new ();
  s->host = g_strdup (host);
  station_release_schema_set_url (s, "https://{host}/api/v1/repos/{repo}/releases?limit=15");
  return s;
}

StationReleaseSchema *
station_release_schema_new_forgejo (const char *host)
{
  return station_release_schema_new_gitea (host);
}

/* ---- URL builder --------------------------------------------------------- */

char *
station_release_schema_build_url (StationReleaseSchema *self, const char *repo)
{
  g_return_val_if_fail (STATION_IS_RELEASE_SCHEMA (self), NULL);
  g_return_val_if_fail (self->url_template != NULL, NULL);
  char **rp = g_strsplit (repo != NULL ? repo : "", "/", 2);
  const char *owner = rp[0] != NULL ? rp[0] : "";
  const char *name  = (rp[0] != NULL && rp[1] != NULL) ? rp[1] : "";

  GString *s = g_string_new (self->url_template);
  g_string_replace (s, "{host}",  self->host != NULL ? self->host : "", 0);
  g_string_replace (s, "{repo}",  repo != NULL ? repo : "", 0);
  g_string_replace (s, "{owner}", owner, 0);
  g_string_replace (s, "{name}",  name, 0);
  g_strfreev (rp);
  return g_string_free (s, FALSE);
}

/* ---- JSON navigation ----------------------------------------------------- */

/* The node at a dotted member path within @o (e.g. "tag_name" or "meta.tag"),
 * or NULL if any step is missing or not an object. */
static JsonNode *
node_at (JsonObject *o, const char *path)
{
  if (o == NULL || path == NULL || *path == '\0')
    return NULL;
  char **parts = g_strsplit (path, ".", -1);
  JsonNode *cur = NULL;
  JsonObject *obj = o;
  for (guint i = 0; parts[i] != NULL; i++)
    {
      if (obj == NULL || !json_object_has_member (obj, parts[i]))
        { cur = NULL; break; }
      cur = json_object_get_member (obj, parts[i]);
      obj = (cur != NULL && JSON_NODE_HOLDS_OBJECT (cur))
              ? json_node_get_object (cur) : NULL;
    }
  g_strfreev (parts);
  return cur;
}

static char *
dup_string_at (JsonObject *o, const char *path)
{
  JsonNode *n = node_at (o, path);
  if (n != NULL && JSON_NODE_HOLDS_VALUE (n)
      && json_node_get_value_type (n) == G_TYPE_STRING)
    return g_strdup (json_node_get_string (n));
  return g_strdup ("");   /* never NULL: the "available" signal args are non-null */
}

static gboolean
bool_at (JsonObject *o, const char *path)
{
  JsonNode *n = node_at (o, path);
  if (n != NULL && JSON_NODE_HOLDS_VALUE (n)
      && json_node_get_value_type (n) == G_TYPE_BOOLEAN)
    return json_node_get_boolean (n);
  return FALSE;
}

GPtrArray *
station_release_schema_parse (StationReleaseSchema *self,
                              const char *body, gsize len, GError **error)
{
  g_return_val_if_fail (STATION_IS_RELEASE_SCHEMA (self), NULL);

  JsonParser *parser = json_parser_new ();
  if (!json_parser_load_from_data (parser, body, (gssize) len, error))
    { g_object_unref (parser); return NULL; }

  JsonNode *root = json_parser_get_root (parser);
  /* JsonPath always yields an array node of the matches; "$[*]" over an array
   * root gives the release objects. */
  JsonNode *matches = root != NULL
    ? json_path_query (self->releases_path != NULL ? self->releases_path : "$[*]",
                       root, error)
    : NULL;
  if (matches == NULL)
    { g_object_unref (parser); return NULL; }
  if (!JSON_NODE_HOLDS_ARRAY (matches))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "release path did not select an array");
      json_node_unref (matches);
      g_object_unref (parser);
      return NULL;
    }

  JsonArray *arr = json_node_get_array (matches);
  guint count = json_array_get_length (arr);
  GPtrArray *out = g_ptr_array_new_with_free_func ((GDestroyNotify) station_release_free);

  for (guint i = 0; i < count; i++)
    {
      JsonNode *el = json_array_get_element (arr, i);
      if (!JSON_NODE_HOLDS_OBJECT (el))
        continue;
      JsonObject *o = json_node_get_object (el);
      /* Skip drafts where the source marks them (GitHub/Gitea "draft"); absent
       * elsewhere, so harmless. */
      if (json_object_has_member (o, "draft")
          && json_object_get_boolean_member_with_default (o, "draft", FALSE))
        continue;
      char *ver = dup_string_at (o, self->version_field);
      if (ver[0] == '\0') { g_free (ver); continue; }

      StationRelease *r = g_new0 (StationRelease, 1);
      r->version    = ver;
      r->prerelease = bool_at (o, self->prerelease_field);
      r->notes      = dup_string_at (o, self->notes_field);
      r->page_url   = dup_string_at (o, self->page_url_field);
      g_ptr_array_add (out, r);
    }

  json_node_unref (matches);
  g_object_unref (parser);
  return out;
}

/* ---- GObject ------------------------------------------------------------- */

static void
station_release_schema_finalize (GObject *object)
{
  StationReleaseSchema *self = STATION_RELEASE_SCHEMA (object);
  g_clear_pointer (&self->host, g_free);
  g_clear_pointer (&self->url_template, g_free);
  g_clear_pointer (&self->releases_path, g_free);
  g_clear_pointer (&self->version_field, g_free);
  g_clear_pointer (&self->notes_field, g_free);
  g_clear_pointer (&self->page_url_field, g_free);
  g_clear_pointer (&self->prerelease_field, g_free);
  G_OBJECT_CLASS (station_release_schema_parent_class)->finalize (object);
}

static void
station_release_schema_class_init (StationReleaseSchemaClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = station_release_schema_finalize;
}

/* Default to the GitHub-shaped field names so a custom source often only needs
 * set_url (+ set_releases for a non-array-root response). */
static void
station_release_schema_init (StationReleaseSchema *self)
{
  self->releases_path    = g_strdup ("$[*]");
  self->version_field    = g_strdup ("tag_name");
  self->notes_field      = g_strdup ("body");
  self->page_url_field   = g_strdup ("html_url");
  self->prerelease_field = g_strdup ("prerelease");
}
