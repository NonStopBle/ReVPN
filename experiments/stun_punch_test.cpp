// ============================================================================
// stun_punch_test.cpp — concept test: server-less peer discovery + UDP hole
// punching using a public STUN server (Google's) instead of a ReVPN
// rendezvous server.
//
// Idea: each side asks a STUN server "what does the internet see me as?"
// (public ip:port through its own NAT), packs that into a short base64
// "token" string, and the two people exchange tokens directly — paste into
// a chat, read over the phone, whatever. Once each side has pasted the
// other's token in, it hole-punches straight to the peer's address and the
// two talk directly — no rendezvous/relay server anywhere in the path, not
// even a shared file. This is the concept behind the "decentralized, no
// server, just a token" item in the project README's Abstract.
//
// Each side actually opens and punches TWO independent paths — a main one
// and a redundant one, both STUN-mapped and both in the token — a bit like
// having two separate phone lines to the same person. A background
// watchdog keeps both alive with keepalive punches and, if the main path
// goes quiet (say, its NAT mapping expired), fails the live tunnel over to
// the redundant path automatically, no user action needed. Only if BOTH
// paths go quiet does it give up and ask for a fresh token exchange —
// at that point either side's reachable address may genuinely have
// changed, and there's no way to route around that without new
// information from the peer.
//
// This is intentionally NOT wired into the ReVPN engine — it is a minimal,
// standalone proof of concept to test that idea in isolation before it
// becomes a real feature.
//
// No JSON library: the token payload is one comma-separated "id,ip,port"
// line, base64-encoded — see make_token()/parse_token() below.
//
// Build:   g++ -std=c++17 -O2 -pthread stun_punch_test.cpp -o stun_punch_test
// Usage:   ./stun_punch_test <local_udp_port> <your_id> <local_tcp_port> [peer_token]
//
// Try it (two terminals, or two machines):
//   Side A: ./stun_punch_test 51001 alice 6001
//     -> prints "Your token: <...>", then waits for you to paste Bob's.
//   Side B: ./stun_punch_test 51002 bob 6002
//     -> prints its own token; paste it into Side A's prompt, and paste
//        Side A's token into Side B's prompt.
//   Both sides then hole-punch to each other. `nc 127.0.0.1 6001` on
//   Alice's side tunnels through the punched UDP path to Bob's local TCP
//   port (6002), with no server involved in reaching him.
//
//   For scripted/non-interactive runs, pass the peer's token as the 4th
//   argument instead of pasting it interactively.
// ============================================================================
#include <iostream>
#include <fstream>
#include <sstream>
#include <thread>
#include <chrono>
#include <vector>
#include <string>
#include <cstring>
#include <mutex>
#include <map>
#include <queue>
#include <atomic>
#include <condition_variable>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#define MAGIC_COOKIE 0x2112A442
#define STUN_SERVER_IP "74.125.250.129" // stun.l.google.com, one of several A records
#define STUN_SERVER_PORT 19302

// How long a path (main or redundant) can go without hearing anything from
// the peer — punch keepalive or real data — before it's considered dead.
// Keepalive fires every 5s, so this tolerates ~2 missed beats.
static const int64_t PATH_TIMEOUT_MS = 12000;

struct ConnInfo {
    std::string id;
    std::string ip;
    uint16_t port;      // main path
    uint16_t alt_port;  // redundant path — same ip, second STUN-mapped socket
};

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct UDPPacket {
    std::vector<uint8_t> data;
    sockaddr_in from_addr;
    std::chrono::steady_clock::time_point timestamp;
};

std::mutex cout_mtx;
void ts_cout(const std::string& s) {
    std::lock_guard<std::mutex> lock(cout_mtx);
    auto now = std::chrono::steady_clock::now();
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    std::cout << "[" << millis << "] " << s << std::endl;
}

// ============================================================================
// Packet dispatcher — demuxes the one UDP socket's inbound packets by
// source address, so each peer's tunnel thread can block waiting on just
// its own traffic.
// ============================================================================
class PacketDispatcher {
private:
    // One queue+mutex+cv per peer, so the queue that dispatch_packet()
    // pushes onto is the exact same object get_packet_from_peer() locks
    // and waits on — no cross-mutex race between producer and consumer.
    struct PeerQueue {
        std::mutex mtx;
        std::condition_variable cv;
        std::queue<UDPPacket> queue;
        size_t packet_count = 0;
    };

