foreach name : ['action', 'config', 'dmenu', 'entry_parse', 'fdm', 'kb', 'shellwords', 'spawn', 'state']
  test(
    name,
    executable(
      'test_' + name,
      'test_' + name + '.c',
      protocol_headers,
      include_directories: core_inc,
      link_with: core_lib,
      dependencies: core_deps,
    ),
  )
endforeach

test(
  'example_config',
  executable(
    'test_example_config',
    'test_example_config.c',
    protocol_headers,
    include_directories: core_inc,
    link_with: core_lib,
    dependencies: core_deps,
  ),
  args: [meson.project_source_root() / 'examples' / 'lm10.ini'],
)
