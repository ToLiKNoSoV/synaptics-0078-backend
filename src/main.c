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
#include <glib.h>
#include <gio/gio.h>
#include <libfprint-2/fprint.h>

#include "device.h"
#include "storage.h"

#define SERVICE_NAME "net.reactivated.Fprint"
#define OBJECT_PATH "/net/reactivated/Fprint/Device/0"
#define MANAGER_PATH "/net/reactivated/Fprint/Manager"

static GMainLoop *loop = NULL;
static char *default_user = NULL;
static char *claimed_user = NULL;
static GDBusConnection *system_bus = NULL;

/* Текущие операции */
typedef struct {
    char *sender;
    char *finger;
    GDBusMethodInvocation *invocation;
    gboolean active;
    FpDevice *dev;
    GCancellable *cancellable;
} CurrentOperation;

static CurrentOperation *current_enroll = NULL;

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

/* ========== Колбэки для асинхронного enroll ========== */

static void enroll_progress_cb(FpDevice *dev, int status, FpPrint *print, void *user_data, GError *error) {
    (void)dev;
    (void)print;
    (void)user_data;
    (void)error;
    g_print("📢 Enroll progress, status=%d\n", status);
}

static void enroll_cb(GObject *source_object, GAsyncResult *res, void *user_data) {
    FpDevice *dev = (FpDevice *)source_object;
    CurrentOperation *op = (CurrentOperation *)user_data;
    GError *error = NULL;

    g_print("DEBUG: enroll_cb called\n");

    FpPrint *print = fp_device_enroll_finish(dev, res, &error);

    if (!print) {
        if (error) {
            g_printerr("ERROR: Enroll failed: %s\n", error->message);
            g_clear_error(&error);
        } else {
            g_printerr("ERROR: Enroll failed (unknown reason)\n");
        }
        send_enroll_status_signal("enroll-failed", TRUE);

        g_free(op->sender);
        g_free(op->finger);
        if (op->cancellable) g_object_unref(op->cancellable);
        g_free(op);
        current_enroll = NULL;
    } else {
        g_print("DEBUG: Enroll succeeded\n");

        const char *user = claimed_user ? claimed_user : default_user;
        if (storage_save_print(print, user, op->finger) == 0) {
            send_enroll_status_signal("enroll-completed", TRUE);
        } else {
            send_enroll_status_signal("enroll-failed", TRUE);
        }

        g_object_unref(print);
        g_free(op->sender);
        g_free(op->finger);
        if (op->cancellable) g_object_unref(op->cancellable);
        g_free(op);
        current_enroll = NULL;
    }

    // НЕ закрываем устройство
    g_print("DEBUG: enroll_cb completed\n");
}

