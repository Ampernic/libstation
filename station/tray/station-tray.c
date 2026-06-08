/* SPDX-License-Identifier: LGPL-3.0-or-later */
#ifndef STATION_COMPILATION
# define STATION_COMPILATION
#endif
#include "station-tray-private.h"

G_DEFINE_FINAL_TYPE (StationTray, station_tray, G_TYPE_OBJECT)

enum { SIG_ACTIVATE, SIG_ACTIVATED, N_SIGNALS };
static guint signals[N_SIGNALS];

static void
clear_item (gpointer data)
{
  StationTrayItem *it = data;
  g_clear_pointer (&it->label, g_free);
}

/* ---- item store accessors (backends) ------------------------------------ */

guint
_station_tray_item_count (StationTray *self)
{
  return self->items->len;
}

const StationTrayItem *
_station_tray_item_at (StationTray *self, guint index)
{
  if (index >= self->items->len)
    return NULL;
  return &g_array_index (self->items, StationTrayItem, index);
}

void
_station_tray_emit_activate (StationTray *self)
{
  g_signal_emit (self, signals[SIG_ACTIVATE], 0);
}

void
_station_tray_emit_activated (StationTray *self, guint id)
{
  g_signal_emit (self, signals[SIG_ACTIVATED], 0, id);
}

/* ---- public API --------------------------------------------------------- */

StationTray *
station_tray_new (const char *app_id)
{
  g_return_val_if_fail (app_id != NULL, NULL);
  StationTray *self = g_object_new (STATION_TYPE_TRAY, NULL);
  self->app_id = g_strdup (app_id);
  self->icon_name = g_strdup (app_id);
  _station_tray_backend_init (self);
  return self;
}

void
station_tray_set_icon_name (StationTray *self, const char *icon_name)
{
  g_return_if_fail (STATION_IS_TRAY (self));
  g_free (self->icon_name);
  self->icon_name = g_strdup (icon_name);
  _station_tray_backend_update (self);
}

void
station_tray_set_icon_file (StationTray *self, const char *path)
{
  g_return_if_fail (STATION_IS_TRAY (self));
  g_free (self->icon_file);
  self->icon_file = g_strdup (path);
  _station_tray_backend_update (self);
}

void
station_tray_set_tooltip (StationTray *self, const char *tooltip)
{
  g_return_if_fail (STATION_IS_TRAY (self));
  g_free (self->tooltip);
  self->tooltip = g_strdup (tooltip);
  _station_tray_backend_update (self);
}

guint
station_tray_add_item (StationTray *self, const char *label)
{
  g_return_val_if_fail (STATION_IS_TRAY (self), 0);
  StationTrayItem it = { self->next_id++, g_strdup (label), TRUE };
  g_array_append_val (self->items, it);
  _station_tray_backend_update (self);
  return it.id;
}

void
station_tray_add_separator (StationTray *self)
{
  g_return_if_fail (STATION_IS_TRAY (self));
  StationTrayItem it = { 0, NULL, FALSE };
  g_array_append_val (self->items, it);
  _station_tray_backend_update (self);
}

static StationTrayItem *
find_item (StationTray *self, guint id)
{
  for (guint i = 0; i < self->items->len; i++)
    {
      StationTrayItem *it = &g_array_index (self->items, StationTrayItem, i);
      if (it->id == id && id != 0)
        return it;
    }
  return NULL;
}

void
station_tray_set_item_label (StationTray *self, guint id, const char *label)
{
  g_return_if_fail (STATION_IS_TRAY (self));
  StationTrayItem *it = find_item (self, id);
  if (it == NULL)
    return;
  g_free (it->label);
  it->label = g_strdup (label);
  _station_tray_backend_update (self);
}

void
station_tray_set_item_enabled (StationTray *self, guint id, gboolean enabled)
{
  g_return_if_fail (STATION_IS_TRAY (self));
  StationTrayItem *it = find_item (self, id);
  if (it == NULL || it->enabled == enabled)
    return;
  it->enabled = enabled;
  _station_tray_backend_update (self);
}

/* ---- GObject ------------------------------------------------------------ */

static void
station_tray_finalize (GObject *object)
{
  StationTray *self = STATION_TRAY (object);
  _station_tray_backend_dispose (self);
  g_clear_pointer (&self->items, g_array_unref);
  g_clear_pointer (&self->app_id, g_free);
  g_clear_pointer (&self->icon_name, g_free);
  g_clear_pointer (&self->icon_file, g_free);
  g_clear_pointer (&self->tooltip, g_free);
  G_OBJECT_CLASS (station_tray_parent_class)->finalize (object);
}

static void
station_tray_class_init (StationTrayClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = station_tray_finalize;
  signals[SIG_ACTIVATE] = g_signal_new ("activate",
      G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
      G_TYPE_NONE, 0);
  signals[SIG_ACTIVATED] = g_signal_new ("activated",
      G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
      G_TYPE_NONE, 1, G_TYPE_UINT);
}

static void
station_tray_init (StationTray *self)
{
  self->items = g_array_new (FALSE, FALSE, sizeof (StationTrayItem));
  g_array_set_clear_func (self->items, clear_item);
  self->next_id = 1;
}
