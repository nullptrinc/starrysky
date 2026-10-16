# Stardust: a declarative UI framework in the spirit of BlackBerry Cascades.
# For now: the render core and a C++ demo; the QML front end comes next.
add_languages('cpp', native: false)
ui_cpp_args = ['-Wno-unused-parameter', '-Wno-missing-field-initializers']

wayland_protocols = dependency('wayland-protocols')
xdg_shell_xml = wayland_protocols.get_variable('pkgdatadir') / 'stable/xdg-shell/xdg-shell.xml'
xdg_shell_code = custom_target('xdg-shell-protocol.c',
  input: xdg_shell_xml, output: 'xdg-shell-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'])
xdg_shell_client = custom_target('xdg-shell-client-protocol.h',
  input: xdg_shell_xml, output: 'xdg-shell-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'])

ui_deps = [
  dependency('wayland-client'),
  dependency('wayland-egl'),
  dependency('egl'),
  dependency('glesv2'),
  dependency('freetype2'),
  dependency('threads'),
  math,
]

ui_core = static_library('stardust-core',
  'core/bridge.cpp', 'core/tree.cpp', 'core/renderer.cpp', 'core/core.cpp',
  xdg_shell_code, xdg_shell_client,
  dependencies: ui_deps,
  cpp_args: ui_cpp_args,
  override_options: ['cpp_std=c++20'])

ui_core_dep = declare_dependency(
  link_with: ui_core,
  include_directories: include_directories('core'),
  dependencies: ui_deps)

executable('stardust-demo', 'demo/demo.cpp',
  dependencies: ui_core_dep,
  cpp_args: ui_cpp_args,
  override_options: ['cpp_std=c++20'],
  install_rpath: rpath,
  install: true)

# QML front end: the QtQml engine as the language, Stardust for everything else.
qt6 = import('qt6')
qt6_dep = dependency('qt6', modules: ['Core', 'Qml'], method: 'pkg-config', required: false)
if qt6_dep.found()
  qml_moc = qt6.compile_moc(headers: ['qml/stardust.h', 'qml/constellation-qml.h'],
    dependencies: qt6_dep)
  executable('stardust', 'qml/main.cpp', 'qml/stardust.cpp', 'qml/constellation-qml.cpp', qml_moc,
    dependencies: [ui_core_dep, qt6_dep, constellation_dep],
    cpp_args: ui_cpp_args,
    override_options: ['cpp_std=c++20'],
    install_rpath: rpath,
    install: true)
  install_data('qml/demo.qml', 'qml/dialer.qml', 'qml/battery.qml', install_dir: get_option('datadir') / 'stardust' / 'examples')
endif
