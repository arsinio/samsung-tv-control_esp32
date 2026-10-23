# 02 — ESP32 findings

Running log for [plan.md](plan.md). Newest entries at the bottom of each section.

## Before hardware (2026-10-23)

- **Firmware written and building.** `firmware/` builds cleanly against ESP-IDF
  v5.2 with no warnings in `main/`. The image is 1.05 MB in a 1.5 MB app
  partition. Components: `espressif/esp_websocket_client`, `espressif/mdns`
  (1.14.1); versions are pinned in `firmware/dependencies.lock`.
- **Toolchain quirk:** `export.sh` picks pyenv's Python 3.13, but the installed
  v5.2 env is Python 3.9. `firmware/idf.sh` sets `IDF_PYTHON_ENV_PATH` to work
  around it.
- **TLS risk retired.** The TV prefers TLS 1.3 (`TLS_AES_256_GCM_SHA384`), which
  v5.2's mbedTLS doesn't enable by default. The TV also accepts **TLS 1.2 with
  ECDHE-RSA-AES128/256-GCM and CHACHA20-POLY1305**, which mbedTLS supports. Its
  cert is RSA-2048, self-signed (Samsung SmartViewSDK Root CA G2), valid until 2072.
- **DHCP server choice:** uses the ESP-IDF DHCP server with no gateway, so no
  router is offered. It always offers **its own address as DNS**. Nothing
  listens on port 53, so that's harmless.
- **Rogue-server guard** is done the PoC way: a hook on the Ethernet receive
  path watches for DHCP replies from other MACs, and a raw DISCOVER probe runs on
  each link-up before serving. The frame code was host-tested against
  `poc/dhcpd.py`: the DISCOVER decodes with valid checksums and draws an OFFER,
  foreign OFFERs are detected, and own-MAC, DISCOVER, non-UDP and short frames
  are ignored.
- **Deviations from the plan:**
  - The `cycle` fallback is a compile-time switch (`TV_USE_CYCLE_MACRO` in
    `config.h`), not a runtime option.
  - Added `GET /api/log` (last 16 commands).
  - API requests arriving over the TV link get `403`.
  - The TV client only connects in the background once paired, so an unpaired
    device never pops the Allow prompt on its own. Pairing starts with
    `POST /api/pair` or the first switch.
- **Independent code review** (against the IDF v5.2 and component source) found:
  - **A boot loop:** `esp_netif_dhcps_option` rejects a pool with
    `start == end`, so the single-address pool aborted at boot. Now `.10`–`.11`:
    the server hands out `start_ip` first, and a warning is logged if a lease
    isn't `.10`.
  - **A transmit race:** raw probe frames could collide with lwIP on the
    EMAC, because `CONFIG_ETH_TRANSMIT_MUTEX` was off. Now on.
  - **Receive buffer:** 2 KB could drop a large `ms.channel.connect` (it lists
    every connected client), losing the token. Now 6 KB.
  - **Fragments:** continuation frames weren't reassembled. They're now
    appended, and only parsed when `fin` is set.
  - **Smaller fixes:**
    - `dhcp_serving` is confirmed with `esp_netif_dhcps_get_status`.
    - A subnet conflict clears when Wi-Fi drops.
    - The TCP/TLS step timeout is now 5 s, so a timed-out connect closes quickly.
    - The httpd stack is 6 KB.
    - The comment on event-handler ordering is corrected: ANY_ID handlers run
      before the glue's, and the sequence still works.
- **Not verifiable without the board:** WT32-ETH01 pins and oscillator enable,
  LAN8720 link, lease to the TV, Wi-Fi, mDNS, the WebSocket/TLS handshake from
  the ESP32, and switch debounce. See the bring-up checklist in
  `firmware/README.md`.
