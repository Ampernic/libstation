/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Android backend: background-operation helpers over the gdk-android public API
 * + JNI. The proxy/app runs in-process on Android (no daemon); these keep it
 * alive (battery-optimization exemption) and show its status (foreground-service
 * notification). The foreground service + application classes live in the APK, so
 * their dotted names are passed in — nothing here is app-specific. Everything
 * goes through gdk_android_display_get_env() + the toplevel's Activity; no
 * private GDK symbols.
 *
 * JNI discipline: every call that can raise is followed by an exception check
 * (a pending exception makes any further JNI call undefined), class/method
 * lookups are NULL-checked, and every local ref is released — these functions run
 * from the GLib loop (a long-lived native thread, not a Java frame), so leaked
 * local refs accumulate instead of being reclaimed on return. */
#ifndef STATION_COMPILATION
# define STATION_COMPILATION
#endif
#include <gdk/android/gdkandroid.h>
#include <gio/gio.h>
#include <jni.h>
#include "station-service.h"

/* The app's foreground-service class, loaded via the app class loader. */
static jclass g_service_cls = NULL;
/* Resume handler set by the UI; invoked on the GLib main loop on each resume. */
static StationResumeFunc g_resume_cb = NULL;

/* True (and clears the exception) if a JNI call left one pending. */
static gboolean
exc (JNIEnv *env)
{
  if ((*env)->ExceptionCheck (env))
    {
      (*env)->ExceptionClear (env);
      return TRUE;
    }
  return FALSE;
}

static gboolean
resume_on_main (gpointer data G_GNUC_UNUSED)
{
  if (g_resume_cb != NULL)
    g_resume_cb ();
  return G_SOURCE_REMOVE;
}

/* JNI: <application_class>.nativeOnResume(). Runs on the Android UI thread, so it
 * just hands off to the GLib main loop. */
static void
native_on_resume (JNIEnv *env G_GNUC_UNUSED, jclass cls G_GNUC_UNUSED)
{
  g_idle_add (resume_on_main, NULL);
}

/* Load an app class through the activity's class loader (the system loader a
 * native thread would use can't see app classes). Returns a global ref or NULL. */
static jclass
load_app_class (JNIEnv *env, jobject activity, const char *dotted_name)
{
  jclass acls = (*env)->GetObjectClass (env, activity);
  if (acls == NULL)
    return NULL;
  jmethodID get_cl = (*env)->GetMethodID (env, acls, "getClassLoader",
                                          "()Ljava/lang/ClassLoader;");
  if (get_cl == NULL)
    {
      (*env)->DeleteLocalRef (env, acls);
      return NULL;
    }
  jobject cl = (*env)->CallObjectMethod (env, activity, get_cl);
  (*env)->DeleteLocalRef (env, acls);
  if (exc (env) || cl == NULL)
    return NULL;
  jclass cl_cls = (*env)->GetObjectClass (env, cl);
  jmethodID load = cl_cls ? (*env)->GetMethodID (env, cl_cls, "loadClass",
                              "(Ljava/lang/String;)Ljava/lang/Class;") : NULL;
  if (cl_cls != NULL)
    (*env)->DeleteLocalRef (env, cl_cls);
  if (load == NULL)
    {
      (*env)->DeleteLocalRef (env, cl);
      return NULL;
    }
  jstring name = (*env)->NewStringUTF (env, dotted_name);
  jobject cls = name ? (*env)->CallObjectMethod (env, cl, load, name) : NULL;
  if (name != NULL)
    (*env)->DeleteLocalRef (env, name);
  (*env)->DeleteLocalRef (env, cl);
  if (exc (env) || cls == NULL)
    return NULL;
  jclass global = (*env)->NewGlobalRef (env, cls);
  (*env)->DeleteLocalRef (env, cls);
  return global;
}

/* Resolve the JNI environment and the current Activity (a Context) from a
 * realized toplevel surface. FALSE if the surface isn't an Android toplevel yet. */
