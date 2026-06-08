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
 * station_updates_check:
 * @self: a #StationUpdates
 * @include_prerelease: also consider -beta/-rc releases (use when the running
 *   build is itself a prerelease)
 *
 * Asynchronously checks the newest applicable release; if it is newer than
 * @current_version, emits "available". Safe to call once per launch.
 */
void station_updates_check (StationUpdates *self, gboolean include_prerelease);

/* Exactly one of these three signals fires per station_updates_check():
 *   "available" (const char *version, const char *url, const char *notes):
 *       a newer release exists. version has no leading "v"; url is its release page.
 *   "up-to-date" (void): the check succeeded and no newer release applies.
 *   "failed" (const char *reason): the check could not complete (network/TLS,
 *       HTTP status, malformed response); reason is a human-readable diagnostic. */

G_END_DECLS
