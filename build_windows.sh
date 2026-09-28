#!/usr/bin/env bash
# Cross-compiles engine/meshvpn.cpp to a native Windows .exe using MinGW-w64,
# and (if `wine` is installed) smoke-tests it by actually running
# --mode server under Wine.
#
# Install the toolchain (Debian/Ubuntu):
#   sudo apt install g++-mingw-w64-x86-64-posix binutils-mingw-w64-x86-64
#   sudo apt install wine wine64        # optional, only for the smoke test
#
# Output: build-windows/ReVPN-engine.exe
#
# Scope: --mode server is fully supported (pure UDP relay/rendezvous, no
# TUN needed) and interoperates with the Linux engine and ReVPN-py — same
# wire protocol. --mode client loads Wintun (wintun.dll, from
# https://www.wintun.net/, must sit next to the .exe) dynamically at
# startup and needs Administrator; untested on real Windows hardware so
# far (built/run only under Wine — no Wintun driver available there to
# actually exercise it).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

CXX=x86_64-w64-mingw32-g++
command -v "$CXX" >/dev/null 2>&1 || {
    echo "build_windows.sh: $CXX not found." >&2
    echo "  sudo apt install g++-mingw-w64-x86-64-posix binutils-mingw-w64-x86-64" >&2
    exit 1
}

mkdir -p build-windows
"$CXX" -std=c++17 -O2 -static -static-libgcc -static-libstdc++ \
    engine/meshvpn.cpp -o build-windows/ReVPN-engine.exe -lws2_32 -lole32

echo "Built: build-windows/ReVPN-engine.exe"
file build-windows/ReVPN-engine.exe 2>/dev/null || true

if command -v wine >/dev/null 2>&1; then
    echo
    echo "wine found — smoke-testing --mode server under Wine ..."
    export WINEDEBUG=-all
    export WINEARCH=win64
    export WINEPREFIX="${WINEPREFIX:-$HOME/.wine-ReVPN-test}"
    wineboot --init >/dev/null 2>&1 || true
    timeout 3 wine build-windows/ReVPN-engine.exe --mode server --bind 0.0.0.0:0 --workers 1 \
        > /tmp/ReVPN-wine-smoketest.log 2>&1 || true
    if grep -q "\[Server\] Starting" /tmp/ReVPN-wine-smoketest.log; then
        echo "OK — server started under Wine. Log: /tmp/ReVPN-wine-smoketest.log"
    else
        echo "Smoke test did not see the expected startup line — check /tmp/ReVPN-wine-smoketest.log"
    fi
else
    echo "(wine not installed — skipping the run-under-Wine smoke test)"
fi
