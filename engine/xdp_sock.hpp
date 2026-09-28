// ============================================================================
// xdp_sock.hpp — AF_XDP zero-copy UDP socket
// ============================================================================
// Implements AF_XDP socket management using raw Linux syscalls.
// No dependency on libxdp or xsk.h — works with just linux/if_xdp.h.
//
// Architecture:
//   UMEM: a large mmap'd buffer divided into fixed-size frames.
//   Four rings connect userspace ↔ kernel:
//     FILL:       userspace → kernel  "here are empty frames to receive into"
//     RX:         kernel → userspace  "here are received packets"
//     TX:         userspace → kernel  "here are packets to send"
//     COMPLETION: kernel → userspace  "these TX frames are done, reuse them"
//
//  Zero-copy path (requires driver support + XDP_ZEROCOPY flag):
//    RX packet data lives directly in UMEM — no copy from NIC.
//    TX packet data written to UMEM frame — DMA'd directly from userspace.
//
//  Copy-mode fallback (XDP_COPY):
//    Same API, ~20% slower, works on any driver.
//    Still faster than recvmmsg because of ring-buffer batch efficiency.
//
// Requirements:
//   - Linux kernel >= 5.1
//   - XDP program loaded + xsk_map populated (done by XdpLoader)
//   - Interface with at least one RX queue
//   - Root / CAP_NET_ADMIN
// ============================================================================
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include <atomic>
#include <functional>

#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <net/if.h>
#include <linux/if_xdp.h>
#include <linux/if_link.h>
#include <errno.h>


// ── Ring helper — wraps a memory-mapped ring buffer ──────────────────────────
struct XdpRing {
    void*     map_addr = nullptr; // mmap base
    size_t    map_size = 0;
    uint32_t* producer = nullptr; // producer index pointer
    uint32_t* consumer = nullptr; // consumer index pointer
    void*     descs    = nullptr; // ring descriptor array
    uint32_t  size     = 0;       // number of entries (power of 2)
    uint32_t  mask     = 0;       // size - 1
};

// ── XDP descriptor (one entry in RX/TX rings) ────────────────────────────────
// Same layout as struct xdp_desc in linux/if_xdp.h
struct XdpDesc {
    uint64_t addr;   // UMEM offset of frame
    uint32_t len;    // byte length of packet
    uint32_t options;
};

// ── UMEM frame metadata ───────────────────────────────────────────────────────
struct XdpFrame {
    uint8_t* data;    // pointer into UMEM buffer
    uint32_t len;     // valid byte count
    uint64_t umem_off;// offset within UMEM (for ring descriptors)
};

// ── XdpSock — AF_XDP socket pair (one per NIC queue) ─────────────────────────
class XdpSock {
public:
    static constexpr uint32_t FRAME_SIZE   = 4096;       // bytes per UMEM frame
    static constexpr uint32_t FRAME_COUNT  = 4096;       // total frames in UMEM
    static constexpr uint32_t RING_SIZE    = 2048;       // ring entries (power of 2)
    static constexpr size_t   UMEM_SIZE    = (size_t)FRAME_SIZE * FRAME_COUNT;

    // Construct: bind to given interface queue, attempt zero-copy.
    // Falls back to copy mode if zero-copy unsupported.
    XdpSock(const std::string& ifname, uint32_t queue_id,
            int xsk_map_fd, bool force_copy = false);
    ~XdpSock();

    // Non-copyable
    XdpSock(const XdpSock&)            = delete;
    XdpSock& operator=(const XdpSock&) = delete;

    // ── Receive path ─────────────────────────────────────────────────────────
    // Drain all available RX packets. Calls cb(frame) for each.
    // Returns number of packets processed.
    int rx_drain(const std::function<void(const XdpFrame&)>& cb);

    // ── Transmit path ────────────────────────────────────────────────────────
    // Copy data into a UMEM TX frame and enqueue for TX.
    // Returns true if packet was enqueued, false if TX ring full.
    bool tx_enqueue(const void* data, uint32_t len);

    // Flush pending TX (call after a batch of tx_enqueue).
    void tx_flush();

    // ── Poll (call before rx_drain in a loop) ────────────────────────────────
    // Returns true if data available on RX ring.
    bool poll_rx(int timeout_ms = 0);

    int  fd()           const { return xsk_fd_; }
    bool zero_copy()    const { return zero_copy_; }
    const std::string& ifname() const { return ifname_; }

    uint64_t stat_rx_pkts() const { return stat_rx_pkts_; }
    uint64_t stat_tx_pkts() const { return stat_tx_pkts_; }
    uint64_t stat_rx_drop() const { return stat_rx_drop_; }

private:
    std::string ifname_;
    uint32_t    queue_id_;
    int         xsk_fd_  = -1;
    bool        zero_copy_ = false;

