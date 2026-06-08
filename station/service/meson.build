station_headers += files('station-service.h')
station_core    += files('station-service.c')
if host_os == 'android'
  station_backends += files('android/station-service-android.c')   # background backend
endif
