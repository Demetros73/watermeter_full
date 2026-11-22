#include "app_state.h"
#include "nvs_storage.h"
#include "pulse_counter.h"
#include "wifi_manager.h"
#include "mqtt_service.h"
#include "telegram_bot.h"
#include "web_server.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "watermeter_main";

static void diagnostics_task(void *arg)
{
    (void)arg;
    for (;;) {
        app_state_t runtime = {0};
        app_state_snapshot(NULL, NULL, &runtime);
        if (xSemaphoreTake(g_app.mutex, portMAX_DELAY) == pdTRUE) {
            g_app.cold_contact_closed = (gpio_get_level(GPIO_NUM_3) == 0);
            g_app.hot_contact_closed = (gpio_get_level(GPIO_NUM_4) == 0);
            xSemaphoreGive(g_app.mutex);
        }
        if (runtime.wifi_connected) {
            wifi_ap_record_t ap;
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                if (xSemaphoreTake(g_app.mutex, portMAX_DELAY) == pdTRUE) {
                    g_app.wifi_rssi = ap.rssi;
                    xSemaphoreGive(g_app.mutex);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10000U));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "WaterMeter %u.%u.%u — ESP32-C3 SuperMini",
             APP_VERSION_MAJOR, APP_VERSION_MINOR, APP_VERSION_PATCH);

    ESP_ERROR_CHECK(nvs_storage_init());
    ESP_ERROR_CHECK(app_state_init());

    /* Счетчики запускаются до сетевых сервисов: учет не зависит от Wi-Fi. */
    ESP_ERROR_CHECK(pulse_counter_init());
    ESP_ERROR_CHECK(wifi_manager_init());
    ESP_ERROR_CHECK(web_server_start());
    ESP_ERROR_CHECK(mqtt_service_init());
    ESP_ERROR_CHECK(telegram_bot_init());

    if (xTaskCreate(diagnostics_task, "diagnostics", 2048, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Не удалось создать задачу диагностики");
    }

    ESP_LOGI(TAG, "Запуск завершен. Веб-интерфейс: http://192.168.4.1 (режим AP) или IP устройства в сети STA");
}
