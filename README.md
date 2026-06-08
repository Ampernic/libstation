# libstation

Cross-platform **desktop & mobile integration for GTK apps**. One GObject API,
native backends per OS (Linux, Windows, macOS, Android) — so a GTK4/libadwaita app
gets a system tray, a background service with login autostart, single-instance
control IPC and shell helpers without scattering `#ifdef`s across its code.

GTK4 dropped the status icon and never had a portable way to run in the
background, autostart, or talk to a single running instance on Windows/macOS.
libstation fills that gap.

## Modules

| Module | What it does | Linux | Windows | macOS | Android |
|---|---|---|---|---|---|
| `Station.Platform` | OS detect, open URI, exe path | xdg / GAppInfo | ShellExecute | `open` | (wip) |
| `Station.Tray` | status icon + menu | StatusNotifierItem | Shell_NotifyIcon | NSStatusItem | notification |
| `Station.Control` | single-instance + status/command IPC | Unix socket | TCP + token | Unix socket | in-process |
| `Station.Service` | background daemon lifecycle + autostart | D-Bus / XDG / portal | Run key + taskkill | LaunchAgent | foreground service |

(`Station.Platform` ships first; the rest land module by module.)

## Using it

The library ships a C header, a GObject-Introspection typelib and a Vala binding,
so it works from C, Vala, Python and JavaScript.

```c
#include <station/station.h>
g_autoptr(GError) error = NULL;
station_open_uri ("tg://proxy?server=…", &error);
```

```vala
Station.open_uri ("tg://proxy?server=…");
```

```python
from gi.repository import Station
Station.open_uri("tg://proxy?server=…")
```

## Build

```sh
meson setup _build
meson compile -C _build
meson install -C _build
```

Options: `-Dintrospection=enabled|disabled`, `-Dvapi=true|false`.

## License

LGPL-3.0-or-later.
