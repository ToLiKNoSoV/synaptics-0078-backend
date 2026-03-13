#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <math.h>
#include <glib.h>
#include <gio/gio.h>
#include <libfprint-2/fprint.h>

#define SERVICE_NAME "net.reactivated.Fprint"
#define OBJECT_PATH "/net/reactivated/Fprint/Device/0"
#define MANAGER_PATH "/net/reactivated/Fprint/Manager"

static GMainLoop *loop = NULL;
static FpContext *fp_ctx = NULL;
static char *default_user = NULL;
static char *claimed_user = NULL;
static GDBusConnection *system_bus = NULL;

/* Состояние для enroll операций */
typedef struct {
    char *sender;
    char *finger;
    int stage;
    char captures_dir[1024];
    GDBusMethodInvocation *invocation;
    gboolean active;
} EnrollState;

static EnrollState *current_enroll = NULL;

/* Состояние для verify операций */
typedef struct {
    char *sender;
    char *user;
    char *finger;
    char template_path[1024];
    int attempt;
    int max_attempts;
    GDBusMethodInvocation *invocation;
    gboolean active;
} VerifyState;

static VerifyState *current_verify = NULL;

/* ========== Вспомогательные функции ========== */

static char *get_default_user(void) {
    struct passwd *pw;
    setpwent();
    while ((pw = getpwent()) != NULL) {
        if (pw->pw_uid >= 1000 && pw->pw_uid < 65534) {
            endpwent();
            return g_strdup(pw->pw_name);
        }
    }
    endpwent();
    return g_strdup("tolik");
}

static char *get_user_dir(const char *user) {
    return g_strdup_printf("/var/lib/fprint/%s", user);
}

static void ensure_user_dir(const char *user) {
    if (!user || strcmp(user, "root") == 0) return;

    char *dir = get_user_dir(user);
    if (g_mkdir_with_parents(dir, 0755) == 0) {
        struct passwd *pw = getpwnam(user);
        if (pw) {
            chown(dir, pw->pw_uid, pw->pw_gid);
        }
    }
    g_free(dir);
}

/* ========== D-Bus сигналы ========== */

static void send_enroll_status_signal(const char *result, gboolean done) {
    if (!system_bus) return;

    GVariant *value = g_variant_new("(sb)", result, done);
    g_dbus_connection_emit_signal(system_bus,
                                  NULL,
                                  OBJECT_PATH,
                                  "net.reactivated.Fprint.Device",
                                  "EnrollStatus",
                                  value,
                                  NULL);
    g_print("📢 EnrollStatus signal sent: %s, done=%d\n", result, done);
}

static void send_verify_status_signal(const char *result, gboolean done) {
    if (!system_bus) return;

    GVariant *value = g_variant_new("(sb)", result, done);
    g_dbus_connection_emit_signal(system_bus,
                                  NULL,
                                  OBJECT_PATH,
                                  "net.reactivated.Fprint.Device",
                                  "VerifyStatus",
                                  value,
                                  NULL);
    g_print("📢 VerifyStatus signal sent: %s, done=%d\n", result, done);
}

/* ========== Улучшенное сравнение файлов ========== */
static int compare_files(const char *file1, const char *file2) {
    FILE *f1 = fopen(file1, "rb");
    FILE *f2 = fopen(file2, "rb");

    if (!f1 || !f2) {
        if (f1) fclose(f1);
        if (f2) fclose(f2);
        return 0;
    }

    /* Получаем размеры */
    fseek(f1, 0, SEEK_END);
    fseek(f2, 0, SEEK_END);
    long size1 = ftell(f1);
    long size2 = ftell(f2);

    /* Если размеры сильно отличаются - точно не совпадают */
    if (abs(size1 - size2) > 1000) {
        fclose(f1);
        fclose(f2);
        return 0;
    }

    /* Перематываем в начало */
    fseek(f1, 0, SEEK_SET);
    fseek(f2, 0, SEEK_SET);

    /* Сравниваем гистограммы яркости */
    double hist1[256] = {0};
    double hist2[256] = {0};
    int total_pixels = 0;

    unsigned char buffer1[8192];
    unsigned char buffer2[8192];
    size_t bytes1, bytes2;

    while (!feof(f1) && !feof(f2)) {
        bytes1 = fread(buffer1, 1, sizeof(buffer1), f1);
        bytes2 = fread(buffer2, 1, sizeof(buffer2), f2);

        size_t min_bytes = (bytes1 < bytes2) ? bytes1 : bytes2;

        for (size_t i = 0; i < min_bytes; i++) {
            hist1[buffer1[i]]++;
            hist2[buffer2[i]]++;
            total_pixels++;
        }
    }

    fclose(f1);
    fclose(f2);

    if (total_pixels == 0) return 0;

    /* Нормализация гистограмм */
    for (int i = 0; i < 256; i++) {
        hist1[i] /= total_pixels;
        hist2[i] /= total_pixels;
    }

    /* Вычисляем корреляцию Пирсона */
    double mean1 = 0, mean2 = 0;
    for (int i = 0; i < 256; i++) {
        mean1 += hist1[i];
        mean2 += hist2[i];
    }
    mean1 /= 256;
    mean2 /= 256;

    double covar = 0, var1 = 0, var2 = 0;
    for (int i = 0; i < 256; i++) {
        double diff1 = hist1[i] - mean1;
        double diff2 = hist2[i] - mean2;
        covar += diff1 * diff2;
        var1 += diff1 * diff1;
        var2 += diff2 * diff2;
    }

    if (var1 == 0 || var2 == 0) return 0;

    double correlation = covar / (sqrt(var1) * sqrt(var2));

    /* Вычисляем среднеквадратичное отклонение */
    double mse = 0;
    for (int i = 0; i < 256; i++) {
        double diff = hist1[i] - hist2[i];
        mse += diff * diff;
    }
    mse /= 256;
    double similarity = 1.0 / (1.0 + mse * 100);

    /* Комбинированная метрика */
    double score = (correlation + similarity) / 2;

    int match = (score > 0.7) ? 1 : 0;

    g_print("  Correlation: %.3f, Similarity: %.3f, Score: %.3f, Match: %s\n",
            correlation, similarity, score, match ? "YES" : "NO");

    return match;
}

