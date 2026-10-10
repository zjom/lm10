project(
  'lm10',
  'c',
  version: '0.1.0',
  license: 'MIT',
  meson_version: '>=0.58.0',
  default_options: [
    'warning_level=3',
    'buildtype=debugoptimized',
    'c_std=gnu11'
  ],
)
add_project_arguments('-D_GNU_SOURCE', language: 'c')
fs = import('fs')

# include_type: 'system' turns -I into -isystem, so warnings from
# glib/pango/wayland headers don't leak into your build output.

wayland_client = dependency('wayland-client', include_type: 'system')
wayland_protocols = dependency('wayland-protocols', version: '>=1.21')
pangocairo = dependency('pangocairo', include_type: 'system')
xkbcommon = dependency('xkbcommon', include_type: 'system')

wayland_scanner_dep = dependency('wayland-scanner', native: true)
wayland_scanner = find_program(
  wayland_scanner_dep.get_variable('wayland_scanner'),
  native: true,
)

wl_protocol_dir = wayland_protocols.get_variable('pkgdatadir')

protocols = [
  'third-party/wlr-layer-shell-unstable-v1.xml',
  wl_protocol_dir / 'stable/xdg-shell/xdg-shell.xml',
  wl_protocol_dir / 'staging/xdg-activation/xdg-activation-v1.xml',
]

protocol_headers = []
protocol_code = []
foreach xml : protocols
  name = fs.stem(xml)
  protocol_headers += custom_target(
    name + '-client-header',
    input: xml,
    output: '@BASENAME@-client-protocol.h',
    command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
  )
  protocol_code += custom_target(
    name + '-private-code',
    input: xml,
    output: '@BASENAME@-protocol.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
  )
endforeach

third_party_inc = include_directories(
  'third-party',
  'third-party/ztd',
  is_system: true,
)

third_party_lib = static_library(
  'lm10-third-party',
  'src/ztd.c',
  protocol_code,
  protocol_headers,
  include_directories: third_party_inc,
  dependencies: [wayland_client],
  override_options: ['warning_level=0'],
)

srcs = files(
  'src/draw.c',
  'src/fdm.c',
  'src/input.c',
  'src/frame.c',
  'src/main.c',
  'src/render.c',
  'src/state.c',
  'src/wayl.c',
  'src/kb.c',
  'src/config.c',
)

executable(
  'lm10',
  srcs,
  protocol_headers,
  include_directories: [include_directories('include'), third_party_inc],
  link_with: third_party_lib,
  dependencies: [
    wayland_client,
    pangocairo,
    xkbcommon,
  ],
  install: true,
)
