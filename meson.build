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
wayland_protocols = dependency('wayland-protocols', version: '>=1.31')
pangocairo = dependency('pangocairo', include_type: 'system')
glib = dependency('glib-2.0', include_type: 'system')
xkbcommon = dependency('xkbcommon', include_type: 'system')
m = meson.get_compiler('c').find_library('m', required: false)

wayland_scanner_dep = dependency('wayland-scanner', native: true)
wayland_scanner = find_program(
  wayland_scanner_dep.get_variable('wayland_scanner'),
  native: true,
)

wl_protocol_dir = wayland_protocols.get_variable('pkgdatadir')

protocols = [
  'third-party/wlr-layer-shell-unstable-v1.xml',
  # Only for xdg_popup_interface, which layer-shell's get_popup refers to.
  wl_protocol_dir / 'stable/xdg-shell/xdg-shell.xml',
  wl_protocol_dir / 'stable/viewporter/viewporter.xml',
  wl_protocol_dir / 'staging/fractional-scale/fractional-scale-v1.xml',
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

# ztd allocates through util's xmalloc, so util links after it.
util_lib = static_library(
  'lm10-util',
  'src/util.c',
  include_directories: include_directories('include'),
)

third_party_lib = static_library(
  'lm10-third-party',
  'src/ztd.c',
  protocol_code,
  protocol_headers,
  include_directories: [include_directories('include'), third_party_inc],
  link_with: util_lib,
  dependencies: [wayland_client],
  override_options: ['warning_level=0'],
)

# Everything but main, so the tests can link against it.
core_srcs = files(
  'src/action.c',
  'src/config.c',
  'src/draw.c',
  'src/entry_parse.c',
  'src/fdm.c',
  'src/frame.c',
  'src/input.c',
  'src/kb.c',
  'src/render.c',
  'src/shell.c',
  'src/shellwords.c',
  'src/spawn.c',
  'src/state.c',
  'src/wayl.c',
)

core_inc = [include_directories('include'), third_party_inc]
core_deps = [wayland_client, glib, pangocairo, xkbcommon, m]

core_lib = static_library(
  'lm10-core',
  core_srcs,
  protocol_headers,
  include_directories: core_inc,
  link_with: third_party_lib,
  dependencies: core_deps,
)

executable(
  'lm10',
  'src/main.c',
  protocol_headers,
  include_directories: core_inc,
  link_with: core_lib,
  dependencies: core_deps,
  install: true,
)

install_data(
  'README.md',
  'examples/lm10.ini',
  install_dir: get_option('datadir') / 'doc' / meson.project_name(),
)

subdir('tests')