/* ========== Обработка захвата для verify ========== */

static gboolean verify_capture_complete(gpointer data) {
    VerifyState *state = (VerifyState *)data;

    if (!state->active) {
        g_print("Verify state inactive, stopping\n");
        g_free(state->sender);
        g_free(state->user);
        g_free(state->finger);
        g_free(state);
        current_verify = NULL;
        return G_SOURCE_REMOVE;
    }

    if (state->attempt > state->max_attempts) {
        g_print("Max attempts (%d) reached, stopping verify\n", state->max_attempts);
        send_verify_status_signal("verify-no-match", TRUE);
        g_free(state->sender);
        g_free(state->user);
        g_free(state->finger);
        g_free(state);
        current_verify = NULL;
        return G_SOURCE_REMOVE;
    }

    g_print("Starting verify capture (attempt %d/%d)\n",
            state->attempt, state->max_attempts);

    /* Запускаем synaptics-capture для захвата */
    FILE *fp = popen("/usr/bin/synaptics-capture capture 10 1", "r");
    if (!fp) {
        g_print("Capture failed to start\n");
        state->attempt++;
        if (state->attempt <= state->max_attempts) {
            g_timeout_add_seconds(1, verify_capture_complete, state);
        } else {
            send_verify_status_signal("verify-no-match", TRUE);
            g_free(state->sender);
            g_free(state->user);
            g_free(state->finger);
            g_free(state);
            current_verify = NULL;
        }
        return G_SOURCE_REMOVE;
    }

    char filename[256];
    if (!fgets(filename, sizeof(filename), fp)) {
        g_print("Capture failed - no output\n");
        pclose(fp);
        state->attempt++;
        if (state->attempt <= state->max_attempts) {
            g_timeout_add_seconds(1, verify_capture_complete, state);
        } else {
            send_verify_status_signal("verify-no-match", TRUE);
            g_free(state->sender);
            g_free(state->user);
            g_free(state->finger);
            g_free(state);
            current_verify = NULL;
        }
        return G_SOURCE_REMOVE;
    }
    pclose(fp);

    /* Убираем перевод строки */
    size_t len = strlen(filename);
    if (len > 0 && filename[len-1] == '\n')
        filename[len-1] = '\0';

    g_print("Captured: %s\n", filename);

    /* Сравниваем с шаблоном */
    int match = compare_files(filename, state->template_path);
    unlink(filename);

    if (match) {
        g_print("✓ Match found!\n");
        send_verify_status_signal("verify-match", TRUE);
        g_free(state->sender);
        g_free(state->user);
        g_free(state->finger);
        g_free(state);
        current_verify = NULL;
        return G_SOURCE_REMOVE;
    } else {
        g_print("✗ No match\n");
        state->attempt++;
        if (state->attempt <= state->max_attempts) {
            g_print("  Next attempt in 1 second...\n");
            g_timeout_add_seconds(1, verify_capture_complete, state);
        } else {
            g_print("  Max attempts reached, reporting failure\n");
            send_verify_status_signal("verify-no-match", TRUE);
            g_free(state->sender);
            g_free(state->user);
            g_free(state->finger);
            g_free(state);
            current_verify = NULL;
        }
    }

    return G_SOURCE_REMOVE;
}

