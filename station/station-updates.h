/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#if !defined(STATION_INSIDE) && !defined(STATION_COMPILATION)
# error "Only <station.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

/* Checks a project's GitHub releases for a version newer than the running build
 * and reports it, so an app distributed outside a managing repository (Windows,
 * macOS, Android, AppImage) can notify the user. It only checks + reports — the
 * download/install is the app's call (binaries are typically unsigned, so an
 * auto-installer would need its own signature verification). The fetch uses a GIO
 * TLS socket (no HTTP-library dependency, works on every platform incl. Android).
 *
 * "Whether to check at all" is policy the caller owns (e.g. skip on a Linux distro
 * package that updates via its repo) — this object is just the mechanism. */

#define STATION_TYPE_UPDATES (station_updates_get_type ())
G_DECLARE_FINAL_TYPE (StationUpdates, station_updates, STATION, UPDATES, GObject)

/**
 * station_updates_new:
 * @repo: GitHub "owner/name" whose Releases are checked
 * @current_version: this build's version (e.g. "2.0.0" or "2.0.0-beta1"); a
 *   leading "v" is ignored
 */
StationUpdates *station_updates_new (const char *repo, const char *current_version);

/**
 * station_updates_add_channel:
 * @self: a #StationUpdates
 * @id: channel identifier (e.g. "beta"); compared case-insensitively
 * @prerelease_labels: (array zero-terminated=1) (nullable): prerelease keywords
 *   the channel accepts — a release tagged "<ver>-<label>…" qualifies when
 *   <label> starts with one of these (e.g. {"beta","rc"}). NULL or empty = none.
 *
 * Registers a selectable channel. This is the developer-side definition of the
 * update channels an app offers; registering is optional (set_channel also takes
 * an unregistered id, treating it as a single accepted label). A stable release
 * qualifies for every channel, so a channel only ever *adds* prereleases. The
 * implicit "stable" channel (the default) accepts releases only.
 */
void station_updates_add_channel (StationUpdates *self, const char *id,
                                  const char *const *prerelease_labels);

/**
 * station_updates_set_channel:
 * @self: a #StationUpdates
 * @id: (nullable): channel id; NULL, "" or "stable" means stable releases only
 */
void station_updates_set_channel (StationUpdates *self, const char *id);

/**
 * station_updates_get_channel:
 * @self: a #StationUpdates
 *
 * Returns: the active channel id.
 */
const char *station_updates_get_channel (StationUpdates *self);

/**
 * station_updates_dup_channels:
 * @self: a #StationUpdates
 *
 * Returns: (array zero-terminated=1) (transfer full): the registered channel ids
 *   in registration order, for building an in-app selector. Free with g_strfreev.
 */
char **station_updates_dup_channels (StationUpdates *self);

/**
 * station_updates_check:
 * @self: a #StationUpdates
 *
 * Asynchronously checks for the newest release that qualifies for the active
 * channel; if it is newer than @current_version, emits "available". Safe to call
 * once per launch.
 */
void station_updates_check (StationUpdates *self);

/**
 * station_updates_download:
 * @self: a #StationUpdates
 * @url: a direct download URL (https; redirects are followed)
 * @dest_path: filesystem path to write the asset to
 *
 * Asynchronously downloads @url to @dest_path over the same transport as the
 * check (GIO TLS on desktop, java.net on Android). Emits "download-progress"
 * as it runs and then "downloaded" or "download-failed". The app picks the asset
 * URL (deterministic from the release tag and its naming scheme) and applies the
 * file once "downloaded" fires.
 */
void station_updates_download (StationUpdates *self, const char *url,
                               const char *dest_path);

/* Check signals — exactly one fires per station_updates_check():
 *   "available" (const char *version, const char *url, const char *notes):
 *       a newer release exists. version has no leading "v"; url is its release page.
 *   "up-to-date" (void): the check succeeded and no newer release applies.
 *   "failed" (const char *reason): the check could not complete (network/TLS,
 *       HTTP status, malformed response); reason is a human-readable diagnostic.
 *
 * Download signals — per station_updates_download():
 *   "download-progress" (double fraction): 0..1, or -1 when the size is unknown.
 *   "downloaded" (const char *path): the asset is at @path.
 *   "download-failed" (const char *reason). */

G_END_DECLS