    std::map<std::string, std::unique_ptr<PeerQueue>> peers;
    std::mutex map_mtx; // guards structural changes to `peers` only

    std::string addr_to_string(const sockaddr_in& addr) {
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
        return std::string(ip) + ":" + std::to_string(ntohs(addr.sin_port));
    }

    PeerQueue& get_or_create(const std::string& peer_key) {
        std::lock_guard<std::mutex> lock(map_mtx);
        auto it = peers.find(peer_key);
        if (it == peers.end()) {
            it = peers.emplace(peer_key, std::make_unique<PeerQueue>()).first;
        }
        return *it->second;
    }

public:
    void dispatch_packet(const UDPPacket& packet) {
        std::string peer_key = addr_to_string(packet.from_addr);
        PeerQueue& pq = get_or_create(peer_key);

        size_t queue_size;
        {
            std::lock_guard<std::mutex> lock(pq.mtx);
            pq.queue.push(packet);
            pq.packet_count++;
            queue_size = pq.queue.size();
        }

        ts_cout("[DISPATCH] Packet #" + std::to_string(pq.packet_count) +
               " from " + peer_key + " (size: " + std::to_string(packet.data.size()) +
               ", queue size: " + std::to_string(queue_size) + ")");

        pq.cv.notify_one();
    }

    bool get_packet_from_peer(const std::string& peer_key, UDPPacket& packet, int timeout_ms = 1000) {
        PeerQueue& pq = get_or_create(peer_key);
        std::unique_lock<std::mutex> lock(pq.mtx);

        if (pq.cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                            [&pq] { return !pq.queue.empty(); })) {
            packet = pq.queue.front();
            pq.queue.pop();
            ts_cout("[RECV] Got packet from " + peer_key + " (size: " + std::to_string(packet.data.size()) + ")");
            return true;
        }
        return false;
    }
};

PacketDispatcher g_dispatcher;

// ============================================================================
// Redundancy: a second, independently STUN-mapped UDP socket/port alongside
// the main one. Both get punched at setup and exchanged in the token, and
// a watchdog below fails the tunnel over to the redundant path if the main
// one goes quiet — same idea as ReVPN's own P2P/relay fallback (T_DROP),
// just for a second direct path instead of a relay. If the redundant path
// dies too, there's no third fallback left: the peer has to send a fresh
// token and both sides restart, since the addresses either side is
// reachable at may genuinely have changed (new NAT mapping, new network).
// ============================================================================
struct PathState {
    int sock = -1;
    sockaddr_in peer_addr{};
    std::string peer_key;
    std::atomic<int64_t> last_seen_ms{0};
};

PathState g_main_path;
PathState g_alt_path;
std::atomic<bool> g_use_alt{false};
std::atomic<bool> g_link_lost{false};
std::string g_self_id;

