#ifndef TELEGRAM_BOT_H
#define TELEGRAM_BOT_H
#include "esp_err.h"
esp_err_t telegram_bot_init(void);
esp_err_t telegram_bot_send_test(void);
esp_err_t telegram_bot_send_report(void);
#endif
