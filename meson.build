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

m_dep = meson.get_compiler('c').find_library('m', required: true)

executable('synaptics-backend',
    [
        'src/main.c',
        'src/device.c',
        'src/storage.c'
    ],
    dependencies: [glib_dep, gio_dep, gio_unix_dep, libfprint_dep, thread_dep, m_dep],
    install: true,
    install_dir: '/usr/lib/synaptics-0078-backend'
)

shared_library('pam_synaptics',
    'src/pam_synaptics.c',
    dependencies: [systemd_dep, pam_dep],
    name_prefix: '',
    install: true,
    install_dir: '/usr/lib/security'
)

install_data('systemd/synaptics-backend.service',
    install_dir: '/etc/systemd/system'
)

install_data('dbus/net.reactivated.Fprint.conf',
    install_dir: '/etc/dbus-1/system.d'
)

install_data('README.md',
    install_dir: '/usr/share/doc/synaptics-0078'
)

meson.add_install_script('scripts/post-install.sh')
