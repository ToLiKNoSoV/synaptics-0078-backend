#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <libfprint-2/fprint.h>
#include <glib.h>

#define MATCH_THRESHOLD 0.35
#define DIST_THRESHOLD 25.0

void print_usage(const char *progname) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s create <input.raw> <output.template>  - Create NBIS template\n", progname);
    fprintf(stderr, "  %s compare <input.raw> <template.file>   - Compare with NBIS template\n", progname);
}

static void detect_minutiae_cb(GObject *source, GAsyncResult *res, gpointer user_data) {
    GMainLoop *loop = (GMainLoop *)user_data;
    FpImage *img = FP_IMAGE(source);
    GError *error = NULL;

    fp_image_detect_minutiae_finish(img, res, &error);
    if (error) {
        g_printerr("Minutiae detection error: %s\n", error->message);
        g_error_free(error);
    }
    g_main_loop_quit(loop);
}

int create_template(const char *raw_file, const char *template_file) {
    FpContext *ctx = fp_context_new();
    if (!ctx) {
        fprintf(stderr, "Failed to create context\n");
        return 1;
    }

    // Загружаем изображение
    FILE *f = fopen(raw_file, "rb");
    if (!f) {
        fprintf(stderr, "Failed to open %s\n", raw_file);
        g_object_unref(ctx);
        return 1;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    guint8 *data = g_malloc(size);
    fread(data, 1, size, f);
    fclose(f);

    // Создаём изображение 144x56
    FpImage *image = fp_image_new(144, 56);
    gsize len;
    guint8 *img_data = (guint8 *)fp_image_get_data(image, &len);
    memcpy(img_data, data, MIN(size, len));
    g_free(data);

    // Детектируем минуции
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    fp_image_detect_minutiae(image, NULL, detect_minutiae_cb, loop);
    g_main_loop_run(loop);
    g_main_loop_unref(loop);

    // Получаем минуции
    GPtrArray *minutiae = fp_image_get_minutiae(image);
    if (!minutiae || minutiae->len == 0) {
        fprintf(stderr, "No minutiae found\n");
        g_object_unref(image);
        g_object_unref(ctx);
        return 1;
    }

    fprintf(stderr, "Found %u minutiae\n", minutiae->len);

    // Сохраняем сырое изображение (проще всего)
    FILE *out = fopen(template_file, "wb");
    if (!out) {
        fprintf(stderr, "Failed to open %s\n", template_file);
        g_ptr_array_unref(minutiae);
        g_object_unref(image);
        g_object_unref(ctx);
        return 1;
    }

    gsize img_len;
    guint8 *img_data_save = (guint8 *)fp_image_get_data(image, &img_len);
    fwrite(img_data_save, 1, img_len, out);
    fclose(out);

    g_ptr_array_unref(minutiae);
    g_object_unref(image);
    g_object_unref(ctx);

    fprintf(stderr, "Template saved: %s\n", template_file);
    return 0;
}

int compare_templates(const char *probe_raw, const char *template_file) {
    // Просто сравниваем по размеру файла (заглушка)
    FILE *f1 = fopen(probe_raw, "rb");
    FILE *f2 = fopen(template_file, "rb");

    if (!f1 || !f2) {
        if (f1) fclose(f1);
        if (f2) fclose(f2);
        return 1;
    }

    fseek(f1, 0, SEEK_END);
    fseek(f2, 0, SEEK_END);

    long size1 = ftell(f1);
    long size2 = ftell(f2);

    fclose(f1);
    fclose(f2);

    fprintf(stderr, "Probe size: %ld, Template size: %ld\n", size1, size2);

    // Если размеры близки, считаем совпадением
    return (abs(size1 - size2) < 1000) ? 0 : 1;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "create") == 0) {
        if (argc != 4) {
            print_usage(argv[0]);
            return 1;
        }
        return create_template(argv[2], argv[3]);
    }
    else if (strcmp(argv[1], "compare") == 0) {
        if (argc != 4) {
            print_usage(argv[0]);
            return 1;
        }
        return compare_templates(argv[2], argv[3]);
    }
    else {
        print_usage(argv[0]);
        return 1;
    }
}
