#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libusb-1.0/libusb.h>
#include <glib.h>
#include <gio/gio.h>
#include <libfprint-2/fprint.h>
#include <sys/stat.h>
#include <errno.h>
#include <signal.h>

#define SERVICE_NAME "net.reactivated.Fprint"
#define MANAGER_PATH "/net/reactivated/Fprint/Manager"
#define DEVICE_PATH "/net/reactivated/Fprint/Device/0"
#define MANAGER_INTERFACE "net.reactivated.Fprint.Manager"
#define DEVICE_INTERFACE "net.reactivated.Fprint.Device"

typedef struct {
    FpContext *ctx;
    FpDevice *dev;
    GDBusConnection *connection;
    GDBusNodeInfo *introspection;
    guint owner_id;
    guint registration_id;
    char *current_user;
    char *current_finger;
    gboolean is_enrolling;
    gboolean is_verifying;
    GCancellable *cancellable;
} FprintDevice;

static FprintDevice *fprint_device = NULL;
static GMainLoop *main_loop = NULL;

/* Storage path functions - исправлено использование STATE_DIRECTORY */
static char* get_storage_path(const char *username, const char *finger) {
    const char *state_dir = g_getenv("STATE_DIRECTORY");
    char *base_path;

    if (state_dir) {
        base_path = g_build_filename(state_dir, "synaptics_0078", username, NULL);
    } else {
        base_path = g_build_filename("/var/lib/fprint", "synaptics_0078", username, NULL);
    }

    if (finger) {
        char *full_path = g_build_filename(base_path, finger, NULL);
        g_free(base_path);
        return full_path;
    }

    return base_path;
}

static gboolean ensure_dir_exists(const char *path) {
    struct stat st = {0};
    if (stat(path, &st) == -1) {
        if (mkdir(path, 0700) == -1 && errno != EEXIST) {
            g_warning("Failed to create directory %s: %s", path, strerror(errno));
            return FALSE;
        }
    }
    return TRUE;
}

/* Save print to storage */
static gboolean save_print(FpPrint *print, const char *username, const char *finger) {
    char *dir_path = get_storage_path(username, NULL);
    char *file_path = get_storage_path(username, finger);

    if (!ensure_dir_exists(dir_path)) {
        g_free(dir_path);
        g_free(file_path);
        return FALSE;
    }

    GError *error = NULL;
    gsize data_size;
    guint8 *data = NULL;

    if (!fp_print_serialize(print, &data, &data_size, &error)) {
        g_warning("Failed to serialize print: %s", error->message);
        g_error_free(error);
        g_free(dir_path);
        g_free(file_path);
        return FALSE;
    }

    FILE *f = fopen(file_path, "wb");
    if (!f) {
        g_warning("Failed to open %s: %s", file_path, strerror(errno));
        g_free(data);
        g_free(dir_path);
        g_free(file_path);
        return FALSE;
    }

    size_t written = fwrite(data, 1, data_size, f);
    fclose(f);
    g_free(data);

    if (written != data_size) {
        g_warning("Failed to write all data to %s", file_path);
        g_free(dir_path);
        g_free(file_path);
        return FALSE;
    }

    g_message("Saved fingerprint for user %s, finger %s", username, finger);

    g_free(dir_path);
    g_free(file_path);
    return TRUE;
}

