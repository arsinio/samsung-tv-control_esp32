// Ethernet bring-up and DHCP server for the TV link.
//
// Uses the ESP-IDF DHCP server with a two-address pool starting at TV_IP (the
// first client gets TV_IP) and no gateway, so no router is offered. (It always offers its own address as DNS;
// nothing listens on port 53, so the TV just can't resolve names.)
//
// Like poc/dhcpd.py, it refuses to become a rogue DHCP server: on every link-up
// it probes with DISCOVERs before serving, and a hook on the raw receive path
// stops serving if any other DHCP server ever answers on the link.

#include "eth_dhcps.h"

#include <string.h>

#include "config.h"
#include "dhcpserver/dhcpserver.h"
#include "driver/gpio.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "status.h"

static const char *TAG = "eth";

#define EV_LINK_UP   BIT0
#define EV_LINK_DOWN BIT1
#define EV_ROGUE     BIT2
#define EV_RECHECK   BIT3

static esp_eth_handle_t s_eth;
static esp_netif_t *s_netif;
static TaskHandle_t s_task;
static uint8_t s_mac[6];
static esp_netif_ip_info_t s_ip_info;
static volatile bool s_rogue;
static volatile bool s_conflict;

// --- Raw frames ---------------------------------------------------------

#define ETH_HDR   14
#define IP_HDR    20
#define UDP_HDR   8
#define BOOTP_LEN 300
#define DHCP_MAGIC 0x63825363

static void put16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static uint16_t get16(const uint8_t *p) { return (p[0] << 8) | p[1]; }

static uint16_t ip_checksum(const uint8_t *p, int len)
{
    uint32_t sum = 0;
    for (int i = 0; i < len; i += 2) {
        sum += get16(p + i);
    }
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }
    return ~sum;
}

// True for a DHCP server reply (BOOTREPLY, UDP 67 -> 68) sent by someone else.
static bool is_foreign_dhcp_reply(const uint8_t *f, uint32_t len)
{
    if (len < ETH_HDR + IP_HDR + UDP_HDR + 240 || get16(f + 12) != 0x0800) {
        return false;
    }
    if (memcmp(f + 6, s_mac, 6) == 0) {
        return false;
    }
    const uint8_t *ip = f + ETH_HDR;
    int ihl = (ip[0] & 0x0f) * 4;
    if ((ip[0] >> 4) != 4 || ip[9] != 17 || len < ETH_HDR + ihl + UDP_HDR + 240) {
        return false;
    }
    const uint8_t *udp = ip + ihl;
    const uint8_t *bootp = udp + UDP_HDR;
    return get16(udp) == 67 && get16(udp + 2) == 68 && bootp[0] == 2;
}

static size_t build_discover(uint8_t *f, uint32_t xid)
{
    memset(f, 0, ETH_HDR + IP_HDR + UDP_HDR + BOOTP_LEN);

    memset(f, 0xff, 6);                 // broadcast
    memcpy(f + 6, s_mac, 6);
    put16(f + 12, 0x0800);

    uint8_t *ip = f + ETH_HDR;
    ip[0] = 0x45;
    put16(ip + 2, IP_HDR + UDP_HDR + BOOTP_LEN);
    put16(ip + 4, esp_random());
    ip[8] = 64;
    ip[9] = 17;
    memset(ip + 16, 0xff, 4);           // 0.0.0.0 -> 255.255.255.255
    put16(ip + 10, ip_checksum(ip, IP_HDR));

    uint8_t *udp = ip + IP_HDR;
    put16(udp, 68);
    put16(udp + 2, 67);
    put16(udp + 4, UDP_HDR + BOOTP_LEN);  // checksum 0 = none

    uint8_t *b = udp + UDP_HDR;
    b[0] = 1;                           // BOOTREQUEST
    b[1] = 1;                           // Ethernet
    b[2] = 6;
    put32(b + 4, xid);
    put16(b + 10, 0x8000);              // ask for broadcast replies
    memcpy(b + 28, s_mac, 6);
    put32(b + 236, DHCP_MAGIC);
    uint8_t *o = b + 240;
    *o++ = 53; *o++ = 1; *o++ = 1;      // DISCOVER
    *o++ = 55; *o++ = 2; *o++ = 1; *o++ = 3;
    *o++ = 255;

    return ETH_HDR + IP_HDR + UDP_HDR + BOOTP_LEN;
}

// Replaces the netif glue's input path: inspect, then hand the frame to lwIP.
static esp_err_t eth_input(esp_eth_handle_t eth, uint8_t *buf, uint32_t len, void *priv)
{
    if (!s_rogue && is_foreign_dhcp_reply(buf, len)) {
        s_rogue = true;
        xTaskNotify(s_task, EV_ROGUE, eSetBits);
    }
    return esp_netif_receive((esp_netif_t *)priv, buf, len, NULL);
}

