station_headers += files('station-tray.h')           # station-tray-private.h is internal
station_core    += files('station-tray.c')
if host_os == 'windows'
  station_backends += files('windows/station-tray-win.c')
elif host_os == 'darwin'
  station_backends += files('macos/station-tray-mac.m')
elif host_os == 'android'
  station_backends += files('android/station-tray-android.c')
else
  station_backends += files('linux/station-tray-sni.c')            # StatusNotifierItem over GDBus
endif
