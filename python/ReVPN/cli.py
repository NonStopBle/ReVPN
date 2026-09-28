"""
ReVPN-py — same Hamachi-style CLI / nmtui-style TUI as the bash `ReVPN`
wrapper in ../../ (ReVPN/), driving a pure-Python engine instead of the C++
one.

Wire-compatible with ReVPN-cpp (ReVPN/engine/meshvpn.cpp): a ReVPN-py
server/client and a ReVPN-cpp server/client can be mixed freely, because
protocol.py packs the exact same bytes the C++ struct casts read.

Runs anywhere Python 3 does — Linux, macOS (server/stress only), and
Windows (server + stress out of the box; client needs Wintun, see
tun.py and README.md).

This module is what `pip install .` wires up as the `ReVPN` console
script (see pyproject.toml: `ReVPN = "ReVPN.cli:main"`), so once installed
`ReVPN` is on PATH and callable from anywhere, same as the bash version.
"""
from __future__ import annotations

import argparse
import json
import os
import platform
import sys
from pathlib import Path

from . import __version__
from .server import Server
from .stress import run_stress
from .tun import TunError

DEFAULTS = {
    "port": 9000,
    "port_set": False,
    "workers": 4,          # accepted for parity with ReVPN-cpp; unused here
    "xdp_iface": "",       # accepted for parity; AF_XDP is Linux/C++ only
    "xdp_copy": False,
    "vpn_ip": "",
    "connect": "",
    "subnet": 16,
    "mtu": 1380,
    "node_id": "",
    "encrypt": True,
    "relay_only": False,
    "st_clients": 20,
    "st_duration": 10,
    "st_rate": 500,
    "st_size": 512,
    "st_port": 19099,
}


# ============================================================================
# Preset — cross-platform config dir, shared meaning with the bash ReVPN's
# preset (same fields) but its own file, since the two are separate programs.
# ============================================================================
def config_dir() -> Path:
    if platform.system() == "Windows":
        base = os.environ.get("APPDATA", str(Path.home()))
        return Path(base) / "ReVPN-py"
    base = os.environ.get("XDG_CONFIG_HOME", str(Path.home() / ".config"))
    return Path(base) / "ReVPN-py"


def config_file() -> Path:
    return config_dir() / "preset.json"


def load_preset(cfg: dict):
    f = config_file()
    if f.exists():
        try:
            cfg.update(json.loads(f.read_text()))
        except (json.JSONDecodeError, OSError):
            pass


def save_preset(cfg: dict):
    d = config_dir()
    d.mkdir(parents=True, exist_ok=True)
    config_file().write_text(json.dumps(cfg, indent=2))


def reset_preset():
    f = config_file()
    if f.exists():
        f.unlink()


# ============================================================================
# --config <file> — same flat "key: value" YAML the bash ReVPN reads (see
# ../../config/*.yaml); no PyYAML dependency, so Windows needs nothing
# extra installed.
# ============================================================================
def load_yaml_config(cfg: dict, path: str):
    p = Path(path)
    if not p.exists():
        print(f"ReVPN: config file not found: {path}", file=sys.stderr)
        sys.exit(1)

    key_map = {
        "port": ("port", int), "workers": ("workers", int),
        "xdp_iface": ("xdp_iface", str), "xdp_copy": ("xdp_copy", _to_bool),
        "connect": ("connect", str), "vpn_ip": ("vpn_ip", str),
        "subnet": ("subnet", int), "node_id": ("node_id", str),
        "mtu": ("mtu", int), "encrypt": ("encrypt", _to_bool),
        "relay_only": ("relay_only", _to_bool),
        "clients": ("st_clients", int), "duration": ("st_duration", float),
        "rate": ("st_rate", float), "size": ("st_size", int),
        "stress_port": ("st_port", int),
    }
    for raw in p.read_text().splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line or ":" not in line:
            continue
        key, val = line.split(":", 1)
        key = key.strip()
        val = val.strip().strip('"').strip("'")
        if key in key_map and val != "":
            dest, caster = key_map[key]
            try:
                cfg[dest] = caster(val)
            except ValueError:
                pass
            if key == "port":
                cfg["port_set"] = True
    print(f"ReVPN: loaded config {path}")


def _to_bool(v: str) -> bool:
    return str(v).strip().lower() in ("true", "1", "yes", "on")


