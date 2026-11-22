#include "nvs_storage.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "nvs_storage";

#define NVS_CONFIG_NAMESPACE "water_cfg"
#define NVS_METER_NAMESPACE  "water_data"
#define NVS_CONFIG_KEY       "settings"
#define NVS_METER_KEY        "meter"
#define NVS_STORAGE_VERSION 2U

typedef struct {
    uint32_t version;
    app_config_t value;
} config_record_t;

typedef struct {
    uint32_t version;
    meter_data_t value;
} meter_record_t;

/* Последние успешно сохраненные значения используются для сравнения. */
static app_config_t s_last_config;
static meter_data_t s_last_meter;
static bool s_config_snapshot_valid;
static bool s_meter_snapshot_valid;
static SemaphoreHandle_t s_storage_mutex;

void app_config_set_defaults(app_config_t *config)
{
    if (config == NULL) {
        return;
    }

    memset(config, 0, sizeof(*config));
    config->cold_pulse_liters = APP_DEFAULT_PULSE_LITERS;
    config->hot_pulse_liters = APP_DEFAULT_PULSE_LITERS;
    config->mqtt_port = APP_DEFAULT_MQTT_PORT;
    config->telegram_period = TELEGRAM_PERIOD_DISABLED;
    config->monthly_21 = false;
    config->monthly_21_hour = APP_DEFAULT_MONTHLY_21_HOUR;
    config->monthly_21_minute = APP_DEFAULT_MONTHLY_21_MINUTE;
    config->last_monthly_ym = 0;

    (void)snprintf(config->mqtt_base_topic,
                   sizeof(config->mqtt_base_topic),
                   "%s",
                   APP_DEFAULT_MQTT_TOPIC);
    (void)snprintf(config->timezone,
                   sizeof(config->timezone),
                   "%s",
                   APP_DEFAULT_TIMEZONE);
}

void meter_data_set_defaults(meter_data_t *data)
{
    if (data == NULL) {
        return;
    }

    memset(data, 0, sizeof(*data));
}

/*
 * Сравнение настроек по полям, а не через memcmp всей структуры:
 * это исключает ложные отличия из-за байтов выравнивания (padding).
 */
static bool config_equal(const app_config_t *a, const app_config_t *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }

    return a->cold_pulse_liters == b->cold_pulse_liters &&
           a->hot_pulse_liters == b->hot_pulse_liters &&
           strcmp(a->wifi_ssid, b->wifi_ssid) == 0 &&
           strcmp(a->wifi_password, b->wifi_password) == 0 &&
           a->mqtt_enabled == b->mqtt_enabled &&
           strcmp(a->mqtt_host, b->mqtt_host) == 0 &&
           a->mqtt_port == b->mqtt_port &&
           strcmp(a->mqtt_username, b->mqtt_username) == 0 &&
           strcmp(a->mqtt_password, b->mqtt_password) == 0 &&
           strcmp(a->mqtt_base_topic, b->mqtt_base_topic) == 0 &&
           strcmp(a->timezone, b->timezone) == 0 &&
           a->telegram_enabled == b->telegram_enabled &&
           strcmp(a->telegram_bot_token, b->telegram_bot_token) == 0 &&
           strcmp(a->telegram_chat_id, b->telegram_chat_id) == 0 &&
           a->telegram_period == b->telegram_period &&
           a->monthly_21 == b->monthly_21 &&
           a->monthly_21_hour == b->monthly_21_hour &&
           a->monthly_21_minute == b->monthly_21_minute &&
           a->last_monthly_ym == b->last_monthly_ym &&
           a->last_telegram_send_epoch == b->last_telegram_send_epoch;
}

static bool meter_equal(const meter_data_t *a, const meter_data_t *b)
{
    size_t i;

    if (a == NULL || b == NULL) {
        return false;
    }

    if (a->cold_total_liters != b->cold_total_liters ||
        a->hot_total_liters != b->hot_total_liters ||
        a->unsynced_cold_liters != b->unsynced_cold_liters ||
        a->unsynced_hot_liters != b->unsynced_hot_liters ||
        a->active_year_month != b->active_year_month) {
        return false;
    }

    for (i = 0U; i < APP_HISTORY_MONTHS; ++i) {
        if (a->history[i].year_month != b->history[i].year_month ||
            a->history[i].cold_liters != b->history[i].cold_liters ||
            a->history[i].hot_liters != b->history[i].hot_liters) {
            return false;
        }
    }

    return true;
}

