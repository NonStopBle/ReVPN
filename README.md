<div align="center">
  
# ReVPN

<img width="2816" height="1536" alt="revpn_pre" src="https://github.com/user-attachments/assets/7110a3a8-d016-49a6-8545-a1f706829103" />

**ReVPN: TUN-based mesh networking with UDP hole punching, automatic
relay fallback, optional AF_XDP zero-copy, and a server-less mode
that needs nothing but a STUN query and a token.**

![C++17](https://img.shields.io/badge/C%2B%2B-17-blue)
![Python](https://img.shields.io/badge/Python-3.8%2B-blue)
![Transport](https://img.shields.io/badge/transport-UDP-orange)
![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20Windows-lightgrey)
![License](https://img.shields.io/badge/license-PolyForm%20Noncommercial%201.0.0-lightgrey)

Creative by **Rezier Labs**.

No accounts, no cloud control plane, no dashboard to sign into. Run
one machine as the server (relay/rendezvous), run `ReVPN.sh` on every
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

<img width="1091" height="675" alt="ReVPN-basic-communication" src="https://github.com/user-attachments/assets/613f9f6a-fa1f-43f4-aabf-d0b81a14883d" />


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
(`ReVPN.sh --decentralized`, [Step 8](#step-8--decentralized-mode-no-server-at-all)):
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

sudo ./ReVPN.sh --server --port 9000                  # on the server machine
sudo ./ReVPN.sh --client --connect <server-ip>:9000 --vpn-ip 10.13.0.2   # on each client
```

Give every client a **different** `--vpn-ip` (`10.13.0.2`, `10.13.0.3`, ...).
That's a working mesh — ping, SSH, file shares, or game between any two
clients using their `10.13.0.x` addresses, same as a LAN.

No flags handy? Run `./ReVPN.sh` with no arguments for a full-screen menu
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
- [ReVPN.sh vs calling the engine directly](#revpnsh-vs-calling-the-engine-directly)
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

<p align="center">
<img width="966" height="664" alt="meshbuild" src="https://github.com/user-attachments/assets/0782dd56-9c21-455f-a07e-4c9f709b1469" />
</p>

<p align="center"><b>Figure 1:</b> A clean <code>./build.sh</code> run, producing <code>build/ReVPN-engine</code>.</p>

<p align="center">
  <img width="842" height="614" alt="revpn_help" src="https://github.com/user-attachments/assets/009af72e-4eb7-4ce5-8390-bfd9d3ca128f" />
</p>

<p align="center"><b>Figure 2:</b> <code>./ReVPN.sh -h</code> — the five modes: server, client, decentralized, stress, help.</p>


```sh
cd ReVPN
./build.sh
```

This builds `build/ReVPN-engine`, the program the `ReVPN.sh` command drives.

---

## Step 2 — Start the server
<p align="center">
  <img width="885" height="645" alt="mesh_server" src="https://github.com/user-attachments/assets/9a2cb110-ef67-47d9-b716-75a9c0e300a0" />
</p>

<p align="center"><b>Figure 3:</b> The relay server just after startup, no clients connected yet.</p>

Pick one computer with a public/reachable IP to be the server everyone
else connects through:

```sh
sudo ./ReVPN.sh --server --port 9000
```

Leave this running. It doesn't join the network itself by default — it
just introduces the clients to each other and relays traffic if needed
(see [Step 7](#step-7--server-as-peer-server-joins-the-mesh-too) if you
want the server machine reachable on the mesh too).

---

## Step 3 — Connect a client

On every other computer you want on the network:

<p align="center">
  <img width="842" height="614" alt="image_clients" src="https://github.com/user-attachments/assets/88536864-5776-48d0-9bf4-e1220d5e848a" />
</p>

<p align="center"><b>Figure 4:</b> A client joining that server — VPN IP, node ID, and comm mode confirmed.</p>

```sh
sudo ./ReVPN.sh --client --connect <server-ip>:9000 --vpn-ip 10.13.0.2
```

Give each computer a **different** `--vpn-ip` — that's its address on
the private network. The next computer would use `--vpn-ip 10.13.0.3`,
and so on.

That's a working mesh: connect any two clients using their `10.13.0.x`
addresses (ping, SSH, file shares, games — whatever you'd normally do
over a LAN).

---

## Step 4 — Prefer a menu over flags?

Run `./ReVPN.sh` with no arguments — a full-screen menu opens. Pick Server
or Client and fill in the same fields interactively; it remembers what
you entered last time and pre-fills it next time.

---

## ReVPN.sh vs calling the engine directly

Everything above goes through `ReVPN.sh` — that's the right entry point
for interactive use, testing, and quickly trying out a config, since it
adds the TUI menu, saved presets, and `--config` YAML loading in bash
before ever touching the engine.

For a real, long-running deployment (a systemd unit, a Docker
container, an init script, anything that shouldn't depend on bash
being present) call the engine binary directly instead — it's a single
static-ish binary with no wrapper dependency, and as of the `--config`
support added to the engine itself, it can load the exact same YAML
files `ReVPN.sh` uses, with no bash in the loop at all:

```sh
# Server, straight from the binary:
sudo ./build/ReVPN-engine --mode server --config ./config/server.yaml

# Client, straight from the binary:
sudo ./build/ReVPN-engine --mode client --config ./config/client.yaml

# Decentralized, straight from the binary:
sudo ./build/ReVPN-engine --mode decentralized --id alice --vpn-ip 10.13.0.2
```

Precedence is identical either way: hardcoded defaults → `--config`
file → any flag also given on the command line. Use `ReVPN.sh` (or its
TUI) while you're figuring out settings or doing a quick manual test;
point systemd/Docker/init scripts at `./build/ReVPN-engine` directly
once you know the config you want to run for real. Run
`./build/ReVPN-engine --help` for the engine's own full flag list.

---

## Step 5 — Common adjustments

- `--encrypt false` — turn off encryption
- `--relay-only` — never attempt a direct connection, always relay
  through the server (useful behind very strict firewalls)
- `./ReVPN.sh --help` — every flag, with defaults

---

## Step 6 — Windows

Two options:

- **No Python needed:** run the prebuilt engine directly, or with the
  bundled menu wrapper — `ReVPN.bat` (server fully works; client and
  decentralized mode need Wintun, see below), or
  `ReVPN-engine.exe --mode server --bind 0.0.0.0:9000`.
- **Full menu, like the Linux version:** install the Python version and
  run `ReVPN` (see `python/README.md`) — this one also has the
  interactive menu.

First run `install.bat` (double-click it, or run from `cmd`/PowerShell).
It checks that `build-windows\ReVPN-engine.exe` exists, downloads
`wintun.dll` for your CPU architecture straight from
[wintun.net](https://www.wintun.net/) into `build-windows\` next to the
engine, and checks whether Python is on `PATH` (only needed for
`python\ReVPN.py`). Run it again any time; it skips steps that are
already done.

**The firewall matters, not just Wintun.** Without an exception,
Windows Firewall silently drops unsolicited inbound UDP — on a
peer-to-peer mode (`--client`/`--decentralized`) that looks exactly
like "my TX counter keeps climbing but RX stays at 0 forever, punch
never succeeds", because the *other* side's punch/ACK packets never
make it past the firewall to the engine. `ReVPN.bat` now handles this
itself: on every launch (any mode, from the menu or a flag) it checks
whether the `ReVPN-engine` Firewall rule exists, and if not, triggers
the normal Windows UAC "allow this app to make changes?" prompt — say
yes once, and it adds the inbound+outbound UDP rule for you and
carries on with whatever you asked for, no separate step needed.

**Tunnel working but `ping` still times out?** That's a second, separate
Windows Firewall default: inbound ICMPv4/ICMPv6 Echo Request ("File and
Printer Sharing — Echo Request") is off by default on any new network
adapter, Wintun's included — independent of the UDP program rule above.
You can have real tunnel traffic flowing (RX counters climbing, `[P2P] OK
DIRECT`) and `ping` will still report 100% loss until this is allowed too.
`ReVPN.bat` adds this rule in the same self-elevating step as the UDP one,
so a single UAC prompt covers both. If you ever need to add either by hand:
Windows Defender Firewall → Advanced Settings → Inbound Rules → New Rule →
Program → point it at `ReVPN-engine.exe` → Allow, for UDP (the tunnel rule),
and a second rule → Custom → Protocol type ICMPv4 (and ICMPv6) → Specific
ICMP types → Echo Request → Allow (the ping rule).

`ReVPN.bat` supports all three modes — `--server`, `--client`, and
`--decentralized` — through the same interactive menu or flags as the
Linux `ReVPN.sh` (see [Step 8](#step-8--decentralized-mode-no-server-at-all)
for what decentralized mode is). There's no `--stress` in the batch
version; use `python\ReVPN.py --stress` for that.

Windows can run the **server** fully today. The **client** and
**decentralized** modes load [Wintun](https://www.wintun.net/)
dynamically at startup — `install.bat` fetches `wintun.dll` for you
(or download it yourself and place it next to `ReVPN-engine.exe`),
then run as Administrator. This path is untested on real Windows
hardware (built and run so far only under Wine, which has no Wintun
driver to actually exercise it against) — if it doesn't work, fall
back to the Linux client, WSL, or ReVPN-py's independent Wintun
backend.

---

## Step 7 — Server-as-peer (server joins the mesh too)

By default the server is a pure relay with no VPN identity of its own.
Add `--vpn-ip` to server mode (Linux, or Windows via Wintun — same as
`--client`/`--decentralized`, see [Step 6](#step-6--windows)) to have
the server machine open its own TUN device and join the mesh as a
regular peer — clients can then reach the server itself at that
address, not just relay through it for each other:

```sh
sudo ./ReVPN.sh --server --port 9000 --vpn-ip 10.13.0.1
```

```bat
REM Windows — run ReVPN-engine.exe as Administrator, with wintun.dll
REM next to it (install.bat sets that up):
ReVPN.bat --server --port 9000 --vpn-ip 10.13.0.1
```

The TUI's Server form (and `ReVPN.bat`'s own menu) asks the same
question ("Also join the mesh yourself?") and fills in
`--vpn-ip`/`--subnet` for you if you say yes.

---

## Step 8 — Decentralized mode (no server at all)

<p align="center">
<img width="1102" height="785" alt="ReVPN-overview" src="https://github.com/user-attachments/assets/2def0d9d-3d39-4ab4-83a8-6f97a7a52ac6" />
</p>

<p align="center"><b>Figure 5:</b> Network topology of decentralized mode — direct tunnels between peer sites, no server.</p>

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

<p align="center">
  <img width="842" height="614" alt="mesh_decent" src="https://github.com/user-attachments/assets/be106294-e9fd-4bb8-9c13-27a48f84b389" />
</p>

<p align="center"><b>Figure 6:</b> The token exchange screen — send your token, paste peers' tokens back.</p>

On each side:

```sh
# Alice:
sudo ./ReVPN.sh --decentralized --id alice --vpn-ip 10.13.0.2

# Bob:
sudo ./ReVPN.sh --decentralized --id bob --vpn-ip 10.13.0.3
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

**Two peers behind the same router?** The token also carries each
side's LAN-facing IP, not just its public one — so if you and a peer
are testing from two machines on the same home/office network (same
public IP as seen by STUN), punching tries the LAN address too instead
of relying only on the public one. That matters because a lot of
consumer routers don't support NAT hairpinning (a packet from inside
the LAN addressed to the router's *own* public IP, meant to be routed
back to another device on the same LAN, silently gets dropped) — which
looks exactly like "TX keeps climbing, RX stays at 0 forever, punch
never succeeds" even though both sides are on, firewalled correctly,
and sending. If you hit that symptom with two peers on the same
network, update to a build with this fix (older tokens lack the LAN
fields and fall back to public-only punching).

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
./ReVPN.sh --stress --clients 200 --duration 30
```

This throws 200 simulated clients at your server locally, no admin
rights or second machine needed, and prints a report (throughput,
packet loss) so you can sanity-check a server before depending on it.

---

## What's in this folder

- `ReVPN.sh` — the command you run (also opens the menu with no arguments)
- `ReVPN.bat` — the Windows equivalent, wrapping `build-windows\ReVPN-engine.exe`
  (same `--server`/`--client`/`--decentralized` modes, plain-text menu)
- `install.bat` — run once on Windows to fetch `wintun.dll` (needed by
  `ReVPN.bat --client`/`--decentralized`) and sanity-check the engine binary
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
  `recvmmsg` batching on Linux. Pass `--vpn-ip` (Linux, or Windows via
  Wintun — see
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

<p align="center"><b>Figure 7:</b> The TUI main menu — the same five modes, picked from a whiptail screen.</p>

Running `ReVPN.sh` with no arguments opens a full-screen menu (`whiptail`,
falling back to `dialog`, falling back to plain `--help` text if
neither is installed) — Server / Client / Stress / Help / Quit, each
walking through the same fields the flags accept, then a confirm
screen before launching. The AF_XDP interface field is a picker built
from this machine's real `ip link` output (never free-typed), and is
only offered if the engine was built with `./build.sh --xdp` (checked
via a `build/.xdp_enabled` marker file, not by probing the binary).
The Server form also offers server-as-peer (Step 7, Linux or Windows)
and adjusts the confirm screen to show the resulting VPN IP/subnet when
enabled. `ReVPN.bat`'s own menu has the same question for its Server
form.

## Saved settings (presets) and `--config`

Every launch (TUI or flags) saves what you used to `config/preset.conf`
(next to `ReVPN.sh` itself, not under your home directory — this keeps
a checkout fully self-contained), and reloads it as the new defaults
next time — flags you do pass always win. `./ReVPN.sh --reset` wipes it
and opens the menu with the original hardcoded defaults. `--config <file>`
loads a flat `key: value` YAML file instead (see `config/server.yaml`
/ `config/client.yaml`); precedence is defaults → saved preset →
`--config` file → explicit flag.

`build/ReVPN-engine` itself also understands `--config <file>` —
same flat YAML format and key set (minus the stress-test-only keys,
which don't apply to the binary), same precedence, no bash or preset
file involved. That's what makes it safe to call the engine directly
in a systemd unit or Docker container: point `--config` at a file you
control and every run behaves identically, with no dependency on
`ReVPN.sh` or its saved preset file at all.

`ReVPN.sh` also **writes** one of these for you: every successful
`--server`, `--client`, or `--decentralized` launch regenerates
`config/generated-<mode>.yaml`, matching whatever settings actually
ran (flags, preset, or an earlier `--config`, whichever won), and
prints the exact `ReVPN-engine --mode ... --config ...` command to run
it directly next time. Peer tokens are one-time secrets and are never
written into it — pass `--peer-token` alongside `--config` for
decentralized mode. Try a setup through the TUI or flags once, then
hand the generated file straight to a systemd unit or container. See
[ReVPN.sh vs calling
the engine directly](#revpnsh-vs-calling-the-engine-directly) above.

The TUI's **Persistence** menu item automates the systemd side of this:
pick client or server mode and a saved `--config` file (typically a
`generated-<mode>.yaml` from above), and it writes a ready-to-use
`config/systemd/revpn-<mode>.service` unit — pointing `ExecStart` at
`build/ReVPN-engine --mode <mode> --config <file>` — plus the exact
`sudo cp` / `systemctl daemon-reload` / `systemctl enable --now revpn-<mode>`
commands to install and start it on every boot. Nothing is installed
system-wide automatically; you run those commands yourself.

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
| `--decentralized`          | Supported via `ReVPN.bat --decentralized` (same Wintun dependency as `--client`) | Not exposed on the Windows build                    |
| Server-as-peer (`--vpn-ip`)| Supported — own Wintun adapter (`ReVPNS0`), same as `--client`  | Not exposed on the Windows build                    |
| Stress test                | Via the bash `ReVPN.sh --stress` wrapper, not the raw .exe       | Built in (`--stress`, or from the menu)             |

Build them with `./build_windows.sh` (needs
`g++-mingw-w64-x86-64-posix binutils-mingw-w64-x86-64`) and
`cd python && ./build_windows_exe.sh` (needs `wine`; downloads a
Windows Python + PyInstaller into a dedicated Wine prefix on first
run) respectively. After building (or grabbing a prebuilt
`ReVPN-engine.exe`), run `install.bat` once to fetch `wintun.dll` for
`--client`/`--decentralized`.

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
  `ReVPN.sh --help` / `--stress` ran correctly from an unrelated
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
- `ReVPN.sh` — the CLI + TUI wrapper end users run.
- `ReVPN.bat` / `install.bat` — the Windows CLI + menu wrapper and its
  dependency installer (fetches `wintun.dll`).
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
