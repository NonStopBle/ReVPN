"""
ReVPN-py client — same REGISTER / UDP hole-punch / auto relay-fallback
state machine as vpn/meshvpn.cpp's --mode client, so it interoperates with
a ReVPN-cpp server (or a ReVPN-py one) and with ReVPN-cpp peers.

Uses plain threads + blocking sockets with short timeouts instead of
epoll/io_uring — this is the portable "everything the same, but simple"
counterpart to the C++ engine, not a low-latency rewrite of it.
"""
from __future__ import annotations

import socket
import threading
import time

from . import protocol as p
from .tun import create_tun, TunError

T_REG = 0.5
T_REG_KA = 5.0
T_PUNCH = 1.0
T_PMAX = 10.0
T_KA = 0.5
T_DROP = 3.0
T_BGRETRY = 30.0
T_SRVDEAD = 15.0
T_STATUS = 2.0

PUNCH_PORT_PROBES = [1, -1, 2, -2, 3, -3, 4, -4, 5, -5, 6, -6, 7, -7, 8, -8]


def derive_node_id(vpn_ip: str) -> int:
    ip_int = int.from_bytes(socket.inet_aton(vpn_ip), "big")
    n = ip_int & 0xFFFFFFFF
    n ^= n >> 16
    n = (n * 0x45D9F3B) & 0xFFFFFFFF
    n ^= n >> 16
    return n


class Peer:
    def __init__(self, vpn_ip: bytes, node_id: int, addr):
        self.vpn_ip = vpn_ip
        self.node_id = node_id
        self.addr = addr
        self.state = "NONE"   # NONE | PUNCHING | DIRECT | FALLBACK
        self.t_pstart = 0.0
        self.t_punch = 0.0
        self.t_p2prx = 0.0
        self.t_ka = 0.0
        self.punch_n = 0
        self.gave_up = False

    def direct(self):
        return self.state == "DIRECT"