static esp_err_t read_config_record(app_config_t *config)
{
    nvs_handle_t handle;
    config_record_t record;
    size_t required_size = sizeof(record);
    esp_err_t err;

    err = nvs_open(NVS_CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_get_blob(handle, NVS_CONFIG_KEY, &record, &required_size);
    nvs_close(handle);

    if (err == ESP_ERR_NVS_INVALID_LENGTH) return ESP_ERR_INVALID_VERSION;
    if (err != ESP_OK) {
        return err;
    }
    if (required_size != sizeof(record) || record.version != NVS_STORAGE_VERSION) {
        ESP_LOGW(TAG, "Запись настроек имеет неизвестный формат; будут применены defaults");
        return ESP_ERR_INVALID_VERSION;
    }

    *config = record.value;
    return ESP_OK;
}

static esp_err_t read_meter_record(meter_data_t *meter)
{
    nvs_handle_t handle;
    meter_record_t record;
    size_t required_size = sizeof(record);
    esp_err_t err;

    err = nvs_open(NVS_METER_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_get_blob(handle, NVS_METER_KEY, &record, &required_size);
    nvs_close(handle);

    if (err == ESP_ERR_NVS_INVALID_LENGTH) return ESP_ERR_INVALID_VERSION;
    if (err != ESP_OK) {
        return err;
    }
    if (required_size != sizeof(record) || record.version != NVS_STORAGE_VERSION) {
        ESP_LOGW(TAG, "Запись счетчиков имеет неизвестный формат; будут применены defaults");
        return ESP_ERR_INVALID_VERSION;
    }

    *meter = record.value;
    return ESP_OK;
}

static esp_err_t write_config_record(const app_config_t *config)
{
    nvs_handle_t handle;
    config_record_t record;
    esp_err_t err;

    memset(&record, 0, sizeof(record));
    record.version = NVS_STORAGE_VERSION;
    record.value = *config;

    err = nvs_open(NVS_CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_blob(handle, NVS_CONFIG_KEY, &record, sizeof(record));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK) {
        s_last_config = *config;
        s_config_snapshot_valid = true;
    }

    return err;
}

static esp_err_t write_meter_record(const meter_data_t *meter)
{
    nvs_handle_t handle;
    meter_record_t record;
    esp_err_t err;

    memset(&record, 0, sizeof(record));
    record.version = NVS_STORAGE_VERSION;
    record.value = *meter;

    err = nvs_open(NVS_METER_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_blob(handle, NVS_METER_KEY, &record, sizeof(record));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK) {
        s_last_meter = *meter;
        s_meter_snapshot_valid = true;
    }

    return err;
}

esp_err_t nvs_storage_init(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Раздел NVS требует восстановления; выполняется erase/init");
        err = nvs_flash_erase();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Ошибка очистки NVS: %s", esp_err_to_name(err));
            return err;
        }
        err = nvs_flash_init();
    }

    if (err == ESP_OK) {
        if (s_storage_mutex == NULL) {
            s_storage_mutex = xSemaphoreCreateMutex();
            if (s_storage_mutex == NULL) {
                ESP_LOGE(TAG, "Не удалось создать mutex модуля NVS");
                return ESP_ERR_NO_MEM;
            }
        }
        s_config_snapshot_valid = false;
        s_meter_snapshot_valid = false;
        ESP_LOGI(TAG, "NVS успешно инициализирована");
    } else {
        ESP_LOGE(TAG, "Не удалось инициализировать NVS: %s", esp_err_to_name(err));
    }

    return err;
}

