// ============================================================================
#include <pthread.h>
#include <sched.h>
// xdp_sock.cpp — AF_XDP socket implementation (raw, no xsk.h)
// ============================================================================

#include "xdp_sock.hpp"

#include <cstdlib>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <algorithm>
#include <chrono>

#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <net/ethernet.h>
#include <arpa/inet.h>
#include <ifaddrs.h>

// ── libbpf, for XdpLoader only ────────────────────────────────────────────────
// The AF_XDP socket path above stays raw-syscall (no libbpf dependency,
// by design -- see xdp_sock.hpp). Loading and attaching the XDP program
// is a different story: iproute2's own bundled BPF loader (`ip link set
// dev <if> xdp obj <path> sec xdp`, the previous approach here) failed
// the kernel verifier on this map's BTF relocation on Ubuntu 20.04's
// older toolchain ("R1 type=inv expected=map_ptr" from the verifier),
// and finding xsk_map/config_map afterward by grepping `bpftool map
// show` output for a name substring is fragile -- it searches every BPF
// map on the whole host, not just this program's, so a stale map from a
// previous run or another program's similarly-named map can make it
// silently bind the wrong one. libbpf's own loader (used directly here,
// the same way as this VPS's other AF_XDP program does) doesn't have
// either problem: it resolves this object's own maps by name via BTF
// relocations it computed itself while loading, not a global grep.
//
// Older kernel headers (predating this VPS's) can lack a full `enum
// bpf_stats_type` definition, which libbpf's bpf.h forward-declares --
// valid under C's looser opaque-enum rule, a hard C++ error without a
// full definition. CMake probes for this (MESHVPN_NEED_BPF_STATS_TYPE_SHIM)
// so the shim isn't defined twice on newer headers that already have it.
#if defined(__cplusplus) && defined(MESHVPN_NEED_BPF_STATS_TYPE_SHIM)
enum bpf_stats_type { BPF_STATS_RUN_TIME = 0 };
#endif
#include <bpf/libbpf.h>
#include <bpf/bpf.h>

// libbpf >= 1.0 renamed bpf_set_link_xdp_fd to bpf_xdp_attach/detach;
// this VPS is still on 0.5.0. Probe the header for the symbol instead
// of hardcoding a version cutoff, so this builds against either.
#if __has_include(<bpf/libbpf_version.h>)
#include <bpf/libbpf_version.h>
#endif
#ifndef LIBBPF_MAJOR_VERSION
#define LIBBPF_MAJOR_VERSION 0
#endif

// ─ Linux XDP socket option constants (from linux/if_xdp.h) ───────────────────
#ifndef SOL_XDP
#define SOL_XDP 283
#endif

// ── XdpSock ──────────────────────────────────────────────────────────────────
XdpSock::XdpSock(const std::string& ifname, uint32_t queue_id,
                 int xsk_map_fd, bool force_copy)
    : ifname_(ifname), queue_id_(queue_id)
{
    uint32_t ifindex = if_nametoindex(ifname.c_str());
    if (!ifindex)
        throw std::runtime_error("XdpSock: interface not found: " + ifname);

    // Increase locked memory limit (UMEM needs to be locked)
    struct rlimit r{ RLIM_INFINITY, RLIM_INFINITY };
    if (setrlimit(RLIMIT_MEMLOCK, &r) < 0) {
        fprintf(stderr, "[XDP] Warning: setrlimit MEMLOCK failed: %s\n",
                strerror(errno));
    }

    // Create the AF_XDP socket
    xsk_fd_ = socket(AF_XDP, SOCK_RAW | SOCK_CLOEXEC, 0);
    if (xsk_fd_ < 0)
        throw std::runtime_error(
            std::string("XdpSock: socket(AF_XDP) failed: ") + strerror(errno));

    // Initialise free frame list (all frames start free)
    free_frames_.reserve(FRAME_COUNT);
    for (uint32_t i = 0; i < FRAME_COUNT; i++)
        free_frames_.push_back((uint64_t)i * FRAME_SIZE);

    setup_umem();
    setup_rings();
    bind_socket(ifindex, queue_id, xsk_map_fd, force_copy);
    populate_fill_ring();

    printf("[XDP] %s queue=%u  zero-copy=%s  frames=%u  ring=%u\n",
           ifname_.c_str(), queue_id_,
           zero_copy_ ? "YES" : "NO (copy mode)",
           FRAME_COUNT, RING_SIZE);
}