static gboolean
resolve (GdkSurface *surface, JNIEnv **env, jobject *activity,
         GdkAndroidToplevel **toplevel)
{
  if (surface == NULL || !GDK_IS_ANDROID_TOPLEVEL (surface))
    {
      g_warning ("station android: surface is not an Android toplevel yet");
      return FALSE;
    }
  GdkDisplay *display = gdk_surface_get_display (surface);
  *env = gdk_android_display_get_env (display);
  if (*env == NULL)
    {
      g_warning ("station android: no JNI env from display");
      return FALSE;
    }
  *toplevel = GDK_ANDROID_TOPLEVEL (surface);
  *activity = gdk_android_toplevel_get_activity (*toplevel);
  if (*activity == NULL)
    g_warning ("station android: toplevel has no activity");
  return *activity != NULL;
}

/* getPackageName() on the activity (a Context). Caller must DeleteLocalRef. */
static jstring
package_name (JNIEnv *env, jobject activity)
{
  jclass ctx = (*env)->GetObjectClass (env, activity);
  if (ctx == NULL)
    return NULL;
  jmethodID m = (*env)->GetMethodID (env, ctx, "getPackageName",
                                     "()Ljava/lang/String;");
  jstring pkg = m ? (*env)->CallObjectMethod (env, activity, m) : NULL;
  (*env)->DeleteLocalRef (env, ctx);
  if (exc (env))
    return NULL;
  return pkg;
}

/* The activity's SDK_INT (Build.VERSION.SDK_INT), or 0 if it can't be read. */
static jint
sdk_int (JNIEnv *env)
{
  jclass ver = (*env)->FindClass (env, "android/os/Build$VERSION");
  if (ver == NULL) { exc (env); return 0; }
  jfieldID f = (*env)->GetStaticFieldID (env, ver, "SDK_INT", "I");
  jint v = f ? (*env)->GetStaticIntField (env, ver, f) : 0;
  (*env)->DeleteLocalRef (env, ver);
  exc (env);
  return v;
}

gboolean
station_android_battery_unrestricted (GdkSurface *surface)
{
  JNIEnv *env;
  jobject activity;
  GdkAndroidToplevel *toplevel;
  if (!resolve (surface, &env, &activity, &toplevel))
    return TRUE; /* couldn't check -> don't nag */

  jclass ctx = (*env)->GetObjectClass (env, activity);
  if (ctx == NULL) return TRUE;
  jmethodID get_service = (*env)->GetMethodID (env, ctx, "getSystemService",
                                               "(Ljava/lang/String;)Ljava/lang/Object;");
  if (get_service == NULL) { (*env)->DeleteLocalRef (env, ctx); return TRUE; }
  jstring power = (*env)->NewStringUTF (env, "power");
  jobject pm = power ? (*env)->CallObjectMethod (env, activity, get_service, power) : NULL;
  if (power != NULL) (*env)->DeleteLocalRef (env, power);
  (*env)->DeleteLocalRef (env, ctx);
  if (exc (env) || pm == NULL)
    return TRUE;

  jstring pkg = package_name (env, activity);
  jclass pm_cls = (*env)->GetObjectClass (env, pm);
  jmethodID is_ign = pm_cls ? (*env)->GetMethodID (env, pm_cls,
                       "isIgnoringBatteryOptimizations", "(Ljava/lang/String;)Z") : NULL;
  jboolean r = JNI_TRUE;
  if (is_ign != NULL && pkg != NULL)
    {
      r = (*env)->CallBooleanMethod (env, pm, is_ign, pkg);
      if (exc (env)) r = JNI_TRUE;
    }
  if (pm_cls != NULL) (*env)->DeleteLocalRef (env, pm_cls);
  (*env)->DeleteLocalRef (env, pm);
  if (pkg != NULL) (*env)->DeleteLocalRef (env, pkg);
  return r ? TRUE : FALSE;
}

/* Build an Intent(action) or Intent(action, uri) and hand it to the activity.
 * uri may be NULL. Consumes nothing; cleans up its own local refs. */
