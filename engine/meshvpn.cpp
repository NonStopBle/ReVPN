// ============================================================================
// meshvpn.cpp — Hub-and-spoke VPN, optimized for high throughput
// ============================================================================
// Created by Rezier Labs
// License: PolyForm Noncommercial 1.0.0 — see LICENSE in the repository root.
// ============================================================================
// Build (standard — recvmmsg):
//   g++ -std=c++17 -O3 -pthread meshvpn.cpp -o meshvpn
// Build (AF_XDP zero-copy — kernel >= 5.1 + XDP NIC):
//   clang -O2 -g -target bpf -I/usr/include -c xdp_kern.c -o /tmp/xdp_kern.o
//   g++ -std=c++17 -O3 -pthread -DWITH_XDP meshvpn.cpp xdp_sock.cpp -lbpf -o meshvpn
//
// SERVER (pure bridge/router — NO TUN, NO VPN IP):
//   sudo ./meshvpn --mode server --bind 0.0.0.0:9000 [--workers 4]
//
// CLIENT:
//   sudo ./meshvpn --mode client --vpn-ip 10.13.0.2 \
//                  --server 1.2.3.4:9000 --comm relay
//   sudo ./meshvpn --mode client --vpn-ip 10.13.0.3 \
//                  --server 1.2.3.4:9000 --comm p2p
//
// Optimizations + bug fixes vs previous version:
//   - FIX1: last_seen is std::atomic — safe concurrent write by N workers
//   - FIX2: by_addr reverse map → O(1) punch sender lookup (was O(N))
//   - FIX3: on_punch_ack matches by full IP:port (CGNAT safe, was IP-only)
//   - FIX4: keepalive/data t_p2prx update by full IP:port (was IP-only)
//   - FIX5: cleanup_loop now also erases by_addr entries
//   - FIX6: on_keepalive uses shared_lock + atomic (was unique_lock)
//   - OPT:  Symmetric NAT port prediction widened from ±2 to ±8 probes
//   - OPT:  Server N worker threads + SO_REUSEPORT (true parallel recv)
//   - OPT:  recvmmsg(64) batch → 64× fewer syscalls per worker
//   - OPT:  epoll(ET) drain loop on client
//   - OPT:  CPU pinning + SCHED_FIFO on workers
//
// Punch state machine (verified working):
//   RELAY ──peer_info──► PUNCHING ──10s──► FALLBACK
//                            ↓ ack                ↓ 30s bg retry
//                         DIRECT ──3s drop──► FALLBACK
//   In FALLBACK with gave_up=true: no auto-retry.  'f' to force.
//   In FALLBACK with gave_up=false: auto-retry every 30s.
// ============================================================================

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <queue>
#include <shared_mutex>
#include <stdexcept>
#include <iostream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// ============================================================================
// Cross-platform socket/OS layer.
//
// SERVER mode (rendezvous/relay, no TUN) is fully cross-platform: it only
// needs UDP sockets + threads, so a MinGW/Windows build of this same source
// gets a real, working ReVPN-engine.exe server that interoperates with the
// Linux engine and with ReVPN-py, byte-for-byte same wire protocol.
//
// CLIENT mode needs a TUN device. Linux uses /dev/net/tun; Windows has no
// equivalent kernel primitive, so this build loads Wintun (wintun.dll,
// https://www.wintun.net/) dynamically and drives it directly — same
// adapter tech OpenVPN2/WireGuard use on Windows. Untested on real Windows
// hardware (built/run so far only under Wine — see README's Windows-builds
// section); ReVPN-py's client has an independent Wintun backend too.
// ============================================================================
#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  #include <objbase.h> // CoCreateGuid — Wintun adapter identity
  #pragma comment(lib, "ws2_32.lib")
  #pragma comment(lib, "ole32.lib")
  #define CLOSESOCK(fd) closesocket((SOCKET)(fd))
  #define SOCK_ERRNO WSAGetLastError()
  // WSAPOLLFD has the same fd/events/revents layout POSIX pollfd does, so
  // the existing poll(&pfd, 1, ms) call sites work unchanged on a SOCKET.
  typedef WSAPOLLFD pollfd;
  static inline int poll(pollfd* fds, unsigned long n, int timeout) {
      return WSAPoll(fds, n, timeout);
  }

  // ── Wintun (https://www.wintun.net/) — loaded dynamically via
  // LoadLibraryW/GetProcAddress rather than linked against an import lib,
  // so ReVPN-engine.exe still runs (server mode, or --help) on a machine
  // that doesn't have wintun.dll — only --mode client needs it present.
  // Signatures per the API reference at git.zx2c4.com/wintun/about/#reference
  // (all strings are WCHAR*, all sizes are DWORD).
  struct WintunApi {
      HMODULE dll = nullptr;
      void*    (WINAPI *CreateAdapter)(const wchar_t*, const wchar_t*, const GUID*) = nullptr;
      void     (WINAPI *CloseAdapter)(void*) = nullptr;
      void*    (WINAPI *StartSession)(void*, DWORD) = nullptr;
      void     (WINAPI *EndSession)(void*) = nullptr;
      HANDLE   (WINAPI *GetReadWaitEvent)(void*) = nullptr;
      uint8_t* (WINAPI *ReceivePacket)(void*, DWORD*) = nullptr;
      void     (WINAPI *ReleaseReceivePacket)(void*, const uint8_t*) = nullptr;
      uint8_t* (WINAPI *AllocateSendPacket)(void*, DWORD) = nullptr;
      void     (WINAPI *SendPacket)(void*, const uint8_t*) = nullptr;

      bool load() {
          if (dll) return true; // already loaded
          dll = LoadLibraryW(L"wintun.dll");
          if (!dll) return false;
          CreateAdapter        = (decltype(CreateAdapter))       GetProcAddress(dll, "WintunCreateAdapter");
          CloseAdapter         = (decltype(CloseAdapter))        GetProcAddress(dll, "WintunCloseAdapter");
          StartSession         = (decltype(StartSession))        GetProcAddress(dll, "WintunStartSession");
          EndSession           = (decltype(EndSession))          GetProcAddress(dll, "WintunEndSession");
          GetReadWaitEvent     = (decltype(GetReadWaitEvent))    GetProcAddress(dll, "WintunGetReadWaitEvent");
          ReceivePacket        = (decltype(ReceivePacket))       GetProcAddress(dll, "WintunReceivePacket");
          ReleaseReceivePacket = (decltype(ReleaseReceivePacket))GetProcAddress(dll, "WintunReleaseReceivePacket");
          AllocateSendPacket   = (decltype(AllocateSendPacket))  GetProcAddress(dll, "WintunAllocateSendPacket");
          SendPacket           = (decltype(SendPacket))          GetProcAddress(dll, "WintunSendPacket");
          return CreateAdapter && CloseAdapter && StartSession && EndSession &&
                 GetReadWaitEvent && ReceivePacket && ReleaseReceivePacket &&
                 AllocateSendPacket && SendPacket;
      }
  };
  static WintunApi g_wintun;
#else
  #include <arpa/inet.h>
  #include <fcntl.h>
  #include <ifaddrs.h>
  #include <linux/if_tun.h>
  #include <net/if.h>
  #include <netinet/in.h>
  #include <poll.h>
  #include <pthread.h>
  #include <sched.h>
  #include <sys/epoll.h>
  #include <sys/ioctl.h>
  #include <sys/socket.h>
  #include <unistd.h>
  #define CLOSESOCK(fd) close(fd)
  #define SOCK_ERRNO errno
#endif

// ── Optional AF_XDP zero-copy (requires kernel >= 5.1, Linux-only) ────────
// Build: g++ ... -DWITH_XDP meshvpn.cpp xdp_sock.cpp -lbpf -o meshvpn
#ifdef WITH_XDP
#include "xdp_sock.hpp"
#endif

// recvmmsg / sendmmsg batch size (Linux fast path only — see recv_batch())
static constexpr int BATCH = 64;
// max VPN payload
static constexpr size_t MAX_PKT = 65536;

#ifdef _WIN32
// ── stdin command thread (Windows has no poll()-on-stdin like POSIX) ──────
// Both Server::run() and the interactive status/quit keys use this: a
// background thread blocking on a line read, publishing single-char
// "commands" the main loop's timer-driven poll checks between packets.
static std::atomic<char> g_stdin_cmd{0};
static void start_stdin_thread() {
    std::thread([]{
        std::string line;
        while (std::getline(std::cin, line)) {
            if (!line.empty()) g_stdin_cmd.store(line[0], std::memory_order_relaxed);
        }
    }).detach();
}
#endif

// ── Timing ────────────────────────────────────────────────────────────────────
static uint64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()).count();
}

// ── Address helpers ───────────────────────────────────────────────────────────
static std::string addrstr(const sockaddr_in& a) {
    char b[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &a.sin_addr, b, sizeof(b));
    return std::string(b) + ":" + std::to_string(ntohs(a.sin_port));
}
static std::string ip4str(uint32_t ip_net) {
    in_addr a; a.s_addr = ip_net;
    char b[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &a, b, sizeof(b));
    return b;
}

// ── Protocol ──────────────────────────────────────────────────────────────────
enum Msg : uint8_t {
    MSG_REGISTER  = 0x01,
    MSG_PEER_INFO = 0x02,
    MSG_PUNCH     = 0x03,
    MSG_PUNCH_ACK = 0x04,
    MSG_KEEPALIVE = 0x05,
    MSG_DATA      = 0x20,
    MSG_DATA_ENC  = 0x21,
};

// REGISTER / PEER_INFO — 50 bytes
struct __attribute__((packed)) RegPkt {
    uint8_t  type;
    uint32_t node_id;
    uint32_t vpn_ip;
    uint16_t udp_port;      // big-endian
    uint8_t  public_ip[4];
    uint8_t  _pad[35];
};
static_assert(sizeof(RegPkt) == 50);

struct __attribute__((packed)) PunchPkt {
    uint8_t  type;
    uint32_t node_id;
};

struct __attribute__((packed)) KaPkt {
    uint8_t  type;
    uint32_t node_id;
};

// VPN data header: type(1)+src_vpn(4)+dst_vpn(4)+payload_len(2) = 11 bytes
struct __attribute__((packed)) DataHdr {
    uint8_t  type;
    uint32_t src_vpn;
    uint32_t dst_vpn;   // 0 = broadcast
    uint16_t payload_len;
};
static constexpr size_t DHSZ = sizeof(DataHdr); // 11

// ── Crypto placeholder (XOR — replace with AES-256-GCM via OpenSSL) ───────────
static const uint8_t XK[32] = {
    0x4d,0x65,0x73,0x68,0x56,0x50,0x4e,0x4b,
    0x65,0x79,0x30,0x31,0x32,0x33,0x34,0x35,
    0x36,0x37,0x38,0x39,0x61,0x62,0x63,0x64,
    0x65,0x66,0x67,0x68,0x69,0x6a,0x6b,0x6c
};
static void xcrypt(const uint8_t* in, uint8_t* out, size_t n, uint64_t nc) {
    const uint8_t* nb = (const uint8_t*)&nc;
    for (size_t i = 0; i < n; i++) out[i] = in[i] ^ XK[i%32] ^ nb[i%8];
}

// ── Signal ────────────────────────────────────────────────────────────────────
static volatile bool g_quit = false;
static void onsig(int) { g_quit = true; }

// ── CPU pinning + real-time priority (Linux fast path; no-op elsewhere) ──────
static void pin_thread(int cpu) {
    if (cpu < 0) return;
#ifdef __linux__
    cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(cpu, &cs);
    pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
    sched_param sp{}; sp.sched_priority = 60;
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
#endif
    // Windows/macOS: skip — this is a perf tweak, not a correctness
    // requirement, and Windows thread affinity/priority APIs differ enough
    // (SetThreadAffinityMask/SetThreadPriority) that it's not worth the
    // portability risk for a "nice to have".
}

// ── Open UDP socket with SO_REUSEPORT + large buffers ────────────────────────
static int open_udp(uint16_t port, bool nonblock = true) {
#ifdef _WIN32
    SOCKET wfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (wfd == INVALID_SOCKET)
        throw std::runtime_error("socket() failed: " + std::to_string(SOCK_ERRNO));
    int fd = (int)wfd;
    if (nonblock) { u_long mode = 1; ioctlsocket(wfd, FIONBIO, &mode); }
#else
    int flags = SOCK_DGRAM | SOCK_CLOEXEC | (nonblock ? SOCK_NONBLOCK : 0);
    int fd = socket(AF_INET, flags, 0);
    if (fd < 0) throw std::runtime_error("socket() failed: " +
                                          std::string(strerror(errno)));
#endif
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,  (const char*)&one, sizeof(one));
#ifndef _WIN32
    // SO_REUSEPORT doesn't exist on Windows; SO_REUSEADDR above is enough
    // for a single-process bind, but Windows builds lose the "N sockets on
    // the same port, kernel load-balances" trick — server still works
    // correctly with --workers 1 there (or accept single-socket fan-in).
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT,  &one, sizeof(one));
#endif

    // Large buffers — each worker thread has its own socket so multiply
    // effective buffer by N workers automatically
    // FIX4: 64MB both — critical for 1GB file transfers without stall
    int rb = 64 * 1024 * 1024; // 64 MB recv
    int sb = 64 * 1024 * 1024; // 64 MB send
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (const char*)&rb, sizeof(rb));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (const char*)&sb, sizeof(sb));

    sockaddr_in b{};
    b.sin_family = AF_INET; b.sin_addr.s_addr = INADDR_ANY;
    b.sin_port   = htons(port);
    if (bind(fd, (sockaddr*)&b, sizeof(b)) < 0)
        throw std::runtime_error(
            std::string("bind() :") + std::to_string(port) +
            ": " + std::to_string(SOCK_ERRNO));
    return fd;
}