# ============================================================================
# Launchers
# ============================================================================
def launch_server(cfg: dict):
    save_preset(cfg)
    if cfg["xdp_iface"]:
        print("ReVPN: --xdp-iface is ignored by ReVPN-py (AF_XDP is Linux/C++-only); "
              "falling back to plain sockets.")
    print(f"ReVPN: starting server on 0.0.0.0:{cfg['port']}")
    Server("0.0.0.0", int(cfg["port"])).run()


def launch_client(cfg: dict):
    if not cfg["vpn_ip"]:
        print("ReVPN: --client requires --vpn-ip <ip>", file=sys.stderr)
        sys.exit(1)
    if not cfg["connect"]:
        print("ReVPN: --client requires --connect <ip:port>", file=sys.stderr)
        sys.exit(1)

    ip, _, port_s = cfg["connect"].rpartition(":")
    if not ip or not port_s.isdigit():
        print("ReVPN: --connect must be ip:port", file=sys.stderr)
        sys.exit(1)
    server_addr = (ip, int(port_s))

    node_id = int(cfg["node_id"], 16) if cfg["node_id"] else None
    local_port = int(cfg["port"]) if cfg["port_set"] else 51820
    comm = "relay" if cfg["relay_only"] else "p2p"

    print(f"ReVPN: joining {cfg['connect']} as {cfg['vpn_ip']}/{cfg['subnet']}  "
          f"(mode={comm}, encrypt={cfg['encrypt']})")
    if comm == "p2p":
        print("ReVPN: will try direct UDP hole punching, auto-relay on failure")

    save_preset(cfg)
    from .client import Client
    try:
        client = Client(
            vpn_ip=cfg["vpn_ip"], server_addr=server_addr, subnet=int(cfg["subnet"]),
            mtu=int(cfg["mtu"]), local_port=local_port, node_id=node_id,
            encrypt=bool(cfg["encrypt"]), p2p=(comm == "p2p"),
        )
    except TunError as e:
        print(f"ReVPN: {e}", file=sys.stderr)
        sys.exit(1)
    client.run()


def launch_stress(cfg: dict):
    save_preset(cfg)
    run_stress(clients=int(cfg["st_clients"]), duration=float(cfg["st_duration"]),
               rate=float(cfg["st_rate"]), size=int(cfg["st_size"]), port=int(cfg["st_port"]))


# ============================================================================
# TUI — plain input()-based menu, deliberately dependency-free (no curses)
# so it runs the same in a Linux terminal and a Windows cmd/PowerShell
# window with nothing extra installed.
# ============================================================================
def ask(prompt: str, default):
    raw = input(f"{prompt} [{default}]: ").strip()
    return raw if raw else str(default)


def ask_bool(prompt: str, default: bool) -> bool:
    d = "Y/n" if default else "y/N"
    raw = input(f"{prompt} ({d}): ").strip().lower()
    if not raw:
        return default
    return raw in ("y", "yes")


def tui_server_plain(cfg: dict):
    print("\n-- Server Settings --")
    cfg["port"] = int(ask("Port to listen on", cfg["port"]))
    cfg["port_set"] = True
    print("\nStart Server:")
    print(f"  Port : {cfg['port']}")
    if ask_bool("Proceed", True):
        launch_server(cfg)


def tui_client_plain(cfg: dict):
    print("\n-- Client Settings --")
    cfg["connect"] = ask("Address of the server to join, ip:port", cfg["connect"] or "203.0.113.10:9000")
    cfg["vpn_ip"] = ask("This node's VPN IP", cfg["vpn_ip"] or "10.13.0.2")
    cfg["subnet"] = int(ask("VPN network prefix length", cfg["subnet"]))
    cfg["encrypt"] = ask_bool("Encrypt tunnel traffic", cfg["encrypt"])
    punch = ask_bool("Try direct UDP hole punching first (auto relay fallback)",
                       not cfg["relay_only"])
    cfg["relay_only"] = not punch
    print("\nJoin as Client:")
    print(f"  Server  : {cfg['connect']}")
    print(f"  VPN IP  : {cfg['vpn_ip']}/{cfg['subnet']}")
    print(f"  Encrypt : {cfg['encrypt']}")
    print(f"  Mode    : {'relay-only' if cfg['relay_only'] else 'p2p + auto relay fallback'}")
    if ask_bool("Proceed", True):
        launch_client(cfg)