static void
launch_settings (JNIEnv *env, GdkAndroidToplevel *toplevel,
                 const char *action, jobject uri)
{
  jclass intent_cls = (*env)->FindClass (env, "android/content/Intent");
  if (intent_cls == NULL) { exc (env); return; }
  jmethodID ctor = uri != NULL
    ? (*env)->GetMethodID (env, intent_cls, "<init>",
                           "(Ljava/lang/String;Landroid/net/Uri;)V")
    : (*env)->GetMethodID (env, intent_cls, "<init>", "(Ljava/lang/String;)V");
  jstring jaction = ctor ? (*env)->NewStringUTF (env, action) : NULL;
  jobject intent = (ctor && jaction)
    ? (uri != NULL ? (*env)->NewObject (env, intent_cls, ctor, jaction, uri)
                   : (*env)->NewObject (env, intent_cls, ctor, jaction))
    : NULL;
  if (jaction != NULL) (*env)->DeleteLocalRef (env, jaction);
  if (!exc (env) && intent != NULL)
    {
      GError *error = NULL;
      gdk_android_toplevel_launch_activity (toplevel, intent, &error);
      if (error != NULL)
        {
          g_warning ("station android: launch_activity failed: %s", error->message);
          g_clear_error (&error);
        }
    }
  if (intent != NULL) (*env)->DeleteLocalRef (env, intent);
  (*env)->DeleteLocalRef (env, intent_cls);
}

void
station_android_request_battery_unrestricted (GdkSurface *surface)
{
  JNIEnv *env;
  jobject activity;
  GdkAndroidToplevel *toplevel;
  if (!resolve (surface, &env, &activity, &toplevel))
    return;

  /* Uri uri = Uri.parse("package:" + getPackageName()) */
  jstring pkg = package_name (env, activity);
  if (pkg == NULL) return;
  const char *cpkg = (*env)->GetStringUTFChars (env, pkg, NULL);
  char *uri_str = g_strconcat ("package:", cpkg ? cpkg : "", NULL);
  if (cpkg != NULL) (*env)->ReleaseStringUTFChars (env, pkg, cpkg);
  (*env)->DeleteLocalRef (env, pkg);

  jclass uri_cls = (*env)->FindClass (env, "android/net/Uri");
  jmethodID parse = uri_cls ? (*env)->GetStaticMethodID (env, uri_cls, "parse",
                      "(Ljava/lang/String;)Landroid/net/Uri;") : NULL;
  jstring juri = parse ? (*env)->NewStringUTF (env, uri_str) : NULL;
  g_free (uri_str);
  jobject uri = juri ? (*env)->CallStaticObjectMethod (env, uri_cls, parse, juri) : NULL;
  if (juri != NULL) (*env)->DeleteLocalRef (env, juri);
  if (!exc (env) && uri != NULL)
    launch_settings (env, toplevel,
                     "android.settings.REQUEST_IGNORE_BATTERY_OPTIMIZATIONS", uri);
  if (uri != NULL) (*env)->DeleteLocalRef (env, uri);
  if (uri_cls != NULL) (*env)->DeleteLocalRef (env, uri_cls);
}

void
station_android_open_notification_settings (GdkSurface *surface)
{
  JNIEnv *env;
  jobject activity;
  GdkAndroidToplevel *toplevel;
  if (!resolve (surface, &env, &activity, &toplevel))
    return;

  jstring pkg = package_name (env, activity);
  if (pkg == NULL) return;
  jclass intent_cls = (*env)->FindClass (env, "android/content/Intent");
  jmethodID ctor = intent_cls ? (*env)->GetMethodID (env, intent_cls, "<init>",
                     "(Ljava/lang/String;)V") : NULL;
  jstring action = ctor ? (*env)->NewStringUTF (env,
                     "android.settings.APP_NOTIFICATION_SETTINGS") : NULL;
  jobject intent = (ctor && action) ? (*env)->NewObject (env, intent_cls, ctor, action) : NULL;
  if (action != NULL) (*env)->DeleteLocalRef (env, action);

  if (!exc (env) && intent != NULL)
    {
      jmethodID put = (*env)->GetMethodID (env, intent_cls, "putExtra",
          "(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;");
      jstring key = put ? (*env)->NewStringUTF (env,
          "android.provider.extra.APP_PACKAGE") : NULL;
      if (put != NULL && key != NULL)
        {
          jobject ret = (*env)->CallObjectMethod (env, intent, put, key, pkg);
          exc (env);
          if (ret != NULL) (*env)->DeleteLocalRef (env, ret);
        }
      if (key != NULL) (*env)->DeleteLocalRef (env, key);

      GError *error = NULL;
      gdk_android_toplevel_launch_activity (toplevel, intent, &error);
      if (error != NULL)
        {
          g_warning ("station android: notification settings launch failed: %s",
                     error->message);
          g_clear_error (&error);
        }
    }
  if (intent != NULL) (*env)->DeleteLocalRef (env, intent);
  if (intent_cls != NULL) (*env)->DeleteLocalRef (env, intent_cls);
  (*env)->DeleteLocalRef (env, pkg);
}