// ── Linux TUN open+configure — shared by Client and, when --vpn-ip is
// given, Server (server-as-peer mode). Same ioctls/`ip route`/TCPMSS-clamp
// as the original client-only implementation this was extracted from.
#ifdef __linux__
static int open_linux_tun(const char* vpn_ip, int prefix, int mtu,
                           const char* name, std::string& out_ifname) {
    int fd = open("/dev/net/tun", O_RDWR|O_CLOEXEC|O_NONBLOCK);
    if (fd < 0) throw std::runtime_error("open /dev/net/tun (need root)");
    ifreq ifr{}; ifr.ifr_flags = IFF_TUN|IFF_NO_PI;
    strncpy(ifr.ifr_name, name, IFNAMSIZ-1);
    if (ioctl(fd, TUNSETIFF, &ifr) < 0)
        throw std::runtime_error(std::string("TUNSETIFF: ")+strerror(errno));

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    { ifreq r{}; memcpy(r.ifr_name,ifr.ifr_name,IFNAMSIZ);
      auto*sa=(sockaddr_in*)&r.ifr_addr; sa->sin_family=AF_INET;
      sa->sin_addr.s_addr=inet_addr(vpn_ip); ioctl(s,SIOCSIFADDR,&r); }
    { ifreq r{}; memcpy(r.ifr_name,ifr.ifr_name,IFNAMSIZ);
      uint32_t mask=prefix?htonl(~((1u<<(32-prefix))-1)):0;
      auto*n=(sockaddr_in*)&r.ifr_netmask; n->sin_family=AF_INET;
      n->sin_addr.s_addr=mask; ioctl(s,SIOCSIFNETMASK,&r); }
    { ifreq r{}; memcpy(r.ifr_name,ifr.ifr_name,IFNAMSIZ);
      r.ifr_mtu=mtu; ioctl(s,SIOCSIFMTU,&r); }
    { ifreq r{}; memcpy(r.ifr_name,ifr.ifr_name,IFNAMSIZ);
      ioctl(s,SIOCGIFFLAGS,&r); r.ifr_flags|=IFF_UP|IFF_RUNNING;
      if(ioctl(s,SIOCSIFFLAGS,&r)<0)
          throw std::runtime_error("bring up tun failed"); }
    close(s);

    uint32_t mask=prefix?htonl(~((1u<<(32-prefix))-1)):0;
    in_addr net; net.s_addr=inet_addr(vpn_ip)&mask;
    char ns[INET_ADDRSTRLEN]; inet_ntop(AF_INET,&net,ns,sizeof(ns));
    std::string cmd="ip route add "+std::string(ns)+"/"+
        std::to_string(prefix)+" dev "+ifr.ifr_name+" 2>/dev/null||true";
    system(cmd.c_str());
    system("iptables -t mangle -C FORWARD -p tcp --tcp-flags SYN,RST SYN "
           "-j TCPMSS --set-mss 1200 2>/dev/null || "
           "iptables -t mangle -A FORWARD -p tcp --tcp-flags SYN,RST SYN "
           "-j TCPMSS --set-mss 1200 2>/dev/null || true");
    printf("[TUN] %s  ip=%s/%d  mtu=%d\n", ifr.ifr_name, vpn_ip, prefix, mtu);
    out_ifname = ifr.ifr_name;
    return fd;
}
#endif

// ── Fast UDP send ─────────────────────────────────────────────────────────────
// In AF_XDP mode there is no real per-worker socket fd to send from (RX
// arrives via the XDP ring, not a bound socket) -- run_xdp() calls
// dispatch() with fd=-1 to signal that, and every reply/forward path
// (on_register/on_data/on_punch) threads that same fd through to here
// unchanged. Route a negative fd through this thread's XdpFastPath
// instead, set once per XDP worker thread before it starts draining its
// ring, so the existing dispatch code needs no changes beyond this.
#ifdef WITH_XDP
static thread_local XdpWorker* tls_xdp_worker = nullptr;
#endif
static inline void usend(int fd, const void* d, size_t n,
                          const sockaddr_in& dst) {
#ifdef WITH_XDP
    if (fd < 0) {
        if (tls_xdp_worker) tls_xdp_worker->send(d, n, dst);
        return;
    }
#endif
#ifdef _WIN32
    sendto(fd, (const char*)d, (int)n, 0, (const sockaddr*)&dst, sizeof(dst));
#else
    sendto(fd, d, n, MSG_DONTWAIT | MSG_NOSIGNAL,
           (const sockaddr*)&dst, sizeof(dst));
#endif
}

// ============================================================================
// SERVER — pure bridge, no TUN, multi-threaded with SO_REUSEPORT
// ============================================================================
struct Server {
    struct ClientEntry {
        sockaddr_in              pub_addr;
        uint32_t                 vpn_ip;
        uint32_t                 node_id;
        std::atomic<uint64_t>    last_seen{0}; // FIX1: atomic — written by N workers
    };

    uint16_t    bind_port;
    int         n_workers;
    std::string xdp_ifname;          // e.g. "eth0" — empty = recvmmsg
    bool        xdp_force_copy = false;

    // Optional: server also joins the mesh as a peer, reachable at its own
    // VPN IP (e.g. so clients can reach the server host itself, not just
    // each other). Off by default — pure bridge, as before — enabled with
    // --vpn-ip on the server CLI. Linux uses open_linux_tun(); Windows uses
    // Wintun (same backend as client/decentralized mode).
    uint32_t    self_vpn_ip   = 0;    // network byte order; 0 = disabled
    uint32_t    self_node_id  = 0;
    bool        self_encrypt  = true;
    int         tun_fd        = -1;
    uint64_t    self_nonce    = 0;
    std::thread self_tun_thread;
#ifdef _WIN32
    void* wintun_adapter = nullptr;
    void* wintun_session = nullptr;
#endif

    // Routing table — protected by shared_mutex (many-reader, single-writer)
    mutable std::shared_mutex tbl_mtx;
    std::unordered_map<uint32_t, ClientEntry> by_node; // node_id → entry
    std::unordered_map<uint32_t, uint32_t>    by_vpn;  // vpn_ip  → node_id

    // by_addr: addr_key(ip,port) → node_id  — O(1) punch sender lookup (FIX2)
    std::unordered_map<uint64_t, uint32_t> by_addr;

    // Per-worker socket FDs (SO_REUSEPORT — kernel hashes packets across them)
    std::vector<int> worker_fds;

    std::atomic<uint64_t> stat_rx{0}, stat_fwd{0};
    std::atomic<bool>     running{true};

    static constexpr uint64_t CLIENT_TTL  = 30000;

    // Compose a unique 64-bit key from IP:port for O(1) reverse lookup
    static uint64_t addr_key(const sockaddr_in& a) {
        return ((uint64_t)a.sin_addr.s_addr << 32) | (uint64_t)a.sin_port;
    }

    // ── Routing helpers ──────────────────────────────────────────────────────
    // FIX1: NEVER call sendto() while holding the lock.
    // These helpers COLLECT destination addresses under the lock.
    // Caller releases the lock THEN sends. Prevents lock starvation if
    // the kernel send buffer is momentarily full.

    // Collect address for unicast (returns false if node not found)
    bool collect_unicast(uint32_t node_id, sockaddr_in& out_dst) {
        auto it = by_node.find(node_id);
        if (it == by_node.end()) return false;
        out_dst = it->second.pub_addr;
        it->second.last_seen.store(now_ms(), std::memory_order_relaxed);
        return true;
    }

    // Collect all addresses except skip_node for broadcast/punch relay
    void collect_broadcast(uint32_t skip_node,
                           std::vector<sockaddr_in>& out_dsts) {
        out_dsts.reserve(by_node.size());
        for (auto& [nid, c] : by_node) {
            if (nid == skip_node) continue;
            out_dsts.push_back(c.pub_addr);
        }
    }

    // ── REGISTER ──────────────────────────────────────────────────────────────
    void on_register(const uint8_t* buf, size_t len,
                     const sockaddr_in& from, int fd) {
        if (len < sizeof(RegPkt)) return;
        const auto* r = (const RegPkt*)buf;

        std::unique_lock<std::shared_mutex> lk(tbl_mtx);
        bool is_new = (by_node.find(r->node_id) == by_node.end());

        ClientEntry& c = by_node[r->node_id];
        c.pub_addr  = from;
        c.vpn_ip    = r->vpn_ip;
        c.node_id   = r->node_id;
        c.last_seen = now_ms();
        by_vpn[r->vpn_ip] = r->node_id;

        // FIX2: maintain by_addr reverse map
        by_addr[addr_key(from)] = r->node_id;

        if (is_new)
            printf("[Server] + Client  vpn=%-16s  pub=%-24s  node=0x%08X\n",
                   ip4str(r->vpn_ip).c_str(), addrstr(from).c_str(), r->node_id);

        // Build announcements under lock:
        //   NEW client     → tell all existing about newcomer + newcomer about all
        //   EXISTING client → re-send PEER_INFO of current peers to it (refresh)
        // Also build an ACK (MSG_KEEPALIVE) to send back as heartbeat proof-of-life.
        // FIX3: re-send PEER_INFO on every REGISTER so reconnecting clients get peers.
        struct Ann { RegPkt pkt; sockaddr_in dst; };
        std::vector<Ann> to_send;
        to_send.reserve(by_node.size() * 2 + 1);

        for (auto& [nid, ce] : by_node) {
            if (nid == r->node_id) continue;

            if (is_new) {
                // Tell existing clients about the newcomer
                RegPkt p1{};
                p1.type = MSG_PEER_INFO; p1.node_id = c.node_id;
                p1.vpn_ip = c.vpn_ip;   p1.udp_port = c.pub_addr.sin_port;
                memcpy(p1.public_ip, &c.pub_addr.sin_addr, 4);
                to_send.push_back({p1, ce.pub_addr});
            }

            // Tell registering client about this existing peer (new AND existing)
            RegPkt p2{};
            p2.type = MSG_PEER_INFO; p2.node_id = ce.node_id;
            p2.vpn_ip = ce.vpn_ip;  p2.udp_port = ce.pub_addr.sin_port;
            memcpy(p2.public_ip, &ce.pub_addr.sin_addr, 4);
            to_send.push_back({p2, from});
        }
        lk.unlock();

        // FIX2: always ACK the REGISTER with MSG_KEEPALIVE so client sees server alive.
        // Client updates t_srv_rx on any packet from server — this keeps it from
        // declaring DEAD between T_REG_KA heartbeats.
        KaPkt ack{}; ack.type = MSG_KEEPALIVE; ack.node_id = 0;
        usend(fd, &ack, sizeof(ack), from);

        // Send PEER_INFO announcements (burst 3x for reliability)
        for (int burst = 0; burst < 3; burst++)
            for (auto& a : to_send)
                usend(fd, &a.pkt, sizeof(a.pkt), a.dst);
    }

    // ── Server-as-peer: own TUN device, reachable at self_vpn_ip ──────────────
    void setup_self_tun(const char* vpn_ip, int prefix, int mtu) {
#ifdef __linux__
        std::string ifname;
        tun_fd = open_linux_tun(vpn_ip, prefix, mtu, "tun0", ifname);
        self_vpn_ip  = inet_addr(vpn_ip);
        uint32_t h32 = self_vpn_ip; h32 ^= h32>>16; h32 *= 0x45d9f3b; h32 ^= h32>>16;
        self_node_id = h32;
        self_tun_thread = std::thread([this]{ self_tun_reader(); });
#elif defined(_WIN32)
        if (!g_wintun.load())
            throw std::runtime_error(
                "wintun.dll not found (or missing expected exports). "
                "Download it from https://www.wintun.net/ and place "
                "wintun.dll next to ReVPN-engine.exe, then run as "
                "Administrator.");

        GUID guid{};
        CoCreateGuid(&guid);
        wintun_adapter = g_wintun.CreateAdapter(L"ReVPNS0", L"ReVPN-Server", &guid);
        if (!wintun_adapter)
            throw std::runtime_error(
                "WintunCreateAdapter failed (GetLastError=" +
                std::to_string(GetLastError()) + ") — need Administrator?");

        wintun_session = g_wintun.StartSession(wintun_adapter, 0x400000);
        if (!wintun_session) {
            g_wintun.CloseAdapter(wintun_adapter); wintun_adapter = nullptr;
            throw std::runtime_error(
                "WintunStartSession failed (GetLastError=" +
                std::to_string(GetLastError()) + ")");
        }

        uint32_t mask = prefix ? htonl(~((1u<<(32-prefix))-1)) : 0;
        in_addr m{}; m.s_addr = mask;
        char maskbuf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &m, maskbuf, sizeof(maskbuf));
        std::string ipcmd = std::string("netsh interface ip set address ") +
            "name=\"ReVPNS0\" static " + vpn_ip + " " + maskbuf;
        system(ipcmd.c_str());
        std::string mtucmd = "netsh interface ipv4 set subinterface "
            "\"ReVPNS0\" mtu=" + std::to_string(mtu) + " store=persistent";
        system(mtucmd.c_str());

        printf("[TUN] ReVPNS0 (Wintun)  ip=%s/%d  mtu=%d\n", vpn_ip, prefix, mtu);

