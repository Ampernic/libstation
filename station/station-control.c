/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef STATION_COMPILATION
# define STATION_COMPILATION
#endif
#include "station-control.h"

#include <string.h>
#include <glib/gstdio.h>
#ifndef G_OS_WIN32
# include <gio/gunixsocketaddress.h>
#endif

/* ---- endpoint helpers ---------------------------------------------------- */

#ifndef G_OS_WIN32
static char *
posix_sock_path (const char *name)
{
  char *file = g_strconcat (name, ".sock", NULL);
  char *path = g_build_filename (g_get_user_runtime_dir (), file, NULL);
  g_free (file);
  return path;
}
#else
static char *
win_endpoint_path (const char *name)
{
  char *file = g_strconcat (name, ".control", NULL);
  char *path = g_build_filename (g_get_user_config_dir (), file, NULL);
  g_free (file);
  return path;
}

static char *
gen_token (void)
{
  GString *s = g_string_new (NULL);
  for (int i = 0; i < 16; i++)
    g_string_append_printf (s, "%02x", g_random_int_range (0, 256));
  return g_string_free (s, FALSE);
}
#endif

/* ======================================================================== */
/*  Server                                                                   */
/* ======================================================================== */

struct _StationControlServer
{
  GObject         parent_instance;
  char           *name;
  GSocketService *service;
  GList          *conns;       /* ServerConn* */
  char           *endpoint;
#ifdef G_OS_WIN32
  char           *token;
#endif
};

G_DEFINE_FINAL_TYPE (StationControlServer, station_control_server, G_TYPE_OBJECT)

enum { SERVER_COMMAND, SERVER_N_SIGNALS };
static guint server_signals[SERVER_N_SIGNALS];

typedef struct
{
  StationControlServer *self;     /* strong ref: keeps the server alive while open */
  GSocketConnection    *conn;     /* strong ref */
  GDataInputStream     *in;
  GCancellable         *cancel;
  gboolean              authed;
} ServerConn;

static void server_read_next (ServerConn *sc);

/* Tear down one connection and release its hold on the server. */
static void
server_conn_drop (ServerConn *sc)
{
  StationControlServer *self = sc->self;
  self->conns = g_list_remove (self->conns, sc);
  g_clear_object (&sc->in);
  g_io_stream_close (G_IO_STREAM (sc->conn), NULL, NULL);
  g_object_unref (sc->conn);
  g_object_unref (sc->cancel);
  g_object_unref (self);   /* may finalize the server */
  g_free (sc);
}

static void
server_read_cb (GObject *src, GAsyncResult *res, gpointer data)
{
  ServerConn *sc = data;
  g_autoptr (GError) error = NULL;
  char *line = g_data_input_stream_read_line_finish (G_DATA_INPUT_STREAM (src),
                                                     res, NULL, &error);
  if (line == NULL)   /* EOF, error, or cancelled */
    {
      server_conn_drop (sc);
      return;
    }

#ifdef G_OS_WIN32
  if (!sc->authed)
    {
      gboolean ok = sc->self->token
        && g_strcmp0 (g_strstrip (line), sc->self->token) == 0;
      g_free (line);
      if (!ok)
        {
          server_conn_drop (sc);
          return;
        }
      sc->authed = TRUE;
      server_read_next (sc);
      return;
    }
#endif

  g_signal_emit (sc->self, server_signals[SERVER_COMMAND], 0, line);
  g_free (line);
  server_read_next (sc);
}

static void
server_read_next (ServerConn *sc)
{
  g_data_input_stream_read_line_async (sc->in, G_PRIORITY_DEFAULT, sc->cancel,
                                       server_read_cb, sc);
}

static gboolean
server_incoming (GSocketService    *service G_GNUC_UNUSED,
                 GSocketConnection *conn,
                 GObject           *source G_GNUC_UNUSED,
                 gpointer           data)
{
  StationControlServer *self = data;
  ServerConn *sc = g_new0 (ServerConn, 1);
  sc->self = g_object_ref (self);
  sc->conn = g_object_ref (conn);
  sc->in = g_data_input_stream_new (g_io_stream_get_input_stream (G_IO_STREAM (conn)));
  sc->cancel = g_cancellable_new ();
#ifdef G_OS_WIN32
  sc->authed = FALSE;
#else
  sc->authed = TRUE;
#endif
  self->conns = g_list_prepend (self->conns, sc);
  server_read_next (sc);
  return FALSE;
}

