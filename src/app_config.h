#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/*
 * Общие типы и настройки проекта WaterMeter для ESP32-C3 SuperMini.
 * Стандарт языка: C11.
 *
 * Все объемы воды внутри программы хранятся в целых литрах.
 * Перевод в м³ выполняется только при формировании интерфейса/API.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APP_VERSION_MAJOR             1U
#define APP_VERSION_MINOR             0U
#define APP_VERSION_PATCH             0U

#define APP_HISTORY_MONTHS            12U
#define APP_DEFAULT_PULSE_LITERS      10U
#define APP_DEFAULT_MQTT_PORT         1883U
#define APP_DEFAULT_AP_SSID            "WaterMeter-Setup"
#define APP_DEFAULT_AP_PASSWORD       "12345678"
#define APP_DEFAULT_MQTT_TOPIC         "home/water"
#define APP_DEFAULT_TIMEZONE          "MSK-3"
#define APP_DEFAULT_MONTHLY_21_HOUR   9U
#define APP_DEFAULT_MONTHLY_21_MINUTE 0U

#define APP_WIFI_SSID_MAX             32U
#define APP_WIFI_PASSWORD_MAX         64U
#define APP_MQTT_HOST_MAX             128U
#define APP_MQTT_USERNAME_MAX         64U
#define APP_MQTT_PASSWORD_MAX         64U
#define APP_MQTT_TOPIC_MAX            128U
#define APP_TELEGRAM_TOKEN_MAX        128U
#define APP_TELEGRAM_CHAT_ID_MAX      32U
#define APP_TIMEZONE_MAX              64U

/* Периодичность регулярного Telegram-отчета. */
typedef enum {
    TELEGRAM_PERIOD_DISABLED = 0,
    TELEGRAM_PERIOD_24_HOURS = 1,
    TELEGRAM_PERIOD_7_DAYS = 2,
    TELEGRAM_PERIOD_30_DAYS = 3
} telegram_period_t;

/* Настройки устройства, сохраняемые в NVS. */
typedef struct {
    uint32_t cold_pulse_liters;
    uint32_t hot_pulse_liters;

    char wifi_ssid[APP_WIFI_SSID_MAX + 1U];
    char wifi_password[APP_WIFI_PASSWORD_MAX + 1U];

    bool mqtt_enabled;
    char mqtt_host[APP_MQTT_HOST_MAX + 1U];
    uint16_t mqtt_port;
    char mqtt_username[APP_MQTT_USERNAME_MAX + 1U];
    char mqtt_password[APP_MQTT_PASSWORD_MAX + 1U];
    char mqtt_base_topic[APP_MQTT_TOPIC_MAX + 1U];

    char timezone[APP_TIMEZONE_MAX + 1U];

    bool telegram_enabled;
    char telegram_bot_token[APP_TELEGRAM_TOKEN_MAX + 1U];
    char telegram_chat_id[APP_TELEGRAM_CHAT_ID_MAX + 1U];
    telegram_period_t telegram_period;

    bool monthly_21;
    uint8_t monthly_21_hour;
    uint8_t monthly_21_minute;
    int32_t last_monthly_ym;
    int64_t last_telegram_send_epoch; /* Последняя регулярная отправка, Unix time. */
} app_config_t;

/* Расход отдельно за один календарный месяц. YYYYMM, например 202611. */
typedef struct {
    uint32_t year_month;
    uint64_t cold_liters;
    uint64_t hot_liters;
} water_month_t;

/*
 * Данные счетчиков и месячной истории.
 * Массив history всегда имеет ровно APP_HISTORY_MONTHS элементов.
 */
typedef struct {
    uint64_t cold_total_liters;
    uint64_t hot_total_liters;

    water_month_t history[APP_HISTORY_MONTHS];

    /* Прирост показаний, полученный до первой синхронизации времени. */
    uint64_t unsynced_cold_liters;
    uint64_t unsynced_hot_liters;

    /* Последний календарный месяц, обработанный приложением (YYYYMM). */
    uint32_t active_year_month;
} meter_data_t;

/* Удобная функция преобразования литров в кубометры для API/UI. */
static inline double app_liters_to_m3(uint64_t liters)
{
    return (double)liters / 1000.0;
}

/* Значения настроек по умолчанию. */
void app_config_set_defaults(app_config_t *config);

/* Начальное обнуление структуры показаний и истории. */
void meter_data_set_defaults(meter_data_t *data);

#ifdef __cplusplus
}
#endif

#endif /* APP_CONFIG_H */
