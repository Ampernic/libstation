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