        self_vpn_ip  = inet_addr(vpn_ip);
        uint32_t h32 = self_vpn_ip; h32 ^= h32>>16; h32 *= 0x45d9f3b; h32 ^= h32>>16;
        self_node_id = h32;
        self_tun_thread = std::thread([this]{ self_tun_reader(); });
#else
        (void)vpn_ip; (void)prefix; (void)mtu;
        throw std::runtime_error(
            "server --vpn-ip (server-as-peer) needs a TUN device, which "
            "this build does not support on this OS yet. Run the server "
            "under Linux/WSL2/Windows, or omit --vpn-ip for plain relay mode.");
#endif
    }

    // Hands a plaintext IP packet to whichever TUN backend this OS has.
    void self_tun_write(const uint8_t* pkt, size_t n) {
#ifdef __linux__
        write(tun_fd, pkt, n);
#elif defined(_WIN32)
        uint8_t* p = g_wintun.AllocateSendPacket(wintun_session, (DWORD)n);
        if (!p) return; // ring full — drop, same as a blocked write() would
        memcpy(p, pkt, n);
        g_wintun.SendPacket(wintun_session, p);
#else
        (void)pkt; (void)n; // unreachable — no TUN backend on this build
#endif
    }

    // Decrypt (if needed) an inbound DATA/DATA_ENC packet addressed to the
    // server's own VPN IP and hand it to the local TUN, instead of
    // forwarding it to another client.
    void deliver_to_self(const uint8_t* buf, size_t len) {
#if defined(__linux__) || defined(_WIN32)
        if (len < DHSZ+1) return;
        const auto* h = (const DataHdr*)buf;
        uint16_t plen = h->payload_len;
        if (h->type == MSG_DATA) {
            if (DHSZ+plen > len) return;
            self_tun_write(buf+DHSZ, plen);
        } else if (h->type == MSG_DATA_ENC) {
            if (DHSZ+plen+8 > len) return;
            uint64_t nc; memcpy(&nc, buf+DHSZ+plen, 8);
            static thread_local uint8_t plain[1400];
            if (plen > sizeof(plain)) return;
            xcrypt(buf+DHSZ, plain, plen, nc);
            self_tun_write(plain, plen);
        }
#else
        (void)buf; (void)len;
#endif
    }

    // Server-originated traffic: TUN -> a client, addressed by the routing
    // table exactly like a normal client's send_vpn().
    void send_self(const uint8_t* pkt, size_t len, uint32_t dst_vpn) {
        if (!len || len > 1400 || worker_fds.empty()) return;
        sockaddr_in dst{};
        {
            std::shared_lock<std::shared_mutex> lk(tbl_mtx);
            auto it = by_vpn.find(dst_vpn);
            if (it == by_vpn.end()) return;
            auto cit = by_node.find(it->second);
            if (cit == by_node.end()) return;
            dst = cit->second.pub_addr;
        }
        static thread_local uint8_t obuf[1400 + DHSZ + 8];
        auto* h = (DataHdr*)obuf;
        h->src_vpn = self_vpn_ip; h->dst_vpn = dst_vpn; h->payload_len = (uint16_t)len;
        int fd = worker_fds[0];
        if (self_encrypt) {
            h->type = MSG_DATA_ENC;
            uint64_t nc = self_nonce++;
            xcrypt(pkt, obuf+DHSZ, len, nc);
            memcpy(obuf+DHSZ+len, &nc, 8);
            usend(fd, obuf, DHSZ+len+8, dst);
        } else {
            h->type = MSG_DATA;
            memcpy(obuf+DHSZ, pkt, len);
            usend(fd, obuf, DHSZ+len, dst);
        }
    }

    void self_tun_reader() {
#ifdef __linux__
        static uint8_t buf[65536];
        while (running.load(std::memory_order_relaxed)) {
            ssize_t r = read(tun_fd, buf, sizeof(buf));
            if (r <= 0) {
                if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK) break;
                usleep(2000);
                continue;
            }
            if (r < 20) continue;
            uint32_t dst; memcpy(&dst, buf+16, 4);
            send_self(buf, (size_t)r, dst);
        }
#elif defined(_WIN32)
        while (running.load(std::memory_order_relaxed)) {
            bool got_any = false;
            for (;;) {
                DWORD sz = 0;
                uint8_t* pkt = g_wintun.ReceivePacket(wintun_session, &sz);
                if (!pkt) break;
                got_any = true;
                if (sz >= 20) {
                    uint32_t dst; memcpy(&dst, pkt+16, 4);
                    send_self(pkt, sz, dst);
                }
                g_wintun.ReleaseReceivePacket(wintun_session, pkt);
            }
            if (!got_any) Sleep(2);
        }
#endif
    }

    // ── VPN DATA routing ──────────────────────────────────────────────────────
    void on_data(const uint8_t* buf, size_t len,
                 const sockaddr_in& from, int fd) {
        if (len < DHSZ) return;
        const auto* h = (const DataHdr*)buf;
        stat_rx.fetch_add(len, std::memory_order_relaxed);

        // Server-as-peer: traffic addressed to the server's own VPN IP is
        // delivered locally instead of relayed to another client.
        if (self_vpn_ip && h->dst_vpn == self_vpn_ip) {
            deliver_to_self(buf, len);
            stat_fwd.fetch_add(len, std::memory_order_relaxed);
            return;
        }

        // FIX1: collect destination(s) under lock, send OUTSIDE lock
        sockaddr_in              unicast_dst{};
        bool                     is_unicast = false;
        std::vector<sockaddr_in> bcast_dsts;

        {
            std::shared_lock<std::shared_mutex> lk(tbl_mtx);

            auto sit = by_vpn.find(h->src_vpn);
            if (sit != by_vpn.end()) {
                auto cit = by_node.find(sit->second);
                if (cit != by_node.end())
                    cit->second.last_seen.store(now_ms(),
                                                std::memory_order_relaxed);
            }

            if (h->dst_vpn == 0) {
                uint32_t skip = (sit != by_vpn.end()) ? sit->second : 0;
                collect_broadcast(skip, bcast_dsts);
            } else {
                auto dit = by_vpn.find(h->dst_vpn);
                if (dit != by_vpn.end())
                    is_unicast = collect_unicast(dit->second, unicast_dst);
            }
        } // lock released before any sendto

        if (is_unicast) {
            usend(fd, buf, len, unicast_dst);
            stat_fwd.fetch_add(len, std::memory_order_relaxed);
        } else {
            for (auto& dst : bcast_dsts) {
                usend(fd, buf, len, dst);
                stat_fwd.fetch_add(len, std::memory_order_relaxed);
            }
        }
    }

    // ── PUNCH / PUNCH_ACK forwarding ──────────────────────────────────────────
    void on_punch(const uint8_t* buf, size_t len,
                  const sockaddr_in& from, int fd) {
        if (len < sizeof(PunchPkt)) return;

        // FIX1+FIX2: O(1) lookup, collect addrs under lock, send outside
        std::vector<sockaddr_in> dsts;
        {
            std::shared_lock<std::shared_mutex> lk(tbl_mtx);
            uint32_t skip_node = 0;
            auto ait = by_addr.find(addr_key(from));
            if (ait != by_addr.end()) skip_node = ait->second;
            collect_broadcast(skip_node, dsts);
        }
        for (auto& dst : dsts)
            usend(fd, buf, len, dst);
    }

    void on_keepalive(const uint8_t* buf, size_t len) {
        if (len < sizeof(KaPkt)) return;
        const auto* k = (const KaPkt*)buf;
        // FIX1+6: shared_lock sufficient — last_seen is atomic
        std::shared_lock<std::shared_mutex> lk(tbl_mtx);
        auto it = by_node.find(k->node_id);
        if (it != by_node.end())
            it->second.last_seen.store(now_ms(), std::memory_order_relaxed);
    }

    // ── Per-packet dispatch ───────────────────────────────────────────────────
    void dispatch(const uint8_t* buf, size_t len,
                  const sockaddr_in& from, int fd) {
        if (!len) return;
        switch ((Msg)buf[0]) {
        case MSG_REGISTER:                  on_register(buf,len,from,fd); break;
        case MSG_DATA: case MSG_DATA_ENC:   on_data(buf,len,from,fd);     break;
        case MSG_PUNCH: case MSG_PUNCH_ACK: on_punch(buf,len,from,fd);    break;
        case MSG_KEEPALIVE:                 on_keepalive(buf,len);         break;
        default: break;
        }
    }

    // ── Worker thread: poll → recvmmsg(DONTWAIT) → dispatch ─────────────────
    // FIX3: Use poll() with 100ms timeout so workers check running flag
    // regularly and exit cleanly. No blocking recvmmsg, no dummy packets needed.
    void worker_loop(int fd, int cpu_hint) {
        pin_thread(cpu_hint);

        static thread_local uint8_t    bufs[BATCH][MAX_PKT];
        static thread_local sockaddr_in addrs[BATCH];
#ifdef __linux__
        static thread_local iovec   iovs[BATCH];
        static thread_local mmsghdr msgs[BATCH];
        memset(msgs, 0, sizeof(msgs));
        for (int i = 0; i < BATCH; i++) {
            iovs[i].iov_base            = bufs[i];
            iovs[i].iov_len             = MAX_PKT;
            msgs[i].msg_hdr.msg_iov     = &iovs[i];
            msgs[i].msg_hdr.msg_iovlen  = 1;
            msgs[i].msg_hdr.msg_name    = &addrs[i];
            msgs[i].msg_hdr.msg_namelen = sizeof(sockaddr_in);
        }
#endif

#ifdef _WIN32
        pollfd pfd{ (SOCKET)fd, POLLIN, 0 };
#else
        pollfd pfd{ fd, POLLIN, 0 };
#endif

        while (running.load(std::memory_order_relaxed)) {
            // Wait up to 100ms — allows timely response to running=false
            int ready = poll(&pfd, 1, 100);
            if (ready < 0) {
#ifndef _WIN32
                if (errno == EINTR) continue;
#endif
                break;
            }
            if (ready == 0) continue; // timeout, check running and loop

            // Drain all available packets in one batch (non-blocking)
#ifdef __linux__
            while (true) {
                for (int i = 0; i < BATCH; i++)
                    msgs[i].msg_hdr.msg_namelen = sizeof(sockaddr_in);

                int n = recvmmsg(fd, msgs, BATCH, MSG_DONTWAIT, nullptr);
                if (n <= 0) break; // EAGAIN = buffer empty

                for (int i = 0; i < n; i++)
                    dispatch(reinterpret_cast<uint8_t*>(bufs[i]),
                             msgs[i].msg_len, addrs[i], fd);
            }
#else
            // Portable fallback (Windows/macOS/etc): one recvfrom() per
            // packet instead of one recvmmsg() per batch — functionally
            // identical, just more syscalls under heavy load.
            for (int i = 0; i < BATCH; i++) {
                socklen_t alen = sizeof(addrs[0]);
#ifdef _WIN32
                int n = recvfrom(fd, (char*)bufs[0], (int)MAX_PKT, 0,
                                  (sockaddr*)&addrs[0], &alen);
#else
                ssize_t n = recvfrom(fd, bufs[0], MAX_PKT, MSG_DONTWAIT,
                                      (sockaddr*)&addrs[0], &alen);
#endif
                if (n <= 0) break;
                dispatch(bufs[0], (size_t)n, addrs[0], fd);
            }
#endif
        }
    }

    // ── Cleanup thread ────────────────────────────────────────────────────────
    void cleanup_loop() {
        while (running.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            uint64_t now = now_ms();
            std::vector<uint32_t> expired;

            {
                std::shared_lock<std::shared_mutex> lk(tbl_mtx);
                for (auto& [nid, c] : by_node)
                    // FIX4: last_seen is atomic, must use .load()
                    if (now - c.last_seen.load(std::memory_order_relaxed)
                            > CLIENT_TTL)
                        expired.push_back(nid);
            }

            if (!expired.empty()) {
                std::unique_lock<std::shared_mutex> lk(tbl_mtx);
                for (uint32_t nid : expired) {
                    auto it = by_node.find(nid);
                    if (it == by_node.end()) continue;
                    printf("[Server] - Expired vpn=%-16s  pub=%s\n",
                           ip4str(it->second.vpn_ip).c_str(),
                           addrstr(it->second.pub_addr).c_str());
                    by_vpn.erase(it->second.vpn_ip);
                    by_addr.erase(addr_key(it->second.pub_addr)); // FIX5
                    by_node.erase(it);
                }
            }
        }
    }

    void print_status() {
        std::shared_lock<std::shared_mutex> lk(tbl_mtx);
        uint64_t now = now_ms();
        printf("[Server] clients=%zu  rx=%llu B  fwd=%llu B\n",
               by_node.size(),
               (unsigned long long)stat_rx.load(),
               (unsigned long long)stat_fwd.load());
        for (auto& [nid, c] : by_node)
            printf("         vpn=%-16s  pub=%-24s  idle=%llus\n",
                   ip4str(c.vpn_ip).c_str(), addrstr(c.pub_addr).c_str(),
                   (unsigned long long)((now-c.last_seen.load())/1000));
    }

