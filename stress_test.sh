#!/usr/bin/env bash
# ============================================================================
# stress_test.sh — load-tests the ReVPN relay server on this one machine.
#
# Spins up a throwaway ReVPN-engine server on loopback, then spawns N
# synthetic clients (tools/stress_client.py) that REGISTER and flood
# MSG_DATA packets at a fixed rate/size for a fixed duration — no root,
# no TUN device. Reports the server's own observed throughput.
#
# Usually launched via `ReVPN --stress ...` or `ReVPN` (TUI), not directly.
# ============================================================================
set -euo pipefail

SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENGINE_BIN="$SELF_DIR/build/ReVPN-engine"
CLIENT_PY="$SELF_DIR/tools/stress_client.py"

PORT=19099
CLIENTS=20
DURATION=10
RATE=500
SIZE=512

while [[ $# -gt 0 ]]; do
    case "$1" in
        --port)     PORT="$2"; shift 2 ;;
        --clients)  CLIENTS="$2"; shift 2 ;;
        --duration) DURATION="$2"; shift 2 ;;
        --rate)     RATE="$2"; shift 2 ;;
        --size)     SIZE="$2"; shift 2 ;;
        *) echo "stress_test.sh: unknown option $1" >&2; exit 1 ;;
    esac
done

[[ -x "$ENGINE_BIN" ]] || { echo "ReVPN: engine not built — run ./build.sh first" >&2; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "ReVPN: python3 required for --stress" >&2; exit 1; }

WORKDIR="$(mktemp -d /tmp/ReVPN-stress.XXXXXX)"
SERVER_LOG="$WORKDIR/server.log"
CLIENT_PIDS=()
SERVER_PID=""

cleanup() {
    for pid in "${CLIENT_PIDS[@]:-}"; do
        kill "$pid" >/dev/null 2>&1 || true
    done
    if [[ -n "$SERVER_PID" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill -TERM "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

echo "=============================================================="
echo " ReVPN stress test"
echo "=============================================================="
echo "  clients   : $CLIENTS"
echo "  duration  : ${DURATION}s"
echo "  rate      : ${RATE} pps/client (target total: $((CLIENTS * RATE)) pps)"
echo "  pkt size  : ${SIZE} bytes payload"
echo "  server    : 127.0.0.1:${PORT}  (workers=4)"
echo "=============================================================="

# ── 1. throwaway server on loopback ────────────────────────────────────────
"$ENGINE_BIN" --mode server --bind "0.0.0.0:${PORT}" --workers 4 \
    > "$SERVER_LOG" 2>&1 &
SERVER_PID=$!
sleep 0.5
kill -0 "$SERVER_PID" 2>/dev/null || { echo "ReVPN: server failed to start, see $SERVER_LOG" >&2; exit 1; }
echo "server pid=$SERVER_PID  log=$SERVER_LOG"

# ── 2. N synthetic clients, arranged in a ring (i talks to i+1) ───────────
vpn_ip_for() { local i=$1; echo "10.250.$(( (i/250)%250 )).$(( (i%250)+2 ))"; }

echo "spawning $CLIENTS synthetic clients ..."
T_START=$(date +%s.%N)
for ((i = 0; i < CLIENTS; i++)); do
    dst=$(( (i + 1) % CLIENTS ))
    python3 "$CLIENT_PY" \
        --server "127.0.0.1:${PORT}" \
        --vpn-ip "$(vpn_ip_for "$i")" \
        --dst-vpn "$(vpn_ip_for "$dst")" \
        --node-id "$(printf '%08x' $((i + 1)))" \
        --duration "$DURATION" --rate "$RATE" --size "$SIZE" \
        > "$WORKDIR/client_$i.out" 2>"$WORKDIR/client_$i.err" &
    CLIENT_PIDS+=($!)
done

echo "running for ${DURATION}s ..."
for pid in "${CLIENT_PIDS[@]}"; do
    wait "$pid" 2>/dev/null || true
done
T_END=$(date +%s.%N)
ELAPSED=$(awk -v a="$T_START" -v b="$T_END" 'BEGIN{printf "%.2f", b-a}')

# ── 3. tally what clients THINK they sent ─────────────────────────────────
SENT_TOTAL=0
for ((i = 0; i < CLIENTS; i++)); do
    n=$(cat "$WORKDIR/client_$i.out" 2>/dev/null || echo 0)
    SENT_TOTAL=$(( SENT_TOTAL + ${n:-0} ))
done

# ── 4. stop server, capture its own final rx/fwd counters ─────────────────
kill -TERM "$SERVER_PID"
wait "$SERVER_PID" 2>/dev/null || true
SERVER_PID=""

RX_LINE="$(grep -a '\[Server\] clients=' "$SERVER_LOG" | tail -n1 || true)"
RX_BYTES="$(echo "$RX_LINE" | sed -n 's/.*rx=\([0-9]*\) B.*/\1/p')"
FWD_BYTES="$(echo "$RX_LINE" | sed -n 's/.*fwd=\([0-9]*\) B.*/\1/p')"
RX_BYTES="${RX_BYTES:-0}"
FWD_BYTES="${FWD_BYTES:-0}"

PKT_BYTES=$(( SIZE + 11 ))            # DataHdr is 11 bytes
EXPECTED_PKTS=$(( CLIENTS * RATE * DURATION ))
EXPECTED_BYTES=$(( EXPECTED_PKTS * PKT_BYTES ))
RX_PPS=$(awk -v b="$RX_BYTES" -v p="$PKT_BYTES" -v t="$ELAPSED" 'BEGIN{ if(t>0) printf "%.0f", (b/p)/t; else print 0 }')
RX_MBPS=$(awk -v b="$RX_BYTES" -v t="$ELAPSED" 'BEGIN{ if(t>0) printf "%.2f", (b*8)/1000000/t; else print 0 }')
LOSS_PCT=$(awk -v e="$EXPECTED_BYTES" -v r="$RX_BYTES" 'BEGIN{ if(e>0){l=(e-r)/e*100; if(l<0)l=0; printf "%.2f", l} else print 0 }')

echo "=============================================================="
echo " RESULTS"
echo "=============================================================="
echo "  wall time            : ${ELAPSED}s"
echo "  client-reported sent  : ${SENT_TOTAL} packets"
echo "  expected (target)     : ${EXPECTED_PKTS} packets / ${EXPECTED_BYTES} bytes"
echo "  server-observed rx    : ${RX_BYTES} bytes  (~${RX_PPS} pkt/s, ${RX_MBPS} Mbps)"
echo "  server-observed fwd   : ${FWD_BYTES} bytes  (routed to peers)"
echo "  estimated packet loss : ${LOSS_PCT}%  (client send drops + socket buffer drops)"
echo "=============================================================="
echo "  full server log: $SERVER_LOG  (kept at: $WORKDIR)"
echo "=============================================================="