static void open_cb(GObject *source_object, GAsyncResult *res, void *user_data) {
    FpDevice *dev = (FpDevice *)source_object;
    CurrentOperation *op = (CurrentOperation *)user_data;
    GError *error = NULL;

    g_print("DEBUG: open_cb called\n");

    if (!fp_device_open_finish(dev, res, &error)) {
        g_printerr("ERROR: Failed to open device: %s\n", error->message);
        send_enroll_status_signal("enroll-failed", TRUE);
        g_clear_error(&error);

        g_free(op->sender);
        g_free(op->finger);
        if (op->cancellable) g_object_unref(op->cancellable);
        g_free(op);
        current_enroll = NULL;
        return;
    }

    g_print("DEBUG: Device opened successfully\n");
    device_opened = TRUE;
    send_enroll_status_signal("enroll-ready", FALSE);

    FpPrint *template = fp_print_new(dev);
    if (!template) {
        g_printerr("ERROR: Failed to create print template\n");
        send_enroll_status_signal("enroll-failed", TRUE);
        g_free(op->sender);
        g_free(op->finger);
        if (op->cancellable) g_object_unref(op->cancellable);
        g_free(op);
        current_enroll = NULL;
        return;
    }

    g_print("DEBUG: Starting enroll with template...\n");
    fp_device_enroll(dev,
                     template,
                     op->cancellable,
                     enroll_progress_cb,
                     op,
                     NULL,
                     enroll_cb,
                     op);

    g_object_unref(template);
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

    // ========== ListEnrolledFingers ==========
    if (g_strcmp0(method_name, "ListEnrolledFingers") == 0) {
        g_print("ListEnrolledFingers called\n");

        const char *username;
        g_variant_get(parameters, "(&s)", &username);
        const char *user = (username && strlen(username) > 0) ? username : default_user;

        GPtrArray *fingers = storage_list_fingers(user);

        GVariantBuilder builder;
        g_variant_builder_init(&builder, G_VARIANT_TYPE("as"));

        if (fingers) {
            for (guint i = 0; i < fingers->len; i++) {
                const char *finger = g_ptr_array_index(fingers, i);
                g_variant_builder_add(&builder, "s", finger);
            }
            g_ptr_array_unref(fingers);
        }

        g_dbus_method_invocation_return_value(invocation,
                                              g_variant_new("(as)", &builder));
    }
    // ========== Claim ==========
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

        g_dbus_method_invocation_return_value(invocation, NULL);
    }
    // ========== Release ==========
    else if (g_strcmp0(method_name, "Release") == 0) {
        g_print("Release called\n");
        g_free(claimed_user);
        claimed_user = NULL;
        g_dbus_method_invocation_return_value(invocation, NULL);
    }
    // ========== DeleteEnrolledFinger ==========
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
        int result = storage_delete_finger(user, finger);

        if (result == 0) {
            g_dbus_method_invocation_return_value(invocation, NULL);
        } else {
            g_dbus_method_invocation_return_error(invocation,
                                                  G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                  "Failed to delete fingerprint");
        }
    }
    // ========== DeleteEnrolledFingers ==========
    else if (g_strcmp0(method_name, "DeleteEnrolledFingers") == 0) {
        g_print("DeleteEnrolledFingers called — not implemented\n");
        g_dbus_method_invocation_return_error(invocation,
                                              G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                              "DeleteEnrolledFingers not implemented");
    }
    // ========== EnrollStart ==========
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

        if (!claimed_user) {
            g_dbus_method_invocation_return_error(invocation,
                                                  G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                  "No user claimed");
            return;
        }

        FpPrint *existing = storage_load_print(claimed_user, finger);
        if (existing) {
            g_object_unref(existing);
            g_dbus_method_invocation_return_error(invocation,
                                                  G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                  "Finger already enrolled");
            return;
        }

        FpDevice *dev = device_get();
        if (!dev) {
            g_dbus_method_invocation_return_error(invocation,
                                                  G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                  "Failed to get device");
            return;
        }

        current_enroll = g_new0(CurrentOperation, 1);
        current_enroll->sender = g_strdup(sender);
        current_enroll->finger = g_strdup(finger);
        current_enroll->invocation = g_object_ref(invocation);
        current_enroll->active = TRUE;
        current_enroll->dev = dev;
        current_enroll->cancellable = g_cancellable_new();

        if (!device_opened) {
            g_print("DEBUG: Opening device asynchronously...\n");
            fp_device_open(dev, current_enroll->cancellable, (GAsyncReadyCallback)open_cb, current_enroll);
        } else {
            g_print("DEBUG: Device already open, starting enroll directly...\n");
            FpPrint *template = fp_print_new(dev);
            if (!template) {
                g_dbus_method_invocation_return_error(invocation,
                                                      G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                      "Failed to create print template");
                g_free(current_enroll->sender);
                g_free(current_enroll->finger);
                g_free(current_enroll);
                current_enroll = NULL;
                return;
            }
            send_enroll_status_signal("enroll-ready", FALSE);
            fp_device_enroll(dev,
                             template,
                             current_enroll->cancellable,
                             enroll_progress_cb,
                             current_enroll,
                             NULL,
                             enroll_cb,
                             current_enroll);
            g_object_unref(template);
        }

        g_dbus_method_invocation_return_value(invocation, NULL);
    }
    // ========== EnrollStop ==========
    else if (g_strcmp0(method_name, "EnrollStop") == 0) {
        g_print("EnrollStop called\n");
        if (current_enroll && current_enroll->cancellable) {
            g_cancellable_cancel(current_enroll->cancellable);
        }
        g_dbus_method_invocation_return_value(invocation, NULL);
    }
    // ========== GetCapabilities ==========
    else if (g_strcmp0(method_name, "GetCapabilities") == 0) {
        g_print("GetCapabilities called\n");
        g_dbus_method_invocation_return_value(invocation,
                                              g_variant_new("(u)", 5));
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
                                                                                                                                                                                                                                                "      <arg type='s' name='username' direction='in'/>"
                                                                                                                                                                                                                                                "      <arg type='as' name='fingers' direction='out'/>"
                                                                                                                                                                                                                                                "    </method>"
                                                                                                                                                                                                                                                "    <method name='Claim'>"
                                                                                                                                                                                                                                                "      <arg type='s' name='username' direction='in'/>"
                                                                                                                                                                                                                                                "    </method>"
                                                                                                                                                                                                                                                "    <method name='Release'/>"
                                                                                                                                                                                                                                                "    <method name='DeleteEnrolledFinger'>"
                                                                                                                                                                                                                                                "      <arg type='s' name='finger' direction='in'/>"
                                                                                                                                                                                                                                                "    </method>"
                                                                                                                                                                                                                                                "    <method name='DeleteEnrolledFingers'/>"
                                                                                                                                                                                                                                                "    <method name='EnrollStart'>"
                                                                                                                                                                                                                                                "      <arg type='s' name='finger' direction='in'/>"
                                                                                                                                                                                                                                                "    </method>"
                                                                                                                                                                                                                                                "    <method name='EnrollStop'/>"
                                                                                                                                                                                                                                                "    <method name='GetCapabilities'>"
                                                                                                                                                                                                                                                "      <arg type='u' name='caps' direction='out'/>"
                                                                                                                                                                                                                                                "    </method>"
                                                                                                                                                                                                                                                "    <signal name='EnrollStatus'>"
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

                                                                                                                                                                                                                                                                if (device_init() != 0) {
                                                                                                                                                                                                                                                                g_printerr("Failed to initialize device\n");
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
                                                                                                                                                                                                                                                                device_shutdown();
                                                                                                                                                                                                                                                                g_free(default_user);
                                                                                                                                                                                                                                                                g_free(claimed_user);

                                                                                                                                                                                                                                                                return 0;
                                                                                                                                                                                                                                                                }
