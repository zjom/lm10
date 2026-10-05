project(
  'lm10',
  'c',
  version: '0.1.0',
  license: 'MIT',
  default_options: ['warning_level=2', 'buildtype=debugoptimized'],
)

wayland_client = dependency('wayland-client')
wayland_protocols = dependency('wayland-protocols')
wayland_scanner = find_program(
  dependency('wayland-scanner', native: true).get_variable('wayland_scanner'),
)

xdg_shell_xml = wayland_protocols.get_variable('pkgdatadir') / 'stable/xdg-shell/xdg-shell.xml'

xdg_shell_header = custom_target(
  'xdg-shell-client-protocol.h',
  input: xdg_shell_xml,
  output: 'xdg-shell-client-protocol.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)

xdg_shell_code = custom_target(
  'xdg-shell-protocol.c',
  input: xdg_shell_xml,
  output: 'xdg-shell-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

executable(
  'lm10',
  'main.c',
  xdg_shell_code,
  xdg_shell_header,
  dependencies: [wayland_client, dependency('pangocairo')],
  install: true,
)
