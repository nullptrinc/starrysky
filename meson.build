project('starry', 'c',
  license: 'MIT',
  version: '0.2',
  default_options: ['c_std=gnu11', 'warning_level=2', 'buildtype=release'])

add_project_arguments('-DWLR_USE_UNSTABLE', '-Wno-unused-parameter',
  '-Wno-missing-field-initializers', language: 'c')

prefix = get_option('prefix')
plugin_dir = get_option('libdir') / 'starry' / 'plugins'
add_project_arguments(
  '-DSTARRY_STARBOARD_PATH="@0@"'.format(prefix / get_option('bindir') / 'starboard'),
  '-DSTARRY_LAUNCH_SOCKET="/run/starry-launch.sock"',
  '-DSTARRY_PLUGIN_DIR="@0@"'.format(prefix / plugin_dir),
  '-DSTARRY_APPS_PATH="@0@"'.format(prefix / get_option('sysconfdir') / 'starry' / 'apps'),
  '-DSTARRY_DATA_DIR="@0@"'.format(prefix / get_option('datadir') / 'starry'),
  language: 'c')
# A private wlroots next to the binaries (e.g. /opt/starry/lib); system prefixes need none.
rpath = prefix in ['/usr', '/usr/local'] ? '' : prefix / get_option('libdir')

cc = meson.get_compiler('c')
math = cc.find_library('m')
wayland_scanner = find_program('wayland-scanner', native: true)

protocol_xml = files('protocol/starry-system-v1.xml')
protocol_code = custom_target('starry-system-v1-protocol.c',
  input: protocol_xml, output: '@BASENAME@-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
protocol_server = custom_target('starry-system-v1-protocol.h',
  input: protocol_xml, output: '@BASENAME@-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])
protocol_client = custom_target('starry-system-v1-client-protocol.h',
  input: protocol_xml, output: '@BASENAME@-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])

screencopy_xml = files('protocol/wlr-screencopy-unstable-v1.xml')
screencopy_code = custom_target('wlr-screencopy-protocol.c',
  input: screencopy_xml, output: '@BASENAME@-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
screencopy_client = custom_target('wlr-screencopy-client-protocol.h',
  input: screencopy_xml, output: '@BASENAME@-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])

plugin_include = include_directories('include')
install_headers('include/starry-plugin.h')

executable('starry-compositor',
  'compositor/main.c', 'compositor/system.c', protocol_code, protocol_server,
  dependencies: [
    dependency('wlroots-0.20'),
    dependency('wayland-server'),
    dependency('xkbcommon'),
    math,
  ],
  install_rpath: rpath,
  install: true)

subdir('constellation')

nanosvg = static_library('nanosvg', 'starboard/nanosvg.c',
  dependencies: math,
  override_options: ['warning_level=0'])

executable('starboard',
  'starboard/main.c', 'starboard/home.c', 'starboard/status.c', 'starboard/switcher.c',
  'starboard/canvas.c', 'starboard/battery.c', protocol_code, protocol_client,
  link_with: nanosvg,
  include_directories: plugin_include,
  dependencies: [
    constellation_dep,
    dependency('wayland-client'),
    dependency('freetype2'),
    cc.find_library('dl', required: false),
    math,
  ],
  install_rpath: rpath,
  install: true)

# Starts apps for starboard, which runs without root.
executable('starry-launchd', 'launcher/starry-launchd.c',
  dependencies: constellation_dep,
  c_args: '-DSTARRY_BINDIR="@0@"'.format(prefix / get_option('bindir')),
  install: true)

# The vibration motor for starboard's haptic tick; other input stays closed.
install_data('data/udev/70-starry-haptics.rules',
  install_dir: get_option('datadir') / 'starry' / 'udev')

# Screenshots of the whole screen, for root on the device.
executable('starry-shot', 'tools/starry-shot.c', screencopy_code, screencopy_client,
  dependencies: dependency('wayland-client'),
  install_rpath: rpath,
  install: true)

configure_file(input: 'data/starry-run.in', output: 'starry-run',
  configuration: {'bindir': prefix / get_option('bindir')},
  install_dir: get_option('bindir'), install_mode: 'rwxr-xr-x')

# sysvinit service; link it into /etc/init.d to start Starry at boot.
configure_file(input: 'data/sysvinit/starry.in', output: 'starry.init',
  configuration: {'bindir': prefix / get_option('bindir')},
  install_dir: get_option('datadir') / 'starry' / 'sysvinit', install_mode: 'rwxr-xr-x',
  install_tag: 'runtime')

shared_module('wifi', 'plugins/wifi.c',
  name_prefix: '',
  include_directories: plugin_include,
  install: true,
  install_dir: plugin_dir)

if get_option('stardust')
  subdir('stardust')
endif

install_data('data/icons/battery.svg', 'data/icons/battery.map',
  install_dir: get_option('datadir') / 'starry' / 'icons')
