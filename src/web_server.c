#include "web_server.h"
#include "app_state.h"
#include "nvs_storage.h"
#include "telegram_bot.h"
#include "driver/gpio.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "web_server";
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");
static httpd_handle_t s_server;

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *text = cJSON_PrintUnformatted(root);
    if (!text) { cJSON_Delete(root); return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON allocation failed"); }
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    esp_err_t err = httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
    cJSON_free(text); cJSON_Delete(root); return err;
}

static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)index_html_start, index_html_end - index_html_start);
}

static esp_err_t status_get(httpd_req_t *req)
{
    app_config_t cfg; meter_data_t meter; app_state_t rt = {0};
    cJSON *root = cJSON_CreateObject();
    if (!root) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    app_state_snapshot(&cfg, &meter, &rt);
    rt.cold_contact_closed = gpio_get_level(GPIO_NUM_3) == 0;
    rt.hot_contact_closed = gpio_get_level(GPIO_NUM_4) == 0;
    uint64_t cold_month = 0, hot_month = 0;
    for (size_t i = 0; i < APP_HISTORY_MONTHS; ++i) if (meter.history[i].year_month == meter.active_year_month) {
        cold_month = meter.history[i].cold_liters; hot_month = meter.history[i].hot_liters; break;
    }
    cJSON_AddNumberToObject(root, "cold_liters", (double)meter.cold_total_liters);
    cJSON_AddNumberToObject(root, "hot_liters", (double)meter.hot_total_liters);
    cJSON_AddNumberToObject(root, "cold_m3", app_liters_to_m3(meter.cold_total_liters));
    cJSON_AddNumberToObject(root, "hot_m3", app_liters_to_m3(meter.hot_total_liters));
    cJSON_AddNumberToObject(root, "cold_month_m3", app_liters_to_m3(cold_month));
    cJSON_AddNumberToObject(root, "hot_month_m3", app_liters_to_m3(hot_month));
    cJSON_AddNumberToObject(root, "active_ym", meter.active_year_month);
    cJSON_AddNumberToObject(root, "unsynced_cold_l", (double)meter.unsynced_cold_liters);
    cJSON_AddNumberToObject(root, "unsynced_hot_l", (double)meter.unsynced_hot_liters);
    cJSON_AddBoolToObject(root, "unsynced", meter.unsynced_cold_liters || meter.unsynced_hot_liters);
    cJSON_AddBoolToObject(root, "cold_contact_closed", rt.cold_contact_closed);
    cJSON_AddBoolToObject(root, "hot_contact_closed", rt.hot_contact_closed);
    cJSON_AddBoolToObject(root, "wifi_connected", rt.wifi_connected);
    cJSON_AddStringToObject(root, "wifi_mode", rt.wifi_mode == WIFI_MODE_AP ? "AP" : "STA");
    cJSON_AddStringToObject(root, "wifi_ssid", rt.wifi_ssid);
    cJSON_AddStringToObject(root, "wifi_ip", rt.wifi_ip);
    cJSON_AddNumberToObject(root, "wifi_rssi", rt.wifi_rssi);
    cJSON_AddBoolToObject(root, "mqtt_enabled", cfg.mqtt_enabled);
    cJSON_AddBoolToObject(root, "mqtt_connected", rt.mqtt_connected);
    cJSON_AddBoolToObject(root, "time_synced", rt.time_synced);
    cJSON_AddNumberToObject(root, "uptime_s", (double)(esp_timer_get_time() / 1000000LL));
    return send_json(req, root);
}

static esp_err_t settings_get(httpd_req_t *req)
{
    app_config_t cfg;
    app_state_snapshot(&cfg, NULL, NULL);
    cJSON *root = cJSON_CreateObject();
    if (!root) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    cJSON_AddNumberToObject(root, "cold_pulse_liters", cfg.cold_pulse_liters);
    cJSON_AddNumberToObject(root, "hot_pulse_liters", cfg.hot_pulse_liters);
    cJSON_AddStringToObject(root, "wifi_ssid", cfg.wifi_ssid);
    cJSON_AddBoolToObject(root, "mqtt_enabled", cfg.mqtt_enabled);
    cJSON_AddStringToObject(root, "mqtt_host", cfg.mqtt_host);
    cJSON_AddNumberToObject(root, "mqtt_port", cfg.mqtt_port);
    cJSON_AddStringToObject(root, "mqtt_username", cfg.mqtt_username);
    cJSON_AddStringToObject(root, "mqtt_base_topic", cfg.mqtt_base_topic);
    cJSON_AddStringToObject(root, "timezone", cfg.timezone);
    cJSON_AddBoolToObject(root, "telegram_enabled", cfg.telegram_enabled);
    cJSON_AddStringToObject(root, "telegram_chat_id", cfg.telegram_chat_id);
    cJSON_AddNumberToObject(root, "telegram_period", cfg.telegram_period);
    cJSON_AddBoolToObject(root, "monthly_21", cfg.monthly_21);
    cJSON_AddNumberToObject(root, "monthly_21_hour", cfg.monthly_21_hour);
    cJSON_AddNumberToObject(root, "monthly_21_minute", cfg.monthly_21_minute);
    return send_json(req, root);
}

