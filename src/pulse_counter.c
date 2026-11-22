#include "pulse_counter.h"
#include "app_state.h"
#include "nvs_storage.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "esp_intr_alloc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include <stdbool.h>

#define GPIO_COLD GPIO_NUM_3
#define GPIO_HOT  GPIO_NUM_4
#define GPIO_LED  GPIO_NUM_8
#define GPIO_BOOT GPIO_NUM_9
#define DEBOUNCE_MS 80U
#define RELEASE_MS 100U
#define SAVE_DELAY_MS 15000U
static const char *TAG = "pulse_counter";
static TaskHandle_t s_cold_task, s_hot_task, s_led_task;
static TimerHandle_t s_save_timer;
static volatile bool s_pulse_flash;
static volatile TickType_t s_flash_until;

typedef enum { CH_IDLE, CH_DEBOUNCE, CH_WAIT_RELEASE } channel_state_t;
typedef struct { gpio_num_t pin; bool hot; TaskHandle_t task; } channel_arg_t;
static channel_arg_t s_cold_arg = { GPIO_COLD, false, NULL };
static channel_arg_t s_hot_arg = { GPIO_HOT, true, NULL };

static void IRAM_ATTR gpio_isr(void *arg)
{
    TaskHandle_t task = (TaskHandle_t)arg;
    BaseType_t wake = pdFALSE;
    if (task) vTaskNotifyGiveFromISR(task, &wake);
    if (wake == pdTRUE) portYIELD_FROM_ISR();
}

static void save_timer_cb(TimerHandle_t timer)
{
    (void)timer;
    meter_data_t meter;
    app_state_snapshot(NULL, &meter, NULL);
    esp_err_t err = nvs_storage_save_meter_if_changed(&meter);
    if (err != ESP_OK) ESP_LOGE(TAG, "Отложенное сохранение NVS: %s", esp_err_to_name(err));
}

static void channel_task(void *parameter)
{
    channel_arg_t *arg = (channel_arg_t *)parameter;
    channel_state_t state = CH_IDLE;
    TickType_t release_since = 0;
    for (;;) {
        if (state == CH_IDLE) {
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            state = CH_DEBOUNCE;
        } else if (state == CH_DEBOUNCE) {
            vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));
            if (gpio_get_level(arg->pin) == 0) {
                app_state_add_pulse(arg->hot);
                s_pulse_flash = true;
                s_flash_until = xTaskGetTickCount() + pdMS_TO_TICKS(50U);
                (void)xTimerReset(s_save_timer, 0);
                state = CH_WAIT_RELEASE;
                release_since = 0;
            } else state = CH_IDLE;
        } else {
            if (gpio_get_level(arg->pin) == 1) {
                if (release_since == 0) release_since = xTaskGetTickCount();
                if ((xTaskGetTickCount() - release_since) >= pdMS_TO_TICKS(RELEASE_MS)) state = CH_IDLE;
            } else release_since = 0;
            vTaskDelay(pdMS_TO_TICKS(10U));
        }
    }
}

static void led_task(void *arg)
{
    (void)arg;
    bool led_on = false;
    TickType_t last_toggle = xTaskGetTickCount();
    for (;;) {
        app_state_t runtime = {0};
        app_state_snapshot(NULL, NULL, &runtime);
        TickType_t now = xTaskGetTickCount();
        if (s_pulse_flash && (int32_t)(now - s_flash_until) < 0) {
            gpio_set_level(GPIO_LED, 0);
        } else {
            s_pulse_flash = false;
            if (runtime.wifi_mode == 2 /* WIFI_MODE_AP */) {
                if ((now - last_toggle) >= pdMS_TO_TICKS(200U)) {
                    led_on = !led_on;
                    last_toggle = now;
                }
                gpio_set_level(GPIO_LED, led_on ? 0 : 1);
            } else {
                /* В STA — короткая вспышка 50 мс один раз в 3 секунды. */
                TickType_t phase = now - last_toggle;
                if (phase >= pdMS_TO_TICKS(3000U)) {
                    last_toggle = now;
                    phase = 0;
                }
                gpio_set_level(GPIO_LED, phase < pdMS_TO_TICKS(50U) ? 0 : 1);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10U));
    }
}

static void boot_button_task(void *arg)
{
    (void)arg;
    TickType_t pressed_at = 0;
    bool handled = false;
    for (;;) {
        if (gpio_get_level(GPIO_BOOT) == 0) {
            if (pressed_at == 0) pressed_at = xTaskGetTickCount();
            if (!handled && (xTaskGetTickCount() - pressed_at) >= pdMS_TO_TICKS(5000U)) {
                handled = true;
                ESP_LOGW(TAG, "BOOT удерживается более 5 секунд: очистка Wi-Fi и перезапуск");
                (void)nvs_storage_clear_wifi();
                vTaskDelay(pdMS_TO_TICKS(250U));
                esp_restart();
            }
        } else { pressed_at = 0; handled = false; }
        vTaskDelay(pdMS_TO_TICKS(50U));
    }
}

esp_err_t pulse_counter_init(void)
{
    gpio_config_t input = {
        .pin_bit_mask = (1ULL << GPIO_COLD) | (1ULL << GPIO_HOT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE
    };
    gpio_config_t led = { .pin_bit_mask = 1ULL << GPIO_LED, .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE };
    gpio_config_t boot = { .pin_bit_mask = 1ULL << GPIO_BOOT, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE };
    ESP_RETURN_ON_ERROR(gpio_config(&input), TAG, "GPIO счетчиков");
    ESP_RETURN_ON_ERROR(gpio_set_pull_mode(GPIO_COLD, GPIO_PULLUP_ONLY), TAG, "pullup cold");
    ESP_RETURN_ON_ERROR(gpio_set_pull_mode(GPIO_HOT, GPIO_PULLUP_ONLY), TAG, "pullup hot");
    ESP_RETURN_ON_ERROR(gpio_config(&led), TAG, "GPIO LED");
    ESP_RETURN_ON_ERROR(gpio_config(&boot), TAG, "GPIO BOOT");
    gpio_set_level(GPIO_LED, 1); /* Инверсный светодиод: выключен. */
    s_save_timer = xTimerCreate("nvs_save", pdMS_TO_TICKS(SAVE_DELAY_MS), pdFALSE, NULL, save_timer_cb);
    if (!s_save_timer) return ESP_ERR_NO_MEM;
    if (xTaskCreate(channel_task, "cold_pulse", 3072, &s_cold_arg, 10, &s_cold_task) != pdPASS) return ESP_ERR_NO_MEM;
    if (xTaskCreate(channel_task, "hot_pulse", 3072, &s_hot_arg, 10, &s_hot_task) != pdPASS) return ESP_ERR_NO_MEM;
    s_cold_arg.task = s_cold_task; s_hot_arg.task = s_hot_task;
    ESP_RETURN_ON_ERROR(gpio_install_isr_service(ESP_INTR_FLAG_IRAM), TAG, "ISR service");
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(GPIO_COLD, gpio_isr, s_cold_task), TAG, "ISR cold");
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(GPIO_HOT, gpio_isr, s_hot_task), TAG, "ISR hot");
    if (xTaskCreate(led_task, "status_led", 2048, NULL, 3, &s_led_task) != pdPASS) return ESP_ERR_NO_MEM;
    if (xTaskCreate(boot_button_task, "boot_button", 2048, NULL, 4, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "Счетчики запущены: ХВС GPIO3, ГВС GPIO4");
    return ESP_OK;
}
