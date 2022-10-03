manymouse_sources = files([
  'linux_evdev.c',
  'macosx_hidmanager.c',
  'macosx_hidutilities.c',
  'manymouse.c',
  'windows_wminput.c',
  'x11_xinput2.c',
])

libmanymouse = static_library('manymouse', manymouse_sources)

manymouse_dep  = declare_dependency(link_with : libmanymouse)
