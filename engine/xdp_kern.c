// xdp_kern.c — BPF/XDP filter program
// Compiled with: clang -O2 -target bpf -c xdp_kern.c -o xdp_kern.o
//
// Attaches to a NIC and redirects incoming UDP packets on VPN_PORT
// to an AF_XDP socket via xsk_map. All other traffic passes through
// the kernel normally (XDP_PASS).
//
// The xsk_map is a BPF_MAP_TYPE_XSKMAP. The userspace loader
// populates entry [queue_id] = xsk_fd before attaching this program.
// ============================================================================

#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <linux/in.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

// ── Map: queue_id → AF_XDP socket fd ─────────────────────────────────────────
// Populated by userspace before program is attached.
struct {
    __uint(type,        BPF_MAP_TYPE_XSKMAP);
    __uint(max_entries, 64);    // support up to 64 RX queues
    __type(key,         __u32);
    __type(value,       __u32);
} xsk_map SEC(".maps");

// ── Map: config (VPN port, etc.) ─────────────────────────────────────────────
struct {
    __uint(type,        BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key,         __u32);
    __type(value,       __u32); // value[0] = VPN UDP port (host order)
} config_map SEC(".maps");

// ── XDP program ──────────────────────────────────────────────────────────────
SEC("xdp")
int xdp_vpn_filter(struct xdp_md* ctx)
{
    void* data     = (void*)(long)ctx->data;
    void* data_end = (void*)(long)ctx->data_end;

    // ── Parse Ethernet header ─────────────────────────────────────────────
    struct ethhdr* eth = data;
    if ((void*)(eth + 1) > data_end) return XDP_PASS;
    if (eth->h_proto != bpf_htons(ETH_P_IP)) return XDP_PASS;

    // ── Parse IPv4 header ─────────────────────────────────────────────────
    struct iphdr* ip = (struct iphdr*)(eth + 1);
    if ((void*)(ip + 1) > data_end) return XDP_PASS;
    if (ip->protocol != IPPROTO_UDP) return XDP_PASS;

    // Skip IPv4 options (ihl is in 4-byte units)
    struct udphdr* udp = (struct udphdr*)((void*)ip + ip->ihl * 4);
    if ((void*)(udp + 1) > data_end) return XDP_PASS;

    // ── Check destination port ─────────────────────────────────────────────
    __u32 key = 0;
    __u32* vpn_port = bpf_map_lookup_elem(&config_map, &key);
    if (!vpn_port) return XDP_PASS;

    if (udp->dest != bpf_htons((__u16)*vpn_port)) return XDP_PASS;

    // ── Redirect to AF_XDP socket on this RX queue ────────────────────────
    // bpf_redirect_map returns XDP_REDIRECT on success, XDP_DROP on miss.
    // If the AF_XDP socket is not yet populated (e.g. queue not owned),
    // XDP_DROP is returned — we convert that to XDP_PASS to let kernel handle it.
    int ret = bpf_redirect_map(&xsk_map, ctx->rx_queue_index, XDP_PASS);
    return ret;
}

char _license[] SEC("license") = "GPL";
