/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#if !defined(STATION_INSIDE) && !defined(STATION_COMPILATION)
# error "Only <station.h> can be included directly."
#endif

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/* Controls a background helper process ("the daemon") and its login autostart.
 *
 * Lifecycle uses a pid file (<app_id>.pid in the user runtime dir): start()
 * spawns @daemon_argv detached and records its pid; is_active() checks the pid is
 * alive; stop() signals it. The daemon should also call
 * station_service_mark_running()/station_service_clear() so it is detected when
 * launched by autostart rather than by start().
 *
 * Autostart is native per OS: an XDG autostart .desktop (Linux), a LaunchAgent
 * plist (macOS), or an HKCU\…\Run value (Windows). */

#define STATION_TYPE_SERVICE (station_service_get_type ())
G_DECLARE_FINAL_TYPE (StationService, station_service, STATION, SERVICE, GObject)

/**
 * station_service_new:
 * @app_id: reverse-DNS id, used for the pid file and autostart entry
 * @daemon_argv: (array zero-terminated=1): command that runs the background helper
 */
StationService *station_service_new (const char         *app_id,
                                     const char * const *daemon_argv);

gboolean station_service_start         (StationService *self, GError **error);
void     station_service_stop          (StationService *self);
gboolean station_service_is_active     (StationService *self);

gboolean station_service_set_autostart (StationService *self,
                                        gboolean        enabled,
                                        GError        **error);
gboolean station_service_get_autostart (StationService *self);

/* Daemon-side: call at startup / shutdown so the controller can find it. */
void station_service_mark_running (const char *app_id);
void station_service_clear        (const char *app_id);

/* ---- Android background operation -------------------------------------------
 *
 * Android has no separate daemon: the app runs in-process and stays alive via a
 * foreground service with an ongoing notification, plus a battery-optimization
 * exemption. These helpers wrap that (no-ops on every other platform). They take
 * the realized GdkSurface so they can reach the JNI env + Activity; GdkSurface is
 * forward-declared here so the header needs no GDK/GTK include off Android.
 *
 * The foreground service + Application classes live in the APK (each app declares
 * its own in its own package), so their dotted names are passed in. The service
 * class must expose a static `setText(String)`; the application class a native
 * `nativeOnResume()` that it invokes from its activity-resume callback.
 *
 * Hidden from g-ir-scanner (GdkSurface isn't in the scanned namespaces, and the
 * Vala binding for these lives in the hand-written libstation-android-1.vapi). */

#ifndef __GI_SCANNER__
typedef struct _GdkSurface GdkSurface;
typedef void (*StationResumeFunc) (void);

gboolean station_android_battery_unrestricted          (GdkSurface *surface);
void     station_android_request_battery_unrestricted  (GdkSurface *surface);
void     station_android_open_notification_settings    (GdkSurface *surface);
/* Open a URI in the system browser/handler (ACTION_VIEW). The contextless
 * station_open_uri() can't do this on Android — it needs the Activity. */
void     station_android_open_uri                      (GdkSurface *surface,
                                                        const char *uri);
/* Request the POST_NOTIFICATIONS runtime permission (Android 13+); no-op if
 * already granted or below API 33. The foreground notification needs it to show. */
void     station_android_request_notification_permission (GdkSurface *surface);

/* Cache the app classes (via the activity's class loader) and register
 * @application_class.nativeOnResume. Call once with a realized surface. */
void     station_android_foreground_bind     (GdkSurface *surface,
                                              const char *application_class,
                                              const char *service_class);
/* Update the ongoing notification text (calls <service_class>.setText). */
void     station_android_foreground_set_text (const char *text);
/* Handler invoked on the GLib main loop each time the activity resumes. */
void     station_android_set_resume_handler  (StationResumeFunc cb);
#endif /* !__GI_SCANNER__ */

G_END_DECLS
