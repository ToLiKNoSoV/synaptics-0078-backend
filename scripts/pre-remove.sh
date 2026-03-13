#!/bin/bash
# Скрипт перед удалением

set -e

echo "Остановка сервиса..."
systemctl stop synaptics-backend
systemctl disable synaptics-backend

echo "Готово к удалению"
