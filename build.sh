#!/usr/bin/env bash
# Builds the ReVPN engine binary into build/ReVPN-engine (server, client,
# and decentralized/token-based modes are all one binary — see --mode).
#
#   ./build.sh              standard build (recvmmsg, XOR placeholder crypto)
#   ./build.sh --xdp        + AF_XDP zero-copy (needs libbpf-dev, clang)
#   ./build.sh --sodium     + real libsodium crypto (needs libsodium-dev)
#   ./build.sh --xdp --sodium --native   all of the above + -march=native
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

CMAKE_FLAGS=()
XDP_BUILD=false
for a in "$@"; do
    case "$a" in
        --xdp)    CMAKE_FLAGS+=(-DMESHVPN_XDP=ON); XDP_BUILD=true ;;
        --sodium) CMAKE_FLAGS+=(-DMESHVPN_SODIUM=ON) ;;
        --native) CMAKE_FLAGS+=(-DMESHVPN_NATIVE=ON) ;;
        *) echo "build.sh: unknown flag $a" >&2; exit 1 ;;
    esac
done

mkdir -p build
cd build
cmake ../engine "${CMAKE_FLAGS[@]}"
make -j"$(nproc)"

# Marker consulted by ./ReVPN.sh (CLI + TUI) to know whether this build actually
# supports --xdp-iface — the engine's --help text mentions the flag either
# way, so the marker (not the binary) is the source of truth.
if [[ "$XDP_BUILD" == true ]]; then
    touch .xdp_enabled
else
    rm -f .xdp_enabled
fi

echo
echo "Built: $(pwd)/ReVPN-engine"
echo "AF_XDP: $([[ "$XDP_BUILD" == true ]] && echo "enabled" || echo "disabled (build with --xdp to enable)")"
echo "Run:   ../ReVPN.sh --help"
