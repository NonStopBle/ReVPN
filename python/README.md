# ReVPN-py

A pure-Python port of `../` (ReVPN/) — same CLI, same TUI concept, same wire
protocol — so it interoperates directly with `ReVPN-cpp` (the C++ engine
in `../engine`) and runs anywhere Python 3 does, **including
Windows**, which the C++ engine (Linux `/dev/net/tun` + `ip`/`iptables`)
does not support.

```
python/
  ReVPN.py            thin launcher for `python3 ReVPN.py` without installing
  pyproject.toml      pip package "ReVPN" — installs the `ReVPN` command
  ReVPN/
    __init__.py
    cli.py            CLI + TUI entry point (what pip wires up as `ReVPN`)
    protocol.py       wire structs — byte-identical to meshvpn.cpp's
    server.py         rendezvous/relay server (stdlib sockets only)
    client.py         TUN + hole-punch + auto relay-fallback client
    tun.py            LinuxTun (/dev/net/tun) + WindowsTun (Wintun)
    stress.py         in-process load test, same idea as ../stress_test.sh
    curses_tui.py     full-screen whiptail-style menu (curses)
  build_windows_exe.sh  builds dist/ReVPN.exe (standalone, TUI included)
```

## Install as a `ReVPN` command (pip)

```sh
cd python
python3 -m venv .venv && source .venv/bin/activate   # or use pipx, see below
pip install .            # or: pip install -e .   (editable, for development)
ReVPN --help             # now on PATH — works from any directory
```

This registers a console-script entry point (`ReVPN = "ReVPN.cli:main"` in
`pyproject.toml`), so after install `ReVPN` is a real command on `PATH` —
callable from anywhere, same as the bash version, no `python3 ReVPN.py`
needed. Verified in this repo: installed into a throwaway venv, then ran
`ReVPN --help`, `ReVPN --stress ...`, and `ReVPN --client --config <path>`
successfully from `/tmp` (nowhere near the source checkout).

For a permanent, isolated global install without manually managing a venv
(works the same way on Windows too — `pipx` puts its own `Scripts`/`bin`
dir on `PATH`):
```sh
pipx install .
```

On Windows (once you have Python + pip): `pip install .` inside a venv, or
`pipx install .`, gives you a `ReVPN.exe` shim on `PATH` the same way —
this is a lighter-weight alternative to the standalone PyInstaller
`dist/ReVPN.exe` below if the target machine already has Python.

## Standalone Windows .exe (has the full TUI, not just flags)

```sh
./build_windows_exe.sh
```

This produces `dist/ReVPN.exe` — a real, standalone Windows binary (no
Python install needed on the target machine) built with PyInstaller,
**with the full curses TUI baked in** via `windows-curses`, not a
flags-only tool. PyInstaller can't cross-compile from Linux, so the script
hosts a real Windows Python + PyInstaller inside a dedicated Wine prefix
and builds there — the output is a genuine PE32+ Windows `.exe`.

Verified end-to-end under Wine (see `../README.md` for the full test log):
`--help`, `--version`, a complete `--stress` run, and the interactive menu
driven all the way through (menu → Stress form, pre-filled from a saved
preset → confirm → real run → back to the menu) all work correctly. One
caveat found during testing: Wine's console has a quirk where redirecting
this exe's stdout straight to a plain file can fail with an "Invalid
handle" error from the embedded interpreter — piping it (`| cat`,
`| tee`, into `tmux`, etc.) works reliably and is how everything above was
tested. This is a Wine console idiosyncrasy, not expected to reproduce on
real Windows, but hasn't been confirmed on real Windows hardware.

Not fully verified: full-screen arrow-key **keyboard** navigation in the
curses TUI specifically when run under a headless Wine console driven
through a Linux pty (this test setup) — output renders correctly, but
key input didn't reach the app in that specific harness. The same
`curses_tui.py` code was separately confirmed fully interactive (arrow
keys, forms, confirm dialogs) on Linux via `ncurses`; on real Windows,
`windows-curses` reads native console input events directly rather than
going through a pty, so this is expected to work, but is unconfirmed on
actual Windows hardware.