#ifdef WITH_XDP
    void run_xdp() {
        // Compile BPF program
        std::string obj = "/tmp/xdp_kern.o";
        if (access("xdp_kern.c", R_OK) == 0) {
            // linux/bpf.h transitively needs asm/types.h, which plain
            // /usr/include doesn't carry -- it lives under the
            // architecture-specific multiarch include dir (e.g.
            // /usr/include/x86_64-linux-gnu). Without it clang fails with
            // "asm/types.h file not found" on Debian/Ubuntu (matches
            // CMakeLists.txt's DEB_HOST_MULTIARCH detection for the
            // build-time xdp_kern.o target).
            std::string arch_inc = "x86_64-linux-gnu";
            FILE* fp = popen("dpkg-architecture -qDEB_HOST_MULTIARCH 2>/dev/null", "r");
            if (fp) {
                char buf[128] = {};
                if (fgets(buf, sizeof(buf), fp)) {
                    size_t n = strlen(buf);
                    while (n && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = '\0';
                    if (n) arch_inc = buf;
                }
                pclose(fp);
            }
            std::string cc = "clang -O2 -g -target bpf -I/usr/include -I/usr/include/" +
                arch_inc + " -c xdp_kern.c -o " + obj + " 2>&1";
            if (system(cc.c_str()) != 0) {
                fprintf(stderr,"[XDP] compile failed, fallback to recvmmsg\n");
                xdp_ifname=""; run(); return;
            }
        } else if (access(obj.c_str(), R_OK) != 0) {
            fprintf(stderr, "[XDP] xdp_kern.c not found in current directory and "
                    "%s doesn't already exist -- run meshvpn from the directory "
                    "containing xdp_kern.c, or pre-build it there.\n", obj.c_str());
            xdp_ifname=""; run(); return;
        }
        // Load XDP program + get xsk_map fd
        int xsk_map_fd = -1;
        try {
            xsk_map_fd = XdpLoader::load(xdp_ifname, obj, bind_port);
        } catch (const std::exception& e) {
            fprintf(stderr,"[XDP] %s — fallback to recvmmsg\n", e.what());
            xdp_ifname=""; run(); return;
        }

        auto dispatch_fn = [this](const uint8_t* b, size_t l,
                                  const sockaddr_in& f){ dispatch(b,l,f,-1); };

        std::vector<std::unique_ptr<XdpWorker>> workers;
        std::vector<std::thread> threads;
        for (int q = 0; q < n_workers; q++) {
            workers.push_back(std::make_unique<XdpWorker>(
                xdp_ifname, (uint32_t)q, xsk_map_fd,
                dispatch_fn, xdp_force_copy));
            threads.emplace_back([&w=*workers.back(),
                                   &r=this->running, q]{
                tls_xdp_worker = &w; // dispatch()'s usend(fd=-1, ...) sends via this
                w.run(r, q+1);
            });
        }
        std::thread cleaner([this]{ cleanup_loop(); });

        pollfd sfd={STDIN_FILENO, POLLIN, 0}; uint64_t t_s=0;
        while (!g_quit) {
            poll(&sfd,1,2000);
            if (sfd.revents & POLLIN) {
                char ln[32]={}; read(STDIN_FILENO,ln,sizeof(ln)-1);
                if (ln[0]=='s'||ln[0]=='S') print_status();
                if (ln[0]=='q'||ln[0]=='Q') g_quit=true;
            }
            if (now_ms()-t_s>=10000) { print_status(); t_s=now_ms(); }
        }
        running.store(false);
        for (auto& t:threads) t.join();
        cleaner.join();
        XdpLoader::detach(xdp_ifname);
        if (xsk_map_fd>=0) close(xsk_map_fd);
    }
#endif

    void run() {
        printf("\n[Server] Starting %d workers on :%u  [%s]\n\n",
               n_workers, bind_port,
               xdp_ifname.empty() ? "recvmmsg" : ("XDP:"+xdp_ifname).c_str());
        printf("Keys: s=status  q=quit\n\n");
#ifdef _WIN32
        start_stdin_thread();
#else
        fcntl(STDIN_FILENO, F_SETFL, fcntl(STDIN_FILENO,F_GETFL)|O_NONBLOCK);
#endif
#ifdef WITH_XDP
        if (!xdp_ifname.empty()) { run_xdp(); return; }
#endif
        // Open N sockets on same port with SO_REUSEPORT
        // Linux kernel load-balances incoming packets across them
        // FIX3: nonblocking — workers use poll(100ms) + recvmmsg(DONTWAIT)
        for (int i = 0; i < n_workers; i++) {
            worker_fds.push_back(open_udp(bind_port, /*nonblock=*/true));
            printf("[Server] Worker %d: fd=%d\n", i, worker_fds.back());
        }

        // Start workers
        std::vector<std::thread> threads;
        for (int i = 0; i < n_workers; i++)
            threads.emplace_back([this, i]{ worker_loop(worker_fds[i], i+1); });

        // Cleanup thread
        std::thread cleaner([this]{ cleanup_loop(); });

        // Main thread: stdin + status
        uint64_t t_status = 0;
#ifdef _WIN32
        while (!g_quit) {
            char cmd = g_stdin_cmd.exchange(0, std::memory_order_relaxed);
            if (cmd == 's' || cmd == 'S') print_status();
            if (cmd == 'q' || cmd == 'Q') g_quit = true;
            if (now_ms() - t_status >= 10000) { print_status(); t_status = now_ms(); }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
#else
        pollfd sfd = {STDIN_FILENO, POLLIN, 0};
        while (!g_quit) {
            poll(&sfd, 1, 2000);
            if (sfd.revents & POLLIN) {
                char line[32]={};
                if (read(STDIN_FILENO, line, sizeof(line)-1) > 0) {
                    switch (line[0]) {
                    case 's': case 'S': print_status(); break;
                    case 'q': case 'Q': g_quit = true; break;
                    default: break;
                    }
                }
            }
            if (now_ms() - t_status >= 10000) {
                print_status(); t_status = now_ms();
            }
        }
#endif

        running.store(false);
        // Workers use poll(100ms) so they will exit within 100ms of
        // running=false — no dummy packets needed.
        for (auto& t : threads) t.join();
        cleaner.join();
        for (int fd : worker_fds) CLOSESOCK(fd);
#ifdef __linux__
        // tun_fd is O_NONBLOCK, so self_tun_reader() only ever blocks in
        // its own 2ms usleep — it notices running=false and exits quickly,
        // no need to close the fd from here to unblock it.
        if (self_tun_thread.joinable()) self_tun_thread.join();
        if (tun_fd >= 0) close(tun_fd);
#elif defined(_WIN32)
        if (self_tun_thread.joinable()) self_tun_thread.join();
        if (wintun_session) g_wintun.EndSession(wintun_session);
        if (wintun_adapter) g_wintun.CloseAdapter(wintun_adapter);
#endif
        print_status();
    }
};

// ============================================================================
// CLIENT — TUN + VPN IP, relay or P2P comm mode
// ============================================================================
enum class CommMode { RELAY, P2P };
enum class P2PSt    { NONE, PUNCHING, DIRECT, FALLBACK };

struct Peer {
    sockaddr_in addr;
    uint32_t    vpn_ip;
    uint32_t    node_id;
    sockaddr_in lan_addr{};     // optional same-LAN candidate (hairpin-NAT fallback)
    bool        has_lan  = false;
    P2PSt       st        = P2PSt::NONE;
    uint64_t    t_pstart  = 0;  // when PUNCHING began
    uint64_t    t_punch   = 0;  // last punch sent
    uint64_t    t_p2prx   = 0;  // last data/ka received from peer
    uint64_t    t_ka      = 0;  // last keepalive sent to peer
    int         punch_n   = 0;  // total punch attempts in current episode
    bool        gave_up   = false; // true: no auto-retry, wait for 'f'

    bool direct() const { return st == P2PSt::DIRECT; }
};

struct Client {
    // Config
    uint32_t    my_vpn;
    uint32_t    my_node;
    uint16_t    local_port;
    CommMode    comm;
    bool        enc_on;
    sockaddr_in srv{};

    // FDs
    int tun_fd = -1;
    int udp_fd = -1;
    int ep_fd  = -1;
#ifdef _WIN32
    void* wintun_adapter = nullptr;
    void* wintun_session  = nullptr;
#endif

    // Peers (only touched by main loop — no lock needed)
    std::unordered_map<uint32_t, Peer>     peers;
    std::unordered_map<uint32_t, uint32_t> vpn_to_node;

    // Stats
    uint64_t tx = 0, rx = 0, nonce = 0;

    // Timers
    uint64_t t_reg     = 0;
    uint64_t t_srv_rx  = 0;
    uint64_t t_status  = 0;
    uint64_t t_netchk  = 0;

    // Punch timing constants
    static constexpr uint64_t T_REG      =   500; // REGISTER interval (until server ACKs)
    static constexpr uint64_t T_REG_KA   =  5000; // REGISTER heartbeat once connected (5s)
    //   ↑ MUST be < T_SRVDEAD. Server ACKs every REGISTER so t_srv_rx stays fresh.
    static constexpr uint64_t T_PUNCH    =  1000; // punch attempt interval
    static constexpr uint64_t T_PMAX     = 10000; // punch timeout → FALLBACK
    static constexpr uint64_t T_KA       =   500; // P2P keepalive interval
    static constexpr uint64_t T_DROP     =  3000; // P2P silence → FALLBACK
    static constexpr uint64_t T_BGRETRY  = 30000; // FALLBACK background retry interval
    static constexpr uint64_t T_SRVDEAD  = 15000; // server silence → DEAD warning
    static constexpr uint64_t T_STATUS   =  2000; // status line interval
    static constexpr uint64_t T_NETCHK   =  5000; // IP change poll interval

    // ── TUN setup ─────────────────────────────────────────────────────────────
    void setup_tun(const char* vpn_ip, int prefix, int mtu) {
#ifdef _WIN32
        if (!g_wintun.load())
            throw std::runtime_error(
                "wintun.dll not found (or missing expected exports). "
                "Download it from https://www.wintun.net/ and place "
                "wintun.dll next to ReVPN-engine.exe, then run as "
                "Administrator.");

        GUID guid{};
        CoCreateGuid(&guid);
        wintun_adapter = g_wintun.CreateAdapter(L"ReVPN0", L"ReVPN", &guid);
        if (!wintun_adapter)
            throw std::runtime_error(
                "WintunCreateAdapter failed (GetLastError=" +
                std::to_string(GetLastError()) + ") — need Administrator?");

        // 4 MiB ring — must be a power of two between 128 KiB and 64 MiB
        // per the Wintun API reference (git.zx2c4.com/wintun/about/#reference).
        wintun_session = g_wintun.StartSession(wintun_adapter, 0x400000);
        if (!wintun_session) {
            g_wintun.CloseAdapter(wintun_adapter); wintun_adapter = nullptr;
            throw std::runtime_error(
                "WintunStartSession failed (GetLastError=" +
                std::to_string(GetLastError()) + ")");
        }

        // Wintun creates the adapter; a normal `netsh` call assigns the
        // IP/MTU, same as how WireGuard-for-Windows does it under the hood
        // (mirrors ReVPN-py's tun.py WindowsTun._configure_ip()).
        uint32_t mask = prefix ? htonl(~((1u<<(32-prefix))-1)) : 0;
        in_addr m{}; m.s_addr = mask;
        char maskbuf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &m, maskbuf, sizeof(maskbuf));
        std::string ipcmd = std::string("netsh interface ip set address ") +
            "name=\"ReVPN0\" static " + vpn_ip + " " + maskbuf;
        system(ipcmd.c_str());
        std::string mtucmd = "netsh interface ipv4 set subinterface "
            "\"ReVPN0\" mtu=" + std::to_string(mtu) + " store=persistent";
        system(mtucmd.c_str());

        printf("[TUN] ReVPN0 (Wintun)  ip=%s/%d  mtu=%d\n", vpn_ip, prefix, mtu);
#elif !defined(__linux__)
        (void)vpn_ip; (void)prefix; (void)mtu;
        throw std::runtime_error(
            "client mode needs a TUN device, which this build does not "
            "support on this OS. Use ReVPN-py's client, or run this "
            "engine's client mode under WSL2/Linux instead.");
#else
        std::string ifname;
        tun_fd = open_linux_tun(vpn_ip, prefix, mtu, "tun0", ifname);
#endif
    }

    // ── epoll setup ───────────────────────────────────────────────────────────
    void setup_epoll() {
#ifdef __linux__
        ep_fd = epoll_create1(EPOLL_CLOEXEC);
        epoll_event ev{};
        ev.events   = EPOLLIN | EPOLLET; // edge-triggered
        ev.data.fd  = tun_fd;
        epoll_ctl(ep_fd, EPOLL_CTL_ADD, tun_fd, &ev);
        ev.data.fd  = udp_fd;
        epoll_ctl(ep_fd, EPOLL_CTL_ADD, udp_fd, &ev);
#endif
        // Non-Linux: unreachable — setup_tun() above already throws before
        // this would be called (see main()'s client path).
    }

    // ── VPN packet send ───────────────────────────────────────────────────────
    void send_vpn(const uint8_t* pkt, size_t len, uint32_t dst_vpn) {
        if (!len || len > 1400) return;

        // Routing: direct P2P or via server
        sockaddr_in dst = srv;
        if (comm == CommMode::P2P) {
            auto it = vpn_to_node.find(dst_vpn);
            if (it != vpn_to_node.end()) {
                auto pit = peers.find(it->second);
                if (pit != peers.end() && pit->second.direct())
                    dst = pit->second.addr;
            }
        }

        static uint8_t buf[1400 + DHSZ + 8];
        auto* h = (DataHdr*)buf;
        h->src_vpn = my_vpn; h->dst_vpn = dst_vpn;
        h->payload_len = (uint16_t)len;

        if (enc_on) {
            h->type = MSG_DATA_ENC;
            uint64_t nc = nonce++;
            xcrypt(pkt, buf+DHSZ, len, nc);
            memcpy(buf+DHSZ+len, &nc, 8);
            usend(udp_fd, buf, DHSZ+len+8, dst);
        } else {
            h->type = MSG_DATA;
            memcpy(buf+DHSZ, pkt, len);
            usend(udp_fd, buf, DHSZ+len, dst);
        }
        tx += len;
    }

    // Hands a decrypted IP packet to whichever TUN backend this OS has.
    void tun_write(const uint8_t* pkt, size_t n) {
#ifdef __linux__
        write(tun_fd, pkt, n);
#elif defined(_WIN32)
        uint8_t* p = g_wintun.AllocateSendPacket(wintun_session, (DWORD)n);
        if (!p) return; // ring full — drop, same as a blocked write() would
        memcpy(p, pkt, n);
        g_wintun.SendPacket(wintun_session, p);
#else
        (void)pkt; (void)n; // unreachable — no TUN backend on this build
#endif
    }

    void inject_tun(const uint8_t* buf, size_t len) {
#if !defined(__linux__) && !defined(_WIN32)
        (void)buf; (void)len; // unreachable — no TUN on this build (see setup_tun())
#else
        if (len < DHSZ+1) return;
        const auto* h = (const DataHdr*)buf;
        uint16_t plen = h->payload_len;
        if (h->type == MSG_DATA) {
            if (DHSZ+plen > len) return;
            tun_write(buf+DHSZ, plen); rx += plen;
        } else if (h->type == MSG_DATA_ENC) {
            if (DHSZ+plen+8 > len) return;
            uint64_t nc; memcpy(&nc, buf+DHSZ+plen, 8);
            static uint8_t plain[1400];
            if (plen > sizeof(plain)) return;
            xcrypt(buf+DHSZ, plain, plen, nc);
            tun_write(plain, plen); rx += plen;
        }
#endif
    }

    void do_register() {
        RegPkt p{}; p.type = MSG_REGISTER;
        p.node_id = my_node; p.vpn_ip = my_vpn;
        p.udp_port = htons(local_port);
        usend(udp_fd, &p, sizeof(p), srv);
        t_reg = now_ms();
    }

    // ── Packet handlers ───────────────────────────────────────────────────────

    void on_peer_info(const RegPkt* r) {
        if (r->node_id == my_node) return;

        // Check for duplicate / update
        bool is_new = (peers.find(r->node_id) == peers.end());
        Peer& p     = peers[r->node_id];

        p.vpn_ip         = r->vpn_ip;
        p.node_id        = r->node_id;
        p.addr.sin_family= AF_INET;
        p.addr.sin_port  = r->udp_port; // already BE
        memcpy(&p.addr.sin_addr, r->public_ip, 4);
        vpn_to_node[r->vpn_ip] = r->node_id;

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, r->public_ip, ip, sizeof(ip));
        printf("[Peer] vpn=%-16s  pub=%s:%u  node=0x%08X%s\n",
               ip4str(r->vpn_ip).c_str(), ip, ntohs(r->udp_port), r->node_id,
               is_new ? "" : " (updated)");

        // ── Punch decision ────────────────────────────────────────────────────
        // Start punching only if:
        //   1. Comm mode is P2P
        //   2. Peer is new OR was in FALLBACK with gave_up=false
        //      (gave_up=true means user must press 'f')
        if (comm != CommMode::P2P) return;

        if (is_new) {
            // Fresh peer — always start punching
            p.st       = P2PSt::PUNCHING;
            p.t_pstart = now_ms();
            p.punch_n  = 0;
            p.t_punch  = 0; // fire immediately on next tick
            p.gave_up  = false;
            printf("[P2P] Punching → vpn=%s  pub=%s\n",
                   ip4str(r->vpn_ip).c_str(), addrstr(p.addr).c_str());
        } else if (p.st == P2PSt::FALLBACK && !p.gave_up) {
            // Updated address while in fallback — restart punch
            p.st       = P2PSt::PUNCHING;
            p.t_pstart = now_ms();
            p.punch_n  = 0;
            p.t_punch  = 0;
            printf("[P2P] Peer addr updated, restarting punch → vpn=%s\n",
                   ip4str(r->vpn_ip).c_str());
        }
        // If DIRECT or PUNCHING already: leave state, address already updated
    }

    void on_punch(const sockaddr_in& from) {
        // ── Reply with ACK ────────────────────────────────────────────────────
        // IMPORTANT: use the actual NAT source address (from),
        //            NOT the address stored in peer table.
        // This is the key to making hole punch work with symmetric NAT:
        // the address we received the punch FROM is the address we send
        // data to, regardless of what the server told us.
        PunchPkt ack{}; ack.type = MSG_PUNCH_ACK; ack.node_id = my_node;
        usend(udp_fd, &ack, sizeof(ack), from);

        printf("[P2P] PUNCH from %s → ACK sent\n", addrstr(from).c_str());

        // Update peer's stored address to real NAT address if we know this peer
        for (auto& [nid, p] : peers) {
            if (p.addr.sin_addr.s_addr == from.sin_addr.s_addr) {
                if (p.addr.sin_port != from.sin_port) {
                    printf("[P2P] NAT port update: %u → %u for vpn=%s\n",
                           ntohs(p.addr.sin_port), ntohs(from.sin_port),
                           ip4str(p.vpn_ip).c_str());
                    p.addr = from;
                }
                break;
            }
        }
    }

    void on_punch_ack(const sockaddr_in& from, uint32_t sender_nid) {
        // ── P2P link established! ─────────────────────────────────────────────
        // FIX3: match by node_id OR full IP:port (not IP-only — CGNAT bug)
        uint64_t from_key = ((uint64_t)from.sin_addr.s_addr << 32)
                          | (uint64_t)from.sin_port;
        for (auto& [nid, p] : peers) {
            uint64_t peer_key = ((uint64_t)p.addr.sin_addr.s_addr << 32)
                              | (uint64_t)p.addr.sin_port;
            if (nid == sender_nid || peer_key == from_key)
            {
                bool was_direct = p.direct();
                p.addr    = from; // lock in actual NAT address
                p.st      = P2PSt::DIRECT;
                p.t_p2prx = now_ms();
                p.gave_up = false;

                if (!was_direct)
                    printf("[P2P] OK DIRECT  vpn=%-16s  %s  (punch#%d)\n",
                           ip4str(p.vpn_ip).c_str(),
                           addrstr(from).c_str(), p.punch_n);
                return;
            }
        }
        // ACK from unknown peer — log it
        printf("[P2P] PUNCH_ACK from unknown %s\n", addrstr(from).c_str());
    }

    void on_udp(const uint8_t* buf, size_t len, const sockaddr_in& from) {
        if (!len) return;

        bool from_srv = (from.sin_addr.s_addr == srv.sin_addr.s_addr &&
                         from.sin_port         == srv.sin_port);
        if (from_srv) t_srv_rx = now_ms();

        switch ((Msg)buf[0]) {
        case MSG_PEER_INFO:
            if (len >= sizeof(RegPkt)) on_peer_info((const RegPkt*)buf);
            break;

        case MSG_PUNCH:
            // Could be from server (relayed) or directly from peer
            on_punch(from);
            break;

        case MSG_PUNCH_ACK:
            if (len >= sizeof(PunchPkt))
                on_punch_ack(from, ((const PunchPkt*)buf)->node_id);
            break;

        case MSG_KEEPALIVE:
            // FIX2: server ACKs REGISTER with MSG_KEEPALIVE → keep t_srv_rx alive
            if (from_srv) t_srv_rx = now_ms();
            // FIX4: also update peer p2p recv time (full IP:port match, CGNAT safe)
            { uint64_t fk = ((uint64_t)from.sin_addr.s_addr<<32)|(uint64_t)from.sin_port;
              for (auto& [nid, p] : peers) {
                uint64_t pk = ((uint64_t)p.addr.sin_addr.s_addr<<32)|(uint64_t)p.addr.sin_port;
                if (pk == fk) { p.t_p2prx = now_ms(); break; }
              }
            }
            break;

        case MSG_DATA:
        case MSG_DATA_ENC:
            if (from_srv) t_srv_rx = now_ms(); // server alive when forwarding data
            inject_tun(buf, len);
            // FIX4: same full IP:port match
            { uint64_t fk = ((uint64_t)from.sin_addr.s_addr<<32)|(uint64_t)from.sin_port;
              for (auto& [nid, p] : peers) {
                uint64_t pk = ((uint64_t)p.addr.sin_addr.s_addr<<32)|(uint64_t)p.addr.sin_port;
                if (pk == fk) { p.t_p2prx = now_ms(); break; }
              }
            }
            break;

        default:
            break;
        }
    }

    // ── P2P state machine tick ────────────────────────────────────────────────
    void tick_p2p() {
        if (comm != CommMode::P2P) return;
        uint64_t now = now_ms();

        for (auto& [nid, p] : peers) {

            // ── PUNCHING ──────────────────────────────────────────────────────
            if (p.st == P2PSt::PUNCHING) {
                uint64_t since_last = now - p.t_punch;
                if (since_last >= T_PUNCH) {
                    // Send PUNCH to peer's known address
                    PunchPkt pk{}; pk.type = MSG_PUNCH; pk.node_id = my_node;
                    usend(udp_fd, &pk, sizeof(pk), p.addr);

                    // Same-LAN candidate (hairpin-NAT fallback) — exact
                    // port, no prediction needed, it's not NAT'd.
                    if (p.has_lan) usend(udp_fd, &pk, sizeof(pk), p.lan_addr);

                    // Symmetric NAT port prediction: try ±8 from known port
                    // Sequential NATs allocate ports +1 each connection.
                    // We send 16 extra probes — cheap since they're tiny UDP packets.
                    for (int delta : {1,-1,2,-2,3,-3,4,-4,5,-5,6,-6,7,-7,8,-8}) {
                        int port = (int)ntohs(p.addr.sin_port) + delta;
                        if (port < 1 || port > 65535) continue;
                        sockaddr_in adj = p.addr;
                        adj.sin_port = htons((uint16_t)port);
                        usend(udp_fd, &pk, sizeof(pk), adj);
                    }

                    p.punch_n++;
                    p.t_punch = now;

                    // Check timeout
                    uint64_t elapsed = now - p.t_pstart;
                    if (elapsed >= T_PMAX) {
                        printf("[P2P] WARN Punch timeout vpn=%-16s"
                               " (%d attempts, %llus)"
                               " → relay fallback. Press 'f' to retry.\n",
                               ip4str(p.vpn_ip).c_str(), p.punch_n,
                               (unsigned long long)(elapsed/1000));
                        p.st      = P2PSt::FALLBACK;
                        p.gave_up = true; // stop auto-retry
                    }
                }
            }

            // ── DIRECT ────────────────────────────────────────────────────────
            else if (p.st == P2PSt::DIRECT) {
                // Send keepalive to maintain NAT mapping
                if (now - p.t_ka >= T_KA) {
                    KaPkt ka{}; ka.type = MSG_KEEPALIVE; ka.node_id = my_node;
                    usend(udp_fd, &ka, sizeof(ka), p.addr);
                    p.t_ka = now;
                }
                // Silence timeout → fallback
                if (now - p.t_p2prx >= T_DROP) {
                    printf("[P2P] vpn=%-16s keepalive timeout → relay fallback\n",
                           ip4str(p.vpn_ip).c_str());
                    p.st      = P2PSt::FALLBACK;
                    p.gave_up = false; // allow background retry
                }
            }

            // ── FALLBACK ──────────────────────────────────────────────────────
            else if (p.st == P2PSt::FALLBACK && !p.gave_up) {
                // Background retry every T_BGRETRY ms
                if (now - p.t_pstart >= T_BGRETRY) {
                    printf("[P2P] Background retry vpn=%s\n",
                           ip4str(p.vpn_ip).c_str());
                    p.st       = P2PSt::PUNCHING;
                    p.t_pstart = now;
                    p.t_punch  = 0;
                    p.punch_n  = 0;
                }
            }
        }
    }

    void tick_timers() {
        uint64_t now = now_ms();

        // REGISTER: fast until server responds, then slow heartbeat
        uint64_t reg_interval = t_srv_rx ? T_REG_KA : T_REG;
        if (now - t_reg >= reg_interval) do_register();

        // P2P per-peer state machine
        tick_p2p();

        // Warn if server never responded
        static uint64_t t_warn = 0;
        if (!t_srv_rx && !t_warn && now - t_reg > 5000) {
            t_warn = now;
            printf("[Client] WARN No server response after 5s\n");
            printf("          Check: is server running at %s?\n",
                   addrstr(srv).c_str());
        }

        // Server dead notification
        if (t_srv_rx && (now-t_srv_rx) > T_SRVDEAD) {
            static uint64_t last_dead_warn = 0;
            if (now - last_dead_warn > 10000) {
                printf("[Client] WARN Server DEAD (%llus silent) — retrying\n",
                       (unsigned long long)((now-t_srv_rx)/1000));
                last_dead_warn = now;
                t_reg = 0; // force immediate re-register attempt
            }
        }

        // IP change detection (Linux only — getifaddrs; unreachable on other
        // OSes anyway since setup_tun() already throws before run() there)
#ifdef __linux__
        if (now - t_netchk >= T_NETCHK) {
            t_netchk = now;
            static uint32_t lip = 0;
            ifaddrs* ifa = nullptr;
            if (!getifaddrs(&ifa)) {
                for (auto* p = ifa; p; p = p->ifa_next) {
                    if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
                    if (!p->ifa_name) continue;
                    if (strncmp(p->ifa_name,"lo",2)==0) continue;
                    if (strncmp(p->ifa_name,"tun",3)==0) continue;
                    uint32_t cur=((sockaddr_in*)p->ifa_addr)->sin_addr.s_addr;
                    if (cur && lip && cur != lip) {
                        printf("[Net] IP changed — re-registering\n");
                        t_reg = 0; t_srv_rx = 0; // force immediate re-register
                        // Reset all P2P peers (addresses will be re-announced)
                        for (auto& [nid, peer] : peers) {
                            peer.st = P2PSt::NONE; peer.gave_up = false;
                        }
                    }
                    if (cur) lip = cur;
                    break;
                }
                freeifaddrs(ifa);
            }
        }
#endif

        // Status line
        if (now - t_status >= T_STATUS) {
            t_status = now;
            bool sv_ok = t_srv_rx && (now-t_srv_rx) < T_SRVDEAD;
            const char* sv_s = !t_srv_rx ? "WAIT" : sv_ok ? "OK" : "DEAD";
            int dc = 0, punch = 0, fallback = 0;
            for (auto& [nid, p] : peers) {
                if (p.direct()) dc++;
                else if (p.st == P2PSt::PUNCHING) punch++;
                else if (p.st == P2PSt::FALLBACK) fallback++;
            }
            printf("[Status] Server:%-5s  Peers:%zu"
                   "  Direct:%d  Punching:%d  Relay:%d  Enc:%s\n",
                   sv_s, peers.size(), dc, punch, fallback,
                   enc_on ? "ON" : "OFF");
        }
    }

    // ── User commands ─────────────────────────────────────────────────────────
    void force_punch_all() {
        if (peers.empty()) { printf("[P2P] No peers known yet\n"); return; }
        for (auto& [nid, p] : peers) {
            printf("[P2P] Force punch → vpn=%s\n", ip4str(p.vpn_ip).c_str());
            p.gave_up  = false;
            p.st       = P2PSt::PUNCHING;
            p.t_pstart = now_ms();
            p.t_punch  = 0;
            p.punch_n  = 0;
        }
        // Re-register so server sends fresh PEER_INFO (refreshed NAT addrs)
        do_register();
    }

    void print_stats() {
        uint64_t now = now_ms();
        bool sv_ok = t_srv_rx && (now-t_srv_rx) < T_SRVDEAD;
        printf("\n=== Client Stats ===\n");
        printf("  Server  : %s  (%s)\n", addrstr(srv).c_str(),
               !t_srv_rx?"CONNECTING":sv_ok?"OK":"DEAD");
        printf("  Comm    : %s\n", comm==CommMode::P2P?"P2P":"RELAY");
        printf("  Encrypt : %s\n", enc_on?"ON":"OFF");
        printf("  TX/RX   : %llu / %llu bytes\n",
               (unsigned long long)tx, (unsigned long long)rx);
        for (auto& [nid, p] : peers) {
            const char* st_s =
                p.st==P2PSt::DIRECT  ?"OK DIRECT"          :
                p.st==P2PSt::PUNCHING?"... PUNCHING"        :
                p.st==P2PSt::FALLBACK?
                    (p.gave_up?"WARN RELAY(gave-up,f=retry)"
                              :"WARN RELAY(bg-retry)")        : "—";
            printf("  Peer vpn=%-16s  pub=%-24s  %s  punch#=%d\n",
                   ip4str(p.vpn_ip).c_str(), addrstr(p.addr).c_str(),
                   st_s, p.punch_n);
        }
        printf("\n");
    }

    // ── Main loop ─────────────────────────────────────────────────────────────
    void run() {
#ifdef _WIN32
        printf("Comm mode : %s\n",
               comm==CommMode::P2P
               ? "P2P (direct when possible, relay fallback)"
               : "RELAY (always via server)");
        printf("Keys: s=stats  e=encrypt  f=force-punch  q=quit\n\n");
        start_stdin_thread();

        do_register();
        printf("[Client] REGISTER -> %s  (node=0x%08X)\n",
               addrstr(srv).c_str(), my_node);

        static uint8_t tbuf[65536];
        while (!g_quit) {
            // TUN -> VPN: drain everything currently queued in the ring.
            // (No epoll/IOCP wait on WintunGetReadWaitEvent here — this
            // busy-drains once per loop iteration instead, same 50ms
            // cadence as the poll() timeout below, so worst-case added
            // latency matches the Linux build's own epoll timer tick.)
            for (;;) {
                DWORD sz = 0;
                uint8_t* p = g_wintun.ReceivePacket(wintun_session, &sz);
                if (!p) break;
                if (sz >= 20) {
                    uint32_t dst; memcpy(&dst, p+16, 4);
                    send_vpn(p, sz, dst);
                }
                g_wintun.ReleaseReceivePacket(wintun_session, p);
            }

            // UDP -> dispatch
            pollfd pfd{}; pfd.fd = (SOCKET)udp_fd; pfd.events = POLLIN;
            if (poll(&pfd, 1, 50) > 0 && (pfd.revents & POLLIN)) {
                for (;;) {
                    sockaddr_in from{}; int fl = sizeof(from);
                    int r = recvfrom(udp_fd, (char*)tbuf, (int)sizeof(tbuf),
                                      0, (sockaddr*)&from, &fl);
                    if (r <= 0) break;
                    on_udp(tbuf, (size_t)r, from);
                }
            }

            char c = g_stdin_cmd.exchange(0, std::memory_order_relaxed);
            if (c) {
                switch (c) {
                case 's': case 'S': print_stats(); break;
                case 'e': case 'E':
                    enc_on = !enc_on;
                    printf("[CMD] Encryption %s\n", enc_on ? "ON" : "OFF");
                    break;
                case 'f': case 'F': force_punch_all(); break;
                case 'q': case 'Q': printf("[CMD] Quit\n"); g_quit = true; break;
                default: break;
                }
            }

            tick_timers();
        }

        print_stats();
        g_wintun.EndSession(wintun_session);
        g_wintun.CloseAdapter(wintun_adapter);
#elif !defined(__linux__)
        // Unreachable in practice: setup_tun() throws first on this OS
        // (see main()'s client path) before this is ever called. Guarded
        // here too so the method still compiles without needing
        // epoll/recvmmsg there.
        throw std::runtime_error("client run loop requires Linux (epoll) or Windows (Wintun)");
#else
        printf("Comm mode : %s\n",
               comm==CommMode::P2P
               ? "P2P (direct when possible, relay fallback)"
               : "RELAY (always via server)");
        printf("Keys: s=stats  e=encrypt  f=force-punch  q=quit\n\n");
        fcntl(STDIN_FILENO, F_SETFL,
              fcntl(STDIN_FILENO, F_GETFL)|O_NONBLOCK);

        // First REGISTER
        do_register();
        printf("[Client] REGISTER → %s  (node=0x%08X)\n",
               addrstr(srv).c_str(), my_node);

        // Add stdin to epoll for non-blocking reads
        epoll_event ev{};
        ev.events  = EPOLLIN;
        ev.data.fd = STDIN_FILENO;
        epoll_ctl(ep_fd, EPOLL_CTL_ADD, STDIN_FILENO, &ev);

        // Pre-alloc batch recv buffers
        static uint8_t    ubuf[BATCH][MAX_PKT];
        static iovec      uiov[BATCH];
        static sockaddr_in uaddr[BATCH];
        static mmsghdr    umsgs[BATCH];
        memset(umsgs, 0, sizeof(umsgs));
        for (int i = 0; i < BATCH; i++) {
            uiov[i].iov_base  = ubuf[i];
            uiov[i].iov_len   = MAX_PKT;
            umsgs[i].msg_hdr.msg_iov     = &uiov[i];
            umsgs[i].msg_hdr.msg_iovlen  = 1;
            umsgs[i].msg_hdr.msg_name    = &uaddr[i];
            umsgs[i].msg_hdr.msg_namelen = sizeof(sockaddr_in);
        }

        static uint8_t tbuf[65536];
        epoll_event events[16];

        while (!g_quit) {
            // epoll with 50ms timeout for timers
            int n = epoll_wait(ep_fd, events, 16, 50);
            if (n < 0) { if (errno==EINTR) continue; break; }

            for (int i = 0; i < n; i++) {
                int fd = events[i].data.fd;

                if (fd == tun_fd) {
                    // TUN → VPN: drain all pending packets (EPOLLET)
                    while (true) {
                        ssize_t r = read(tun_fd, tbuf, sizeof(tbuf));
                        if (r <= 0) break;
                        if (r < 20) continue; // need IP header
                        uint32_t dst; memcpy(&dst, tbuf+16, 4);
                        send_vpn(tbuf, (size_t)r, dst);
                    }

                } else if (fd == udp_fd) {
                    // UDP → dispatch: batch recv
                    while (true) {
                        // Reset namelen before each recvmmsg
                        for (int j = 0; j < BATCH; j++)
                            umsgs[j].msg_hdr.msg_namelen = sizeof(sockaddr_in);

                        int got = recvmmsg(udp_fd, umsgs, BATCH,
                                           MSG_DONTWAIT, nullptr);
                        if (got <= 0) break;
                        for (int j = 0; j < got; j++)
                            on_udp(ubuf[j], umsgs[j].msg_len, uaddr[j]);
                    }

                } else if (fd == STDIN_FILENO) {
                    char line[32]={};
                    if (read(STDIN_FILENO, line, sizeof(line)-1) > 0) {
                        switch (line[0]) {
                        case 's':case 'S': print_stats(); break;
                        case 'e':case 'E':
                            enc_on = !enc_on;
                            printf("[CMD] Encryption %s\n",enc_on?"ON":"OFF");
                            break;
                        case 'f':case 'F': force_punch_all(); break;
                        case 'q':case 'Q':
                            printf("[CMD] Quit\n"); g_quit=true; break;
                        default: break;
                        }
                    }
                }
            }

            tick_timers();
        }

        print_stats();
#endif
    }
};

