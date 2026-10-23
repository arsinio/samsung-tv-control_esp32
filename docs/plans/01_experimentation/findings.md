# 01 — Experimentation findings

Running log for [plan.md](plan.md). Newest entries at the bottom of each section.

## Environment

- 2026-10-23: TV is on `en5` (USB 10/100/1000 LAN, `00:e0:4c:68:00:0e`), link at
  100baseTX full-duplex.
- 2026-10-23: A device MAC `28:07:08:6b:0f:cd` was seen on `en5` (bridge learning
  table). Probably the TV; confirm from the first DHCP DISCOVER.

## DHCP

- 2026-10-23: **macOS Internet Sharing was on**, sharing Wi-Fi (`en0`) to `en5`
  through `bridge100` (`192.168.2.1/24`), with Apple's `bootpd` set up to hand out
  `192.168.2.2–254` with router and DNS. The TV link was not isolated. The
  `192.168.2.2` manual address on `en5` sat inside that range. Turned Sharing off.
- 2026-10-23: launchd keeps `*:67` (UDP) open for on-demand `bootpd`, so any
  normal socket-based DHCP server (dnsmasq included) fails with "address already
  in use". `pgrep bootpd` doesn't catch this, because launchd holds the socket
  while `bootpd` isn't running.
- 2026-10-23: Dropped dnsmasq for `poc/dhcpd.py`: raw Ethernet via `/dev/bpf`
  attached to `en5`. Works without `sudo` because the user is in the
  `access_bpf` group (`/dev/bpf*` is `root:access_bpf 0660`, from Wireshark's
  ChmodBPF). Passes an offline self-test of DISCOVER/OFFER, REQUEST/ACK, NAK,
  renew, INFORM, pool exhaustion, lease persistence and rogue-server detection.
- 2026-10-23: After Sharing was turned off, `en5` lost its IPv4 even though
  `networksetup -getinfo` still reports manual `192.168.2.2/24`. It has to be
  re-applied before `dhcpd.py` will start.
- 2026-10-23: Turning Internet Sharing off left `en5` with no kernel IPv4 even
  though configd still reported `192.168.2.2` (`ipconfig getifaddr` returned it,
  `scutil` had no state). Re-entering the same settings didn't help; replugging
  the USB adapter did.