StationControlServer *
station_control_server_new (const char *name)
{
  g_return_val_if_fail (name != NULL, NULL);
  StationControlServer *self = g_object_new (STATION_TYPE_CONTROL_SERVER, NULL);
  self->name = g_strdup (name);
  return self;
}

gboolean
station_control_server_start (StationControlServer *self,
                              GError              **error)
{
  g_return_val_if_fail (STATION_IS_CONTROL_SERVER (self), FALSE);
  g_return_val_if_fail (self->service == NULL, FALSE);

  self->service = g_socket_service_new ();

#ifdef G_OS_WIN32
  GInetAddress *loop = g_inet_address_new_loopback (G_SOCKET_FAMILY_IPV4);
  GSocketAddress *addr = g_inet_socket_address_new (loop, 0);
  g_object_unref (loop);
  GSocketAddress *effective = NULL;
  gboolean ok = g_socket_listener_add_address (G_SOCKET_LISTENER (self->service),
      addr, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_TCP, NULL, &effective, error);
  g_object_unref (addr);
  if (!ok)
    {
      g_clear_object (&self->service);
      return FALSE;
    }
  guint16 port = g_inet_socket_address_get_port (G_INET_SOCKET_ADDRESS (effective));
  g_object_unref (effective);
  self->token = gen_token ();
  self->endpoint = win_endpoint_path (self->name);
  /* The endpoint file holds the auth token for the loopback control port. It
   * lives under the per-user config dir (g_get_user_config_dir), so other local
   * users can't read it under default Windows ACLs — that directory's
   * permissions are the protection (the POSIX path locks the socket to 0600 for
   * the same reason). A tighter per-file ACL would need SetNamedSecurityInfo. */
  char *content = g_strdup_printf ("%u\n%s\n", port, self->token);
  ok = g_file_set_contents (self->endpoint, content, -1, error);
  g_free (content);
  if (!ok)
    {
      g_clear_object (&self->service);
      return FALSE;
    }
#else
  self->endpoint = posix_sock_path (self->name);
  if (g_file_test (self->endpoint, G_FILE_TEST_EXISTS))
    g_unlink (self->endpoint);   /* stale socket from a crash */
  GSocketAddress *addr = g_unix_socket_address_new (self->endpoint);
  gboolean ok = g_socket_listener_add_address (G_SOCKET_LISTENER (self->service),
      addr, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT, NULL, NULL, error);
  g_object_unref (addr);
  if (!ok)
    {
      g_clear_object (&self->service);
      return FALSE;
    }
  g_chmod (self->endpoint, 0600);
#endif

  g_signal_connect (self->service, "incoming", G_CALLBACK (server_incoming), self);
  g_socket_service_start (self->service);
  return TRUE;
}

void
station_control_server_broadcast (StationControlServer *self,
                                  const char           *line)
{
  g_return_if_fail (STATION_IS_CONTROL_SERVER (self));
  g_return_if_fail (line != NULL);
  char *out = g_strconcat (line, "\n", NULL);
  for (GList *l = self->conns; l != NULL; l = l->next)
    {
      ServerConn *sc = l->data;
      if (sc->authed)
        {
          GOutputStream *os = g_io_stream_get_output_stream (G_IO_STREAM (sc->conn));
          g_output_stream_write_all (os, out, strlen (out), NULL, NULL, NULL);
        }
    }
  g_free (out);
}

void
station_control_server_stop (StationControlServer *self)
{
  g_return_if_fail (STATION_IS_CONTROL_SERVER (self));
  if (self->service)
    {
      g_socket_service_stop (self->service);
      g_clear_object (&self->service);
    }
  /* Cancel each read; its callback runs server_conn_drop (which frees sc and
   * releases the server ref). Iterate a copy — the list mutates as they drop. */
  for (GList *l = g_list_copy (self->conns); l != NULL; l = g_list_delete_link (l, l))
    g_cancellable_cancel (((ServerConn *) l->data)->cancel);
  if (self->endpoint)
    {
      g_unlink (self->endpoint);
      g_clear_pointer (&self->endpoint, g_free);
    }
}

