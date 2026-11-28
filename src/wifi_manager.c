#include "wifi_manager.h"
#include "app_state.h"
#include "nvs_storage.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <stdio.h>

static const char *TAG = "wifi_manager";
static EventGroupHandle_t s_wifi_events;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_RETRY_BIT BIT1
static bool s_sntp_started;
static volatile bool s_reset_backoff;

static void time_reconcile_task(void *arg)
{
    (void)arg;
    app_state_reconcile_time();
    ESP_LOGI(TAG, "Время синхронизировано по SNTP");
    vTaskDelete(NULL);
}

static void time_sync_cb(struct timeval *tv)
{
    (void)tv;
    /* Не выполняем запись Flash из callback SNTP: переносим работу в отдельную задачу. */
    if (xTaskCreate(time_reconcile_task, "time_reconcile", 3072, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Не удалось создать задачу обработки SNTP");
    }
}

static void start_sntp(void)
{
    app_config_t cfg;
    app_state_snapshot(&cfg, NULL, NULL);
    setenv("TZ", cfg.timezone[0] ? cfg.timezone : APP_DEFAULT_TIMEZONE, 1);
    tzset();
    if (!s_sntp_started) {
        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        config.sync_cb = time_sync_cb;
        esp_err_t err = esp_netif_sntp_init(&config);
        if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) s_sntp_started = true;
        else ESP_LOGE(TAG, "SNTP init: %s", esp_err_to_name(err));
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        xEventGroupSetBits(s_wifi_events, WIFI_RETRY_BIT);
        if (xSemaphoreTake(g_app.mutex, 0) == pdTRUE) {
            g_app.wifi_connected = false;
            g_app.wifi_ip[0] = '\0';
            xSemaphoreGive(g_app.mutex);
        }
        ESP_LOGW(TAG, "Wi-Fi отключен; повторное подключение по backoff");
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        char ip[16];
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&event->ip_info.ip));
        if (xSemaphoreTake(g_app.mutex, portMAX_DELAY) == pdTRUE) {
            g_app.wifi_connected = true;
            strlcpy(g_app.wifi_ip, ip, sizeof(g_app.wifi_ip));
            g_app.wifi_mode = WIFI_MODE_STA;
            xSemaphoreGive(g_app.mutex);
        }
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        xEventGroupClearBits(s_wifi_events, WIFI_RETRY_BIT);
        s_reset_backoff = true;
        start_sntp();
        ESP_LOGI(TAG, "Получен IP: %s", ip);
    }
}

static void reconnect_task(void *arg)
{
    (void)arg;
    uint32_t delay_ms = 2000U;
    for (;;) {
        xEventGroupWaitBits(s_wifi_events, WIFI_RETRY_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
        if (s_reset_backoff) { delay_ms = 2000U; s_reset_backoff = false; }
        if (xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT) {
            xEventGroupClearBits(s_wifi_events, WIFI_RETRY_BIT);
            delay_ms = 2000U;
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        if (!(xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT)) {
            esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
            delay_ms = delay_ms < 60000U ? delay_ms * 2U : 60000U;
            if (delay_ms > 60000U) delay_ms = 60000U;
        } else delay_ms = 2000U;
    }
}

esp_err_t wifi_manager_init(void)
{
    app_config_t cfg;
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t wifi_cfg;
    esp_netif_t *sta_netif;
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    s_wifi_events = xEventGroupCreate();
    if (!s_wifi_events) return ESP_ERR_NO_MEM;
    sta_netif = esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_ERROR(sta_netif ? ESP_OK : ESP_FAIL, TAG, "STA netif");
    ESP_RETURN_ON_ERROR(esp_netif_set_hostname(sta_netif, "watermeter_esp32_c3"), TAG, "STA hostname");
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL), TAG, "wifi handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL), TAG, "ip handler");
    app_state_snapshot(&cfg, NULL, NULL);
    if (cfg.wifi_ssid[0] == '\0') {
        esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
        if (!ap_netif) return ESP_FAIL;
        memset(&wifi_cfg, 0, sizeof(wifi_cfg));
        strlcpy((char *)wifi_cfg.ap.ssid, APP_DEFAULT_AP_SSID, sizeof(wifi_cfg.ap.ssid));
        strlcpy((char *)wifi_cfg.ap.password, APP_DEFAULT_AP_PASSWORD, sizeof(wifi_cfg.ap.password));
        wifi_cfg.ap.ssid_len = strlen(APP_DEFAULT_AP_SSID);
        wifi_cfg.ap.channel = 1;
        wifi_cfg.ap.max_connection = 4;
        wifi_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "set AP mode");
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &wifi_cfg), TAG, "set AP config");
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start AP");
        if (xSemaphoreTake(g_app.mutex, portMAX_DELAY) == pdTRUE) {
            g_app.wifi_mode = WIFI_MODE_AP;
            strlcpy(g_app.wifi_ip, "192.168.4.1", sizeof(g_app.wifi_ip));
            strlcpy(g_app.wifi_ssid, APP_DEFAULT_AP_SSID, sizeof(g_app.wifi_ssid));
            xSemaphoreGive(g_app.mutex);
        }
        ESP_LOGW(TAG, "Режим настройки AP: SSID=%s, IP=192.168.4.1", APP_DEFAULT_AP_SSID);
    } else {
        memset(&wifi_cfg, 0, sizeof(wifi_cfg));
        /* Поле SSID в esp-idf не обязано завершаться NUL: допустимы все 32 байта. */
        memcpy(wifi_cfg.sta.ssid, cfg.wifi_ssid, strnlen(cfg.wifi_ssid, sizeof(wifi_cfg.sta.ssid)));
        strlcpy((char *)wifi_cfg.sta.password, cfg.wifi_password, sizeof(wifi_cfg.sta.password));
        wifi_cfg.sta.threshold.authmode = cfg.wifi_password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set STA mode");
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg), TAG, "set STA config");
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start STA");
        if (xSemaphoreTake(g_app.mutex, portMAX_DELAY) == pdTRUE) {
            g_app.wifi_mode = WIFI_MODE_STA;
            strlcpy(g_app.wifi_ssid, cfg.wifi_ssid, sizeof(g_app.wifi_ssid));
            xSemaphoreGive(g_app.mutex);
        }
        ESP_RETURN_ON_ERROR(esp_wifi_connect(), TAG, "first connect");
        xEventGroupSetBits(s_wifi_events, WIFI_RETRY_BIT);
        if (xTaskCreate(reconnect_task, "wifi_retry", 3072, NULL, 5, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool wifi_manager_is_connected(void)
{
    return (s_wifi_events && (xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT));
}
