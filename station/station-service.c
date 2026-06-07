/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef STATION_COMPILATION
# define STATION_COMPILATION
#endif
#include "station-service.h"

#include <glib/gstdio.h>
#include <stdlib.h>

#ifdef G_OS_WIN32
# include <windows.h>
#else
# include <signal.h>
# include <errno.h>
# include <unistd.h>
#endif

/* ---- pid file ----------------------------------------------------------- */

static char *
pidfile_path (const char *app_id)
{
  char *file = g_strconcat (app_id, ".pid", NULL);
  char *path = g_build_filename (g_get_user_runtime_dir (), file, NULL);
  g_free (file);
  return path;
}

static int
read_pid (const char *app_id)
{
  char *path = pidfile_path (app_id);
  char *content = NULL;
  int pid = 0;
  if (g_file_get_contents (path, &content, NULL, NULL))
    {
      /* Validate it's a plain positive integer: a corrupt/garbage pid file must
       * not be turned into a kill target for some unrelated process. */
      const char *p = g_strstrip (content);
      gboolean ok = (*p != '\0');
      for (const char *c = p; *c != '\0'; c++)
        if (!g_ascii_isdigit (*c)) { ok = FALSE; break; }
      gint64 v = ok ? g_ascii_strtoll (p, NULL, 10) : 0;
      if (v > 0 && v <= G_MAXINT)
        pid = (int) v;
      g_free (content);
    }
  g_free (path);
  return pid;
}

static void
write_pid (const char *app_id, int pid)
{
  char *path = pidfile_path (app_id);
  char *content = g_strdup_printf ("%d", pid);
  g_file_set_contents (path, content, -1, NULL);
  g_free (content);
  g_free (path);
}

static int
self_pid (void)
{
#ifdef G_OS_WIN32
  return (int) GetCurrentProcessId ();
#else
  return (int) getpid ();
#endif
}

static gboolean
pid_alive (int pid)
{
  if (pid <= 0)
    return FALSE;
#ifdef G_OS_WIN32
  HANDLE h = OpenProcess (SYNCHRONIZE, FALSE, (DWORD) pid);
  if (h == NULL)
    return FALSE;
  DWORD r = WaitForSingleObject (h, 0);
  CloseHandle (h);
  return r == WAIT_TIMEOUT;   /* still running */
#else
  return kill ((pid_t) pid, 0) == 0 || errno == EPERM;
#endif
}

static void
pid_terminate (int pid)
{
  if (pid <= 0)
    return;
#ifdef G_OS_WIN32
  HANDLE h = OpenProcess (PROCESS_TERMINATE, FALSE, (DWORD) pid);
  if (h != NULL)
    {
      TerminateProcess (h, 0);
      CloseHandle (h);
    }
#else
  kill ((pid_t) pid, SIGTERM);
#endif
}

/* ---- object ------------------------------------------------------------- */

struct _StationService
{
  GObject  parent_instance;
  char    *app_id;
  char   **argv;   /* daemon command, NULL-terminated */
};

G_DEFINE_FINAL_TYPE (StationService, station_service, G_TYPE_OBJECT)

StationService *
station_service_new (const char         *app_id,
                     const char * const *daemon_argv)
{
  g_return_val_if_fail (app_id != NULL, NULL);
  g_return_val_if_fail (daemon_argv != NULL && daemon_argv[0] != NULL, NULL);
  StationService *self = g_object_new (STATION_TYPE_SERVICE, NULL);
  self->app_id = g_strdup (app_id);
  self->argv = g_strdupv ((char **) daemon_argv);
  return self;
}

gboolean
station_service_is_active (StationService *self)
{
  g_return_val_if_fail (STATION_IS_SERVICE (self), FALSE);
  return pid_alive (read_pid (self->app_id));
}

gboolean
station_service_start (StationService *self,
                       GError        **error)
{
  g_return_val_if_fail (STATION_IS_SERVICE (self), FALSE);
  if (station_service_is_active (self))
    return TRUE;

  GSubprocess *proc = g_subprocess_newv ((const gchar * const *) self->argv,
      G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
      error);
  if (proc == NULL)
    return FALSE;

  const char *ident = g_subprocess_get_identifier (proc);   /* pid as string */
  if (ident != NULL)
    write_pid (self->app_id, atoi (ident));
  g_object_unref (proc);   /* does not terminate the detached child */
  return TRUE;
}

