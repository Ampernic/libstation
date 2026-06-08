/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Internal coupling between station-updates.c and the schema: the request-URL
 * builder and the JSON parser that turns a response body into release records.
 * Not installed. */
#pragma once

#include "station-updates-schema.h"

G_BEGIN_DECLS

/* One downloadable file of a release. digest is "" unless the source carries one
 * (e.g. Gitea's "sha256:HEX"); the checksums asset is the fallback. */
typedef struct
{
  char *name;
  char *url;
  char *digest;     /* algorithm-prefixed, e.g. "sha256:abc…", or "" */
} StationAsset;

/* One parsed release. version is the raw tag (a leading 'v' is stripped by the
 * caller's version logic). */
typedef struct
{
  char      *version;
  gboolean   prerelease;
  char      *notes;
  char      *page_url;
  GPtrArray *assets;     /* (element-type StationAsset), may be empty */
} StationRelease;

void            station_release_free (StationRelease *r);
StationRelease *station_release_copy (const StationRelease *r);

/* Build the request URL: substitute {host} (from the schema) and
 * {repo}/{owner}/{name} (from @repo, "owner/name"). */
char *station_release_schema_build_url (StationReleaseSchema *self, const char *repo);

/* Parse @body per the schema into a #GPtrArray of #StationRelease* (free func
 * set). NULL + @error on malformed / unexpected JSON. */
GPtrArray *station_release_schema_parse (StationReleaseSchema *self,
                                         const char *body, gsize len, GError **error);

G_END_DECLS
