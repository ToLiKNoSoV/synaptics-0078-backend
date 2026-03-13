#include "storage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <pwd.h>
#include <dirent.h>

static char* get_user_storage_dir(const char *username) {
    static char path[1024];
    snprintf(path, sizeof(path), "/var/lib/fprint/%s", username);
    return path;
}

static char* get_finger_path(const char *username, const char *finger) {
    static char path[1024];
    snprintf(path, sizeof(path), "/var/lib/fprint/%s/%s", username, finger);
    return path;
}

static void ensure_user_dir(const char *username) {
    char *dir = get_user_storage_dir(username);
    g_mkdir_with_parents(dir, 0755);

    // Устанавливаем правильного владельца
    struct passwd *pw = getpwnam(username);
    if (pw) {
        chown(dir, pw->pw_uid, pw->pw_gid);
    }
}

int storage_save_print(FpPrint *print, const char *username, const char *finger) {
    if (!print || !username || !finger) return -1;

    // Создаём директорию пользователя
    ensure_user_dir(username);

    // Сериализуем шаблон
    guchar *data = NULL;
    gsize length = 0;
    GError *error = NULL;

    if (!fp_print_serialize(print, &data, &length, &error)) {
        g_printerr("Failed to serialize print: %s\n", error->message);
        g_error_free(error);
        return -1;
    }

    // Сохраняем в файл
    char *path = get_finger_path(username, finger);
    FILE *f = fopen(path, "wb");
    if (!f) {
        g_printerr("Failed to open %s for writing\n", path);
        g_free(data);
        return -1;
    }

    fwrite(data, 1, length, f);
    fclose(f);
    g_free(data);

    // Устанавливаем правильного владельца
    struct passwd *pw = getpwnam(username);
    if (pw) {
        chown(path, pw->pw_uid, pw->pw_gid);
    }

    g_print("Saved fingerprint: %s\n", path);
    return 0;
}

FpPrint* storage_load_print(const char *username, const char *finger) {
    if (!username || !finger) return NULL;

    char *path = get_finger_path(username, finger);
    FILE *f = fopen(path, "rb");
    if (!f) {
        g_print("No fingerprint found at %s\n", path);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    guchar *data = g_malloc(size);
    fread(data, 1, size, f);
    fclose(f);

    GError *error = NULL;
    FpPrint *print = fp_print_deserialize(NULL, data, size, &error);
    if (!print) {
        g_printerr("Failed to deserialize print: %s\n", error->message);
        g_error_free(error);
        g_free(data);
        return NULL;
    }

    g_free(data);
    g_print("Loaded fingerprint: %s\n", path);
    return print;
}

GPtrArray* storage_list_fingers(const char *username) {
    if (!username) return NULL;

    char *dir_path = get_user_storage_dir(username);
    GDir *dir = g_dir_open(dir_path, 0, NULL);
    if (!dir) return NULL;

    GPtrArray *fingers = g_ptr_array_new_with_free_func(g_free);

    const char *name;
    while ((name = g_dir_read_name(dir)) != NULL) {
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;

        char *path = g_build_filename(dir_path, name, NULL);
        if (g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
            g_ptr_array_add(fingers, g_strdup(name));
        }
        g_free(path);
    }

    g_dir_close(dir);
    return fingers;
}

int storage_delete_finger(const char *username, const char *finger) {
    if (!username || !finger) return -1;

    char *path = get_finger_path(username, finger);
    if (unlink(path) == 0) {
        g_print("Deleted fingerprint: %s\n", path);
        return 0;
    }

    g_printerr("Failed to delete %s\n", path);
    return -1;
}
