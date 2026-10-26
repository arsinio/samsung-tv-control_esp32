# Monitor Control firmware (WT32-ETH01)

ESP-IDF firmware for the [Monitor Control](../README.md) box. It:

- **Serves DHCP to the TV** on Ethernet: `192.168.77.10`, no router, isolated from Wi-Fi.
- **Joins Wi-Fi** and serves a REST API and status page at `http://monitor-control.local/`.
- **Reads a 3-position toggle:** HDMI 1 / API / HDMI 2.
- **Has a power button** that toggles the TV: `KEY_POWER` when on, Wake-on-LAN when off.

The design is in [docs/plans/02_esp32/plan.md](../docs/plans/02_esp32/plan.md).

## Wiring

```
USB-serial (3.3 V)     WT32-ETH01
  TX  ───────────────► RX0       (not RXD: see below)
  RX  ◄─────────────── TX0       (not TXD)
  DTR ───────────────► IO0       auto-reset adapter: DTR/RTS put it in download mode
  RTS ───────────────► EN
  GND ──────────────── GND

5 V supply ─────────── 5V, GND   (its own supply, not the TV's USB port)

ON-OFF-ON toggle       WT32-ETH01
  common ───────────── GND
  throw A ──────────── IO32      (HDMI 1)
  throw B ──────────── IO33      (HDMI 2)
                                 center = API

Momentary button       WT32-ETH01
  one side ─────────── IO4       (power)
  other side ───────── GND

Ethernet jack ──────── TV (direct cable)
```

- **Power:** use a separate 5 V supply. The TV cuts its USB power in standby,
  which takes the board down too, so the power button could never turn the TV
  back on.
- **Serial:** use **`TX0`/`RX0`** (GPIO1/GPIO3). The `TXD`/`RXD` pins
  (GPIO17/GPIO5) are the factory AT firmware's port. On them you'll see
  `Module OK`, but flashing never connects.
- **Download mode:** an adapter wired DTR→IO0 and RTS→EN flashes like a dev
  kit. Without one, hold IO0 to GND while resetting, flash, then release IO0
  and reset.
- **The toggle and button need no resistors:** the firmware enables the
  internal pull-ups.

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
| `POST /api/power` | Toggle TV power, like the button (any mode) |
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

## Power button

One press toggles power, in any toggle position:

- The board first reads the TV's `PowerState` (`http://192.168.77.10:8001/api/v2/`).
- If the TV is **on**, it sends `KEY_POWER`.
- If the TV is **in standby or not answering**, it sends Wake-on-LAN to the TV's
  MAC (saved in flash from its last lease). Once the TV is up (about 3 s), the
  board switches to the toggle's input. It does nothing in API mode, and a newer
  command takes priority.

A press counts after 30 ms held low, and the next one only after a clean
release. `tv.power` in the status refreshes every 30 s (`waking` right after
a wake).

## Tuning

Everything lives in [main/config.h](main/config.h):

- **Pins and subnet.**
- **Menu timings:** 0.7 s for the menu to open, 0.3 s per key. Raise them if
  switches land wrong or leave the menu open.
- **Wake settle time.**
- **`TV_USE_CYCLE_MACRO`:** uses the `KEY_TV` + `KEY_HDMI` fallback instead of
  the Source menu.

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

All of these except 9 (checked by config only) and 10 passed on 2026-10-26,
along with the power button via `POST /api/power`. See the findings.
