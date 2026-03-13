#!/bin/bash
set -e

echo "Настройка прав доступа..."

# Права для бэкенда
if [ -f /usr/lib/synaptics-0078-backend/synaptics-backend ]; then
    chmod 755 /usr/lib/synaptics-0078-backend/synaptics-backend
    echo "✓ /usr/lib/synaptics-0078-backend/synaptics-backend"
fi

# Права для PAM модуля
if [ -f /usr/lib/security/pam_synaptics.so ]; then
    chmod 755 /usr/lib/security/pam_synaptics.so
    echo "✓ PAM модуль /usr/lib/security/pam_synaptics.so"
fi

# Удаляем старые утилиты (если есть)
rm -f /usr/bin/synaptics-capture
rm -f /usr/bin/nbis-helper

# Права для systemd unit
chmod 644 /etc/systemd/system/synaptics-backend.service 2>/dev/null || true
echo "✓ systemd unit"

# Права для DBus конфига
chmod 644 /etc/dbus-1/system.d/net.reactivated.Fprint.conf 2>/dev/null || true
echo "✓ DBus конфиг"

# Включение и запуск сервиса
systemctl daemon-reload
systemctl enable synaptics-backend
systemctl restart synaptics-backend
echo "✓ Сервис запущен"

echo "Готово!"
