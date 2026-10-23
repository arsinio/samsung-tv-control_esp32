#include "wifi.h"

#include <string.h>

#include "config.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "eth_dhcps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "mdns.h"
#include "secrets.h"
#include "status.h"

static const char *TAG = "wifi";

#define RETRY_MIN_MS 1000
#define RETRY_MAX_MS 30000

static TimerHandle_t s_retry_timer;
static uint32_t s_retry_ms = RETRY_MIN_MS;

static void retry_connect(TimerHandle_t t)
{
    esp_wifi_connect();
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *ev = data;
        ESP_LOGW(TAG, "disconnected (reason %d); retrying in %lu ms", ev->reason, (unsigned long)s_retry_ms);
        status_t *st = status_lock();
        st->wifi_connected = false;
        st->wifi_ip[0] = '\0';
        status_unlock();
        eth_dhcps_wifi_lost();
        xTimerChangePeriod(s_retry_timer, pdMS_TO_TICKS(s_retry_ms), 0);
        s_retry_ms = s_retry_ms * 2 > RETRY_MAX_MS ? RETRY_MAX_MS : s_retry_ms * 2;
    }
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    ip_event_got_ip_t *ev = data;
    ESP_LOGI(TAG, "got " IPSTR " (http://" MDNS_HOSTNAME ".local/)", IP2STR(&ev->ip_info.ip));
    s_retry_ms = RETRY_MIN_MS;

    status_t *st = status_lock();
    st->wifi_connected = true;
    snprintf(st->wifi_ip, sizeof(st->wifi_ip), IPSTR, IP2STR(&ev->ip_info.ip));
    status_unlock();

    eth_dhcps_set_wifi_subnet(ev->ip_info.ip, ev->ip_info.netmask);
}

static void start_mdns(void)
{
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set(MDNS_HOSTNAME));
    ESP_ERROR_CHECK(mdns_instance_name_set(MDNS_INSTANCE));
    ESP_ERROR_CHECK(mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0));
}

void wifi_start(void)
{
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(netif, MDNS_HOSTNAME);

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));

    s_retry_timer = xTimerCreate("wifi_retry", pdMS_TO_TICKS(RETRY_MIN_MS), pdFALSE, NULL, retry_connect);
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL));

    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, WIFI_SSID, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, WIFI_PASSWORD, sizeof(cfg.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));  // snappier API responses
    ESP_ERROR_CHECK(esp_wifi_start());

    start_mdns();
}
