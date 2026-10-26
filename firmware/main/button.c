// Momentary button between POWER_BUTTON_GPIO and GND (internal pull-up).
//
// A press counts once the pin has read low for BUTTON_DEBOUNCE_MS; it then has
// to read high for BUTTON_DEBOUNCE_MS before another press can count, so
// contact bounce on press or release never adds a second toggle. Works in every
// toggle mode: power isn't an input change.

#include "button.h"

#include "config.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tv.h"

static const char *TAG = "button";

static TaskHandle_t s_task;

static void IRAM_ATTR on_edge(void *arg)
{
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_task, &woken);
    portYIELD_FROM_ISR(woken);
}

static bool pressed(void)
{
    return gpio_get_level(POWER_BUTTON_GPIO) == 0;
}

// True if the pin holds `want` for BUTTON_DEBOUNCE_MS without an edge.
static bool stable(bool want)
{
    ulTaskNotifyTake(pdTRUE, 0);
    if (pressed() != want) {
        return false;
    }
    return ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS)) == 0 && pressed() == want;
}

static void button_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!stable(true)) {
            continue;  // bounce or a glitch, not a press
        }
        ESP_LOGI(TAG, "power pressed");
        tv_request_power(SRC_BUTTON);

        // Wait for a clean release before arming again
        while (!stable(false)) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS));
        }
    }
}

void button_start(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << POWER_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    xTaskCreate(button_task, "button", 3072, NULL, 6, &s_task);

    ESP_ERROR_CHECK(gpio_isr_handler_add(POWER_BUTTON_GPIO, on_edge, NULL));
}