XdpSock::~XdpSock()
{
    // Unmap rings
    for (XdpRing* r : { &fill_ring_, &comp_ring_, &rx_ring_, &tx_ring_ }) {
        if (r->map_addr && r->map_addr != MAP_FAILED)
            munmap(r->map_addr, r->map_size);
    }
    if (umem_area_ && umem_area_ != MAP_FAILED)
        munmap(umem_area_, UMEM_SIZE);
    if (xsk_fd_ >= 0) close(xsk_fd_);
}

// ── UMEM registration ─────────────────────────────────────────────────────────
void XdpSock::setup_umem()
{
    // Allocate huge-page aligned UMEM with MAP_HUGETLB if available
    umem_area_ = mmap(nullptr, UMEM_SIZE,
                      PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
                      -1, 0);
    if (umem_area_ == MAP_FAILED)
        throw std::runtime_error("XdpSock: mmap UMEM failed");

    struct xdp_umem_reg umem_reg{};
    umem_reg.addr       = (uint64_t)umem_area_;
    umem_reg.len        = UMEM_SIZE;
    umem_reg.chunk_size = FRAME_SIZE;
    umem_reg.headroom   = 0;
    umem_reg.flags      = 0;

    if (setsockopt(xsk_fd_, SOL_XDP, XDP_UMEM_REG,
                   &umem_reg, sizeof(umem_reg)) < 0)
        throw std::runtime_error(
            std::string("XdpSock: XDP_UMEM_REG failed: ") + strerror(errno));

    // Set ring sizes
    uint32_t sz = RING_SIZE;
    if (setsockopt(xsk_fd_, SOL_XDP, XDP_UMEM_FILL_RING,
                   &sz, sizeof(sz)) < 0)
        throw std::runtime_error("XDP_UMEM_FILL_RING");
    if (setsockopt(xsk_fd_, SOL_XDP, XDP_UMEM_COMPLETION_RING,
                   &sz, sizeof(sz)) < 0)
        throw std::runtime_error("XDP_UMEM_COMPLETION_RING");
    if (setsockopt(xsk_fd_, SOL_XDP, XDP_RX_RING, &sz, sizeof(sz)) < 0)
        throw std::runtime_error("XDP_RX_RING");
    if (setsockopt(xsk_fd_, SOL_XDP, XDP_TX_RING, &sz, sizeof(sz)) < 0)
        throw std::runtime_error("XDP_TX_RING");
}

// ── Ring mmap ─────────────────────────────────────────────────────────────────
XdpRing XdpSock::map_ring(int fd, uint64_t pgoff, uint32_t entry_size,
                           uint64_t prod_off, uint64_t cons_off,
                           uint64_t desc_off, uint32_t n)
{
    size_t sz = desc_off + (size_t)n * entry_size;

    XdpRing r{};
    r.size     = n;
    r.mask     = n - 1;
    r.map_size = sz;
    r.map_addr = mmap(nullptr, sz,
                      PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_POPULATE,
                      fd, (off_t)pgoff);
    if (r.map_addr == MAP_FAILED)
        throw std::runtime_error("XdpSock: ring mmap failed");

    r.producer = (uint32_t*)((uint8_t*)r.map_addr + prod_off);
    r.consumer = (uint32_t*)((uint8_t*)r.map_addr + cons_off);
    r.descs    = (uint8_t*)r.map_addr + desc_off;
    return r;
}