void
station_android_open_uri (GdkSurface *surface, const char *uri)
{
  JNIEnv *env;
  jobject activity;
  GdkAndroidToplevel *toplevel;
  if (!resolve (surface, &env, &activity, &toplevel))
    return;

  jclass uri_cls = (*env)->FindClass (env, "android/net/Uri");
  jmethodID parse = uri_cls ? (*env)->GetStaticMethodID (env, uri_cls, "parse",
                      "(Ljava/lang/String;)Landroid/net/Uri;") : NULL;
  jstring juri = parse ? (*env)->NewStringUTF (env, uri ? uri : "") : NULL;
  jobject uobj = juri ? (*env)->CallStaticObjectMethod (env, uri_cls, parse, juri) : NULL;
  if (juri != NULL) (*env)->DeleteLocalRef (env, juri);
  if (!exc (env) && uobj != NULL)
    launch_settings (env, toplevel, "android.intent.action.VIEW", uobj);
  if (uobj != NULL) (*env)->DeleteLocalRef (env, uobj);
  if (uri_cls != NULL) (*env)->DeleteLocalRef (env, uri_cls);
}

void
station_android_install_apk (GdkSurface *surface, const char *helper_class,
                             const char *apk_path)
{
  JNIEnv *env;
  jobject activity;
  GdkAndroidToplevel *toplevel;
  if (!resolve (surface, &env, &activity, &toplevel))
    return;
  if (helper_class == NULL || apk_path == NULL)
    return;

  /* <helper_class>.installApk(activity, apk_path) — the app-side helper drives
   * the PackageInstaller session and the system install prompt. */
  jclass cls = load_app_class (env, activity, helper_class);
  if (cls == NULL)
    {
      g_warning ("station android: could not load install helper %s", helper_class);
      return;
    }
  jmethodID m = (*env)->GetStaticMethodID (env, cls, "installApk",
                  "(Landroid/content/Context;Ljava/lang/String;)V");
  if (m != NULL)
    {
      jstring jpath = (*env)->NewStringUTF (env, apk_path);
      if (jpath != NULL)
        {
          (*env)->CallStaticVoidMethod (env, cls, m, activity, jpath);
          if (exc (env))
            g_warning ("station android: installApk threw");
          (*env)->DeleteLocalRef (env, jpath);
        }
    }
  else
    {
      exc (env);
      g_warning ("station android: install helper has no installApk(Context,String)");
    }
  (*env)->DeleteGlobalRef (env, cls);
}

void
station_android_request_notification_permission (GdkSurface *surface)
{
  JNIEnv *env;
  jobject activity;
  GdkAndroidToplevel *toplevel;
  if (!resolve (surface, &env, &activity, &toplevel))
    return;
  /* POST_NOTIFICATIONS is a runtime permission only on Android 13+ (API 33);
   * before that the foreground-service notification shows without a grant. */
  if (sdk_int (env) < 33)
    return;

  const char *perm = "android.permission.POST_NOTIFICATIONS";
  jclass acls = (*env)->GetObjectClass (env, activity);
  if (acls == NULL) { exc (env); return; }

  /* if (checkSelfPermission(perm) == PERMISSION_GRANTED) return; */
  jmethodID check = (*env)->GetMethodID (env, acls, "checkSelfPermission",
                                         "(Ljava/lang/String;)I");
  if (check != NULL)
    {
      jstring p = (*env)->NewStringUTF (env, perm);
      jint r = p ? (*env)->CallIntMethod (env, activity, check, p) : -1;
      if (p != NULL) (*env)->DeleteLocalRef (env, p);
      if (!exc (env) && r == 0 /* PackageManager.PERMISSION_GRANTED */)
        { (*env)->DeleteLocalRef (env, acls); return; }
    }

  /* activity.requestPermissions(new String[]{perm}, 0) */
  jmethodID req = (*env)->GetMethodID (env, acls, "requestPermissions",
                                       "([Ljava/lang/String;I)V");
  jclass str_cls = (*env)->FindClass (env, "java/lang/String");
  if (req != NULL && str_cls != NULL)
    {
      jobjectArray arr = (*env)->NewObjectArray (env, 1, str_cls, NULL);
      jstring p = (*env)->NewStringUTF (env, perm);
      if (arr != NULL && p != NULL)
        {
          (*env)->SetObjectArrayElement (env, arr, 0, p);
          (*env)->CallVoidMethod (env, activity, req, arr, 0);
          exc (env);
        }
      if (p != NULL) (*env)->DeleteLocalRef (env, p);
      if (arr != NULL) (*env)->DeleteLocalRef (env, arr);
    }
  else
    exc (env);
  if (str_cls != NULL) (*env)->DeleteLocalRef (env, str_cls);
  (*env)->DeleteLocalRef (env, acls);
}

