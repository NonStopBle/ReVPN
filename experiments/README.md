# experiments/

Standalone concept tests — not wired into the `ReVPN` product, not built by
`build.sh`. Each file here is a throwaway proof of concept for an idea
before (or instead of) it becomes a real feature.

## stun_punch_test.cpp

Tests the idea in the root [README's Abstract](../README.md#abstract):
can two peers connect **without any rendezvous/relay server**, using a
public STUN server (Google's) purely to discover each side's own
`ip:port` as seen from the internet, then exchanging that as a short
copy-pasteable **token** (no shared file, no server) and hole-punching
straight to each other?

```sh
g++ -std=c++17 -O2 -pthread stun_punch_test.cpp -o stun_punch_test

# Terminal A:
./stun_punch_test 51001 alice 6001
#   -> prints "Your token: <...>", then waits for you to paste Bob's.
# Terminal B:
./stun_punch_test 51002 bob 6002
#   -> prints its own token; paste it into Terminal A's prompt, and
#      paste Terminal A's token into Terminal B's prompt.

# Both sides then hole-punch to each other. `nc 127.0.0.1 6001` on
# Alice's side tunnels through the punched UDP path to Bob's local TCP
# port (6002) — no server anywhere in that path.
```

For scripted runs, pass the peer's token as a 4th argument instead of
pasting it interactively.

No JSON library — the token payload is one comma-separated
`id,ip,main_port,redundant_port` line, base64-encoded.

### Redundant path + automatic failover

Each side actually opens and punches **two** independent UDP sockets —
a main one and a redundant one, both STUN-mapped, both included in the
token. A background watchdog keeps both alive with periodic keepalive
punches (a NAT's UDP mapping isn't permanent — routers expire it after a
period of silence) and, if the main path goes quiet for too long,
automatically fails the live tunnel over to the redundant path and
re-punches it — no user action needed, and any in-progress local TCP
connection through the tunnel keeps working through the switch.

If the redundant path *also* goes quiet, there's nothing left to fail
over to — at that point either side's actual reachable address may have
genuinely changed (NAT remapped, network switched) — so it reports the
link as lost and says what to do: get a fresh token from your peer and
restart to resync. If a packet from the peer shows up again later on
either path, it's treated as recovered automatically.

### A note on testing this locally

Hole punching fundamentally needs two *different* NATs to prove the
"no port forwarding" case — that's the whole point of it. Running both
sides on one machine behind one NAT/router relies on that router
supporting **NAT hairpinning** (routing your own public IP back to a
LAN device), which plenty of routers, and many sandboxed/virtualized
dev environments in particular, don't support in both directions or at
all. If a local two-terminal test shows one-way or zero punch traffic,
that's very likely this hairpin limitation, not a bug in the code —
the real test is two machines on two different networks.