void XdpSock::setup_rings()
{
    struct xdp_mmap_offsets off{};
    socklen_t optlen = sizeof(off);
    if (getsockopt(xsk_fd_, SOL_XDP, XDP_MMAP_OFFSETS, &off, &optlen) < 0)
        throw std::runtime_error("XDP_MMAP_OFFSETS");

    fill_ring_ = map_ring(xsk_fd_, XDP_UMEM_PGOFF_FILL_RING,
                          sizeof(uint64_t),
                          off.fr.producer, off.fr.consumer,
                          off.fr.desc, RING_SIZE);

    comp_ring_ = map_ring(xsk_fd_, XDP_UMEM_PGOFF_COMPLETION_RING,
                          sizeof(uint64_t),
                          off.cr.producer, off.cr.consumer,
                          off.cr.desc, RING_SIZE);

    rx_ring_   = map_ring(xsk_fd_, XDP_PGOFF_RX_RING,
                          sizeof(XdpDesc),
                          off.rx.producer, off.rx.consumer,
                          off.rx.desc, RING_SIZE);

    tx_ring_   = map_ring(xsk_fd_, XDP_PGOFF_TX_RING,
                          sizeof(XdpDesc),
                          off.tx.producer, off.tx.consumer,
                          off.tx.desc, RING_SIZE);
}

// ── Bind to NIC queue ─────────────────────────────────────────────────────────
void XdpSock::bind_socket(uint32_t ifindex, uint32_t queue_id,
                           int xsk_map_fd, bool force_copy)
{
    // Try zero-copy first unless forced into copy mode
    uint16_t flags = force_copy ? XDP_COPY : XDP_ZEROCOPY;

    struct sockaddr_xdp sxdp{};
    sxdp.sxdp_family   = AF_XDP;
    sxdp.sxdp_ifindex  = ifindex;
    sxdp.sxdp_queue_id = queue_id;
    sxdp.sxdp_flags    = flags;

    if (bind(xsk_fd_, (struct sockaddr*)&sxdp, sizeof(sxdp)) < 0) {
        // Drivers signal "zero-copy not supported" inconsistently: some
        // return EINVAL, but this VPS's virtio_net returns EOPNOTSUPP
        // ("Operation not supported") instead -- catch both rather than
        // only the one this fallback originally handled, or a driver
        // using the other code silently never falls back to copy mode.
        if (!force_copy && (errno == EINVAL || errno == EOPNOTSUPP)) {
            // Zero-copy not supported by this driver — fall back to copy mode
            fprintf(stderr, "[XDP] Zero-copy not supported on %s, "
                    "using copy mode\n", ifname_.c_str());
            sxdp.sxdp_flags = XDP_COPY;
            if (bind(xsk_fd_, (struct sockaddr*)&sxdp, sizeof(sxdp)) < 0)
                throw std::runtime_error(
                    std::string("XdpSock: bind(AF_XDP, COPY) failed: ")
                    + strerror(errno));
            zero_copy_ = false;
        } else {
            throw std::runtime_error(
                std::string("XdpSock: bind(AF_XDP) failed: ")
                + strerror(errno));
        }
    } else {
        // Check if zero-copy actually engaged
        struct xdp_options opts{};
        socklen_t len = sizeof(opts);
        if (getsockopt(xsk_fd_, SOL_XDP, XDP_OPTIONS, &opts, &len) == 0)
            zero_copy_ = (opts.flags & XDP_OPTIONS_ZEROCOPY) != 0;
    }

    // Register this socket in the XDP program's xsk_map
    // xsk_map[queue_id] = xsk_fd tells the XDP prog where to redirect
    XdpLoader::register_xsk(xsk_map_fd, queue_id, xsk_fd_);
}

// ── Pre-fill FILL ring with free frames ──────────────────────────────────────
void XdpSock::populate_fill_ring()
{
    uint32_t prod = ring_prod(fill_ring_);
    uint32_t n = std::min((uint32_t)free_frames_.size(), RING_SIZE);

    for (uint32_t i = 0; i < n; i++) {
        uint64_t* slot = (uint64_t*)fill_ring_.descs + ((prod + i) & fill_ring_.mask);
        *slot = free_frames_.back();
        free_frames_.pop_back();
    }
    ring_prod_store(fill_ring_, prod + n);
}

