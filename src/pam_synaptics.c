/*
 * PAM module for Synaptics 0078 fingerprint driver
 * Based on pam_fprintd design
 *
 * Copyright (C) 2026 Anatoliy Nosov <toliknosov1994@eyandex.ru>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define PAM_SM_AUTH
#define _GNU_SOURCE

#include <security/pam_modules.h>
#include <security/pam_appl.h>
#include <systemd/sd-bus.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <syslog.h>
#include <pwd.h>
#include <dirent.h>
#include <sys/stat.h>      /* Добавлено для stat и S_ISREG */

#define FPRINTD_NAME "net.reactivated.Fprint"
#define FPRINTD_PATH "/net/reactivated/Fprint/Device/0"
#define FPRINTD_INTERFACE "net.reactivated.Fprint.Device"
#define DEFAULT_TIMEOUT 30
#define DEFAULT_MAX_TRIES 3

/* Состояние verify операции */
typedef struct {
    int result;        /* -1: не готов, 0: неудача, 1: успех */
    int done;          /* 0: нет, 1: да */
    const char *finger; /* Имя пальца */
} VerifyState;

/* Опции модуля */
typedef struct {
    int debug;
    int max_tries;
    int timeout;
    const char *finger;  /* Может быть NULL */
} PamOptions;

/* Получить первый доступный палец пользователя */
static char *get_first_finger(const char *user) {
    char *user_dir = NULL;
    DIR *dir = NULL;
    struct dirent *entry;
    char *finger = NULL;
    struct stat st;
    char *path = NULL;

    if (!user || strcmp(user, "root") == 0) {
        return NULL;
    }

    if (asprintf(&user_dir, "/var/lib/fprint/%s", user) == -1) {
        return NULL;
    }

    dir = opendir(user_dir);
    if (!dir) {
        free(user_dir);
        return NULL;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        if (asprintf(&path, "%s/%s", user_dir, entry->d_name) == -1) {
            continue;
        }

        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            finger = strdup(entry->d_name);
            free(path);
            break;
        }
        free(path);
    }

    closedir(dir);
    free(user_dir);

    return finger;
}

/* Парсинг опций модуля */
static void parse_options(pam_handle_t *pamh, int argc, const char **argv, PamOptions *opts) {
    opts->debug = 0;
    opts->max_tries = DEFAULT_MAX_TRIES;
    opts->timeout = DEFAULT_TIMEOUT;
    opts->finger = NULL;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "debug") == 0) {
            opts->debug = 1;
        } else if (strncmp(argv[i], "max-tries=", 10) == 0) {
            opts->max_tries = atoi(argv[i] + 10);
        } else if (strncmp(argv[i], "timeout=", 8) == 0) {
            opts->timeout = atoi(argv[i] + 8);
        } else if (strncmp(argv[i], "finger=", 7) == 0) {
            opts->finger = argv[i] + 7;
        }
    }

    if (opts->debug) {
        openlog("pam_synaptics", LOG_PID | LOG_CONS, LOG_AUTH);
        if (opts->finger) {
            syslog(LOG_DEBUG, "PAM module loaded with options: max_tries=%d, timeout=%d, finger=%s",
                   opts->max_tries, opts->timeout, opts->finger);
        } else {
            syslog(LOG_DEBUG, "PAM module loaded with options: max_tries=%d, timeout=%d, finger=auto",
                   opts->max_tries, opts->timeout);
        }
        closelog();
    }
}

/* Callback для сигнала VerifyStatus */
static int on_verify_status(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
    VerifyState *state = (VerifyState *)userdata;
    const char *result;
    int done;

    sd_bus_message_read(m, "sb", &result, &done);

    if (strcmp(result, "verify-match") == 0) {
        state->result = 1;
    } else if (strcmp(result, "verify-no-match") == 0) {
        state->result = 0;
    } else if (strcmp(result, "verify-failed") == 0) {
        state->result = 0;
    }

    state->done = done;

    return 0;
}

