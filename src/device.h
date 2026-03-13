#ifndef DEVICE_H
#define DEVICE_H

#include <libfprint-2/fprint.h>

/**
 * Инициализация устройства Synaptics
 * @return 0 при успехе, -1 при ошибке
 */
int device_init(void);

/**
 * Получить указатель на устройство
 * @return FpDevice* или NULL если устройство не найдено
 */
FpDevice* device_get(void);

/**
 * Закрыть устройство и освободить ресурсы
 */
void device_close(void);

#endif // DEVICE_H
