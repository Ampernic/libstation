/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Linux backend: StatusNotifierItem (org.kde.StatusNotifierItem) + its
 * com.canonical.dbusmenu, hand-rolled over GDBus. Ported from the Vala tray the
 * TGProxy app shipped. No SNI host (no tray extension) → graceful no-op. */
#ifndef STATION_COMPILATION
# define STATION_COMPILATION
#endif
#include "station-tray-private.h"

#include <gio/gio.h>
#include <unistd.h>

#define WATCHER_NAME "org.kde.StatusNotifierWatcher"
#define WATCHER_PATH "/StatusNotifierWatcher"

typedef struct
{
  GDBusConnection *conn;
  GDBusNodeInfo   *sni_info;
  GDBusNodeInfo   *menu_info;
  char            *bus_name;
  guint            own_id;
  guint            watch_id;
  guint            item_reg;
  guint            menu_reg;
  guint            revision;
} Sni;

static const char SNI_XML[] =
  "<node><interface name='org.kde.StatusNotifierItem'>"
  "<property name='Category' type='s' access='read'/>"
  "<property name='Id' type='s' access='read'/>"
  "<property name='Title' type='s' access='read'/>"
  "<property name='Status' type='s' access='read'/>"
  "<property name='IconName' type='s' access='read'/>"
  "<property name='ItemIsMenu' type='b' access='read'/>"
  "<property name='Menu' type='o' access='read'/>"
  "<property name='ToolTip' type='(sa(iiay)ss)' access='read'/>"
  "<method name='Activate'><arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
  "<method name='SecondaryActivate'><arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
  "<method name='ContextMenu'><arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
  "<method name='Scroll'><arg type='i' direction='in'/><arg type='s' direction='in'/></method>"
  "<signal name='NewIcon'/><signal name='NewToolTip'/>"
  "<signal name='NewStatus'><arg type='s'/></signal>"
  "</interface></node>";

static const char MENU_XML[] =
  "<node><interface name='com.canonical.dbusmenu'>"
  "<property name='Version' type='u' access='read'/>"
  "<property name='Status' type='s' access='read'/>"
  "<property name='TextDirection' type='s' access='read'/>"
  "<method name='GetLayout'>"
  "<arg type='i' direction='in'/><arg type='i' direction='in'/><arg type='as' direction='in'/>"
  "<arg type='u' direction='out'/><arg type='(ia{sv}av)' direction='out'/></method>"
  "<method name='GetGroupProperties'>"
  "<arg type='ai' direction='in'/><arg type='as' direction='in'/>"
  "<arg type='a(ia{sv})' direction='out'/></method>"
  "<method name='GetProperty'><arg type='i' direction='in'/><arg type='s' direction='in'/><arg type='v' direction='out'/></method>"
  "<method name='Event'><arg type='i' direction='in'/><arg type='s' direction='in'/><arg type='v' direction='in'/><arg type='u' direction='in'/></method>"
  "<method name='AboutToShow'><arg type='i' direction='in'/><arg type='b' direction='out'/></method>"
  "<signal name='ItemsPropertiesUpdated'><arg type='a(ia{sv})'/><arg type='a(ias)'/></signal>"
  "<signal name='LayoutUpdated'><arg type='u'/><arg type='i'/></signal>"
  "<signal name='ItemActivationRequested'><arg type='i'/><arg type='u'/></signal>"
  "</interface></node>";

/* ---- menu layout builders ----------------------------------------------- */

static GVariant *
menu_item_props (StationTray *self, int nodeid)
{
  GVariantBuilder b;
  g_variant_builder_init (&b, G_VARIANT_TYPE ("a{sv}"));
  if (nodeid == 0)
    {
      g_variant_builder_add (&b, "{sv}", "children-display",
                             g_variant_new_string ("submenu"));
      return g_variant_builder_end (&b);
    }
  const StationTrayItem *it = _station_tray_item_at (self, (guint) (nodeid - 1));
  if (it == NULL)
    return g_variant_builder_end (&b);
  g_variant_builder_add (&b, "{sv}", "visible", g_variant_new_boolean (TRUE));
  if (it->id == 0)
    g_variant_builder_add (&b, "{sv}", "type", g_variant_new_string ("separator"));
  else
    {
      g_variant_builder_add (&b, "{sv}", "label",
                             g_variant_new_string (it->label ? it->label : ""));
      g_variant_builder_add (&b, "{sv}", "enabled",
                             g_variant_new_boolean (it->enabled));
    }
  return g_variant_builder_end (&b);
}

static GVariant *
menu_node (StationTray *self, int nodeid)
{
  GVariantBuilder kids;
  g_variant_builder_init (&kids, G_VARIANT_TYPE ("av"));
  if (nodeid == 0)
    {
      guint n = _station_tray_item_count (self);
      for (guint i = 0; i < n; i++)
        g_variant_builder_add_value (&kids,
            g_variant_new_variant (menu_node (self, (int) (i + 1))));
    }
  return g_variant_new ("(i@a{sv}av)", nodeid, menu_item_props (self, nodeid), &kids);
}