// =============================================================================
// DECENTRALIZED MODE  (`--mode decentralized`)
// A real VPN peer — TUN device, actual mesh traffic — established with NO
// rendezvous/relay server at all. Reuses Client's TUN setup, wire format
// (send_vpn/inject_tun) and P2P punch/keepalive/retry state machine
// (tick_p2p/on_udp) unchanged; the only thing decentralized mode replaces
// is *how the peer's address is learned* — a public STUN server (Google's)
// plus a short copy-pasted token, instead of a server sending PEER_INFO.
// Single UDP port carries everything: STUN query, punch, keepalive, and
// the actual VPN data once direct — see the root README's Abstract.
//
// Token payload: "id,pub_ip,pub_port,vpn_ip,node_id[,lan_ip,lan_port]",
// base64-encoded — the pieces Client::on_peer_info() would normally learn
// from a server's PEER_INFO packet, learned here from the peer directly
// instead. The optional lan_ip/lan_port let two peers behind the SAME
// router (same pub_ip) connect without relying on NAT hairpinning, which
// many consumer routers don't support — without it, both sides punch
// forever (TX climbing, RX stuck at 0) because packets addressed to your
// own public IP from inside the same LAN never make the round trip back
// in. tick_p2p() punches both candidates; whichever answers first wins.
// =============================================================================
#define P2P_MAGIC_COOKIE 0x2112A442
#define P2P_STUN_SERVER_IP "74.125.250.129" // stun.l.google.com, one of several A records
#define P2P_STUN_SERVER_PORT 19302

