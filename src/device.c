#include "device.h"
#include <glib.h>
#include <stdio.h>

static FpContext *ctx = NULL;
static FpDevice *dev = NULL;

int device_init(void) {
    // Создаём контекст libfprint
    ctx = fp_context_new();
    if (!ctx) {
        g_printerr("Failed to create libfprint context\n");
        return -1;
    }

    // Получаем список устройств
    GPtrArray *devices = fp_context_get_devices(ctx);
    if (!devices || devices->len == 0) {
        g_printerr("No fingerprint devices found\n");
        g_object_unref(ctx);
        ctx = NULL;
        return -1;
    }

    // Ищем наше устройство Synaptics 0078
    for (guint i = 0; i < devices->len; i++) {
        FpDevice *d = g_ptr_array_index(devices, i);
        const char *driver = fp_device_get_driver(d);

        if (g_strcmp0(driver, "synaptics_0078") == 0) {
            dev = g_object_ref(d);
            g_print("Found Synaptics 0078 device\n");
            break;
        }
    }

    g_ptr_array_unref(devices);

    if (!dev) {
        g_printerr("Synaptics 0078 device not found\n");
        g_object_unref(ctx);
        ctx = NULL;
        return -1;
    }

    return 0;
}

FpDevice* device_get(void) {
    return dev;
}

void device_close(void) {
    if (dev) {
        g_object_unref(dev);
        dev = NULL;
    }
    if (ctx) {
        g_object_unref(ctx);
        ctx = NULL;
    }
}