void
station_service_stop (StationService *self)
{
  g_return_if_fail (STATION_IS_SERVICE (self));
  pid_terminate (read_pid (self->app_id));
  char *path = pidfile_path (self->app_id);
  g_unlink (path);
  g_free (path);
}

void
station_service_mark_running (const char *app_id)
{
  g_return_if_fail (app_id != NULL);
  write_pid (app_id, self_pid ());
}

void
station_service_clear (const char *app_id)
{
  g_return_if_fail (app_id != NULL);
  char *path = pidfile_path (app_id);
  g_unlink (path);
  g_free (path);
}

/* ---- autostart ---------------------------------------------------------- */

static char *
argv_to_cmdline (char **argv)
{
  GString *s = g_string_new (NULL);
  for (int i = 0; argv[i] != NULL; i++)
    {
      if (i > 0)
        g_string_append_c (s, ' ');
      g_string_append_c (s, '"');
      /* Escape embedded quotes/backslashes so an arg with either doesn't break
       * the Run-key command line / Exec= line. */
      for (const char *c = argv[i]; *c != '\0'; c++)
        {
          if (*c == '"' || *c == '\\')
            g_string_append_c (s, '\\');
          g_string_append_c (s, *c);
        }
      g_string_append_c (s, '"');
    }
  return g_string_free (s, FALSE);
}

#if !defined(G_OS_WIN32) && !defined(__APPLE__)
static char *
xdg_autostart_path (const char *app_id)
{
  char *file = g_strconcat (app_id, ".desktop", NULL);
  char *path = g_build_filename (g_get_user_config_dir (), "autostart", file, NULL);
  g_free (file);
  return path;
}
#elif defined(__APPLE__)
static char *
launchagent_path (const char *app_id)
{
  char *file = g_strconcat (app_id, ".plist", NULL);
  char *path = g_build_filename (g_get_home_dir (), "Library", "LaunchAgents", file, NULL);
  g_free (file);
  return path;
}
#endif

#ifdef G_OS_WIN32
# define STATION_RUN_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#endif

gboolean
station_service_set_autostart (StationService *self,
                               gboolean        enabled,
                               GError        **error)
{
  g_return_val_if_fail (STATION_IS_SERVICE (self), FALSE);

#if defined(G_OS_WIN32)
  wchar_t *wname = (wchar_t *) g_utf8_to_utf16 (self->app_id, -1, NULL, NULL, error);
  if (wname == NULL)
    return FALSE;
  LSTATUS rc;
  if (enabled)
    {
      char *cmd = argv_to_cmdline (self->argv);
      wchar_t *wcmd = (wchar_t *) g_utf8_to_utf16 (cmd, -1, NULL, NULL, NULL);
      g_free (cmd);
      rc = RegSetKeyValueW (HKEY_CURRENT_USER, STATION_RUN_KEY, wname, REG_SZ,
          wcmd, (DWORD) ((wcslen (wcmd) + 1) * sizeof (wchar_t)));
      g_free (wcmd);
    }
  else
    {
      rc = RegDeleteKeyValueW (HKEY_CURRENT_USER, STATION_RUN_KEY, wname);
      if (rc == ERROR_FILE_NOT_FOUND)
        rc = ERROR_SUCCESS;
    }
  g_free (wname);
  if (rc != ERROR_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "registry autostart failed (%ld)", (long) rc);
      return FALSE;
    }
  return TRUE;

#elif defined(__APPLE__)
  char *path = launchagent_path (self->app_id);
  gboolean ok = TRUE;
  if (enabled)
    {
      GString *args = g_string_new (NULL);
      for (int i = 0; self->argv[i] != NULL; i++)
        g_string_append_printf (args, "    <string>%s</string>\n", self->argv[i]);
      char *plist = g_strdup_printf (
          "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
          "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
          "<plist version=\"1.0\"><dict>\n"
          "  <key>Label</key><string>%s</string>\n"
          "  <key>ProgramArguments</key><array>\n%s  </array>\n"
          "  <key>RunAtLoad</key><true/>\n"
          "</dict></plist>\n",
          self->app_id, args->str);
      g_string_free (args, TRUE);
      char *dir = g_path_get_dirname (path);
      g_mkdir_with_parents (dir, 0755);
      g_free (dir);
      ok = g_file_set_contents (path, plist, -1, error);
      g_free (plist);
    }
  else
    {
      g_unlink (path);
    }
  g_free (path);
  return ok;