/* Load print from storage */
static FpPrint* load_print(FpDevice *dev, const char *username, const char *finger) {
    char *file_path = get_storage_path(username, finger);

    FILE *f = fopen(file_path, "rb");
    if (!f) {
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
    FpPrint *print = fp_print_deserialize(dev, data, data_size, &error);
    g_free(data);
    g_free(file_path);

    if (error) {
        g_warning("Failed to deserialize print: %s", error->message);
        g_error_free(error);
        return NULL;
    }

    return print;
}

/* Enroll callback */
static void enroll_progress_cb(FpDevice *dev, FpPrint *print, GObject *enroll_stage, gpointer user_data) {
    FprintDevice *fdev = (FprintDevice *)user_data;
    GVariantBuilder builder;

    g_variant_builder_init(&builder, G_VARIANT_TYPE_TUPLE);

    switch (fp_print_get_enroll_result(print)) {
        case FP_ENROLL_COMPLETE:
            g_message("Enroll complete");

            /* Save the print */
            if (save_print(print, fdev->current_user, fdev->current_finger)) {
                g_dbus_connection_emit_signal(fdev->connection,
                                              NULL,
                                              DEVICE_PATH,
                                              DEVICE_INTERFACE,
                                              "EnrollStatus",
                                              g_variant_new("(sb)", "enroll-completed", TRUE),
                                              NULL);
            } else {
                g_dbus_connection_emit_signal(fdev->connection,
                                              NULL,
                                              DEVICE_PATH,
                                              DEVICE_INTERFACE,
                                              "EnrollStatus",
                                              g_variant_new("(sb)", "enroll-failed", TRUE),
                                              NULL);
            }

            fdev->is_enrolling = FALSE;
            g_clear_object(&fdev->cancellable);
            g_free(fdev->current_finger);
            fdev->current_finger = NULL;
            break;

        case FP_ENROLL_FAILED:
            g_warning("Enroll failed");
            g_dbus_connection_emit_signal(fdev->connection,
                                          NULL,
                                          DEVICE_PATH,
                                          DEVICE_INTERFACE,
                                          "EnrollStatus",
                                          g_variant_new("(sb)", "enroll-failed", TRUE),
                                          NULL);
            fdev->is_enrolling = FALSE;
            g_clear_object(&fdev->cancellable);
            g_free(fdev->current_finger);
            fdev->current_finger = NULL;
            break;

        case FP_ENROLL_RETRY:
            g_message("Enroll retry");
            g_dbus_connection_emit_signal(fdev->connection,
                                          NULL,
                                          DEVICE_PATH,
                                          DEVICE_INTERFACE,
                                          "EnrollStatus",
                                          g_variant_new("(sb)", "enroll-retry", FALSE),
                                          NULL);
            break;

        case FP_ENROLL_RETRY_TOO_SHORT:
            g_message("Swipe too short");
            g_dbus_connection_emit_signal(fdev->connection,
                                          NULL,
                                          DEVICE_PATH,
                                          DEVICE_INTERFACE,
                                          "EnrollStatus",
                                          g_variant_new("(sb)", "enroll-swipe-too-short", FALSE),
                                          NULL);
            break;

        case FP_ENROLL_RETRY_CENTER_FINGER:
            g_message("Center finger");
            g_dbus_connection_emit_signal(fdev->connection,
                                          NULL,
                                          DEVICE_PATH,
                                          DEVICE_INTERFACE,
                                          "EnrollStatus",
                                          g_variant_new("(sb)", "enroll-center-finger", FALSE),
                                          NULL);
            break;

        case FP_ENROLL_RETRY_REMOVE_FINGER:
            g_message("Remove finger");
            g_dbus_connection_emit_signal(fdev->connection,
                                          NULL,
                                          DEVICE_PATH,
                                          DEVICE_INTERFACE,
                                          "EnrollStatus",
                                          g_variant_new("(sb)", "enroll-remove-finger", FALSE),
                                          NULL);
            break;

        default:
            break;
    }
}

/* Verify callback */
static void verify_progress_cb(FpDevice *dev, FpPrint *print, GObject *verify_stage, gpointer user_data) {
    FprintDevice *fdev = (FprintDevice *)user_data;

    switch (fp_print_get_verify_result(print)) {
        case FP_VERIFY_COMPLETE:
            g_message("Verify complete - match");
            g_dbus_connection_emit_signal(fdev->connection,
                                          NULL,
                                          DEVICE_PATH,
                                          DEVICE_INTERFACE,
                                          "VerifyStatus",
                                          g_variant_new("(sb)", "verify-match", TRUE),
                                          NULL);
            fdev->is_verifying = FALSE;
            g_clear_object(&fdev->cancellable);
            break;

        case FP_VERIFY_NO_MATCH:
            g_message("Verify complete - no match");
            g_dbus_connection_emit_signal(fdev->connection,
                                          NULL,
                                          DEVICE_PATH,
                                          DEVICE_INTERFACE,
                                          "VerifyStatus",
                                          g_variant_new("(sb)", "verify-no-match", TRUE),
                                          NULL);
            fdev->is_verifying = FALSE;
            g_clear_object(&fdev->cancellable);
            break;

        case FP_VERIFY_RETRY:
            g_message("Verify retry");
            g_dbus_connection_emit_signal(fdev->connection,
                                          NULL,
                                          DEVICE_PATH,
                                          DEVICE_INTERFACE,
                                          "VerifyStatus",
                                          g_variant_new("(sb)", "verify-retry", FALSE),
                                          NULL);
            break;

        case FP_VERIFY_RETRY_TOO_SHORT:
            g_message("Swipe too short");
            g_dbus_connection_emit_signal(fdev->connection,
                                          NULL,
                                          DEVICE_PATH,
                                          DEVICE_INTERFACE,
                                          "VerifyStatus",
                                          g_variant_new("(sb)", "verify-swipe-too-short", FALSE),
                                          NULL);
            break;

        case FP_VERIFY_RETRY_CENTER_FINGER:
            g_message("Center finger");
            g_dbus_connection_emit_signal(fdev->connection,
                                          NULL,
                                          DEVICE_PATH,
                                          DEVICE_INTERFACE,
                                          "VerifyStatus",
                                          g_variant_new("(sb)", "verify-center-finger", FALSE),
                                          NULL);
            break;

        case FP_VERIFY_RETRY_REMOVE_FINGER:
            g_message("Remove finger");
            g_dbus_connection_emit_signal(fdev->connection,
                                          NULL,
                                          DEVICE_PATH,
                                          DEVICE_INTERFACE,
                                          "VerifyStatus",
                                          g_variant_new("(sb)", "verify-remove-finger", FALSE),
                                          NULL);
            break;

        default:
            break;
    }
}

/* Device open callback */
static void device_open_cb(FpDevice *dev, GAsyncResult *res, gpointer user_data) {
    GError *error = NULL;

    if (!fp_device_open_finish(dev, res, &error)) {
        g_warning("Failed to open device: %s", error->message);
        g_error_free(error);
        return;
    }

    g_message("Device opened successfully");
}

/* Method handlers */
static void handle_get_default_device(FprintDevice *fdev, GDBusMethodInvocation *invocation) {
    g_dbus_method_invocation_return_value(invocation,
                                          g_variant_new("(o)", DEVICE_PATH));
}

static void handle_get_devices(FprintDevice *fdev, GDBusMethodInvocation *invocation) {
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("ao"));
    g_variant_builder_add(&builder, "o", DEVICE_PATH);
    g_dbus_method_invocation_return_value(invocation,
                                          g_variant_new("(ao)", &builder));
}

