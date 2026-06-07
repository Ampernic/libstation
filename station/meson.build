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

station_headers = files(
  'station.h',
  'station-platform.h',
  'station-control.h',
  'station-service.h',
)
station_sources = files(
  'station-platform.c',
  'station-control.c',
  'station-service.c',
)

station_extra_deps = []
if host_os == 'windows'
  station_extra_deps += cc.find_library('shell32')    # ShellExecute
  station_extra_deps += cc.find_library('advapi32')   # registry (autostart)
endif

header_subdir = 'libstation-' + api_version / 'station'
install_headers(station_headers, version_h, subdir: header_subdir)

libstation = library('station-' + api_version,
  station_sources, version_h,
  dependencies: station_deps + station_extra_deps,
  c_args: ['-DSTATION_COMPILATION'],
  soversion: '0',
  version: meson.project_version(),
  install: true,
)

station_dep = declare_dependency(
  link_with: libstation,
  include_directories: include_directories('.'),
  dependencies: station_deps,
)

# GObject-Introspection (GIR + typelib) so the library is usable from C, Python, JS.
gir = []
if get_option('introspection').allowed()
  gir = gnome.generate_gir(libstation,
    sources: station_sources + station_headers + [version_h],
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

  # Vala binding generated from the GIR.
  if get_option('vapi')
    gnome.generate_vapi('libstation-' + api_version,
      sources: gir[0],
      packages: ['glib-2.0', 'gobject-2.0', 'gio-2.0'],
      install: true,
    )
  endif
endif

pkgconfig = import('pkgconfig')
pkgconfig.generate(libstation,
  name: 'libstation-' + api_version,
  description: 'Cross-platform desktop/mobile integration for GTK apps',
  subdirs: 'libstation-' + api_version,
  requires: ['glib-2.0', 'gobject-2.0', 'gio-2.0'],
)
