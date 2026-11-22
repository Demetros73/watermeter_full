#ifndef APP_STATE_H
#define APP_STATE_H
#include "app_config.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    app_config_t config;
    meter_data_t meter;
    SemaphoreHandle_t mutex;
    bool wifi_connected;
    bool time_synced;
    bool mqtt_connected;
    bool cold_contact_closed;
    bool hot_contact_closed;
    int wifi_rssi;
    char wifi_ip[16];
    char wifi_ssid[APP_WIFI_SSID_MAX + 1U];
    int wifi_mode; /* WIFI_MODE_STA / WIFI_MODE_AP, хранится как int без зависимости заголовка. */
} app_state_t;

extern app_state_t g_app;
esp_err_t app_state_init(void);
void app_state_snapshot(app_config_t *cfg, meter_data_t *meter, app_state_t *runtime);
void app_state_add_pulse(bool hot);
void app_state_reconcile_time(void);
uint32_t app_current_year_month(void);
void app_history_ensure_month_locked(uint32_t year_month);
#endif