// Best-effort local (LAN-facing) IPv4 address, for the hairpin-NAT fallback
// above. UDP connect() just picks a route/source address via the routing
// table — no packet is actually sent to 8.8.8.8.
std::string p2p_get_local_ip() {
    int s = (int)socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return "";
    sockaddr_in remote{};
    remote.sin_family = AF_INET;
    remote.sin_port   = htons(53);
    remote.sin_addr.s_addr = inet_addr("8.8.8.8");
    if (connect(s, (sockaddr*)&remote, sizeof(remote)) != 0) { CLOSESOCK(s); return ""; }
    sockaddr_in local{};
    socklen_t len = sizeof(local);
    if (getsockname(s, (sockaddr*)&local, &len) != 0) { CLOSESOCK(s); return ""; }
    CLOSESOCK(s);
    char buf[INET_ADDRSTRLEN];
    if (!inet_ntop(AF_INET, &local.sin_addr, buf, sizeof(buf))) return "";
    return std::string(buf);
}

bool p2p_parse_stun_response(const uint8_t* resp, size_t len, std::string& ip_out, uint16_t& port_out) {
    if (len < 20) return false;
    size_t pos = 20;
    while (pos + 4 <= len) {
        uint16_t type = ntohs(*(uint16_t*)(resp + pos));
        uint16_t length = ntohs(*(uint16_t*)(resp + pos + 2));
        if (pos + 4 + length > len) break;
        if (type == 0x0020 && length >= 8) { // XOR-MAPPED-ADDRESS
            const uint8_t* value = resp + pos + 4;
            if (value[1] != 0x01) return false; // IPv4 family
            uint16_t xport = ntohs(*(uint16_t*)(value + 2)) ^ (P2P_MAGIC_COOKIE >> 16);
            uint32_t xip = ntohl(*(uint32_t*)(value + 4)) ^ P2P_MAGIC_COOKIE;
            struct in_addr addr;
            addr.s_addr = htonl(xip);
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &addr, ip, sizeof(ip));
            ip_out = ip;
            port_out = xport;
            return true;
        }
        pos += 4 + length;
    }
    return false;
}

bool p2p_get_public_address(std::string& ip_out, uint16_t& port_out, int sock) {
    struct sockaddr_in stun_addr{};
    stun_addr.sin_family = AF_INET;
    stun_addr.sin_port = htons(P2P_STUN_SERVER_PORT);
    inet_pton(AF_INET, P2P_STUN_SERVER_IP, &stun_addr.sin_addr);

    uint8_t req[20] = {0};
    req[0] = 0x00; req[1] = 0x01; // Binding Request
    req[4] = (P2P_MAGIC_COOKIE >> 24) & 0xFF;
    req[5] = (P2P_MAGIC_COOKIE >> 16) & 0xFF;
    req[6] = (P2P_MAGIC_COOKIE >> 8) & 0xFF;
    req[7] = P2P_MAGIC_COOKIE & 0xFF;

    if (sendto(sock, (const char*)req, sizeof(req), 0, (sockaddr*)&stun_addr, sizeof(stun_addr)) < 0) {
        perror("sendto STUN");
        return false;
    }

    // `sock` may be non-blocking (open_udp() defaults to that), so wait for
    // the response with poll() instead of assuming a blocking recvfrom —
    // this also gives the STUN query a real timeout instead of hanging
    // forever if the server never answers.
    pollfd pfd{}; pfd.fd = (decltype(pfd.fd))sock; pfd.events = POLLIN;
    if (poll(&pfd, 1, 3000) <= 0 || !(pfd.revents & POLLIN)) {
        fprintf(stderr, "STUN request timed out\n");
        return false;
    }

    uint8_t resp[512];
    struct sockaddr_in from{};
    socklen_t from_len = sizeof(from);
    ssize_t n = recvfrom(sock, (char*)resp, sizeof(resp), 0, (sockaddr*)&from, &from_len);
    if (n <= 0) {
        perror("recvfrom STUN");
        return false;
    }

    return p2p_parse_stun_response(resp, n, ip_out, port_out);
}

