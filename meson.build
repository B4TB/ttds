project('ttds', 'c',
  version : '0.1',
  default_options : ['warning_level=3', 'werror=true', 'c_std=c23', 'optimization=g'])

libdrm = dependency('libdrm')
libsystemd = dependency('libsystemd')

add_project_arguments([
  '-pedantic', '-D_POSIX_C_SOURCE=200809L', '-D_FORTIFY_SOURCE=1'
], language : 'c')

exe = executable('ttds',
  'main.c', 'threads/ui.c', 'rendering/rendering.h', 'rendering/rendering.c',
  'threads/input.c', 'threads/termination.c', 'threads/commands.c',
  install : true,
  dependencies : [ libdrm, libsystemd ])

test('basic', exe)