/* ---- dbusmenu vtable ---------------------------------------------------- */

static void
menu_method (GDBusConnection *conn G_GNUC_UNUSED, const char *sender G_GNUC_UNUSED,
             const char *path G_GNUC_UNUSED, const char *iface G_GNUC_UNUSED,
             const char *method, GVariant *params,
             GDBusMethodInvocation *inv, gpointer user_data)
{
  StationTray *self = user_data;
  Sni *s = self->backend;

  if (g_strcmp0 (method, "GetLayout") == 0)
    {
      g_dbus_method_invocation_return_value (inv,
          g_variant_new ("(u@(ia{sv}av))", s->revision, menu_node (self, 0)));
    }
  else if (g_strcmp0 (method, "GetGroupProperties") == 0)
    {
      GVariantBuilder out;
      g_variant_builder_init (&out, G_VARIANT_TYPE ("a(ia{sv})"));
      g_variant_builder_add (&out, "(i@a{sv})", 0, menu_item_props (self, 0));
      guint n = _station_tray_item_count (self);
      for (guint i = 0; i < n; i++)
        g_variant_builder_add (&out, "(i@a{sv})", (int) (i + 1),
                               menu_item_props (self, (int) (i + 1)));
      g_dbus_method_invocation_return_value (inv,
          g_variant_new ("(a(ia{sv}))", &out));
    }
  else if (g_strcmp0 (method, "GetProperty") == 0)
    {
      int id; const char *name;
      g_variant_get (params, "(i&s)", &id, &name);
      GVariant *props = menu_item_props (self, id);
      GVariant *val = g_variant_lookup_value (props, name, NULL);
      g_dbus_method_invocation_return_value (inv,
          g_variant_new ("(v)", val ? val : g_variant_new_boolean (FALSE)));
      g_variant_unref (props);
    }
  else if (g_strcmp0 (method, "Event") == 0)
    {
      gint32 id; const char *event; GVariant *data = NULL; guint32 ts;
      g_variant_get (params, "(i&s@vu)", &id, &event, &data, &ts);
      if (g_strcmp0 (event, "clicked") == 0 && id > 0)
        {
          const StationTrayItem *it = _station_tray_item_at (self, (guint) (id - 1));
          if (it != NULL && it->id != 0)
            _station_tray_emit_activated (self, it->id);
        }
      if (data != NULL)
        g_variant_unref (data);
      g_dbus_method_invocation_return_value (inv, NULL);
    }
  else if (g_strcmp0 (method, "AboutToShow") == 0)
    {
      g_dbus_method_invocation_return_value (inv, g_variant_new ("(b)", FALSE));
    }
  else
    {
      g_dbus_method_invocation_return_value (inv, NULL);
    }
}

static GVariant *
menu_get_property (GDBusConnection *conn G_GNUC_UNUSED, const char *sender G_GNUC_UNUSED,
                   const char *path G_GNUC_UNUSED, const char *iface G_GNUC_UNUSED,
                   const char *prop, GError **error G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
  if (g_strcmp0 (prop, "Version") == 0)       return g_variant_new_uint32 (3);
  if (g_strcmp0 (prop, "Status") == 0)        return g_variant_new_string ("normal");
  if (g_strcmp0 (prop, "TextDirection") == 0) return g_variant_new_string ("ltr");
  return NULL;
}

/* ---- SNI item vtable ---------------------------------------------------- */

static void
sni_method (GDBusConnection *conn G_GNUC_UNUSED, const char *sender G_GNUC_UNUSED,
            const char *path G_GNUC_UNUSED, const char *iface G_GNUC_UNUSED,
            const char *method, GVariant *params G_GNUC_UNUSED,
            GDBusMethodInvocation *inv, gpointer user_data)
{
  StationTray *self = user_data;
  if (g_strcmp0 (method, "Activate") == 0 || g_strcmp0 (method, "SecondaryActivate") == 0)
    _station_tray_emit_activate (self);
  /* ContextMenu: the host shows the dbusmenu itself (ItemIsMenu=true). */
  g_dbus_method_invocation_return_value (inv, NULL);
}

