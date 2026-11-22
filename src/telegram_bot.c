#include "telegram_bot.h"
#include "app_state.h"
#include "nvs_storage.h"
#include "wifi_manager.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "telegram_bot";

static esp_err_t send_message(const char *message)
{
    app_config_t cfg;
    app_state_snapshot(&cfg, NULL, NULL);
    if (!cfg.telegram_enabled || !cfg.telegram_bot_token[0] || !cfg.telegram_chat_id[0]) return ESP_ERR_INVALID_STATE;
    char url[256];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", cfg.telegram_bot_token);
    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(root, "chat_id", cfg.telegram_chat_id);
    cJSON_AddStringToObject(root, "text", message);
    cJSON_AddStringToObject(root, "parse_mode", "HTML");
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return ESP_ERR_NO_MEM;
    esp_http_client_config_t http_cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) { cJSON_free(body); return ESP_FAIL; }
    esp_err_t err = esp_http_client_set_header(client, "Content-Type", "application/json; charset=utf-8");
    if (err == ESP_OK) err = esp_http_client_set_post_field(client, body, (int)strlen(body));
    if (err == ESP_OK) err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if (status < 200 || status >= 300) err = ESP_FAIL;
        ESP_LOGI(TAG, "Telegram HTTP status=%d", status);
    }
    esp_http_client_cleanup(client);
    cJSON_free(body);
    return err;
}

static void make_report(char *out, size_t out_size, bool test)
{
    app_config_t cfg;
    meter_data_t meter;
    size_t i;
    uint64_t cold_month = 0, hot_month = 0;
    app_state_snapshot(&cfg, &meter, NULL);
    for (i = 0; i < APP_HISTORY_MONTHS; ++i) if (meter.history[i].year_month == meter.active_year_month) {
        cold_month = meter.history[i].cold_liters; hot_month = meter.history[i].hot_liters; break;
    }
    snprintf(out, out_size,
        "%s<b>WaterMeter</b>\nХВС: <b>%.3f м³</b>\nГВС: <b>%.3f м³</b>\n\n"
        "Расход за месяц (%u):\nХВС: %.3f м³\nГВС: %.3f м³\n"
        "Несинхронизированный объем: ХВС %.3f л, ГВС %.3f л",
        test ? "🧪 Тестовое сообщение\n" : "📊 Ежемесячный отчет\n",
        app_liters_to_m3(meter.cold_total_liters), app_liters_to_m3(meter.hot_total_liters),
        (unsigned)meter.active_year_month, app_liters_to_m3(cold_month), app_liters_to_m3(hot_month),
        (double)meter.unsynced_cold_liters, (double)meter.unsynced_hot_liters);
}

esp_err_t telegram_bot_send_report(void)
{
    char message[768];
    make_report(message, sizeof(message), false);
    return send_message(message);
}

esp_err_t telegram_bot_send_test(void)
{
    char message[768];
    make_report(message, sizeof(message), true);
    return send_message(message);
}

static uint32_t period_seconds(telegram_period_t period)
{
    switch (period) {
        case TELEGRAM_PERIOD_24_HOURS: return 86400U;
        case TELEGRAM_PERIOD_7_DAYS: return 604800U;
        case TELEGRAM_PERIOD_30_DAYS: return 2592000U;
        default: return 0U;
    }
}

static void telegram_task(void *arg)
{
    (void)arg;
    for (;;) {
        app_config_t cfg;
        app_state_snapshot(&cfg, NULL, NULL);
        time_t now = time(NULL);
        struct tm local_tm;
        if (cfg.telegram_enabled && wifi_manager_is_connected() && now > 1704067200 && localtime_r(&now, &local_tm)) {
            uint32_t ym = (uint32_t)(local_tm.tm_year + 1900) * 100U + (uint32_t)(local_tm.tm_mon + 1);
            bool monthly_due = cfg.monthly_21 && local_tm.tm_mday == 21 &&
                (local_tm.tm_hour > cfg.monthly_21_hour ||
                 (local_tm.tm_hour == cfg.monthly_21_hour && local_tm.tm_min >= cfg.monthly_21_minute)) &&
                cfg.last_monthly_ym != (int32_t)ym;
            uint32_t interval = period_seconds(cfg.telegram_period);
            bool periodic_due = interval > 0U &&
                (cfg.last_telegram_send_epoch == 0 || now - cfg.last_telegram_send_epoch >= (time_t)interval);
            if (monthly_due || periodic_due) {
                if (telegram_bot_send_report() == ESP_OK) {
                    app_config_t updated;
                    app_state_snapshot(&updated, NULL, NULL);
                    updated.last_telegram_send_epoch = (int64_t)now;
                    if (monthly_due) updated.last_monthly_ym = (int32_t)ym;
                    if (xSemaphoreTake(g_app.mutex, portMAX_DELAY) == pdTRUE) {
                        g_app.config = updated;
                        xSemaphoreGive(g_app.mutex);
                    }
                    (void)nvs_storage_save_config_if_changed(&updated);
                } else ESP_LOGW(TAG, "Отправка отчета Telegram не удалась");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(30000U));
    }
}

esp_err_t telegram_bot_init(void)
{
    if (xTaskCreate(telegram_task, "telegram_schedule", 8192, NULL, 4, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}