def tui_stress_plain(cfg: dict):
    print("\n-- Stress Test Settings --")
    cfg["st_clients"] = int(ask("Simulated clients", cfg["st_clients"]))
    cfg["st_duration"] = float(ask("Test length, seconds", cfg["st_duration"]))
    cfg["st_rate"] = float(ask("Packets/sec per client", cfg["st_rate"]))
    cfg["st_size"] = int(ask("Payload bytes/packet", cfg["st_size"]))
    if ask_bool("Proceed", True):
        launch_stress(cfg)


def tui_main_plain(cfg: dict):
    try:
        while True:
            print(f"\nReVPN-py {__version__} — Main Menu — Creative By Rezier Labs")
            print("  1) Server  - start this machine as the relay/rendezvous server")
            print("  2) Client  - join a ReVPN server as a client")
            print("  3) Stress  - stress-test the relay server (no root/admin needed)")
            print("  4) Help")
            print("  5) Quit")
            choice = input("Choose [1-5]: ").strip()
            if choice == "1":
                tui_server_plain(cfg)
            elif choice == "2":
                tui_client_plain(cfg)
            elif choice == "3":
                tui_stress_plain(cfg)
            elif choice == "4":
                print_help()
            elif choice in ("5", "", "q", "quit"):
                return
            else:
                print("Unknown choice.")
    except (EOFError, KeyboardInterrupt):
        print()
        return


# ============================================================================
# Curses TUI — full-screen, boxed, arrow-key driven: the Python counterpart
# to the bash ReVPN's whiptail menu. Same fields, same order, same Confirm
# wording. Falls back to tui_main_plain() above if curses isn't usable.
# ============================================================================
def _curses_server_form(stdscr, cfg):
    from . import curses_tui as ct
    cfg["port"] = int(ct.input_box(stdscr, __version__, "Server: UDP Port",
                                    "Port to listen on", cfg["port"]))
    cfg["port_set"] = True
    return ct.yesno(stdscr, __version__, "Confirm",
                     f"Start Server (relay / rendezvous):\n\n  Port: {cfg['port']}\n\nProceed?", True)


def _curses_client_form(stdscr, cfg):
    from . import curses_tui as ct
    cfg["connect"] = ct.input_box(stdscr, __version__, "Client: Server Address",
                                    "Address of the server to join, ip:port",
                                    cfg["connect"] or "203.0.113.10:9000")
    cfg["vpn_ip"] = ct.input_box(stdscr, __version__, "Client: VPN IP",
                                   "This node's VPN IP", cfg["vpn_ip"] or "10.13.0.2")
    cfg["subnet"] = int(ct.input_box(stdscr, __version__, "Client: Subnet Prefix",
                                       "VPN network prefix length", cfg["subnet"]))
    cfg["encrypt"] = ct.yesno(stdscr, __version__, "Encryption",
                                "Encrypt tunnel traffic?", cfg["encrypt"])
    punch = ct.yesno(stdscr, __version__, "Hole Punching",
                       "Try direct UDP hole punching between peers first,\n"
                       "and automatically fall back to relaying through\n"
                       "the server if punching fails?\n\n"
                       "(Choose 'No' to force relay-only.)", not cfg["relay_only"])
    cfg["relay_only"] = not punch
    mode_s = "Relay-only" if cfg["relay_only"] else "P2P + auto relay fallback"
    return ct.yesno(stdscr, __version__, "Confirm",
                     f"Join as Client:\n\n"
                     f"  Server  : {cfg['connect']}\n"
                     f"  VPN IP  : {cfg['vpn_ip']}/{cfg['subnet']}\n"
                     f"  Encrypt : {cfg['encrypt']}\n"
                     f"  Mode    : {mode_s}\n\nProceed?", True)


