#ifndef DEVICE_H
#define DEVICE_H

#include <libfprint-2/fprint.h>

int device_init(void);
FpDevice* device_get(void);
void device_close(void);
void device_shutdown(void);

extern gboolean device_opened;

#endif /* DEVICE_H */
