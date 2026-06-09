/* SPDX-License-Identifier: LGPL-3.0-or-later */
#pragma once

#if !defined(STATION_INSIDE) && !defined(STATION_COMPILATION)
# error "Only <station.h> can be included directly."
#endif

#include <glib.h>

G_BEGIN_DECLS

/* Invoked in the primary instance, on the GLib main loop, when a later instance
 * tries to launch — the app should raise/present its window. */
typedef void (*StationActivateFunc) (void);

/**
 * station_single_instance_acquire:
 * @app_id: a stable application identifier (used to name the lock)
 * @on_activate: (scope forever) (nullable): called in the primary when another instance launches
 *
 * Enforces a single running instance. Returns %TRUE in the first (primary)
 * instance — proceed normally. Returns %FALSE in a later instance after asking
 * the primary to come forward (invoking its @on_activate); that caller should
 * exit immediately.
 *
 * On Windows this is a named mutex plus a hidden message window (GApplication's
 * own uniqueness relies on a D-Bus session bus, which MSYS2/Windows lacks). On
 * other platforms it is a no-op returning %TRUE, since GApplication already makes
 * the app unique there.
 */
gboolean station_single_instance_acquire (const char *app_id,
                                          StationActivateFunc on_activate);

G_END_DECLS
