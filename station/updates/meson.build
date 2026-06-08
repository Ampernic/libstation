station_headers  += files('station-updates.h', 'station-updates-schema.h')
station_core     += files('station-updates.c', 'station-updates-schema.c')
station_internal += files('station-http.c')          # desktop HTTPS transport
if host_os == 'android'
  station_backends += files('android/station-updates-http-android.c')   # java.net transport
endif