/* ========== Обработка захвата для enroll ========== */

static gboolean capture_stage(gpointer data) {
    EnrollState *state = (EnrollState *)data;

    if (!state->active) {
        return G_SOURCE_REMOVE;
    }

    g_print("Starting capture stage %d/5\n", state->stage);

    /* Запускаем synaptics-capture для захвата */
    FILE *fp = popen("/usr/bin/synaptics-capture capture 15 1", "r");
    if (!fp) {
        g_print("Capture failed to start\n");
        send_enroll_status_signal("enroll-failed", TRUE);
        state->active = FALSE;
        return G_SOURCE_REMOVE;
    }

    char filename[256];
    if (!fgets(filename, sizeof(filename), fp)) {
        g_print("Capture failed - no output\n");
        pclose(fp);
        send_enroll_status_signal("enroll-failed", TRUE);
        state->active = FALSE;
        return G_SOURCE_REMOVE;
    }
    pclose(fp);

    /* Убираем перевод строки */
    size_t len = strlen(filename);
    if (len > 0 && filename[len-1] == '\n')
        filename[len-1] = '\0';

    g_print("Stage %d: captured %s\n", state->stage, filename);

    /* Копируем во временную директорию */
    char dest[1024];
    int ret = snprintf(dest, sizeof(dest), "%s/stage_%d.raw",
                       state->captures_dir, state->stage);
    if (ret < 0 || (size_t)ret >= sizeof(dest)) {
        g_print("Stage %d: destination path too long\n", state->stage);
        unlink(filename);
        send_enroll_status_signal("enroll-failed", TRUE);
        state->active = FALSE;
        return G_SOURCE_REMOVE;
    }

    FILE *src = fopen(filename, "rb");
    FILE *dst = fopen(dest, "wb");

    if (!src || !dst) {
        g_print("Stage %d: failed to copy file\n", state->stage);
        if (src) fclose(src);
        if (dst) fclose(dst);
        unlink(filename);
        send_enroll_status_signal("enroll-failed", TRUE);
        state->active = FALSE;
        return G_SOURCE_REMOVE;
    }

    char buffer[8192];
    size_t bytes;
    while ((bytes = fread(buffer, 1, sizeof(buffer), src)) > 0) {
        fwrite(buffer, 1, bytes, dst);
    }

    fclose(src);
    fclose(dst);
    unlink(filename);

    g_print("Stage %d completed\n", state->stage);

    /* Отправляем сигнал о прогрессе */
    if (state->stage < 5) {
        send_enroll_status_signal("enroll-stage-passed", FALSE);
    }

    state->stage++;

    if (state->stage > 5) {
        /* Все стадии завершены - создаем финальный шаблон */
        g_print("All 5 stages completed\n");

        const char *user = claimed_user ? claimed_user : default_user;
        char *final_file = g_build_filename("/var/lib/fprint", user, state->finger, NULL);

        /* Объединяем все захваты в один файл */
        FILE *final = fopen(final_file, "wb");
        if (final) {
            for (int i = 1; i <= 5; i++) {
                char stage_file[1024];
                ret = snprintf(stage_file, sizeof(stage_file), "%s/stage_%d.raw",
                               state->captures_dir, i);
                if (ret < 0 || (size_t)ret >= sizeof(stage_file)) {
                    g_print("  Stage file path too long\n");
                    continue;
                }

                FILE *stage = fopen(stage_file, "rb");
                if (stage) {
                    char buf[8192];
                    size_t n;
                    while ((n = fread(buf, 1, sizeof(buf), stage)) > 0) {
                        fwrite(buf, 1, n, final);
                    }
                    fclose(stage);
                }
            }
            fclose(final);

            /* Устанавливаем правильного владельца */
            struct passwd *pw = getpwnam(user);
            if (pw) {
                chown(final_file, pw->pw_uid, pw->pw_gid);
            }

            g_print("✓ Enrollment completed: %s\n", final_file);
            send_enroll_status_signal("enroll-completed", TRUE);
        } else {
            g_print("✗ Failed to save final template\n");
            send_enroll_status_signal("enroll-failed", TRUE);
        }

        g_free(final_file);

        /* Очищаем временную директорию */
        char cmd[1024];
        ret = snprintf(cmd, sizeof(cmd), "rm -rf %s", state->captures_dir);
        if (ret >= 0 && (size_t)ret < sizeof(cmd)) {
            system(cmd);
        }

        state->active = FALSE;
        return G_SOURCE_REMOVE;
    }

    /* Запускаем следующую стадию через 1 секунду */
    g_timeout_add_seconds(1, capture_stage, state);
    return G_SOURCE_REMOVE;
}

