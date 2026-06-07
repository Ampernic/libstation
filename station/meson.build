ver = meson.project_version().split('.')
conf = configuration_data()
conf.set('VERSION', meson.project_version())
conf.set('MAJOR', ver[0])
conf.set('MINOR', ver[1])
conf.set('MICRO', ver[2])
version_h = configure_file(
  input: 'station-version.h.in',
  output: 'station-version.h',
  configuration: conf,
)

# Public headers (installed + introspected).
station_headers = files(
  'station.h',
  'station-platform.h',
  'station-control.h',
  'station-service.h',
  'station-tray.h',
)
# Sources with public, introspectable API.
station_core = files(
  'station-platform.c',
  'station-control.c',
  'station-service.c',
  'station-tray.c',
)
# Native backends (internal symbols only — excluded from introspection).
station_backends = []
station_extra_deps = []
if host_os == 'windows'
  station_extra_deps += cc.find_library('shell32')    # ShellExecute / Shell_NotifyIcon
  station_extra_deps += cc.find_library('advapi32')   # registry (autostart)
  station_extra_deps += cc.find_library('user32')     # window + menu
  station_backends += files('station-tray-win.c')
elif host_os == 'darwin'
  add_languages('objc', native: false)
  station_extra_deps += dependency('appleframeworks', modules: ['Cocoa'])
  station_backends += files('station-tray-mac.m')
else
  station_backends += files('station-tray-sni.c')      # StatusNotifierItem over GDBus
endif

header_subdir = 'libstation-' + api_version / 'station'
install_headers(station_headers, version_h, subdir: header_subdir)

libstation = library('station-' + api_version,
  station_core, station_backends, version_h,
  dependencies: station_deps + station_extra_deps,
  c_args: ['-DSTATION_COMPILATION'],
  objc_args: ['-DSTATION_COMPILATION'],
  soversion: '0',
  version: meson.project_version(),
  install: true,
)

# GObject-Introspection (GIR + typelib) — scan only the public C API.
gir = []
station_dep_list = station_deps
if get_option('introspection').allowed()
  gir = gnome.generate_gir(libstation,
    sources: station_core + station_headers + [version_h],
    namespace: 'Station',
    nsversion: api_version,
    identifier_prefix: 'Station',
    symbol_prefix: 'station',
    export_packages: 'libstation-' + api_version,
    includes: ['GLib-2.0', 'GObject-2.0', 'Gio-2.0'],
    header: 'station.h',
    extra_args: ['-DSTATION_COMPILATION'],
    install: true,
  )

  if get_option('vapi')
    # The returned dependency carries the .vapi so a Vala consumer (e.g. the
    # subproject case) picks it up just by depending on station_dep — no manual
    # --vapidir/--pkg needed.
    station_dep_list += gnome.generate_vapi('libstation-' + api_version,
      sources: gir[0],
      packages: ['glib-2.0', 'gobject-2.0', 'gio-2.0'],
      install: true,
    )
  endif
endif

station_dep = declare_dependency(
  link_with: libstation,
  include_directories: include_directories('.'),
  dependencies: station_dep_list,
)

pkgconfig = import('pkgconfig')
pkgconfig.generate(libstation,
  name: 'libstation-' + api_version,
  description: 'Cross-platform desktop/mobile integration for GTK apps',
  subdirs: 'libstation-' + api_version,
  requires: ['glib-2.0', 'gobject-2.0', 'gio-2.0'],
)