// ── Recycle TX completion frames back to free list ────────────────────────────
void XdpSock::recycle_completions()
{
    uint32_t cons = ring_cons(comp_ring_);
    uint32_t prod = ring_prod(comp_ring_);
    uint32_t n    = prod - cons;
    if (!n) return;

    for (uint32_t i = 0; i < n; i++) {
        uint64_t* slot = (uint64_t*)comp_ring_.descs + ((cons + i) & comp_ring_.mask);
        free_frames_.push_back(*slot);
    }
    ring_cons_store(comp_ring_, cons + n);
}

// ── RX drain ─────────────────────────────────────────────────────────────────
int XdpSock::rx_drain(const std::function<void(const XdpFrame&)>& cb)
{
    uint32_t prod = ring_prod(rx_ring_);
    uint32_t cons = ring_cons(rx_ring_);
    uint32_t n    = prod - cons;
    if (!n) return 0;

    int processed = 0;
    uint32_t fill_prod = ring_prod(fill_ring_);

    for (uint32_t i = 0; i < n; i++) {
        XdpDesc* d = (XdpDesc*)rx_ring_.descs + ((cons + i) & rx_ring_.mask);

        XdpFrame f{};
        f.umem_off = d->addr;
        f.len      = d->len;
        f.data     = (uint8_t*)umem_area_ + d->addr;

        cb(f);
        processed++;
        stat_rx_pkts_++;

        // Return the frame to the FILL ring so kernel can receive into it again
        uint64_t* fill_slot = (uint64_t*)fill_ring_.descs
                            + (fill_prod & fill_ring_.mask);
        *fill_slot = d->addr;
        fill_prod++;
    }

    ring_cons_store(rx_ring_, cons + n);
    ring_prod_store(fill_ring_, fill_prod);
    return processed;
}

// ── TX enqueue ───────────────────────────────────────────────────────────────
bool XdpSock::tx_enqueue(const void* data, uint32_t len)
{
    recycle_completions();

    if (free_frames_.empty()) {
        stat_rx_drop_++;
        return false;
    }
    if (len > FRAME_SIZE) return false;

    uint32_t prod = ring_prod(tx_ring_);
    uint32_t cons = ring_cons(tx_ring_);
    if (prod - cons >= RING_SIZE) {
        stat_rx_drop_++;
        return false; // TX ring full
    }

    // Get a free UMEM frame and copy data into it
    uint64_t frame_addr = free_frames_.back();
    free_frames_.pop_back();

    memcpy((uint8_t*)umem_area_ + frame_addr, data, len);

    XdpDesc* d = (XdpDesc*)tx_ring_.descs + (prod & tx_ring_.mask);
    d->addr    = frame_addr;
    d->len     = len;
    d->options = 0;
    ring_prod_store(tx_ring_, prod + 1);

    pending_tx_.push_back(frame_addr);
    stat_tx_pkts_++;
    return true;
}

// ── TX flush — kick the kernel to actually send ───────────────────────────────
void XdpSock::tx_flush()
{
    if (pending_tx_.empty()) return;

    // sendto() with null args wakes the kernel to process TX ring
    // (only needed when XDP_USE_NEED_WAKEUP flag is set, but safe always)
    sendto(xsk_fd_, nullptr, 0, MSG_DONTWAIT, nullptr, 0);
    pending_tx_.clear();
}

// ── Poll ──────────────────────────────────────────────────────────────────────
bool XdpSock::poll_rx(int timeout_ms)
{
    pollfd pfd{ xsk_fd_, POLLIN, 0 };
    return poll(&pfd, 1, timeout_ms) > 0;
}

// ============================================================================
// XdpLoader
// ============================================================================