/* ========== Поток enroll ========== */

static gpointer enroll_thread(gpointer data) {
    (void)data;

    g_print("=== Enroll thread started ===\n");

    if (!current_enroll) {
        g_print("No enroll state\n");
        return NULL;
    }

    /* Создаем временную директорию для захватов */
    int ret = snprintf(current_enroll->captures_dir,
                       sizeof(current_enroll->captures_dir),
                       "/tmp/enroll_%d_%s", getpid(), current_enroll->finger);
    if (ret < 0 || (size_t)ret >= sizeof(current_enroll->captures_dir)) {
        g_print("Failed to create temp dir path - too long\n");
        send_enroll_status_signal("enroll-failed", TRUE);
        g_free(current_enroll->sender);
        g_free(current_enroll->finger);
        g_free(current_enroll);
        current_enroll = NULL;
        return NULL;
    }

    g_print("Creating temp dir: %s\n", current_enroll->captures_dir);
    if (mkdir(current_enroll->captures_dir, 0700) != 0) {
        g_print("Failed to create temp dir\n");
        send_enroll_status_signal("enroll-failed", TRUE);
        g_free(current_enroll->sender);
        g_free(current_enroll->finger);
        g_free(current_enroll);
        current_enroll = NULL;
        return NULL;
    }

    current_enroll->stage = 1;
    current_enroll->active = TRUE;

    /* Отправляем сигнал готовности */
    send_enroll_status_signal("enroll-ready", FALSE);

    /* Запускаем первую стадию */
    capture_stage(current_enroll);

    return NULL;
}

/* ========== D-Bus методы ========== */

