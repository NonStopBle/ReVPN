# Wintun — credit and license

ReVPN vendors the [Wintun](https://www.wintun.net/) driver (version 0.14.1)
in this folder to provide TUN device support on Windows (`--client`,
`--decentralized`, and server-as-peer modes via `ReVPN.bat`).

- **Project:** Wintun — Simple TUN Driver for Windows
- **Author/Copyright:** WireGuard LLC / Jason A. Donenfeld
- **Homepage:** https://www.wintun.net/
- **Version vendored:** 0.14.1
- **License:** see [`LICENSE.txt`](LICENSE.txt) in this folder (GPLv2,
  or the separate commercial license offered by WireGuard LLC)

Wintun is not written or maintained by the ReVPN project; it is bundled
here unmodified, strictly for convenience, so `install.bat` doesn't need
network access to fetch it at install time. All credit for the driver
itself belongs to its upstream author. If you redistribute ReVPN, keep
this folder (including `LICENSE.txt`) alongside it.

Layout:

```
vendor/wintun/
├── LICENSE.txt       # upstream Wintun license
├── CREDIT.md         # this file
└── bin/
    ├── amd64/wintun.dll
    ├── arm64/wintun.dll
    ├── arm/wintun.dll
    └── x86/wintun.dll
```