// Token codec: base64("id,ip,port,vpn_ip,node_id") — a compact,
// copy-pasteable string a person hands their peer directly (chat, email,
// voice), instead of a rendezvous server sending PEER_INFO.
static const char* P2P_B64_CHARS =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string p2p_base64_encode(const std::string& in) {
    std::string out;
    int val = 0, bits = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c;
        bits += 8;
        while (bits >= 0) {
            out += P2P_B64_CHARS[(val >> bits) & 0x3F];
            bits -= 6;
        }
    }
    if (bits > -6) out += P2P_B64_CHARS[((val << 8) >> (bits + 8)) & 0x3F];
    while (out.size() % 4) out += '=';
    return out;
}

std::string p2p_base64_decode(const std::string& in) {
    std::vector<int> table(256, -1);
    for (int i = 0; i < 64; i++) table[(unsigned char)P2P_B64_CHARS[i]] = i;

    std::string out;
    int val = 0, bits = -8;
    for (unsigned char c : in) {
        if (table[c] == -1) continue; // skip '=', whitespace, stray chars
        val = (val << 6) + table[c];
        bits += 6;
        if (bits >= 0) {
            out += char((val >> bits) & 0xFF);
            bits -= 8;
        }
    }
    return out;
}

struct P2PPeerToken {
    std::string id;
    std::string ip;
    uint16_t    port;
    uint32_t    vpn_ip;   // network byte order, same as RegPkt::vpn_ip
    uint32_t    node_id;
    std::string lan_ip;   // optional — empty if peer couldn't determine one
    uint16_t    lan_port = 0;
};

std::string p2p_make_token(const std::string& id, const std::string& ip, uint16_t port,
                            uint32_t vpn_ip, uint32_t node_id,
                            const std::string& lan_ip, uint16_t lan_port) {
    std::string payload = id + "," + ip + "," + std::to_string(port) + "," +
        std::to_string(vpn_ip) + "," + std::to_string(node_id) + "," +
        lan_ip + "," + std::to_string(lan_port);
    return p2p_base64_encode(payload);
}

bool p2p_parse_token(const std::string& token, P2PPeerToken& out) {
    std::string payload = p2p_base64_decode(token);
    std::vector<std::string> f;
    size_t start = 0;
    for (size_t i = 0; i <= payload.size(); i++) {
        if (i == payload.size() || payload[i] == ',') {
            f.push_back(payload.substr(start, i - start));
            start = i + 1;
        }
    }
    if (f.size() != 5 && f.size() != 7) return false;
    try {
        out.id      = f[0];
        out.ip      = f[1];
        out.port    = static_cast<uint16_t>(std::stoi(f[2]));
        out.vpn_ip  = static_cast<uint32_t>(std::stoul(f[3]));
        out.node_id = static_cast<uint32_t>(std::stoul(f[4]));
        if (f.size() == 7 && !f[5].empty() && !f[6].empty()) {
            out.lan_ip   = f[5];
            out.lan_port = static_cast<uint16_t>(std::stoi(f[6]));
        }
    } catch (...) {
        return false;
    }
    return !out.ip.empty() && out.port != 0 && out.vpn_ip != 0;
}

// Entry point for `--mode decentralized`, called from main() below.
// Returns a process exit code.
int run_decentralized(const std::string& self_id, const std::string& vpn_ip_str, int subnet, int mtu,
                       uint16_t local_udp_port, bool enc, const std::vector<std::string>& peer_token_args) {
    Client cli;
    cli.my_vpn     = inet_addr(vpn_ip_str.c_str());
    cli.local_port = local_udp_port;
    cli.comm       = CommMode::P2P;
    cli.enc_on     = enc;

    uint32_t ip = cli.my_vpn;
    cli.my_node = ip; cli.my_node ^= cli.my_node >> 16;
    cli.my_node *= 0x45d9f3b; cli.my_node ^= cli.my_node >> 16;

    try {
        cli.udp_fd = open_udp(local_udp_port, /*nonblock=*/true);
    } catch (const std::exception& e) {
        fprintf(stderr, "[Decentralized] %s\n", e.what());
        return 1;
    }
    printf("[Decentralized] UDP socket bound to port %u\n", local_udp_port);

    std::string pub_ip; uint16_t pub_port;
    if (!p2p_get_public_address(pub_ip, pub_port, cli.udp_fd)) {
        fprintf(stderr, "[Decentralized] Failed to get public address via STUN\n");
        CLOSESOCK(cli.udp_fd); return 1;
    }
    printf("[Decentralized] Public address (via STUN): %s:%u\n", pub_ip.c_str(), pub_port);

    std::string lan_ip = p2p_get_local_ip();
    if (!lan_ip.empty())
        printf("[Decentralized] LAN address (fallback for same-router peers): %s:%u\n",
               lan_ip.c_str(), local_udp_port);

    std::string my_token = p2p_make_token(self_id, pub_ip, pub_port, cli.my_vpn, cli.my_node,
                                           lan_ip, local_udp_port);
    printf("\n============================================================\n"
           " Step 1 — send this token to EVERY peer you want to mesh with\n"
           " (chat, email, voice):\n"
           "============================================================\n\n"
           "  %s\n\n"
           "============================================================\n"
           " Step 2 — paste each peer's token below, one per line.\n"
           " Leave a line blank when you're done adding peers.\n"
           "============================================================\n\n", my_token.c_str());

    // Collect one or more peer tokens: pre-supplied via --peer-token
    // (repeatable, or comma-separated), or pasted interactively one per
    // line until a blank line. Every peer needs everyone else's token —
    // there's no server to introduce them, so mesh size is however many
    // tokens you hand-exchange.
    std::vector<std::string> raw_tokens;
    for (const auto& arg : peer_token_args) {
        size_t start = 0;
        while (start <= arg.size()) {
            size_t comma = arg.find(',', start);
            std::string tok = arg.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            if (!tok.empty()) raw_tokens.push_back(tok);
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    }
    if (raw_tokens.empty()) {
        for (;;) {
            printf("Peer's token (blank to finish): ");
            fflush(stdout);
            std::string line;
            if (!std::getline(std::cin, line) || line.empty()) break;
            raw_tokens.push_back(line);
        }
    }
    if (raw_tokens.empty()) {
        fprintf(stderr, "[Decentralized] No peer tokens given — nothing to connect to.\n");
        CLOSESOCK(cli.udp_fd); return 1;
    }

    std::vector<uint32_t> peer_node_ids;
    for (const auto& tok : raw_tokens) {
        P2PPeerToken peer;
        if (!p2p_parse_token(tok, peer)) {
            fprintf(stderr, "[Decentralized] That doesn't look like a valid token — "
                    "check it was copied in full, then try again: %s\n", tok.c_str());
            CLOSESOCK(cli.udp_fd); return 1;
        }
        printf("Got it — will punch through to %s now.\n", peer.id.c_str());

        // Seed the peer directly — this is the one thing that would
        // normally come from a server's PEER_INFO packet (see
        // Client::on_peer_info()).
        Peer p{};
        p.vpn_ip = peer.vpn_ip;
        p.node_id = peer.node_id;
        p.addr.sin_family = AF_INET;
        p.addr.sin_port = htons(peer.port);
        inet_pton(AF_INET, peer.ip.c_str(), &p.addr.sin_addr);
        if (!peer.lan_ip.empty() && peer.lan_port != 0) {
            p.lan_addr.sin_family = AF_INET;
            p.lan_addr.sin_port   = htons(peer.lan_port);
            if (inet_pton(AF_INET, peer.lan_ip.c_str(), &p.lan_addr.sin_addr) == 1)
                p.has_lan = true;
        }
        p.st = P2PSt::PUNCHING;
        p.t_pstart = now_ms();
        cli.peers[peer.node_id] = p;
        cli.vpn_to_node[peer.vpn_ip] = peer.node_id;
        peer_node_ids.push_back(peer.node_id);
    }
    printf("\n[Decentralized] %zu peer(s) configured.\n\n", peer_node_ids.size());

    try {
        cli.setup_tun(vpn_ip_str.c_str(), subnet, mtu);
    } catch (const std::exception& e) {
        fprintf(stderr, "[Decentralized] TUN setup failed: %s\n", e.what());
        CLOSESOCK(cli.udp_fd); return 1;
    }
#ifdef __linux__
    cli.setup_epoll();
#endif

    printf("[Decentralized] VPN IP  : %s/%d\n", vpn_ip_str.c_str(), subnet);
    printf("[Decentralized] Node ID : 0x%08X\n", cli.my_node);
    printf("[Decentralized] Encrypt : %s\n\n", enc ? "YES" : "NO");

    uint64_t t_status = 0;

    // No relay to lean on here (unlike normal client/server mode), so a
    // peer that gave up after the initial punch timeout should keep
    // auto-retrying rather than wait for a manual force-punch. Called once
    // per loop iteration by both the Linux and Windows loops below.
    auto retry_and_report = [&cli, &t_status, &peer_node_ids]() {
        for (auto& [nid, peer_it] : cli.peers) {
            if (peer_it.st == P2PSt::FALLBACK && peer_it.gave_up) {
                if (now_ms() - peer_it.t_pstart >= 10000) {
                    printf("[Decentralized] Retrying punch to 0x%08X...\n", nid);
                    peer_it.st = P2PSt::PUNCHING;
                    peer_it.gave_up = false;
                    peer_it.t_pstart = now_ms();
                    peer_it.t_punch = 0;
                    peer_it.punch_n = 0;
                }
            }
        }

        uint64_t now = now_ms();
        if (now - t_status >= 5000) {
            t_status = now;
            std::string line = "[Status] ";
            for (uint32_t nid : peer_node_ids) {
                auto it = cli.peers.find(nid);
                const char* st = "?";
                if (it != cli.peers.end()) {
                    st = it->second.direct() ? "DIRECT"
                       : it->second.st == P2PSt::PUNCHING ? "PUNCHING" : "RETRYING";
                }
                char buf[48];
                snprintf(buf, sizeof(buf), "0x%08X:%s  ", nid, st);
                line += buf;
            }
            line += "TX:" + std::to_string(cli.tx) + "  RX:" + std::to_string(cli.rx);
            printf("%s\n", line.c_str());
        }
    };

#ifdef _WIN32
    while (!g_quit) {
        // TUN -> VPN: drain everything currently queued in the Wintun ring.
        for (;;) {
            DWORD sz = 0;
            uint8_t* pkt = g_wintun.ReceivePacket(cli.wintun_session, &sz);
            if (!pkt) break;
            if (sz >= 20) {
                uint32_t dst; memcpy(&dst, pkt + 16, 4);
                cli.send_vpn(pkt, sz, dst);
            }
            g_wintun.ReleaseReceivePacket(cli.wintun_session, pkt);
        }

        // UDP -> dispatch
        static uint8_t ubuf[65536];
        pollfd pfd{}; pfd.fd = (SOCKET)cli.udp_fd; pfd.events = POLLIN;
        if (poll(&pfd, 1, 50) > 0 && (pfd.revents & POLLIN)) {
            for (;;) {
                sockaddr_in from{}; int fl = sizeof(from);
                int r = recvfrom(cli.udp_fd, (char*)ubuf, (int)sizeof(ubuf), 0, (sockaddr*)&from, &fl);
                if (r <= 0) break;
                cli.on_udp(ubuf, (size_t)r, from);
            }
        }

        cli.tick_p2p();
        retry_and_report();
    }
#elif !defined(__linux__)
    // Unreachable in practice: setup_tun() throws first on this OS (see
    // above) before this would be reached. Guarded here too so this
    // function still compiles without needing epoll/recvmmsg there.
    (void)retry_and_report;
    throw std::runtime_error("decentralized mode requires Linux (epoll) or Windows (Wintun)");
#else
    static uint8_t tbuf[65536];
    static uint8_t ubuf[65536];
    epoll_event events[16];

    while (!g_quit) {
        int n = epoll_wait(cli.ep_fd, events, 16, 50);
        if (n < 0) { if (errno == EINTR) continue; break; }

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            if (fd == cli.tun_fd) {
                while (true) {
                    ssize_t r = read(cli.tun_fd, tbuf, sizeof(tbuf));
                    if (r <= 0) break;
                    if (r < 20) continue; // need IP header
                    uint32_t dst; memcpy(&dst, tbuf + 16, 4);
                    cli.send_vpn(tbuf, (size_t)r, dst);
                }
            } else if (fd == cli.udp_fd) {
                while (true) {
                    sockaddr_in from{}; socklen_t fl = sizeof(from);
                    ssize_t r = recvfrom(cli.udp_fd, ubuf, sizeof(ubuf), MSG_DONTWAIT, (sockaddr*)&from, &fl);
                    if (r <= 0) break;
                    cli.on_udp(ubuf, (size_t)r, from);
                }
            }
        }

        cli.tick_p2p();
        retry_and_report();
    }
#endif

    printf("\n[Decentralized] Shutting down. TX:%llu RX:%llu\n",
           (unsigned long long)cli.tx, (unsigned long long)cli.rx);
    CLOSESOCK(cli.udp_fd);
    return 0;
}

// ── CLI ───────────────────────────────────────────────────────────────────────

