# 02 — ESP32 controller (WT32-ETH01)

**Status:** Draft · **Date:** 2026-10-23 · **Owner:** Christopher Armenio
**Builds on:** [01_experimentation](../01_experimentation/plan.md) ([findings](../01_experimentation/findings.md))

## Goal

Move the PoC off the Mac onto a WT32-ETH01 (ESP32 + LAN8720 Ethernet) that:

1. **Serves DHCP to the TV** on an isolated Ethernet link: no router, no DNS, no forwarding.
2. **Joins the home Wi-Fi** and exposes an unauthenticated HTTP REST API, advertised over mDNS.
3. **Has a 3-position toggle (HDMI 1 / API / HDMI 2).** The outer positions force an input and lock out the API. The center gives control to the API.

### Decisions

| Topic | Decision |
|---|---|
| Firmware | ESP-IDF, C |
| Switch | ON-OFF-ON latching toggle; acts only on a change of position |
| API | HTTP REST, no auth, mDNS name `monitor-control.local` |
| Wi-Fi credentials | Compile-time, in a git-ignored file |

### Out of scope

- MQTT, HomeKit, any web UI beyond a bare status page
- OTA updates, a Wi-Fi setup portal
- TV power on/off (Wake-on-LAN), the Game Mode popup (see 01 findings)
- API authentication

## Carried over from the PoC

All of these were verified in phase 01:

- **TV:** QN43Q7FAAFXZA, wired MAC `28:07:08:6b:0f:cd`, BusyBox `udhcpc`.
  It accepts a lease with only a subnet mask.
- **Control:** `wss://<tv>:8002/api/v2/channels/samsung.remote.control?name=<b64 name>[&token=<t>]`,
  with a self-signed cert. The first connect shows an Allow prompt on the TV,
  and `ms.channel.connect` returns `data.token`. Keys are sent as
  `{"method":"ms.remote.control","params":{"Cmd":"Click","DataOfCmd":"KEY_…","Option":"false","TypeOfRemote":"SendRemoteKey"}}`.
- **Switching:** `KEY_HDMI1`/`KEY_HDMI2` are ignored. The working macro is
  `KEY_SOURCE`, wait 1.0 s, `KEY_UP` ×4, `KEY_DOWN` ×N, `KEY_ENTER`, with 0.5 s
  between keys. The menu doesn't wrap, and Left exits it, so the macro never
  presses Left. Fallback: `KEY_TV` + `KEY_HDMI` ×N.
- **No input readback:** the TV can't report its current input. The device
  knows only the last input it commanded.

## Hardware

| Item | Notes |
|---|---|
| WT32-ETH01 | ESP32-D0WD, LAN8720A PHY, no USB port |
| USB-TTL adapter, 3.3 V logic | TX→RX0, RX→TX0, GND. Hold IO0 low at reset to flash |
| 5 V supply | 5V pin, or 3V3 from a regulated supply |
| ON-OFF-ON toggle | Common → GND; throws → **GPIO32** (HDMI 1) and **GPIO33** (HDMI 2), internal pull-ups |
| Cat cable | WT32-ETH01 ↔ TV |

**WT32-ETH01 pins** (confirm against the board in Phase 0):
- Ethernet: RMII, 50 MHz clock into **GPIO0** from the on-board oscillator, PHY
  power **GPIO16**, MDC **GPIO23**, MDIO **GPIO18**, PHY address **1**.
- Avoid the strapping pins 0, 2, 12 and 15. GPIO35/36/39 are input-only with no
  pull-ups. GPIO32/33 are safe inputs with pull-ups.

## Architecture

```
          home Wi-Fi (DHCP client)                  isolated Ethernet (DHCP server)
  phone/laptop ──HTTP──►  ┌──────────────── WT32-ETH01 ─────────────────┐  ──wss:8002──►  TV
                          │ api.c  ──┐                                  │   192.168.77.10
  toggle (GPIO32/33) ────►│ switch.c ├─► cmd queue ─► tv.c (WebSocket)  │
                          │          │     (latest wins)   │ token: NVS │
                          │ wifi.c  mdns   eth_dhcps.c ◄───┘            │
                          └──────────────────────────────────────────────┘
                              192.168.77.1/24, lwIP IP forwarding OFF
```

