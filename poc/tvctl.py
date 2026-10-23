#!/usr/bin/env python3
"""Control the Samsung TV over its local Tizen WebSocket API.

  tvctl.py info              device info from the REST endpoint (no pairing needed)
  tvctl.py pair [--force]    pair once; accept the prompt on the TV, token is saved
  tvctl.py key KEY [KEY...]  send remote keys, e.g. KEY_VOLUP KEY_INFO
  tvctl.py hdmi 1|2          switch input (--method cycle|menu)

The Q7F ignores KEY_HDMI1/KEY_HDMI2 and has no way to read the current input,
so both methods start from a known anchor:
  cycle: KEY_TV jumps to the tuner; KEY_HDMI then cycles HDMI 1 -> HDMI 2 -> TV.
  menu:  the Source menu lists inputs in one column (TV, HDMI 1, HDMI 2) and Up
         stops at TV, so Up past the top, Down N, Enter.
"""

import argparse
import html
import json
import logging
import os
import sys
import urllib.request
from pathlib import Path

from samsungtvws import SamsungTVWS
from samsungtvws.exceptions import ConnectionFailure, UnauthorizedError

REPO = Path(__file__).resolve().parent.parent
DEFAULT_HOST = os.environ.get("TV_HOST", "192.168.2.10")
DEFAULT_TOKEN_FILE = Path(os.environ.get("TV_TOKEN_FILE", REPO / ".state" / "tv-token"))
CLIENT_NAME = "monitor_control"
# Up presses to be sure the Source menu highlight is on TV (top of the input column)
MENU_MAX_ROWS = 4

INFO_FIELDS = ["name", "modelName", "OS", "ip", "networkType", "wifiMac",
               "PowerState", "TokenAuthSupport", "firmwareVersion"]


def connect(args, timeout: float) -> SamsungTVWS:
    return SamsungTVWS(host=args.host, port=8002, token_file=str(args.token_file),
                       name=CLIENT_NAME, timeout=timeout, key_press_delay=args.delay)


def cmd_info(args):
    with urllib.request.urlopen(f"http://{args.host}:8001/api/v2/", timeout=5) as resp:
        device = json.load(resp)["device"]
    if args.json:
        print(json.dumps(device, indent=2))
        return
    for field in INFO_FIELDS:
        print(f"{field:17} {html.unescape(str(device.get(field)))}")


def cmd_pair(args):
    if args.token_file.exists() and not args.force:
        sys.exit(f"tvctl: already paired ({args.token_file}); use --force to pair again")
    args.token_file.parent.mkdir(parents=True, exist_ok=True)
    args.token_file.unlink(missing_ok=True)
    print("tvctl: connecting; accept the prompt on the TV (waiting up to 60 s)")
    with connect(args, timeout=60) as tv:
        tv.open()
    if not args.token_file.exists():
        sys.exit("tvctl: connected, but the TV returned no token")
    print(f"tvctl: paired; token saved to {args.token_file}")


def cmd_key(args):
    with connect(args, timeout=10) as tv:
        for key in args.keys:
            print(f"tvctl: {key}")
            tv.send_key(key)


def cmd_hdmi(args):
    if args.method == "menu":
        steps = ([("KEY_SOURCE", args.menu_open_delay)]
                 + [("KEY_UP", args.menu_delay)] * MENU_MAX_ROWS
                 + [("KEY_DOWN", args.menu_delay)] * args.port
                 + [("KEY_ENTER", 0)])
    else:
        steps = [("KEY_TV", args.tv_delay)] + [("KEY_HDMI", args.hdmi_delay)] * args.port
    # short tail on the last key so the socket doesn't close under it
    steps[-1] = (steps[-1][0], 0.5)
    with connect(args, timeout=10) as tv:
        for key, delay in steps:
            print(f"tvctl: {key}")
            tv.send_key(key, key_press_delay=delay)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default=DEFAULT_HOST, help=f"TV address (default {DEFAULT_HOST}, env TV_HOST)")
    ap.add_argument("--token-file", type=Path, default=DEFAULT_TOKEN_FILE)
    ap.add_argument("--delay", type=float, default=0.3, help="seconds between keys (default 0.3)")
    ap.add_argument("--debug", action="store_true", help="log WebSocket traffic")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("info")
    p.add_argument("--json", action="store_true", help="print the full device record")
    p.set_defaults(func=cmd_info)

    p = sub.add_parser("pair")
    p.add_argument("--force", action="store_true", help="discard the saved token and pair again")
    p.set_defaults(func=cmd_pair)

    p = sub.add_parser("key")
    p.add_argument("keys", nargs="+", metavar="KEY")
    p.set_defaults(func=cmd_key)

    p = sub.add_parser("hdmi")
    p.add_argument("port", type=int, choices=[1, 2])
    p.add_argument("--tv-delay", type=float, default=1.5,
                   help="seconds to wait after KEY_TV before the first KEY_HDMI (default 1.5)")
    p.add_argument("--hdmi-delay", type=float, default=3.0,
                   help="seconds to wait between KEY_HDMI presses (default 3.0)")
    p.add_argument("--method", choices=["cycle", "menu"], default="menu",
                   help="menu: Source menu, no tuner static (default); cycle: KEY_TV + KEY_HDMI")
    p.add_argument("--menu-open-delay", type=float, default=1.0,
                   help="seconds to wait for the Source menu to appear (default 1.0)")
    p.add_argument("--menu-delay", type=float, default=0.5,
                   help="seconds between Source menu key presses (default 0.5)")
    p.set_defaults(func=cmd_hdmi)

    args = ap.parse_args()
    logging.basicConfig(level=logging.DEBUG if args.debug else logging.WARNING,
                        format="%(asctime)s %(name)s %(message)s")
    try:
        args.func(args)
    except UnauthorizedError:
        sys.exit("tvctl: the TV refused the connection; check Device Connection Manager, then pair --force")
    except (ConnectionFailure, OSError) as e:
        sys.exit(f"tvctl: can't reach the TV at {args.host}: {e}")


if __name__ == "__main__":
    main()
