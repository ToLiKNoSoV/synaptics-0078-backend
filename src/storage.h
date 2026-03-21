#ifndef STORAGE_H
#define STORAGE_H

#include <glib.h>
#include <libfprint-2/fprint.h>

int storage_save_print(FpPrint *print, const char *username, const char *finger);
FpPrint* storage_load_print(const char *username, const char *finger);
GPtrArray* storage_list_fingers(const char *username);
int storage_delete_finger(const char *username, const char *finger);

#endif /* STORAGE_H */