// ============================================================================
// STUN — minimal RFC 5389 Binding Request/Response, just enough to read
// XOR-MAPPED-ADDRESS back out.
// ============================================================================
bool parse_stun_response(const uint8_t* resp, size_t len, std::string& ip_out, uint16_t& port_out) {
    if (len < 20) return false;
    size_t pos = 20;
    while (pos + 4 <= len) {
        uint16_t type = ntohs(*(uint16_t*)(resp + pos));
        uint16_t length = ntohs(*(uint16_t*)(resp + pos + 2));
        if (pos + 4 + length > len) break;
        if (type == 0x0020 && length >= 8) { // XOR-MAPPED-ADDRESS
            const uint8_t* value = resp + pos + 4;
            if (value[1] != 0x01) return false; // IPv4 family
            uint16_t xport = ntohs(*(uint16_t*)(value + 2)) ^ (MAGIC_COOKIE >> 16);
            uint32_t xip = ntohl(*(uint32_t*)(value + 4)) ^ MAGIC_COOKIE;
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

bool get_public_address(std::string& ip_out, uint16_t& port_out, int sock) {
    struct sockaddr_in stun_addr{};
    stun_addr.sin_family = AF_INET;
    stun_addr.sin_port = htons(STUN_SERVER_PORT);
    inet_pton(AF_INET, STUN_SERVER_IP, &stun_addr.sin_addr);

    uint8_t req[20] = {0};
    req[0] = 0x00; req[1] = 0x01; // Binding Request
    req[4] = (MAGIC_COOKIE >> 24) & 0xFF;
    req[5] = (MAGIC_COOKIE >> 16) & 0xFF;
    req[6] = (MAGIC_COOKIE >> 8) & 0xFF;
    req[7] = MAGIC_COOKIE & 0xFF;

    if (sendto(sock, req, sizeof(req), 0, (sockaddr*)&stun_addr, sizeof(stun_addr)) < 0) {
        perror("sendto STUN");
        return false;
    }

    uint8_t resp[512];
    struct sockaddr_in from{};
    socklen_t from_len = sizeof(from);
    ssize_t n = recvfrom(sock, resp, sizeof(resp), 0, (sockaddr*)&from, &from_len);
    if (n <= 0) {
        perror("recvfrom STUN");
        return false;
    }

    return parse_stun_response(resp, n, ip_out, port_out);
}

// ============================================================================
// Token codec: base64("id,ip,port") — a compact, copy-pasteable string a
// person hands to their peer directly (chat, email, voice), instead of a
// file. No JSON, no library: the payload is one comma-separated line.
// ============================================================================
static const char* B64_CHARS =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const std::string& in) {
    std::string out;
    int val = 0, bits = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c;
        bits += 8;
        while (bits >= 0) {
            out += B64_CHARS[(val >> bits) & 0x3F];
            bits -= 6;
        }
    }
    if (bits > -6) out += B64_CHARS[((val << 8) >> (bits + 8)) & 0x3F];
    while (out.size() % 4) out += '=';
    return out;
}

std::string base64_decode(const std::string& in) {
    std::vector<int> table(256, -1);
    for (int i = 0; i < 64; i++) table[(unsigned char)B64_CHARS[i]] = i;

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

std::string make_token(const ConnInfo& info) {
    std::string payload = info.id + "," + info.ip + "," +
        std::to_string(info.port) + "," + std::to_string(info.alt_port);
    return base64_encode(payload);
}

bool parse_token(const std::string& token, ConnInfo& out) {
    std::string payload = base64_decode(token);
    size_t c1 = payload.find(',');
    if (c1 == std::string::npos) return false;
    size_t c2 = payload.find(',', c1 + 1);
    if (c2 == std::string::npos) return false;
    size_t c3 = payload.find(',', c2 + 1);
    if (c3 == std::string::npos) return false;

    out.id = payload.substr(0, c1);
    out.ip = payload.substr(c1 + 1, c2 - c1 - 1);
    try {
        out.port = static_cast<uint16_t>(std::stoi(payload.substr(c2 + 1, c3 - c2 - 1)));
        out.alt_port = static_cast<uint16_t>(std::stoi(payload.substr(c3 + 1)));
    } catch (...) {
        return false;
    }
    return !out.ip.empty() && out.port != 0 && out.alt_port != 0;
}

// ============================================================================
// UDP send/recv framing: 4-byte big-endian length prefix + payload.
// ============================================================================
bool udp_send_packet(int udp_sock, const sockaddr_in& peer_addr, const std::vector<uint8_t>& data) {
    uint32_t len = htonl((uint32_t)data.size());
    std::vector<uint8_t> packet(4 + data.size());
    memcpy(packet.data(), &len, 4);
    memcpy(packet.data() + 4, data.data(), data.size());

    ssize_t sent = sendto(udp_sock, packet.data(), packet.size(), 0, (sockaddr*)&peer_addr, sizeof(peer_addr));
    if (sent != (ssize_t)packet.size()) {
        ts_cout(std::string("[UDP_SEND] FAILED: ") + strerror(errno));
        return false;
    }
    ts_cout("[UDP_SEND] sent " + std::to_string(sent) + " bytes ok");
    return true;
}

bool udp_recv_packet_from_peer(const std::string& peer_key, std::vector<uint8_t>& data_out, int timeout_ms = 1000) {
    UDPPacket packet;
    if (!g_dispatcher.get_packet_from_peer(peer_key, packet, timeout_ms)) return false;
    if (packet.data.size() < 4) return false;

    uint32_t len;
    memcpy(&len, packet.data.data(), 4);
    len = ntohl(len);
    if (len > (uint32_t)(packet.data.size() - 4)) return false;

    data_out.assign(packet.data.begin() + 4, packet.data.begin() + 4 + len);
    return true;
}

// ============================================================================
// TCP<->UDP tunnel: a local `nc`/app talks plain TCP to us; we frame and
// forward it over the punched UDP path to the peer, and back.
// ============================================================================
// Both tunnel directions re-check g_use_alt on every iteration, so a
// failover mid-transfer takes effect on the very next packet without
// tearing down the local TCP connection the app is using.
void tcp_to_udp_tunnel(int tcp_fd) {
    uint8_t buffer[1400];
    while (true) {
        ssize_t n = recv(tcp_fd, buffer, sizeof(buffer), 0);
        if (n <= 0) break;
        std::vector<uint8_t> data(buffer, buffer + n);
        PathState& p = g_use_alt.load() ? g_alt_path : g_main_path;
        if (!udp_send_packet(p.sock, p.peer_addr, data)) break;
    }
    ts_cout("[TCP2UDP] Tunnel ended");
    close(tcp_fd);
}

void udp_to_tcp_tunnel(int tcp_fd) {
    while (true) {
        PathState& p = g_use_alt.load() ? g_alt_path : g_main_path;
        std::vector<uint8_t> data;
        if (!udp_recv_packet_from_peer(p.peer_key, data, 1000)) continue;
        ssize_t sent = send(tcp_fd, data.data(), data.size(), MSG_NOSIGNAL);
        if (sent <= 0) break;
    }
    ts_cout("[UDP2TCP] Tunnel ended");
    close(tcp_fd);
}

void udp_hole_punching(int udp_sock, sockaddr_in peer_addr, const std::string& id) {
    std::string punch_msg = "punch_from_" + id;
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &peer_addr.sin_addr, ip, sizeof(ip));
    ts_cout("[PUNCH] Starting hole punching to " + std::string(ip) + ":" + std::to_string(ntohs(peer_addr.sin_port)));

    for (int i = 0; i < 10; i++) {
        sendto(udp_sock, punch_msg.c_str(), punch_msg.size(), 0, (sockaddr*)&peer_addr, sizeof(peer_addr));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    ts_cout("[PUNCH] Hole punching completed");
}

// A NAT's UDP mapping (and any hairpin/loopback mapping when both peers
// share one NAT, as in local testing) is not permanent — routers expire it
// after a period of silence, often well under a minute. The initial burst
// above only opens it; something has to keep sending afterward or the hole
// quietly closes before real data ever needs to cross it. Runs for the
// life of the tunnel, detached by the caller. Keeps BOTH paths alive
// (main and redundant) regardless of which one is currently active, so
// the idle one is still a real fallback and not a stale mapping by the
// time it's needed.
void udp_keepalive_loop() {
    std::string punch_msg = "punch_from_" + g_self_id;
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        sendto(g_main_path.sock, punch_msg.c_str(), punch_msg.size(), 0,
               (sockaddr*)&g_main_path.peer_addr, sizeof(g_main_path.peer_addr));
        sendto(g_alt_path.sock, punch_msg.c_str(), punch_msg.size(), 0,
               (sockaddr*)&g_alt_path.peer_addr, sizeof(g_alt_path.peer_addr));
    }
}

// Watches both paths' last-seen times (updated by udp_recv_loop below on
// every punch or data packet). If the active path goes quiet, fails over
// to the redundant one and re-punches it immediately rather than waiting
// for its own 5s keepalive tick. If the redundant path *also* goes quiet
// after that, there's nothing left to fail over to — both addresses may
// genuinely be stale (NAT remapped, network changed) — so it just reports
// the link as lost and says what to do: exchange fresh tokens and restart.
void watchdog_loop() {
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        if (g_link_lost.load()) continue;

        int64_t now = now_ms();
        if (!g_use_alt.load()) {
            if (now - g_main_path.last_seen_ms.load() > PATH_TIMEOUT_MS) {
                ts_cout("[FAILOVER] Main path silent for over " +
                        std::to_string(PATH_TIMEOUT_MS / 1000) +
                        "s — switching to the redundant port and re-punching.");
                g_use_alt = true;
                g_alt_path.last_seen_ms = now_ms(); // grace window for the re-punch below
                std::thread(udp_hole_punching, g_alt_path.sock, g_alt_path.peer_addr, g_self_id).detach();
            }
        } else {
            if (now - g_alt_path.last_seen_ms.load() > PATH_TIMEOUT_MS) {
                g_link_lost = true;
                ts_cout("[LOST] Redundant path is silent too — both direct paths to "
                        "the peer are dead.");
                ts_cout("[LOST] This can happen when either side's NAT mapping or "
                        "network genuinely changed. Ask your peer for a fresh token, "
                        "and restart this side with it to resync.");
            }
        }
    }
}