No third-party dependencies on Linux/macOS. On Windows, `--server` and
`--stress` need nothing extra; `--client` needs the Wintun driver.

## Interop with ReVPN-cpp

`ReVPN/protocol.py` packs `RegPkt` / `DataHdr` / `PunchPkt` exactly like
`meshvpn.cpp` does — same field sizes, same byte order per field (see the
comment at the top of that file). That means:

- a **ReVPN-py server** can rendezvous **ReVPN-cpp clients** (and vice
  versa),
- a **ReVPN-py client** can hole-punch / relay against a **ReVPN-cpp
  server** (and vice versa).

Verified in this repo: a Python server (`ReVPN.py --server`) correctly
REGISTERed and routed traffic from `../tools/stress_client.py`
(a raw-protocol synthetic client using the same struct layout the C++
engine reads), and `ReVPN.py --client --config ../config/client.yml`
parses the same YAML config files the bash `ReVPN` uses.

## Usage — identical flags to the bash `ReVPN`

```sh
python3 ReVPN.py                      # interactive menu (curses, or plain input() fallback)
python3 ReVPN.py --server --port 9000
python3 ReVPN.py --client --connect <server-ip>:9000 --vpn-ip 10.13.0.2
python3 ReVPN.py --client --connect <server-ip>:9000 --vpn-ip 10.13.0.3 --encrypt false
python3 ReVPN.py --stress --clients 200 --duration 30 --rate 1000
python3 ReVPN.py --client --config ../config/client.yml
python3 ReVPN.py --reset              # clear saved preset, open the menu
python3 ReVPN.py --help
```

Precedence, same as the bash version: hardcoded defaults → saved preset
(`~/.config/ReVPN-py/preset.json`, or `%APPDATA%\ReVPN-py\preset.json` on
Windows) → `--config` YAML file → explicit CLI flag.

`--client` defaults to hole-punch-first with automatic relay fallback,
same state machine/timers as `meshvpn.cpp` (10s punch timeout, 3s direct-
link drop, 30s background retry). `--relay-only` disables punching.

## TUI

Running with no arguments opens a **full-screen curses menu** — arrow
keys + Enter, boxed forms, a Yes/No confirm screen — the same flow and
field order as the bash `ReVPN`'s `whiptail` menu (Server / Client /
Stress / Help / Quit). If curses isn't available (not a real terminal, or
no `windows-curses` installed on Windows) it falls back automatically to
a plain numbered `input()` menu — still fully usable, just not full-screen.

One difference from the bash version: because ReVPN-py runs the engine
in-process rather than `exec`-ing a separate binary, finishing or
Ctrl-C'ing a Server/Client/Stress run returns you to the menu instead of
exiting the program — pick Quit to actually exit.

## Windows notes

- **`--server` / `--stress`**: work out of the box — pure `socket` +
  `threading`, no platform-specific code.
- **`--client`**: needs a TUN-like adapter. Windows has no built-in
  `/dev/net/tun`, so this uses [Wintun](https://www.wintun.net/) (the same
  adapter tech WireGuard-for-Windows uses) via `ctypes` bindings in
  `ReVPN/tun.py`:
  1. Download `wintun.dll` and place it next to `ReVPN.py`.
  2. Run `ReVPN.py --client ...` from an **Administrator** shell.
  3. The adapter's IP/MTU are set with `netsh`, mirroring what the Linux
     backend does with `ip`/`iptables`.
- **TUI**: `pip install windows-curses` for the full-screen menu; without
  it, ReVPN-py automatically uses the plain menu instead.

## Stress testing

Same idea as `../stress_test.sh`, done in-process: a throwaway
`Server` on a background thread plus N synthetic UDP client threads
(`ReVPN/stress.py`) REGISTER and flood `MSG_DATA` in a ring (client *i* →
client *i+1*), then the server's own `rx`/`fwd` byte counters are reported
as packets/sec, Mbps, and estimated loss.

```sh
python3 ReVPN.py --stress --clients 150 --duration 8 --rate 400 --size 800
```
