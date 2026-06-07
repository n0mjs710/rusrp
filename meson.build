project('rusrp', 'c',
  version : '0.1.0',
  default_options : [
    'c_std=c11',
    'warning_level=2',
    'b_pie=true',
  ]
)

cc = meson.get_compiler('c')
add_project_arguments('-D_GNU_SOURCE', language : 'c')

alsa_dep   = dependency('alsa')
sd_dep     = dependency('libsystemd')
thread_dep = dependency('threads')
m_dep      = cc.find_library('m')

tomlc99_lib = static_library('tomlc99',
  files('vendor/tomlc99/toml.c'),
  c_args : ['-Wno-unused-result'],
  install : false,
)

tomlc99_dep = declare_dependency(
  link_with : tomlc99_lib,
  include_directories : include_directories('vendor/tomlc99'),
)

subdir('src')

install_data(
  'config/rusrp.toml.example',
  install_dir : get_option('sysconfdir') / 'rusrp',
)

systemd_system_unit_dir = get_option('systemd_system_unit_dir')
if systemd_system_unit_dir == ''
  systemd_dep = dependency('systemd', required : false)
  if systemd_dep.found()
    systemd_system_unit_dir = systemd_dep.get_variable('systemdsystemunitdir')
  else
    systemd_system_unit_dir = get_option('prefix') / 'lib/systemd/system'
  endif
endif

install_data(
  'systemd/rusrp@.service',
  install_dir : systemd_system_unit_dir,
)

install_data(
  'udev/90-cm119a.rules',
  install_dir : get_option('sysconfdir') / '../lib/udev/rules.d',
)
