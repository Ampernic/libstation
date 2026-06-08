/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Android update transport: HTTPS GET + download over java.net (the platform TLS
 * stack + system CA store), since the APK ships no glib-networking. Split out of
 * the Android backend so it sits beside the desktop station-http it parallels. */
#ifndef STATION_COMPILATION
# define STATION_COMPILATION
#endif
#define G_LOG_DOMAIN "Station-Updates"

#include <gdk/android/gdkandroid.h>   /* prime() grabs the JNIEnv from the display */
#include <gio/gio.h>
#include <jni.h>
#include <string.h>

/* Progress sink + the worker-side reentry, both defined in station-updates.c. */
extern void station_updates_report_progress (gpointer self, gint64 got, gint64 total);

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
