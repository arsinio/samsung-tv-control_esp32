// ON-OFF-ON toggle: common to GND, throws to SWITCH_HDMI1_GPIO / SWITCH_HDMI2_GPIO
// with internal pull-ups, so a closed throw reads low.
//
//   HDMI1 pin  HDMI2 pin  mode
//   low        high       hdmi1  (switch owns the TV)
//   high       high       api    (center)
//   high       low        hdmi2
//   low        low        fault  (locked, no action)
//
// Entering hdmi1/hdmi2 switches the TV; entering api or fault does nothing.
// At boot the mode is applied (lock) but no switch is sent.

#include "switch.h"

#include "config.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "status.h"
#include "tv.h"

static const char *TAG = "switch";

static TaskHandle_t s_task;

static ctl_mode_t read_mode(void)
{
    bool h1 = gpio_get_level(SWITCH_HDMI1_GPIO) == 0;
    bool h2 = gpio_get_level(SWITCH_HDMI2_GPIO) == 0;
    if (h1 && h2) {
        return MODE_FAULT;
    }
    return h1 ? MODE_HDMI1 : h2 ? MODE_HDMI2 : MODE_API;
}

static void IRAM_ATTR on_edge(void *arg)
{
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_task, &woken);
    portYIELD_FROM_ISR(woken);
}

static void set_mode(ctl_mode_t mode)
{
    status_lock()->mode = mode;
    status_unlock();
}

static void switch_task(void *arg)
{
    ctl_mode_t mode = read_mode();
    set_mode(mode);
    ESP_LOGI(TAG, "boot mode %s (no switch sent)", status_mode_name(mode));
    if (mode == MODE_FAULT) {
        ESP_LOGE(TAG, "both throws read closed; check the switch wiring");
    }

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        // Debounce: wait until the pins have been quiet for SWITCH_DEBOUNCE_MS
        while (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(SWITCH_DEBOUNCE_MS))) {
        }

        ctl_mode_t now = read_mode();
        if (now == mode) {
            continue;
        }
        ESP_LOGI(TAG, "mode %s -> %s", status_mode_name(mode), status_mode_name(now));
        mode = now;
        set_mode(mode);

        if (mode == MODE_HDMI1 || mode == MODE_HDMI2) {
            // Replaces any queued API request (latest wins)
            tv_request_input(mode == MODE_HDMI1 ? 1 : 2, SRC_SWITCH);
        } else if (mode == MODE_FAULT) {
            ESP_LOGE(TAG, "both throws read closed; check the switch wiring");
        }
    }
}

void switch_start(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << SWITCH_HDMI1_GPIO) | (1ULL << SWITCH_HDMI2_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    xTaskCreate(switch_task, "switch", 3072, NULL, 6, &s_task);

    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(SWITCH_HDMI1_GPIO, on_edge, NULL));
    ESP_ERROR_CHECK(gpio_isr_handler_add(SWITCH_HDMI2_GPIO, on_edge, NULL));
}
