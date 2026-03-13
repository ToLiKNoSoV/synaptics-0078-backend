#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libfprint-2/fprint.h>
#include <glib.h>

#define IMG_WIDTH 144
#define IMG_HEIGHT 56

void print_usage(const char *progname) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s capture [timeout] [wait_remove]           - Capture fingerprint\n", progname);
}

int capture_mode(int argc, char *argv[]) {
    int timeout = 30;
    int wait_remove = 1;

    if (argc > 2) timeout = atoi(argv[2]);
    if (argc > 3) wait_remove = atoi(argv[3]);

    FpContext *ctx = fp_context_new();
    if (!ctx) {
        fprintf(stderr, "Failed to create context\n");
        return 1;
    }

    GPtrArray *devices = fp_context_get_devices(ctx);
    if (!devices || devices->len == 0) {
        fprintf(stderr, "No devices found\n");
        g_object_unref(ctx);
        return 1;
    }

    FpDevice *dev = NULL;
    for (guint i = 0; i < devices->len; i++) {
        FpDevice *d = g_ptr_array_index(devices, i);
        const char *driver = fp_device_get_driver(d);
        if (g_strcmp0(driver, "synaptics_0078") == 0) {
            dev = g_object_ref(d);
            break;
        }
    }

    g_ptr_array_unref(devices);

    if (!dev) {
        fprintf(stderr, "No synaptics_0078 device found\n");
        g_object_unref(ctx);
        return 1;
    }

    GError *error = NULL;
    if (!fp_device_open_sync(dev, NULL, &error)) {
        fprintf(stderr, "Failed to open device: %s\n", error->message);
        g_clear_error(&error);
        g_object_unref(dev);
        g_object_unref(ctx);
        return 1;
    }

    fprintf(stderr, "Waiting for finger (timeout %d seconds)...\n", timeout);
    FpImage *image = fp_device_capture_sync(dev, timeout, NULL, &error);

    if (image) {
        gsize len;
        const guint8 *data = fp_image_get_data(image, &len);

        char filename[256];
        snprintf(filename, sizeof(filename), "/run/fingerprint_raw_%d.raw", getpid());

        FILE *f = fopen(filename, "wb");
        if (f) {
            fwrite(data, 1, len, f);
            fclose(f);
            printf("%s\n", filename);
            fflush(stdout);

            if (wait_remove) {
                fprintf(stderr, "Finger captured. Please remove finger...\n");
                sleep(1);
                fprintf(stderr, "OK\n");
            }
        } else {
            fprintf(stderr, "Failed to open %s for writing\n", filename);
        }

        g_object_unref(image);
    } else {
        fprintf(stderr, "Capture failed: %s\n", error->message);
        g_clear_error(&error);
    }

    fp_device_close_sync(dev, NULL, NULL);
    g_object_unref(dev);
    g_object_unref(ctx);

    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "capture") == 0) {
        return capture_mode(argc, argv);
    } else {
        print_usage(argv[0]);
        return 1;
    }
}