// The loaded object stays alive for the process's lifetime -- the
// kernel's XDP attachment is keyed off the interface, not this fd, so
// closing it early wouldn't detach anything, but xsk_map/config_map
// lookups by fd need the object (and the fds they returned) to remain
// valid for as long as the caller keeps using them.
static struct bpf_object* g_bpf_obj = nullptr;

// Populate xsk_map[queue_id] = xsk_fd
void XdpLoader::register_xsk(int xsk_map_fd, uint32_t queue_id, int xsk_fd)
{
    if (bpf_map_update_elem(xsk_map_fd, &queue_id, &xsk_fd, BPF_ANY) < 0)
        throw std::runtime_error(
            std::string("XdpLoader: xsk_map update failed: ")
            + strerror(errno));
}

// Load compiled BPF object and attach to NIC via libbpf directly.
//
// This used to shell out to `ip link set dev <if> xdp obj <path> sec
// xdp` and then find xsk_map/config_map by grepping `bpftool map show`
// for a name substring across every BPF map on the host. Both were
// unreliable: iproute2's own bundled loader failed the kernel verifier
// on this map's BTF relocation on this box's toolchain ("R1 type=inv
// expected=map_ptr"), and the bpftool-grep approach could silently
// bind a stale or wrong map if one with a similar name existed from
// another program or a previous run. libbpf resolves this object's own
// maps directly from the relocations it computed while loading it, so
// neither problem applies here.
int XdpLoader::load(const std::string& ifname,
                    const std::string& bpf_obj_path,
                    uint16_t           vpn_port,
                    uint32_t           xdp_flags)
{
    struct bpf_object* obj = bpf_object__open_file(bpf_obj_path.c_str(), nullptr);
    if (libbpf_get_error(obj))
        throw std::runtime_error("XdpLoader: bpf_object__open_file failed for " + bpf_obj_path);
    if (bpf_object__load(obj)) {
        bpf_object__close(obj);
        throw std::runtime_error("XdpLoader: bpf_object__load failed (" + bpf_obj_path + ")");
    }
    g_bpf_obj = obj;

    struct bpf_program* prog = bpf_object__find_program_by_name(obj, "xdp_vpn_filter");
    if (!prog)
        throw std::runtime_error("XdpLoader: program 'xdp_vpn_filter' not found in " + bpf_obj_path);
    int prog_fd = bpf_program__fd(prog);

    int xsk_map_fd = bpf_object__find_map_fd_by_name(obj, "xsk_map");
    int cfg_map_fd = bpf_object__find_map_fd_by_name(obj, "config_map");
    if (xsk_map_fd < 0 || cfg_map_fd < 0)
        throw std::runtime_error("XdpLoader: xsk_map/config_map not found in " + bpf_obj_path);

    uint32_t key = 0, val = static_cast<uint32_t>(vpn_port);
    bpf_map_update_elem(cfg_map_fd, &key, &val, BPF_ANY);

    uint32_t ifindex = if_nametoindex(ifname.c_str());
    if (!ifindex)
        throw std::runtime_error("XdpLoader: interface not found: " + ifname);

    uint32_t flags = xdp_flags ? xdp_flags : XDP_FLAGS_DRV_MODE;
#if LIBBPF_MAJOR_VERSION >= 1
    int err = bpf_xdp_attach(ifindex, prog_fd, flags, nullptr);
    if (err && flags != XDP_FLAGS_SKB_MODE)
        err = bpf_xdp_attach(ifindex, prog_fd, XDP_FLAGS_SKB_MODE, nullptr);
#else
    int err = bpf_set_link_xdp_fd(ifindex, prog_fd, flags);
    if (err && flags != XDP_FLAGS_SKB_MODE)
        err = bpf_set_link_xdp_fd(ifindex, prog_fd, XDP_FLAGS_SKB_MODE);
#endif
    if (err)
        throw std::runtime_error(
            "XdpLoader: failed to attach XDP program to " + ifname +
            ": " + strerror(-err));

    printf("[XDP] Program attached to %s  vpn_port=%u\n", ifname.c_str(), vpn_port);
    return xsk_map_fd;
}