static GVariant *
sni_get_property (GDBusConnection *conn G_GNUC_UNUSED, const char *sender G_GNUC_UNUSED,
                  const char *path G_GNUC_UNUSED, const char *iface G_GNUC_UNUSED,
                  const char *prop, GError **error G_GNUC_UNUSED, gpointer user_data)
{
  StationTray *self = user_data;
  if (g_strcmp0 (prop, "Category") == 0)   return g_variant_new_string ("ApplicationStatus");
  if (g_strcmp0 (prop, "Id") == 0)         return g_variant_new_string (self->app_id);
  if (g_strcmp0 (prop, "Title") == 0)      return g_variant_new_string (self->app_id);
  if (g_strcmp0 (prop, "Status") == 0)     return g_variant_new_string ("Active");
  if (g_strcmp0 (prop, "IconName") == 0)   return g_variant_new_string (self->icon_name ? self->icon_name : "");
  if (g_strcmp0 (prop, "ItemIsMenu") == 0) return g_variant_new_boolean (TRUE);
  if (g_strcmp0 (prop, "Menu") == 0)       return g_variant_new_object_path ("/MenuBar");
  if (g_strcmp0 (prop, "ToolTip") == 0)
    {
      GVariantBuilder pix;
      g_variant_builder_init (&pix, G_VARIANT_TYPE ("a(iiay)"));
      return g_variant_new ("(sa(iiay)ss)",
          self->icon_name ? self->icon_name : "", &pix,
          self->app_id, self->tooltip ? self->tooltip : "");
    }
  return NULL;
}

/* ---- registration ------------------------------------------------------- */

static void
sni_register_item (StationTray *self)
{
  Sni *s = self->backend;
  if (s->item_reg == 0)
    return;
  g_dbus_connection_call (s->conn, WATCHER_NAME, WATCHER_PATH, WATCHER_NAME,
      "RegisterStatusNotifierItem", g_variant_new ("(s)", s->bus_name),
      NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static void
on_name_acquired (GDBusConnection *conn, const char *name G_GNUC_UNUSED, gpointer user_data)
{
  StationTray *self = user_data;
  Sni *s = self->backend;
  static const GDBusInterfaceVTable item_vt = { sni_method, sni_get_property, NULL, { 0 } };
  static const GDBusInterfaceVTable menu_vt = { menu_method, menu_get_property, NULL, { 0 } };
  s->item_reg = g_dbus_connection_register_object (conn, "/StatusNotifierItem",
      s->sni_info->interfaces[0], &item_vt, self, NULL, NULL);
  s->menu_reg = g_dbus_connection_register_object (conn, "/MenuBar",
      s->menu_info->interfaces[0], &menu_vt, self, NULL, NULL);
  sni_register_item (self);
}

static void
on_watcher_up (GDBusConnection *conn G_GNUC_UNUSED, const char *name G_GNUC_UNUSED,
               const char *owner G_GNUC_UNUSED, gpointer user_data)
{
  sni_register_item (user_data);
}

void
_station_tray_backend_init (StationTray *self)
{
  Sni *s = g_new0 (Sni, 1);
  self->backend = s;
  s->revision = 1;

  s->conn = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
  if (s->conn == NULL)
    return;   /* no session bus: graceful no-op */

  s->sni_info = g_dbus_node_info_new_for_xml (SNI_XML, NULL);
  s->menu_info = g_dbus_node_info_new_for_xml (MENU_XML, NULL);
  s->bus_name = g_strdup_printf ("org.kde.StatusNotifierItem-%d-1", (int) getpid ());

  s->own_id = g_bus_own_name_on_connection (s->conn, s->bus_name,
      G_BUS_NAME_OWNER_FLAGS_NONE, on_name_acquired, NULL, self, NULL);
  s->watch_id = g_bus_watch_name (G_BUS_TYPE_SESSION, WATCHER_NAME,
      G_BUS_NAME_WATCHER_FLAGS_NONE, on_watcher_up, NULL, self, NULL);
}

void
_station_tray_backend_update (StationTray *self)
{
  Sni *s = self->backend;
  if (s == NULL || s->conn == NULL || s->menu_reg == 0)
    return;
  s->revision++;
  g_dbus_connection_emit_signal (s->conn, NULL, "/StatusNotifierItem",
      "org.kde.StatusNotifierItem", "NewIcon", NULL, NULL);
  g_dbus_connection_emit_signal (s->conn, NULL, "/StatusNotifierItem",
      "org.kde.StatusNotifierItem", "NewToolTip", NULL, NULL);
  g_dbus_connection_emit_signal (s->conn, NULL, "/MenuBar",
      "com.canonical.dbusmenu", "LayoutUpdated",
      g_variant_new ("(ui)", s->revision, 0), NULL);
}

void
_station_tray_backend_dispose (StationTray *self)
{
  Sni *s = self->backend;
  if (s == NULL)
    return;
  if (s->watch_id) g_bus_unwatch_name (s->watch_id);
  if (s->own_id)   g_bus_unown_name (s->own_id);
  if (s->conn && s->item_reg) g_dbus_connection_unregister_object (s->conn, s->item_reg);
  if (s->conn && s->menu_reg) g_dbus_connection_unregister_object (s->conn, s->menu_reg);
  g_clear_pointer (&s->sni_info, g_dbus_node_info_unref);
  g_clear_pointer (&s->menu_info, g_dbus_node_info_unref);
  g_clear_object (&s->conn);
  g_free (s->bus_name);
  g_free (s);
  self->backend = NULL;
}
