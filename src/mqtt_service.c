#include "mqtt_service.h"
#include "app_state.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "wifi_manager.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "mqtt_service";
static esp_mqtt_client_handle_t s_client;
static bool s_started;
static char s_mqtt_uri[192];
static char s_mqtt_username[APP_MQTT_USERNAME_MAX + 1U];
static char s_mqtt_password[APP_MQTT_PASSWORD_MAX + 1U];

static void mqtt_event(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args; (void)base; (void)event_data;
    if (event_id == MQTT_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "MQTT подключен");
        if (xSemaphoreTake(g_app.mutex, portMAX_DELAY) == pdTRUE) { g_app.mqtt_connected = true; xSemaphoreGive(g_app.mutex); }
    } else if (event_id == MQTT_EVENT_DISCONNECTED) {
        if (xSemaphoreTake(g_app.mutex, portMAX_DELAY) == pdTRUE) { g_app.mqtt_connected = false; xSemaphoreGive(g_app.mutex); }
        ESP_LOGW(TAG, "MQTT отключен");
    } else if (event_id == MQTT_EVENT_ERROR) {
        ESP_LOGE(TAG, "Ошибка MQTT клиента");
    }
}

static void publish_snapshot(void)
{
    app_config_t cfg;
    meter_data_t meter;
    app_state_t runtime = {0};
    char topic[APP_MQTT_TOPIC_MAX + 32U];
    char *json;
    cJSON *root;
    uint64_t cold_month = 0, hot_month = 0;
    size_t i;
    app_state_snapshot(&cfg, &meter, &runtime);
    if (!cfg.mqtt_enabled || !runtime.mqtt_connected || !s_client) return;
    for (i = 0; i < APP_HISTORY_MONTHS; ++i) if (meter.history[i].year_month == meter.active_year_month) {
        cold_month = meter.history[i].cold_liters; hot_month = meter.history[i].hot_liters; break;
    }
    root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddNumberToObject(root, "cold_liters", (double)meter.cold_total_liters);
    cJSON_AddNumberToObject(root, "hot_liters", (double)meter.hot_total_liters);
    cJSON_AddNumberToObject(root, "cold_m3", app_liters_to_m3(meter.cold_total_liters));
    cJSON_AddNumberToObject(root, "hot_m3", app_liters_to_m3(meter.hot_total_liters));
    cJSON_AddNumberToObject(root, "cold_month_liters", (double)cold_month);
    cJSON_AddNumberToObject(root, "hot_month_liters", (double)hot_month);
    cJSON_AddNumberToObject(root, "rssi", runtime.wifi_rssi);
    cJSON_AddBoolToObject(root, "cold_contact_closed", runtime.cold_contact_closed);
    cJSON_AddBoolToObject(root, "hot_contact_closed", runtime.hot_contact_closed);
    cJSON_AddBoolToObject(root, "time_synced", runtime.time_synced);
    json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return;
    snprintf(topic, sizeof(topic), "%s/cold", cfg.mqtt_base_topic);
    char value[64]; snprintf(value, sizeof(value), "%.3f", app_liters_to_m3(meter.cold_total_liters));
    (void)esp_mqtt_client_publish(s_client, topic, value, 0, 1, 1);
    snprintf(topic, sizeof(topic), "%s/hot", cfg.mqtt_base_topic);
    snprintf(value, sizeof(value), "%.3f", app_liters_to_m3(meter.hot_total_liters));
    (void)esp_mqtt_client_publish(s_client, topic, value, 0, 1, 1);
    snprintf(topic, sizeof(topic), "%s/state", cfg.mqtt_base_topic);
    (void)esp_mqtt_client_publish(s_client, topic, json, 0, 1, 1);
    cJSON_free(json);
}

static esp_err_t start_client(const app_config_t *cfg)
{
    esp_mqtt_client_config_t mqtt_cfg = {0};
    snprintf(s_mqtt_uri, sizeof(s_mqtt_uri), "mqtt://%s:%u", cfg->mqtt_host, (unsigned)cfg->mqtt_port);
    strlcpy(s_mqtt_username, cfg->mqtt_username, sizeof(s_mqtt_username));
    strlcpy(s_mqtt_password, cfg->mqtt_password, sizeof(s_mqtt_password));
    mqtt_cfg.broker.address.uri = s_mqtt_uri;
    if (s_mqtt_username[0]) mqtt_cfg.credentials.username = s_mqtt_username;
    if (s_mqtt_password[0]) mqtt_cfg.credentials.authentication.password = s_mqtt_password;
    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_client) return ESP_FAIL;
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event, NULL));
    esp_err_t err = esp_mqtt_client_start(s_client);
    if (err == ESP_OK) s_started = true;
    return err;
}

static bool mqtt_config_same(const app_config_t *a, const app_config_t *b)
{
    return a->mqtt_enabled == b->mqtt_enabled && a->mqtt_port == b->mqtt_port &&
        strcmp(a->mqtt_host, b->mqtt_host) == 0 && strcmp(a->mqtt_username, b->mqtt_username) == 0 &&
        strcmp(a->mqtt_password, b->mqtt_password) == 0 && strcmp(a->mqtt_base_topic, b->mqtt_base_topic) == 0;
}

static void mqtt_task(void *arg)
{
    (void)arg;
    app_config_t previous = {0};
    bool have_previous = false;
    uint64_t last_cold = UINT64_MAX, last_hot = UINT64_MAX;
    TickType_t last_periodic = 0;
    for (;;) {
        app_config_t cfg;
        meter_data_t meter;
        app_state_t runtime = {0};
        app_state_snapshot(&cfg, &meter, &runtime);
        if (have_previous && !mqtt_config_same(&cfg, &previous) && s_client) {
            (void)esp_mqtt_client_stop(s_client);
            (void)esp_mqtt_client_destroy(s_client);
            s_client = NULL;
            s_started = false;
            if (xSemaphoreTake(g_app.mutex, portMAX_DELAY) == pdTRUE) { g_app.mqtt_connected = false; xSemaphoreGive(g_app.mutex); }
        }
        previous = cfg;
        have_previous = true;
        if (cfg.mqtt_enabled && cfg.mqtt_host[0] && wifi_manager_is_connected()) {
            if (!s_started) {
                esp_err_t err = start_client(&cfg);
                if (err != ESP_OK) ESP_LOGE(TAG, "Не удалось запустить MQTT: %s", esp_err_to_name(err));
            }
            bool changed = meter.cold_total_liters != last_cold || meter.hot_total_liters != last_hot;
            bool periodic = (xTaskGetTickCount() - last_periodic) >= pdMS_TO_TICKS(60000U);
            if (s_started && runtime.mqtt_connected && (changed || periodic)) {
                publish_snapshot();
                last_cold = meter.cold_total_liters;
                last_hot = meter.hot_total_liters;
                last_periodic = xTaskGetTickCount();
            }
        } else if (!cfg.mqtt_enabled && s_client) {
            (void)esp_mqtt_client_stop(s_client);
            (void)esp_mqtt_client_destroy(s_client);
            s_client = NULL; s_started = false;
        }
        vTaskDelay(pdMS_TO_TICKS(2000U));
    }
}

esp_err_t mqtt_service_init(void)
{
    if (xTaskCreate(mqtt_task, "mqtt_service", 4096, NULL, 4, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}
