project('synaptics-0078-backend', 'c',
        version: '1.0.0',
        license: 'GPL2+',
        default_options: [
            'c_std=c11',
            'warning_level=2'
        ])

cc = meson.get_compiler('c')

# Dependencies
glib_dep = dependency('glib-2.0', version: '>=2.44')
gio_dep = dependency('gio-2.0', version: '>=2.44')
gio_unix_dep = dependency('gio-unix-2.0', version: '>=2.44')
libusb_dep = dependency('libusb-1.0', version: '>=1.0.20')
libfprint_dep = dependency('libfprint-2', version: '>=1.94.0')

deps = [
    glib_dep,
    gio_dep,
    gio_unix_dep,
    libusb_dep,
    libfprint_dep
]

# Executable
executable('synaptics-backend',
           'src/main.c',
           dependencies: deps,
           install: true,
           install_dir: get_option('libexecdir') / 'synaptics-0078-backend')

# Systemd service - используем systemd_systemunit_dir
systemd = dependency('systemd', required: false)
if systemd.found()
    systemd_unit_dir = systemd.get_variable(pkgconfig: 'systemdsystemunitdir')

    configure_file(
        input: 'data/synaptics-backend.service.in',
        output: 'synaptics-backend.service',
        configuration: {
            'LIBEXEC_DIR': get_option('prefix') / get_option('libexecdir')
        },
        install: true,
        install_dir: systemd_unit_dir
    )
endif

# Create state directory
install_emptydir('/var/lib/fprint/synaptics_0078')
