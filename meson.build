project('synaptics-0078', 'c',
    version: '1.0.0',
    license: 'LGPL2.1+',
    default_options: ['c_std=c99']
)

glib_dep = dependency('glib-2.0')
gio_dep = dependency('gio-2.0')
gio_unix_dep = dependency('gio-unix-2.0')
libfprint_dep = dependency('libfprint-2')
thread_dep = dependency('threads')
systemd_dep = dependency('libsystemd')
pam_dep = dependency('pam', required: true)
math_dep = meson.get_compiler('c').find_library('m', required: true)

# Утилита захвата
executable('synaptics-capture',
    'src/capture.c',
    dependencies: [glib_dep, libfprint_dep, thread_dep],
    install: true,
    install_dir: '/usr/bin'
)

# Основной бэкенд
executable('synaptics-backend',
    'src/main.c',
    dependencies: [glib_dep, gio_dep, gio_unix_dep, libfprint_dep, thread_dep, math_dep],
    install: true,
    install_dir: '/usr/lib/synaptics-0078-backend'
)

# PAM модуль
shared_library('pam_synaptics',
    'src/pam_synaptics.c',
    dependencies: [systemd_dep, pam_dep],
    name_prefix: '',
    install: true,
    install_dir: '/usr/lib/security'
)

# Только необходимые файлы
install_data('README.md',
    install_dir: '/usr/share/doc/synaptics-0078'
)