int create_local_listener(uint16_t port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) < 0) { close(sock); return -1; }
    if (listen(sock, 10) < 0) { close(sock); return -1; }
    return sock;
}

void udp_tcp_tunnel_loop(const ConnInfo& peer, uint16_t local_tcp_port) {
    ts_cout("[TUNNEL] Setting up tunnel for peer " + peer.id +
            " (main " + g_main_path.peer_key + ", redundant " + g_alt_path.peer_key + ")");

    udp_hole_punching(g_main_path.sock, g_main_path.peer_addr, g_self_id);
    udp_hole_punching(g_alt_path.sock, g_alt_path.peer_addr, g_self_id);
    g_main_path.last_seen_ms = now_ms();
    g_alt_path.last_seen_ms = now_ms();

    std::thread keepalive(udp_keepalive_loop);
    keepalive.detach();
    std::thread watchdog(watchdog_loop);
    watchdog.detach();

    int local_listener = create_local_listener(local_tcp_port);
    if (local_listener < 0) {
        ts_cout("[TUNNEL] Failed to listen on local TCP port " + std::to_string(local_tcp_port));
        return;
    }
    ts_cout("[TUNNEL] Listening on local TCP port " + std::to_string(local_tcp_port) + " for peer " + peer.id);

    while (true) {
        int tcp_client_sock = accept(local_listener, nullptr, nullptr);
        if (tcp_client_sock < 0) continue;

        ts_cout("[TUNNEL] Accepted TCP client for peer " + peer.id);
        std::thread tcp2udp(tcp_to_udp_tunnel, tcp_client_sock);
        std::thread udp2tcp(udp_to_tcp_tunnel, tcp_client_sock);
        tcp2udp.detach();
        udp2tcp.detach();
    }
}