void XdpLoader::detach(const std::string& ifname)
{
    uint32_t ifindex = if_nametoindex(ifname.c_str());
    if (ifindex) {
#if LIBBPF_MAJOR_VERSION >= 1
        bpf_xdp_detach(ifindex, XDP_FLAGS_DRV_MODE, nullptr);
        bpf_xdp_detach(ifindex, XDP_FLAGS_SKB_MODE, nullptr);
#else
        bpf_set_link_xdp_fd(ifindex, -1, XDP_FLAGS_DRV_MODE);
        bpf_set_link_xdp_fd(ifindex, -1, XDP_FLAGS_SKB_MODE);
#endif
    }
    if (g_bpf_obj) { bpf_object__close(g_bpf_obj); g_bpf_obj = nullptr; }
    printf("[XDP] Program detached from %s\n", ifname.c_str());
}

// ============================================================================
// XdpWorker — full RX/TX loop with L2 header handling
// ============================================================================
// AF_XDP operates at Layer 2 (Ethernet frames). The worker adds/strips
// Ethernet+IP+UDP headers around the VPN payload.
// ============================================================================
XdpWorker::XdpWorker(const std::string& ifname, uint32_t queue_id,
                     int xsk_map_fd, DispatchFn dispatch_fn, bool force_copy)
    : xsk_(ifname, queue_id, xsk_map_fd, force_copy)
    , dispatch_(std::move(dispatch_fn))
{
    // Resolve our MAC and gateway MAC for building outgoing frames
    // Get interface MAC
    struct ifreq ifr{};
    strncpy(ifr.ifr_name, ifname.c_str(), IFNAMSIZ-1);
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s >= 0) {
        if (ioctl(s, SIOCGIFHWADDR, &ifr) == 0)
            memcpy(src_mac_, ifr.ifr_hwaddr.sa_data, 6);
        if (ioctl(s, SIOCGIFADDR, &ifr) == 0)
            src_ip_ = ((struct sockaddr_in*)&ifr.ifr_addr)->sin_addr.s_addr;
        close(s);
    }

    printf("[XDP] Worker: iface=%s  src_mac=%02x:%02x:%02x:%02x:%02x:%02x"
           "  src_ip=%s\n",
           ifname.c_str(),
           src_mac_[0],src_mac_[1],src_mac_[2],
           src_mac_[3],src_mac_[4],src_mac_[5],
           inet_ntoa(*(struct in_addr*)&src_ip_));

    gw_mac_resolved_ = resolve_gateway_mac();
    if (gw_mac_resolved_) {
        printf("[XDP] Worker: gateway_mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
               dst_mac_[0],dst_mac_[1],dst_mac_[2],
               dst_mac_[3],dst_mac_[4],dst_mac_[5]);
    } else {
        fprintf(stderr, "[XDP] Warning: gateway MAC not resolved yet -- "
                "outgoing AF_XDP frames will be dropped until it is "
                "(will keep retrying lazily on send)\n");
    }
}

