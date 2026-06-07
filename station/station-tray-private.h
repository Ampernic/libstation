/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include "station-tray.h"

G_BEGIN_DECLS

typedef struct
{
  guint    id;            /* 0 for a separator */
  char    *label;
  gboolean enabled;
} StationTrayItem;

struct _StationTray
{
  GObject   parent_instance;
  char     *app_id;
  char     *icon_name;
  char     *icon_file;
  char     *tooltip;
  GArray   *items;        /* StationTrayItem (cleared with the clear func) */
  guint     next_id;
  gpointer  backend;      /* per-OS native handle */
};

/* Item store accessors for backends. */
guint                  _station_tray_item_count (StationTray *self);
const StationTrayItem *_station_tray_item_at    (StationTray *self, guint index);

/* Emit helpers for backends (called on the main thread). */
void _station_tray_emit_activate  (StationTray *self);
void _station_tray_emit_activated (StationTray *self, guint id);

/* Backend hooks, implemented once per OS in station-tray-<os>.{c,m}. */
void _station_tray_backend_init    (StationTray *self);   /* create the native icon */
void _station_tray_backend_update  (StationTray *self);   /* re-sync icon/tooltip/menu */
void _station_tray_backend_dispose (StationTray *self);   /* remove the native icon */

G_END_DECLS
