// Monitor Control for the WT32-ETH01: DHCP server for the TV on Ethernet,
// REST API on Wi-Fi, a 3-position HDMI 1 / API / HDMI 2 toggle, and a power button.

#include "api.h"
#include "button.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "eth_dhcps.h"
#include "nvs_flash.h"
#include "status.h"
#include "switch.h"
#include "tv.h"
#include "wifi.h"

static const char *TAG = "main";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    status_init();
    ESP_ERROR_CHECK(gpio_install_isr_service(0));  // shared by the switch and button
    tv_start();
    // The switch decides the mode before anything can accept API requests
    switch_start();
    button_start();
    eth_dhcps_start();
    wifi_start();
    api_start();

    ESP_LOGI(TAG, "up");
}