static void
station_control_server_finalize (GObject *object)
{
  StationControlServer *self = STATION_CONTROL_SERVER (object);
  /* conns hold a ref, so finalize only runs once they're all gone. */
  if (self->service)
    {
      g_socket_service_stop (self->service);
      g_clear_object (&self->service);
    }
  g_clear_pointer (&self->endpoint, g_free);
  g_clear_pointer (&self->name, g_free);
#ifdef G_OS_WIN32
  g_clear_pointer (&self->token, g_free);
#endif
  G_OBJECT_CLASS (station_control_server_parent_class)->finalize (object);
}

static void
station_control_server_class_init (StationControlServerClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = station_control_server_finalize;
  server_signals[SERVER_COMMAND] = g_signal_new ("command",
      G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
      G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
station_control_server_init (StationControlServer *self G_GNUC_UNUSED)
{
}

/* ======================================================================== */
/*  Client                                                                   */
/* ======================================================================== */

struct _StationControlClient
{
  GObject            parent_instance;
  char              *name;
  gboolean           want;
  gboolean           connected;
  GSocketConnection *conn;
  GOutputStream     *out;        /* unowned */
  GDataInputStream  *in;
};

G_DEFINE_FINAL_TYPE (StationControlClient, station_control_client, G_TYPE_OBJECT)

enum { CLIENT_MESSAGE, CLIENT_CONNECTED, CLIENT_N_SIGNALS };
static guint client_signals[CLIENT_N_SIGNALS];

static void client_try_connect (StationControlClient *self);

static void
client_set_connected (StationControlClient *self, gboolean connected)
{
  if (self->connected == connected)
    return;
  self->connected = connected;
  g_signal_emit (self, client_signals[CLIENT_CONNECTED], 0, connected);
}

static void
client_close (StationControlClient *self)
{
  g_clear_object (&self->in);
  if (self->conn)
    {
      g_io_stream_close (G_IO_STREAM (self->conn), NULL, NULL);
      g_clear_object (&self->conn);
    }
  self->out = NULL;
  client_set_connected (self, FALSE);
}

static gboolean
client_retry_cb (gpointer data)
{
  StationControlClient *self = data;
  if (self->want && !self->connected)
    client_try_connect (self);
  g_object_unref (self);
  return G_SOURCE_REMOVE;
}

static void
client_schedule_retry (StationControlClient *self)
{
  if (self->want)
    g_timeout_add_seconds (1, client_retry_cb, g_object_ref (self));
}

static void
client_read_cb (GObject *src, GAsyncResult *res, gpointer data)
{
  StationControlClient *self = data;   /* carries the read-loop ref */
  g_autoptr (GError) error = NULL;
  char *line = g_data_input_stream_read_line_finish (G_DATA_INPUT_STREAM (src),
                                                     res, NULL, &error);
  /* Stale: the client was closed/stopped while this read was in flight. */
  if (line == NULL || !self->want || (gpointer) src != (gpointer) self->in)
    {
      g_free (line);
      if (line == NULL && self->want)
        {
          client_close (self);
          client_schedule_retry (self);
        }
      g_object_unref (self);
      return;
    }
  g_signal_emit (self, client_signals[CLIENT_MESSAGE], 0, line);
  g_free (line);
  g_data_input_stream_read_line_async (self->in, G_PRIORITY_DEFAULT, NULL,
                                       client_read_cb, self);   /* reuse the ref */
}

static void
client_connect_cb (GObject *src, GAsyncResult *res, gpointer data)
{
  StationControlClient *self = data;
  g_autoptr (GError) error = NULL;
  GSocketConnection *conn = g_socket_client_connect_finish (G_SOCKET_CLIENT (src),
                                                            res, &error);
  if (conn == NULL || !self->want)
    {
      g_clear_object (&conn);
      if (self->want)
        client_schedule_retry (self);
      g_object_unref (self);   /* release the connect ref */
      return;
    }
  self->conn = conn;
  self->out = g_io_stream_get_output_stream (G_IO_STREAM (conn));
  self->in = g_data_input_stream_new (g_io_stream_get_input_stream (G_IO_STREAM (conn)));

#ifdef G_OS_WIN32
  char *tok = g_object_steal_data (G_OBJECT (self), "station-token");
  if (tok)
    {
      char *l = g_strconcat (tok, "\n", NULL);
      g_output_stream_write_all (self->out, l, strlen (l), NULL, NULL, NULL);
      g_free (l);
      g_free (tok);
    }
#endif

  client_set_connected (self, TRUE);
  g_data_input_stream_read_line_async (self->in, G_PRIORITY_DEFAULT, NULL,
                                       client_read_cb, self);   /* reuse the ref */
}

static void
client_try_connect (StationControlClient *self)
{
  GSocketClient *client = g_socket_client_new ();
  GSocketAddress *addr = NULL;

#ifdef G_OS_WIN32
  char *endpoint = win_endpoint_path (self->name);
  char *content = NULL;
  gboolean got = g_file_get_contents (endpoint, &content, NULL, NULL);
  g_free (endpoint);
  if (!got)
    {
      g_object_unref (client);
      client_schedule_retry (self);
      return;
    }
  char **lines = g_strsplit (content, "\n", 3);
  g_free (content);
  guint port = (lines[0]) ? (guint) g_ascii_strtoull (lines[0], NULL, 10) : 0;
  if (port == 0)
    {
      g_strfreev (lines);
      g_object_unref (client);
      client_schedule_retry (self);
      return;
    }
  g_object_set_data_full (G_OBJECT (self), "station-token",
      g_strdup (lines[1] ? g_strstrip (lines[1]) : ""), g_free);
  g_strfreev (lines);
  GInetAddress *loop = g_inet_address_new_loopback (G_SOCKET_FAMILY_IPV4);
  addr = g_inet_socket_address_new (loop, port);
  g_object_unref (loop);
#else
  char *path = posix_sock_path (self->name);
  gboolean exists = g_file_test (path, G_FILE_TEST_EXISTS);
  if (exists)
    addr = g_unix_socket_address_new (path);
  g_free (path);
  if (!exists)
    {
      g_object_unref (client);
      client_schedule_retry (self);
      return;
    }
#endif

  g_socket_client_connect_async (client, G_SOCKET_CONNECTABLE (addr), NULL,
                                 client_connect_cb, g_object_ref (self));
  g_object_unref (addr);
  g_object_unref (client);
}

StationControlClient *
station_control_client_new (const char *name)
{
  g_return_val_if_fail (name != NULL, NULL);
  StationControlClient *self = g_object_new (STATION_TYPE_CONTROL_CLIENT, NULL);
  self->name = g_strdup (name);
  return self;
}

void
station_control_client_start (StationControlClient *self)
{
  g_return_if_fail (STATION_IS_CONTROL_CLIENT (self));
  if (self->want)
    return;
  self->want = TRUE;
  client_try_connect (self);
}

void
station_control_client_stop (StationControlClient *self)
{
  g_return_if_fail (STATION_IS_CONTROL_CLIENT (self));
  self->want = FALSE;
  client_close (self);
}

void
station_control_client_send (StationControlClient *self,
                             const char           *line)
{
  g_return_if_fail (STATION_IS_CONTROL_CLIENT (self));
  if (self->out == NULL || line == NULL)
    return;
  char *l = g_strconcat (line, "\n", NULL);
  g_output_stream_write_all (self->out, l, strlen (l), NULL, NULL, NULL);
  g_free (l);
}

static void
station_control_client_finalize (GObject *object)
{
  StationControlClient *self = STATION_CONTROL_CLIENT (object);
  self->want = FALSE;
  client_close (self);
  g_clear_pointer (&self->name, g_free);
  G_OBJECT_CLASS (station_control_client_parent_class)->finalize (object);
}

static void
station_control_client_class_init (StationControlClientClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = station_control_client_finalize;
  client_signals[CLIENT_MESSAGE] = g_signal_new ("message",
      G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
      G_TYPE_NONE, 1, G_TYPE_STRING);
  client_signals[CLIENT_CONNECTED] = g_signal_new ("connected",
      G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
      G_TYPE_NONE, 1, G_TYPE_BOOLEAN);
}

static void
station_control_client_init (StationControlClient *self G_GNUC_UNUSED)
{
}
