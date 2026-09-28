"""
Cross-platform TUN device.

- Linux:   /dev/net/tun via fcntl/ioctl, same TUNSETIFF + `ip addr`/`ip link`
           dance as vpn/meshvpn.cpp's setup_tun(). Needs root.
- Windows: Wintun (https://www.wintun.net/) via ctypes bindings to
           wintun.dll. Needs Administrator + wintun.dll next to this file
           (or on PATH) + the Wintun driver, which the DLL installs on
           first use. This is the same adapter tech OpenVPN2/WireGuard use
           on Windows, so it's a normal, supported way to get a TUN-like
           device there — Windows has no native /dev/net/tun equivalent.

Both expose the same tiny interface: open(), read(), write(), close().
"""
from __future__ import annotations

import ctypes
import os
import platform
import struct
import subprocess


class TunError(RuntimeError):
    pass


def create_tun(vpn_ip: str, prefix: int, mtu: int, name: str = "ReVPN0"):
    system = platform.system()
    if system == "Linux":
        return LinuxTun(vpn_ip, prefix, mtu, name)
    if system == "Windows":
        return WindowsTun(vpn_ip, prefix, mtu, name)
    raise TunError(
        f"No TUN backend for platform '{system}'. "
        "ReVPN-py client currently supports Linux (/dev/net/tun) and "
        "Windows (Wintun). Use --relay-only stress/testing modes elsewhere."
    )


