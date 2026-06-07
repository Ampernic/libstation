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
# Native backends — the only place per-OS code lives, under backends/<os>/
# (internal symbols only, excluded from introspection).
station_backends = []
station_extra_deps = []
if host_os == 'windows'
  station_extra_deps += cc.find_library('shell32')    # ShellExecute / Shell_NotifyIcon
  station_extra_deps += cc.find_library('advapi32')   # registry (autostart)
  station_extra_deps += cc.find_library('user32')     # window + menu
  station_backends += files('backends/windows/station-tray-win.c')
elif host_os == 'darwin'
  add_languages('objc', native: false)
  station_extra_deps += dependency('appleframeworks', modules: ['Cocoa'])
  station_backends += files('backends/macos/station-tray-mac.m')
else
  station_backends += files('backends/linux/station-tray-sni.c')   # StatusNotifierItem over GDBus
endif

header_subdir = 'libstation-' + api_version / 'station'
install_headers(station_headers, version_h, subdir: header_subdir)

# The public + private headers live in station/; the backends include them by
# bare name, so put this dir on the include path.
station_inc = include_directories('.')

libstation = library('station-' + api_version,
  station_core, station_backends, version_h,
  dependencies: station_deps + station_extra_deps,
  include_directories: station_inc,
  c_args: ['-DSTATION_COMPILATION'],
  objc_args: ['-DSTATION_COMPILATION'],
  soversion: '0',
  version: meson.project_version(),
  install: true,
)

# Vala binding: a COMMITTED vapi (station/vapi/), the source of truth for Vala
# consumers on every platform — including the Android cross build, where
# g-ir-scanner/vapigen can't run. Generated once from the GIR on a desktop build
# and checked in. Consumers add `--pkg libstation-1 --vapidir <vapidir>`; the
# vapidir is exposed below (source dir for subproject use, datadir for installed).
vapi_pkg = 'libstation-' + api_version
vapi_dir = meson.current_source_dir() / 'vapi'
install_data(
  vapi_dir / (vapi_pkg + '.vapi'),
  vapi_dir / (vapi_pkg + '.deps'),
  install_dir: get_option('datadir') / 'vala' / 'vapi',
)

# GObject-Introspection (GIR + typelib) — optional, for non-Vala consumers
# (Python/JS). Off automatically when g-ir-scanner is unavailable (e.g. cross).
gir = []
if get_option('introspection').allowed()
  gir = gnome.generate_gir(libstation,
    sources: station_core + station_headers + [version_h],
    namespace: 'Station',
    nsversion: api_version,
    identifier_prefix: 'Station',
    symbol_prefix: 'station',
    export_packages: vapi_pkg,
    includes: ['GLib-2.0', 'GObject-2.0', 'Gio-2.0'],
    header: 'station.h',
    extra_args: ['-DSTATION_COMPILATION'],
    install: true,
  )
endif

station_dep = declare_dependency(
  link_with: libstation,
  include_directories: station_inc,
  dependencies: station_deps,
  variables: {'vapidir': vapi_dir},
)

pkgconfig = import('pkgconfig')
pkgconfig.generate(libstation,
  name: vapi_pkg,
  description: 'Cross-platform desktop/mobile integration for GTK apps',
  subdirs: 'libstation-' + api_version,
  requires: ['glib-2.0', 'gobject-2.0', 'gio-2.0'],
  variables: ['vapidir=${datadir}/vala/vapi'],
)