// Finds the default-route gateway for xsk_.ifname() and resolves its MAC.
//
// AF_XDP TX operates below the kernel's normal IP stack -- there is no
// routing table lookup or ARP resolution for us, we hand the NIC a
// complete Ethernet frame. To reach any address off this LAN (which is
// the common case for a mesh VPN peer reached over the internet), that
// frame's *destination* MAC must be the local gateway's, not the
// peer's -- exactly like normal IP routing's next-hop behavior, just
// done by hand here instead of by the kernel.
bool XdpWorker::resolve_gateway_mac()
{
    const std::string& ifname = xsk_.ifname();

    // 1. Find this interface's default-route gateway IP from
    //    /proc/net/route (kernel-order fields: Iface Destination Gateway
    //    Flags ..., all hex except Iface/Flags-as-hex too; a default
    //    route has Destination == 0).
    uint32_t gw_ip = 0;
    {
        FILE* f = fopen("/proc/net/route", "r");
        if (!f) return false;
        char line[256];
        if (!fgets(line, sizeof(line), f)) { fclose(f); return false; } // header
        char iface[64];
        unsigned long dest = 0, gateway = 0;
        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "%63s %lx %lx", iface, &dest, &gateway) == 3) {
                if (dest == 0 && gateway != 0 && ifname == iface) {
                    gw_ip = static_cast<uint32_t>(gateway);
                    break;
                }
            }
        }
        fclose(f);
    }
    if (!gw_ip) return false;

    // 2. Nudge the kernel to ARP-resolve it: a connected UDP socket
    //    triggers route/neighbor resolution for its destination even
    //    though nothing is ever actually sent on the wire yet (a zero-
    //    length datagram to a discard-ish port is enough).
    {
        int s = socket(AF_INET, SOCK_DGRAM, 0);
        if (s >= 0) {
            struct sockaddr_in dst{};
            dst.sin_family = AF_INET;
            dst.sin_addr.s_addr = gw_ip;
            dst.sin_port = htons(9); // discard port; nothing needs to listen
            ::sendto(s, "", 0, 0, (struct sockaddr*)&dst, sizeof(dst));
            close(s);
        }
    }

    // 3. Poll /proc/net/arp for the resolved entry -- ARP resolution is
    //    asynchronous, so the entry may not be populated immediately
    //    after the nudge above.
    for (int attempt = 0; attempt < 20; attempt++) {
        FILE* f = fopen("/proc/net/arp", "r");
        if (f) {
            char line[256];
            fgets(line, sizeof(line), f); // header
            char ip_str[32], hwtype[16], flags_str[16], mac_str[32], mask[32], dev[32];
            while (fgets(line, sizeof(line), f)) {
                if (sscanf(line, "%31s %15s %15s %31s %31s %31s",
                           ip_str, hwtype, flags_str, mac_str, mask, dev) == 6) {
                    struct in_addr a{};
                    if (inet_aton(ip_str, &a) && a.s_addr == gw_ip &&
                        ifname == dev && strcmp(mac_str, "00:00:00:00:00:00") != 0) {
                        unsigned m[6];
                        if (sscanf(mac_str, "%x:%x:%x:%x:%x:%x",
                                   &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) == 6) {
                            for (int i = 0; i < 6; i++) dst_mac_[i] = static_cast<uint8_t>(m[i]);
                            fclose(f);
                            return true;
                        }
                    }
                }
            }
            fclose(f);
        }
        usleep(50000); // 50ms between polls, ~1s total worst case
    }
    return false;
}

// Fill an Ethernet+IPv4+UDP header for an outgoing packet
void XdpWorker::fill_headers(uint8_t* frame, const void* payload, size_t plen,
                              const struct sockaddr_in& dst) const
{
    // Ethernet header (14 bytes)
    struct ether_header* eth = (struct ether_header*)frame;
    memcpy(eth->ether_dhost, dst_mac_, 6);
    memcpy(eth->ether_shost, src_mac_, 6);
    eth->ether_type = htons(ETHERTYPE_IP);

    // IPv4 header (20 bytes)
    struct iphdr* ip = (struct iphdr*)(frame + sizeof(struct ether_header));
    ip->ihl      = 5;
    ip->version  = 4;
    ip->tos      = 0;
    ip->tot_len  = htons(sizeof(struct iphdr) + sizeof(struct udphdr) + plen);
    ip->id       = 0;
    ip->frag_off = htons(IP_DF);
    ip->ttl      = 64;
    ip->protocol = IPPROTO_UDP;
    ip->saddr    = src_ip_;
    ip->daddr    = dst.sin_addr.s_addr;
    ip->check    = 0; // checksum offload or software below

    // IPv4 checksum (software — NIC offload not guaranteed in copy mode)
    uint32_t sum = 0;
    uint16_t* p  = (uint16_t*)ip;
    for (int i = 0; i < 10; i++) sum += ntohs(p[i]);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    ip->check = htons((uint16_t)~sum);

    // UDP header (8 bytes)
    struct udphdr* udp = (struct udphdr*)(ip + 1);
    udp->source = htons(src_port_ ? src_port_ : 51820);
    udp->dest   = dst.sin_port; // already big-endian
    udp->len    = htons(sizeof(struct udphdr) + plen);
    udp->check  = 0; // UDP checksum optional for IPv4

    // Payload
    memcpy((uint8_t*)(udp + 1), payload, plen);
}

