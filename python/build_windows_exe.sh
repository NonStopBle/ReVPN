#!/usr/bin/env bash
# Builds a standalone Windows ReVPN.exe from ReVPN.py using PyInstaller —
# with the full TUI baked in (windows-curses, so the same full-screen
# arrow-key menu as the Linux curses TUI, not just the plain-input fallback).
#
# PyInstaller doesn't support cross-compiling from Linux directly, so this
# hosts a real Windows Python + PyInstaller *inside Wine* and runs the
# build there — the output is a genuine PE32+ Windows .exe, not a Linux
# binary. Verified end-to-end under Wine: --help, --stress (full run), and
# the TUI (menu -> stress form -> confirm -> real run -> back to menu) all
# work. See ../README.md for what is/isn't verified.
#
# Usage:
#   ./build_windows_exe.sh
# Output:
#   dist/ReVPN.exe
#
# Needs: wine, internet access (first run only, to install Python +
# PyInstaller + windows-curses into a dedicated Wine prefix).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

command -v wine >/dev/null 2>&1 || { echo "build_windows_exe.sh: wine not found (sudo apt install wine wine64)" >&2; exit 1; }

export WINEDEBUG=-all
export WINEARCH=win64
export WINEPREFIX="${WINEPREFIX:-$HOME/.wine-ReVPN-build}"
PYVER=3.11.9
PYDIR="$WINEPREFIX/drive_c/users/$USER/AppData/Local/Programs/Python/Python311"
PY="$PYDIR/python.exe"

if [[ ! -f "$PY" ]]; then
    echo "Setting up a Wine-hosted Windows Python $PYVER (one-time) ..."
    mkdir -p "$WINEPREFIX"
    wineboot --init >/dev/null 2>&1 || true
    curl -sL -o /tmp/python-installer.exe \
        "https://www.python.org/ftp/python/${PYVER}/python-${PYVER}-amd64.exe"
    wine /tmp/python-installer.exe /quiet InstallAllUsers=0 PrependPath=1 \
        Include_test=0 SimpleInstall=1
    [[ -f "$PY" ]] || { echo "Python install into Wine prefix failed." >&2; exit 1; }
fi

echo "Installing/upgrading PyInstaller + windows-curses ..."
wine "$PY" -m pip install --quiet --upgrade pip
wine "$PY" -m pip install --quiet pyinstaller windows-curses

echo "Building dist/ReVPN.exe ..."
wine "$PY" -m PyInstaller --onefile --console --name ReVPN --clean ReVPN.py

echo
echo "Built: dist/ReVPN.exe"
echo "Test (through a pipe — Wine's console has a quirk where redirecting"
echo "straight to a file can fail; piping, as below, is reliable):"
echo "  wine dist/ReVPN.exe --help | cat"