- **One TV task owns the WebSocket.** The switch and the API post "go to input
  N" onto a queue. A running macro always finishes. A newer request replaces
  any queued one (latest wins).
- **The switch sets the mode.** In `hdmi1` and `hdmi2` the switch owns the TV.
  Entering one of these switches to that input, overriding any queued API
  request. API input and key requests return `409`. In `api` (center) the API
  owns the TV, and entering it changes nothing.
- **The connection stays open** between commands, so switches skip the TLS
  handshake. It reconnects with backoff while the TV is off or rebooting.
- **Subnet:** `192.168.77.0/24` by default (device `.1`, TV `.10`), and
  configurable. It avoids the PoC's `192.168.2.x`, which is macOS Internet
  Sharing's default. The DHCP server won't start if the Wi-Fi subnet overlaps it.

## Layout

```
firmware/
├── CMakeLists.txt
├── sdkconfig.defaults         # ETH, TLS-insecure, HTTP server, mDNS, forwarding off
├── main/
│   ├── CMakeLists.txt
│   ├── idf_component.yml      # espressif/esp_websocket_client, espressif/mdns
│   ├── app_main.c             # init order, NVS, event loop
│   ├── config.h               # pins, subnet, timings, mDNS name
│   ├── secrets.example.h      # copy to secrets.h (git-ignored): SSID/password
│   ├── eth_dhcps.c/.h         # LAN8720 bring-up, static IP, DHCP server
│   ├── wifi.c/.h              # STA, reconnect, mDNS
│   ├── tv.c/.h                # WebSocket client, pairing, token, key macros
│   ├── api.c/.h               # esp_http_server routes
│   └── switch.c/.h            # GPIO32/33 debounce → mode + queue
└── README.md                  # wiring, flashing, pairing
```

## API (v1)

| Method & path | Response | Notes |
|---|---|---|
| `GET /api/status` | `{"input":1\|2\|null,"mode":"hdmi1"\|"api"\|"hdmi2"\|"fault","tv":{"link":bool,"ip":"…","connected":bool,"paired":bool},"busy":bool}` | `input` is the last input commanded |
| `POST /api/input/1`, `/api/input/2` | `202 {"queued":N}` or `409 {"error":"locked","mode":"…"}` | `api` mode only; returns immediately, poll `busy` |
| `POST /api/pair` | `202` | Any mode. Drops the token and reconnects, so the TV shows the Allow prompt |
| `POST /api/key/{KEY_NAME}` | `202` or `409` | `api` mode only. Raw key, for debugging |
| `GET /` | HTML | Mode, status, and two buttons (disabled when locked); no JS frameworks |

---

## Phase 0 — Toolchain and board bring-up

- [ ] Use ESP-IDF v5.2, already installed at `~/esp/v5.2/esp-idf`. Move to
      v5.5 only if a bug forces it.
- [ ] Wire the USB-TTL adapter, flash `hello_world`, confirm the serial monitor.
- [ ] Record the board revision and confirm the Ethernet pins.

**Exit:** `idf.py flash monitor` flashes and boots the board.

## Phase 1 — Ethernet + DHCP server (replaces `poc/dhcpd.py`)

- Bring up the LAN8720 (`esp_eth_mac_new_esp32` + `esp_eth_phy_new_lan87xx`,
  external clock on GPIO0, power on GPIO16).
- Run the Ethernet `esp_netif` as a DHCP server on a static `192.168.77.1/24`.
  The pool is the single address `.10`, so the TV always gets `.10` without a
  MAC pin. Lease 12 h.
- Disable the router and DNS offers (`esp_netif_dhcps_option`), and keep
  `CONFIG_LWIP_IP_FORWARD=n`.
- On link-up, probe with a few DHCP DISCOVERs before serving. If any server
  answers, don't serve, and report it in status.