// --- Serving --------------------------------------------------------------

static void set_serving(bool serve)
{
    bool serving;
    status_t *st = status_lock();
    serving = st->dhcp_serving;
    status_unlock();
    if (serve == serving) {
        return;
    }

    esp_err_t err = serve ? esp_netif_dhcps_start(s_netif) : esp_netif_dhcps_stop(s_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED &&
        err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGE(TAG, "DHCP server %s failed: %s", serve ? "start" : "stop", esp_err_to_name(err));
        return;
    }
    // Start returns OK without starting if the netif went down meanwhile
    esp_netif_dhcp_status_t dhcps;
    if (serve && (esp_netif_dhcps_get_status(s_netif, &dhcps) != ESP_OK || dhcps != ESP_NETIF_DHCP_STARTED)) {
        ESP_LOGW(TAG, "DHCP server not started (link went down?)");
        return;
    }
    ESP_LOGI(TAG, "DHCP server %s", serve ? "serving " TV_IP : "stopped");

    status_lock()->dhcp_serving = serve;
    status_unlock();
}

static void probe_for_other_servers(void)
{
    static uint8_t frame[ETH_HDR + IP_HDR + UDP_HDR + BOOTP_LEN];
    uint32_t xid = esp_random();

    ESP_LOGI(TAG, "probing for other DHCP servers");
    for (int i = 0; i < DHCP_PROBE_COUNT && !s_rogue; i++) {
        size_t len = build_discover(frame, xid);
        esp_err_t err = esp_eth_transmit(s_eth, frame, len);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "probe transmit failed: %s", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(DHCP_PROBE_INTERVAL_MS));
    }
}

static void eth_task(void *arg)
{
    bool link = false;

    for (;;) {
        uint32_t ev = 0;
        xTaskNotifyWait(0, UINT32_MAX, &ev, portMAX_DELAY);

        if (ev & EV_LINK_DOWN) {
            link = false;
            s_rogue = false;
        }
        if (ev & EV_LINK_UP) {
            link = true;
            s_rogue = false;
            // Stop unconditionally: the netif glue may have started the server
            // behind our back (ETHERNET_EVENT_START), so the cached flag can't be trusted
            esp_netif_dhcp_status_t was = ESP_NETIF_DHCP_INIT;
            esp_netif_dhcps_get_status(s_netif, &was);
            esp_netif_dhcps_stop(s_netif);
            status_lock()->dhcp_serving = false;
            status_unlock();
            ESP_LOGI(TAG, "DHCP server held for probe (was %s)",
                     was == ESP_NETIF_DHCP_STARTED ? "started" : was == ESP_NETIF_DHCP_STOPPED ? "stopped" : "init");
            probe_for_other_servers();
        }

        if (s_rogue) {
            ESP_LOGE(TAG, "another DHCP server answered on the TV link; not serving until the link drops");
        }

        status_t *st = status_lock();
        st->eth_link = link;
        st->dhcp_rogue = s_rogue;
        st->subnet_conflict = s_conflict;
        status_unlock();

        set_serving(link && !s_rogue && !s_conflict);
    }
}

// --- Events ---------------------------------------------------------------

static void on_eth_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case ETHERNET_EVENT_START:
        // Keep the DHCP server stopped until the probe passes
        esp_netif_dhcps_stop(s_netif);
        break;
    case ETHERNET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "link up");
        xTaskNotify(s_task, EV_LINK_UP, eSetBits);
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "link down");
        xTaskNotify(s_task, EV_LINK_DOWN, eSetBits);
        break;
    default:
        break;
    }
}

static void on_lease(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    ip_event_ap_staipassigned_t *ev = data;
    if (ev->esp_netif != s_netif) {
        return;
    }
    ESP_LOGI(TAG, "lease " IPSTR " -> " MACSTR, IP2STR(&ev->ip), MAC2STR(ev->mac));
    esp_ip4_addr_t tv_ip;
    esp_netif_str_to_ip4(TV_IP, &tv_ip);
    if (ev->ip.addr != tv_ip.addr) {
        ESP_LOGW(TAG, "lease is not " TV_IP "; the TV client won't find this device");
    }

    status_t *st = status_lock();
    st->tv_leased = true;
    memcpy(st->tv_mac, ev->mac, 6);
    status_unlock();
}