// One of these runs per socket (main and redundant). Any inbound packet —
// punch or data — marks that specific path as alive, which is exactly
// what the watchdog above is watching.
void udp_recv_loop(int udp_sock, PathState& path) {
    uint8_t buf[1500];
    sockaddr_in from{};
    socklen_t from_len = sizeof(from);

    while (true) {
        ssize_t n = recvfrom(udp_sock, buf, sizeof(buf), 0, (sockaddr*)&from, &from_len);
        if (n <= 0) {
            if (n < 0) ts_cout("[UDP_RECV] Receive error: " + std::string(strerror(errno)));
            continue;
        }

        path.last_seen_ms = now_ms();
        if (g_link_lost.exchange(false)) {
            // A packet arrived after the link was declared lost — the peer
            // is reachable again on this path after all; resume using it.
            ts_cout("[RECOVER] Peer reachable again — resuming.");
        }

        std::string msg(reinterpret_cast<char*>(buf), n);
        if (msg.rfind("punch_from_", 0) == 0) {
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
            ts_cout("[UDP_RECV] Punch message from " + std::string(ip) + ":" +
                    std::to_string(ntohs(from.sin_port)) + " -> " + msg);
            continue;
        }

        UDPPacket packet;
        packet.data.assign(buf, buf + n);
        packet.from_addr = from;
        packet.timestamp = std::chrono::steady_clock::now();
        g_dispatcher.dispatch_packet(packet);
    }
}

