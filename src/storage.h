#ifndef STORAGE_H
#define STORAGE_H

#include <libfprint-2/fprint.h>
#include <glib.h>

/**
 * Сохранить шаблон отпечатка в файл
 * @param print Шаблон для сохранения
 * @param username Имя пользователя
 * @param finger Название пальца (например, "right-index-finger")
 * @return 0 при успехе, -1 при ошибке
 */
int storage_save_print(FpPrint *print, const char *username, const char *finger);

/**
 * Загрузить шаблон отпечатка из файла
 * @param username Имя пользователя
 * @param finger Название пальца
 * @return FpPrint* или NULL при ошибке (должен быть освобождён через g_object_unref)
 */
FpPrint* storage_load_print(const char *username, const char *finger);

/**
 * Получить список всех зарегистрированных пальцев пользователя
 * @param username Имя пользователя
 * @return GPtrArray* со строками (имена пальцев) или NULL при ошибке
 */
GPtrArray* storage_list_fingers(const char *username);

/**
 * Удалить шаблон отпечатка
 * @param username Имя пользователя
 * @param finger Название пальца
 * @return 0 при успехе, -1 при ошибке
 */
int storage_delete_finger(const char *username, const char *finger);

#endif // STORAGE_H