# ============================================================================
# Linux — mirrors meshvpn.cpp's setup_tun() exactly: same ioctls, same
# `ip route add` / TCPMSS clamp commands, so behavior matches the C++ client.
# ============================================================================
class LinuxTun:
    IFF_TUN = 0x0001
    IFF_NO_PI = 0x1000
    TUNSETIFF = 0x400454CA
    SIOCSIFADDR = 0x8916
    SIOCSIFNETMASK = 0x891C
    SIOCSIFMTU = 0x8922
    SIOCGIFFLAGS = 0x8913
    SIOCSIFFLAGS = 0x8914
    IFF_UP = 0x1
    IFF_RUNNING = 0x40

    def __init__(self, vpn_ip: str, prefix: int, mtu: int, name: str):
        import fcntl
        import socket as _socket

        try:
            self.fd = os.open("/dev/net/tun", os.O_RDWR)
        except PermissionError as e:
            raise TunError("open /dev/net/tun failed — need root") from e

        ifr = struct.pack("16sH", name.encode(), self.IFF_TUN | self.IFF_NO_PI)
        try:
            res = fcntl.ioctl(self.fd, self.TUNSETIFF, ifr)
        except OSError as e:
            raise TunError(f"TUNSETIFF failed: {e}") from e
        self.name = res[:16].split(b"\x00", 1)[0].decode()

        s = _socket.socket(_socket.AF_INET, _socket.SOCK_DGRAM)

        def ioctl_addr(cmd, ip):
            packed = struct.pack("16sH2s4s8s", self.name.encode(), _socket.AF_INET,
                                  b"\x00\x00", _socket.inet_aton(ip), b"\x00" * 8)
            fcntl.ioctl(s.fileno(), cmd, packed)

        ioctl_addr(self.SIOCSIFADDR, vpn_ip)
        mask_int = (0xFFFFFFFF << (32 - prefix)) & 0xFFFFFFFF if prefix else 0
        mask_ip = _socket.inet_ntoa(struct.pack("!I", mask_int))
        ioctl_addr(self.SIOCSIFNETMASK, mask_ip)

        ifr_mtu = struct.pack("16sI", self.name.encode(), mtu)
        fcntl.ioctl(s.fileno(), self.SIOCSIFMTU, ifr_mtu)

        ifr_flags = struct.pack("16sh", self.name.encode(), 0)
        flags = struct.unpack("16sh", fcntl.ioctl(s.fileno(), self.SIOCGIFFLAGS, ifr_flags))[1]
        flags |= self.IFF_UP | self.IFF_RUNNING
        fcntl.ioctl(s.fileno(), self.SIOCSIFFLAGS, struct.pack("16sh", self.name.encode(), flags))
        s.close()

        import ipaddress
        net = ipaddress.IPv4Network(f"{vpn_ip}/{prefix}", strict=False)
        subprocess.run(["ip", "route", "add", str(net), "dev", self.name],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.run(
            ["iptables", "-t", "mangle", "-A", "FORWARD", "-p", "tcp",
             "--tcp-flags", "SYN,RST", "SYN", "-j", "TCPMSS", "--set-mss", "1200"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        os.set_blocking(self.fd, False)
        print(f"[TUN] {self.name}  ip={vpn_ip}/{prefix}  mtu={mtu}")

    def read(self, n=65536):
        try:
            return os.read(self.fd, n)
        except BlockingIOError:
            return b""

    def write(self, data: bytes):
        try:
            os.write(self.fd, data)
        except BlockingIOError:
            pass

    def fileno(self):
        return self.fd

    def close(self):
        os.close(self.fd)


# ============================================================================
# Windows — Wintun adapter via ctypes. Requires wintun.dll (download from
# wintun.net, put next to ReVPN.py or on PATH) and Administrator privileges.
# ============================================================================
class _GUID(ctypes.Structure):
    _fields_ = [
        ("Data1", ctypes.c_uint32),
        ("Data2", ctypes.c_uint16),
        ("Data3", ctypes.c_uint16),
        ("Data4", ctypes.c_ubyte * 8),
    ]


class WindowsTun:
    def __init__(self, vpn_ip: str, prefix: int, mtu: int, name: str):
        try:
            self._dll = ctypes.WinDLL("wintun.dll", use_last_error=True)  # type: ignore[attr-defined]
        except OSError as e:
            raise TunError(
                "wintun.dll not found. Download it from https://www.wintun.net/ "
                "and place it next to ReVPN.py (run as Administrator)."
            ) from e

        self._setup_prototypes()

        guid_bytes = os.urandom(16)
        guid = _GUID.from_buffer_copy(guid_bytes)
        self._adapter = self._dll.WintunCreateAdapter(name, "ReVPN", ctypes.byref(guid))
        if not self._adapter:
            raise TunError(
                f"WintunCreateAdapter failed (GetLastError={ctypes.get_last_error()}) "
                "— need Administrator?"
            )

        # 4 MiB ring — must be a power of two between 128 KiB and 64 MiB per
        # the Wintun API reference (git.zx2c4.com/wintun/about/#reference).
        self._session = self._dll.WintunStartSession(self._adapter, 0x400000)
        if not self._session:
            self._dll.WintunCloseAdapter(self._adapter)
            raise TunError(f"WintunStartSession failed (GetLastError={ctypes.get_last_error()})")

        self.name = name
        self._configure_ip(name, vpn_ip, prefix, mtu)
        print(f"[TUN] {name} (Wintun)  ip={vpn_ip}/{prefix}  mtu={mtu}")

    def _setup_prototypes(self):
        # Signatures per git.zx2c4.com/wintun/about/#reference — all string
        # params are WCHAR* (wide), sizes are DWORD (c_uint32), and every
        # entry point is declared explicitly rather than relying on ctypes'
        # implicit str/int argument guessing, which is easy to get subtly
        # wrong (e.g. size args silently truncating on a 64-bit DWORD*).
        d = self._dll
        d.WintunCreateAdapter.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.POINTER(_GUID)]
        d.WintunCreateAdapter.restype = ctypes.c_void_p
        d.WintunCloseAdapter.argtypes = [ctypes.c_void_p]
        d.WintunCloseAdapter.restype = None
        d.WintunStartSession.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
        d.WintunStartSession.restype = ctypes.c_void_p
        d.WintunEndSession.argtypes = [ctypes.c_void_p]
        d.WintunEndSession.restype = None
        d.WintunReceivePacket.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32)]
        d.WintunReceivePacket.restype = ctypes.POINTER(ctypes.c_ubyte)
        d.WintunReleaseReceivePacket.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ubyte)]
        d.WintunReleaseReceivePacket.restype = None
        d.WintunAllocateSendPacket.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
        d.WintunAllocateSendPacket.restype = ctypes.POINTER(ctypes.c_ubyte)
        d.WintunSendPacket.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ubyte)]
        d.WintunSendPacket.restype = None

    def _configure_ip(self, name, vpn_ip, prefix, mtu):
        # Wintun creates the adapter; standard `netsh` assigns the IP/MTU,
        # same as how WireGuard-for-Windows does it under the hood.
        subprocess.run(
            ["netsh", "interface", "ip", "set", "address", f"name={name}",
             "static", vpn_ip, _prefix_to_mask(prefix)],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.run(
            ["netsh", "interface", "ipv4", "set", "subinterface", name,
             f"mtu={mtu}", "store=persistent"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def read(self, n=65536):
        size = ctypes.c_uint32(0)
        ptr = self._dll.WintunReceivePacket(self._session, ctypes.byref(size))
        if not ptr:
            # NULL is the normal "no packet ready" case (GetLastError ==
            # ERROR_NO_MORE_ITEMS / 259) as well as real errors; either way
            # there's nothing to hand back right now.
            return b""
        try:
            return bytes(ctypes.cast(ptr, ctypes.POINTER(ctypes.c_ubyte * size.value)).contents)
        finally:
            self._dll.WintunReleaseReceivePacket(self._session, ptr)

    def write(self, data: bytes):
        ptr = self._dll.WintunAllocateSendPacket(self._session, len(data))
        if not ptr:
            return
        ctypes.memmove(ptr, data, len(data))
        self._dll.WintunSendPacket(self._session, ptr)

    def fileno(self):
        return None  # Wintun is event/poll-driven, not a plain fd; see client.py

    def close(self):
        self._dll.WintunEndSession(self._session)
        self._dll.WintunCloseAdapter(self._adapter)


def _prefix_to_mask(prefix: int) -> str:
    import socket as _socket
    import struct as _struct
    mask_int = (0xFFFFFFFF << (32 - prefix)) & 0xFFFFFFFF if prefix else 0
    return _socket.inet_ntoa(_struct.pack("!I", mask_int))