void
station_android_foreground_bind (GdkSurface *surface,
                                 const char *application_class,
                                 const char *service_class)
{
  if (g_service_cls != NULL)
    return;
  JNIEnv *env;
  jobject activity;
  GdkAndroidToplevel *toplevel;
  if (!resolve (surface, &env, &activity, &toplevel))
    return;

  if (service_class != NULL)
    {
      g_service_cls = load_app_class (env, activity, service_class);
      if (g_service_cls == NULL)
        g_warning ("station android: could not load service class %s", service_class);
    }

  /* Bind <application_class>.nativeOnResume → native_on_resume. The lib is
   * dlopen'd, so the JVM can't resolve native methods by name; register it. */
  if (application_class != NULL)
    {
      jclass app_cls = load_app_class (env, activity, application_class);
      if (app_cls != NULL)
        {
          JNINativeMethod m = { "nativeOnResume", "()V", (void *) native_on_resume };
          if ((*env)->RegisterNatives (env, app_cls, &m, 1) != 0)
            {
              exc (env);
              g_warning ("station android: could not register nativeOnResume");
            }
          (*env)->DeleteGlobalRef (env, app_cls);
        }
    }
}

void
station_android_foreground_set_text (const char *text)
{
  if (g_service_cls == NULL)
    return; /* foreground_bind not run yet */

  GdkDisplay *display = gdk_display_get_default ();
  if (display == NULL)
    return;
  JNIEnv *env = gdk_android_display_get_env (display);
  if (env == NULL)
    return;

  jmethodID m = (*env)->GetStaticMethodID (env, g_service_cls, "setText",
                                           "(Ljava/lang/String;)V");
  if (m != NULL)
    {
      jstring s = (*env)->NewStringUTF (env, text != NULL ? text : "");
      if (s != NULL)
        {
          (*env)->CallStaticVoidMethod (env, g_service_cls, m, s);
          exc (env);
          (*env)->DeleteLocalRef (env, s);
        }
    }
  else
    exc (env);
}

void
station_android_set_resume_handler (StationResumeFunc cb)
{
  g_resume_cb = cb;
}

/* ---- HTTPS GET for the update check (java.net) --------------------------- *
 * The APK ships no glib-networking GIO TLS backend, so GIO's own HTTPS fails
 * ("TLS support is not available"). Instead do the GET through java.net, which
 * uses the platform's TLS stack and system CA store. The fetch runs on a GLib
 * worker thread (not a Java frame), so it attaches to the JVM captured here. */

static JavaVM *g_jvm = NULL;

/* Capture the JavaVM. Call from the main thread (where gdk holds a JNIEnv). */
void
station_updates_android_prime (void)
{
  if (g_jvm != NULL)
    return;
  GdkDisplay *display = gdk_display_get_default ();
  if (display == NULL)
    return;
  JNIEnv *env = gdk_android_display_get_env (display);
  if (env != NULL)
    (*env)->GetJavaVM (env, &g_jvm);
}

/* Read an InputStream fully into a GByteArray (caps the size). Returns FALSE and
 * clears any pending exception on a read error. */
