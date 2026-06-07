/* Android-only extras for libstation: background-operation helpers that take a
 * Gdk.Surface (kept out of the core libstation-1.vapi so desktop consumers need
 * no GTK). Hand-written; pairs with the station_android_* C API in the Service
 * module. Add `--pkg libstation-android-1` only on Android.
 *
 * cheader_filename is set per symbol, NOT on the namespace: the core
 * libstation-1.vapi already declares `namespace Station` with a namespace-level
 * cheader, and a second namespace-level cheader collides so it is never emitted —
 * generated C would then call these with no #include. Per-symbol headers avoid it. */

[CCode (cprefix = "Station", lower_case_cprefix = "station_")]
namespace Station {
	[CCode (cname = "StationResumeFunc", cheader_filename = "station.h", has_target = false)]
	public delegate void ResumeFunc ();

	[CCode (cname = "station_android_battery_unrestricted", cheader_filename = "station.h")]
	public static bool android_battery_unrestricted (Gdk.Surface surface);
	[CCode (cname = "station_android_request_battery_unrestricted", cheader_filename = "station.h")]
	public static void android_request_battery_unrestricted (Gdk.Surface surface);
	[CCode (cname = "station_android_open_notification_settings", cheader_filename = "station.h")]
	public static void android_open_notification_settings (Gdk.Surface surface);
	[CCode (cname = "station_android_open_uri", cheader_filename = "station.h")]
	public static void android_open_uri (Gdk.Surface surface, string uri);
	[CCode (cname = "station_android_request_notification_permission", cheader_filename = "station.h")]
	public static void android_request_notification_permission (Gdk.Surface surface);
	[CCode (cname = "station_android_foreground_bind", cheader_filename = "station.h")]
	public static void android_foreground_bind (Gdk.Surface surface, string application_class, string service_class);
	[CCode (cname = "station_android_foreground_set_text", cheader_filename = "station.h")]
	public static void android_foreground_set_text (string text);
	[CCode (cname = "station_android_set_resume_handler", cheader_filename = "station.h")]
	public static void android_set_resume_handler (Station.ResumeFunc cb);
}
