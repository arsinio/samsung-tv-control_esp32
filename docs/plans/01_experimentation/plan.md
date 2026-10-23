# 01 — Experimentation / PoC: Network control of Samsung Q7F

**Status:** Draft · **Date:** 2026-10-23 · **Owner:** Christopher Armenio

## Goal

Prove that a script on this Mac can, over a private wired link:

1. Hand out a DHCP address to the TV on **Ethernet only** (never Wi-Fi).
2. Connect to the TV's local control API.
3. Switch between **HDMI 1** and **HDMI 2** on command.

Everything here is throwaway-quality on purpose. The output of this phase is a
yes/no on each goal plus a findings log that drives the next plan.

### Out of scope (for now)

- Internet access for the TV, SmartThings cloud API
- Running as a background service / launchd, auto-start
- Power on/off, volume, any input besides HDMI 1/2
- A UI, hotkeys, or packaging

## Known environment

| Item | Value |
|---|---|
| Host | macOS 26.6.2, Python 3.13.2 (pyenv), Homebrew present |
| Wired interface | `en5` — "USB 10/100/1000 LAN", MAC `00:e0:4c:68:00:0e`, link up at 100baseTX |
| Current `en5` config | Manual, `192.168.2.2/24`, no router |
| Wi-Fi interface | `en0` — **must never serve DHCP** |
| TV | Samsung QN43Q7FAAFXZA (43" Q7F, 2025, Tizen), cabled directly to `en5`, MAC `28:07:08:6b:0f:cd` |
| Network policy | Isolated: no NAT, no default route, no DNS handed to the TV |

## Approach summary

```
 Mac (en5 192.168.2.2/24)  ──── Cat cable ────  Samsung Q7F (DHCP → 192.168.2.10)
   ├─ dhcpd.py (raw Ethernet via /dev/bpf, attached to en5 only)
   └─ tvctl.py ──wss://192.168.2.10:8002──► Tizen remote-control WebSocket API
```

- **DHCP:** `poc/dhcpd.py`, a stdlib-only Python DHCP server that reads and writes
  raw frames on `en5` through `/dev/bpf`. Needs no `sudo` (the user is in
  `access_bpf`), never binds UDP 67 (launchd holds it for `bootpd`), and can't
  answer on any other interface. Hands out a subnet mask only: no router, no
  DNS. (Replaced the original dnsmasq approach — see `findings.md`.)
- **Control:** Samsung's local Tizen WebSocket API (port 8001 plain / 8002 TLS),
  via the Python [`samsungtvws`](https://github.com/xchwarze/samsung-tv-ws-api)
  library. First connection triggers an on-TV "Allow" prompt and returns a
  token we persist.
- **Input switching:** send remote key codes. `KEY_HDMI1` / `KEY_HDMI2` are the
  direct-input keys; whether the Q7F honors them is **the main unknown** of
  this phase (see Phase 3).

## Proposed PoC layout

```
monitor_control/
├── docs/plans/01_experimentation/
│   ├── plan.md              # this file
│   └── findings.md          # experiment log (created during Phase 1)
├── poc/
│   ├── dhcpd.py             # raw-packet DHCP server, en5 only, stdlib only
│   ├── tvctl.py             # info | pair | key <KEY> | hdmi <1|2>
│   └── requirements.txt     # samsungtvws
└── .state/                  # gitignored: TV token, DHCP lease file
```

---

## Phase 0 — Prep (TV + Mac)

**TV settings** (menu names vary slightly by firmware; record the actual paths
in `findings.md`):

- [ ] Settings → General → Network → Network Settings → **Wired**, IP settings
      **Obtain automatically** (undo any static config left over).
- [ ] Make sure the TV is not also joined to Wi-Fi (forget the network if so) so
      we know which interface is answering.
- [ ] Settings → General → External Device Manager → Device Connection Manager:
      **Access Notification = First time only** (or Always), and clear any stale
      entries in the device list.
- [ ] Note whether **IP Remote** / **Power On with Mobile** exist and enable
      them; we won't use power-on yet, but it tells us what the firmware exposes.
- [ ] Record model code (sticker / Settings → Support → About This TV) and
      firmware version.

**Mac:**

- [x] `python3 -m venv .venv && .venv/bin/pip install -r poc/requirements.txt`
      (`samsungtvws==3.0.6`).
- [x] macOS Internet Sharing (Wi-Fi → `en5`) turned **off**.
- [ ] `en5` has a static `192.168.2.2/24` (configured, but not applied after
      Internet Sharing was turned off — re-apply it).
- [x] `git init`, `.gitignore` for `.venv/` and `.state/`.

## Phase 1 — DHCP on Ethernet only

`poc/dhcpd.py` (stdlib only, no `sudo`):

```bash
python3 poc/dhcpd.py                       # pool 192.168.2.100-150
python3 poc/dhcpd.py --pin <tv-mac>=192.168.2.10
```

**Built-in safety checks** (refuses to start rather than become a rogue DHCP
server): the interface exists, has link, and has a static IPv4; the default
route is not via it; it is not a member of a bridge (Internet Sharing). While
running, it exits if it sees any other DHCP server answering on `en5`.

**Behavior:** answers DISCOVER/REQUEST/INFORM per RFC 2131 delivery rules
(unicast to the client's MAC, or broadcast when asked); NAKs requests for
addresses it didn't assign; ignores relayed packets; persists pool leases in
`.state/dhcp_leases.json`; logs hostname, vendor class and the parameter list
of every request (useful TV fingerprinting data).

**Steps:**

1. Run `poc/dhcpd.py`.
2. Unplug/replug the TV's cable (or toggle its wired network) and watch for
   `DISCOVER → OFFER → REQUEST → ACK`.
3. Grab the TV's MAC from the log and restart with
   `--pin <tv-mac>=192.168.2.10`; force a renew.
4. Verify: `ping 192.168.2.10`, `arp -an -i en5`.

**Exit criteria:** TV holds `192.168.2.10`, survives a TV reboot, and gets no
router/DNS.

## Phase 2 — Discover and pair

1. `curl -s http://192.168.2.10:8001/api/v2/ | python3 -m json.tool` — confirm
   the REST endpoint responds. Record `modelName`, `OS`, `firmwareVersion`,
   `TokenAuthSupport`, `PowerState`, and `wifiMac`/wired MAC.
2. `tvctl.py pair` — open `wss://192.168.2.10:8002/api/v2/channels/samsung.remote.control`
   with a fixed client name (e.g. `monitor_control`), accept the on-TV prompt,
   save the returned token to `.state/tv-token`.
3. Disconnect, reconnect with the token: **no prompt** should appear.
4. Sanity check with a harmless key (`KEY_VOLUP`/`KEY_VOLDOWN` or `KEY_INFO`).
5. Note any behavior from the TV having no internet (warnings, slow API start
   after boot, API unavailable in standby).

**Exit criteria:** token persists across script runs and a TV reboot; a key
press visibly works.

## Phase 3 — HDMI input switching (the real experiment)

Try in order; stop at the first that works reliably. Verification is visual
(the local API has no reliable "current input" query), so put a distinct
source on each port — e.g. a laptop on HDMI 1 and a streaming stick or second
machine on HDMI 2.

| # | Method | Notes |
|---|---|---|
| A | `KEY_HDMI1`, `KEY_HDMI2` | Direct-select keys. Works on many Tizen models; some newer firmware ignores them. |
| B | `KEY_HDMI` | Usually cycles through HDMI inputs. Workable for two inputs only if the cycle order is stable and we know where we start. |
| C | `KEY_SOURCE` + arrow/`KEY_ENTER` macro | Opens the source picker and navigates. Fragile (depends on menu layout and current position), but a known last resort. |
| D | App-channel / other endpoints | Probe `/api/v2/applications/*`, SSDP/UPnP services (`urn:samsung.com:...`), port 9197 DIAL. Exploratory — just log what exists. |

Things to record for each method in `findings.md`:

- Does it work from **each** starting state (HDMI1 → 2, HDMI2 → 1, from TV
  home/apps, from a source with no signal)?
- Latency from command to picture switch.
- Any on-screen side effects (source banner, menu left open).
- Behavior if the target HDMI port has no active signal.

**Exit criteria:** `tvctl.py hdmi 1` and `tvctl.py hdmi 2` land on the right
input from every starting state, 10/10 times each.

## Phase 4 — Consolidate the PoC

- `tvctl.py` subcommands: `info`, `pair`, `key <KEY_NAME>`, `hdmi <1|2>`, with
  the TV IP and token path as flags/env vars (defaults `192.168.2.10`,
  `.state/tv-token`).
- Short `poc/README.md`: start DHCP, pair, switch.
- Write up results and recommendations at the bottom of `findings.md`.

## Risks and unknowns

| Risk | Impact | Mitigation |
|---|---|---|
| Q7F ignores `KEY_HDMI1/2` | Main goal blocked on direct method | Methods B/C; if all fail, revisit SmartThings (needs internet) or HDMI-CEC in a later phase |
| DHCP leaks to Wi-Fi | Rogue DHCP on home network | BPF attached to `en5` only; preflight refuses bridged/uplink interfaces; exits if another DHCP server answers |
| `en5` later plugged into a real LAN | Rogue DHCP on that LAN | Preflight checks; never install as a service during PoC |
| TV rejects/forgets pairing | Re-prompt each run | Fixed client name, persisted token, check Device Connection Manager settings |
| WebSocket API unavailable in standby or slow after boot | Commands fail intermittently | Note timings; power handling is a next-phase topic |
| TV nags or degrades without internet | Annoyance or reduced API | Record it; reconsider NAT only if it blocks the goals |
| 100 Mbps link / USB adapter quirks | Low — control traffic is tiny | None needed |

## Done = PoC proven

- [ ] TV receives a pinned DHCP lease on `en5` only
- [ ] Script pairs once and reconnects silently
- [ ] `hdmi 1` / `hdmi 2` switch reliably from any starting state
- [ ] `findings.md` captures the working method, failures, and open questions

## Questions to settle before the next plan

- Trigger mechanism: CLI only, global hotkey, menu-bar app, Stream Deck, etc.?
- Should the TV ever get internet (firmware updates, SmartThings)?
- Does the controller need to power the TV on/off too?
- Run on-demand vs. always-on service (launchd) for DHCP?
