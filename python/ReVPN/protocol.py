"""
Wire protocol — byte-for-byte compatible with vpn/meshvpn.cpp and
ReVPN/engine/meshvpn.cpp. A ReVPN-py server/client can talk to a ReVPN-cpp
server/client interchangeably because every struct here packs to the exact
same bytes the C++ side reads with `(const RegPkt*)buf` etc.

Struct field byte orders (must match meshvpn.cpp exactly):
  - node_id, payload_len, nonce : native little-endian (x86 raw memory, no
    htons/htonl applied in the C++ source)
  - vpn_ip, dst_vpn, src_vpn, public_ip : already-network-order 4 bytes
    (produced by inet_addr()/inet_aton() on both sides)
  - udp_port : big-endian (explicit htons() in the C++ source)
"""
from __future__ import annotations

import struct
import socket

MSG_REGISTER = 0x01
MSG_PEER_INFO = 0x02
MSG_PUNCH = 0x03
MSG_PUNCH_ACK = 0x04
MSG_KEEPALIVE = 0x05
MSG_DATA = 0x20
MSG_DATA_ENC = 0x21

REGPKT_SIZE = 50   # 1 + 4 + 4 + 2 + 4 + 35
PUNCHPKT_SIZE = 5  # 1 + 4
KAPKT_SIZE = 5     # 1 + 4
DATAHDR_SIZE = 11  # 1 + 4 + 4 + 2

# ── XOR placeholder cipher — identical to meshvpn.cpp's xcrypt() ───────────
# Replace with real crypto (matching -DMESHVPN_SODIUM) if you turn that on
# on the C++ side; interop then requires the same change on both ends.
XOR_KEY = bytes([
    0x4d, 0x65, 0x73, 0x68, 0x56, 0x50, 0x4e, 0x4b,
    0x65, 0x79, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35,
    0x36, 0x37, 0x38, 0x39, 0x61, 0x62, 0x63, 0x64,
    0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b, 0x6c,
])


def xcrypt(data: bytes, nonce: int) -> bytes:
    nb = nonce.to_bytes(8, "little")
    out = bytearray(len(data))
    for i, b in enumerate(data):
        out[i] = b ^ XOR_KEY[i % 32] ^ nb[i % 8]
    return bytes(out)


def ip_to_bytes(ip: str) -> bytes:
    return socket.inet_aton(ip)


def bytes_to_ip(b: bytes) -> str:
    return socket.inet_ntoa(b)


# ── RegPkt (REGISTER / PEER_INFO) ───────────────────────────────────────────
def pack_reg(msg_type: int, node_id: int, vpn_ip: bytes, udp_port: int,
             public_ip: bytes = b"\x00\x00\x00\x00") -> bytes:
    return (
        struct.pack("<B", msg_type)
        + struct.pack("<I", node_id & 0xFFFFFFFF)
        + vpn_ip
        + struct.pack(">H", udp_port & 0xFFFF)
        + public_ip
        + b"\x00" * 35
    )


def unpack_reg(buf: bytes) -> dict:
    if len(buf) < REGPKT_SIZE:
        raise ValueError("short RegPkt")
    msg_type = buf[0]
    node_id = struct.unpack("<I", buf[1:5])[0]
    vpn_ip = buf[5:9]
    udp_port = struct.unpack(">H", buf[9:11])[0]
    public_ip = buf[11:15]
    return {
        "type": msg_type,
        "node_id": node_id,
        "vpn_ip": vpn_ip,
        "udp_port": udp_port,
        "public_ip": public_ip,
    }


# ── PunchPkt / KaPkt (identical layout: type + node_id) ────────────────────
def pack_small(msg_type: int, node_id: int) -> bytes:
    return struct.pack("<B", msg_type) + struct.pack("<I", node_id & 0xFFFFFFFF)


def unpack_small(buf: bytes) -> dict:
    if len(buf) < 5:
        raise ValueError("short packet")
    return {"type": buf[0], "node_id": struct.unpack("<I", buf[1:5])[0]}


# ── DataHdr ──────────────────────────────────────────────────────────────
def pack_data_header(msg_type: int, src_vpn: bytes, dst_vpn: bytes, payload_len: int) -> bytes:
    return (
        struct.pack("<B", msg_type)
        + src_vpn
        + dst_vpn
        + struct.pack("<H", payload_len & 0xFFFF)
    )


def unpack_data_header(buf: bytes) -> dict:
    if len(buf) < DATAHDR_SIZE:
        raise ValueError("short DataHdr")
    return {
        "type": buf[0],
        "src_vpn": buf[1:5],
        "dst_vpn": buf[5:9],
        "payload_len": struct.unpack("<H", buf[9:11])[0],
    }
