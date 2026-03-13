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
} CurrentOperation;

static CurrentOperation *current_enroll = NULL;
static CurrentOperation *current_verify = NULL;

/* ========== Объявления функций ========== */
static gpointer enroll_thread_func(gpointer data);
static gpointer verify_thread_func(gpointer data);
static void send_enroll_status_signal(const char *result, gboolean done);
static void send_verify_status_signal(const char *result, gboolean done);

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

/* ========== Обработчики операций ========== */

static gpointer enroll_thread_func(gpointer data) {
    CurrentOperation *op = (CurrentOperation *)data;

    g_print("Starting enroll for finger: %s\n", op->finger);

    FpDevice *dev = device_get();
    if (!dev) {
        send_enroll_status_signal("enroll-failed", TRUE);
        g_free(op->sender);
        g_free(op->finger);
        g_free(op);
        current_enroll = NULL;
        return NULL;
    }

    GError *error = NULL;
    if (!fp_device_open_sync(dev, NULL, &error)) {
        g_printerr("Failed to open device: %s\n", error->message);
        send_enroll_status_signal("enroll-failed", TRUE);
        g_clear_error(&error);
        g_free(op->sender);
        g_free(op->finger);
        g_free(op);
        current_enroll = NULL;
        return NULL;
    }

    send_enroll_status_signal("enroll-ready", FALSE);

    // ПРАВИЛЬНЫЙ ВЫЗОВ
    GCancellable *cancellable = NULL;
    FpPrint *template_print = NULL;  // для первого пальца передаём NULL
    FpPrint *print = fp_device_enroll_sync(dev, cancellable, template_print,
                                           NULL, NULL, &error);

    if (!print) {
        g_printerr("Enroll failed: %s\n", error->message);
        send_enroll_status_signal("enroll-failed", TRUE);
        g_clear_error(&error);
    } else {
        const char *user = claimed_user ? claimed_user : default_user;
        if (storage_save_print(print, user, op->finger) == 0) {
            send_enroll_status_signal("enroll-completed", TRUE);
        } else {
            send_enroll_status_signal("enroll-failed", TRUE);
        }
        g_object_unref(print);
    }

    fp_device_close_sync(dev, NULL, NULL);

    g_free(op->sender);
    g_free(op->finger);
    g_free(op);
    current_enroll = NULL;

    return NULL;
}

