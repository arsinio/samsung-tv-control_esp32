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
- **Not verifiable without the board** (as of 2026-10-23): WT32-ETH01 pins and oscillator enable,
  LAN8720 link, lease to the TV, Wi-Fi, mDNS, the WebSocket/TLS handshake from
  the ESP32, and switch debounce. See the bring-up checklist in
  `firmware/README.md`.

## On hardware (2026-10-26)

- **Flashing:** the WT32-ETH01's `TXD`/`RXD` pins (GPIO17/GPIO5) are the factory
  AT firmware's command port (`ETH01 AT Version 1.0.8`, which prints `Module OK`
  on boot), **not** UART0. Flashing and our log need **`TX0`/`RX0`**
  (GPIO1/GPIO3). With an auto-reset adapter (DTR→IO0, RTS→EN) wired to
  TX0/RX0, plain `./idf.sh -p … flash` works with esptool's default reset.
- Chip: ESP32-D0WD rev 1.1, Wi-Fi MAC `68:09:47:d2:05:64`, Ethernet MAC
  `68:09:47:d2:05:67`.
- **Ethernet + DHCP verified.** The LAN8720 comes up with the GPIO16 oscillator
  enable and the external clock on GPIO0, using the pins in `config.h` as-is.
  Link-up arrives about 1.5 s after boot. The probe sees no other server.
  Serving starts 3 s later, and the TV (`28:07:08:6b:0f:cd`) takes
  `192.168.77.10` about 1 s after that.
- **DHCP start order checked:** on this board, the netif glue starts the
  server on `ETHERNET_EVENT_START` (logged `DHCP server started on interface
  ETH_TV`), and our handler stops it in the same event, before link-up. Link-up
  logs `held for probe (was stopped)`. Link-up now also stops the server
  unconditionally, in case the glue ever starts it again.
- Wi-Fi with placeholder credentials: reason 201 (no AP found), retrying with
  backoff as designed.
- **TLS WebSocket and pairing verified from the ESP32.** The first flip, while
  unpaired, opened `wss://192.168.77.10:8002` in about 0.45 s. The Allow prompt
  was accepted about 4 s later, and the token was saved to NVS (`paired; token
  saved`). The default mbedTLS config negotiates TLS 1.2 with the TV fine.
- **Toggle verified:** api → hdmi1 → api → hdmi2 decoded cleanly with no
  bounce. The first switch took 8.1 s, including the pairing wait. The second
  took 4.0 s, reusing the open connection with no new handshake: 1 s menu open,
  then 0.5 s per key, matching the Source-menu timing from the PoC.
- **Wi-Fi + API verified** on the home Wi-Fi (192.168.1.214, RSSI -52, no subnet
  conflict). The pairing token survived a reflash, since NVS isn't erased by `flash`.
- **mDNS AAAA stall, fixed.** The board answered only A queries, so every
  `.local` lookup from macOS (curl, and browsers too) waited about 5 s for the
  AAAA answer. `curl -4` was instant. The board now creates an IPv6 link-local
  address on Wi-Fi connect, so mDNS answers AAAA. Lookups take about 10 ms and
  requests about 30 ms, and clients now connect over `fe80::…`.
- **API switching:** `POST /api/input/2` then `/1` returned `202`, and both
  ran OK (4.0 s, 3.5 s). Raw keys took about 10 ms on the open connection.
  Validation works: bad input → `404`, malformed key → `400`, wrong method → `405`.
- **Lockout verified:** the lever went to `hdmi1`, and the TV switched in 3.5 s.
  `POST /api/input/1` and `/api/key/KEY_INFO` both returned
  `409 {"error":"locked","mode":"hdmi1"}`. Back at center, the mode returned to
  `api`.
- **Reboot survival verified:** after a reset the device stayed paired, read
  the lever without switching, served the TV its lease again (`.10` about 6 s
  after boot), and reconnected to the TV at about 31 s (the first background
  reconnect tick) with no prompt.
- **Not tested:** the rogue-server guard (it needs the Ethernet on a real
  LAN), and isolation from the TV side. It's guaranteed by config (no router
  offered, forwarding off), but nothing was probed from the TV.

## Power button (2026-10-26)

- Added a momentary button on **GPIO4** (to GND, internal pull-up) and
  `POST /api/power`. Both work in any toggle mode. The command reads
  `PowerState` from `:8001/api/v2/`. If the TV is on, it sends `KEY_POWER`.
  Otherwise it sends Wake-on-LAN: UDP broadcast to port 9, 3 packets, built as
  raw frames on the TV link. The TV's MAC is persisted in NVS from leases.
- **Powering the board from the TV's USB port doesn't work:** the TV cuts USB
  power in standby, and the board died silently right after `KEY_POWER`. With
  a separate 5 V supply, it's fine.
- **Verified via the API:**
  - `KEY_POWER`: TV off in 46 ms. `PowerState` read `standby` at once, then
    `off` (the REST endpoint stops answering) within about 35 s.
  - Wake-on-LAN from `off`: the TV re-requested its lease 1 s after the magic
    packets, and was `on` by the next 30 s check. The TV keeps its Ethernet
    link up in standby, which is what makes WOL work.
  - The TV MAC was loaded from NVS after a reboot.
- `tv.power` is refreshed on the 30 s idle tick. It reads `waking` right after
  a WOL. The background reconnect only runs when the TV is on.
- **Not verified:** the physical button's debounce. Nothing is wired to GPIO4
  yet; it uses the same press/release code path as the API command.
- The GPIO ISR service is now installed once in `app_main`, which removes an
  `E` "already installed" log line.
- **Switch to the lever's input after a wake.** Once the TV reports `on`
  and accepts the WebSocket, the board waits 1.5 s (`TV_WAKE_SETTLE_MS`) and
  runs the Source-menu macro for the lever's input. In `api` mode it does
  nothing. A command queued meanwhile wins. Verified: TV on HDMI 1 via the API,
  powered off, lever flipped to HDMI 2 while off (that switch failed cleanly as
  `unreachable` after 5 s), then woken. It came up on HDMI 1 and moved to
  HDMI 2: lease at +0.9 s, connected at +2.7 s, done at 10.6 s. Watched on the
  TV.
- **The socket is now closed after `KEY_POWER`.** The TV went dark without
  closing it, so the board still showed `connected` and would have "sent"
  later keys into nothing.