static gboolean
read_stream (JNIEnv *env, jobject is, jclass is_cls, GByteArray *out)
{
  jmethodID read_m = (*env)->GetMethodID (env, is_cls, "read", "([B)I");
  if (read_m == NULL)
    { exc (env); return FALSE; }
  jbyteArray jbuf = (*env)->NewByteArray (env, 8192);
  if (jbuf == NULL)
    { exc (env); return FALSE; }

  gboolean ok = TRUE;
  for (;;)
    {
      jint n = (*env)->CallIntMethod (env, is, read_m, jbuf);
      if (exc (env)) { ok = FALSE; break; }
      if (n < 0) break;                          /* end of stream */
      if (n == 0) continue;
      if (out->len + (guint) n > (4u * 1024u * 1024u)) break;  /* sanity cap */
      jbyte *elems = (*env)->GetByteArrayElements (env, jbuf, NULL);
      if (elems == NULL) { exc (env); ok = FALSE; break; }
      g_byte_array_append (out, (const guint8 *) elems, (guint) n);
      (*env)->ReleaseByteArrayElements (env, jbuf, elems, JNI_ABORT);
    }
  (*env)->DeleteLocalRef (env, jbuf);
  return ok;
}

guint8 *
station_updates_android_http_get (const char *url, gsize *out_len, GError **error)
{
  *out_len = 0;
  if (g_jvm == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "Android JVM not available");
      return NULL;
    }

  JNIEnv *env = NULL;
  gboolean attached = FALSE;
  jint st = (*g_jvm)->GetEnv (g_jvm, (void **) &env, JNI_VERSION_1_6);
  if (st == JNI_EDETACHED)
    {
      if ((*g_jvm)->AttachCurrentThread (g_jvm, &env, NULL) != 0 || env == NULL)
        {
          g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "could not attach to the Android JVM");
          return NULL;
        }
      attached = TRUE;
    }
  else if (st != JNI_OK || env == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "no JNI environment");
      return NULL;
    }

  guint8 *result = NULL;
  GByteArray *body = NULL;
  jstring jurl = NULL, jua = NULL, jua_key = NULL;
  jobject url_obj = NULL, conn = NULL, is = NULL;
  jclass url_cls = NULL, conn_cls = NULL, is_cls = NULL;

  url_cls = (*env)->FindClass (env, "java/net/URL");
  jmethodID url_ctor = url_cls ? (*env)->GetMethodID (env, url_cls, "<init>",
                         "(Ljava/lang/String;)V") : NULL;
  jurl = url_ctor ? (*env)->NewStringUTF (env, url) : NULL;
  url_obj = jurl ? (*env)->NewObject (env, url_cls, url_ctor, jurl) : NULL;
  if (exc (env) || url_obj == NULL)
    { g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "bad URL"); goto out; }

  jmethodID open = (*env)->GetMethodID (env, url_cls, "openConnection",
                                        "()Ljava/net/URLConnection;");
  conn = open ? (*env)->CallObjectMethod (env, url_obj, open) : NULL;
  if (exc (env) || conn == NULL)
    { g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "openConnection failed"); goto out; }
  conn_cls = (*env)->GetObjectClass (env, conn);

  /* User-Agent header (GitHub requires one) + timeouts. */
  jmethodID set_prop = (*env)->GetMethodID (env, conn_cls, "setRequestProperty",
                         "(Ljava/lang/String;Ljava/lang/String;)V");
  jua_key = (*env)->NewStringUTF (env, "User-Agent");
  jua = (*env)->NewStringUTF (env, "libstation");
  if (set_prop != NULL && jua_key != NULL && jua != NULL)
    { (*env)->CallVoidMethod (env, conn, set_prop, jua_key, jua); exc (env); }
  jmethodID set_ct = (*env)->GetMethodID (env, conn_cls, "setConnectTimeout", "(I)V");
  if (set_ct != NULL) { (*env)->CallVoidMethod (env, conn, set_ct, 15000); exc (env); }
  jmethodID set_rt = (*env)->GetMethodID (env, conn_cls, "setReadTimeout", "(I)V");
  if (set_rt != NULL) { (*env)->CallVoidMethod (env, conn, set_rt, 15000); exc (env); }

  jmethodID get_code = (*env)->GetMethodID (env, conn_cls, "getResponseCode", "()I");
  jint code = get_code ? (*env)->CallIntMethod (env, conn, get_code) : -1;
  if (exc (env))
    { g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "connection failed"); goto out; }
  if (code != 200)
    { g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "HTTP error %d", (int) code);
      goto out; }

  jmethodID get_is = (*env)->GetMethodID (env, conn_cls, "getInputStream",
                       "()Ljava/io/InputStream;");
  is = get_is ? (*env)->CallObjectMethod (env, conn, get_is) : NULL;
  if (exc (env) || is == NULL)
    { g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "no response body"); goto out; }
  is_cls = (*env)->GetObjectClass (env, is);

  body = g_byte_array_new ();
  if (!read_stream (env, is, is_cls, body))
    { g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                           "error reading response"); goto out; }

  *out_len = body->len;
  result = g_byte_array_free (body, FALSE);
  body = NULL;

