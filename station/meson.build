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

# Source is organised one directory per subsystem; each subsystem's meson.build
# (subdir() below) contributes to these lists and picks its own per-OS backend
# from <subsystem>/<os>/. station_headers = public/installed (introspected);
# station_core = public-API sources (introspected); station_internal = internal
# helpers (built in, not introspected); station_backends = native per-OS code.
station_headers = []
station_core = []
station_internal = []
station_backends = []
station_extra_deps = []

# Cross-cutting per-OS deps (the subsystem sources that use them are added below).
if host_os != 'windows'
  # ipc's POSIX transport uses GUnixSocketAddress (<gio/gunixsocketaddress.h>);
  # declare gio-unix explicitly so the include path is right everywhere.
  station_extra_deps += dependency('gio-unix-2.0')
endif
if host_os == 'windows'
  station_extra_deps += cc.find_library('shell32')    # ShellExecute / Shell_NotifyIcon
  station_extra_deps += cc.find_library('advapi32')   # registry (autostart)
  station_extra_deps += cc.find_library('user32')     # window + menu
elif host_os == 'darwin'
  add_languages('objc', native: false)
  station_extra_deps += dependency('appleframeworks', modules: ['Cocoa'])
elif host_os == 'android'
  # gdk-android gives the JNI env + Activity from a surface; jni.h is from the NDK.
  station_extra_deps += dependency('gtk4')
endif

subdir('platform')
subdir('ipc')
subdir('service')
subdir('tray')
subdir('updates')
subdir('single-instance')

header_subdir = 'libstation-' + api_version / 'station'
install_headers(station_headers, version_h, subdir: header_subdir)

# Every subsystem dir is on the include path so the bare-name includes (e.g.
# "station-tray-private.h", "station-http-private.h") resolve from any backend.
station_inc = include_directories('.', 'platform', 'ipc', 'service', 'tray',
                                  'updates', 'single-instance')

libstation = library('station-' + api_version,
  station_core, station_internal, station_backends, version_h,
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
vapi_install_dir = get_option('datadir') / 'vala' / 'vapi'
install_data(
  vapi_dir / (vapi_pkg + '.vapi'),
  vapi_dir / (vapi_pkg + '.deps'),
  install_dir: vapi_install_dir,
)
# The Android extras binding (Gdk.Surface-typed) is only useful — and only
# parseable (it needs the gtk4 vapi) — on Android.
if host_os == 'android'
  install_data(
    vapi_dir / 'libstation-android-' + api_version + '.vapi',
    vapi_dir / 'libstation-android-' + api_version + '.deps',
    install_dir: vapi_install_dir,
  )
endif

# GObject-Introspection (GIR + typelib) — optional, for non-Vala consumers
# (Python/JS). Off on Android (cross can't run g-ir-scanner) and wherever the
# scanner is unavailable.
gir = []
if host_os != 'android' and get_option('introspection').allowed()
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
  # Headers install under libstation-<api>/station/, and the vapi includes
  # <station.h>, so the -I must reach that directory, not just its parent.
  subdirs: 'libstation-' + api_version + '/station',
  requires: ['glib-2.0', 'gobject-2.0', 'gio-2.0'],
  variables: ['vapidir=${datadir}/vala/vapi'],
)
