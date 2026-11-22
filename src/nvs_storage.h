#ifndef NVS_STORAGE_H
#define NVS_STORAGE_H

/*
 * Интерфейс постоянного хранения WaterMeter.
 * Реализация использует NVS ESP-IDF и не выполняет лишних записей,
 * если переданные данные совпадают с последним сохраненным снимком.
 */

#include "app_config.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Инициализация NVS. При несовместимой версии раздела выполняется recovery. */
esp_err_t nvs_storage_init(void);

/*
 * Загрузка конфигурации и данных счетчиков.
 * Если записи отсутствуют или имеют неподдерживаемую версию,
 * используются значения по умолчанию.
 */
esp_err_t nvs_storage_load(app_config_t *config, meter_data_t *meter);

/*
 * Сохранить настройки только при фактическом изменении.
 * Для обычных изменений настроек можно вызывать сразу.
 */
esp_err_t nvs_storage_save_config_if_changed(const app_config_t *config);

/*
 * Сохранить счетчики, историю и несинхронизированный остаток,
 * только если данные отличаются от последнего снимка во Flash.
 * Отложенную задержку 15 секунд организует вызывающий модуль.
 */
esp_err_t nvs_storage_save_meter_if_changed(const meter_data_t *meter);

/* Немедленно сохранить показания после ручной корректировки. */
esp_err_t nvs_storage_save_meter_now(const meter_data_t *meter);

/* Очистить только Wi-Fi SSID и пароль; остальные настройки сохранить. */
esp_err_t nvs_storage_clear_wifi(void);

#ifdef __cplusplus
}
#endif

#endif /* NVS_STORAGE_H */