void XdpWorker::send(const void* data, size_t len, const struct sockaddr_in& dst)
{
    // Without a resolved gateway MAC every frame would go out addressed
    // to 00:00:00:00:00:00 and be dropped at L2 -- rather than silently
    // eating packets forever if the constructor's resolution attempt
    // lost the ARP race (e.g. interface still coming up), retry lazily
    // here, rate-limited so a sustained send burst doesn't turn into a
    // read()-per-packet storm against /proc/net/arp.
    if (!gw_mac_resolved_) {
        uint64_t now_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        if (now_ms - last_resolve_attempt_ms_ > 1000) {
            last_resolve_attempt_ms_ = now_ms;
            gw_mac_resolved_ = resolve_gateway_mac();
            if (gw_mac_resolved_) {
                printf("[XDP] Worker: gateway_mac resolved on retry = "
                       "%02x:%02x:%02x:%02x:%02x:%02x\n",
                       dst_mac_[0],dst_mac_[1],dst_mac_[2],
                       dst_mac_[3],dst_mac_[4],dst_mac_[5]);
            }
        }
        if (!gw_mac_resolved_) return; // still unresolved -- drop rather than send garbage
    }

    // Build a full Ethernet frame in a local buffer and enqueue
    static thread_local uint8_t frame[2048];
    constexpr size_t HDR = sizeof(struct ether_header)
                         + sizeof(struct iphdr)
                         + sizeof(struct udphdr);

    if (len + HDR > sizeof(frame)) return;

    fill_headers(frame, data, len, dst);
    xsk_.tx_enqueue(frame, HDR + len);
}

void XdpWorker::run(const std::atomic<bool>& running, int cpu_hint)
{
    if (cpu_hint >= 0) {
        cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(cpu_hint, &cs);
        pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
    }

    // Pre-calc L2 header size for stripping on RX
    constexpr size_t ETH_HDR  = sizeof(struct ether_header);
    constexpr size_t IP_HDR   = sizeof(struct iphdr);
    constexpr size_t UDP_HDR  = sizeof(struct udphdr);
    constexpr size_t L2_HDR   = ETH_HDR + IP_HDR + UDP_HDR;

    printf("[XDP] Worker running  zero-copy=%s\n",
           xsk_.zero_copy() ? "YES" : "NO");

    while (running.load(std::memory_order_relaxed)) {
        // Wait up to 1ms for data
        bool ready = xsk_.poll_rx(1);

        if (ready) {
            xsk_.rx_drain([&](const XdpFrame& f) {
                // Frame is a raw Ethernet frame. Strip L2+L3+L4 headers.
                if (f.len <= L2_HDR) return;

                const uint8_t* payload = f.data + L2_HDR;
                size_t         plen    = f.len  - L2_HDR;

                // Reconstruct sockaddr_in from IP header for dispatch
                const struct iphdr* ip =
                    (const struct iphdr*)(f.data + ETH_HDR);
                const struct udphdr* udp =
                    (const struct udphdr*)(f.data + ETH_HDR + IP_HDR);

                struct sockaddr_in from{};
                from.sin_family      = AF_INET;
                from.sin_addr.s_addr = ip->saddr;
                from.sin_port        = udp->source;

                dispatch_(payload, plen, from);
            });
        }

        // Flush any queued TX
        xsk_.tx_flush();
    }
}

uint64_t XdpWorker::rx_pkts() const { return xsk_.stat_rx_pkts(); }
uint64_t XdpWorker::tx_pkts() const { return xsk_.stat_tx_pkts(); }