static void handle_method_call(GDBusConnection *connection,
                               const gchar *sender,
                               const gchar *object_path,
                               const gchar *interface_name,
                               const gchar *method_name,
                               GVariant *parameters,
                               GDBusMethodInvocation *invocation,
                               gpointer user_data) {

    g_print("========================================\n");
    g_print("Method called: '%s'\n", method_name);
    g_print("  interface: '%s'\n", interface_name);
    g_print("  object_path: '%s'\n", object_path);
    g_print("  sender: '%s'\n", sender);
    g_print("  parameters type: %s\n", g_variant_get_type_string(parameters));

    if (g_strcmp0(interface_name, "net.reactivated.Fprint.Device") != 0) {
        g_dbus_method_invocation_return_error(invocation,
                                              G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_INTERFACE,
                                              "Unknown interface");
        return;
    }

    if (g_strcmp0(method_name, "ListEnrolledFingers") == 0) {
        g_print("ListEnrolledFingers called\n");

        const char *user = claimed_user ? claimed_user : default_user;
        char *user_dir = get_user_dir(user);
        GDir *dir = g_dir_open(user_dir, 0, NULL);
        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE("as"));

        if (dir) {
            const char *name;
            while ((name = g_dir_read_name(dir)) != NULL) {
                if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
                    continue;

                char *path = g_build_filename(user_dir, name, NULL);
                if (g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
                    g_print("  Found finger: %s\n", name);
                    g_variant_builder_add(&builder, "s", name);
                }
                g_free(path);
            }
            g_dir_close(dir);
        }
        g_free(user_dir);

        g_dbus_method_invocation_return_value(invocation,
                                              g_variant_new("(as)", &builder));
    }
    else if (g_strcmp0(method_name, "Claim") == 0) {
        const char *username;
        g_variant_get(parameters, "(&s)", &username);

        g_free(claimed_user);

        if (username && strlen(username) > 0) {
            claimed_user = g_strdup(username);
            g_print("Claimed for user: %s\n", username);
        } else {
            claimed_user = g_strdup(default_user);
            g_print("Claimed for user (default): %s\n", default_user);
        }

        ensure_user_dir(claimed_user);
        g_dbus_method_invocation_return_value(invocation, NULL);
    }
    else if (g_strcmp0(method_name, "Release") == 0) {
        g_print("Release called\n");
        g_free(claimed_user);
        claimed_user = NULL;
        g_dbus_method_invocation_return_value(invocation, NULL);
    }
    else if (g_strcmp0(method_name, "EnrollStart") == 0) {
        const char *finger;
        g_variant_get(parameters, "(&s)", &finger);
        g_print("EnrollStart for finger: %s\n", finger);

        if (current_enroll) {
            g_dbus_method_invocation_return_error(invocation,
                                                  G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                  "Enroll already in progress");
            return;
        }

        if (!finger || strlen(finger) == 0) {
            g_dbus_method_invocation_return_error(invocation,
                                                  G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                  "No finger specified");
            return;
        }

        current_enroll = g_new0(EnrollState, 1);
        current_enroll->sender = g_strdup(sender);
        current_enroll->finger = g_strdup(finger);
        current_enroll->invocation = g_object_ref(invocation);

        GThread *thread = g_thread_new("enroll-thread", enroll_thread, invocation);
        g_thread_unref(thread);

        g_dbus_method_invocation_return_value(invocation, NULL);
    }
    else if (g_strcmp0(method_name, "EnrollStop") == 0) {
        g_print("EnrollStop called\n");
        if (current_enroll) {
            current_enroll->active = FALSE;
            g_free(current_enroll->sender);
            g_free(current_enroll->finger);
            g_free(current_enroll);
            current_enroll = NULL;
        }
        g_dbus_method_invocation_return_value(invocation, NULL);
    }
    else if (g_strcmp0(method_name, "DeleteEnrolledFinger") == 0) {
        const char *finger;
        g_variant_get(parameters, "(&s)", &finger);
        g_print("DeleteEnrolledFinger: %s\n", finger ? finger : "NULL");

        if (!finger || strlen(finger) == 0) {
            g_dbus_method_invocation_return_error(invocation,
                                                  G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                  "No finger specified");
            return;
        }

        const char *user = claimed_user ? claimed_user : default_user;
        if (!user) {
            g_dbus_method_invocation_return_error(invocation,
                                                  G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                  "No user specified");
            return;
        }

        char *finger_path = g_build_filename("/var/lib/fprint", user, finger, NULL);
        g_print("  Path: %s\n", finger_path);

        if (g_file_test(finger_path, G_FILE_TEST_EXISTS)) {
            if (unlink(finger_path) == 0) {
                g_print("  Deleted successfully\n");
            } else {
                g_print("  Failed to delete: %s\n", strerror(errno));
                g_free(finger_path);
                g_dbus_method_invocation_return_error(invocation,
                                                      G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                      "Failed to delete fingerprint: %s",
                                                      strerror(errno));
                return;
            }
        } else {
            g_print("  File does not exist\n");
        }

        g_free(finger_path);
        g_dbus_method_invocation_return_value(invocation, NULL);
    }
    else if (g_strcmp0(method_name, "VerifyStart") == 0) {
        const char *finger;
        g_variant_get(parameters, "(&s)", &finger);
        g_print("VerifyStart for finger: %s\n", finger ? finger : "NULL");

        if (current_verify) {
            g_dbus_method_invocation_return_error(invocation,
                                                  G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                  "Verify already in progress");
            return;
        }

        if (!finger || strlen(finger) == 0) {
            g_dbus_method_invocation_return_error(invocation,
                                                  G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                  "No finger specified");
            return;
        }

        const char *user = claimed_user ? claimed_user : default_user;
        if (!user) {
            g_dbus_method_invocation_return_error(invocation,
                                                  G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                  "No user specified");
            return;
        }

        /* Проверяем существование шаблона */
        char *template_path = g_build_filename("/var/lib/fprint", user, finger, NULL);
        if (!g_file_test(template_path, G_FILE_TEST_IS_REGULAR)) {
            g_print("  No template found for %s/%s\n", user, finger);
            g_free(template_path);
            send_verify_status_signal("verify-no-match", TRUE);
            g_dbus_method_invocation_return_value(invocation, NULL);
            return;
        }

        /* Создаем состояние verify */
        current_verify = g_new0(VerifyState, 1);
        current_verify->sender = g_strdup(sender);
        current_verify->user = g_strdup(user);
        current_verify->finger = g_strdup(finger);
        strncpy(current_verify->template_path, template_path, sizeof(current_verify->template_path) - 1);
        current_verify->template_path[sizeof(current_verify->template_path) - 1] = '\0';
        current_verify->attempt = 1;
        current_verify->max_attempts = 3;
        current_verify->active = TRUE;
        current_verify->invocation = g_object_ref(invocation);
        g_free(template_path);

        /* Запускаем первую попытку */
        g_timeout_add(100, verify_capture_complete, current_verify);

        g_dbus_method_invocation_return_value(invocation, NULL);
    }
    else if (g_strcmp0(method_name, "VerifyStop") == 0) {
        g_print("VerifyStop called\n");
        if (current_verify) {
            current_verify->active = FALSE;
            g_free(current_verify->sender);
            g_free(current_verify->user);
            g_free(current_verify->finger);
            g_free(current_verify);
            current_verify = NULL;
        }
        g_dbus_method_invocation_return_value(invocation, NULL);
    }
    else if (g_strcmp0(method_name, "GetCapabilities") == 0) {
        g_print("GetCapabilities called\n");
        g_dbus_method_invocation_return_value(invocation,
                                              g_variant_new("(u)", 5));
    }
    else if (g_strcmp0(method_name, "GetScanType") == 0) {
        g_print("GetScanType called\n");
        g_dbus_method_invocation_return_value(invocation,
                                              g_variant_new("(s)", "press"));
    }
    else {
        g_dbus_method_invocation_return_error(invocation,
                                              G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
                                              "Unknown method: %s", method_name);
    }
                               }

                               /* ========== Обработчики свойств ========== */

                               static GVariant *handle_get_property(GDBusConnection *connection,
                                                                    const gchar *sender,
                                                                    const gchar *object_path,
                                                                    const gchar *interface_name,
                                                                    const gchar *property_name,
                                                                    GError **error,
                                                                    gpointer user_data) {

                                   g_print("Get property: %s\n", property_name);

                                   if (g_strcmp0(interface_name, "net.reactivated.Fprint.Device") != 0) {
                                       g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_INTERFACE,
                                                   "Unknown interface");
                                       return NULL;
                                   }

                                   if (g_strcmp0(property_name, "scan-type") == 0) {
                                       return g_variant_new_string("press");
                                   }
                                   else if (g_strcmp0(property_name, "num-enroll-stages") == 0) {
                                       return g_variant_new_int32(5);
                                   }

                                   g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY,
                                               "Unknown property: %s", property_name);
                                   return NULL;
                                                                    }

                                                                    static gboolean handle_set_property(GDBusConnection *connection,
                                                                                                        const gchar *sender,
                                                                                                        const gchar *object_path,
                                                                                                        const gchar *interface_name,
                                                                                                        const gchar *property_name,
                                                                                                        GVariant *value,
                                                                                                        GError **error,
                                                                                                        gpointer user_data) {

                                                                        g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                                                    "Properties are read-only");
                                                                        return FALSE;
                                                                                                        }

                                                                                                        /* ========== Manager методы ========== */

                                                                                                        static void handle_manager_method_call(GDBusConnection *connection,
                                                                                                                                               const gchar *sender,
                                                                                                                                               const gchar *object_path,
                                                                                                                                               const gchar *interface_name,
                                                                                                                                               const gchar *method_name,
                                                                                                                                               GVariant *parameters,
                                                                                                                                               GDBusMethodInvocation *invocation,
                                                                                                                                               gpointer user_data) {

                                                                                                            g_print("Manager method called: %s\n", method_name);

                                                                                                            if (g_strcmp0(method_name, "GetDefaultDevice") == 0) {
                                                                                                                g_dbus_method_invocation_return_value(invocation,
                                                                                                                                                      g_variant_new("(o)", OBJECT_PATH));
                                                                                                            }
                                                                                                            else if (g_strcmp0(method_name, "GetDevices") == 0) {
                                                                                                                const char *username, *application;
                                                                                                                g_variant_get(parameters, "(&s&s)", &username, &application);

                                                                                                                GVariantBuilder builder;
                                                                                                                g_variant_builder_init(&builder, G_VARIANT_TYPE("ao"));
                                                                                                                g_variant_builder_add(&builder, "o", OBJECT_PATH);

                                                                                                                g_dbus_method_invocation_return_value(invocation,
                                                                                                                                                      g_variant_new("(ao)", &builder));
                                                                                                            }
                                                                                                            else {
                                                                                                                g_dbus_method_invocation_return_error(invocation,
                                                                                                                                                      G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
                                                                                                                                                      "Unknown method: %s", method_name);
                                                                                                            }
                                                                                                                                               }

                                                                                                                                               static GVariant *handle_manager_get_property(GDBusConnection *connection,
                                                                                                                                                                                            const gchar *sender,
                                                                                                                                                                                            const gchar *object_path,
                                                                                                                                                                                            const gchar *interface_name,
                                                                                                                                                                                            const gchar *property_name,
                                                                                                                                                                                            GError **error,
                                                                                                                                                                                            gpointer user_data) {
                                                                                                                                                   g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY,
                                                                                                                                                               "Manager has no properties");
                                                                                                                                                   return NULL;
                                                                                                                                                                                            }

                                                                                                                                                                                            static gboolean handle_manager_set_property(GDBusConnection *connection,
                                                                                                                                                                                                                                        const gchar *sender,
                                                                                                                                                                                                                                        const gchar *object_path,
                                                                                                                                                                                                                                        const gchar *interface_name,
                                                                                                                                                                                                                                        const gchar *property_name,
                                                                                                                                                                                                                                        GVariant *value,
                                                                                                                                                                                                                                        GError **error,
                                                                                                                                                                                                                                        gpointer user_data) {
                                                                                                                                                                                                g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                                                                                                                                                                            "Manager has no writable properties");
                                                                                                                                                                                                return FALSE;
                                                                                                                                                                                                                                        }

                                                                                                                                                                                                                                        /* ========== Инициализация D-Bus ========== */

                                                                                                                                                                                                                                        static void on_bus_acquired(GDBusConnection *connection,
                                                                                                                                                                                                                                                                const gchar *name,
                                                                                                                                                                                                                                                                gpointer user_data) {

                                                                                                                                                                                                                                            g_print("Bus acquired: %s\n", name);
                                                                                                                                                                                                                                            system_bus = connection;

                                                                                                                                                                                                                                            GDBusInterfaceVTable device_vtable = {
                                                                                                                                                                                                                                                handle_method_call,
                                                                                                                                                                                                                                                handle_get_property,
                                                                                                                                                                                                                                                handle_set_property
                                                                                                                                                                                                                                            };

                                                                                                                                                                                                                                            GDBusNodeInfo *device_info = g_dbus_node_info_new_for_xml(
                                                                                                                                                                                                                                                "<node>"
                                                                                                                                                                                                                                                "  <interface name='net.reactivated.Fprint.Device'>"
                                                                                                                                                                                                                                                "    <method name='ListEnrolledFingers'>"
                                                                                                                                                                                                                                                "      <arg type='s' name='dummy' direction='in'/>"
                                                                                                                                                                                                                                                "      <arg type='as' name='fingers' direction='out'/>"
                                                                                                                                                                                                                                                "    </method>"
                                                                                                                                                                                                                                                "    <method name='Claim'>"
                                                                                                                                                                                                                                                "      <arg type='s' name='username' direction='in'/>"
                                                                                                                                                                                                                                                "    </method>"
                                                                                                                                                                                                                                                "    <method name='Release'/>"
                                                                                                                                                                                                                                                "    <method name='DeleteEnrolledFinger'>"
                                                                                                                                                                                                                                                "      <arg type='s' name='finger' direction='in'/>"
                                                                                                                                                                                                                                                "    </method>"
                                                                                                                                                                                                                                                "    <method name='EnrollStart'>"
                                                                                                                                                                                                                                                "      <arg type='s' name='finger' direction='in'/>"
                                                                                                                                                                                                                                                "    </method>"
                                                                                                                                                                                                                                                "    <method name='EnrollStop'/>"
                                                                                                                                                                                                                                                "    <method name='VerifyStart'>"
                                                                                                                                                                                                                                                "      <arg type='s' name='finger' direction='in'/>"
                                                                                                                                                                                                                                                "    </method>"
                                                                                                                                                                                                                                                "    <method name='VerifyStop'/>"
                                                                                                                                                                                                                                                "    <method name='GetCapabilities'>"
                                                                                                                                                                                                                                                "      <arg type='u' name='caps' direction='out'/>"
                                                                                                                                                                                                                                                "    </method>"
                                                                                                                                                                                                                                                "    <method name='GetScanType'>"
                                                                                                                                                                                                                                                "      <arg type='s' name='type' direction='out'/>"
                                                                                                                                                                                                                                                "    </method>"
                                                                                                                                                                                                                                                "    <signal name='EnrollStatus'>"
                                                                                                                                                                                                                                                "      <arg type='s' name='result'/>"
                                                                                                                                                                                                                                                "      <arg type='b' name='done'/>"
                                                                                                                                                                                                                                                "    </signal>"
                                                                                                                                                                                                                                                "    <signal name='VerifyStatus'>"
                                                                                                                                                                                                                                                "      <arg type='s' name='result'/>"
                                                                                                                                                                                                                                                "      <arg type='b' name='done'/>"
                                                                                                                                                                                                                                                "    </signal>"
                                                                                                                                                                                                                                                "    <property name='scan-type' type='s' access='read'/>"
                                                                                                                                                                                                                                                "    <property name='num-enroll-stages' type='i' access='read'/>"
                                                                                                                                                                                                                                                "  </interface>"
                                                                                                                                                                                                                                                "</node>", NULL);

                                                                                                                                                                                                                                            guint device_reg = g_dbus_connection_register_object(connection,
                                                                                                                                                                                                                                                                OBJECT_PATH,
                                                                                                                                                                                                                                                                device_info->interfaces[0],
                                                                                                                                                                                                                                                                &device_vtable,
                                                                                                                                                                                                                                                                NULL,
                                                                                                                                                                                                                                                                NULL,
                                                                                                                                                                                                                                                                NULL);

                                                                                                                                                                                                                                            if (device_reg == 0) {
                                                                                                                                                                                                                                                g_printerr("Failed to register device object\n");
                                                                                                                                                                                                                                                return;
                                                                                                                                                                                                                                            }
                                                                                                                                                                                                                                            g_print("Device object registered\n");

                                                                                                                                                                                                                                            GDBusInterfaceVTable manager_vtable = {
                                                                                                                                                                                                                                                handle_manager_method_call,
                                                                                                                                                                                                                                                handle_manager_get_property,
                                                                                                                                                                                                                                                handle_manager_set_property
                                                                                                                                                                                                                                            };

                                                                                                                                                                                                                                            GDBusNodeInfo *manager_info = g_dbus_node_info_new_for_xml(
                                                                                                                                                                                                                                                "<node>"
                                                                                                                                                                                                                                                "  <interface name='net.reactivated.Fprint.Manager'>"
                                                                                                                                                                                                                                                "    <method name='GetDefaultDevice'>"
                                                                                                                                                                                                                                                "      <arg type='o' name='device' direction='out'/>"
                                                                                                                                                                                                                                                "    </method>"
                                                                                                                                                                                                                                                "    <method name='GetDevices'>"
                                                                                                                                                                                                                                                "      <arg type='s' name='username' direction='in'/>"
                                                                                                                                                                                                                                                "      <arg type='s' name='application' direction='in'/>"
                                                                                                                                                                                                                                                "      <arg type='ao' name='devices' direction='out'/>"
                                                                                                                                                                                                                                                "    </method>"
                                                                                                                                                                                                                                                "  </interface>"
                                                                                                                                                                                                                                                "</node>", NULL);

                                                                                                                                                                                                                                            guint manager_reg = g_dbus_connection_register_object(connection,
                                                                                                                                                                                                                                                                MANAGER_PATH,
                                                                                                                                                                                                                                                                manager_info->interfaces[0],
                                                                                                                                                                                                                                                                &manager_vtable,
                                                                                                                                                                                                                                                                NULL,
                                                                                                                                                                                                                                                                NULL,
                                                                                                                                                                                                                                                                NULL);

                                                                                                                                                                                                                                            if (manager_reg == 0) {
                                                                                                                                                                                                                                                g_printerr("Failed to register manager object\n");
                                                                                                                                                                                                                                                return;
                                                                                                                                                                                                                                            }
                                                                                                                                                                                                                                            g_print("Manager object registered\n");

                                                                                                                                                                                                                                            g_print("Objects registered successfully\n");
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                static void on_name_acquired(GDBusConnection *connection,
                                                                                                                                                                                                                                                                const gchar *name,
                                                                                                                                                                                                                                                                gpointer user_data) {
                                                                                                                                                                                                                                                                g_print("Name acquired: %s\n", name);
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                static void on_name_lost(GDBusConnection *connection,
                                                                                                                                                                                                                                                                const gchar *name,
                                                                                                                                                                                                                                                                gpointer user_data) {
                                                                                                                                                                                                                                                                g_print("Name lost: %s\n", name);
                                                                                                                                                                                                                                                                g_main_loop_quit(loop);
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                /* ========== main ========== */

                                                                                                                                                                                                                                                                int main(int argc, char *argv[]) {
                                                                                                                                                                                                                                                                default_user = get_default_user();
                                                                                                                                                                                                                                                                g_print("Default user: %s\n", default_user);

                                                                                                                                                                                                                                                                fp_ctx = fp_context_new();
                                                                                                                                                                                                                                                                if (!fp_ctx) {
                                                                                                                                                                                                                                                                g_printerr("Failed to create libfprint context\n");
                                                                                                                                                                                                                                                                return 1;
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                loop = g_main_loop_new(NULL, FALSE);

                                                                                                                                                                                                                                                                guint id = g_bus_own_name(G_BUS_TYPE_SYSTEM,
                                                                                                                                                                                                                                                                SERVICE_NAME,
                                                                                                                                                                                                                                                                G_BUS_NAME_OWNER_FLAGS_NONE,
                                                                                                                                                                                                                                                                on_bus_acquired,
                                                                                                                                                                                                                                                                on_name_acquired,
                                                                                                                                                                                                                                                                on_name_lost,
                                                                                                                                                                                                                                                                NULL,
                                                                                                                                                                                                                                                                NULL);

                                                                                                                                                                                                                                                                g_print("Starting main loop...\n");
                                                                                                                                                                                                                                                                g_main_loop_run(loop);

                                                                                                                                                                                                                                                                g_bus_unown_name(id);
                                                                                                                                                                                                                                                                g_main_loop_unref(loop);
                                                                                                                                                                                                                                                                g_object_unref(fp_ctx);
                                                                                                                                                                                                                                                                g_free(default_user);
                                                                                                                                                                                                                                                                g_free(claimed_user);

                                                                                                                                                                                                                                                                return 0;
                                                                                                                                                                                                                                                                }
