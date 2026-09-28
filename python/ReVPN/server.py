"""
ReVPN-py server — a pure UDP rendezvous/relay, no TUN, no admin/root
required. Speaks the exact same wire protocol as ReVPN/engine/meshvpn.cpp's
--mode server, so ReVPN-cpp and ReVPN-py clients can mix freely against
either a C++ or a Python server.

Pure Python stdlib (socket + threading) — runs on Linux, macOS and Windows.
"""
from __future__ import annotations

import socket
import threading
import time

from . import protocol as p

CLIENT_TTL = 30.0  # seconds


class ClientEntry:
    __slots__ = ("addr", "vpn_ip", "node_id", "last_seen")

    def __init__(self, addr, vpn_ip: bytes, node_id: int):
        self.addr = addr
        self.vpn_ip = vpn_ip
        self.node_id = node_id
        self.last_seen = time.monotonic()


class Server:
    def __init__(self, bind_host: str = "0.0.0.0", bind_port: int = 9000, log=print):
        self.bind_host = bind_host
        self.bind_port = bind_port
        self.log = log

        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind((bind_host, bind_port))

        self.lock = threading.RLock()
        self.by_node: dict[int, ClientEntry] = {}
        self.by_vpn: dict[bytes, int] = {}

        self.rx_bytes = 0
        self.fwd_bytes = 0
        self.running = True

    # ── handlers ─────────────────────────────────────────────────────────
    def on_register(self, buf: bytes, from_addr):
        r = p.unpack_reg(buf)
        with self.lock:
            is_new = r["node_id"] not in self.by_node
            c = self.by_node.get(r["node_id"])
            if c is None:
                c = ClientEntry(from_addr, r["vpn_ip"], r["node_id"])
                self.by_node[r["node_id"]] = c
            c.addr = from_addr
            c.vpn_ip = r["vpn_ip"]
            c.last_seen = time.monotonic()
            self.by_vpn[r["vpn_ip"]] = r["node_id"]

            if is_new:
                self.log(f"[Server] + Client vpn={p.bytes_to_ip(c.vpn_ip):<15} "
                          f"pub={from_addr[0]}:{from_addr[1]:<5} node=0x{c.node_id:08X}")

            to_send = []
            for nid, ce in self.by_node.items():
                if nid == r["node_id"]:
                    continue
                if is_new:
                    pkt = p.pack_reg(p.MSG_PEER_INFO, c.node_id, c.vpn_ip,
                                      c.addr[1], socket.inet_aton(c.addr[0]))
                    to_send.append((pkt, ce.addr))
                pkt2 = p.pack_reg(p.MSG_PEER_INFO, ce.node_id, ce.vpn_ip,
                                    ce.addr[1], socket.inet_aton(ce.addr[0]))
                to_send.append((pkt2, from_addr))

        ack = p.pack_small(p.MSG_KEEPALIVE, 0)
        self.sock.sendto(ack, from_addr)
        for _ in range(3):
            for pkt, dst in to_send:
                self.sock.sendto(pkt, dst)

    def on_data(self, buf: bytes, from_addr):
        if len(buf) < p.DATAHDR_SIZE:
            return
        h = p.unpack_data_header(buf)
        self.rx_bytes += len(buf)

        with self.lock:
            src_nid = self.by_vpn.get(h["src_vpn"])
            if src_nid is not None and src_nid in self.by_node:
                self.by_node[src_nid].last_seen = time.monotonic()

            if h["dst_vpn"] == b"\x00\x00\x00\x00":
                dsts = [ce.addr for nid, ce in self.by_node.items() if nid != src_nid]
            else:
                dnid = self.by_vpn.get(h["dst_vpn"])
                dsts = [self.by_node[dnid].addr] if dnid in self.by_node else []

        for dst in dsts:
            self.sock.sendto(buf, dst)
            self.fwd_bytes += len(buf)

    def on_punch(self, buf: bytes, from_addr):
        with self.lock:
            skip = None
            for nid, ce in self.by_node.items():
                if ce.addr == from_addr:
                    skip = nid
                    break
            dsts = [ce.addr for nid, ce in self.by_node.items() if nid != skip]
        for dst in dsts:
            self.sock.sendto(buf, dst)

    def on_keepalive(self, buf: bytes):
        k = p.unpack_small(buf)
        with self.lock:
            c = self.by_node.get(k["node_id"])
            if c:
                c.last_seen = time.monotonic()

    def dispatch(self, buf: bytes, from_addr):
        if not buf:
            return
        t = buf[0]
        if t == p.MSG_REGISTER:
            self.on_register(buf, from_addr)
        elif t in (p.MSG_DATA, p.MSG_DATA_ENC):
            self.on_data(buf, from_addr)
        elif t in (p.MSG_PUNCH, p.MSG_PUNCH_ACK):
            self.on_punch(buf, from_addr)
        elif t == p.MSG_KEEPALIVE:
            self.on_keepalive(buf)

    def cleanup_loop(self):
        while self.running:
            time.sleep(5)
            now = time.monotonic()
            with self.lock:
                expired = [nid for nid, c in self.by_node.items()
                           if now - c.last_seen > CLIENT_TTL]
                for nid in expired:
                    c = self.by_node.pop(nid, None)
                    if c:
                        self.by_vpn.pop(c.vpn_ip, None)
                        self.log(f"[Server] - Expired vpn={p.bytes_to_ip(c.vpn_ip)}")

    def print_status(self):
        with self.lock:
            n = len(self.by_node)
        self.log(f"[Server] clients={n}  rx={self.rx_bytes} B  fwd={self.fwd_bytes} B")

    def run(self):
        self.log(f"[Server] ReVPN-py listening on {self.bind_host}:{self.bind_port}")
        threading.Thread(target=self.cleanup_loop, daemon=True).start()

        last_status = time.monotonic()
        self.sock.settimeout(1.0)
        try:
            while self.running:
                try:
                    buf, addr = self.sock.recvfrom(65536)
                except socket.timeout:
                    pass
                else:
                    self.dispatch(buf, addr)
                if time.monotonic() - last_status >= 10:
                    self.print_status()
                    last_status = time.monotonic()
        except KeyboardInterrupt:
            pass
        finally:
            self.print_status()
            self.sock.close()