static esp_err_t history_get(httpd_req_t *req)
{
    meter_data_t meter; app_state_snapshot(NULL, &meter, NULL);
    cJSON *root = cJSON_CreateObject(); cJSON *array = cJSON_CreateArray();
    uint64_t cold_sum = 0, hot_sum = 0;
    if (!root || !array) { cJSON_Delete(root); cJSON_Delete(array); return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory"); }
    for (size_t i = 0; i < APP_HISTORY_MONTHS; ++i) {
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddNumberToObject(entry, "ym", meter.history[i].year_month);
        cJSON_AddNumberToObject(entry, "cold_l", (double)meter.history[i].cold_liters);
        cJSON_AddNumberToObject(entry, "hot_l", (double)meter.history[i].hot_liters);
        cJSON_AddNumberToObject(entry, "cold_m3", app_liters_to_m3(meter.history[i].cold_liters));
        cJSON_AddNumberToObject(entry, "hot_m3", app_liters_to_m3(meter.history[i].hot_liters));
        cJSON_AddItemToArray(array, entry);
        cold_sum += meter.history[i].cold_liters; hot_sum += meter.history[i].hot_liters;
    }
    cJSON_AddItemToObject(root, "months", array);
    cJSON_AddNumberToObject(root, "cold_total_m3", app_liters_to_m3(cold_sum));
    cJSON_AddNumberToObject(root, "hot_total_m3", app_liters_to_m3(hot_sum));
    cJSON_AddBoolToObject(root, "unsynced", meter.unsynced_cold_liters || meter.unsynced_hot_liters);
    return send_json(req, root);
}

static bool read_body(httpd_req_t *req, char **out)
{
    if (req->content_len <= 0 || req->content_len > 8192) return false;
    char *buf = calloc(1, (size_t)req->content_len + 1U);
    if (!buf) return false;
    int received = 0;
    while (received < req->content_len) {
        int n = httpd_req_recv(req, buf + received, req->content_len - received);
        if (n <= 0) { free(buf); return false; }
        received += n;
    }
    *out = buf; return true;
}

static cJSON *obj(cJSON *root, const char *key) { return cJSON_GetObjectItemCaseSensitive(root, key); }
static void set_string(cJSON *root, const char *key, char *dst, size_t size, bool preserve_empty)
{
    cJSON *v = obj(root, key);
    if (cJSON_IsString(v) && v->valuestring && (!preserve_empty || v->valuestring[0])) strlcpy(dst, v->valuestring, size);
}
static void set_bool(cJSON *root, const char *key, bool *dst)
{
    cJSON *v = obj(root, key); if (cJSON_IsBool(v)) *dst = cJSON_IsTrue(v);
}
static void set_uint(cJSON *root, const char *key, uint32_t *dst, uint32_t min, uint32_t max)
{
    cJSON *v = obj(root, key); if (cJSON_IsNumber(v) && v->valuedouble >= min && v->valuedouble <= max) *dst = (uint32_t)v->valuedouble;
}

static esp_err_t settings_post(httpd_req_t *req)
{
    char *body = NULL;
    if (!read_body(req, &body)) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
    cJSON *root = cJSON_Parse(body); free(body);
    if (!root) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON");
    app_config_t cfg; meter_data_t meter; app_state_snapshot(&cfg, &meter, NULL);
    char old_ssid[sizeof(cfg.wifi_ssid)];
    char old_wifi_password[sizeof(cfg.wifi_password)];
    strlcpy(old_ssid, cfg.wifi_ssid, sizeof(old_ssid));
    strlcpy(old_wifi_password, cfg.wifi_password, sizeof(old_wifi_password));
    set_uint(root, "cold_pulse_liters", &cfg.cold_pulse_liters, 1U, 100000U);
    set_uint(root, "hot_pulse_liters", &cfg.hot_pulse_liters, 1U, 100000U);
    set_string(root, "wifi_ssid", cfg.wifi_ssid, sizeof(cfg.wifi_ssid), false);
    set_string(root, "wifi_password", cfg.wifi_password, sizeof(cfg.wifi_password), true);
    set_bool(root, "mqtt_enabled", &cfg.mqtt_enabled);
    set_string(root, "mqtt_host", cfg.mqtt_host, sizeof(cfg.mqtt_host), false);
    uint32_t port = cfg.mqtt_port; set_uint(root, "mqtt_port", &port, 1U, 65535U); cfg.mqtt_port = (uint16_t)port;
    set_string(root, "mqtt_username", cfg.mqtt_username, sizeof(cfg.mqtt_username), false);
    set_string(root, "mqtt_password", cfg.mqtt_password, sizeof(cfg.mqtt_password), true);
    set_string(root, "mqtt_base_topic", cfg.mqtt_base_topic, sizeof(cfg.mqtt_base_topic), false);
    set_string(root, "timezone", cfg.timezone, sizeof(cfg.timezone), false);
    set_bool(root, "telegram_enabled", &cfg.telegram_enabled);
    set_string(root, "telegram_bot_token", cfg.telegram_bot_token, sizeof(cfg.telegram_bot_token), true);
    set_string(root, "telegram_chat_id", cfg.telegram_chat_id, sizeof(cfg.telegram_chat_id), false);
    uint32_t period = (uint32_t)cfg.telegram_period; set_uint(root, "telegram_period", &period, 0U, 3U); cfg.telegram_period = (telegram_period_t)period;
    set_bool(root, "monthly_21", &cfg.monthly_21);
    uint32_t hour = cfg.monthly_21_hour, minute = cfg.monthly_21_minute;
    set_uint(root, "monthly_21_hour", &hour, 0U, 23U); set_uint(root, "monthly_21_minute", &minute, 0U, 59U);
    cfg.monthly_21_hour = (uint8_t)hour; cfg.monthly_21_minute = (uint8_t)minute;
    cJSON *cold = obj(root, "cold_m3"), *hot = obj(root, "hot_m3");
    bool meter_changed = false;
    if (cJSON_IsNumber(cold) && cold->valuedouble >= 0.0 && cold->valuedouble < 1.0e12) {
        uint64_t value = (uint64_t)(cold->valuedouble * 1000.0 + 0.5);
        if (value != meter.cold_total_liters) { meter.cold_total_liters = value; meter_changed = true; }
    }
    if (cJSON_IsNumber(hot) && hot->valuedouble >= 0.0 && hot->valuedouble < 1.0e12) {
        uint64_t value = (uint64_t)(hot->valuedouble * 1000.0 + 0.5);
        if (value != meter.hot_total_liters) { meter.hot_total_liters = value; meter_changed = true; }
    }
    cJSON_Delete(root);
    if (xSemaphoreTake(g_app.mutex, portMAX_DELAY) == pdTRUE) { g_app.config = cfg; if (meter_changed) g_app.meter = meter; xSemaphoreGive(g_app.mutex); }
    setenv("TZ", cfg.timezone[0] ? cfg.timezone : APP_DEFAULT_TIMEZONE, 1);
    tzset();
    esp_err_t err = nvs_storage_save_config_if_changed(&cfg);
    if (err == ESP_OK && meter_changed) err = nvs_storage_save_meter_now(&meter);
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"reboot\":false}");
    if (strcmp(old_ssid, cfg.wifi_ssid) != 0 || strcmp(old_wifi_password, cfg.wifi_password) != 0) {
        ESP_LOGW(TAG, "Изменены параметры Wi-Fi; перезапуск для применения настроек");
        vTaskDelay(pdMS_TO_TICKS(500)); esp_restart();
    }
    return ESP_OK;
}

static esp_err_t telegram_test_post(httpd_req_t *req)
{
    esp_err_t err = telegram_bot_send_test();
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    httpd_resp_set_type(req, "application/json"); return httpd_resp_sendstr(req, "{\"ok\":true}");
}

esp_err_t web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 12;
    config.stack_size = 12288;
    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &config), TAG, "httpd start");
    const httpd_uri_t routes[] = {
        { .uri = "/", .method = HTTP_GET, .handler = root_get },
        { .uri = "/api/status", .method = HTTP_GET, .handler = status_get },
        { .uri = "/api/history", .method = HTTP_GET, .handler = history_get },
        { .uri = "/api/settings", .method = HTTP_GET, .handler = settings_get },
        { .uri = "/api/settings", .method = HTTP_POST, .handler = settings_post },
        { .uri = "/api/telegram/test", .method = HTTP_POST, .handler = telegram_test_post }
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &routes[i]), TAG, "register URI");
    ESP_LOGI(TAG, "Веб-интерфейс доступен по HTTP");
    return ESP_OK;
}