#else
  char *path = xdg_autostart_path (self->app_id);
  gboolean ok = TRUE;
  if (enabled)
    {
      char *cmd = argv_to_cmdline (self->argv);
      char *entry = g_strdup_printf (
          "[Desktop Entry]\n"
          "Type=Application\n"
          "Name=%s\n"
          "Exec=%s\n"
          "Terminal=false\n"
          "NoDisplay=true\n"
          "X-GNOME-Autostart-enabled=true\n",
          self->app_id, cmd);
      g_free (cmd);
      char *dir = g_path_get_dirname (path);
      g_mkdir_with_parents (dir, 0755);
      g_free (dir);
      ok = g_file_set_contents (path, entry, -1, error);
      g_free (entry);
    }
  else
    {
      g_unlink (path);
    }
  g_free (path);
  return ok;
#endif
}

gboolean
station_service_get_autostart (StationService *self)
{
  g_return_val_if_fail (STATION_IS_SERVICE (self), FALSE);
#if defined(G_OS_WIN32)
  wchar_t *wname = (wchar_t *) g_utf8_to_utf16 (self->app_id, -1, NULL, NULL, NULL);
  if (wname == NULL)
    return FALSE;
  LSTATUS rc = RegGetValueW (HKEY_CURRENT_USER, STATION_RUN_KEY, wname,
                             RRF_RT_REG_SZ, NULL, NULL, NULL);
  g_free (wname);
  return rc == ERROR_SUCCESS;
#elif defined(__APPLE__)
  char *path = launchagent_path (self->app_id);
  gboolean ok = g_file_test (path, G_FILE_TEST_EXISTS);
  g_free (path);
  return ok;
#else
  char *path = xdg_autostart_path (self->app_id);
  gboolean ok = g_file_test (path, G_FILE_TEST_EXISTS);
  g_free (path);
  return ok;
#endif
}

static void
station_service_finalize (GObject *object)
{
  StationService *self = STATION_SERVICE (object);
  g_clear_pointer (&self->app_id, g_free);
  g_clear_pointer (&self->argv, g_strfreev);
  G_OBJECT_CLASS (station_service_parent_class)->finalize (object);
}

static void
station_service_class_init (StationServiceClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = station_service_finalize;
}

static void
station_service_init (StationService *self G_GNUC_UNUSED)
{
}

/* ---- Android background operation --------------------------------------- */
/* The real JNI-backed implementation lives in backends/android/station-android.c
 * (built only for Android). Everywhere else these are no-ops. */
#ifndef __ANDROID__
gboolean
station_android_battery_unrestricted (GdkSurface *surface G_GNUC_UNUSED)
{
  return TRUE;   /* no battery manager off Android: nothing to restrict */
}

void
station_android_request_battery_unrestricted (GdkSurface *surface G_GNUC_UNUSED)
{
}

void
station_android_open_notification_settings (GdkSurface *surface G_GNUC_UNUSED)
{
}

void
station_android_request_notification_permission (GdkSurface *surface G_GNUC_UNUSED)
{
}

void
station_android_open_uri (GdkSurface *surface G_GNUC_UNUSED,
                          const char *uri G_GNUC_UNUSED)
{
}

void
station_android_foreground_bind (GdkSurface *surface G_GNUC_UNUSED,
                                 const char *application_class G_GNUC_UNUSED,
                                 const char *service_class G_GNUC_UNUSED)
{
}

void
station_android_foreground_set_text (const char *text G_GNUC_UNUSED)
{
}

void
station_android_set_resume_handler (StationResumeFunc cb G_GNUC_UNUSED)
{
}
#endif /* !__ANDROID__ */
