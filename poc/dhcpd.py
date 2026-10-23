#!/usr/bin/env python3
"""Minimal DHCP server for the isolated TV link.

Reads and writes raw Ethernet frames on a single interface through /dev/bpf, so:
  - it needs no root, only membership in the access_bpf group (Wireshark's ChmodBPF);
  - it never binds UDP port 67, so it doesn't fight launchd/bootpd for it;
  - it cannot answer on any interface except the one it is attached to.

It hands out a subnet mask only: no router, no DNS, so the TV stays isolated.
"""

import argparse
import ipaddress
import json
import os
import random
import re
import select
import struct
import subprocess
import sys
import time
from dataclasses import dataclass
from fcntl import ioctl
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DEFAULT_LEASES = REPO / ".state" / "dhcp_leases.json"

# <net/bpf.h> ioctls (macOS, 64-bit)
BIOCGBLEN = 0x40044266
BIOCSETIF = 0x8020426C
BIOCIMMEDIATE = 0x80044270
BIOCSHDRCMPLT = 0x80044275
BIOCSSEESENT = 0x80044277
BPF_ALIGNMENT = 4

ETH_P_IP = 0x0800
BROADCAST_MAC = b"\xff" * 6
BROADCAST_IP = ipaddress.IPv4Address("255.255.255.255")
ZERO_IP = ipaddress.IPv4Address("0.0.0.0")
MAGIC_COOKIE = b"\x63\x82\x53\x63"

DISCOVER, OFFER, REQUEST, DECLINE, ACK, NAK, RELEASE, INFORM = range(1, 9)
MSG_NAMES = {1: "DISCOVER", 2: "OFFER", 3: "REQUEST", 4: "DECLINE",
             5: "ACK", 6: "NAK", 7: "RELEASE", 8: "INFORM"}

OPT_MASK, OPT_HOSTNAME, OPT_REQUESTED_IP, OPT_LEASE_TIME = 1, 12, 50, 51
OPT_MSG_TYPE, OPT_SERVER_ID, OPT_PARAM_LIST = 53, 54, 55
OPT_RENEWAL, OPT_REBINDING, OPT_VENDOR_CLASS, OPT_CLIENT_ID = 58, 59, 60, 61


def log(msg):
    print(f"{time.strftime('%H:%M:%S')} {msg}", flush=True)


def mac_str(mac: bytes) -> str:
    return ":".join(f"{b:02x}" for b in mac)


def mac_bytes(mac: str) -> bytes:
    parts = re.split(r"[:-]", mac.strip())
    if len(parts) != 6:
        raise ValueError(f"bad MAC: {mac}")
    return bytes(int(p, 16) for p in parts)


def checksum(data: bytes) -> int:
    if len(data) % 2:
        data += b"\0"
    total = sum(struct.unpack(f"!{len(data) // 2}H", data))
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return ~total & 0xFFFF


# --- Interface inspection -------------------------------------------------

@dataclass
class Iface:
    name: str
    mac: bytes
    addr: ipaddress.IPv4Interface


def ifconfig(*args) -> str:
    return subprocess.run(["ifconfig", *args], capture_output=True, text=True).stdout


def preflight(name: str) -> Iface:
    """Refuse to run anywhere we could become a rogue DHCP server."""
    out = ifconfig(name)
    if not out:
        sys.exit(f"dhcpd: interface {name} does not exist")
    if "status: active" not in out:
        sys.exit(f"dhcpd: {name} has no link")
    ether = re.search(r"ether ([0-9a-f:]{17})", out)
    inet = re.search(r"inet (\S+) netmask (0x[0-9a-f]+)", out)
    if not ether:
        sys.exit(f"dhcpd: {name} has no MAC address")
    if not inet:
        sys.exit(f"dhcpd: {name} has no IPv4 address; give it a static one first")

    route = subprocess.run(["route", "-n", "get", "default"], capture_output=True, text=True).stdout
    if re.search(rf"interface: {re.escape(name)}\b", route):
        sys.exit(f"dhcpd: default route is via {name}; looks like a real LAN, refusing")

    for other in ifconfig("-l").split():
        if other.startswith("bridge") and re.search(rf"member: {re.escape(name)}\b", ifconfig(other)):
            sys.exit(f"dhcpd: {name} is a member of {other} (Internet Sharing?); turn it off first")

    netmask = ipaddress.IPv4Address(int(inet.group(2), 16))
    return Iface(name, mac_bytes(ether.group(1)), ipaddress.IPv4Interface(f"{inet.group(1)}/{netmask}"))