void eth_dhcps_set_wifi_subnet(esp_ip4_addr_t ip, esp_ip4_addr_t netmask)
{
    // Overlap if either network contains the other: compare under the shorter mask
    uint32_t mask = s_ip_info.netmask.addr & netmask.addr;
    bool conflict = (ip.addr & mask) == (s_ip_info.ip.addr & mask);
    if (conflict) {
        ESP_LOGE(TAG, "Wi-Fi subnet " IPSTR " overlaps the TV link; not serving", IP2STR(&ip));
    }
    if (conflict != s_conflict) {
        s_conflict = conflict;
        xTaskNotify(s_task, EV_RECHECK, eSetBits);
    }
}

void eth_dhcps_wifi_lost(void)
{
    if (s_conflict) {
        s_conflict = false;
        xTaskNotify(s_task, EV_RECHECK, eSetBits);
    }
}

// --- Init -----------------------------------------------------------------

static esp_netif_t *create_netif(void)
{
    esp_netif_str_to_ip4(TV_NET_DEVICE_IP, &s_ip_info.ip);
    esp_netif_str_to_ip4(TV_NET_NETMASK, &s_ip_info.netmask);
    s_ip_info.gw.addr = 0;  // no gateway, so no router option

    esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_ETH();
    base.flags = ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP;
    base.ip_info = &s_ip_info;
    base.get_ip_event = 0;
    base.lost_ip_event = 0;
    base.if_key = "ETH_TV";
    base.if_desc = "tv";
    base.route_prio = 10;  // below Wi-Fi, so the default route stays on Wi-Fi

    esp_netif_config_t cfg = {
        .base = &base,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };
    esp_netif_t *netif = esp_netif_new(&cfg);

    // Options can only be set while the server is stopped (it is, until start)
    dhcps_lease_t pool = {.enable = true};
    esp_netif_str_to_ip4(TV_IP, (esp_ip4_addr_t *)&pool.start_ip);
    esp_netif_str_to_ip4(TV_POOL_END, (esp_ip4_addr_t *)&pool.end_ip);
    ESP_ERROR_CHECK(esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET, ESP_NETIF_REQUESTED_IP_ADDRESS,
                                           &pool, sizeof(pool)));
    uint32_t lease_minutes = TV_LEASE_MINUTES;
    ESP_ERROR_CHECK(esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET, ESP_NETIF_IP_ADDRESS_LEASE_TIME,
                                           &lease_minutes, sizeof(lease_minutes)));
    dhcps_offer_t no_router = 0;
    ESP_ERROR_CHECK(esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET, ESP_NETIF_ROUTER_SOLICITATION_ADDRESS,
                                           &no_router, sizeof(no_router)));
    return netif;
}

static esp_eth_handle_t install_driver(void)
{
    // The WT32-ETH01's 50 MHz oscillator (RMII clock into GPIO0) is enabled by GPIO16
    gpio_reset_pin(ETH_OSC_EN_GPIO);
    gpio_set_direction(ETH_OSC_EN_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(ETH_OSC_EN_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_config.smi_mdc_gpio_num = ETH_MDC_GPIO;
    emac_config.smi_mdio_gpio_num = ETH_MDIO_GPIO;
    emac_config.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    emac_config.clock_config.rmii.clock_gpio = EMAC_CLK_IN_GPIO;
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = ETH_PHY_ADDR;
    phy_config.reset_gpio_num = -1;
    esp_eth_phy_t *phy = esp_eth_phy_new_lan87xx(&phy_config);

    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth = NULL;
    ESP_ERROR_CHECK(esp_eth_driver_install(&config, &eth));
    return eth;
}

void eth_dhcps_start(void)
{
    xTaskCreate(eth_task, "eth_dhcps", 4096, NULL, 5, &s_task);

    s_eth = install_driver();
    ESP_ERROR_CHECK(esp_eth_ioctl(s_eth, ETH_CMD_G_MAC_ADDR, s_mac));
    ESP_LOGI(TAG, "MAC " MACSTR ", TV link " TV_NET_DEVICE_IP "/" TV_NET_NETMASK, MAC2STR(s_mac));

    s_netif = create_netif();
    ESP_ERROR_CHECK(esp_netif_attach(s_netif, esp_eth_new_netif_glue(s_eth)));
    // After attach, so it replaces the glue's own input path
    ESP_ERROR_CHECK(esp_eth_update_input_path(s_eth, eth_input, s_netif));

    // ANY_ID handlers run before the glue's per-event ones. That's fine: on START
    // our stop leaves the server STOPPED, so the glue's esp_netif_start won't
    // start it; on CONNECTED the probe outlasts the glue bringing the netif up.
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_eth_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED, on_lease, NULL));

    ESP_ERROR_CHECK(esp_eth_start(s_eth));
}
