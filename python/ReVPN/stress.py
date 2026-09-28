"""
Same load-test idea as ReVPN/stress_test.sh + tools/stress_client.py, native
to ReVPN-py: spins up a throwaway Server (in-process, a background thread)
and N synthetic UDP "clients" that REGISTER and flood MSG_DATA at a fixed
rate, arranged in a ring, then reports the server's observed throughput.
No root, no TUN, cross-platform.
"""
from __future__ import annotations

import os
import socket
import threading
import time

from . import protocol as p
from .server import Server


def _vpn_ip_for(i: int) -> str:
    return f"10.250.{(i // 250) % 250}.{(i % 250) + 2}"


def _synthetic_client(server_addr, vpn_ip: str, dst_vpn: str, node_id: int,
                        duration: float, rate: float, size: int, counters: list, idx: int):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(0)
    vpn_bytes = socket.inet_aton(vpn_ip)
    dst_bytes = socket.inet_aton(dst_vpn)

    reg = p.pack_reg(p.MSG_REGISTER, node_id, vpn_bytes, 0)
    sock.sendto(reg, server_addr)

    payload = os.urandom(size)
    pkt = p.pack_data_header(p.MSG_DATA, vpn_bytes, dst_bytes, len(payload)) + payload

    interval = 1.0 / rate if rate > 0 else 0
    t_end = time.monotonic() + duration
    t_reg = time.monotonic()
    sent = 0
    while time.monotonic() < t_end:
        t0 = time.monotonic()
        try:
            sock.sendto(pkt, server_addr)
            sent += 1
        except OSError:
            pass
        if time.monotonic() - t_reg > 5:
            sock.sendto(reg, server_addr)
            t_reg = time.monotonic()
        try:
            while True:
                sock.recvfrom(65536)
        except OSError:
            pass
        if interval:
            dt = interval - (time.monotonic() - t0)
            if dt > 0:
                time.sleep(dt)
    counters[idx] = sent
    sock.close()


def run_stress(clients=20, duration=10.0, rate=500.0, size=512, port=19099, log=print):
    log("=" * 62)
    log(" ReVPN-py stress test")
    log("=" * 62)
    log(f"  clients  : {clients}")
    log(f"  duration : {duration}s")
    log(f"  rate     : {rate} pps/client (target total: {int(clients * rate)} pps)")
    log(f"  size     : {size} bytes payload")
    log(f"  server   : 127.0.0.1:{port}")
    log("=" * 62)

    srv = Server("127.0.0.1", port, log=lambda *_: None)
    srv_thread = threading.Thread(target=srv.run, daemon=True)
    srv_thread.start()
    time.sleep(0.3)

    counters = [0] * clients
    threads = []
    t_start = time.monotonic()
    for i in range(clients):
        dst = (i + 1) % clients
        t = threading.Thread(
            target=_synthetic_client,
            args=((("127.0.0.1", port)), _vpn_ip_for(i), _vpn_ip_for(dst), i + 1,
                  duration, rate, size, counters, i),
            daemon=True,
        )
        threads.append(t)
        t.start()

    log(f"running for {duration}s ...")
    for t in threads:
        t.join()
    elapsed = time.monotonic() - t_start

    sent_total = sum(counters)
    srv.running = False
    time.sleep(1.2)  # let the run() loop's 1s recv timeout notice and exit

    pkt_bytes = size + p.DATAHDR_SIZE
    expected_pkts = int(clients * rate * duration)
    expected_bytes = expected_pkts * pkt_bytes
    rx_pps = (srv.rx_bytes / pkt_bytes) / elapsed if elapsed > 0 else 0
    rx_mbps = (srv.rx_bytes * 8) / 1_000_000 / elapsed if elapsed > 0 else 0
    loss_pct = max(0.0, (expected_bytes - srv.rx_bytes) / expected_bytes * 100) if expected_bytes else 0.0

    log("=" * 62)
    log(" RESULTS")
    log("=" * 62)
    log(f"  wall time             : {elapsed:.2f}s")
    log(f"  client-reported sent  : {sent_total} packets")
    log(f"  expected (target)     : {expected_pkts} packets / {expected_bytes} bytes")
    log(f"  server-observed rx    : {srv.rx_bytes} bytes  (~{rx_pps:.0f} pkt/s, {rx_mbps:.2f} Mbps)")
    log(f"  server-observed fwd   : {srv.fwd_bytes} bytes  (routed to peers)")
    log(f"  estimated packet loss : {loss_pct:.2f}%")
    log("=" * 62)