/* Выполнение verify операции */
static int do_verify(pam_handle_t *pamh, const char *user, PamOptions *opts) {
    sd_bus *bus = NULL;
    sd_bus_slot *slot = NULL;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    VerifyState state = { -1, 0, NULL };
    int ret = PAM_AUTH_ERR;
    int try_count = 0;
    char *finger_to_use = NULL;
    int free_finger = 0;

    /* Определяем, какой палец использовать */
    if (opts->finger) {
        state.finger = opts->finger;
        if (opts->debug) {
            openlog("pam_synaptics", LOG_PID | LOG_CONS, LOG_AUTH);
            syslog(LOG_DEBUG, "Using configured finger: %s", state.finger);
            closelog();
        }
    } else {
        finger_to_use = get_first_finger(user);
        if (!finger_to_use) {
            if (opts->debug) {
                openlog("pam_synaptics", LOG_PID | LOG_CONS, LOG_AUTH);
                syslog(LOG_DEBUG, "No enrolled fingers found for user %s", user);
                closelog();
            }
            return PAM_AUTH_ERR;
        }
        state.finger = finger_to_use;
        free_finger = 1;
        if (opts->debug) {
            openlog("pam_synaptics", LOG_PID | LOG_CONS, LOG_AUTH);
            syslog(LOG_DEBUG, "Using first available finger: %s", state.finger);
            closelog();
        }
    }

    /* Подключаемся к system bus */
    if (sd_bus_default_system(&bus) < 0) {
        if (free_finger) free(finger_to_use);
        openlog("pam_synaptics", LOG_PID | LOG_CONS, LOG_AUTH);
        syslog(LOG_ERR, "Cannot connect to system bus");
        closelog();
        return PAM_AUTH_ERR;
    }

    if (opts->debug) {
        openlog("pam_synaptics", LOG_PID | LOG_CONS, LOG_AUTH);
        syslog(LOG_DEBUG, "Connected to system bus");
        closelog();
    }

    /* Подписываемся на сигнал VerifyStatus */
    sd_bus_add_match(
        bus,
        &slot,
        "type='signal',"
        "interface='net.reactivated.Fprint.Device',"
        "member='VerifyStatus',"
        "path='/net/reactivated/Fprint/Device/0'",
        on_verify_status,
        &state
    );

    if (opts->debug) {
        openlog("pam_synaptics", LOG_PID | LOG_CONS, LOG_AUTH);
        syslog(LOG_DEBUG, "Subscribed to VerifyStatus signals");
        closelog();
    }

    /* Claim устройство */
    sd_bus_call_method(
        bus,
        FPRINTD_NAME,
        FPRINTD_PATH,
        FPRINTD_INTERFACE,
        "Claim",
        &error,
        NULL,
        "s",
        user
    );

    if (opts->debug) {
        openlog("pam_synaptics", LOG_PID | LOG_CONS, LOG_AUTH);
        syslog(LOG_DEBUG, "Claimed device for user: %s", user);
        closelog();
    }

    /* Пробуем несколько раз */
    while (try_count < opts->max_tries && state.result != 1) {
        if (opts->debug) {
            openlog("pam_synaptics", LOG_PID | LOG_CONS, LOG_AUTH);
            syslog(LOG_DEBUG, "Verify attempt %d/%d for finger: %s",
                   try_count + 1, opts->max_tries, state.finger);
            closelog();
        }

        /* Запускаем verify */
        sd_bus_error_free(&error);
        sd_bus_call_method(
            bus,
            FPRINTD_NAME,
            FPRINTD_PATH,
            FPRINTD_INTERFACE,
            "VerifyStart",
            &error,
            NULL,
            "s",
            state.finger
        );

        if (sd_bus_error_is_set(&error)) {
            openlog("pam_synaptics", LOG_PID | LOG_CONS, LOG_AUTH);
            syslog(LOG_ERR, "Failed to start verify: %s", error.message);
            closelog();
            break;
        }

        /* Ждем результат */
        time_t start = time(NULL);
        while (state.result == -1 && (time(NULL) - start) < opts->timeout) {
            sd_bus_process(bus, NULL);
            usleep(100000);  /* 100ms */
        }

        /* Завершаем verify */
        sd_bus_call_method(
            bus,
            FPRINTD_NAME,
            FPRINTD_PATH,
            FPRINTD_INTERFACE,
            "VerifyStop",
            NULL,
            NULL,
            ""
        );

        if (state.result == 1) {
            if (opts->debug) {
                openlog("pam_synaptics", LOG_PID | LOG_CONS, LOG_AUTH);
                syslog(LOG_DEBUG, "Verify successful on attempt %d", try_count + 1);
                closelog();
            }
            ret = PAM_SUCCESS;
            break;
        } else if (state.result == 0) {
            if (opts->debug) {
                openlog("pam_synaptics", LOG_PID | LOG_CONS, LOG_AUTH);
                syslog(LOG_DEBUG, "Verify failed on attempt %d", try_count + 1);
                closelog();
            }
            /* Сбрасываем состояние для следующей попытки */
            state.result = -1;
            state.done = 0;
            try_count++;
        } else {
            /* Таймаут */
            if (opts->debug) {
                openlog("pam_synaptics", LOG_PID | LOG_CONS, LOG_AUTH);
                syslog(LOG_DEBUG, "Verify timeout on attempt %d", try_count + 1);
                closelog();
            }
            try_count++;
        }
    }

    /* Release устройство */
    sd_bus_call_method(
        bus,
        FPRINTD_NAME,
        FPRINTD_PATH,
        FPRINTD_INTERFACE,
        "Release",
        NULL,
        NULL,
        ""
    );

    if (opts->debug) {
        openlog("pam_synaptics", LOG_PID | LOG_CONS, LOG_AUTH);
        syslog(LOG_DEBUG, "Released device");
        closelog();
    }

    sd_bus_slot_unref(slot);
    sd_bus_unref(bus);

    if (free_finger) free(finger_to_use);

    return ret;
}