    // UMEM
    void*    umem_area_ = MAP_FAILED;
    int      umem_fd_   = -1;  // same as xsk_fd_ for primary socket

    // Rings
    XdpRing fill_ring_;
    XdpRing comp_ring_;
    XdpRing rx_ring_;
    XdpRing tx_ring_;

    // Free frame stack (indices into UMEM)
    std::vector<uint64_t> free_frames_;
    std::vector<uint64_t> pending_tx_; // frames awaiting completion

    // Stats
    uint64_t stat_rx_pkts_ = 0;
    uint64_t stat_tx_pkts_ = 0;
    uint64_t stat_rx_drop_ = 0;

    void setup_umem();
    void setup_rings();
    void bind_socket(uint32_t ifindex, uint32_t queue_id,
                     int xsk_map_fd, bool force_copy);
    void populate_fill_ring();
    void recycle_completions();

    // Ring accessors
    static uint32_t ring_prod(const XdpRing& r) {
        return __atomic_load_n(r.producer, __ATOMIC_ACQUIRE);
    }
    static uint32_t ring_cons(const XdpRing& r) {
        return __atomic_load_n(r.consumer, __ATOMIC_ACQUIRE);
    }
    static void ring_prod_store(const XdpRing& r, uint32_t v) {
        __atomic_store_n(r.producer, v, __ATOMIC_RELEASE);
    }
    static void ring_cons_store(const XdpRing& r, uint32_t v) {
        __atomic_store_n(r.consumer, v, __ATOMIC_RELEASE);
    }
    static XdpDesc* fill_desc(const XdpRing& r, uint32_t idx) {
        return (XdpDesc*)r.descs + (idx & r.mask);
    }

    XdpRing map_ring(int fd, uint64_t pgoff, uint32_t entry_size,
                     uint64_t prod_off, uint64_t cons_off,
                     uint64_t desc_off, uint32_t n);
};

// ── XdpLoader — loads XDP BPF program onto NIC and populates xsk_map ─────────
class XdpLoader {
public:
    // Load xdp_kern.o onto interface, set VPN port in config_map.
    // Returns xsk_map_fd (to pass to XdpSock constructor).
    // Throws on failure.
    static int load(const std::string& ifname,
                    const std::string& bpf_obj_path,
                    uint16_t           vpn_port,
                    uint32_t           flags = 0 /* XDP_FLAGS_* */);

    // Detach XDP program from interface
    static void detach(const std::string& ifname);

    // Populate xsk_map[queue_id] = xsk_fd
    static void register_xsk(int xsk_map_fd, uint32_t queue_id, int xsk_fd);
};

// ── XdpWorker — full zero-copy worker combining socket + dispatch ─────────────
// Drops in as a replacement for the recvmmsg worker in Server.
class XdpWorker {
public:
    using DispatchFn = std::function<void(const uint8_t*, size_t,
                                          const struct sockaddr_in&)>;

    XdpWorker(const std::string& ifname,
              uint32_t queue_id,
              int xsk_map_fd,
              DispatchFn dispatch_fn,
              bool force_copy = false);

    // Run the RX/TX loop. Blocking. Returns when g_running = false.
    void run(const std::atomic<bool>& running, int cpu_hint = -1);

    // Enqueue a packet for TX (thread-safe via spinlock for cross-thread sends)
    void send(const void* data, size_t len, const struct sockaddr_in& dst);

    uint64_t rx_pkts() const;
    uint64_t tx_pkts() const;

private:
    XdpSock   xsk_;
    DispatchFn dispatch_;
    // We embed Ethernet + IP + UDP headers to reconstruct outgoing packets
    // because AF_XDP operates at L2 (raw frames including Ethernet header).
    uint8_t   src_mac_[6]{};
    uint8_t   dst_mac_[6]{};  // gateway MAC (filled from ARP)
    uint32_t  src_ip_ = 0;
    uint16_t  src_port_ = 0;
    bool      gw_mac_resolved_ = false;
    uint64_t  last_resolve_attempt_ms_ = 0; // rate-limits lazy retries from send()

    void fill_headers(uint8_t* frame, const void* payload, size_t plen,
                      const struct sockaddr_in& dst) const;
    // Finds this interface's default-route gateway IP (/proc/net/route),
    // nudges the kernel to ARP-resolve it (a throwaway UDP send), then
    // polls /proc/net/arp for the resolved MAC. AF_XDP TX builds raw L2
    // frames itself -- unlike a normal socket, the kernel never fills in
    // the destination MAC for us, so every off-box packet needs this to
    // actually reach the wire instead of going out as 00:00:00:00:00:00.
    bool resolve_gateway_mac();
};