out:
  if (is != NULL)
    {
      jmethodID close_m = is_cls ? (*env)->GetMethodID (env, is_cls, "close", "()V") : NULL;
      if (close_m != NULL) { (*env)->CallVoidMethod (env, is, close_m); exc (env); }
    }
  if (body != NULL)       g_byte_array_free (body, TRUE);
  if (is != NULL)         (*env)->DeleteLocalRef (env, is);
  if (is_cls != NULL)     (*env)->DeleteLocalRef (env, is_cls);
  if (conn != NULL)       (*env)->DeleteLocalRef (env, conn);
  if (conn_cls != NULL)   (*env)->DeleteLocalRef (env, conn_cls);
  if (url_obj != NULL)    (*env)->DeleteLocalRef (env, url_obj);
  if (url_cls != NULL)    (*env)->DeleteLocalRef (env, url_cls);
  if (jurl != NULL)       (*env)->DeleteLocalRef (env, jurl);
  if (jua != NULL)        (*env)->DeleteLocalRef (env, jua);
  if (jua_key != NULL)    (*env)->DeleteLocalRef (env, jua_key);
  if (attached)           (*g_jvm)->DetachCurrentThread (g_jvm);
  return result;
}

/* station-updates.c: emit "download-progress" on the main thread. */
extern void station_updates_report_progress (gpointer self, gint64 got, gint64 total);

/* Stream @url into @out, reporting progress. java.net follows the GitHub asset
 * redirect (github.com → objects.githubusercontent.com, both https). */