# --- DHCP packets ---------------------------------------------------------

@dataclass
class Dhcp:
    xid: int
    flags: int
    ciaddr: ipaddress.IPv4Address
    giaddr: ipaddress.IPv4Address
    chaddr: bytes
    options: dict

    @property
    def msg_type(self):
        opt = self.options.get(OPT_MSG_TYPE)
        return opt[0] if opt else None

    def opt_ip(self, code):
        opt = self.options.get(code)
        return ipaddress.IPv4Address(opt) if opt and len(opt) == 4 else None

    def opt_text(self, code):
        opt = self.options.get(code)
        return opt.decode(errors="replace") if opt else None


def parse_dhcp(payload: bytes):
    if len(payload) < 240 or payload[236:240] != MAGIC_COOKIE:
        return None
    op, htype, hlen, _hops, xid, _secs, flags = struct.unpack_from("!BBBBIHH", payload)
    if htype != 1 or hlen != 6:
        return None
    options = {}
    i = 240
    while i < len(payload):
        code = payload[i]
        if code == 0:
            i += 1
            continue
        if code == 255 or i + 1 >= len(payload):
            break
        length = payload[i + 1]
        options[code] = payload[i + 2:i + 2 + length]
        i += 2 + length
    return op, Dhcp(
        xid=xid,
        flags=flags,
        ciaddr=ipaddress.IPv4Address(payload[12:16]),
        giaddr=ipaddress.IPv4Address(payload[24:28]),
        chaddr=payload[28:34],
        options=options,
    )


def build_dhcp(req: Dhcp, msg_type: int, yiaddr, ciaddr, options: list) -> bytes:
    pkt = bytearray(240)
    struct.pack_into("!BBBBIHH", pkt, 0, 2, 1, 6, 0, req.xid, 0, req.flags)
    pkt[12:16] = ciaddr.packed
    pkt[16:20] = yiaddr.packed
    pkt[24:28] = req.giaddr.packed
    pkt[28:34] = req.chaddr
    pkt[236:240] = MAGIC_COOKIE
    pkt += bytes([OPT_MSG_TYPE, 1, msg_type])
    for code, value in options:
        pkt += bytes([code, len(value)]) + value
    pkt += b"\xff"
    return bytes(pkt.ljust(300, b"\0"))  # BOOTP minimum


def build_frame(src_mac, dst_mac, src_ip, dst_ip, payload: bytes) -> bytes:
    udp_len = 8 + len(payload)
    pseudo = src_ip.packed + dst_ip.packed + struct.pack("!BBH", 0, 17, udp_len)
    udp = struct.pack("!HHHH", 67, 68, udp_len, 0) + payload
    udp_sum = checksum(pseudo + udp) or 0xFFFF
    udp = udp[:6] + struct.pack("!H", udp_sum) + udp[8:]
    ip = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + udp_len, random.getrandbits(16),
                     0, 64, 17, 0, src_ip.packed, dst_ip.packed)
    ip = ip[:10] + struct.pack("!H", checksum(ip)) + ip[12:]
    return dst_mac + src_mac + struct.pack("!H", ETH_P_IP) + ip + udp


def parse_frame(frame: bytes):
    """Return (src_mac, sport, dport, udp payload) for IPv4/UDP frames, else None."""
    if len(frame) < 42 or struct.unpack_from("!H", frame, 12)[0] != ETH_P_IP:
        return None
    ip = frame[14:]
    ihl = (ip[0] & 0x0F) * 4
    if ip[0] >> 4 != 4 or ip[9] != 17 or len(ip) < ihl + 8:
        return None
    if struct.unpack_from("!H", ip, 6)[0] & 0x3FFF:  # fragment
        return None
    sport, dport, ulen = struct.unpack_from("!HHH", ip, ihl)
    return frame[6:12], sport, dport, ip[ihl + 8:ihl + ulen]