class Client:
    def __init__(self, vpn_ip: str, server_addr, subnet=16, mtu=1380,
                 local_port=51820, node_id=None, encrypt=True,
                 p2p=True, log=print):
        self.vpn_ip_str = vpn_ip
        self.vpn_ip = socket.inet_aton(vpn_ip)
        self.subnet = subnet
        self.mtu = mtu
        self.server_addr = server_addr
        self.node_id = node_id if node_id is not None else derive_node_id(vpn_ip)
        self.encrypt = encrypt
        self.p2p = p2p
        self.log = log

        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("0.0.0.0", local_port))
        self.sock.settimeout(0.05)
        self.local_port = self.sock.getsockname()[1]

        self.peers: dict[int, Peer] = {}
        self.vpn_to_node: dict[bytes, int] = {}

        self.nonce = 0
        self.tx = 0
        self.rx = 0

        self.t_reg = 0.0
        self.t_srv_rx = 0.0
        self.running = True

        self.tun = create_tun(vpn_ip, subnet, mtu)

    # ── outgoing ────────────────────────────────────────────────────────
    def do_register(self):
        pkt = p.pack_reg(p.MSG_REGISTER, self.node_id, self.vpn_ip, self.local_port)
        self.sock.sendto(pkt, self.server_addr)
        self.t_reg = time.monotonic()

    def send_vpn(self, payload: bytes, dst_vpn: bytes):
        if not payload or len(payload) > 1400:
            return
        dst_addr = self.server_addr
        if self.p2p and dst_vpn in self.vpn_to_node:
            peer = self.peers.get(self.vpn_to_node[dst_vpn])
            if peer and peer.direct():
                dst_addr = peer.addr

        if self.encrypt:
            nonce = self.nonce
            self.nonce += 1
            enc = p.xcrypt(payload, nonce)
            hdr = p.pack_data_header(p.MSG_DATA_ENC, self.vpn_ip, dst_vpn, len(payload))
            pkt = hdr + enc + nonce.to_bytes(8, "little")
        else:
            hdr = p.pack_data_header(p.MSG_DATA, self.vpn_ip, dst_vpn, len(payload))
            pkt = hdr + payload

        self.sock.sendto(pkt, dst_addr)
        self.tx += len(payload)

    # ── incoming ────────────────────────────────────────────────────────
    def on_peer_info(self, r):
        if r["node_id"] == self.node_id:
            return
        is_new = r["node_id"] not in self.peers
        peer = self.peers.get(r["node_id"])
        addr = (p.bytes_to_ip(r["public_ip"]), r["udp_port"])
        if peer is None:
            peer = Peer(r["vpn_ip"], r["node_id"], addr)
            self.peers[r["node_id"]] = peer
        else:
            peer.addr = addr
        self.vpn_to_node[r["vpn_ip"]] = r["node_id"]

        self.log(f"[Peer] vpn={p.bytes_to_ip(r['vpn_ip']):<15} pub={addr[0]}:{addr[1]} "
                  f"node=0x{r['node_id']:08X}{'' if is_new else ' (updated)'}")

        if not self.p2p:
            return
        if is_new:
            peer.state = "PUNCHING"
            peer.t_pstart = time.monotonic()
            peer.punch_n = 0
            peer.t_punch = 0
            peer.gave_up = False
            self.log(f"[P2P] Punching -> vpn={p.bytes_to_ip(peer.vpn_ip)} pub={addr}")
        elif peer.state == "FALLBACK" and not peer.gave_up:
            peer.state = "PUNCHING"
            peer.t_pstart = time.monotonic()
            peer.punch_n = 0
            peer.t_punch = 0

    def on_punch(self, from_addr):
        ack = p.pack_small(p.MSG_PUNCH_ACK, self.node_id)
        self.sock.sendto(ack, from_addr)
        for peer in self.peers.values():
            if peer.addr[0] == from_addr[0] and peer.addr != from_addr:
                self.log(f"[P2P] NAT port update: {peer.addr[1]} -> {from_addr[1]}")
                peer.addr = from_addr
                break

    def on_punch_ack(self, from_addr, sender_nid):
        for nid, peer in self.peers.items():
            if nid == sender_nid or peer.addr == from_addr:
                was_direct = peer.direct()
                peer.addr = from_addr
                peer.state = "DIRECT"
                peer.t_p2prx = time.monotonic()
                peer.gave_up = False
                if not was_direct:
                    self.log(f"[P2P] DIRECT vpn={p.bytes_to_ip(peer.vpn_ip)} {from_addr} "
                              f"(punch#{peer.punch_n})")
                return

    def on_udp(self, buf: bytes, from_addr):
        if not buf:
            return
        from_srv = from_addr == self.server_addr
        if from_srv:
            self.t_srv_rx = time.monotonic()

        t = buf[0]
        if t == p.MSG_PEER_INFO and len(buf) >= p.REGPKT_SIZE:
            self.on_peer_info(p.unpack_reg(buf))
        elif t == p.MSG_PUNCH:
            self.on_punch(from_addr)
        elif t == p.MSG_PUNCH_ACK and len(buf) >= 5:
            self.on_punch_ack(from_addr, p.unpack_small(buf)["node_id"])
        elif t == p.MSG_KEEPALIVE:
            for peer in self.peers.values():
                if peer.addr == from_addr:
                    peer.t_p2prx = time.monotonic()
                    break
        elif t in (p.MSG_DATA, p.MSG_DATA_ENC):
            self.inject_tun(buf)
            for peer in self.peers.values():
                if peer.addr == from_addr:
                    peer.t_p2prx = time.monotonic()
                    break

    def inject_tun(self, buf: bytes):
        if len(buf) < p.DATAHDR_SIZE + 1:
            return
        h = p.unpack_data_header(buf)
        plen = h["payload_len"]
        body = buf[p.DATAHDR_SIZE:]
        if h["type"] == p.MSG_DATA:
            if len(body) < plen:
                return
            self.tun.write(body[:plen])
            self.rx += plen
        elif h["type"] == p.MSG_DATA_ENC:
            if len(body) < plen + 8:
                return
            nonce = int.from_bytes(body[plen:plen + 8], "little")
            plain = p.xcrypt(body[:plen], nonce)
            self.tun.write(plain)
            self.rx += plen

    # ── state machine tick ─────────────────────────────────────────────
    def tick_p2p(self):
        if not self.p2p:
            return
        now = time.monotonic()
        for peer in self.peers.values():
            if peer.state == "PUNCHING":
                if now - peer.t_punch >= T_PUNCH:
                    pk = p.pack_small(p.MSG_PUNCH, self.node_id)
                    self.sock.sendto(pk, peer.addr)
                    for delta in PUNCH_PORT_PROBES:
                        port = peer.addr[1] + delta
                        if 1 <= port <= 65535:
                            self.sock.sendto(pk, (peer.addr[0], port))
                    peer.punch_n += 1
                    peer.t_punch = now
                    if now - peer.t_pstart >= T_PMAX:
                        self.log(f"[P2P] Punch timeout vpn={p.bytes_to_ip(peer.vpn_ip)} "
                                  f"({peer.punch_n} attempts) -> relay fallback")
                        peer.state = "FALLBACK"
                        peer.gave_up = True
            elif peer.state == "DIRECT":
                if now - peer.t_ka >= T_KA:
                    ka = p.pack_small(p.MSG_KEEPALIVE, self.node_id)
                    self.sock.sendto(ka, peer.addr)
                    peer.t_ka = now
                if now - peer.t_p2prx >= T_DROP:
                    self.log(f"[P2P] vpn={p.bytes_to_ip(peer.vpn_ip)} timeout -> relay fallback")
                    peer.state = "FALLBACK"
                    peer.gave_up = False
            elif peer.state == "FALLBACK" and not peer.gave_up:
                if now - peer.t_pstart >= T_BGRETRY:
                    peer.state = "PUNCHING"
                    peer.t_pstart = now
                    peer.t_punch = 0
                    peer.punch_n = 0

    def force_punch_all(self):
        for peer in self.peers.values():
            peer.gave_up = False
            peer.state = "PUNCHING"
            peer.t_pstart = time.monotonic()
            peer.t_punch = 0
            peer.punch_n = 0
        self.do_register()

    def print_stats(self):
        sv_ok = self.t_srv_rx and (time.monotonic() - self.t_srv_rx) < T_SRVDEAD
        print("\n=== Client Stats ===")
        print(f"  Server  : {self.server_addr}  "
              f"({'CONNECTING' if not self.t_srv_rx else ('OK' if sv_ok else 'DEAD')})")
        print(f"  Comm    : {'P2P' if self.p2p else 'RELAY'}")
        print(f"  Encrypt : {'ON' if self.encrypt else 'OFF'}")
        print(f"  TX/RX   : {self.tx} / {self.rx} bytes")
        for peer in self.peers.values():
            print(f"  Peer vpn={p.bytes_to_ip(peer.vpn_ip):<15} pub={peer.addr}  "
                  f"state={peer.state}  punch#={peer.punch_n}")
        print()

    # ── main loop ───────────────────────────────────────────────────────
    def _tun_reader(self):
        while self.running:
            data = self.tun.read()
            if not data:
                time.sleep(0.005)
                continue
            if len(data) < 20:
                continue
            dst_vpn = data[16:20]
            self.send_vpn(data, dst_vpn)

    def _udp_reader(self):
        while self.running:
            try:
                buf, addr = self.sock.recvfrom(65536)
            except socket.timeout:
                continue
            except OSError:
                break
            self.on_udp(buf, addr)

    def run(self):
        self.log(f"[Client] node=0x{self.node_id:08X} vpn={self.vpn_ip_str}/{self.subnet} "
                  f"-> server {self.server_addr}  comm={'p2p' if self.p2p else 'relay'} "
                  f"encrypt={'on' if self.encrypt else 'off'}")
        self.do_register()

        threading.Thread(target=self._tun_reader, daemon=True).start()
        threading.Thread(target=self._udp_reader, daemon=True).start()

        t_status = 0.0
        try:
            while self.running:
                now = time.monotonic()
                reg_interval = T_REG_KA if self.t_srv_rx else T_REG
                if now - self.t_reg >= reg_interval:
                    self.do_register()
                self.tick_p2p()
                if now - t_status >= T_STATUS:
                    t_status = now
                    sv_ok = self.t_srv_rx and (now - self.t_srv_rx) < T_SRVDEAD
                    sv_s = "WAIT" if not self.t_srv_rx else ("OK" if sv_ok else "DEAD")
                    dc = sum(1 for pr in self.peers.values() if pr.direct())
                    self.log(f"[Status] Server:{sv_s}  Peers:{len(self.peers)}  "
                              f"Direct:{dc}  Enc:{'ON' if self.encrypt else 'OFF'}")
                time.sleep(0.05)
        except KeyboardInterrupt:
            pass
        finally:
            self.running = False
            self.print_stats()
            self.tun.close()
            self.sock.close()