int main(int argc, char* argv[]) {
    if (argc < 4 || argc > 5) {
        std::cerr << "Usage:\n"
                  << argv[0] << " <local_udp_port> <your_id> <local_tcp_port> [peer_token]\n"
                  << "\n"
                  << "  peer_token is optional — omit it to be prompted interactively\n"
                  << "  (paste the token your peer printed on their side). Pass it as an\n"
                  << "  argument instead for scripted/non-interactive test runs.\n";
        return 1;
    }

    uint16_t local_udp_port = static_cast<uint16_t>(atoi(argv[1]));
    g_self_id = argv[2];
    uint16_t local_tcp_port = static_cast<uint16_t>(atoi(argv[3]));
    std::string peer_token_arg = (argc == 5) ? argv[4] : "";

    ts_cout("[MAIN] Starting, id=" + g_self_id);

    // Main socket (local_udp_port) and redundant socket (local_udp_port+1),
    // each independently bound and STUN-mapped, so they're two genuinely
    // separate NAT bindings — not just one path relabeled.
    auto open_and_map = [](uint16_t local_port, const char* label, PathState& path) -> bool {
        path.sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (path.sock < 0) { perror("socket"); return false; }

        sockaddr_in local_addr{};
        local_addr.sin_family = AF_INET;
        local_addr.sin_port = htons(local_port);
        local_addr.sin_addr.s_addr = INADDR_ANY;
        if (bind(path.sock, (sockaddr*)&local_addr, sizeof(local_addr)) < 0) {
            perror("bind");
            close(path.sock);
            return false;
        }
        ts_cout(std::string("[MAIN] ") + label + " UDP socket bound to port " + std::to_string(local_port));
        return true;
    };

    if (!open_and_map(local_udp_port, "Main", g_main_path)) return 1;
    if (!open_and_map(local_udp_port + 1, "Redundant", g_alt_path)) return 1;

    std::string main_ip, alt_ip;
    uint16_t main_port, alt_port;
    if (!get_public_address(main_ip, main_port, g_main_path.sock)) {
        ts_cout("[MAIN] Failed to get public address via STUN (main path)");
        return 1;
    }
    ts_cout("[MAIN] Main path public address (via STUN): " + main_ip + ":" + std::to_string(main_port));

    if (!get_public_address(alt_ip, alt_port, g_alt_path.sock)) {
        ts_cout("[MAIN] Failed to get public address via STUN (redundant path)");
        return 1;
    }
    ts_cout("[MAIN] Redundant path public address (via STUN): " + alt_ip + ":" + std::to_string(alt_port));

    ConnInfo self{g_self_id, main_ip, main_port, alt_port};
    std::string my_token = make_token(self);

    std::cout << "\n=== Your token — send this to your peer ===\n"
              << my_token << "\n"
              << "============================================\n\n";

    std::string peer_token = peer_token_arg;
    if (peer_token.empty()) {
        std::cout << "Paste your peer's token, then press Enter: " << std::flush;
        std::getline(std::cin, peer_token);
    }

    ConnInfo peer;
    if (!parse_token(peer_token, peer)) {
        ts_cout("[MAIN] Could not parse peer token — aborting.");
        return 1;
    }
    ts_cout("[MAIN] Peer: " + peer.id + " -> main " + peer.ip + ":" + std::to_string(peer.port) +
            ", redundant " + peer.ip + ":" + std::to_string(peer.alt_port));

    g_main_path.peer_addr.sin_family = AF_INET;
    g_main_path.peer_addr.sin_port = htons(peer.port);
    inet_pton(AF_INET, peer.ip.c_str(), &g_main_path.peer_addr.sin_addr);
    g_main_path.peer_key = peer.ip + ":" + std::to_string(peer.port);

    g_alt_path.peer_addr.sin_family = AF_INET;
    g_alt_path.peer_addr.sin_port = htons(peer.alt_port);
    inet_pton(AF_INET, peer.ip.c_str(), &g_alt_path.peer_addr.sin_addr);
    g_alt_path.peer_key = peer.ip + ":" + std::to_string(peer.alt_port);

    std::thread main_recv(udp_recv_loop, g_main_path.sock, std::ref(g_main_path));
    main_recv.detach();
    std::thread alt_recv(udp_recv_loop, g_alt_path.sock, std::ref(g_alt_path));
    alt_recv.detach();

    udp_tcp_tunnel_loop(peer, local_tcp_port);

    close(g_main_path.sock);
    close(g_alt_path.sock);
    return 0;
}
