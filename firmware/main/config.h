// Board pins, network layout and TV timings.
#pragma once

// WT32-ETH01 Ethernet (LAN8720A over RMII, 50 MHz clock into GPIO0)
#define ETH_MDC_GPIO           23
#define ETH_MDIO_GPIO          18
#define ETH_PHY_ADDR           1
#define ETH_OSC_EN_GPIO        16  // enables the board's 50 MHz oscillator / PHY

// 3-position toggle: common to GND, throws to these pins (internal pull-ups)
#define SWITCH_HDMI1_GPIO      32
#define SWITCH_HDMI2_GPIO      33
#define SWITCH_DEBOUNCE_MS     50

// Momentary power button: to GND (internal pull-up)
#define POWER_BUTTON_GPIO      4
#define BUTTON_DEBOUNCE_MS     30

// Isolated TV link. The pool is TV_IP..TV_POOL_END: esp_netif rejects a
// one-address pool, and the server hands out the first address first.
#define TV_NET_DEVICE_IP       "192.168.77.1"
#define TV_NET_NETMASK         "255.255.255.0"
#define TV_IP                  "192.168.77.10"
#define TV_POOL_END            "192.168.77.11"
#define TV_LEASE_MINUTES       (12 * 60)

// Before serving, send DISCOVERs and listen for another DHCP server
#define DHCP_PROBE_COUNT       3
#define DHCP_PROBE_INTERVAL_MS 1000

// Wi-Fi side
#define MDNS_HOSTNAME          "monitor-control"
#define MDNS_INSTANCE          "Monitor Control"

// TV remote-control WebSocket (see docs/plans/01_experimentation/findings.md)
#define TV_WS_PORT             8002
#define TV_CLIENT_NAME         "monitor_control_esp32"
#define TV_CONNECT_TIMEOUT_MS  10000
#define TV_NETWORK_TIMEOUT_MS  5000    // per TCP/TLS step; keeps close-after-timeout short
#define TV_PAIR_TIMEOUT_MS     30000   // time to accept the Allow prompt on the TV
#define TV_RECONNECT_MS        30000   // background reconnect interval when paired
#define TV_REST_PORT           8001    // http://TV_IP:8001/api/v2/ reports PowerState
#define TV_POWER_QUERY_MS      2000
#define WOL_REPEAT             3       // magic packets per wake

// Source-menu macro: KEY_SOURCE, Up x MENU_MAX_ROWS, Down x N, Enter
#define MENU_MAX_ROWS          4
#define MENU_OPEN_DELAY_MS     1000
#define MENU_KEY_DELAY_MS      500

// Fallback macro: KEY_TV, then KEY_HDMI x N (passes through tuner static).
// Uncomment to use it instead of the Source menu.
// #define TV_USE_CYCLE_MACRO
#define CYCLE_TV_DELAY_MS      1500
#define CYCLE_HDMI_DELAY_MS    3000
