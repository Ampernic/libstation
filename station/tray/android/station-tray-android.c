/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Android has no status-area tray; the app surfaces status through the
 * foreground-service notification (see station-android.c) instead. Station.Tray
 * still constructs so cross-platform callers need no #ifdef — it's just inert. */
#ifndef STATION_COMPILATION
# define STATION_COMPILATION
#endif
#include "station-tray-private.h"

void
_station_tray_backend_init (StationTray *self G_GNUC_UNUSED)
{
}

void
_station_tray_backend_update (StationTray *self G_GNUC_UNUSED)
{
}

void
_station_tray_backend_dispose (StationTray *self G_GNUC_UNUSED)
{
}
