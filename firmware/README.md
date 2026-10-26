# Monitor Control firmware (WT32-ETH01)

ESP-IDF firmware that replaces `poc/`. It:

- **Serves DHCP to the TV** on Ethernet: `192.168.77.10`, no router, isolated from Wi-Fi.
- **Joins Wi-Fi** and serves a REST API and status page at `http://monitor-control.local/`.
- **Reads a 3-position toggle:** HDMI 1 / API / HDMI 2.

The design is in [docs/plans/02_esp32/plan.md](../docs/plans/02_esp32/plan.md).

## Wiring

```
USB-TTL (3.3 V)        WT32-ETH01
  TX  ───────────────► RX0
  RX  ◄─────────────── TX0
  GND ──────────────── GND
  5V  ──────────────── 5V        (or power the board separately; share GND)
                       IO0 ──┐   hold to GND while resetting = download mode
                       GND ──┘

ON-OFF-ON toggle       WT32-ETH01
  common ───────────── GND
  throw A ──────────── IO32      (HDMI 1)
  throw B ──────────── IO33      (HDMI 2)
                                 center = API

Ethernet jack ──────── TV (direct cable)
```

Use **`TX0`/`RX0`** (GPIO1/GPIO3). The pins labelled `TXD`/`RXD` (GPIO17/GPIO5)
are the factory AT firmware's command port. On them you'll see `Module OK` and
AT replies, but flashing never connects.

The board has no USB port or auto-reset circuit. With an adapter that drives
DTR→IO0 and RTS→EN (like a dev kit), `flash` just works. Otherwise, jumper IO0
to GND, reset, flash, then remove the jumper and reset again.

## Build and flash

```bash
cp main/secrets.example.h main/secrets.h   # then set WIFI_SSID / WIFI_PASSWORD
./idf.sh build
./idf.sh -p /dev/cu.usbserial-XXXX flash monitor
```

`idf.sh` runs `idf.py` with ESP-IDF v5.2 (`~/esp/v5.2/esp-idf`) and its Python 3.9
environment. Override `IDF_PATH` / `IDF_PYTHON_ENV_PATH` to use another install.
Exit the monitor with `Ctrl-]`.

## First pairing

The TV has to approve this device once (the token is kept in flash):

```bash
curl -X POST http://monitor-control.local/api/pair
```

Accept the prompt on the TV within 30 s. With no token yet, the first input
switch also triggers the prompt.

## API

| Request | Effect |
|---|---|
| `GET /api/status` | Mode, last input, TV/DHCP/Wi-Fi state |
| `GET /api/log` | Last 16 commands with result and duration |
| `POST /api/input/1`, `POST /api/input/2` | Switch input; `202`, or `409` when the toggle isn't centered |
| `POST /api/key/KEY_NAME` | Send one remote key (e.g. `KEY_VOLUP`); `409` when locked |
| `POST /api/pair` | Forget the token and pair again (any mode) |
| `GET /` | Status page with buttons |

```bash
curl -s http://monitor-control.local/api/status | python3 -m json.tool
curl -X POST http://monitor-control.local/api/input/2
```

`input` in status is the last input this device commanded. The TV can't report
its actual input. Requests arriving over the TV link get `403`.

## Toggle behavior

| Position | On entering | API |
|---|---|---|
| HDMI 1 / HDMI 2 | Switches the TV to that input, replacing any queued API request | Locked (`409`) |
| API (center) | Nothing | In control |

At boot, the position sets the lock but sends nothing, so a power blip never
changes the input. If both throws ever read closed, the mode is `fault`
(locked, no action).

## Tuning

Everything lives in [main/config.h](main/config.h): pins, subnet, menu timings,
and `TV_USE_CYCLE_MACRO` to switch from the Source-menu macro to the
`KEY_TV` + `KEY_HDMI` fallback.

## Bring-up checklist

Work through these in order, watching `./idf.sh monitor`. Record results in
[docs/plans/02_esp32/findings.md](../docs/plans/02_esp32/findings.md).

1. **Boots:** `main: up` is logged, along with `switch: boot mode …`.
2. **Ethernet:** `eth: MAC …` then `eth: link up` with the TV cable plugged in. If
   the link never comes up, check the GPIO16/GPIO0 oscillator and PHY address (see config.h).
3. **Probe:** `eth: probing for other DHCP servers`, then `DHCP server serving 192.168.77.10`.
4. **Lease:** replug the TV cable (or toggle its network) and look for
   `eth: lease 192.168.77.10 -> 28:07:08:6b:0f:cd`.
5. **Wi-Fi:** `wifi: got <ip>`. `curl http://monitor-control.local/api/status` from the Mac.
6. **Pair:** `POST /api/pair` and accept on the TV. Look for `tv: paired; token saved`.
7. **Switch via API:** with the toggle centered, `POST /api/input/2` then `/1`.
8. **Toggle:** HDMI 1 → center → HDMI 2 → center → HDMI 1, checking the TV and
   `"mode"` each time. In an outer position, `POST /api/input/1` should return `409`.
9. **Isolation:** status shows `"subnet_conflict": false`. The TV link offers no
   router (the TV has no route to the home LAN).
10. **Rogue guard (optional):** plug the Ethernet into a home-LAN switch. It
    should log `another DHCP server answered…` and never serve.
11. **Reboot survival:** power-cycle the board. It stays paired and reconnects
    (`tv: connected to TV` within ~30 s).

All of these except 9 (checked by config only) and 10 passed on 2026-10-26; see
the findings.
