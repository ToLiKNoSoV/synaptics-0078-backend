#include <stdio.h>
#include <libusb-1.0/libusb.h>
#include <glib.h>
#include <libfprint-2/fprint.h>
#include "device.h"

static FpContext *ctx = NULL;
static FpDevice *dev = NULL;
gboolean device_opened = FALSE;  // Определение переменной

int device_init(void) {
    if (ctx)
        return 0;
    ctx = fp_context_new();
    if (!ctx) {
        g_printerr("Failed to create libfprint context\n");
        return -1;
    }
    return 0;
}

FpDevice* device_get(void) {
    if (dev) {
        return dev;
    }

    GPtrArray *devices = fp_context_get_devices(ctx);
    if (!devices || devices->len == 0) {
        g_printerr("No fingerprint devices found\n");
        return NULL;
    }

    dev = g_object_ref(g_ptr_array_index(devices, 0));
    g_ptr_array_unref(devices);

    return dev;
}

void device_close(void) {
    // Устройство не закрываем, оставляем открытым
    g_print("DEBUG: device_close() called (ignored)\n");
}

void device_shutdown(void) {
    if (dev) {
        fp_device_close_sync(dev, NULL, NULL);
        g_object_unref(dev);
        dev = NULL;
    }
    if (ctx) {
        g_object_unref(ctx);
        ctx = NULL;
    }
    device_opened = FALSE;
}
