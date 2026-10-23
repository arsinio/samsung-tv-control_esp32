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

_Not started._

## Input switching

_Not started._
