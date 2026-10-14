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
  '-DSTARRY_PLUGIN_DIR="@0@"'.format(prefix / plugin_dir),
  '-DSTARRY_APPS_PATH="@0@"'.format(prefix / get_option('sysconfdir') / 'starry' / 'apps'),
  language: 'c')
rpath = prefix / get_option('libdir')

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

executable('starboard',
  'starboard/main.c', 'starboard/home.c', 'starboard/status.c',
  'starboard/canvas.c', protocol_code, protocol_client,
  include_directories: plugin_include,
  dependencies: [
    dependency('wayland-client'),
    dependency('freetype2'),
    cc.find_library('dl', required: false),
    math,
  ],
  install_rpath: rpath,
  install: true)

configure_file(input: 'data/starry-run.in', output: 'starry-run',
  configuration: {'bindir': prefix / get_option('bindir')},
  install_dir: get_option('bindir'), install_mode: 'rwxr-xr-x')

shared_module('wifi', 'plugins/wifi.c',
  name_prefix: '',
  include_directories: plugin_include,
  install: true,
  install_dir: plugin_dir)
