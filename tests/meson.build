foreach name : ['action', 'config', 'entry_parse', 'fdm', 'kb', 'shellwords', 'spawn', 'state']
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