esp_err_t nvs_storage_load(app_config_t *config, meter_data_t *meter)
{
    esp_err_t config_err;
    esp_err_t meter_err;

    if (config == NULL || meter == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_storage_mutex == NULL || xSemaphoreTake(s_storage_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }

    app_config_set_defaults(config);
    meter_data_set_defaults(meter);

    config_err = read_config_record(config);
    if (config_err != ESP_OK && config_err != ESP_ERR_NVS_NOT_FOUND &&
        config_err != ESP_ERR_INVALID_VERSION) {
        ESP_LOGE(TAG, "Ошибка чтения настроек: %s", esp_err_to_name(config_err));
        xSemaphoreGive(s_storage_mutex);
        return config_err;
    }

    meter_err = read_meter_record(meter);
    if (meter_err != ESP_OK && meter_err != ESP_ERR_NVS_NOT_FOUND &&
        meter_err != ESP_ERR_INVALID_VERSION) {
        ESP_LOGE(TAG, "Ошибка чтения показаний: %s", esp_err_to_name(meter_err));
        xSemaphoreGive(s_storage_mutex);
        return meter_err;
    }

    /* Гарантируем корректные завершающие нули в строковых полях. */
    config->wifi_ssid[APP_WIFI_SSID_MAX] = '\0';
    config->wifi_password[APP_WIFI_PASSWORD_MAX] = '\0';
    config->mqtt_host[APP_MQTT_HOST_MAX] = '\0';
    config->mqtt_username[APP_MQTT_USERNAME_MAX] = '\0';
    config->mqtt_password[APP_MQTT_PASSWORD_MAX] = '\0';
    config->mqtt_base_topic[APP_MQTT_TOPIC_MAX] = '\0';
    config->timezone[APP_TIMEZONE_MAX] = '\0';
    config->telegram_bot_token[APP_TELEGRAM_TOKEN_MAX] = '\0';
    config->telegram_chat_id[APP_TELEGRAM_CHAT_ID_MAX] = '\0';

    /* Запоминаем нормализованные данные для последующего сравнения. */
    s_last_config = *config;
    s_last_meter = *meter;
    s_config_snapshot_valid = (config_err == ESP_OK);
    s_meter_snapshot_valid = (meter_err == ESP_OK);

    ESP_LOGI(TAG, "Загрузка завершена: ХВС=%llu л, ГВС=%llu л",
             (unsigned long long)meter->cold_total_liters,
             (unsigned long long)meter->hot_total_liters);

    xSemaphoreGive(s_storage_mutex);
    return ESP_OK;
}

esp_err_t nvs_storage_save_config_if_changed(const app_config_t *config)
{
    esp_err_t err;

    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_storage_mutex == NULL || xSemaphoreTake(s_storage_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_config_snapshot_valid && config_equal(config, &s_last_config)) {
        ESP_LOGD(TAG, "Настройки не изменились — запись в NVS пропущена");
        err = ESP_OK;
    } else {
        err = write_config_record(config);
    }

    xSemaphoreGive(s_storage_mutex);
    return err;
}

esp_err_t nvs_storage_save_meter_if_changed(const meter_data_t *meter)
{
    esp_err_t err;

    if (meter == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_storage_mutex == NULL || xSemaphoreTake(s_storage_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_meter_snapshot_valid && meter_equal(meter, &s_last_meter)) {
        ESP_LOGD(TAG, "Показания и история не изменились — запись в NVS пропущена");
        err = ESP_OK;
    } else {
        err = write_meter_record(meter);
    }

    xSemaphoreGive(s_storage_mutex);
    return err;
}

esp_err_t nvs_storage_save_meter_now(const meter_data_t *meter)
{
    /* Та же проверка изменений, но вызывается сразу после ручной правки. */
    return nvs_storage_save_meter_if_changed(meter);
}

esp_err_t nvs_storage_clear_wifi(void)
{
    app_config_t config;
    esp_err_t err;

    if (s_storage_mutex == NULL || xSemaphoreTake(s_storage_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }

    app_config_set_defaults(&config);

    /* Считываем текущую конфигурацию, чтобы не затронуть MQTT/Telegram. */
    err = read_config_record(&config);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND && err != ESP_ERR_INVALID_VERSION) {
        xSemaphoreGive(s_storage_mutex);
        return err;
    }

    memset(config.wifi_ssid, 0, sizeof(config.wifi_ssid));
    memset(config.wifi_password, 0, sizeof(config.wifi_password));

    err = write_config_record(&config);
    xSemaphoreGive(s_storage_mutex);
    return err;
}
