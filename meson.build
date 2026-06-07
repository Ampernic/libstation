# libstation — cross-platform desktop/mobile integration for GTK apps:
# tray/status, background service + autostart, single-instance control IPC, and
# platform shell helpers, with one API and native backends per OS (Linux, Windows,
# macOS, Android). Written in C/GObject; ships a GIR/typelib + Vala binding so it
# is usable from C, Vala, Python and JS.
project('libstation', 'c',
  version: '0.1.0',
  meson_version: '>= 0.62',
  license: 'LGPL-2.1-or-later',
  default_options: ['warning_level=2', 'c_std=gnu11'],
)

# API/soname version, independent of the release version (Station-1).
api_version = '1'

gnome = import('gnome')

station_deps = [
  dependency('glib-2.0'),
  dependency('gobject-2.0'),
  dependency('gio-2.0'),
]

cc = meson.get_compiler('c')
host_os = host_machine.system()

subdir('station')
