# ReVPN

**ReVPN: TUN-based mesh networking with UDP hole punching, automatic
relay fallback, optional AF_XDP zero-copy, and a server-less mode
that needs nothing but a STUN query and a token.**

![C++17](https://img.shields.io/badge/C%2B%2B-17-blue)
![Python](https://img.shields.io/badge/Python-3.8%2B-blue)
![Transport](https://img.shields.io/badge/transport-UDP-orange)
![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20Windows-lightgrey)
![License](https://img.shields.io/badge/license-PolyForm%20Noncommercial%201.0.0-lightgrey)

Created by **Rezier Labs**.

No accounts, no cloud control plane, no dashboard to sign into. Run
one machine as the server (relay/rendezvous), run `ReVPN` on every
other computer to join — clients connect directly to each other when
they can, and automatically fall back to relaying through the server
when they can't (strict routers/firewalls, symmetric NAT). Two
interoperable implementations — a **C++ engine** and a **Python
port** — speak the exact same wire protocol, so a Linux server, a
Windows client, and a Python client can all sit on one mesh together.

</div>

---

## Abstract

This project began with a simple question: how can a public server
reach *me* — answer my requests, accept my connections — when I never
configured port forwarding on my own router, and never opened a
single port? I spent a long time researching that question before the
answer clicked: **UDP hole punching**.

In short, most home routers use NAT (Network Address Translation) to
share one public IP across every device behind them, and by default
they silently drop unsolicited inbound traffic — which is exactly why
port forwarding is normally required for anyone outside to reach you.
UDP hole punching works around this without touching the router at
all: both peers send a UDP packet *toward each other* at roughly the
same time, using addresses learned through a third party (a
rendezvous/signaling server) that both sides can already reach. Each
outbound packet causes the router's NAT table to open a temporary
"hole" — a mapping that lets return traffic from that specific remote
address back in, purely as a side effect of the connection having been
initiated from the inside. Once both sides' packets cross, that hole
lines up on both ends, and the two peers can talk to each other
directly — no forwarded port, no router configuration, no third party
in the traffic path once the connection is established.

Understanding that mechanism was, honestly, one of the more exciting
moments I've had studying real-time systems over the WAN. It led
directly to this project: **ReVPN** takes UDP hole punching and uses
it to bridge a virtual network interface (TUN) peer-to-peer between
machines — conceptually similar to WireGuard, but deliberately simpler
and more compact, built specifically to solve the everyday case of a
PC sitting behind a NAT/router with no port forwarding available (a
locked-down home router, a college dorm network, a mobile hotspot).

That idea goes further in **decentralized mode**
(`ReVPN --decentralized`, [Step 8](#step-8--decentralized-mode-no-server-at-all)):
no relay/rendezvous server at all, not even for setup. Instead of a
server both sides connect to, each side asks a public STUN server
"what does the internet see me as?", packs the answer into a short
token, and the two people exchange that token out-of-band — a chat
message, a voice call, however — then both sides use it to punch
straight through to each other. It's a real VPN peer once connected —
same TUN device, same wire protocol, same encryption as `--client` —
just with the server removed from the equation entirely, not only
from the data path but from the setup itself. Verified working live
between two independently-NATed machines on different networks: both
sides punched through on the first attempt.

---

## Quickstart

```sh
cd ReVPN
./build.sh                                        # build the engine once

sudo ./ReVPN --server --port 9000                  # on the server machine
sudo ./ReVPN --client --connect <server-ip>:9000 --vpn-ip 10.13.0.2   # on each client
```

Give every client a **different** `--vpn-ip` (`10.13.0.2`, `10.13.0.3`, ...).
That's a working mesh — ping, SSH, file shares, or game between any two
clients using their `10.13.0.x` addresses, same as a LAN.

No flags handy? Run `./ReVPN` with no arguments for a full-screen menu
instead — pick Server or Client, fill in the same fields interactively,
and it remembers what you entered for next time.

---

## Table of contents

- [Abstract](#abstract)
- [Quickstart](#quickstart)
- [Step 1 — Build it (Linux)](#step-1--build-it-linux)
- [Step 2 — Start the server](#step-2--start-the-server)
- [Step 3 — Connect a client](#step-3--connect-a-client)
- [Step 4 — Prefer a menu over flags?](#step-4--prefer-a-menu-over-flags)
- [Step 5 — Common adjustments](#step-5--common-adjustments)
- [Step 6 — Windows](#step-6--windows)
- [Step 7 — Server-as-peer (server joins the mesh too)](#step-7--server-as-peer-server-joins-the-mesh-too)
- [Step 8 — Decentralized mode (no server at all)](#step-8--decentralized-mode-no-server-at-all)
- [Step 9 — Test it before relying on it](#step-9--test-it-before-relying-on-it)
- [What's in this folder](#whats-in-this-folder)
- [Technical](#technical)
  - [Architecture](#architecture)
  - [The TUI](#the-tui)
  - [Saved settings (presets) and `--config`](#saved-settings-presets-and---config)
  - [Stress test internals](#stress-test-internals)
  - [Windows builds — what exists and what's verified](#windows-builds--what-exists-and-whats-verified)
  - [Layout](#layout)

---

## Step 1 — Build it (Linux)

```sh
cd ReVPN
./build.sh
```

This builds `build/ReVPN-engine`, the program the `ReVPN` command drives.

---

## Step 2 — Start the server

Pick one computer with a public/reachable IP to be the server everyone
else connects through:

```sh
sudo ./ReVPN --server --port 9000
```

Leave this running. It doesn't join the network itself by default — it
just introduces the clients to each other and relays traffic if needed
(see [Step 7](#step-7--server-as-peer-server-joins-the-mesh-too) if you
want the server machine reachable on the mesh too).

---

## Step 3 — Connect a client

On every other computer you want on the network:

```sh
sudo ./ReVPN --client --connect <server-ip>:9000 --vpn-ip 10.13.0.2
```

Give each computer a **different** `--vpn-ip` — that's its address on
the private network. The next computer would use `--vpn-ip 10.13.0.3`,
and so on.

That's a working mesh: connect any two clients using their `10.13.0.x`
addresses (ping, SSH, file shares, games — whatever you'd normally do
over a LAN).

---

## Step 4 — Prefer a menu over flags?

Run `./ReVPN` with no arguments — a full-screen menu opens. Pick Server
or Client and fill in the same fields interactively; it remembers what
you entered last time and pre-fills it next time.

---

## Step 5 — Common adjustments

- `--encrypt false` — turn off encryption
- `--relay-only` — never attempt a direct connection, always relay
  through the server (useful behind very strict firewalls)
- `./ReVPN --help` — every flag, with defaults

---

## Step 6 — Windows

Two options:

- **No Python needed:** run the prebuilt engine directly, or with the
  bundled menu wrapper — `ReVPN.bat` (server fully works; client needs
  Wintun, see below), or
  `ReVPN-engine.exe --mode server --bind 0.0.0.0:9000`.
- **Full menu, like the Linux version:** install the Python version and
  run `ReVPN` (see `python/README.md`) — this one also has the
  interactive menu.

Windows can run the **server** fully today. The **client** loads
[Wintun](https://www.wintun.net/) dynamically at startup — download
`wintun.dll` and place it next to `ReVPN-engine.exe`, then run as
Administrator. This path is untested on real Windows hardware (built
and run so far only under Wine, which has no Wintun driver to actually
exercise it against) — if it doesn't work, fall back to the Linux
client, WSL, or ReVPN-py's independent Wintun backend.

---

## Step 7 — Server-as-peer (server joins the mesh too)

By default the server is a pure relay with no VPN identity of its own.
Add `--vpn-ip` to server mode (Linux only) to have the server machine
open its own TUN device and join the mesh as a regular peer — clients
can then reach the server itself at that address, not just relay
through it for each other:

```sh
sudo ./ReVPN --server --port 9000 --vpn-ip 10.13.0.1
```

The TUI's Server form asks the same question ("Also join the mesh
yourself?") and fills in `--vpn-ip`/`--subnet` for you if you say yes.

---

## Step 8 — Decentralized mode (no server at all)

Everything above needs one machine running `--server` that both sides
can reach. Decentralized mode drops that requirement entirely — peers
connect **directly**, with nothing in between but a public STUN
server (used only to ask "what does the internet see me as?") and a
short token each pair exchanges by hand — paste it into a chat,
read it over a call, whatever. See the [Abstract](#abstract) for the
UDP hole punching concept behind this.

This isn't limited to two peers — it's a real mesh. Every peer prints
its own token and accepts one or more tokens back, so a group of 3+
just exchanges tokens all-around (everyone needs everyone else's,
since there's no server to introduce them).

On each side:

```sh
# Alice:
sudo ./ReVPN --decentralized --id alice --vpn-ip 10.13.0.2

# Bob:
sudo ./ReVPN --decentralized --id bob --vpn-ip 10.13.0.3
```

Each side prints a token and then waits for the other side's:

```
============================================================
 Step 1 — send this token to EVERY peer you want to mesh with
 (chat, email, voice):
============================================================

  YWxpY2UsNDkuMjM3LjE3NC4xODksOTM3OCwzMzU1Nzc3MCw1MDMwOTk1NzA=

============================================================
 Step 2 — paste each peer's token below, one per line.
 Leave a line blank when you're done adding peers.
============================================================

Peer's token (blank to finish):
```

Send Alice's token to Bob (and Bob's back to Alice) any way you like —
chat, email, read it aloud. Once each side has pasted the other's
token in, both hole-punch straight to each other and a real TUN
device comes up on each side (`10.13.0.2` and `10.13.0.3` here) —
`ping`, SSH, anything, same as `--client`/`--server`, just with no
server in the picture at any point.

For a group of 3+, everybody pastes in one line per peer (Alice
pastes Bob's and Carol's tokens, Bob pastes Alice's and Carol's, and
so on), then hits blank/enter to start punching to all of them.

No arguments? The TUI has a **Decentralized** entry on the main menu
that asks for your name and VPN IP, then walks you through the same
token exchange on a plain screen.

```
--vpn-ip     ip     This node's VPN IP, e.g. 10.13.0.2       (required)
--id         name   Your display name, shown in the token    (required)
--port       n      Local UDP port                            (default: 51001)
--peer-token <tok>  A peer's token, as a flag instead of being prompted
                     for it. Repeat the flag (or comma-separate) for a
                     mesh of 3+ peers.
--encrypt <true|false>  Encrypt tunnel traffic               (default: true)
```

There's no relay to fall back on here — if the direct UDP path ever
dies on both sides (NAT remapped, network changed), the fix is a
fresh token exchange and a restart, not a server hop.

---

## Step 9 — Test it before relying on it

No second computer handy? Simulate one:

```sh
./ReVPN --stress --clients 200 --duration 30
```

This throws 200 simulated clients at your server locally, no admin
rights or second machine needed, and prints a report (throughput,
packet loss) so you can sanity-check a server before depending on it.

---

## What's in this folder

- `ReVPN` — the command you run (also opens the menu with no arguments)
- `python/` — a Python version of the same tool, for Windows and anyone
  who'd rather not compile C++ (see `python/README.md`)
- `config/` — example settings files (`--config path/to/file.yaml`)
- everything else (`engine/`, `build*.sh`, `tools/`) is internal — see
  [Layout](#layout) below

---

# Technical

## Architecture

- **Server** (`--mode server` in the engine): a stateless UDP
  rendezvous/relay. No TUN device, no VPN identity of its own by
  default — it just remembers each client's public `ip:port` and
  forwards packets between them. Multi-threaded with `SO_REUSEPORT` +
  `recvmmsg` batching on Linux. Pass `--vpn-ip` (Linux only — see
  [Step 7](#step-7--server-as-peer-server-joins-the-mesh-too)) to have
  it also open a TUN device and join the mesh as a regular peer;
  clients already route unknown/server-bound traffic to the server's
  address by default, so no client-side change is needed.
- **Client**: opens a TUN device (`tun0`), registers with the server,
  and runs a hole-punching state machine per peer:
  `PUNCHING → (ack) → DIRECT`, or `PUNCHING → (10s timeout) →
  FALLBACK` (relay via server). A `DIRECT` link that goes silent for
  3s drops back to `FALLBACK`; fallback retries punching again every
  30s unless you force it. Symmetric-NAT port prediction: probes
  ±1..8 around the known port on every punch.
- **Wire protocol**: fixed-size packed structs — `RegPkt` (50B,
  REGISTER/PEER_INFO), `PunchPkt`/`KaPkt` (5B), `DataHdr` (11B) +
  payload. See `engine/meshvpn.cpp`'s top comment and
  `python/ReVPN/protocol.py` for exact byte layouts. This is what
  makes the C++ engine, the Python port, and the Windows builds all
  interoperate — same bytes on the wire regardless of which one sent
  them.
- Optional AF_XDP zero-copy RX path on Linux (`--xdp-iface`, needs a
  build with `./build.sh --xdp`); falls back to `recvmmsg` everywhere
  else.
- Encryption is currently an XOR placeholder cipher (`--encrypt false`
  to disable) — swap for real crypto via `-DMESHVPN_SODIUM` if you
  need it.

## The TUI

<p align="center">
  <img width="889" height="656" alt="tui_ui" src="https://github.com/user-attachments/assets/9b6e4a38-4674-4c99-a2ae-6015085b1e22" />
</p>

Running `ReVPN` with no arguments opens a full-screen menu (`whiptail`,
falling back to `dialog`, falling back to plain `--help` text if
neither is installed) — Server / Client / Stress / Help / Quit, each
walking through the same fields the flags accept, then a confirm
screen before launching. The AF_XDP interface field is a picker built
from this machine's real `ip link` output (never free-typed), and is
only offered if the engine was built with `./build.sh --xdp` (checked
via a `build/.xdp_enabled` marker file, not by probing the binary).
The Server form also offers server-as-peer (Step 7, Linux only) and
adjusts the confirm screen to show the resulting VPN IP/subnet when
enabled.

## Saved settings (presets) and `--config`

Every launch (TUI or flags) saves what you used to
`~/.config/ReVPN/preset.conf`, and reloads it as the new defaults next
time — flags you do pass always win. `./ReVPN --reset` wipes it and
opens the menu with the original hardcoded defaults. `--config <file>`
loads a flat `key: value` YAML file instead (see `config/server.yaml`
/ `config/client.yaml`); precedence is defaults → saved preset →
`--config` file → explicit flag.

## Stress test internals

`--stress` starts a throwaway server plus N synthetic UDP "clients"
(`tools/stress_client.py`, or the Python port's in-process equivalent)
that speak just enough of the wire protocol to REGISTER and flood data
packets, arranged in a ring (client *i* → client *i+1*) so the server
does real per-packet routing-table lookups rather than a simple
broadcast fan-out. It reports wall-clock time, the server's own
observed rx/fwd byte counters (converted to pps/Mbps), and estimated
packet loss. Sample run (150 simulated clients, 8s, 400 pps each, 800B
payload, single dev machine): ~294 Mbps, ~45k packets/sec, ~4.5% loss
— useful as a relative benchmark across machines/kernels, not an
absolute number.

## Windows builds — what exists and what's verified

There are **two** separate Windows binaries:

|                            | `build-windows/ReVPN-engine.exe`                          | `python/dist/ReVPN.exe`                         |
|----------------------------|-------------------------------------------------------------|----------------------------------------------------|
| Built from                 | `engine/meshvpn.cpp` (C++), MinGW cross-compile              | `python/ReVPN.py`, PyInstaller (hosted in Wine)     |
| Has the interactive menu?  | Via `ReVPN.bat` (plain text menu)                            | Yes — same curses menu as Linux                     |
| `--server`                 | Full C++ engine (recvmmsg-class throughput)                  | Pure-Python server                                  |
| `--client` / TUN           | Own Wintun integration (dynamically loaded `wintun.dll`), unverified on real hardware | Independent Wintun backend, unverified on real hardware |
| Server-as-peer (`--vpn-ip`)| Linux only — fails cleanly on Windows with a clear message    | Not exposed on the Windows build                    |
| Stress test                | Via the bash `ReVPN --stress` wrapper, not the raw .exe       | Built in (`--stress`, or from the menu)             |

Build them with `./build_windows.sh` (needs
`g++-mingw-w64-x86-64-posix binutils-mingw-w64-x86-64`) and
`cd python && ./build_windows_exe.sh` (needs `wine`; downloads a
Windows Python + PyInstaller into a dedicated Wine prefix on first
run) respectively.

**What's actually been tested (all under Wine — no real Windows
hardware was available):**

- Both `.exe`s: `--mode server` starts, multi-worker mode works, and
  real UDP traffic from a synthetic client was correctly REGISTERed
  and routed (rx/fwd byte counters increased as expected).
- The C++ `.exe`'s `--mode client` correctly fails with a clear
  message when `wintun.dll` isn't present (Wine has no Wintun driver,
  so the real adapter-creation path — `WintunCreateAdapter`/
  `WintunStartSession`/TUN read-write — could not be exercised end to
  end; that part needs verification on real Windows hardware).
- `--mode server --vpn-ip ...` (server-as-peer) on the Windows build
  fails cleanly with an explicit "needs Linux/WSL2" message rather
  than crashing, since TUN creation there is Linux-only.
- The Python `.exe`'s menu was driven end-to-end through a pipe (menu
  → Stress form, pre-filled from a saved preset → confirm → a real
  stress run → back to the menu). Full-screen arrow-key input
  specifically wasn't confirmed working through this test's
  headless-Wine-over-pty setup (rendering was correct; key events
  didn't forward in that specific harness) — expected to work on a
  real Windows console, which reads input differently, but unconfirmed
  on real hardware.
- `pip install .` (the Python package) was verified on a **freshly
  installed** Wine-hosted Windows Python (a clean-machine proxy): it
  resolved and installed the `windows-curses` dependency
  automatically, created a working `ReVPN.exe` shim on `PATH`, and
  `ReVPN --help` / `--stress` ran correctly from an unrelated
  directory via `cmd.exe`.
- One Wine-specific quirk found (not expected on real Windows): the
  Python `.exe`'s stdout can fail with an "Invalid handle" error if
  redirected straight to a plain file; piping (`| cat`, `| tee`, into
  `tmux`, etc.) works reliably and is how all the above was tested.

## Layout

- `engine/` — the C++ engine source (`meshvpn.cpp`, `xdp_kern.c`,
  `xdp_sock.*`, `CMakeLists.txt`), originally from `../vpn`.
- `build.sh` / `build_windows.sh` — build `engine/` into
  `build/ReVPN-engine` (Linux) or `build-windows/ReVPN-engine.exe`
  (MinGW cross-compile).
- `ReVPN` — the CLI + TUI wrapper end users run.
- `stress_test.sh` / `tools/stress_client.py` — the load-test harness.
- `config/` — example `server.yaml` / `client.yaml` for `--config`.
- `python/` — `ReVPN-py`: a pure-Python, wire-compatible port of this
  same CLI/TUI/server/client (`pip install`-able, plus a standalone
  Windows `.exe` build). See `python/README.md`.
- `docs/ReVPN-architecture.drawio` — technical architecture diagram
  (server/client/decentralized modes, wire protocol, build outputs).
  Open at [app.diagrams.net](https://app.diagrams.net) (File → Open From
  → Device) or the [draw.io](https://marketplace.visualstudio.com/items?itemName=hediet.vscode-drawio)
  VS Code extension.
- `experiments/` — standalone concept tests, not part of the product
  build. See `experiments/README.md`.

## License

Created by **Rezier Labs**. Licensed under the
[PolyForm Noncommercial License 1.0.0](LICENSE) — free to use, modify,
and redistribute for any noncommercial purpose (personal, educational,
research, nonprofit). Commercial use requires a separate license from
Rezier Labs. See the [`LICENSE`](LICENSE) file for the full terms.
