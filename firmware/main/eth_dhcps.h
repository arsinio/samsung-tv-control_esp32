// Ethernet (LAN8720) bring-up and the DHCP server for the isolated TV link.
#pragma once

#include "esp_err.h"
#include "esp_netif_ip_addr.h"

void eth_dhcps_start(void);

// Called with the Wi-Fi address once it's known; blocks serving if the Wi-Fi
// subnet overlaps the TV link.
void eth_dhcps_set_wifi_subnet(esp_ip4_addr_t ip, esp_ip4_addr_t netmask);
// Called when Wi-Fi disconnects; clears any subnet conflict.
void eth_dhcps_wifi_lost(void);

// Sends Wake-on-LAN magic packets to the TV's last known MAC.
// ESP_ERR_INVALID_STATE if no MAC is known yet (the TV never took a lease).
esp_err_t eth_dhcps_send_wol(void);
