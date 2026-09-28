"""
Full-screen nmtui/whiptail-style TUI, curses-based — the Python counterpart
to the bash ReVPN's whiptail menu. Same flow, same field order, same
Confirm-screen wording, just drawn with curses boxes instead of whiptail.

curses is stdlib on Linux/macOS. On Windows it needs the `windows-curses`
package (`pip install windows-curses`); ReVPN.py falls back to the plain
input()-based menu automatically if curses isn't importable or there's no
real terminal (e.g. piped input, some CI runners).
"""
from __future__ import annotations

import curses
import curses.textpad


class Cancelled(Exception):
    pass


def _center_win(stdscr, h, w):
    max_y, max_x = stdscr.getmaxyx()
    y = max(0, (max_y - h) // 2)
    x = max(0, (max_x - w) // 2)
    return curses.newwin(h, w, y, x)


def _backtitle(stdscr, version):
    max_y, max_x = stdscr.getmaxyx()
    title = f" ReVPN-py {version} — Mesh VPN — Creative By Rezier Labs "
    try:
        stdscr.addstr(0, max(0, (max_x - len(title)) // 2), title, curses.A_REVERSE)
    except curses.error:
        pass
    stdscr.refresh()


def menu(stdscr, version, title: str, subtitle: str, items: list[tuple[str, str]]) -> str:
    """items: list of (tag, description). Returns the chosen tag, or raises Cancelled (Esc/q)."""
    h = len(items) + 8
    w = max(len(title) + 4, max(len(f"{t}  {d}") for t, d in items) + 8, 50)
    idx = 0
    while True:
        stdscr.erase()
        _backtitle(stdscr, version)
        win = _center_win(stdscr, h, w)
        win.box()
        win.addstr(1, 2, title, curses.A_BOLD)
        win.addstr(2, 2, subtitle)
        for i, (tag, desc) in enumerate(items):
            attr = curses.A_REVERSE if i == idx else curses.A_NORMAL
            line = f"{tag:<8} {desc}"[: w - 4]
            win.addstr(4 + i, 2, line.ljust(w - 4), attr)
        win.addstr(h - 2, 2, "↑/↓ move   Enter select   Esc/q cancel", curses.A_DIM)
        win.refresh()

        key = stdscr.getch()
        if key in (curses.KEY_UP, ord("k")):
            idx = (idx - 1) % len(items)
        elif key in (curses.KEY_DOWN, ord("j")):
            idx = (idx + 1) % len(items)
        elif key in (curses.KEY_ENTER, 10, 13):
            return items[idx][0]
        elif key in (27, ord("q")):
            raise Cancelled()


def input_box(stdscr, version, title: str, prompt: str, default) -> str:
    h, w = 8, max(len(prompt) + 4, len(title) + 4, 50)
    stdscr.erase()
    _backtitle(stdscr, version)
    win = _center_win(stdscr, h, w)
    win.box()
    win.addstr(1, 2, title, curses.A_BOLD)
    win.addstr(2, 2, prompt[: w - 4])
    win.addstr(h - 2, 2, "Enter confirm   Esc cancel", curses.A_DIM)
    win.refresh()

    edit_win = curses.newwin(1, w - 4, win.getbegyx()[0] + 4, win.getbegyx()[1] + 2)
    curses.curs_set(1)
    curses.echo()
    box = curses.textpad.Textbox(edit_win)

    default_s = str(default)
    for ch in default_s:
        edit_win.addch(ch)
    edit_win.move(0, len(default_s))
    edit_win.refresh()

    cancelled = False

    def _validator(k):
        nonlocal cancelled
        if k == 27:  # Esc -> terminate edit (Ctrl-G) and flag cancellation
            cancelled = True
            return 7
        return k

    try:
        raw = box.edit(_validator).strip()
    finally:
        curses.noecho()
        curses.curs_set(0)

    if cancelled:
        raise Cancelled()
    return raw if raw else default_s


def yesno(stdscr, version, title: str, text: str, default: bool = True) -> bool:
    lines = text.splitlines()
    h = len(lines) + 6
    w = max(max((len(l) for l in lines), default=0) + 4, len(title) + 4, 50)
    choice = 0 if default else 1  # 0=Yes 1=No
    while True:
        stdscr.erase()
        _backtitle(stdscr, version)
        win = _center_win(stdscr, h, w)
        win.box()
        win.addstr(1, 2, title, curses.A_BOLD)
        for i, l in enumerate(lines):
            win.addstr(2 + i, 2, l[: w - 4])
        yn_row = h - 3
        yes_attr = curses.A_REVERSE if choice == 0 else curses.A_NORMAL
        no_attr = curses.A_REVERSE if choice == 1 else curses.A_NORMAL
        win.addstr(yn_row, w // 2 - 10, "[ Yes ]", yes_attr)
        win.addstr(yn_row, w // 2 + 3, "[ No ]", no_attr)
        win.refresh()

        key = stdscr.getch()
        if key in (curses.KEY_LEFT, curses.KEY_RIGHT, ord("\t")):
            choice = 1 - choice
        elif key in (curses.KEY_ENTER, 10, 13):
            return choice == 0
        elif key in (ord("y"), ord("Y")):
            return True
        elif key in (ord("n"), ord("N")):
            return False
        elif key in (27, ord("q")):
            raise Cancelled()


def msgbox(stdscr, version, title: str, text: str):
    lines = text.splitlines() or [""]
    h = min(len(lines) + 5, stdscr.getmaxyx()[0] - 2)
    w = min(max((len(l) for l in lines), default=0) + 4, stdscr.getmaxyx()[1] - 2)
    w = max(w, len(title) + 4, 40)
    stdscr.erase()
    _backtitle(stdscr, version)
    win = _center_win(stdscr, h, w)
    win.box()
    win.addstr(1, 2, title, curses.A_BOLD)
    for i, l in enumerate(lines[: h - 4]):
        try:
            win.addstr(2 + i, 2, l[: w - 4])
        except curses.error:
            pass
    win.addstr(h - 2, 2, "Press any key...", curses.A_DIM)
    win.refresh()
    stdscr.getch()
