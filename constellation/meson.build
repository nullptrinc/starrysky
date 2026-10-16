# Constellation: the /blinks namespace broker, its client library and services.
constellation_lib = static_library('constellation', 'object.c', 'client.c',
  pic: true)
constellation_dep = declare_dependency(
  link_with: constellation_lib,
  include_directories: include_directories('.'))
install_headers('constellation.h')

executable('constellationd', 'constellationd.c',
  dependencies: constellation_dep,
  install: true)
executable('blinks', 'blinks.c',
  dependencies: constellation_dep,
  install: true)
executable('constellation-power', 'services/power.c',
  dependencies: constellation_dep,
  install: true)

configure_file(input: 'constellation.init.in', output: 'constellation.init',
  configuration: {'bindir': prefix / get_option('bindir')},
  install_dir: get_option('datadir') / 'starry' / 'sysvinit', install_mode: 'rwxr-xr-x',
  install_tag: 'runtime')
