# Synaptics 0078 Fingerprint Driver

Полноценный драйвер и бэкенд для сканера отпечатков Synaptics 06cb:0078.

## Установка

```bash
# Сборка и установка
cd synaptics-0078-project
meson setup builddir
cd builddir
ninja
sudo ninja install
Что устанавливается
/usr/bin/synaptics-capture - утилита захвата отпечатков (setuid root)

/usr/bin/nbis-helper - утилита работы с шаблонами

/usr/lib/synaptics-0078-backend/ - Python бэкенд и PAM скрипт

/etc/systemd/system/synaptics-backend.service - systemd сервис

/etc/dbus-1/system.d/net.reactivated.Fprint.conf - DBus конфигурация

/etc/pam.d/* - обновленные PAM конфиги для аутентификации

/usr/lib/synaptics-0078/scripts/ - вспомогательные скрипты

После установки
Сервис запустится автоматически. Отпечатки можно настроить через:

KDE System Settings → Users

или через командную строку

Удаление
bash
sudo /usr/lib/synaptics-0078/scripts/pre-remove.sh
sudo rm -rf /usr/bin/synaptics-capture /usr/bin/nbis-helper /usr/lib/synaptics-0078-backend /usr/lib/synaptics-0078
sudo rm /etc/systemd/system/synaptics-backend.service
sudo rm /etc/dbus-1/system.d/net.reactivated.Fprint.conf
# PAM файлы нужно восстановить вручную или через restore-pam.sh
Восстановление PAM
bash
sudo /usr/lib/synaptics-0078/scripts/restore-pam.sh
