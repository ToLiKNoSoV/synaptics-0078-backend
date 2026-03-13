#!/bin/bash
set -e

echo "Настройка прав доступа..."

# Права для C-утилиты
if [ -f /usr/bin/synaptics-capture ]; then
    chmod 4755 /usr/bin/synaptics-capture
    echo "✓ /usr/bin/synaptics-capture"
fi

# Права для C-бэкенда
if [ -f /usr/lib/synaptics-0078-backend/synaptics-backend ]; then
    chmod 755 /usr/lib/synaptics-0078-backend/synaptics-backend
    echo "✓ /usr/lib/synaptics-0078-backend/synaptics-backend"
fi

# Права для PAM модуля
if [ -f /usr/lib/security/pam_synaptics.so ]; then
    chmod 755 /usr/lib/security/pam_synaptics.so
    echo "✓ PAM модуль /usr/lib/security/pam_synaptics.so"
fi

# Удаляем старый PAM скрипт и его следы
if [ -f /usr/lib/synaptics-0078-backend/check-fingerprint.sh ]; then
    rm -f /usr/lib/synaptics-0078-backend/check-fingerprint.sh
    echo "✓ Удален старый PAM скрипт"
fi

# Права для systemd unit
chmod 644 /etc/systemd/system/synaptics-backend.service 2>/dev/null || true
echo "✓ systemd unit"

# Права для DBus конфига
chmod 644 /etc/dbus-1/system.d/net.reactivated.Fprint.conf 2>/dev/null || true
echo "✓ DBus конфиг"

# Права для PAM файлов
chmod 644 /etc/pam.d/kscreenlocker 2>/dev/null || true
chmod 644 /etc/pam.d/kde-fingerprint 2>/dev/null || true
chmod 644 /etc/pam.d/sddm 2>/dev/null || true
chmod 644 /etc/pam.d/sddm-fingerprint 2>/dev/null || true
chmod 644 /etc/pam.d/sudo 2>/dev/null || true
chmod 644 /etc/pam.d/login 2>/dev/null || true
chmod 644 /etc/pam.d/gdm-password 2>/dev/null || true
chmod 644 /etc/pam.d/polkit-1 2>/dev/null || true
chmod 644 /etc/pam.d/fingerprint-auth 2>/dev/null || true
echo "✓ PAM файлы"

# Включение и запуск сервиса
systemctl daemon-reload
systemctl enable synaptics-backend
systemctl restart synaptics-backend
echo "✓ Сервис запущен"

echo "Готово!"