gboolean
station_updates_android_download (const char *url, GOutputStream *out,
                                  gpointer self, GCancellable *cancel, GError **error)
{
  if (g_jvm == NULL)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Android JVM not available");
      return FALSE;
    }

  JNIEnv *env = NULL;
  gboolean attached = FALSE;
  jint st = (*g_jvm)->GetEnv (g_jvm, (void **) &env, JNI_VERSION_1_6);
  if (st == JNI_EDETACHED)
    {
      if ((*g_jvm)->AttachCurrentThread (g_jvm, &env, NULL) != 0 || env == NULL)
        { g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "could not attach to the Android JVM"); return FALSE; }
      attached = TRUE;
    }
  else if (st != JNI_OK || env == NULL)
    { g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "no JNI environment");
      return FALSE; }

  gboolean ok = FALSE;
  jstring jurl = NULL, jua = NULL, jua_key = NULL;
  jobject url_obj = NULL, conn = NULL, is = NULL;
  jclass url_cls = NULL, conn_cls = NULL, is_cls = NULL;
  jbyteArray jbuf = NULL;

  url_cls = (*env)->FindClass (env, "java/net/URL");
  jmethodID url_ctor = url_cls ? (*env)->GetMethodID (env, url_cls, "<init>",
                         "(Ljava/lang/String;)V") : NULL;
  jurl = url_ctor ? (*env)->NewStringUTF (env, url) : NULL;
  url_obj = jurl ? (*env)->NewObject (env, url_cls, url_ctor, jurl) : NULL;
  if (exc (env) || url_obj == NULL)
    { g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "bad URL"); goto out; }

  jmethodID open = (*env)->GetMethodID (env, url_cls, "openConnection",
                                        "()Ljava/net/URLConnection;");
  conn = open ? (*env)->CallObjectMethod (env, url_obj, open) : NULL;
  if (exc (env) || conn == NULL)
    { g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "openConnection failed");
      goto out; }
  conn_cls = (*env)->GetObjectClass (env, conn);

  jmethodID set_prop = (*env)->GetMethodID (env, conn_cls, "setRequestProperty",
                         "(Ljava/lang/String;Ljava/lang/String;)V");
  jua_key = (*env)->NewStringUTF (env, "User-Agent");
  jua = (*env)->NewStringUTF (env, "libstation");
  if (set_prop != NULL && jua_key != NULL && jua != NULL)
    { (*env)->CallVoidMethod (env, conn, set_prop, jua_key, jua); exc (env); }
  jmethodID set_ct = (*env)->GetMethodID (env, conn_cls, "setConnectTimeout", "(I)V");
  if (set_ct != NULL) { (*env)->CallVoidMethod (env, conn, set_ct, 20000); exc (env); }
  jmethodID set_rt = (*env)->GetMethodID (env, conn_cls, "setReadTimeout", "(I)V");
  if (set_rt != NULL) { (*env)->CallVoidMethod (env, conn, set_rt, 30000); exc (env); }

  jmethodID get_code = (*env)->GetMethodID (env, conn_cls, "getResponseCode", "()I");
  jint code = get_code ? (*env)->CallIntMethod (env, conn, get_code) : -1;
  if (exc (env))
    { g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "connection failed"); goto out; }
  if (code != 200)
    { g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "HTTP error %d", (int) code); goto out; }

  jmethodID get_len = (*env)->GetMethodID (env, conn_cls, "getContentLength", "()I");
  gint64 total = get_len ? (gint64) (*env)->CallIntMethod (env, conn, get_len) : -1;
  exc (env);

  jmethodID get_is = (*env)->GetMethodID (env, conn_cls, "getInputStream",
                       "()Ljava/io/InputStream;");
  is = get_is ? (*env)->CallObjectMethod (env, conn, get_is) : NULL;
  if (exc (env) || is == NULL)
    { g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED, "no response body"); goto out; }
  is_cls = (*env)->GetObjectClass (env, is);
  jmethodID read_m = (*env)->GetMethodID (env, is_cls, "read", "([B)I");
  jbuf = read_m ? (*env)->NewByteArray (env, 8192) : NULL;
  if (jbuf == NULL) { exc (env); g_set_error_literal (error, G_IO_ERROR,
                       G_IO_ERROR_FAILED, "read setup failed"); goto out; }

  guint8 cbuf[8192];
  gint64 got = 0, last = 0;
  ok = TRUE;
  for (;;)
    {
      if (g_cancellable_set_error_if_cancelled (cancel, error)) { ok = FALSE; break; }
      jint n = (*env)->CallIntMethod (env, is, read_m, jbuf);
      if (exc (env)) { g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                         "error reading response"); ok = FALSE; break; }
      if (n < 0) break;                 /* end of stream */
      if (n == 0) continue;
      (*env)->GetByteArrayRegion (env, jbuf, 0, n, (jbyte *) cbuf);
      if (exc (env)) { ok = FALSE; break; }
      if (!g_output_stream_write_all (out, cbuf, (gsize) n, NULL, cancel, error))
        { ok = FALSE; break; }
      got += n;
      if (got - last >= 65536)
        { station_updates_report_progress (self, got, total); last = got; }
    }
  if (ok)
    station_updates_report_progress (self, got, total > 0 ? total : got);

out:
  if (is != NULL)
    {
      jmethodID close_m = is_cls ? (*env)->GetMethodID (env, is_cls, "close", "()V") : NULL;
      if (close_m != NULL) { (*env)->CallVoidMethod (env, is, close_m); exc (env); }
    }
  if (jbuf != NULL)       (*env)->DeleteLocalRef (env, jbuf);
  if (is != NULL)         (*env)->DeleteLocalRef (env, is);
  if (is_cls != NULL)     (*env)->DeleteLocalRef (env, is_cls);
  if (conn != NULL)       (*env)->DeleteLocalRef (env, conn);
  if (conn_cls != NULL)   (*env)->DeleteLocalRef (env, conn_cls);
  if (url_obj != NULL)    (*env)->DeleteLocalRef (env, url_obj);
  if (url_cls != NULL)    (*env)->DeleteLocalRef (env, url_cls);
  if (jurl != NULL)       (*env)->DeleteLocalRef (env, jurl);
  if (jua != NULL)        (*env)->DeleteLocalRef (env, jua);
  if (jua_key != NULL)    (*env)->DeleteLocalRef (env, jua_key);
  if (attached)           (*g_jvm)->DetachCurrentThread (g_jvm);
  return ok;
}