static void usage(const char* p) {
    printf("Usage:\n");
    printf("  # Server (pure bridge — no TUN, no VPN IP):\n");
    printf("  sudo %s --mode server --bind 0.0.0.0:9000 [--workers 4]\n\n",p);
    printf("  # Client relay mode:\n");
    printf("  sudo %s --mode client --vpn-ip 10.13.0.2"
           " --server IP:9000 --comm relay\n\n",p);
    printf("  # Client P2P mode:\n");
    printf("  sudo %s --mode client --vpn-ip 10.13.0.3"
           " --server IP:9000 --comm p2p\n\n",p);
    printf("  # Decentralized — real VPN peer, no server at all, just tokens\n");
    printf("  # you exchange by hand (chat, voice, ...). Repeat --peer-token\n");
    printf("  # (or comma-separate) for a mesh of 3+ peers — everyone needs\n");
    printf("  # everyone else's token, since there's no server to introduce them:\n");
    printf("  sudo %s --mode decentralized --id alice --vpn-ip 10.13.0.2\n"
           "    [--port 51001] [--peer-token <token> [--peer-token <token> ...]]\n\n",p);
    printf("Options:\n");
    printf("  --bind     ip:port  Server bind addr        (default 0.0.0.0:9000)\n");
    printf("  --workers  n        Server worker threads   (default 4)\n");
    printf("  --vpn-ip   ip       (server) also join the mesh as a peer,\n");
    printf("                      reachable at this VPN IP — Linux or Windows (Wintun)\n");
    printf("  --xdp-iface if     AF_XDP NIC (e.g. eth0)   (requires -DWITH_XDP)\n");
    printf("  --xdp-copy         Force XDP copy-mode\n");
    printf("  --server   ip:port  Server address (client)\n");
    printf("  --comm     relay|p2p Client comm mode       (default relay)\n");
    printf("  --port     n        Client UDP port         (default 51820)\n");
    printf("  --subnet   n        VPN prefix length       (default 16)\n");
    printf("  --node-id  hex      32-bit node ID          (default auto)\n");
    printf("  --no-encrypt        Disable encryption\n");
    printf("  --mtu      n        TUN MTU                 (default 1380)\n");
    printf("  --id         name   (decentralized) your display name in the token\n");
    printf("  --vpn-ip     ip     (decentralized) this node's VPN IP, e.g. 10.13.0.2\n");
    printf("  --port       n      (decentralized) local UDP port          (default 51001)\n");
    printf("  --peer-token tok    (decentralized) a peer's token; repeat this flag\n");
    printf("                      (or comma-separate) for multiple peers — skips\n");
    printf("                      the interactive paste prompt\n");
    printf("  --config     file   Load settings from a flat YAML file (same format\n");
    printf("                      and keys as the ReVPN.sh wrapper's --config, minus\n");
    printf("                      the stress-test-only keys). Any flag also given on\n");
    printf("                      the command line still overrides the file.\n");
}

// ── --config <file> ──────────────────────────────────────────────────────────
// A flat "key: value" YAML loader — no nesting/lists, matches the same
// minimal format and key set ReVPN.sh's own load_yaml_config() parses
// (config/server.yaml, config/client.yaml), minus the stress-test-only
// keys (clients, duration, rate, size, stress_port), since those don't
// apply to this binary directly. Values loaded here become new defaults;
// they're applied BEFORE the real flag loop in main() runs, so any flag
// also given on the command line still wins — same precedence the
// wrapper script documents.
struct EngineConfig {
    std::string mode, vpn_ip, server_ip, comm_s, xdp_ifname, p2p_id;
    uint16_t bind_port, server_port, client_port;
    int subnet, mtu, n_workers;
    uint32_t node_id; bool enc, xdp_force_copy;
};

static std::string yaml_trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t");
    std::string t = s.substr(a, b - a + 1);
    if (t.size() >= 2 && ((t.front()=='"' && t.back()=='"') || (t.front()=='\'' && t.back()=='\'')))
        t = t.substr(1, t.size()-2);
    return t;
}

static void load_yaml_config(const std::string& path, EngineConfig& c) {
    std::ifstream f(path);
    if (!f) { fprintf(stderr, "config file not found: %s\n", path.c_str()); exit(1); }

    std::string line;
    while (std::getline(f, line)) {
        auto hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;

        std::string key = yaml_trim(line.substr(0, colon));
        std::string val = yaml_trim(line.substr(colon + 1));
        if (key.empty()) continue;

        if (key == "port") {
            if (val.empty()) continue;
            uint16_t p = (uint16_t)std::stoi(val);
            if (c.mode == "server") c.bind_port = p; else c.client_port = p;
        }
        else if (key == "workers")   { if (!val.empty()) c.n_workers = std::stoi(val); }
        else if (key == "xdp_iface") { c.xdp_ifname = val; }
        else if (key == "xdp_copy")  { c.xdp_force_copy = (val == "true"); }
        else if (key == "connect") {
            auto p = val.rfind(':');
            if (p != std::string::npos) {
                c.server_ip = val.substr(0, p);
                c.server_port = (uint16_t)std::stoi(val.substr(p + 1));
            }
        }
        else if (key == "vpn_ip")     { c.vpn_ip = val; }
        else if (key == "subnet")     { if (!val.empty()) c.subnet = std::stoi(val); }
        else if (key == "node_id")    { if (!val.empty()) c.node_id = (uint32_t)strtoul(val.c_str(), 0, 16); }
        else if (key == "mtu")        { if (!val.empty()) c.mtu = std::stoi(val); }
        else if (key == "encrypt")    { c.enc = (val != "false"); }
        else if (key == "relay_only") { c.comm_s = (val == "true") ? "relay" : "p2p"; }
        else if (key == "id")         { c.p2p_id = val; }
        // unknown keys (including stress-test-only ones) are ignored
    }
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }
#endif
    if (argc < 2) { usage(argv[0]); return 1; }

    std::string mode, vpn_ip, server_ip, comm_s="relay", xdp_ifname;
    uint16_t bind_port=9000, server_port=9000, client_port=51820;
    int subnet=16, mtu=1380, n_workers=4;
    uint32_t node_id=0; bool enc=true, xdp_force_copy=false;
    std::string p2p_id;
    std::vector<std::string> peer_tokens;

    // A config file's values become the new defaults before the real flag
    // loop below runs, so any flag also given on the command line still
    // overrides it — same precedence ReVPN.sh documents for its own
    // --config. --mode is pre-scanned too since the config loader needs it
    // to know whether a bare "port:" key means the server's bind port or
    // the client/decentralized local port.
    std::string cfg_path;
    for (int i=1; i<argc; i++) {
        std::string a=argv[i];
        if      (a=="--config" && i+1<argc) cfg_path=argv[i+1];
        else if (a=="--mode"   && i+1<argc) mode=argv[i+1];
    }
    if (!cfg_path.empty()) {
        EngineConfig c{mode, vpn_ip, server_ip, comm_s, xdp_ifname, p2p_id,
                       bind_port, server_port, client_port,
                       subnet, mtu, n_workers, node_id, enc, xdp_force_copy};
        load_yaml_config(cfg_path, c);
        vpn_ip=c.vpn_ip; server_ip=c.server_ip; comm_s=c.comm_s; xdp_ifname=c.xdp_ifname;
        p2p_id=c.p2p_id;
        bind_port=c.bind_port; server_port=c.server_port; client_port=c.client_port;
        subnet=c.subnet; mtu=c.mtu; n_workers=c.n_workers; node_id=c.node_id;
        enc=c.enc; xdp_force_copy=c.xdp_force_copy;
    }

    for (int i=1; i<argc; i++) {
        std::string a=argv[i];
        if      (a=="--config"  &&i+1<argc) ++i; // already applied above
        else if (a=="--mode"    &&i+1<argc) mode=argv[++i];
        else if (a=="--vpn-ip"  &&i+1<argc) vpn_ip=argv[++i];
        else if (a=="--bind"    &&i+1<argc) {
            std::string r=argv[++i]; auto c=r.rfind(':');
            bind_port=(c!=std::string::npos)?(uint16_t)std::stoi(r.substr(c+1)):9000;
        }
        else if (a=="--server"  &&i+1<argc) {
            std::string r=argv[++i]; auto c=r.rfind(':');
            if(c==std::string::npos){fprintf(stderr,"--server needs ip:port\n");return 1;}
            server_ip=r.substr(0,c); server_port=(uint16_t)std::stoi(r.substr(c+1));
        }
        else if (a=="--comm"    &&i+1<argc) comm_s=argv[++i];
        else if (a=="--port"    &&i+1<argc) client_port=(uint16_t)atoi(argv[++i]);
        else if (a=="--subnet"  &&i+1<argc) subnet=atoi(argv[++i]);
        else if (a=="--workers"   &&i+1<argc) n_workers=atoi(argv[++i]);
        else if (a=="--xdp-iface" &&i+1<argc) xdp_ifname=argv[++i];
        else if (a=="--xdp-copy")             xdp_force_copy=true;
        else if (a=="--node-id"   &&i+1<argc) node_id=(uint32_t)strtoul(argv[++i],0,16);
        else if (a=="--mtu"     &&i+1<argc) mtu=atoi(argv[++i]);
        else if (a=="--no-encrypt") enc=false;
        else if (a=="--id"          &&i+1<argc) p2p_id=argv[++i];
        else if (a=="--peer-token"  &&i+1<argc) peer_tokens.push_back(argv[++i]);
        else if (a=="--help"||a=="-h") { usage(argv[0]); return 0; }
        else { fprintf(stderr,"Unknown: %s\n",a.c_str()); return 1; }
    }

    if (mode == "decentralized") {
        if (p2p_id.empty())  { fprintf(stderr,"decentralized mode needs --id\n\n"); usage(argv[0]); return 1; }
        if (vpn_ip.empty())  { fprintf(stderr,"decentralized mode needs --vpn-ip\n\n"); usage(argv[0]); return 1; }
        if (client_port == 51820) client_port = 51001; // pick a default distinct from client mode's own default
        signal(SIGINT,onsig); signal(SIGTERM,onsig);
#ifndef _WIN32
        signal(SIGPIPE,SIG_IGN);
#endif
        return run_decentralized(p2p_id, vpn_ip, subnet, mtu, client_port, enc, peer_tokens);
    }

    if (mode!="server"&&mode!="client") {
        fprintf(stderr,"--mode must be server, client, or decentralized\n\n");
        usage(argv[0]); return 1;
    }
    if (mode=="client"&&(vpn_ip.empty()||server_ip.empty())) {
        fprintf(stderr,"client needs --vpn-ip and --server\n\n");
        usage(argv[0]); return 1;
    }

    if (!node_id) {
        if (!vpn_ip.empty()) {
            uint32_t ip=inet_addr(vpn_ip.c_str());
            node_id=ip; node_id^=node_id>>16;
            node_id*=0x45d9f3b; node_id^=node_id>>16;
        } else {
            node_id=(uint32_t)now_ms();
        }
    }

    printf("╔══════════════════════════════════════════╗\n");
    printf("║       MeshVPN  (hub-and-spoke)          ║\n");
    printf("╚══════════════════════════════════════════╝\n\n");

    signal(SIGINT,onsig); signal(SIGTERM,onsig);
#ifndef _WIN32
    signal(SIGPIPE,SIG_IGN);
#endif

    try {
        if (mode == "server") {
            printf("  Mode    : SERVER (bridge%s)\n",
                   vpn_ip.empty() ? ", no TUN" : " + peer");
            printf("  Bind    : 0.0.0.0:%u\n", bind_port);
            printf("  Workers : %d\n", n_workers);
            if (!vpn_ip.empty())
                printf("  VPN IP  : %s/%d  (server reachable on the mesh)\n",
                       vpn_ip.c_str(), subnet);
            printf("  Node ID : 0x%08X\n\n", node_id);

            Server srv;
            srv.bind_port      = bind_port;
            srv.n_workers      = n_workers;
            srv.xdp_ifname     = xdp_ifname;
            srv.xdp_force_copy = xdp_force_copy;
            srv.self_encrypt   = enc;
            if (!vpn_ip.empty()) srv.setup_self_tun(vpn_ip.c_str(), subnet, mtu);
            srv.run();

        } else {
            CommMode cm = (comm_s=="p2p") ? CommMode::P2P : CommMode::RELAY;
            printf("  Mode    : CLIENT\n");
            printf("  VPN IP  : %s/%d\n", vpn_ip.c_str(), subnet);
            printf("  Server  : %s:%u\n", server_ip.c_str(), server_port);
            printf("  Comm    : %s\n", cm==CommMode::P2P
                   ?"P2P":"RELAY");
            printf("  Port    : %u\n",   client_port);
            printf("  Node ID : 0x%08X\n", node_id);
            printf("  Encrypt : %s\n\n", enc?"YES":"NO");

            Client cli;
            cli.my_vpn     = inet_addr(vpn_ip.c_str());
            cli.my_node    = node_id;
            cli.local_port = client_port;
            cli.comm       = cm;
            cli.enc_on     = enc;
            cli.srv.sin_family      = AF_INET;
            cli.srv.sin_port        = htons(server_port);
            cli.srv.sin_addr.s_addr = inet_addr(server_ip.c_str());
            if (cli.srv.sin_addr.s_addr == INADDR_NONE)
                throw std::runtime_error("Invalid server IP: " + server_ip);

            cli.udp_fd = open_udp(client_port, true);
            cli.setup_tun(vpn_ip.c_str(), subnet, mtu);
            cli.setup_epoll();
            cli.run();
        }
    } catch (const std::exception& e) {
        fprintf(stderr,"[FATAL] %s\n",e.what()); return 1;
    }

    printf("[MeshVPN] Done\n");
    return 0;
}