#!/usr/bin/env python3
"""
Thin launcher for direct `python3 ReVPN.py` use without installing anything.

If you `pip install .` (or `pipx install .`) from this directory instead,
you get a `ReVPN` command on PATH — see pyproject.toml / README.md — and
don't need this file at all; it just re-exports the same entry point for
people running the script in place.
"""
from ReVPN.cli import main

if __name__ == "__main__":
    main()