# --- Server ---------------------------------------------------------------

class Server:
    def __init__(self, iface: Iface, pins: dict, pool: list, lease_secs: int, lease_file: Path):
        self.iface = iface
        self.ip = iface.addr.ip
        self.pins = pins
        self.pool = pool
        self.lease_secs = lease_secs
        self.lease_file = lease_file
        self.leases = {}
        if lease_file.exists():
            self.leases = {k: ipaddress.IPv4Address(v) for k, v in json.loads(lease_file.read_text()).items()}

    def save_leases(self):
        self.lease_file.parent.mkdir(parents=True, exist_ok=True)
        self.lease_file.write_text(json.dumps({k: str(v) for k, v in self.leases.items()}, indent=2) + "\n")

    def address_for(self, mac: str):
        if mac in self.pins:
            return self.pins[mac]
        if mac in self.leases and self.leases[mac] in self.pool:
            return self.leases[mac]
        taken = set(self.leases.values()) | set(self.pins.values())
        for candidate in self.pool:
            if candidate not in taken:
                self.leases[mac] = candidate
                self.save_leases()
                return candidate
        return None

    def handle(self, frame: bytes):
        """Return a reply frame for this frame, or None."""
        parsed = parse_frame(frame)
        if not parsed:
            return None
        src_mac, sport, dport, payload = parsed
        if src_mac == self.iface.mac:
            return None
        dhcp = parse_dhcp(payload)
        if not dhcp:
            return None
        op, req = dhcp

        if op == 2 and sport == 67:
            raise RogueServer(f"another DHCP server ({mac_str(src_mac)}) answered on {self.iface.name}")
        if op != 1 or dport != 67:
            return None
        if req.giaddr != ZERO_IP:
            log(f"ignoring relayed packet via {req.giaddr}")
            return None

        mac = mac_str(req.chaddr)
        kind = MSG_NAMES.get(req.msg_type, str(req.msg_type))
        details = {
            "hostname": req.opt_text(OPT_HOSTNAME),
            "vendor": req.opt_text(OPT_VENDOR_CLASS),
            "requested": req.opt_ip(OPT_REQUESTED_IP),
            "ciaddr": req.ciaddr if req.ciaddr != ZERO_IP else None,
            "params": list(req.options[OPT_PARAM_LIST]) if OPT_PARAM_LIST in req.options else None,
        }
        log(f"<- {kind:8} {mac} " + " ".join(f"{k}={v}" for k, v in details.items() if v is not None))

        if req.msg_type == DISCOVER:
            ip = self.address_for(mac)
            if ip is None:
                log("   pool exhausted, not offering")
                return None
            return self.reply(req, OFFER, ip)

        if req.msg_type == REQUEST:
            server_id = req.opt_ip(OPT_SERVER_ID)
            if server_id is not None and server_id != self.ip:
                log(f"   client chose server {server_id}, staying quiet")
                return None
            wanted = req.opt_ip(OPT_REQUESTED_IP) or req.ciaddr
            ip = self.address_for(mac)
            if ip is not None and wanted == ip:
                return self.reply(req, ACK, ip)
            return self.reply(req, NAK, ZERO_IP)

        if req.msg_type == INFORM:
            return self.reply(req, ACK, ZERO_IP, inform=True)

        return None  # DECLINE / RELEASE: logged above, nothing to send

    def reply(self, req: Dhcp, msg_type: int, yiaddr, inform=False):
        options = [(OPT_SERVER_ID, self.ip.packed)]
        if msg_type != NAK:
            options.append((OPT_MASK, self.iface.addr.netmask.packed))
            if not inform:
                options += [
                    (OPT_LEASE_TIME, struct.pack("!I", self.lease_secs)),
                    (OPT_RENEWAL, struct.pack("!I", self.lease_secs // 2)),
                    (OPT_REBINDING, struct.pack("!I", self.lease_secs * 7 // 8)),
                ]
        ciaddr = req.ciaddr if msg_type == ACK else ZERO_IP
        payload = build_dhcp(req, msg_type, yiaddr, ciaddr, options)

        # RFC 2131 §4.1 delivery rules (no relay support)
        if msg_type == NAK or (req.ciaddr == ZERO_IP and req.flags & 0x8000):
            dst_mac, dst_ip = BROADCAST_MAC, BROADCAST_IP
        elif req.ciaddr != ZERO_IP:
            dst_mac, dst_ip = req.chaddr, req.ciaddr
        else:
            dst_mac, dst_ip = req.chaddr, yiaddr

        log(f"-> {MSG_NAMES[msg_type]:8} {mac_str(req.chaddr)} yiaddr={yiaddr} to {dst_ip}")
        return build_frame(self.iface.mac, dst_mac, self.ip, dst_ip, payload)


class RogueServer(Exception):
    pass


# --- BPF ------------------------------------------------------------------

def open_bpf(name: str):
    for i in range(256):
        try:
            fd = os.open(f"/dev/bpf{i}", os.O_RDWR)
            break
        except FileNotFoundError:
            sys.exit("dhcpd: no free /dev/bpf device")
        except PermissionError:
            sys.exit("dhcpd: no permission for /dev/bpf*; install Wireshark's ChmodBPF or join access_bpf")
        except OSError as e:
            if e.errno == 16:  # EBUSY
                continue
            raise
    else:
        sys.exit("dhcpd: no free /dev/bpf device")
    ioctl(fd, BIOCSETIF, struct.pack("16s16x", name.encode()))
    ioctl(fd, BIOCIMMEDIATE, struct.pack("I", 1))
    ioctl(fd, BIOCSHDRCMPLT, struct.pack("I", 1))  # we supply the source MAC
    ioctl(fd, BIOCSSEESENT, struct.pack("I", 0))   # don't read back our own frames
    blen = struct.unpack("I", ioctl(fd, BIOCGBLEN, struct.pack("I", 0)))[0]
    return fd, blen


def bpf_frames(buf: bytes):
    off = 0
    while off + 18 <= len(buf):
        _sec, _usec, caplen, _datalen, hdrlen = struct.unpack_from("=IIIIH", buf, off)
        yield buf[off + hdrlen:off + hdrlen + caplen]
        off += (hdrlen + caplen + BPF_ALIGNMENT - 1) & ~(BPF_ALIGNMENT - 1)


# --- CLI ------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--iface", default="en5")
    ap.add_argument("--pin", action="append", default=[], metavar="MAC=IP",
                    help="always give this MAC this IP (repeatable)")
    ap.add_argument("--pool", default="100-150", metavar="FIRST-LAST",
                    help="host numbers within the interface's subnet (default 100-150)")
    ap.add_argument("--lease", type=int, default=12 * 3600, help="lease seconds (default 12h)")
    ap.add_argument("--leases-file", type=Path, default=DEFAULT_LEASES)
    args = ap.parse_args()

    iface = preflight(args.iface)
    net = iface.addr.network

    first, last = (int(n) for n in args.pool.split("-"))
    pool = [net[n] for n in range(first, last + 1) if net[n] != iface.addr.ip]

    pins = {}
    for pin in args.pin:
        mac, ip = pin.split("=")
        ip = ipaddress.IPv4Address(ip)
        if ip not in net or ip == iface.addr.ip:
            sys.exit(f"dhcpd: pin {ip} is not a usable address in {net}")
        pins[mac_str(mac_bytes(mac))] = ip

    server = Server(iface, pins, pool, args.lease, args.leases_file)
    fd, blen = open_bpf(iface.name)
    log(f"serving {net} on {iface.name} ({mac_str(iface.mac)}, {iface.addr.ip}); "
        f"pool {pool[0]}-{pool[-1]}, pins {pins or 'none'}; no router/DNS. Ctrl-C to stop")

    try:
        while True:
            ready, _, _ = select.select([fd], [], [], 1.0)
            if not ready:
                continue
            for frame in bpf_frames(os.read(fd, blen)):
                reply = server.handle(frame)
                if reply:
                    os.write(fd, reply)
    except RogueServer as e:
        sys.exit(f"dhcpd: {e}; stopping")
    except KeyboardInterrupt:
        log("stopped")
    finally:
        os.close(fd)


if __name__ == "__main__":
    main()