static gpointer verify_thread_func(gpointer data) {
    CurrentOperation *op = (CurrentOperation *)data;

    g_print("Starting verify for finger: %s\n", op->finger);

    const char *user = claimed_user ? claimed_user : default_user;

    FpPrint *enrolled = storage_load_print(user, op->finger);
    if (!enrolled) {
        send_verify_status_signal("verify-no-match", TRUE);
        g_free(op->sender);
        g_free(op->finger);
        g_free(op);
        current_verify = NULL;
        return NULL;
    }

    FpDevice *dev = device_get();
    if (!dev) {
        g_object_unref(enrolled);
        send_verify_status_signal("verify-failed", TRUE);
        g_free(op->sender);
        g_free(op->finger);
        g_free(op);
        current_verify = NULL;
        return NULL;
    }

    GError *error = NULL;
    if (!fp_device_open_sync(dev, NULL, &error)) {
        g_printerr("Failed to open device: %s\n", error->message);
        g_object_unref(enrolled);
        send_verify_status_signal("verify-failed", TRUE);
        g_clear_error(&error);
        g_free(op->sender);
        g_free(op->finger);
        g_free(op);
        current_verify = NULL;
        return NULL;
    }

    // ПРАВИЛЬНЫЙ ВЫЗОВ
    GCancellable *cancellable = NULL;
    gboolean match = FALSE;
    gboolean result = fp_device_verify_sync(dev, enrolled, cancellable,
                                            NULL, NULL, &match, &error);

    if (!result) {
        g_printerr("Verify failed: %s\n", error->message);
        send_verify_status_signal("verify-no-match", TRUE);
        g_clear_error(&error);
    } else if (match) {
        send_verify_status_signal("verify-match", TRUE);
    } else {
        send_verify_status_signal("verify-no-match", TRUE);
    }

    g_object_unref(enrolled);
    fp_device_close_sync(dev, NULL, NULL);

    g_free(op->sender);
    g_free(op->finger);
    g_free(op);
    current_verify = NULL;

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

    if (g_strcmp0(interface_name, "net.reactivated.Fprint.Device") != 0) {
        g_dbus_method_invocation_return_error(invocation,
                                              G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_INTERFACE,
                                              "Unknown interface");
        return;
    }

    if (g_strcmp0(method_name, "ListEnrolledFingers") == 0) {
        const char *user = claimed_user ? claimed_user : default_user;
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
    else if (g_strcmp0(method_name, "Claim") == 0) {
        const char *username;
        g_variant_get(parameters, "(&s)", &username);

        g_free(claimed_user);
        claimed_user = g_strdup(username && strlen(username) > 0 ? username : default_user);
        g_print("Claimed for user: %s\n", claimed_user);

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

        if (current_enroll) {
            g_dbus_method_invocation_return_error(invocation,
                                                  G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                  "Enroll already in progress");
            return;
        }

        current_enroll = g_new0(CurrentOperation, 1);
        current_enroll->sender = g_strdup(sender);
        current_enroll->finger = g_strdup(finger);
        current_enroll->invocation = g_object_ref(invocation);

        GThread *thread = g_thread_new("enroll-thread", enroll_thread_func, current_enroll);
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
    else if (g_strcmp0(method_name, "VerifyStart") == 0) {
        const char *finger;
        g_variant_get(parameters, "(&s)", &finger);

        if (current_verify) {
            g_dbus_method_invocation_return_error(invocation,
                                                  G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                                                  "Verify already in progress");
            return;
        }

        current_verify = g_new0(CurrentOperation, 1);
        current_verify->sender = g_strdup(sender);
        current_verify->finger = g_strdup(finger);
        current_verify->invocation = g_object_ref(invocation);

        GThread *thread = g_thread_new("verify-thread", verify_thread_func, current_verify);
        g_thread_unref(thread);

        g_dbus_method_invocation_return_value(invocation, NULL);
    }
    else if (g_strcmp0(method_name, "VerifyStop") == 0) {
        g_print("VerifyStop called\n");
        if (current_verify) {
            current_verify->active = FALSE;
            g_free(current_verify->sender);
            g_free(current_verify->finger);
            g_free(current_verify);
            current_verify = NULL;
        }
        g_dbus_method_invocation_return_value(invocation, NULL);
    }
    else if (g_strcmp0(method_name, "GetCapabilities") == 0) {
        g_dbus_method_invocation_return_value(invocation,
                                              g_variant_new("(u)", 5));
    }
    else if (g_strcmp0(method_name, "GetScanType") == 0) {
        g_dbus_method_invocation_return_value(invocation,
                                              g_variant_new("(s)", "press"));
    }
    else {
        g_dbus_method_invocation_return_error(invocation,
                                              G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
                                              "Unknown method: %s", method_name);
    }
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

                                                                      /* ========== Инициализация D-Bus ========== */

                                                                      static void on_bus_acquired(GDBusConnection *connection,
                                                                                                  const gchar *name,
                                                                                                  gpointer user_data) {

                                                                          g_print("Bus acquired: %s\n", name);
                                                                          system_bus = connection;

                                                                          GDBusInterfaceVTable device_vtable = {
                                                                              handle_method_call,
                                                                              NULL,
                                                                              NULL
                                                                          };

                                                                          GDBusNodeInfo *device_info = g_dbus_node_info_new_for_xml(
                                                                              "<node>"
                                                                              "  <interface name='net.reactivated.Fprint.Device'>"
                                                                              "    <method name='ListEnrolledFingers'>"
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
                                                                              NULL,
                                                                              NULL
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

                                                                                                                                                            // Инициализируем устройство
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
                                                                                                                                                            device_close();
                                                                                                                                                            g_free(default_user);
                                                                                                                                                            g_free(claimed_user);

                                                                                                                                                            return 0;
                                                                                                                                                        }
