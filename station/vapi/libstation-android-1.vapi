/* Android-only extras for libstation: background-operation helpers that take a
 * Gdk.Surface (kept out of the core libstation-1.vapi so desktop consumers need
 * no GTK). Hand-written; pairs with the station_android_* C API in the Service
 * module. Add `--pkg libstation-android-1` only on Android. */

[CCode (cprefix = "Station", lower_case_cprefix = "station_", cheader_filename = "station.h")]
namespace Station {
	[CCode (cname = "StationResumeFunc", has_target = false)]
	public delegate void ResumeFunc ();

	[CCode (cname = "station_android_battery_unrestricted")]
	public static bool android_battery_unrestricted (Gdk.Surface surface);
	[CCode (cname = "station_android_request_battery_unrestricted")]
	public static void android_request_battery_unrestricted (Gdk.Surface surface);
	[CCode (cname = "station_android_open_notification_settings")]
	public static void android_open_notification_settings (Gdk.Surface surface);
	[CCode (cname = "station_android_foreground_bind")]
	public static void android_foreground_bind (Gdk.Surface surface, string application_class, string service_class);
	[CCode (cname = "station_android_foreground_set_text")]
	public static void android_foreground_set_text (string text);
	[CCode (cname = "station_android_set_resume_handler")]
	public static void android_set_resume_handler (Station.ResumeFunc cb);
}