def _curses_stress_form(stdscr, cfg):
    from . import curses_tui as ct
    cfg["st_clients"] = int(ct.input_box(stdscr, __version__, "Stress Test: Clients",
                                           "Simulated clients", cfg["st_clients"]))
    cfg["st_duration"] = float(ct.input_box(stdscr, __version__, "Stress Test: Duration",
                                              "Test length, seconds", cfg["st_duration"]))
    cfg["st_rate"] = float(ct.input_box(stdscr, __version__, "Stress Test: Rate",
                                          "Packets/sec per client", cfg["st_rate"]))
    cfg["st_size"] = int(ct.input_box(stdscr, __version__, "Stress Test: Size",
                                        "Payload bytes/packet", cfg["st_size"]))
    return ct.yesno(stdscr, __version__, "Confirm",
                     f"Run Relay-Server Stress Test:\n\n"
                     f"  Clients : {cfg['st_clients']}\n"
                     f"  Duration: {cfg['st_duration']}s\n"
                     f"  Rate    : {cfg['st_rate']} pps/client\n"
                     f"  Size    : {cfg['st_size']} bytes\n\n"
                     f"No root/admin or TUN device needed. Proceed?", True)


def _curses_main(stdscr, cfg):
    import curses
    from . import curses_tui as ct
    curses.curs_set(0)
    while True:
        try:
            choice = ct.menu(
                stdscr, __version__, "ReVPN-py — Main Menu",
                "No arguments given — pick what to do (like nmtui):",
                [("Server", "Start this machine as the relay/rendezvous server"),
                 ("Client", "Join a ReVPN server as a client"),
                 ("Stress", "Stress-test the relay server (no root/admin needed)"),
                 ("Help", "Show full --help text"),
                 ("Quit", "Exit")],
            )
        except ct.Cancelled:
            return

        try:
            if choice == "Server":
                if _curses_server_form(stdscr, cfg):
                    curses.endwin()
                    launch_server(cfg)
            elif choice == "Client":
                if _curses_client_form(stdscr, cfg):
                    curses.endwin()
                    launch_client(cfg)
            elif choice == "Stress":
                if _curses_stress_form(stdscr, cfg):
                    curses.endwin()
                    launch_stress(cfg)
            elif choice == "Help":
                ct.msgbox(stdscr, __version__, "ReVPN-py --help", help_text())
            elif choice == "Quit":
                return
        except ct.Cancelled:
            continue


def tui_main(cfg: dict):
    """Entry point: try the full-screen curses menu, fall back to plain input()."""
    use_curses = sys.stdin.isatty() and sys.stdout.isatty()
    curses = None
    import_error = None
    if use_curses:
        try:
            import curses
        except ImportError as e:
            use_curses = False
            import_error = e

    if not use_curses and import_error is not None and platform.system() == "Windows":
        # On Windows this almost always means windows-curses isn't
        # installed/bundled — tell the user exactly what to do instead of
        # silently dropping to the plain menu with no explanation.
        print(f"ReVPN: full-screen menu unavailable ({import_error}).")
        print("       pip install windows-curses   # then re-run for the full menu")
        print("       Falling back to the plain menu for now.\n")

    if use_curses:
        try:
            curses.wrapper(_curses_main, cfg)
            return
        except Exception as e:
            # Any curses failure (not just curses.error — e.g. a broken
            # terminfo/console setup) should fall back visibly, not
            # silently swap in a different-looking menu with no reason
            # given.
            print(f"ReVPN: curses TUI failed to start ({e!r}), falling back to plain menu.")

    tui_main_plain(cfg)


# ============================================================================
# --help
# ============================================================================
def help_text() -> str:
    return f"""ReVPN-py {__version__} — cross-platform port of ReVPN (same protocol as ReVPN-cpp)
Creative By Rezier Labs

USAGE:
  ReVPN                                  Launch the interactive menu (like nmtui)
  ReVPN --server  [options]              Start this machine as the relay/rendezvous server
  ReVPN --client  [options]              Join a ReVPN server as a client (creates a TUN device)
  ReVPN --stress  [options]              Load-test the relay server (no root/admin needed)
  ReVPN --reset                          Clear saved preset, then open the menu
  ReVPN --server|--client --config F     Load settings from a YAML file (same format as
                                          ../config/server.yaml / client.yaml)
  ReVPN --help, -h                       Show this help
  ReVPN --version                        Show version

SERVER OPTIONS:  --port <n>  (default 9000)
CLIENT OPTIONS:  --connect <ip:port>  --vpn-ip <ip>  --subnet <n>  --port <n>
                 --node-id <hex>  --mtu <n>  --encrypt <true|false>  --relay-only
STRESS OPTIONS:  --clients <n>  --duration <s>  --rate <pps>  --size <bytes>  --stress-port <n>

By default --client always tries direct UDP hole punching first and
auto-falls-back to relaying through the server if that fails. Interoperates
directly with ReVPN-cpp servers/clients (../) — same wire protocol.

Windows: --server and --stress work out of the box (stdlib sockets only).
--client additionally needs a TUN adapter — install Wintun (wintun.net) and
run as Administrator; see ReVPN/tun.py.
"""