PAM_EXTERN int pam_sm_authenticate(pam_handle_t *pamh, int flags, int argc, const char **argv) {
    const char *user;
    PamOptions opts;
    int ret;

    /* Парсим опции */
    parse_options(pamh, argc, argv, &opts);

    if (opts.debug) {
        openlog("pam_synaptics", LOG_PID | LOG_CONS, LOG_AUTH);
        syslog(LOG_DEBUG, "pam_sm_authenticate called");
        closelog();
    }

    /* Получаем имя пользователя */
    ret = pam_get_user(pamh, &user, NULL);
    if (ret != PAM_SUCCESS || !user) {
        openlog("pam_synaptics", LOG_PID | LOG_CONS, LOG_AUTH);
        syslog(LOG_ERR, "Cannot get username");
        closelog();
        return PAM_AUTH_ERR;
    }

    if (opts.debug) {
        openlog("pam_synaptics", LOG_PID | LOG_CONS, LOG_AUTH);
        syslog(LOG_DEBUG, "Authenticating user: %s", user);
        closelog();
    }

    /* Проверяем, не root ли это */
    if (strcmp(user, "root") == 0) {
        if (opts.debug) {
            openlog("pam_synaptics", LOG_PID | LOG_CONS, LOG_AUTH);
            syslog(LOG_DEBUG, "Root user - skipping fingerprint auth");
            closelog();
        }
        return PAM_IGNORE;
    }

    /* Выполняем verify */
    ret = do_verify(pamh, user, &opts);

    if (ret == PAM_SUCCESS) {
        if (opts.debug) {
            openlog("pam_synaptics", LOG_PID | LOG_CONS, LOG_AUTH);
            syslog(LOG_DEBUG, "Authentication successful");
            closelog();
        }
    } else {
        if (opts.debug) {
            openlog("pam_synaptics", LOG_PID | LOG_CONS, LOG_AUTH);
            syslog(LOG_DEBUG, "Authentication failed");
            closelog();
        }
    }

    return ret;
}

PAM_EXTERN int pam_sm_setcred(pam_handle_t *pamh, int flags, int argc, const char **argv) {
    return PAM_SUCCESS;
}

PAM_EXTERN int pam_sm_acct_mgmt(pam_handle_t *pamh, int flags, int argc, const char **argv) {
    return PAM_SUCCESS;
}

PAM_EXTERN int pam_sm_open_session(pam_handle_t *pamh, int flags, int argc, const char **argv) {
    return PAM_SUCCESS;
}

PAM_EXTERN int pam_sm_close_session(pam_handle_t *pamh, int flags, int argc, const char **argv) {
    return PAM_SUCCESS;
}

PAM_EXTERN int pam_sm_chauthtok(pam_handle_t *pamh, int flags, int argc, const char **argv) {
    return PAM_SUCCESS;
}

#ifdef PAM_STATIC
struct pam_module _pam_synaptics_modstruct = {
    "pam_synaptics",
    pam_sm_authenticate,
    pam_sm_setcred,
    pam_sm_acct_mgmt,
    pam_sm_open_session,
    pam_sm_close_session,
    pam_sm_chauthtok,
};
#endif
