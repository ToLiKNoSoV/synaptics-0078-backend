#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pwd.h>
#include <dirent.h>
#include <glib.h>
#include <libfprint-2/fprint.h>
#include "storage.h"

static char* get_user_path(const char *username) {
    return g_build_filename("/var/lib/fprint", username, NULL);
}

static char* get_finger_path(const char *username, const char *finger) {
    return g_build_filename("/var/lib/fprint", username, finger, NULL);
}

static gboolean ensure_dir_exists(const char *path) {
    struct stat st = {0};
    if (stat(path, &st) == -1) {
        if (mkdir(path, 0700) == -1 && errno != EEXIST) {
            return FALSE;
        }
    }
    return TRUE;
}

int storage_save_print(FpPrint *print, const char *username, const char *finger) {
    char *dir_path = get_user_path(username);
    char *file_path = get_finger_path(username, finger);

    if (!ensure_dir_exists(dir_path)) {
        g_free(dir_path);
        g_free(file_path);
        return -1;
    }

    GError *error = NULL;
    gsize data_size;
    guint8 *data = NULL;

    if (!fp_print_serialize(print, &data, &data_size, &error)) {
        g_warning("Failed to serialize print: %s", error->message);
        g_error_free(error);
        g_free(dir_path);
        g_free(file_path);
        return -1;
    }

    FILE *f = fopen(file_path, "wb");
    if (!f) {
        g_warning("Failed to open %s: %s", file_path, strerror(errno));
        g_free(data);
        g_free(dir_path);
        g_free(file_path);
        return -1;
    }

    size_t written = fwrite(data, 1, data_size, f);
    fclose(f);
    g_free(data);

    if (written != data_size) {
        g_warning("Failed to write all data to %s", file_path);
        g_free(dir_path);
        g_free(file_path);
        return -1;
    }

    struct passwd *pw = getpwnam(username);
    if (pw) {
        chown(file_path, pw->pw_uid, pw->pw_gid);
    }

    g_free(dir_path);
    g_free(file_path);
    return 0;
}

FpPrint* storage_load_print(const char *username, const char *finger) {
    char *file_path = get_finger_path(username, finger);

    FILE *f = fopen(file_path, "rb");
    if (!f) {
        g_print("No fingerprint found at %s\n", file_path);
        g_free(file_path);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long data_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    guint8 *data = g_malloc(data_size);
    size_t read = fread(data, 1, data_size, f);
    fclose(f);

    if (read != (size_t)data_size) {
        g_free(data);
        g_free(file_path);
        return NULL;
    }

    GError *error = NULL;
    FpPrint *print = fp_print_deserialize(data, data_size, &error);
    g_free(data);
    g_free(file_path);

    if (error) {
        g_warning("Failed to deserialize print: %s", error->message);
        g_error_free(error);
        return NULL;
    }

    return print;
}

GPtrArray* storage_list_fingers(const char *username) {
    char *dir_path = get_user_path(username);
    GPtrArray *fingers = g_ptr_array_new_with_free_func(g_free);

    GDir *dir = g_dir_open(dir_path, 0, NULL);
    if (!dir) {
        g_free(dir_path);
        return fingers;
    }

    const char *name;
    while ((name = g_dir_read_name(dir)) != NULL) {
        char *full_path = g_build_filename(dir_path, name, NULL);
        if (g_file_test(full_path, G_FILE_TEST_IS_REGULAR)) {
            g_ptr_array_add(fingers, g_strdup(name));
        }
        g_free(full_path);
    }
    g_dir_close(dir);
    g_free(dir_path);

    return fingers;
}

int storage_delete_finger(const char *username, const char *finger) {
    char *file_path = get_finger_path(username, finger);
    int ret = unlink(file_path);
    g_free(file_path);
    return ret;
}