def print_help():
    print(help_text())


# ============================================================================
# Entry point
# ============================================================================
def _fix_windows_console():
    """
    Windows consoles default to a legacy codepage (437/850/...), not UTF-8,
    so plain print()'d text containing e.g. an em dash ('—') comes out as
    mojibake ('�') even though the exact same string is fine on Linux.
    curses itself (PDCurses's WinCon backend) draws via a wide-character
    Win32 API and isn't affected by this — only the plain print()-based
    help text/menu are. Best-effort: failures here are non-fatal.
    """
    if platform.system() != "Windows":
        return
    try:
        import ctypes
        ctypes.windll.kernel32.SetConsoleOutputCP(65001)
        ctypes.windll.kernel32.SetConsoleCP(65001)
    except Exception:
        pass
    for stream_name in ("stdout", "stderr"):
        stream = getattr(sys, stream_name)
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure:
            try:
                reconfigure(encoding="utf-8")
            except Exception:
                pass


def main():
    _fix_windows_console()
    cfg = dict(DEFAULTS)

    argv = sys.argv[1:]
    if "--help" in argv or "-h" in argv:
        print_help()
        return
    if "--version" in argv:
        print(f"ReVPN-py {__version__}")
        return

    reset = "--reset" in argv
    if reset:
        argv = [a for a in argv if a != "--reset"]

    config_path = None
    if "--config" in argv:
        i = argv.index("--config")
        config_path = argv[i + 1]
        argv = argv[:i] + argv[i + 2:]

    if reset:
        reset_preset()
        print("ReVPN: saved preset cleared — using defaults")
    else:
        load_preset(cfg)

    if config_path:
        load_yaml_config(cfg, config_path)

    if not argv:
        tui_main(cfg)
        return

    ap = argparse.ArgumentParser(add_help=False)
    ap.add_argument("--server", action="store_true")
    ap.add_argument("--client", action="store_true")
    ap.add_argument("--stress", action="store_true")
    ap.add_argument("--port", type=int)
    ap.add_argument("--workers", type=int)
    ap.add_argument("--xdp-iface")
    ap.add_argument("--xdp-copy", action="store_true")
    ap.add_argument("--connect")
    ap.add_argument("--vpn-ip")
    ap.add_argument("--subnet", type=int)
    ap.add_argument("--node-id")
    ap.add_argument("--mtu", type=int)
    ap.add_argument("--encrypt", choices=["true", "false"])
    ap.add_argument("--relay-only", action="store_true")
    ap.add_argument("--clients", type=int)
    ap.add_argument("--duration", type=float)
    ap.add_argument("--rate", type=float)
    ap.add_argument("--size", type=int)
    ap.add_argument("--stress-port", type=int)
    args = ap.parse_args(argv)

    if args.port is not None: cfg["port"] = args.port; cfg["port_set"] = True
    if args.workers is not None: cfg["workers"] = args.workers
    if args.xdp_iface is not None: cfg["xdp_iface"] = args.xdp_iface
    if args.xdp_copy: cfg["xdp_copy"] = True
    if args.connect is not None: cfg["connect"] = args.connect
    if args.vpn_ip is not None: cfg["vpn_ip"] = args.vpn_ip
    if args.subnet is not None: cfg["subnet"] = args.subnet
    if args.node_id is not None: cfg["node_id"] = args.node_id
    if args.mtu is not None: cfg["mtu"] = args.mtu
    if args.encrypt is not None: cfg["encrypt"] = (args.encrypt == "true")
    if args.relay_only: cfg["relay_only"] = True
    if args.clients is not None: cfg["st_clients"] = args.clients
    if args.duration is not None: cfg["st_duration"] = args.duration
    if args.rate is not None: cfg["st_rate"] = args.rate
    if args.size is not None: cfg["st_size"] = args.size
    if args.stress_port is not None: cfg["st_port"] = args.stress_port

    if args.server:
        launch_server(cfg)
    elif args.client:
        launch_client(cfg)
    elif args.stress:
        launch_stress(cfg)
    else:
        print_help()
        print("ReVPN: one of --server, --client or --stress is required", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
