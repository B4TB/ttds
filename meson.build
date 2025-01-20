project('ttds', 'c',
  version : '0.1',
  default_options : ['warning_level=3', 'werror=true', 'c_std=c23'])

libdrm = dependency('libdrm')
libsystemd = dependency('libsystemd')

add_project_arguments([
  '-pedantic', '-D_POSIX_C_SOURCE=200809L'
], language : 'c')

exe = executable('ttds',
  'main.c',
  'threads/ui.c', 'threads/input.c', 'threads/termination.c', 'threads/commands.c',
  'rendering/rendering.c', 'rendering/canvas.c',
  'rendering/backends/drm.c',
  install : true,
  dependencies : [ libdrm, libsystemd ])

test('basic', exe)
