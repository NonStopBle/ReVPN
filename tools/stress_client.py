#!/usr/bin/env python3
"""
Synthetic ReVPN client used only for load-testing the relay server.

It speaks just enough of the engine's UDP wire protocol (see meshvpn.cpp)
to REGISTER and then flood MSG_DATA packets at a fixed rate/size/duration —
no TUN device, no root required. This lets `ReVPN --stress` hammer the
relay/routing path (the part that matters under load) on a single machine
with hundreds of "clients" that a real TUN-based test could never spin up
locally.

Wire format (all struct fields are exactly as meshvpn.cpp packs them):
  RegPkt : type(1) node_id(4) vpn_ip(4) udp_port(2,BE) public_ip(4) pad(35) = 50B
  DataHdr: type(1) src_vpn(4) dst_vpn(4) payload_len(2)                    = 11B
MSG_REGISTER=0x01  MSG_DATA=0x20
"""
import argparse
import os
import socket
import struct
import time

MSG_REGISTER = 0x01
MSG_DATA = 0x20


def parse_addr(s):
    ip, port = s.rsplit(":", 1)
    return ip, int(port)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--server", required=True, help="ip:port of the ReVPN server")
    ap.add_argument("--vpn-ip", required=True, help="this synthetic client's VPN IP")
    ap.add_argument("--dst-vpn", required=True, help="VPN IP of the peer to send to")
    ap.add_argument("--node-id", required=True, help="hex node id, unique per client")
    ap.add_argument("--duration", type=float, default=10.0)
    ap.add_argument("--rate", type=float, default=500.0, help="packets/sec")
    ap.add_argument("--size", type=int, default=512, help="payload bytes")
    args = ap.parse_args()

    srv_ip, srv_port = parse_addr(args.server)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setblocking(False)

    node_id = int(args.node_id, 16) & 0xFFFFFFFF
    vpn_ip = socket.inet_aton(args.vpn_ip)
    dst_vpn = socket.inet_aton(args.dst_vpn)

    reg = struct.pack("<B I 4s H 4s 35s", MSG_REGISTER, node_id, vpn_ip,
                       0, b"\x00" * 4, b"\x00" * 35)
    sock.sendto(reg, (srv_ip, srv_port))

    payload = os.urandom(max(0, args.size))
    hdr = struct.pack("<B 4s 4s H", MSG_DATA, vpn_ip, dst_vpn, len(payload))
    pkt = hdr + payload

    interval = 1.0 / args.rate if args.rate > 0 else 0
    t_end = time.time() + args.duration
    t_reg = time.time()
    sent = 0

    while time.time() < t_end:
        t0 = time.time()
        try:
            sock.sendto(pkt, (srv_ip, srv_port))
            sent += 1
        except (BlockingIOError, OSError):
            pass
        # keep the registration fresh (server TTL is 30s)
        if time.time() - t_reg > 5:
            sock.sendto(reg, (srv_ip, srv_port))
            t_reg = time.time()
        # drain any inbound (forwarded) traffic so recv buffers don't fill
        try:
            while True:
                sock.recvfrom(65536)
        except (BlockingIOError, OSError):
            pass
        if interval:
            dt = interval - (time.time() - t0)
            if dt > 0:
                time.sleep(dt)

    print(sent)


if __name__ == "__main__":
    main()
