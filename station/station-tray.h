/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#if !defined(STATION_INSIDE) && !defined(STATION_COMPILATION)
# error "Only <station.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

/* A system-tray / status-area icon with a right-click menu, backed by the native
 * mechanism per OS: StatusNotifierItem (D-Bus) on Linux, NSStatusItem on macOS,
 * Shell_NotifyIcon on Windows. GTK4 has no portable status icon; this fills it.
 *
 * Build a menu with station_tray_add_item()/add_separator(); a click emits
 * "activated" with the item id. A primary (left) click emits "activate". */

#define STATION_TYPE_TRAY (station_tray_get_type ())
G_DECLARE_FINAL_TYPE (StationTray, station_tray, STATION, TRAY, GObject)

/**
 * station_tray_new:
 * @app_id: reverse-DNS id (used as the SNI id and the default themed icon name)
 */
StationTray *station_tray_new (const char *app_id);

/* Themed icon name (Linux); macOS/Windows prefer the file set below. */
void  station_tray_set_icon_name (StationTray *self, const char *icon_name);
/* Image file used as the icon on macOS/Windows (and as a Linux pixmap fallback). */
void  station_tray_set_icon_file (StationTray *self, const char *path);
void  station_tray_set_tooltip   (StationTray *self, const char *tooltip);

/* Append a clickable item; returns its id (>0). */
guint station_tray_add_item        (StationTray *self, const char *label);
void  station_tray_add_separator   (StationTray *self);
void  station_tray_set_item_label  (StationTray *self, guint id, const char *label);
void  station_tray_set_item_enabled(StationTray *self, guint id, gboolean enabled);

/* Signals:
 *   "activate"            — primary (left) click on the icon
 *   "activated" (guint id)— a menu item was chosen
 */

G_END_DECLS