- Log leases (MAC, IP) over serial.

**Exit:**
- The TV gets `192.168.77.10` with no router or DNS, and keeps it across a TV reboot.
- On a home-LAN switch, the device refuses to serve.

## Phase 2 — Wi-Fi, mDNS, REST skeleton

- Wi-Fi STA from `secrets.h`, reconnecting with backoff. Ethernet works without Wi-Fi.
- mDNS `monitor-control.local` with an `_http._tcp` service.
- `esp_http_server` with `GET /api/status` (TV fields stubbed) and `GET /`.
- The Wi-Fi/TV subnet overlap check.

**Exit:**
- `curl http://monitor-control.local/api/status` works from the Mac.
- The TV can't reach hosts on the Wi-Fi LAN.

## Phase 3 — TV client (replaces `poc/tvctl.py`)

- `esp_websocket_client` over TLS, with certificate verification skipped
  (`CONFIG_ESP_TLS_INSECURE` + `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY`). The
  TV's cert is self-signed, and only the isolated link reaches it.
- Connect with the base64 client name `monitor_control_esp32` and the stored
  token, if any. Save `data.token` from `ms.channel.connect` in NVS. On
  `ms.channel.unauthorized`, mark the device unpaired.
- Send keys with cJSON. Macros: `menu` (default) and `cycle` (fallback), with
  timings from `config.h`.
- Wire `/api/input/N`, `/api/pair` and `/api/key/…` to the TV task.

**Exit:**
- The token from `POST /api/pair` survives power cycles of both devices.
- `POST /api/input/1|2` switches correctly from either input.

## Phase 4 — 3-position toggle

- GPIO32/33 inputs with pull-ups and any-edge interrupts feed a 50 ms debounce
  task, which decodes the mode:

  | GPIO32 | GPIO33 | Mode |
  |---|---|---|
  | low | high | `hdmi1` |
  | high | high | `api` |
  | high | low | `hdmi2` |
  | low | low | `fault`: locked, no action, logged, shown on the status page |

- At boot, read the mode and apply the lock, but don't switch, so a power blip
  never changes the input.

**Exit:**
- Going HDMI 1 → API → HDMI 2 → API → HDMI 1 switches the TV at each outer
  position and leaves it alone in the center.
- `POST /api/input/N` returns `409` in an outer position and switches in the center.
- Flipping to an outer position while an API macro is queued ends on the switch's input.

## Phase 5 — Polish

- `README.md`: wiring diagram, flashing, first pairing, API examples.
- A serial and HTTP log of the last N commands with timings, for chasing dropped keys.
- Keep `docs/plans/02_esp32/findings.md` up to date throughout.

## Risks

| Risk | Impact | Mitigation |
|---|---|---|
| ESP32 TLS can't handshake with the TV (ciphers, key size) | No control | Test early in Phase 3. Fallback: plain `ws://:8001`, if the TV allows remote control without TLS |
| `KEY_SOURCE` dropped (seen once in PoC) | Up/Down/Enter go to the current input or its CEC device | Generous delays, timing logs, `cycle` fallback |
| lwIP DHCP server quirks with `udhcpc` | No lease | A single-address pool; if that fails, port `dhcpd.py` onto a raw UDP socket |
| Ethernet plugged into a real LAN | Rogue DHCP server | Pre-serve probe (Phase 1) |
| Wi-Fi and TV subnets overlap | Routing confusion | Configurable subnet, boot-time check |
| Unauthenticated API | Anyone on the LAN can switch while the toggle is centered | Accepted; the outer positions are a physical lock-out, and an API key is an easy add later |
| Switch bounce, or passing through the center on an HDMI 1 ↔ HDMI 2 flip | Spurious mode changes | Debounce; the center triggers nothing; latest-wins queue |
| WT32-ETH01 clone pin differences | No Ethernet | Confirm pins in Phase 0 |

## Open questions

- Where will the box live, and how will it be powered? If it runs off the TV's
  USB port, it may lose power when the TV sleeps.
- Is `192.168.77.0/24` free on the home network?