- 2026-10-23: **First live lease worked**, ~70 s after the adapter replug (the
  link flap made the TV re-run DHCP):
  - MAC `28:07:08:6b:0f:cd`, hostname `Samsung`, vendor class `udhcpc1.21.1`
    (BusyBox udhcpc).
  - Parameter request list `[1, 3, 26, 252, 43, 42, 6, 12]`: mask, router,
    MTU, WPAD, vendor-specific, NTP, DNS, hostname.
  - Accepted a lease with **no router and no DNS** without complaint, and
    unicast replies to its MAC were fine (it doesn't set the broadcast flag).
  - Right after the lease it sends SSDP (`239.255.255.250:1900`), Samsung
    discovery on UDP `15600` (broadcast and multicast), IGMP and IPv6 router
    solicitations.
- 2026-10-23: Restarted with `--pin 28:07:08:6b:0f:cd=192.168.2.10`. The TV keeps
  `.100` until it renews (T1 = 6 h) or its link flaps. On renewal the server
  NAKs `.100`, so the TV rediscovers and gets `.10`.
- 2026-10-23: **Pinned lease verified.** After the TV's cable was replugged, it
  sent a fresh DISCOVER (not an INIT-REBOOT request for `.100`, so the NAK path
  wasn't exercised live), got `192.168.2.10`, and the API reports `ip`
  `192.168.2.10`. `.100` no longer answers.
- _TODO: confirm the pin survives a full TV reboot (Phase 1 exit criterion)._

## TV identity (from `http://<tv>:8001/api/v2/`)

- `modelName` `QN43Q7FAAFXZA` (43" Q7F, 2025), `OS` Tizen, API `version` 2.0.25,
  `remote` 1.0
- `networkType` wired; the `wifiMac` field holds the wired MAC
- `TokenAuthSupport` true, `remote_available` true, `PowerState` on
- `firmwareVersion` reports `Unknown`; `smartHubAgreement` false (fine offline)

## Pairing

- 2026-10-23: `poc/tvctl.py` (`info`, `pair`, `key`, `hdmi`) built on
  `samsungtvws` 3.0.6, client name `monitor_control`, token in
  `.state/tv-token`.
- 2026-10-23: **macOS Local Network privacy gets in the way.** Python
  connections to `192.168.2.10` failed with `EHOSTUNREACH` (`[Errno 65] No route
  to host`) even though `ping` and `curl` worked. The first failure came from
  Claude's shell, which then started working, maybe after a permission prompt was
  answered. It kept failing from the Terminal-panel tab. Whatever runs the
  controller needs Local Network permission (System Settings → Privacy &
  Security → Local Network). This matters for any future background service.
- 2026-10-23: **Pairing worked.** The `wss://…:8002` connection showed the on-TV
  prompt; after Allow, the `ms.channel.connect` event returned a token about 5 s
  later. The token is a short numeric string. The `--debug` log prints it, so
  keep debug logs in `.state/`.
- 2026-10-23: **Silent reconnect worked.** With the token, `ms.channel.connect`
  arrived about 230 ms after connecting, with no prompt. `KEY_VOLUP` +
  `KEY_VOLDOWN` took about 1 s end to end (0.3 s gap between keys).
- _TODO: confirm the token survives a TV reboot._

## Input switching

- 2026-10-23: **Method A (`KEY_HDMI1` / `KEY_HDMI2`) does nothing on the Q7F.**
  Both keys were accepted over the WebSocket with no error event, but the TV
  didn't change input (watched visually). The same session's volume keys
  worked, so the connection is fine. The firmware ignores the direct-input keys.
- 2026-10-23: **Method B (`KEY_HDMI`) switches input.** One press moved the TV to
  the next HDMI input.
- 2026-10-23: **`KEY_HDMI` cycle is HDMI 1 → HDMI 2 → TV (tuner, static) →
  HDMI 1.** Four presses about 8 s apart, starting on HDMI 1 (the Mac), landed on
  HDMI 2, TV, HDMI 1, HDMI 2. HDMI 3 was skipped, probably because nothing is
  connected to it. The TV tuner is part of the cycle. On its own, cycling is
  blind: the API can't report the current input, so we need a known anchor
  state first.
- 2026-10-23: **`KEY_TV` jumps straight to the tuner**, so it works as an anchor.
  From HDMI 2: `KEY_TV` → static, then 8 s later `KEY_HDMI` → HDMI 1. That gives
  the working recipe: `hdmi 1` = `KEY_TV` + `KEY_HDMI`; `hdmi 2` = `KEY_TV` +
  `KEY_HDMI` ×2. The cost is a brief pass through static (and, for `hdmi 2`,
  through HDMI 1).
- 2026-10-23: **Key spacing matters.** With 2 s between keys, `hdmi 1` worked
  but `hdmi 2` didn't reach HDMI 2: a `KEY_HDMI` press during the previous
  switch seems to get dropped. With 5 s, `hdmi 2` worked, and `hdmi 1` then
  returned to HDMI 1. Default `--switch-delay` raised to 5 s, so `hdmi 2` takes
  about 10.5 s. The shortest reliable gap, and whether it differs after
  `KEY_TV` vs after `KEY_HDMI`, is still to be found.
- 2026-10-23: One `hdmi 1` at 5 s (12:24) didn't return to HDMI 1. The user
  couldn't see the TV and the Mac at the same time, so the failing step is
  unknown and it couldn't be reproduced.
- 2026-10-23: **4/4 with 5 s spacing**, watched live: `hdmi 2` → `hdmi 1` →
  `hdmi 2` → `hdmi 1`, each from the other HDMI input, about 8 s apart. The
  downside is visible time on the tuner (static) for the full 5 s wait after
  `KEY_TV`.
- 2026-10-23: **Delays split** into `--tv-delay` (after `KEY_TV`) and
  `--hdmi-delay` (between `KEY_HDMI` presses). Single watched runs:

  | Command | tv-delay | hdmi-delay | Start | Result |
  |---|---|---|---|---|
  | `hdmi 1` | 2 | n/a | HDMI 1 | stayed on HDMI 1, no static seen (doesn't tell success from ignored keys) |
  | `hdmi 1` | 1 | n/a | HDMI 1 | same |
  | `hdmi 2` | 1 | 5 | HDMI 1 | ✅ HDMI 2 |
  | `hdmi 2` | 1 | 3 | HDMI 2 | ✅ HDMI 2 |
  | `hdmi 2` | 1 | 2 | HDMI 2 | ✅ HDMI 2 |

  With a 1 s tuner wait the static barely shows. The earlier 2 s / 2 s
  `hdmi 2` failure didn't reproduce, so drops look **intermittent** rather than
  a hard threshold. That makes the 10× reliability runs essential. Defaults are
  now `--tv-delay 1.5 --hdmi-delay 3` for margin: `hdmi 1` ≈ 2 s, `hdmi 2` ≈ 5 s.

## Source menu (method C)

- 2026-10-23: `KEY_SOURCE` opens a **3 × 2 grid**:

  | | Left | Right |
  |---|---|---|
  | Top | TV | SmartThings |
  | Middle | HDMI 1 | Help |
  | Bottom | PC (HDMI 2, auto-named after the connected device) | Setup Universal Remote |

  HDMI 3 isn't listed, presumably because nothing is connected to it. The
  highlight **starts on the current input** (PC when on HDMI 2). The menu
  **closes by itself** after roughly a minute.
- 2026-10-23: Edges: **Up, Down and Right stop at the edge, with no wrap**
  (Up ×4 from PC stopped on TV, and the user confirmed the rest by hand).
  **Left from the left column exits the menu.** So "Up past the top" is a fixed
  anchor on TV, and the macro must never press Left.
- 2026-10-23: The user has **no physical remote** at hand, so `tvctl.py` is the
  only way into this menu.
- 2026-10-23: `tvctl.py hdmi N --method menu` = `KEY_SOURCE`, Up ×4, Down ×N,
  `KEY_ENTER` (1.0 s for the menu to open, 0.5 s between presses).
- 2026-10-23: **`hdmi 2 --method menu` from HDMI 1 worked**, taking about 5 s
  with no tuner static.
- 2026-10-23: **`hdmi 1 --method menu` from HDMI 2 worked** (about 4 s). The
  menu method is now the default; `--method cycle` is kept as a fallback.
- 2026-10-23: **Menu order is stable after use.** After switching HDMI 2 → HDMI 1
  via the menu, the left column still read TV, HDMI 1, PC.
- 2026-10-23: One `KEY_SOURCE` (12:49:25) apparently didn't open the menu, or
  it had already timed out by the time the user looked (about 30 s or more
  later). The resend at 12:50:01 opened it. **Risk:** if `KEY_SOURCE` is ever
  dropped, the macro's Up/Down/Enter go to the current input instead, and may
  be passed to the HDMI device over CEC (Enter = "select" on that device).
  Watch for this in the reliability runs.

## Reading the current input

- 2026-10-23: **No local way to read the active source found.** `/api/v2/` has
  no source field. SSDP shows only a DLNA MediaRenderer on `:9197/dmr`
  (RenderingControl, ConnectionManager, AVTransport; the `X_` extensions cover
  aspect ratio, captions, zoom and slideshow, not input) and DIAL on
  `:7678/nservice/`. The legacy MainTVAgent2 service (`:7676`,
  `GetCurrentExternalSource`) isn't present. SmartThings' `mediaInputSource`
  would report it but needs the cloud, which is out of scope.
- 2026-10-23: **The Mac can't tell either.** With the TV on HDMI 2, macOS still
  listed the SAMSUNG display as online at 3840×2160. The TV keeps hot-plug up on
  inactive inputs, so display presence can't verify a switch.

## Reliability run (partial)

- 2026-10-23: Alternating `hdmi 1` / `hdmi 2` with defaults (1.5 s / 3 s), about
  6 s between commands: steps 1–8 were sent with no errors (`hdmi 1` ≈ 2–3 s,
  `hdmi 2` ≈ 5 s). Stopped there: the user couldn't keep count, so the landing
  results weren't tallied.
- 2026-10-23: **`KEY_SOURCE` opens the Source menu** (closed by hand afterwards).
  Method C stays available as a fallback.