static void handle_claim(FprintDevice *fdev, GDBusMethodInvocation *invocation,
                         const char *username) {
    if (!username || strlen(username) == 0) {
        g_dbus_method_invocation_return_error(invocation,
                                              G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
                                              "Username cannot be empty");
        return;
    }

    if (fdev->current_user) {
        g_free(fdev->current_user);
    }
    fdev->current_user = g_strdup(username);

    g_message("Device claimed by user: %s", username);
    g_dbus_method_invocation_return_value(invocation, NULL);
                         }

                         static void handle_release(FprintDevice *fdev, GDBusMethodInvocation *invocation) {
                             if (fdev->is_enrolling || fdev->is_verifying) {
                                 if (fdev->cancellable) {
                                     g_cancellable_cancel(fdev->cancellable);
                                     g_clear_object(&fdev->cancellable);
                                 }
                                 fdev->is_enrolling = FALSE;
                                 fdev->is_verifying = FALSE;
                             }

                             if (fdev->current_user) {
                                 g_free(fdev->current_user);
                                 fdev->current_user = NULL;
                             }

                             if (fdev->current_finger) {
                                 g_free(fdev->current_finger);
                                 fdev->current_finger = NULL;
                             }

                             g_message("Device released");
                             g_dbus_method_invocation_return_value(invocation, NULL);
                         }

                         static void handle_list_enrolled_fingers(FprintDevice *fdev, GDBusMethodInvocation *invocation) {
                             GVariantBuilder builder;
                             g_variant_builder_init(&builder, G_VARIANT_TYPE("as"));

                             if (fdev->current_user) {
                                 char *dir_path = get_storage_path(fdev->current_user, NULL);
                                 GDir *dir = g_dir_open(dir_path, 0, NULL);

                                 if (dir) {
                                     const char *name;
                                     while ((name = g_dir_read_name(dir)) != NULL) {
                                         char *full_path = g_build_filename(dir_path, name, NULL);
                                         if (g_file_test(full_path, G_FILE_TEST_IS_REGULAR)) {
                                             g_variant_builder_add(&builder, "s", name);
                                         }
                                         g_free(full_path);
                                     }
                                     g_dir_close(dir);
                                 }

                                 g_free(dir_path);
                             }

                             g_dbus_method_invocation_return_value(invocation,
                                                                   g_variant_new("(as)", &builder));
                         }

                         static void handle_delete_enrolled_finger(FprintDevice *fdev, GDBusMethodInvocation *invocation,
                                                                   const char *finger) {
                             if (!fdev->current_user) {
                                 g_dbus_method_invocation_return_error(invocation,
                                                                       G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                                       "No user claimed");
                                 return;
                             }

                             char *file_path = get_storage_path(fdev->current_user, finger);

                             if (unlink(file_path) == 0) {
                                 g_message("Deleted finger %s for user %s", finger, fdev->current_user);
                                 g_dbus_method_invocation_return_value(invocation, NULL);
                             } else {
                                 g_dbus_method_invocation_return_error(invocation,
                                                                       G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                                       "Failed to delete finger: %s", strerror(errno));
                             }

                             g_free(file_path);
                                                                   }

                                                                   static void handle_delete_enrolled_fingers(FprintDevice *fdev, GDBusMethodInvocation *invocation) {
                                                                       if (!fdev->current_user) {
                                                                           g_dbus_method_invocation_return_error(invocation,
                                                                                                                 G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                                                                                 "No user claimed");
                                                                           return;
                                                                       }

                                                                       char *dir_path = get_storage_path(fdev->current_user, NULL);
                                                                       GDir *dir = g_dir_open(dir_path, 0, NULL);

                                                                       if (dir) {
                                                                           const char *name;
                                                                           while ((name = g_dir_read_name(dir)) != NULL) {
                                                                               char *full_path = g_build_filename(dir_path, name, NULL);
                                                                               unlink(full_path);
                                                                               g_free(full_path);
                                                                           }
                                                                           g_dir_close(dir);
                                                                       }

                                                                       g_free(dir_path);

                                                                       g_message("Deleted all fingers for user %s", fdev->current_user);
                                                                       g_dbus_method_invocation_return_value(invocation, NULL);
                                                                   }

                                                                   static void handle_get_capabilities(FprintDevice *fdev, GDBusMethodInvocation *invocation) {
                                                                       guint caps = 0;
                                                                       g_dbus_method_invocation_return_value(invocation,
                                                                                                             g_variant_new("(u)", caps));
                                                                   }

                                                                   static void handle_enroll_start(FprintDevice *fdev, GDBusMethodInvocation *invocation,
                                                                                                   const char *finger) {
                                                                       if (!fdev->current_user) {
                                                                           g_dbus_method_invocation_return_error(invocation,
                                                                                                                 G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                                                                                 "No user claimed");
                                                                           return;
                                                                       }

                                                                       if (fdev->is_enrolling || fdev->is_verifying) {
                                                                           g_dbus_method_invocation_return_error(invocation,
                                                                                                                 G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                                                                                 "Device busy");
                                                                           return;
                                                                       }

                                                                       if (!fdev->dev) {
                                                                           g_dbus_method_invocation_return_error(invocation,
                                                                                                                 G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                                                                                 "No device available");
                                                                           return;
                                                                       }

                                                                       char *file_path = get_storage_path(fdev->current_user, finger);
                                                                       if (g_file_test(file_path, G_FILE_TEST_EXISTS)) {
                                                                           g_free(file_path);
                                                                           g_dbus_method_invocation_return_error(invocation,
                                                                                                                 G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                                                                                 "Finger already enrolled");
                                                                           return;
                                                                       }
                                                                       g_free(file_path);

                                                                       fdev->current_finger = g_strdup(finger);
                                                                       fdev->is_enrolling = TRUE;
                                                                       fdev->cancellable = g_cancellable_new();

                                                                       g_message("Starting enrollment for finger: %s", finger);
                                                                       g_dbus_method_invocation_return_value(invocation, NULL);

                                                                       fp_device_enroll(fdev->dev,
                                                                                        NULL,  /* No template */
                                                                                        fdev->cancellable,
                                                                                        enroll_progress_cb,
                                                                                        fdev,
                                                                                        NULL,
                                                                                        NULL);
                                                                                                   }

                                                                                                   static void handle_enroll_stop(FprintDevice *fdev, GDBusMethodInvocation *invocation) {
                                                                                                       if (fdev->is_enrolling && fdev->cancellable) {
                                                                                                           g_cancellable_cancel(fdev->cancellable);
                                                                                                           g_clear_object(&fdev->cancellable);
                                                                                                           fdev->is_enrolling = FALSE;
                                                                                                           g_free(fdev->current_finger);
                                                                                                           fdev->current_finger = NULL;
                                                                                                           g_message("Enrollment stopped");
                                                                                                       }

                                                                                                       g_dbus_method_invocation_return_value(invocation, NULL);
                                                                                                   }

                                                                                                   static void handle_verify_start(FprintDevice *fdev, GDBusMethodInvocation *invocation,
                                                                                                                                   const char *finger) {
                                                                                                       if (!fdev->current_user) {
                                                                                                           g_dbus_method_invocation_return_error(invocation,
                                                                                                                                                 G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                                                                                                                 "No user claimed");
                                                                                                           return;
                                                                                                       }

                                                                                                       if (fdev->is_enrolling || fdev->is_verifying) {
                                                                                                           g_dbus_method_invocation_return_error(invocation,
                                                                                                                                                 G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                                                                                                                 "Device busy");
                                                                                                           return;
                                                                                                       }

                                                                                                       if (!fdev->dev) {
                                                                                                           g_dbus_method_invocation_return_error(invocation,
                                                                                                                                                 G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                                                                                                                 "No device available");
                                                                                                           return;
                                                                                                       }

                                                                                                       FpPrint *print = load_print(fdev->dev, fdev->current_user, finger);
                                                                                                       if (!print) {
                                                                                                           g_dbus_method_invocation_return_error(invocation,
                                                                                                                                                 G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                                                                                                                 "No enrolled finger found");
                                                                                                           return;
                                                                                                       }

                                                                                                       fdev->is_verifying = TRUE;
                                                                                                       fdev->cancellable = g_cancellable_new();

                                                                                                       g_message("Starting verification for finger: %s", finger);
                                                                                                       g_dbus_method_invocation_return_value(invocation, NULL);

                                                                                                       fp_device_verify(fdev->dev,
                                                                                                                        print,
                                                                                                                        fdev->cancellable,
                                                                                                                        verify_progress_cb,
                                                                                                                        fdev,
                                                                                                                        NULL,
                                                                                                                        NULL);

                                                                                                       g_object_unref(print);
                                                                                                                                   }

                                                                                                                                   static void handle_verify_stop(FprintDevice *fdev, GDBusMethodInvocation *invocation) {
                                                                                                                                       if (fdev->is_verifying && fdev->cancellable) {
                                                                                                                                           g_cancellable_cancel(fdev->cancellable);
                                                                                                                                           g_clear_object(&fdev->cancellable);
                                                                                                                                           fdev->is_verifying = FALSE;
                                                                                                                                           g_message("Verification stopped");
                                                                                                                                       }

                                                                                                                                       g_dbus_method_invocation_return_value(invocation, NULL);
                                                                                                                                   }

                                                                                                                                   static GVariant* handle_get_claimed(GDBusConnection *connection,
                                                                                                                                                                       const gchar *sender,
                                                                                                                                                                       const gchar *object_path,
                                                                                                                                                                       const gchar *interface_name,
                                                                                                                                                                       const gchar *property_name,
                                                                                                                                                                       GError **error,
                                                                                                                                                                       gpointer user_data) {
                                                                                                                                       FprintDevice *fdev = (FprintDevice *)user_data;
                                                                                                                                       return g_variant_new_boolean(fdev->current_user != NULL);
                                                                                                                                                                       }

                                                                                                                                                                       static GVariant* handle_get_scan_type(GDBusConnection *connection,
                                                                                                                                                                                                             const gchar *sender,
                                                                                                                                                                                                             const gchar *object_path,
                                                                                                                                                                                                             const gchar *interface_name,
                                                                                                                                                                                                             const gchar *property_name,
                                                                                                                                                                                                             GError **error,
                                                                                                                                                                                                             gpointer user_data) {
                                                                                                                                                                           return g_variant_new_string("press");
                                                                                                                                                                                                             }

                                                                                                                                                                                                             static GVariant* handle_get_num_enroll_stages(GDBusConnection *connection,
                                                                                                                                                                                                                                                           const gchar *sender,
                                                                                                                                                                                                                                                           const gchar *object_path,
                                                                                                                                                                                                                                                           const gchar *interface_name,
                                                                                                                                                                                                                                                           const gchar *property_name,
                                                                                                                                                                                                                                                           GError **error,
                                                                                                                                                                                                                                                           gpointer user_data) {
                                                                                                                                                                                                                 FprintDevice *fdev = (FprintDevice *)user_data;
                                                                                                                                                                                                                 if (fdev->dev) {
                                                                                                                                                                                                                     return g_variant_new_uint32(fp_device_get_nr_enroll_stages(fdev->dev));
                                                                                                                                                                                                                 }
                                                                                                                                                                                                                 return g_variant_new_uint32(1);
                                                                                                                                                                                                                                                           }

                                                                                                                                                                                                                                                           /* D-Bus interface vtables */
                                                                                                                                                                                                                                                           static const GDBusInterfaceVTable manager_vtable = {
                                                                                                                                                                                                                                                               .method_call = [](GDBusConnection *connection,
                                                                                                                                                                                                                                                                const gchar *sender,
                                                                                                                                                                                                                                                                const gchar *object_path,
                                                                                                                                                                                                                                                                const gchar *interface_name,
                                                                                                                                                                                                                                                                const gchar *method_name,
                                                                                                                                                                                                                                                                GVariant *parameters,
                                                                                                                                                                                                                                                                GDBusMethodInvocation *invocation,
                                                                                                                                                                                                                                                                gpointer user_data) {
                                                                                                                                                                                                                                                                FprintDevice *fdev = (FprintDevice *)user_data;

                                                                                                                                                                                                                                                                if (g_strcmp0(method_name, "GetDefaultDevice") == 0) {
                                                                                                                                                                                                                                                                handle_get_default_device(fdev, invocation);
                                                                                                                                                                                                                                                                } else if (g_strcmp0(method_name, "GetDevices") == 0) {
                                                                                                                                                                                                                                                                handle_get_devices(fdev, invocation);
                                                                                                                                                                                                                                                                } else {
                                                                                                                                                                                                                                                                g_dbus_method_invocation_return_error(invocation,
                                                                                                                                                                                                                                                                G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
                                                                                                                                                                                                                                                                "Unknown method: %s", method_name);
                                                                                                                                                                                                                                                                }
                                                                                                                                                                                                                                                                },
                                                                                                                                                                                                                                                                .get_property = NULL,
                                                                                                                                                                                                                                                                .set_property = NULL
                                                                                                                                                                                                                                                           };

                                                                                                                                                                                                                                                           static const GDBusInterfaceVTable device_vtable = {
                                                                                                                                                                                                                                                               .method_call = [](GDBusConnection *connection,
                                                                                                                                                                                                                                                                const gchar *sender,
                                                                                                                                                                                                                                                                const gchar *object_path,
                                                                                                                                                                                                                                                                const gchar *interface_name,
                                                                                                                                                                                                                                                                const gchar *method_name,
                                                                                                                                                                                                                                                                GVariant *parameters,
                                                                                                                                                                                                                                                                GDBusMethodInvocation *invocation,
                                                                                                                                                                                                                                                                gpointer user_data) {
                                                                                                                                                                                                                                                                FprintDevice *fdev = (FprintDevice *)user_data;

                                                                                                                                                                                                                                                                if (g_strcmp0(method_name, "Claim") == 0) {
                                                                                                                                                                                                                                                                const char *username;
                                                                                                                                                                                                                                                                g_variant_get(parameters, "(&s)", &username);
                                                                                                                                                                                                                                                                handle_claim(fdev, invocation, username);
                                                                                                                                                                                                                                                                } else if (g_strcmp0(method_name, "Release") == 0) {
                                                                                                                                                                                                                                                                handle_release(fdev, invocation);
                                                                                                                                                                                                                                                                } else if (g_strcmp0(method_name, "ListEnrolledFingers") == 0) {
                                                                                                                                                                                                                                                                handle_list_enrolled_fingers(fdev, invocation);
                                                                                                                                                                                                                                                                } else if (g_strcmp0(method_name, "DeleteEnrolledFinger") == 0) {
                                                                                                                                                                                                                                                                const char *finger;
                                                                                                                                                                                                                                                                g_variant_get(parameters, "(&s)", &finger);
                                                                                                                                                                                                                                                                handle_delete_enrolled_finger(fdev, invocation, finger);
                                                                                                                                                                                                                                                                } else if (g_strcmp0(method_name, "DeleteEnrolledFingers") == 0) {
                                                                                                                                                                                                                                                                handle_delete_enrolled_fingers(fdev, invocation);
                                                                                                                                                                                                                                                                } else if (g_strcmp0(method_name, "GetCapabilities") == 0) {
                                                                                                                                                                                                                                                                handle_get_capabilities(fdev, invocation);
                                                                                                                                                                                                                                                                } else if (g_strcmp0(method_name, "EnrollStart") == 0) {
                                                                                                                                                                                                                                                                const char *finger;
                                                                                                                                                                                                                                                                g_variant_get(parameters, "(&s)", &finger);
                                                                                                                                                                                                                                                                handle_enroll_start(fdev, invocation, finger);
                                                                                                                                                                                                                                                                } else if (g_strcmp0(method_name, "EnrollStop") == 0) {
                                                                                                                                                                                                                                                                handle_enroll_stop(fdev, invocation);
                                                                                                                                                                                                                                                                } else if (g_strcmp0(method_name, "VerifyStart") == 0) {
                                                                                                                                                                                                                                                                const char *finger;
                                                                                                                                                                                                                                                                g_variant_get(parameters, "(&s)", &finger);
                                                                                                                                                                                                                                                                handle_verify_start(fdev, invocation, finger);
                                                                                                                                                                                                                                                                } else if (g_strcmp0(method_name, "VerifyStop") == 0) {
                                                                                                                                                                                                                                                                handle_verify_stop(fdev, invocation);
                                                                                                                                                                                                                                                                } else {
                                                                                                                                                                                                                                                                g_dbus_method_invocation_return_error(invocation,
                                                                                                                                                                                                                                                                G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
                                                                                                                                                                                                                                                                "Unknown method: %s", method_name);
                                                                                                                                                                                                                                                                }
                                                                                                                                                                                                                                                                },
                                                                                                                                                                                                                                                                .get_property = [](GDBusConnection *connection,
                                                                                                                                                                                                                                                                const gchar *sender,
                                                                                                                                                                                                                                                                const gchar *object_path,
                                                                                                                                                                                                                                                                const gchar *interface_name,
                                                                                                                                                                                                                                                                const gchar *property_name,
                                                                                                                                                                                                                                                                GError **error,
                                                                                                                                                                                                                                                                gpointer user_data) -> GVariant* {
                                                                                                                                                                                                                                                                FprintDevice *fdev = (FprintDevice *)user_data;

                                                                                                                                                                                                                                                                if (g_strcmp0(property_name, "Claimed") == 0) {
                                                                                                                                                                                                                                                                return handle_get_claimed(connection, sender, object_path,
                                                                                                                                                                                                                                                                interface_name, property_name,
                                                                                                                                                                                                                                                                error, user_data);
                                                                                                                                                                                                                                                                } else if (g_strcmp0(property_name, "ScanType") == 0) {
                                                                                                                                                                                                                                                                return handle_get_scan_type(connection, sender, object_path,
                                                                                                                                                                                                                                                                interface_name, property_name,
                                                                                                                                                                                                                                                                error, user_data);
                                                                                                                                                                                                                                                                } else if (g_strcmp0(property_name, "NumEnrollStages") == 0) {
                                                                                                                                                                                                                                                                return handle_get_num_enroll_stages(connection, sender, object_path,
                                                                                                                                                                                                                                                                interface_name, property_name,
                                                                                                                                                                                                                                                                error, user_data);
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                                                                                                                                                                                                                                                "Unknown property: %s", property_name);
                                                                                                                                                                                                                                                                return NULL;
                                                                                                                                                                                                                                                                },
                                                                                                                                                                                                                                                                .set_property = NULL
                                                                                                                                                                                                                                                           };

                                                                                                                                                                                                                                                           /* Bus acquired callback */
                                                                                                                                                                                                                                                           static void on_bus_acquired(GDBusConnection *connection,
                                                                                                                                                                                                                                                                const gchar *name,
                                                                                                                                                                                                                                                                gpointer user_data) {
                                                                                                                                                                                                                                                               FprintDevice *fdev = (FprintDevice *)user_data;
                                                                                                                                                                                                                                                               GError *error = NULL;

                                                                                                                                                                                                                                                               fdev->connection = g_object_ref(connection);
                                                                                                                                                                                                                                                               g_message("Bus acquired: %s", name);

                                                                                                                                                                                                                                                               /* Register manager */
                                                                                                                                                                                                                                                               fdev->registration_id = g_dbus_connection_register_object(connection,
                                                                                                                                                                                                                                                                MANAGER_PATH,
                                                                                                                                                                                                                                                                fdev->introspection->interfaces[0],
                                                                                                                                                                                                                                                                &manager_vtable,
                                                                                                                                                                                                                                                                fdev,
                                                                                                                                                                                                                                                                NULL,
                                                                                                                                                                                                                                                                &error);

                                                                                                                                                                                                                                                               if (fdev->registration_id == 0) {
                                                                                                                                                                                                                                                                g_error("Failed to register manager: %s", error->message);
                                                                                                                                                                                                                                                                g_error_free(error);
                                                                                                                                                                                                                                                                return;
                                                                                                                                                                                                                                                               }

                                                                                                                                                                                                                                                               /* Register device */
                                                                                                                                                                                                                                                               fdev->registration_id = g_dbus_connection_register_object(connection,
                                                                                                                                                                                                                                                                DEVICE_PATH,
                                                                                                                                                                                                                                                                fdev->introspection->interfaces[1],
                                                                                                                                                                                                                                                                &device_vtable,
                                                                                                                                                                                                                                                                fdev,
                                                                                                                                                                                                                                                                NULL,
                                                                                                                                                                                                                                                                &error);

                                                                                                                                                                                                                                                               if (fdev->registration_id == 0) {
                                                                                                                                                                                                                                                                g_error("Failed to register device: %s", error->message);
                                                                                                                                                                                                                                                                g_error_free(error);
                                                                                                                                                                                                                                                                return;
                                                                                                                                                                                                                                                               }

                                                                                                                                                                                                                                                               g_message("D-Bus objects registered");
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                /* Name acquired callback */
                                                                                                                                                                                                                                                                static void on_name_acquired(GDBusConnection *connection,
                                                                                                                                                                                                                                                                const gchar *name,
                                                                                                                                                                                                                                                                gpointer user_data) {
                                                                                                                                                                                                                                                                g_message("Name acquired: %s", name);
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                /* Name lost callback */
                                                                                                                                                                                                                                                                static void on_name_lost(GDBusConnection *connection,
                                                                                                                                                                                                                                                                const gchar *name,
                                                                                                                                                                                                                                                                gpointer user_data) {
                                                                                                                                                                                                                                                                g_message("Name lost: %s", name);
                                                                                                                                                                                                                                                                g_main_loop_quit((GMainLoop *)user_data);
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                /* Signal handler for graceful shutdown */
                                                                                                                                                                                                                                                                static void signal_handler(int signum) {
                                                                                                                                                                                                                                                                g_message("Received signal %d, shutting down...", signum);
                                                                                                                                                                                                                                                                if (main_loop) {
                                                                                                                                                                                                                                                                g_main_loop_quit(main_loop);
                                                                                                                                                                                                                                                                }
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                int main(int argc, char *argv[]) {
                                                                                                                                                                                                                                                                FprintDevice fdev = {0};
                                                                                                                                                                                                                                                                GError *error = NULL;

                                                                                                                                                                                                                                                                /* Set up signal handling */
                                                                                                                                                                                                                                                                signal(SIGINT, signal_handler);
                                                                                                                                                                                                                                                                signal(SIGTERM, signal_handler);

                                                                                                                                                                                                                                                                /* Set default user if provided */
                                                                                                                                                                                                                                                                char *default_user = NULL;
                                                                                                                                                                                                                                                                if (argc > 1) {
                                                                                                                                                                                                                                                                default_user = argv[1];
                                                                                                                                                                                                                                                                g_message("Default user: %s", default_user);
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                /* Initialize libfprint */
                                                                                                                                                                                                                                                                fdev.ctx = fp_context_new();
                                                                                                                                                                                                                                                                if (!fdev.ctx) {
                                                                                                                                                                                                                                                                g_error("Failed to create libfprint context");
                                                                                                                                                                                                                                                                return 1;
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                /* Get devices */
                                                                                                                                                                                                                                                                GPtrArray *devices = fp_context_get_devices(fdev.ctx);
                                                                                                                                                                                                                                                                if (!devices || devices->len == 0) {
                                                                                                                                                                                                                                                                g_error("No fingerprint devices found");
                                                                                                                                                                                                                                                                return 1;
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                /* Find Synaptics device */
                                                                                                                                                                                                                                                                for (size_t i = 0; i < devices->len; i++) {
                                                                                                                                                                                                                                                                FpDevice *dev = g_ptr_array_index(devices, i);
                                                                                                                                                                                                                                                                const char *driver = fp_device_get_driver(dev);
                                                                                                                                                                                                                                                                const char *name = fp_device_get_name(dev);

                                                                                                                                                                                                                                                                if (strstr(driver, "synaptics") || strstr(name, "Synaptics")) {
                                                                                                                                                                                                                                                                fdev.dev = g_object_ref(dev);
                                                                                                                                                                                                                                                                g_message("Found Synaptics device: %s (%s)", name, driver);
                                                                                                                                                                                                                                                                break;
                                                                                                                                                                                                                                                                }
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                g_ptr_array_unref(devices);

                                                                                                                                                                                                                                                                if (!fdev.dev) {
                                                                                                                                                                                                                                                                g_error("No Synaptics device found");
                                                                                                                                                                                                                                                                return 1;
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                /* Open device asynchronously */
                                                                                                                                                                                                                                                                fp_device_open(fdev.dev, NULL, device_open_cb, &fdev);

                                                                                                                                                                                                                                                                /* Parse introspection XML */
                                                                                                                                                                                                                                                                const gchar *xml =
                                                                                                                                                                                                                                                                "<node>"
                                                                                                                                                                                                                                                                "  <interface name='net.reactivated.Fprint.Manager'>"
                                                                                                                                                                                                                                                                "    <method name='GetDefaultDevice'>"
                                                                                                                                                                                                                                                                "      <arg type='o' name='device' direction='out'/>"
                                                                                                                                                                                                                                                                "    </method>"
                                                                                                                                                                                                                                                                "    <method name='GetDevices'>"
                                                                                                                                                                                                                                                                "      <arg type='ao' name='devices' direction='out'/>"
                                                                                                                                                                                                                                                                "    </method>"
                                                                                                                                                                                                                                                                "  </interface>"
                                                                                                                                                                                                                                                                "  <interface name='net.reactivated.Fprint.Device'>"
                                                                                                                                                                                                                                                                "    <method name='Claim'>"
                                                                                                                                                                                                                                                                "      <arg type='s' name='username' direction='in'/>"
                                                                                                                                                                                                                                                                "    </method>"
                                                                                                                                                                                                                                                                "    <method name='Release'/>"
                                                                                                                                                                                                                                                                "    <method name='ListEnrolledFingers'>"
                                                                                                                                                                                                                                                                "      <arg type='as' name='fingers' direction='out'/>"
                                                                                                                                                                                                                                                                "    </method>"
                                                                                                                                                                                                                                                                "    <method name='DeleteEnrolledFinger'>"
                                                                                                                                                                                                                                                                "      <arg type='s' name='finger' direction='in'/>"
                                                                                                                                                                                                                                                                "    </method>"
                                                                                                                                                                                                                                                                "    <method name='DeleteEnrolledFingers'/>"
                                                                                                                                                                                                                                                                "    <method name='GetCapabilities'>"
                                                                                                                                                                                                                                                                "      <arg type='u' name='caps' direction='out'/>"
                                                                                                                                                                                                                                                                "    </method>"
                                                                                                                                                                                                                                                                "    <method name='EnrollStart'>"
                                                                                                                                                                                                                                                                "      <arg type='s' name='finger' direction='in'/>"
                                                                                                                                                                                                                                                                "    </method>"
                                                                                                                                                                                                                                                                "    <method name='EnrollStop'/>"
                                                                                                                                                                                                                                                                "    <method name='VerifyStart'>"
                                                                                                                                                                                                                                                                "      <arg type='s' name='finger' direction='in'/>"
                                                                                                                                                                                                                                                                "    </method>"
                                                                                                                                                                                                                                                                "    <method name='VerifyStop'/>"
                                                                                                                                                                                                                                                                "    <signal name='EnrollStatus'>"
                                                                                                                                                                                                                                                                "      <arg type='s' name='result'/>"
                                                                                                                                                                                                                                                                "      <arg type='b' name='done'/>"
                                                                                                                                                                                                                                                                "    </signal>"
                                                                                                                                                                                                                                                                "    <signal name='VerifyStatus'>"
                                                                                                                                                                                                                                                                "      <arg type='s' name='result'/>"
                                                                                                                                                                                                                                                                "      <arg type='b' name='done'/>"
                                                                                                                                                                                                                                                                "    </signal>"
                                                                                                                                                                                                                                                                "    <property name='Claimed' type='b' access='read'/>"
                                                                                                                                                                                                                                                                "    <property name='ScanType' type='s' access='read'/>"
                                                                                                                                                                                                                                                                "    <property name='NumEnrollStages' type='u' access='read'/>"
                                                                                                                                                                                                                                                                "  </interface>"
                                                                                                                                                                                                                                                                "</node>";

                                                                                                                                                                                                                                                                fdev.introspection = g_dbus_node_info_new_for_xml(xml, &error);
                                                                                                                                                                                                                                                                if (error) {
                                                                                                                                                                                                                                                                g_error("Failed to parse introspection XML: %s", error->message);
                                                                                                                                                                                                                                                                return 1;
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                if (default_user) {
                                                                                                                                                                                                                                                                fdev.current_user = g_strdup(default_user);
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                /* Create main loop */
                                                                                                                                                                                                                                                                main_loop = g_main_loop_new(NULL, FALSE);
                                                                                                                                                                                                                                                                g_message("Starting main loop...");

                                                                                                                                                                                                                                                                /* Own bus name */
                                                                                                                                                                                                                                                                fdev.owner_id = g_bus_own_name(G_BUS_TYPE_SYSTEM,
                                                                                                                                                                                                                                                                SERVICE_NAME,
                                                                                                                                                                                                                                                                G_BUS_NAME_OWNER_FLAGS_NONE,
                                                                                                                                                                                                                                                                on_bus_acquired,
                                                                                                                                                                                                                                                                on_name_acquired,
                                                                                                                                                                                                                                                                on_name_lost,
                                                                                                                                                                                                                                                                main_loop,
                                                                                                                                                                                                                                                                NULL);

                                                                                                                                                                                                                                                                /* Run main loop */
                                                                                                                                                                                                                                                                g_main_loop_run(main_loop);

                                                                                                                                                                                                                                                                /* Cleanup */
                                                                                                                                                                                                                                                                g_message("Cleaning up...");

                                                                                                                                                                                                                                                                if (fdev.is_enrolling || fdev.is_verifying) {
                                                                                                                                                                                                                                                                if (fdev.cancellable) {
                                                                                                                                                                                                                                                                g_cancellable_cancel(fdev.cancellable);
                                                                                                                                                                                                                                                                g_clear_object(&fdev.cancellable);
                                                                                                                                                                                                                                                                }
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                if (fdev.dev) {
                                                                                                                                                                                                                                                                fp_device_close_sync(fdev.dev, NULL, NULL);
                                                                                                                                                                                                                                                                g_object_unref(fdev.dev);
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                if (fdev.ctx) {
                                                                                                                                                                                                                                                                g_object_unref(fdev.ctx);
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                if (fdev.owner_id > 0) {
                                                                                                                                                                                                                                                                g_bus_unown_name(fdev.owner_id);
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                if (fdev.connection) {
                                                                                                                                                                                                                                                                g_object_unref(fdev.connection);
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                if (fdev.introspection) {
                                                                                                                                                                                                                                                                g_dbus_node_info_unref(fdev.introspection);
                                                                                                                                                                                                                                                                }

                                                                                                                                                                                                                                                                g_free(fdev.current_user);
                                                                                                                                                                                                                                                                g_free(fdev.current_finger);

                                                                                                                                                                                                                                                                g_main_loop_unref(main_loop);

                                                                                                                                                                                                                                                                g_message("Shutdown complete");
                                                                                                                                                                                                                                                                return 0;
                                                                                                                                                                                                                                                                }
