/* SPDX-License-Identifier: LGPL-3.0-or-later */
#pragma once

#if !defined(STATION_INSIDE) && !defined(STATION_COMPILATION)
# error "Only <station.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

/* A declarative description of a JSON "releases" source for #StationUpdates, so an
 * app can check a self-hosted Gitea/Forgejo, GitHub, or any custom JSON endpoint
 * without the checker hard-coding one provider. The presets fill the schema for
 * the common forges (their releases APIs share GitHub's JSON shape); a custom
 * source is described field by field.
 *
 * The request URL is a template with {host}, {repo} ("owner/name"), {owner} and
 * {name} placeholders. The response is navigated with a json-glib JsonPath for
 * the releases array, plus a member name (dotted paths allowed) for each field. */

#define STATION_TYPE_RELEASE_SCHEMA (station_release_schema_get_type ())
G_DECLARE_FINAL_TYPE (StationReleaseSchema, station_release_schema, STATION, RELEASE_SCHEMA, GObject)

/* Presets — fully-formed schemas for the common forges. @host is the instance
 * (e.g. "git.example.com"); GitHub uses "api.github.com". */
StationReleaseSchema *station_release_schema_new_github  (void);
StationReleaseSchema *station_release_schema_new_gitea   (const char *host);
StationReleaseSchema *station_release_schema_new_forgejo (const char *host);

/* A blank schema to describe a custom source with the setters below. */
StationReleaseSchema *station_release_schema_new (void);

/**
 * station_release_schema_set_url:
 * @tmpl: request URL template, e.g.
 *   "https://{host}/api/v1/repos/{repo}/releases?limit=15". Placeholders:
 *   {host}, {repo} (owner/name), {owner}, {name}.
 */
void station_release_schema_set_url        (StationReleaseSchema *self, const char *tmpl);

/**
 * station_release_schema_set_releases:
 * @jsonpath: a json-glib JsonPath selecting the array of release objects in the
 *   response, e.g. "$[*]" (root is the array) or "$.data[*]".
 */
void station_release_schema_set_releases   (StationReleaseSchema *self, const char *jsonpath);

/* Field accessors within one release object; each is a member name or a dotted
 * path (e.g. "tag_name", or "meta.tag"). */
void station_release_schema_set_version    (StationReleaseSchema *self, const char *field);
void station_release_schema_set_notes      (StationReleaseSchema *self, const char *field);
void station_release_schema_set_page_url   (StationReleaseSchema *self, const char *field);
void station_release_schema_set_prerelease (StationReleaseSchema *self, const char *field);

G_END_DECLS
