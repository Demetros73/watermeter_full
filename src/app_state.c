#include "app_state.h"
#include "nvs_storage.h"
#include "esp_log.h"
#include <string.h>
#include <time.h>

static const char *TAG = "app_state";
app_state_t g_app;

static uint32_t next_month(uint32_t ym)
{
    uint32_t year = ym / 100U, month = ym % 100U;
    if (month >= 12U) { year++; month = 1U; } else { month++; }
    return year * 100U + month;
}

uint32_t app_current_year_month(void)
{
    time_t now = time(NULL);
    struct tm tm_now;
    if (now < 0 || localtime_r(&now, &tm_now) == NULL || tm_now.tm_year < (2024 - 1900)) return 0U;
    return (uint32_t)(tm_now.tm_year + 1900) * 100U + (uint32_t)(tm_now.tm_mon + 1);
}

void app_history_ensure_month_locked(uint32_t ym)
{
    size_t i;
    if (ym == 0U) return;
    for (i = 0; i < APP_HISTORY_MONTHS; ++i) {
        if (g_app.meter.history[i].year_month == ym) {
            g_app.meter.active_year_month = ym;
            return;
        }
    }
    if (g_app.meter.active_year_month == 0U) {
        uint32_t first = ym;
        for (i = 1U; i < APP_HISTORY_MONTHS; ++i) {
            uint32_t year = first / 100U, month = first % 100U;
            if (month == 1U) { year--; month = 12U; } else { month--; }
            first = year * 100U + month;
        }
        for (i = 0U; i < APP_HISTORY_MONTHS; ++i) {
            g_app.meter.history[i].year_month = first;
            if (i + 1U < APP_HISTORY_MONTHS) first = next_month(first);
        }
        g_app.meter.active_year_month = ym;
        return;
    }
    /* При переходе вперед сдвигаем окно. Пропущенные месяцы получают нулевые строки. */
    if (ym > g_app.meter.active_year_month) {
        uint32_t cursor = g_app.meter.active_year_month;
        while (cursor < ym) {
            for (i = 0; i + 1U < APP_HISTORY_MONTHS; ++i) g_app.meter.history[i] = g_app.meter.history[i + 1U];
            memset(&g_app.meter.history[APP_HISTORY_MONTHS - 1U], 0, sizeof(water_month_t));
            cursor = next_month(cursor);
            g_app.meter.history[APP_HISTORY_MONTHS - 1U].year_month = cursor;
        }
        g_app.meter.active_year_month = ym;
    } else {
        /* Старая дата за пределами окна: не портим актуальную историю. */
        ESP_LOGW(TAG, "Получен месяц вне текущего окна: %u", (unsigned)ym);
    }
}

esp_err_t app_state_init(void)
{
    memset(&g_app, 0, sizeof(g_app));
    g_app.mutex = xSemaphoreCreateMutex();
    if (g_app.mutex == NULL) return ESP_ERR_NO_MEM;
    app_config_set_defaults(&g_app.config);
    meter_data_set_defaults(&g_app.meter);
    return nvs_storage_load(&g_app.config, &g_app.meter);
}

void app_state_snapshot(app_config_t *cfg, meter_data_t *meter, app_state_t *runtime)
{
    if (xSemaphoreTake(g_app.mutex, portMAX_DELAY) != pdTRUE) return;
    if (cfg) *cfg = g_app.config;
    if (meter) *meter = g_app.meter;
    if (runtime) {
        runtime->wifi_connected = g_app.wifi_connected;
        runtime->time_synced = g_app.time_synced;
        runtime->mqtt_connected = g_app.mqtt_connected;
        runtime->cold_contact_closed = g_app.cold_contact_closed;
        runtime->hot_contact_closed = g_app.hot_contact_closed;
        runtime->wifi_rssi = g_app.wifi_rssi;
        runtime->wifi_mode = g_app.wifi_mode;
        memcpy(runtime->wifi_ip, g_app.wifi_ip, sizeof(runtime->wifi_ip));
        memcpy(runtime->wifi_ssid, g_app.wifi_ssid, sizeof(runtime->wifi_ssid));
    }
    xSemaphoreGive(g_app.mutex);
}

void app_state_add_pulse(bool hot)
{
    uint32_t pulse_liters, ym;
    bool synced;
    if (xSemaphoreTake(g_app.mutex, portMAX_DELAY) != pdTRUE) return;
    pulse_liters = hot ? g_app.config.hot_pulse_liters : g_app.config.cold_pulse_liters;
    if (pulse_liters == 0U) pulse_liters = APP_DEFAULT_PULSE_LITERS;
    if (hot) g_app.meter.hot_total_liters += pulse_liters;
    else g_app.meter.cold_total_liters += pulse_liters;
    ym = app_current_year_month();
    synced = (ym != 0U);
    if (!synced) {
        if (hot) g_app.meter.unsynced_hot_liters += pulse_liters;
        else g_app.meter.unsynced_cold_liters += pulse_liters;
    } else {
        app_history_ensure_month_locked(ym);
        if (g_app.meter.history[APP_HISTORY_MONTHS - 1U].year_month == ym) {
            if (hot) g_app.meter.history[APP_HISTORY_MONTHS - 1U].hot_liters += pulse_liters;
            else g_app.meter.history[APP_HISTORY_MONTHS - 1U].cold_liters += pulse_liters;
        }
    }
    xSemaphoreGive(g_app.mutex);
}

void app_state_reconcile_time(void)
{
    uint32_t ym = app_current_year_month();
    if (ym == 0U || xSemaphoreTake(g_app.mutex, portMAX_DELAY) != pdTRUE) return;
    app_history_ensure_month_locked(ym);
    if (g_app.meter.unsynced_cold_liters || g_app.meter.unsynced_hot_liters) {
        water_month_t *m = &g_app.meter.history[APP_HISTORY_MONTHS - 1U];
        m->cold_liters += g_app.meter.unsynced_cold_liters;
        m->hot_liters += g_app.meter.unsynced_hot_liters;
        g_app.meter.unsynced_cold_liters = 0;
        g_app.meter.unsynced_hot_liters = 0;
    }
    g_app.time_synced = true;
    meter_data_t snapshot = g_app.meter;
    xSemaphoreGive(g_app.mutex);
    (void)nvs_storage_save_meter_if_changed(&snapshot);
}
