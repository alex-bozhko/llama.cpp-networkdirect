#include "transport.h"
#include "ggml-impl.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  ifndef NOMINMAX
#     define NOMINMAX
#  endif
#  include <windows.h>
#  include <winsock2.h>
#else
#  include <arpa/inet.h>
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <netdb.h>
#  include <unistd.h>
#endif
#include <cstdlib>
#include <mutex>
#include <optional>

#ifdef GGML_RPC_RDMA
#  include <infiniband/verbs.h>
#  include <time.h>
#  ifndef _WIN32
#    include <poll.h>
#  endif
#endif // GGML_RPC_RDMA

#ifdef GGML_RPC_ND
#  include <ws2tcpip.h>
#  include <ndsupport.h>
#  include <chrono>
#  include <deque>
#  include <future>
#  include <vector>
#endif // GGML_RPC_ND

#ifdef _WIN32
typedef SOCKET sockfd_t;
using ssize_t = __int64;
#else
typedef int sockfd_t;
#endif

static const char * RPC_DEBUG = std::getenv("GGML_RPC_DEBUG");

#define LOG_DBG(...) \
    do { if (RPC_DEBUG) GGML_LOG_DEBUG(__VA_ARGS__); } while (0)

#ifdef GGML_RPC_RDMA
static constexpr size_t RDMA_CHUNK    = 256 * 1024;   // 256 KiB per send/recv (fits default 8 MiB memlock)
static constexpr int    RDMA_RX_DEPTH = 24;            // pre-posted recv ring: 24 × 256 KiB = 6 MiB
static constexpr size_t RDMA_GID_SIZE = 16;            // RoCE GID / IB GID is always 16 bytes
using rdma_gid_t = std::array<uint8_t, RDMA_GID_SIZE>;

struct rdma_conn {
    struct ibv_context * ctx = nullptr;
    struct ibv_pd * pd  = nullptr;
    struct ibv_cq * scq = nullptr;   // send completions
    struct ibv_cq * rcq = nullptr;   // recv completions
    struct ibv_qp * qp  = nullptr;

    void          * tx_buf = nullptr;
    struct ibv_mr * tx_mr  = nullptr;

    void          * rx_buf = nullptr; // RDMA_RX_DEPTH × RDMA_CHUNK contiguous
    struct ibv_mr * rx_mr  = nullptr;
    int             rx_head = 0;

    uint32_t        max_inline = 0;

    uint8_t * rx_slot(int i) const {
        return static_cast<uint8_t *>(rx_buf) + static_cast<size_t>(i) * RDMA_CHUNK;
    }

    bool post_rx(int i) {
        struct ibv_sge sge = {};
        sge.addr   = (uintptr_t)rx_slot(i);
        sge.length = RDMA_CHUNK;
        sge.lkey   = rx_mr->lkey;
        struct ibv_recv_wr wr = {}, * bad = nullptr;
        wr.wr_id   = (uint64_t)i;
        wr.sg_list = &sge;
        wr.num_sge = 1;
        return ibv_post_recv(qp, &wr, &bad) == 0;
    }

    ~rdma_conn() {
        if (tx_mr) ibv_dereg_mr(tx_mr);
        if (rx_mr) ibv_dereg_mr(rx_mr);
        free(tx_buf);
        free(rx_buf);
        if (qp)  ibv_destroy_qp(qp);
        if (scq) ibv_destroy_cq(scq);
        if (rcq) ibv_destroy_cq(rcq);
        if (pd)  ibv_dealloc_pd(pd);
        if (ctx) ibv_close_device(ctx);
    }
};

// Local RDMA parameters captured during the probe phase and later consumed
// by rdma_activate() after the remote side's caps arrive via HELLO.
struct rdma_local_info {
    uint32_t qpn     = 0;
    uint32_t psn     = 0;
    uint8_t  gid[RDMA_GID_SIZE] = {};
    uint8_t  ib_port = 0;
    int      gid_idx = 0;
    enum ibv_mtu path_mtu = IBV_MTU_1024;
};

struct rdma_caps {
    uint32_t qpn;
    uint32_t psn;
    uint8_t  gid[RDMA_GID_SIZE];
};

static_assert(sizeof(rdma_caps) == RPC_CONN_CAPS_SIZE, "rdma_caps must match conn_caps size");

#endif // GGML_RPC_RDMA

#ifdef GGML_RPC_ND

// NetworkDirect (Windows RDMA) transport.
//
// Same shape as the libibverbs path: the TCP connection is established first and carries
// HELLO, the ND connection is negotiated out of band through conn_caps, and any failure
// silently leaves the socket on TCP.
//
// The provider implements only a subset of ND SPI v2 - no RDMA Read, no shared receive
// queues, no usable memory windows, no inline sends, and only the first SGE of a request
// is honoured - so everything below stays inside that subset.
//
// Flow control: ND does not retry when the peer has no receive posted, so the sender may
// never have more than ND_WINDOW chunks outstanding. After ND_WINDOW chunks it blocks for
// an ack, which the receiver emits once it has consumed and reposted the same number of
// chunks. Both counters are cumulative over the connection, so they stay in lockstep and
// at most one ack is ever in flight - that is why ND_WINDOW leaves one slot spare.

static constexpr uint32_t ND_CAPS_MAGIC = 0x4E445232; // 'NDR2', guards against an ibverbs peer
static constexpr uint32_t ND_MSG_MAGIC  = 0x4E444D31; // 'NDM1'

static constexpr size_t   ND_PAYLOAD    = 256 * 1024;      // provider caps a transfer at 1 MiB
static constexpr int      ND_RX_DEPTH   = 24;              // pre-posted receives: 24 x 256 KiB = 6 MiB
static constexpr int      ND_WINDOW     = ND_RX_DEPTH - 1; // chunks in flight before an ack is required
static constexpr int      ND_CONNECT_TIMEOUT_S = 20;
static constexpr uint64_t ND_POLL_SPIN  = 4096;
static constexpr uint32_t ND_POLL_TICK_MS = 200;   // how long to sleep on the CQ between peer liveness checks
static constexpr uint32_t ND_REAP_MS      = 200;   // bounded reap of a pending CQ notify during teardown
static constexpr uint32_t ND_MAX_EMPTY_NOTIFY = 64;      // CQ claims ready but yields nothing this many times
static constexpr uint32_t ND_MAX_IDLE_STEPS   = 100000;  // completions consumed while no payload arrives
static constexpr uint16_t ND_CM_PORT    = 23517;   // TCP port for ND connection management

enum nd_msg_type : uint32_t {
    ND_MSG_DATA = 1,
    ND_MSG_ACK  = 2,
    ND_MSG_BYE  = 3,
};

struct nd_msg_hdr {
    uint32_t magic;
    uint32_t type;
    uint32_t len;
    uint32_t seq;
};

static constexpr size_t ND_SLOT = ND_PAYLOAD + sizeof(nd_msg_hdr);

struct nd_caps {
    uint32_t reserved0; // must stay zero: an ibverbs peer reads this as qpn and declines the offer
    uint32_t magic;
    uint32_t ipv4;      // network byte order
    uint32_t flags;
    uint8_t  reserved[8];
};

static_assert(sizeof(nd_caps) == RPC_CONN_CAPS_SIZE, "nd_caps must match conn_caps size");

// receive contexts are 1-based so that a null context never looks like slot 0
static inline void * nd_ctx_recv(int slot) { return (void *)(uintptr_t)(slot + 1); }
static inline int    nd_ctx_slot(void * ctx) { return (int)(uintptr_t)ctx - 1; }
static void * const  ND_CTX_SEND = (void *)(uintptr_t)(ND_RX_DEPTH + 2);

struct nd_ov {
    OVERLAPPED ov = {};

    bool init() {
        ov.hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        return ov.hEvent != nullptr;
    }

    ~nd_ov() {
        if (ov.hEvent) {
            CloseHandle(ov.hEvent);
        }
    }
};

static bool nd_done(HRESULT hr, IND2Overlapped * obj, OVERLAPPED * ov) {
    if (hr == ND_PENDING) {
        hr = obj->GetOverlappedResult(ov, TRUE);
    }
    return SUCCEEDED(hr);
}

struct nd_rx_item {
    int      slot;
    uint32_t len;
    uint32_t off;
};

// per-connection phase accounting; enabled with GGML_ND_STATS=<messages per report>
static const char * ND_STATS_ENV = std::getenv("GGML_ND_STATS");
static const uint64_t ND_STATS = ND_STATS_ENV ? strtoull(ND_STATS_ENV, nullptr, 10) : 0;

struct nd_stats {
    uint64_t n_send      = 0;   // data messages posted
    uint64_t n_ack       = 0;   // ack/bye messages posted
    uint64_t n_rx_post   = 0;   // Receive re-posts
    uint64_t n_notify    = 0;   // times the user-mode spin gave up and Notify was called
    uint64_t n_kwait     = 0;   // times a kernel wait was actually entered
    uint64_t post_us     = 0;   // time inside IND2QueuePair::Send
    uint64_t rx_post_us  = 0;   // time inside IND2QueuePair::Receive
    uint64_t cmpl_us     = 0;   // time waiting for our own send completion
    uint64_t recv_us     = 0;   // time waiting for payload to arrive
    uint64_t ackw_us     = 0;   // time blocked on flow-control credit
    uint64_t bytes_tx    = 0;
    uint64_t bytes_rx    = 0;
};

static inline uint64_t nd_now_us() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    static const double s_scale = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1000000.0 / double(f.QuadPart);
    }();
    return (uint64_t)(double(c.QuadPart) * s_scale);
}

struct nd_conn {
    IND2Adapter         * adapter   = nullptr;
    IND2CompletionQueue * cq        = nullptr;
    IND2QueuePair       * qp        = nullptr;
    IND2Connector       * connector = nullptr;
    IND2Listener        * listener  = nullptr;
    IND2MemoryRegion    * tx_mr     = nullptr;
    IND2MemoryRegion    * rx_mr     = nullptr;
    HANDLE                file      = nullptr;

    nd_ov ov;
    nd_ov ov_listen;
    nd_ov ov_cq;      // kept separate: a notify can still be pending while teardown reuses ov

    uint8_t * tx_buf   = nullptr;
    uint8_t * rx_buf   = nullptr;
    UINT32    tx_token = 0;
    UINT32    rx_token = 0;

    struct sockaddr_in local = {};
    struct sockaddr_in peer  = {};

    std::deque<nd_rx_item> rx_ready;
    // slots whose payload has been consumed but whose Receive has not been re-posted yet;
    // posting costs ~150 us on this provider, so it is kept off the reply path
    std::vector<int> rx_repost;
    bool     connected    = false;
    bool     peer_gone    = false;
    bool     notify_armed = false;
    int      ack_pending  = 0;
    int      send_done    = 0;
    int      tx_since_ack = 0;
    int      rx_since_ack = 0;
    uint32_t tx_seq       = 0;
    size_t   tx_fill      = 0;   // payload bytes staged in tx_buf behind the header
    int      corked       = 0;

    nd_stats st;
    nd_stats st_prev;
    uint64_t st_t0 = 0;

    uint8_t * rx_slot(int i) const {
        return rx_buf + (size_t)i * ND_SLOT;
    }

    bool post_rx(int i) {
        ND2_SGE sge = {};
        sge.Buffer            = rx_slot(i);
        sge.BufferLength      = (ULONG)ND_SLOT;
        sge.MemoryRegionToken = rx_token;
        const uint64_t t0 = ND_STATS ? nd_now_us() : 0;
        const bool ok = SUCCEEDED(qp->Receive(nd_ctx_recv(i), &sge, 1));
        if (ND_STATS) {
            st.rx_post_us += nd_now_us() - t0;
            st.n_rx_post++;
        }
        return ok;
    }

    // reports the delta since the previous report, so a window inside steady-state decode
    // can be read straight out of the log without model load skewing it
    void dump_stats(const char * who) {
        if (!ND_STATS) {
            return;
        }
        const uint64_t now  = nd_now_us();
        const uint64_t msgs = (st.n_send - st_prev.n_send) + (st.n_ack - st_prev.n_ack);
        if (msgs == 0) {
            return;
        }
        const double wall = double(now - st_t0);
        const double d    = double(msgs);
        fprintf(stderr, "ND stats [%s] %llu msgs in %.1f ms | per msg: send_ioctl %.1f us, send_cmpl %.1f us, "
                        "rx_post %.1f us | waits: recv %.1f us, credit %.1f us | notify %llu, kwait %llu | tx %.2f MiB rx %.2f MiB\n",
            who, (unsigned long long)msgs, wall/1000.0,
            double(st.post_us    - st_prev.post_us)    / d,
            double(st.cmpl_us    - st_prev.cmpl_us)    / d,
            double(st.rx_post_us - st_prev.rx_post_us) / d,
            double(st.recv_us    - st_prev.recv_us)    / d,
            double(st.ackw_us    - st_prev.ackw_us)    / d,
            (unsigned long long)(st.n_notify - st_prev.n_notify),
            (unsigned long long)(st.n_kwait  - st_prev.n_kwait),
            double(st.bytes_tx - st_prev.bytes_tx)/1048576.0,
            double(st.bytes_rx - st_prev.bytes_rx)/1048576.0);
        fflush(stderr);
        st_prev = st;
        st_t0   = now;
    }

    ~nd_conn() {
        if (connected) {
            LOG_DBG("ND tearing down connection (%u chunks sent)\n", tx_seq);
        }
        if (notify_armed && cq) {
            if (WaitForSingleObject(ov_cq.ov.hEvent, ND_REAP_MS) == WAIT_OBJECT_0) {
                cq->GetOverlappedResult(&ov_cq.ov, FALSE);
            }
            notify_armed = false;
        }
        if (connector) {
            HRESULT hr = connector->Disconnect(&ov.ov);
            if (hr == ND_PENDING) {
                connector->GetOverlappedResult(&ov.ov, TRUE);
            }
        }
        if (qp) {
            qp->Release();
        }
        if (tx_mr) {
            HRESULT hr = tx_mr->Deregister(&ov.ov);
            if (hr == ND_PENDING) {
                tx_mr->GetOverlappedResult(&ov.ov, TRUE);
            }
            tx_mr->Release();
        }
        if (rx_mr) {
            HRESULT hr = rx_mr->Deregister(&ov.ov);
            if (hr == ND_PENDING) {
                rx_mr->GetOverlappedResult(&ov.ov, TRUE);
            }
            rx_mr->Release();
        }
        if (cq) {
            cq->Release();
        }
        if (listener) {
            listener->Release();
        }
        if (connector) {
            connector->Release();
        }
        if (adapter) {
            adapter->Release();
        }
        if (file && file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
        }
        _aligned_free(tx_buf);
        _aligned_free(rx_buf);
    }
};

#endif // GGML_RPC_ND

struct socket_t::impl {
    impl(sockfd_t fd, bool is_server = false) : use_rdma(false), is_server(is_server), fd(fd) {}
    ~impl();
    bool send_data(const void * data, size_t size);
    bool recv_data(void * data, size_t size);
    void cork();
    bool uncork();
    void get_caps(uint8_t * local_caps);
    void update_caps(const uint8_t * remote_caps);

#if defined(GGML_RPC_RDMA) || defined(GGML_RPC_ND)
    bool tcp_peer_closed();
#endif

#ifdef GGML_RPC_RDMA
    std::optional<rdma_gid_t> rdma_build_target_gid();
    bool rdma_probe();
    bool rdma_activate(uint32_t remote_qpn, uint32_t remote_psn, const uint8_t * remote_gid);
    bool rdma_poll(struct ibv_cq * cq, struct ibv_wc * wc);
    bool rdma_send(const void * data, size_t size);
    bool rdma_recv(void * data, size_t size);

    std::unique_ptr<rdma_conn> rdma;
    rdma_local_info            rdma_local = {};
#endif // GGML_RPC_RDMA

#ifdef GGML_RPC_ND
    bool nd_local_addr(struct sockaddr_in * out);
    bool nd_setup(const struct sockaddr_in & local_addr);
    bool nd_setup_any(struct sockaddr_in * chosen);
    bool nd_listen();
    bool nd_accept();
    bool nd_connect(const struct sockaddr_in & peer_addr);
    bool nd_poll(ND2_RESULT * res);
    bool nd_step();
    bool nd_post_send(size_t len);
    bool nd_flush();
    bool nd_repost_rx();
    bool nd_send_ack();
    bool nd_send_bye();
    bool nd_send(const void * data, size_t size);
    bool nd_recv(void * data, size_t size);

    std::unique_ptr<nd_conn> nd;
    bool use_nd = false;
#endif // GGML_RPC_ND
    bool     use_rdma;
    bool     is_server;
    sockfd_t fd;
};

socket_t::impl::~impl() {
#ifdef GGML_RPC_RDMA
    rdma.reset();
#endif // GGML_RPC_RDMA
#ifdef GGML_RPC_ND
    if (use_nd && nd && nd->connected && !nd->peer_gone) {
        (void) nd_send_bye();
    }
    nd.reset();
#endif // GGML_RPC_ND
    LOG_DBG("[%s] closing socket %d\n", __func__, this->fd);
#ifdef _WIN32
    if (fd != INVALID_SOCKET) closesocket(this->fd);
#else
    if (fd >= 0) close(this->fd);
#endif
}

#if defined(GGML_RPC_RDMA) || defined(GGML_RPC_ND)

// Once an RDMA transport is active no payload travels over the TCP socket, so any
// readability on it means end-of-stream rather than pending data. This is the only
// way to notice a peer that vanished without an orderly shutdown.
bool socket_t::impl::tcp_peer_closed() {
#ifdef _WIN32
    if (fd == INVALID_SOCKET) return false;
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(fd, &rd);
    timeval tv = { 0, 0 };
    if (select(0, &rd, nullptr, nullptr, &tv) <= 0) {
        return false;
    }
    char b;
    int r = recv(fd, &b, 1, MSG_PEEK);
    if (r == 0) return true;
    return r < 0 && WSAGetLastError() != WSAEWOULDBLOCK;
#else
    if (fd < 0) return false;
    struct pollfd pfd = { fd, POLLIN | POLLRDHUP, 0 };
    int r = poll(&pfd, 1, 0);
    return r > 0 && (pfd.revents & (POLLHUP | POLLERR | POLLRDHUP));
#endif
}

#endif // GGML_RPC_RDMA || GGML_RPC_ND

#ifdef GGML_RPC_RDMA

// Build a RoCE GID-shaped 16-byte target from a TCP socket's local address.
// Used to match the socket's local IP against the kernel's GID table so that
// a single memcmp handles IPv4, IPv4-mapped IPv6, and native IPv6 uniformly:
//   AF_INET                -> ::ffff:a.b.c.d  (bytes 10-11 = 0xff, last 4 = IPv4)
//   AF_INET6 (IPv4-mapped) -> ::ffff:a.b.c.d  (already in GID shape)
//   AF_INET6 (native v6)   -> the 16-byte IPv6 address as-is
// Returns std::nullopt on unsupported family or getsockname failure.
std::optional<rdma_gid_t> socket_t::impl::rdma_build_target_gid() {
    sockaddr_storage addr = {};
    socklen_t addr_len = sizeof(addr);
    if (getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &addr_len) != 0) {
        return std::nullopt;
    }
    rdma_gid_t target = {};
    if (addr.ss_family == AF_INET) {
        const auto * a = reinterpret_cast<const sockaddr_in *>(&addr);
        target[10] = 0xff;
        target[11] = 0xff;
        memcpy(&target[12], &a->sin_addr, 4);
        return target;
    }
    if (addr.ss_family == AF_INET6) {
        const auto * a = reinterpret_cast<const sockaddr_in6 *>(&addr);
        memcpy(target.data(), &a->sin6_addr, RDMA_GID_SIZE);
        return target;
    }
    return std::nullopt;
}

bool socket_t::impl::rdma_probe() {
    const char * dev_env = std::getenv("GGML_RDMA_DEV");
    const char * gid_env = std::getenv("GGML_RDMA_GID");

    auto target_gid = rdma_build_target_gid();
    if (!target_gid) {
        return false;
    }

    const uint8_t ib_port = 1;
    int num_devs = 0;
    ibv_device ** devs = ibv_get_device_list(&num_devs);
    if (!devs || num_devs == 0) return false;

    ibv_context * ibctx = nullptr;
    const char * matched_dev = nullptr;
    int gid_idx = gid_env ? atoi(gid_env) : -1;
    int gid_version = IBV_GID_TYPE_IB;  // 0 = unknown/IB

    for (int d = 0; d < num_devs; d++) {
        const char * dn = ibv_get_device_name(devs[d]);
        if (dev_env && strcmp(dev_env, dn) != 0) continue;

        ibv_context * ctx = ibv_open_device(devs[d]);
        if (!ctx) continue;

        ibv_port_attr pa;
        if (ibv_query_port(ctx, ib_port, &pa) != 0) { ibv_close_device(ctx); continue; }

        int found_gid = gid_idx;
        int found_version = IBV_GID_TYPE_IB;
        if (found_gid < 0) {
            // Find a GID on this port whose bytes equal the local TCP address
            // (IPv4 or IPv6). Prefer RoCE v2 (UDP/IP, L3-routable) over v1
            // (raw Ethernet, same-L2 only) so silent hangs on L3-routed paths
            // are avoided. ibv_query_gid_ex returns gid+type in one call.
            int v2_idx = -1;
            int v1_idx = -1;
            for (int i = 0; i < pa.gid_tbl_len; i++) {
                ibv_gid_entry entry = {};
                if (ibv_query_gid_ex(ctx, ib_port, i, &entry, 0) != 0) continue;
                if (memcmp(entry.gid.raw, target_gid->data(), RDMA_GID_SIZE) != 0) continue;
                if (entry.gid_type == IBV_GID_TYPE_ROCE_V2 && v2_idx < 0) {
                    v2_idx = i;
                } else if (entry.gid_type == IBV_GID_TYPE_ROCE_V1 && v1_idx < 0) {
                    v1_idx = i;
                }
            }
            if (v2_idx >= 0) {
                found_gid = v2_idx;
                found_version = IBV_GID_TYPE_ROCE_V2;
            } else if (v1_idx >= 0) {
                found_gid = v1_idx;
                found_version = IBV_GID_TYPE_ROCE_V1;
            }
        } else {
            // Explicit GID index from GGML_RDMA_GID — fetch its type for logging.
            ibv_gid_entry entry = {};
            if (ibv_query_gid_ex(ctx, ib_port, found_gid, &entry, 0) == 0) {
                found_version = entry.gid_type;
            }
        }
        if (found_gid >= 0) {
            ibctx = ctx;
            gid_idx = found_gid;
            gid_version = found_version;
            matched_dev = dn;
            rdma_local.path_mtu = pa.active_mtu;
            break;
        }
        ibv_close_device(ctx);
    }
    ibv_free_device_list(devs);
    if (!ibctx) return false;

    rdma_local.ib_port = ib_port;
    rdma_local.gid_idx = gid_idx;

    rdma = std::make_unique<rdma_conn>();
    rdma->ctx = ibctx;

    rdma->pd = ibv_alloc_pd(ibctx);
    if (!rdma->pd) return false;

    rdma->scq = ibv_create_cq(ibctx, 16, nullptr, nullptr, 0);
    rdma->rcq = ibv_create_cq(ibctx, RDMA_RX_DEPTH + 4, nullptr, nullptr, 0);
    if (!rdma->scq || !rdma->rcq) return false;

    ibv_qp_init_attr qia = {};
    qia.send_cq = rdma->scq;
    qia.recv_cq = rdma->rcq;
    qia.qp_type = IBV_QPT_RC;
    qia.cap.max_send_wr     = 4;
    qia.cap.max_recv_wr     = RDMA_RX_DEPTH + 4;
    qia.cap.max_send_sge    = 1;
    qia.cap.max_recv_sge    = 1;
    qia.cap.max_inline_data = 256;

    rdma->qp = ibv_create_qp(rdma->pd, &qia);
    if (!rdma->qp) return false;
    rdma->max_inline = qia.cap.max_inline_data;

    rdma->tx_buf = aligned_alloc(4096, RDMA_CHUNK);
    rdma->rx_buf = aligned_alloc(4096, static_cast<size_t>(RDMA_RX_DEPTH) * RDMA_CHUNK);
    if (!rdma->tx_buf || !rdma->rx_buf) return false;

    rdma->tx_mr = ibv_reg_mr(rdma->pd, rdma->tx_buf, RDMA_CHUNK, IBV_ACCESS_LOCAL_WRITE);
    rdma->rx_mr = ibv_reg_mr(rdma->pd, rdma->rx_buf, static_cast<size_t>(RDMA_RX_DEPTH) * RDMA_CHUNK,
                           IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!rdma->tx_mr || !rdma->rx_mr) return false;

    ibv_gid local_gid;
    if (ibv_query_gid(ibctx, ib_port, gid_idx, &local_gid) != 0) return false;

    rdma_local.qpn = rdma->qp->qp_num;
    rdma_local.psn = rdma->qp->qp_num & 0xffffff;
    memcpy(&rdma_local.gid, &local_gid, RDMA_GID_SIZE);

    const char * ver_str = "";
    if (gid_version == IBV_GID_TYPE_ROCE_V2) {
        ver_str = " RoCEv2";
    } else if (gid_version == IBV_GID_TYPE_ROCE_V1) {
        ver_str = " RoCEv1";
    }
    GGML_LOG_INFO("RDMA probed: dev=%s gid=%d%s qpn=%u inline=%u\n",
                  matched_dev, gid_idx, ver_str, rdma_local.qpn, rdma->max_inline);
    return true;
}

// Phase 2: Given remote QPN/PSN/GID, transition QP: RESET->INIT->pre-post->RTR->RTS.
// On success, the connection is live and ready for rdma_send/rdma_recv.
bool socket_t::impl::rdma_activate(uint32_t remote_qpn, uint32_t remote_psn, const uint8_t * remote_gid) {
    // RESET -> INIT
    {
        struct ibv_qp_attr a = {};
        a.qp_state        = IBV_QPS_INIT;
        a.port_num        = rdma_local.ib_port;
        a.pkey_index      = 0;
        a.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_LOCAL_WRITE;
        if (ibv_modify_qp(rdma->qp, &a,
                IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS) != 0) {
            return false;
        }
    }

    for (int i = 0; i < RDMA_RX_DEPTH; i++) {
        if (!rdma->post_rx(i)) return false;
    }

    // INIT -> RTR
    {
        struct ibv_qp_attr a = {};
        a.qp_state           = IBV_QPS_RTR;
        a.path_mtu           = rdma_local.path_mtu;
        a.dest_qp_num        = remote_qpn;
        a.rq_psn             = remote_psn;
        a.max_dest_rd_atomic = 1;
        a.min_rnr_timer      = 1;
        a.ah_attr.is_global  = 1;
        memcpy(&a.ah_attr.grh.dgid, remote_gid, RDMA_GID_SIZE);
        a.ah_attr.grh.hop_limit  = 1;
        a.ah_attr.grh.sgid_index = rdma_local.gid_idx;
        a.ah_attr.dlid       = 0;
        a.ah_attr.port_num   = rdma_local.ib_port;
        if (ibv_modify_qp(rdma->qp, &a,
                IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
                IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER) != 0) {
            return false;
        }
    }

    // RTR -> RTS
    {
        struct ibv_qp_attr a = {};
        a.qp_state     = IBV_QPS_RTS;
        a.timeout      = 14;
        a.retry_cnt    = 7;
        a.rnr_retry    = 7;
        a.sq_psn       = rdma_local.psn;
        a.max_rd_atomic = 1;
        if (ibv_modify_qp(rdma->qp, &a,
                IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
                IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC) != 0) {
            return false;
        }
    }

    GGML_LOG_INFO("RDMA activated: qpn=%u->%u mtu=%d rx_depth=%d\n",
                  rdma_local.qpn, remote_qpn, 128 << rdma_local.path_mtu, RDMA_RX_DEPTH);
    return true;
}

bool socket_t::impl::rdma_poll(struct ibv_cq * cq, struct ibv_wc * wc) {
    for (uint64_t s = 0; ; s++) {
        int n = ibv_poll_cq(cq, 1, wc);
        if (n > 0) {
            if (wc->status != IBV_WC_SUCCESS) {
                GGML_LOG_ERROR("RDMA CQ wc error: status=%d (%s) vendor_err=0x%x\n",
                    wc->status, ibv_wc_status_str(wc->status), wc->vendor_err);
            }
            return wc->status == IBV_WC_SUCCESS;
        }
        if (n < 0) return false;
        if ((s & 0xFFFFF) == 0 && s > 0) {
            if (tcp_peer_closed()) {
                return false;
            }
        }
    }
}

bool socket_t::impl::rdma_send(const void * data, size_t size) {
    rdma_conn * c = rdma.get();
    const uint8_t * src = (const uint8_t *)data;
    size_t rem = size;
    while (rem > 0) {
        size_t chunk = std::min(rem, RDMA_CHUNK);

        struct ibv_sge sge = {};
        struct ibv_send_wr wr = {}, * bad = nullptr;
        wr.opcode  = IBV_WR_SEND;
        wr.sg_list = &sge;
        wr.num_sge = 1;

        if (chunk <= c->max_inline) {
            sge.addr   = (uintptr_t)src;
            sge.length = chunk;
            wr.send_flags = IBV_SEND_SIGNALED | IBV_SEND_INLINE;
        } else {
            memcpy(c->tx_buf, src, chunk);
            sge.addr   = (uintptr_t)c->tx_buf;
            sge.length = chunk;
            sge.lkey   = c->tx_mr->lkey;
            wr.send_flags = IBV_SEND_SIGNALED;
        }

        if (ibv_post_send(c->qp, &wr, &bad) != 0) return false;
        struct ibv_wc wc;
        if (!rdma_poll(c->scq, &wc)) return false;

        src += chunk;
        rem -= chunk;
    }
    return true;
}

bool socket_t::impl::rdma_recv(void * data, size_t size) {
    rdma_conn * c = rdma.get();
    uint8_t * dst = (uint8_t *)data;
    size_t rem = size;
    while (rem > 0) {
        struct ibv_wc wc;
        if (!rdma_poll(c->rcq, &wc)) return false;

        int slot = (int)wc.wr_id;
        size_t got = wc.byte_len;
        memcpy(dst, c->rx_slot(slot), got);

        if (!c->post_rx(slot)) return false;

        dst += got;
        rem -= got;
    }
    return true;
}

#endif // GGML_RPC_RDMA

#ifdef GGML_RPC_ND

struct nd_addr_buf { char s[32]; };

static nd_addr_buf nd_addr_str(const struct sockaddr_in & sa) {
    nd_addr_buf b;
    char ip[INET_ADDRSTRLEN] = "?";
    InetNtopA(AF_INET, (void *)&sa.sin_addr, ip, sizeof(ip));
    snprintf(b.s, sizeof(b.s), "%s:%u", ip, (unsigned)ntohs(sa.sin_port));
    return b;
}

bool socket_t::impl::nd_local_addr(struct sockaddr_in * out) {
    // an explicit override lets the RDMA NIC differ from the one carrying the TCP control channel
    if (const char * env = std::getenv("GGML_ND_ADDR")) {
        memset(out, 0, sizeof(*out));
        out->sin_family = AF_INET;
        if (InetPtonA(AF_INET, env, &out->sin_addr) != 1) {
            GGML_LOG_ERROR("GGML_ND_ADDR is not a valid IPv4 address: %s\n", env);
            return false;
        }
        LOG_DBG("ND local address %s (from GGML_ND_ADDR)\n", nd_addr_str(*out).s);
        return true;
    }
    int len = sizeof(*out);
    if (getsockname(fd, (struct sockaddr *)out, &len) != 0 || out->sin_family != AF_INET) {
        LOG_DBG("ND getsockname on the TCP channel failed, no local address\n");
        return false;
    }
    out->sin_port = 0;
    LOG_DBG("ND local address %s (from the TCP channel)\n", nd_addr_str(*out).s);
    return true;
}

bool socket_t::impl::nd_setup(const struct sockaddr_in & local_addr) {
    auto c = std::make_unique<nd_conn>();
    if (!c->ov.init() || !c->ov_listen.init() || !c->ov_cq.init()) {
        return false;
    }
    c->local = local_addr;

    HRESULT hr = NdOpenAdapter(IID_IND2Adapter, (const struct sockaddr *)&c->local, sizeof(c->local),
                               (void **)&c->adapter);
    if (FAILED(hr)) {
        LOG_DBG("NdOpenAdapter(%s) failed: 0x%08lx\n", nd_addr_str(c->local).s, (unsigned long)hr);
        return false;
    }
    LOG_DBG("ND adapter opened on %s\n", nd_addr_str(c->local).s);
    hr = c->adapter->CreateOverlappedFile(&c->file);
    if (FAILED(hr)) {
        LOG_DBG("ND CreateOverlappedFile failed: 0x%08lx\n", (unsigned long)hr);
        return false;
    }

    ND2_ADAPTER_INFO info = {};
    info.InfoVersion = ND_VERSION_2;
    ULONG cb_info = sizeof(info);
    hr = c->adapter->Query(&info, &cb_info);
    if (FAILED(hr)) {
        LOG_DBG("ND adapter Query failed: 0x%08lx\n", (unsigned long)hr);
        return false;
    }
    LOG_DBG("ND adapter vendor=0x%04x device=0x%04x id=0x%016llx flags=0x%08lx\n",
            (unsigned)info.VendorId, (unsigned)info.DeviceId,
            (unsigned long long)info.AdapterId, (unsigned long)info.AdapterFlags);
    LOG_DBG("ND adapter limits: transfer=%lu inline=%lu reg=%zu sge(init/recv)=%lu/%lu\n",
            (unsigned long)info.MaxTransferLength, (unsigned long)info.MaxInlineDataSize,
            (size_t)info.MaxRegistrationSize,
            (unsigned long)info.MaxInitiatorSge, (unsigned long)info.MaxReceiveSge);
    LOG_DBG("ND adapter depths: rq=%lu iq=%lu cq=%lu\n",
            (unsigned long)info.MaxReceiveQueueDepth, (unsigned long)info.MaxInitiatorQueueDepth,
            (unsigned long)info.MaxCompletionQueueDepth);
    if (info.MaxTransferLength < ND_SLOT || info.MaxReceiveQueueDepth < ND_RX_DEPTH) {
        GGML_LOG_ERROR("ND adapter limits too small (transfer=%lu, rq=%lu)\n",
                       (unsigned long)info.MaxTransferLength, (unsigned long)info.MaxReceiveQueueDepth);
        return false;
    }

    c->tx_buf = (uint8_t *)_aligned_malloc(ND_SLOT, 4096);
    c->rx_buf = (uint8_t *)_aligned_malloc(ND_SLOT * ND_RX_DEPTH, 4096);
    if (!c->tx_buf || !c->rx_buf) {
        return false;
    }

    hr = c->adapter->CreateMemoryRegion(IID_IND2MemoryRegion, c->file, (void **)&c->tx_mr);
    if (FAILED(hr)) {
        return false;
    }
    hr = c->adapter->CreateMemoryRegion(IID_IND2MemoryRegion, c->file, (void **)&c->rx_mr);
    if (FAILED(hr)) {
        return false;
    }
    hr = c->tx_mr->Register(c->tx_buf, ND_SLOT, ND_MR_FLAG_ALLOW_LOCAL_WRITE, &c->ov.ov);
    if (!nd_done(hr, c->tx_mr, &c->ov.ov)) {
        LOG_DBG("ND tx Register failed: 0x%08lx\n", (unsigned long)hr);
        return false;
    }
    hr = c->rx_mr->Register(c->rx_buf, ND_SLOT * ND_RX_DEPTH, ND_MR_FLAG_ALLOW_LOCAL_WRITE, &c->ov.ov);
    if (!nd_done(hr, c->rx_mr, &c->ov.ov)) {
        LOG_DBG("ND rx Register failed: 0x%08lx\n", (unsigned long)hr);
        return false;
    }
    c->tx_token = c->tx_mr->GetLocalToken();
    c->rx_token = c->rx_mr->GetLocalToken();
    LOG_DBG("ND registered tx=%zu B (token 0x%08lx) rx=%zu B (token 0x%08lx)\n",
            ND_SLOT, (unsigned long)c->tx_token,
            ND_SLOT * (size_t)ND_RX_DEPTH, (unsigned long)c->rx_token);

    const ULONG cq_depth = std::min<ULONG>(info.MaxCompletionQueueDepth, 2 * ND_RX_DEPTH + 8);
    hr = c->adapter->CreateCompletionQueue(IID_IND2CompletionQueue, c->file, cq_depth, 0, 0, (void **)&c->cq);
    if (FAILED(hr)) {
        LOG_DBG("ND CreateCompletionQueue(depth=%lu) failed: 0x%08lx\n", (unsigned long)cq_depth, (unsigned long)hr);
        return false;
    }
    LOG_DBG("ND completion queue created (depth=%lu)\n", (unsigned long)cq_depth);
    const ULONG iq_depth = std::min<ULONG>(info.MaxInitiatorQueueDepth, ND_RX_DEPTH);
    hr = c->adapter->CreateQueuePair(IID_IND2QueuePair, c->cq, c->cq, nullptr,
                                     ND_RX_DEPTH, iq_depth,
                                     1, 1, 0, (void **)&c->qp);
    if (FAILED(hr)) {
        LOG_DBG("ND CreateQueuePair(rq=%d iq=%lu) failed: 0x%08lx\n",
                ND_RX_DEPTH, (unsigned long)iq_depth, (unsigned long)hr);
        return false;
    }
    LOG_DBG("ND queue pair created (rq=%d iq=%lu sge=1)\n", ND_RX_DEPTH, (unsigned long)iq_depth);
    hr = c->adapter->CreateConnector(IID_IND2Connector, c->file, (void **)&c->connector);
    if (FAILED(hr)) {
        LOG_DBG("ND CreateConnector failed: 0x%08lx\n", (unsigned long)hr);
        return false;
    }

    // receives must be posted before the connection is accepted or completed
    for (int i = 0; i < ND_RX_DEPTH; i++) {
        if (!c->post_rx(i)) {
            LOG_DBG("ND initial Receive post failed at slot %d\n", i);
            return false;
        }
    }
    LOG_DBG("ND posted %d receives of %zu B each\n", ND_RX_DEPTH, ND_SLOT);

    c->st_t0 = nd_now_us();
    nd = std::move(c);
    return true;
}

// tries the adapter behind the TCP channel first, then any other ND capable local address
bool socket_t::impl::nd_setup_any(struct sockaddr_in * chosen) {
    struct sockaddr_in addr = {};
    if (nd_local_addr(&addr) && nd_setup(addr)) {
        *chosen = addr;
        return true;
    }
    nd.reset();
    if (std::getenv("GGML_ND_ADDR")) {
        // an explicit choice must not be silently overridden
        return false;
    }
    LOG_DBG("ND setup on the TCP-local address failed, scanning ND capable addresses\n");

    SIZE_T cb_list = 0;
    NdQueryAddressList(0, nullptr, &cb_list);
    if (cb_list == 0) {
        LOG_DBG("ND NdQueryAddressList reports no ND capable local addresses\n");
        return false;
    }
    std::vector<uint8_t> raw(cb_list);
    SOCKET_ADDRESS_LIST * list = (SOCKET_ADDRESS_LIST *)raw.data();
    if (FAILED(NdQueryAddressList(0, list, &cb_list))) {
        LOG_DBG("ND NdQueryAddressList failed\n");
        return false;
    }
    LOG_DBG("ND found %d local address(es)\n", list->iAddressCount);
    for (int i = 0; i < list->iAddressCount; i++) {
        const struct sockaddr * sa = list->Address[i].lpSockaddr;
        if (!sa || sa->sa_family != AF_INET) {
            continue;
        }
        memset(&addr, 0, sizeof(addr));
        memcpy(&addr, sa, sizeof(addr));
        addr.sin_port = 0;
        LOG_DBG("ND trying local address %s\n", nd_addr_str(addr).s);
        if (nd_setup(addr)) {
            *chosen = addr;
            return true;
        }
        nd.reset();
    }
    return false;
}

bool socket_t::impl::nd_listen() {
    nd_conn * c = nd.get();
    struct sockaddr_in listen_addr = c->local;
    listen_addr.sin_port = htons(ND_CM_PORT);

    // Probe the CM port to prevent duplicate listeners with providers that allow address reuse.
    {
        sockfd_t probe = socket(AF_INET, SOCK_STREAM, 0);
        if (probe == INVALID_SOCKET) {
            return false;
        }
        const bool taken = bind(probe, (const struct sockaddr *)&listen_addr, sizeof(listen_addr)) != 0;
        closesocket(probe);
        if (taken) {
            LOG_DBG("ND CM port %u already in use on this host, staying on TCP\n", ND_CM_PORT);
            return false;
        }
    }

    HRESULT hr = c->adapter->CreateListener(IID_IND2Listener, c->file, (void **)&c->listener);
    if (FAILED(hr)) {
        LOG_DBG("ND CreateListener failed: 0x%08lx\n", (unsigned long)hr);
        return false;
    }
    hr = c->listener->Bind((const struct sockaddr *)&listen_addr, sizeof(listen_addr));
    if (FAILED(hr)) {
        LOG_DBG("ND listener Bind failed: 0x%08lx\n", (unsigned long)hr);
        return false;
    }
    hr = c->listener->Listen(1);
    if (FAILED(hr)) {
        LOG_DBG("ND Listen failed: 0x%08lx\n", (unsigned long)hr);
        return false;
    }
    LOG_DBG("ND listening on %s (CM port %u)\n", nd_addr_str(listen_addr).s, ND_CM_PORT);
    return true;
}

bool socket_t::impl::nd_accept() {
    nd_conn * c = nd.get();

    LOG_DBG("ND waiting for a connection request (timeout %ds)\n", ND_CONNECT_TIMEOUT_S);
    // GetConnectionRequest blocks, so bound the wait in case the peer never dials in
    auto pending = std::async(std::launch::async, [c]() {
        return c->listener->GetConnectionRequest(c->connector, &c->ov_listen.ov);
    });
    if (pending.wait_for(std::chrono::seconds(ND_CONNECT_TIMEOUT_S)) != std::future_status::ready) {
        c->listener->CancelOverlappedRequests();
        pending.wait();
        GGML_LOG_ERROR("ND connection request timed out\n");
        return false;
    }
    HRESULT hr = pending.get();
    if (!nd_done(hr, c->listener, &c->ov_listen.ov)) {
        LOG_DBG("ND GetConnectionRequest failed: 0x%08lx\n", (unsigned long)hr);
        return false;
    }
    struct sockaddr_in peer = {};
    ULONG cb_peer = sizeof(peer);
    if (SUCCEEDED(c->connector->GetPeerAddress((struct sockaddr *)&peer, &cb_peer))) {
        LOG_DBG("ND connection request from %s\n", nd_addr_str(peer).s);
    }
    hr = c->connector->Accept(c->qp, 0, 0, nullptr, 0, &c->ov.ov);
    if (!nd_done(hr, c->connector, &c->ov.ov)) {
        LOG_DBG("ND Accept failed: 0x%08lx\n", (unsigned long)hr);
        return false;
    }
    LOG_DBG("ND connection accepted\n");
    c->connected = true;
    return true;
}

bool socket_t::impl::nd_connect(const struct sockaddr_in & peer_addr) {
    // the NIC that reaches the peer's RDMA address may not be the one carrying the TCP channel
    struct sockaddr_in resolved = {};
    SIZE_T cb_resolved = sizeof(resolved);
    HRESULT hr = NdResolveAddress((const struct sockaddr *)&peer_addr, sizeof(peer_addr),
                                  (struct sockaddr *)&resolved, &cb_resolved);
    if (SUCCEEDED(hr) && resolved.sin_family == AF_INET &&
        resolved.sin_addr.s_addr != nd->local.sin_addr.s_addr) {
        resolved.sin_port = 0;
        LOG_DBG("ND peer %s is reached via %s, reopening the adapter\n",
                nd_addr_str(peer_addr).s, nd_addr_str(resolved).s);
        nd.reset();
        if (!nd_setup(resolved)) {
            return false;
        }
    }

    nd_conn * c = nd.get();
    c->peer = peer_addr;
    c->peer.sin_port = htons(ND_CM_PORT);

    hr = c->connector->Bind((const struct sockaddr *)&c->local, sizeof(c->local));
    if (FAILED(hr)) {
        LOG_DBG("ND connector Bind failed: 0x%08lx\n", (unsigned long)hr);
        return false;
    }
    LOG_DBG("ND connecting %s -> %s (CM port %u, timeout %ds)\n",
            nd_addr_str(c->local).s, nd_addr_str(c->peer).s, ND_CM_PORT, ND_CONNECT_TIMEOUT_S);
    // Connect blocks in the provider's TCP-based CM, so bound it the same way nd_accept is bounded
    auto pending = std::async(std::launch::async, [c]() {
        return c->connector->Connect(c->qp, (const struct sockaddr *)&c->peer, sizeof(c->peer),
                                     0, 0, nullptr, 0, &c->ov.ov);
    });
    if (pending.wait_for(std::chrono::seconds(ND_CONNECT_TIMEOUT_S)) != std::future_status::ready) {
        c->connector->CancelOverlappedRequests();
        pending.wait();
        GGML_LOG_ERROR("ND connect timed out\n");
        return false;
    }
    hr = pending.get();
    if (!nd_done(hr, c->connector, &c->ov.ov)) {
        LOG_DBG("ND Connect failed: 0x%08lx\n", (unsigned long)hr);
        return false;
    }
    hr = c->connector->CompleteConnect(&c->ov.ov);
    if (!nd_done(hr, c->connector, &c->ov.ov)) {
        LOG_DBG("ND CompleteConnect failed: 0x%08lx\n", (unsigned long)hr);
        return false;
    }
    LOG_DBG("ND connection established with %s\n", nd_addr_str(c->peer).s);
    c->connected = true;
    return true;
}

bool socket_t::impl::nd_poll(ND2_RESULT * res) {
    nd_conn * c = nd.get();
    uint32_t ready_no_result = 0;
    for (uint64_t s = 0; ; s++) {
        if (c->cq->GetResults(res, 1) == 1) {
            if (FAILED(res->Status)) {
                // the provider reports every error event as a failed send, so the request
                // context is the only reliable way to tell what actually failed
                const int slot = nd_ctx_slot(res->RequestContext);
                GGML_LOG_ERROR("ND completion failed: status=0x%08lx type=%d on %s (bytes=%lu)\n",
                               (unsigned long)res->Status, (int)res->RequestType,
                               res->RequestContext == ND_CTX_SEND ? "send" :
                               (slot >= 0 && slot < ND_RX_DEPTH) ? "recv" : "unknown request",
                               (unsigned long)res->BytesTransferred);
                if (slot >= 0 && slot < ND_RX_DEPTH) {
                    GGML_LOG_ERROR("ND   failing recv slot %d of %d\n", slot, ND_RX_DEPTH);
                }
                return false;
            }
            return true;
        }
        if (s < ND_POLL_SPIN) {
            YieldProcessor();
            continue;
        }
        if (!c->notify_armed) {
            if (ND_STATS) { c->st.n_notify++; }
            HRESULT hr = c->cq->Notify(ND_CQ_NOTIFY_ANY, &c->ov_cq.ov);
            // ND_PENDING has success severity, so it must be tested before SUCCEEDED/FAILED
            if (hr == ND_PENDING) {
                c->notify_armed = true;
            } else if (FAILED(hr)) {
                GGML_LOG_ERROR("ND CQ Notify failed: 0x%08lx\n", (unsigned long)hr);
                return false;
            } else {
                // the queue claims to be non-empty yet yields nothing, so spinning here would
                // never end; bail out rather than burn a core forever
                if (++ready_no_result > ND_MAX_EMPTY_NOTIFY) {
                    GGML_LOG_ERROR("ND CQ reports ready but returns no completion, giving up\n");
                    return false;
                }
                s = 0;
                continue;
            }
        }
        // the provider cannot cancel a pending notify, so keep waiting on the same request
        // and only bound how long we sleep before checking whether the peer is still there
        if (ND_STATS) { c->st.n_kwait++; }
        const DWORD w = WaitForSingleObject(c->ov_cq.ov.hEvent, ND_POLL_TICK_MS);
        if (w == WAIT_TIMEOUT) {
            if (tcp_peer_closed()) {
                LOG_DBG("ND peer dropped the control channel\n");
                return false;
            }
            continue;
        }
        if (w != WAIT_OBJECT_0) {
            GGML_LOG_ERROR("ND CQ wait failed: %lu\n", (unsigned long)GetLastError());
            return false;
        }
        c->notify_armed = false;
        HRESULT hr = c->cq->GetOverlappedResult(&c->ov_cq.ov, FALSE);
        if (FAILED(hr)) {
            GGML_LOG_ERROR("ND CQ notify result failed: 0x%08lx\n", (unsigned long)hr);
            return false;
        }
        s = 0;
    }
}

// consumes exactly one completion and routes it to the send, ack or data path
bool socket_t::impl::nd_step() {
    nd_conn * c = nd.get();
    ND2_RESULT res = {};
    if (!nd_poll(&res)) {
        return false;
    }
    if (res.RequestContext == ND_CTX_SEND) {
        c->send_done++;
        return true;
    }
    const int slot = nd_ctx_slot(res.RequestContext);
    if (slot < 0 || slot >= ND_RX_DEPTH) {
        GGML_LOG_ERROR("ND completion with unknown context\n");
        return false;
    }
    nd_msg_hdr hdr = {};
    memcpy(&hdr, c->rx_slot(slot), sizeof(hdr));
    if (hdr.magic != ND_MSG_MAGIC) {
        GGML_LOG_ERROR("ND framing lost (magic 0x%08x)\n", hdr.magic);
        return false;
    }
    if (hdr.type == ND_MSG_BYE) {
        LOG_DBG("ND peer closed the connection\n");
        c->peer_gone = true;
        return false;
    }
    if (hdr.type == ND_MSG_ACK) {
        c->ack_pending++;
        return c->post_rx(slot);
    }
    if (hdr.type != ND_MSG_DATA || hdr.len == 0 || hdr.len > ND_PAYLOAD) {
        GGML_LOG_ERROR("ND bad message (type=%u len=%u)\n", hdr.type, hdr.len);
        return false;
    }
    c->rx_ready.push_back({ slot, hdr.len, 0 });
    return true;
}

bool socket_t::impl::nd_post_send(size_t len) {
    nd_conn * c = nd.get();
    ND2_SGE sge = {};
    sge.Buffer            = c->tx_buf;
    sge.BufferLength      = (ULONG)len;
    sge.MemoryRegionToken = c->tx_token;

    c->send_done = 0;
    const uint64_t t0 = ND_STATS ? nd_now_us() : 0;
    if (FAILED(c->qp->Send(ND_CTX_SEND, &sge, 1, 0))) {
        GGML_LOG_ERROR("ND Send post failed\n");
        return false;
    }
    const uint64_t t1 = ND_STATS ? nd_now_us() : 0;
    while (c->send_done == 0) {
        if (!nd_step()) {
            return false;
        }
    }
    if (ND_STATS) {
        c->st.post_us  += t1 - t0;
        c->st.cmpl_us  += nd_now_us() - t1;
        c->st.bytes_tx += len;
    }
    return true;
}

bool socket_t::impl::nd_send_ack() {
    nd_conn * c = nd.get();
    const nd_msg_hdr hdr = { ND_MSG_MAGIC, ND_MSG_ACK, 0, c->tx_seq++ };
    memcpy(c->tx_buf, &hdr, sizeof(hdr));
    if (ND_STATS) { c->st.n_ack++; }
    return nd_post_send(sizeof(hdr));
}

// The provider raises no completion and no notification when a peer disconnects, so an
// explicit bye is what lets the other side leave its completion queue wait promptly.
// It rides in the slot ND_WINDOW keeps spare, so it needs no flow control of its own.
bool socket_t::impl::nd_send_bye() {
    nd_conn * c = nd.get();
    const nd_msg_hdr hdr = { ND_MSG_MAGIC, ND_MSG_BYE, 0, c->tx_seq++ };
    memcpy(c->tx_buf, &hdr, sizeof(hdr));
    if (ND_STATS) { c->st.n_ack++; }
    return nd_post_send(sizeof(hdr));
}

bool socket_t::impl::nd_flush() {
    nd_conn * c = nd.get();
    if (c->tx_fill == 0) {
        return true;
    }
    if (c->tx_since_ack >= ND_WINDOW) {
        const uint64_t t0 = ND_STATS ? nd_now_us() : 0;
        while (c->ack_pending == 0) {
            if (!nd_step()) {
                return false;
            }
        }
        if (ND_STATS) { c->st.ackw_us += nd_now_us() - t0; }
        c->ack_pending--;
        c->tx_since_ack = 0;
    }
    const nd_msg_hdr hdr = { ND_MSG_MAGIC, ND_MSG_DATA, (uint32_t)c->tx_fill, c->tx_seq++ };
    memcpy(c->tx_buf, &hdr, sizeof(hdr));
    const size_t len = sizeof(hdr) + c->tx_fill;
    c->tx_fill = 0;
    if (ND_STATS) { c->st.n_send++; }
    if (!nd_post_send(len)) {
        return false;
    }
    c->tx_since_ack++;
    if (ND_STATS && (c->st.n_send + c->st.n_ack) - (c->st_prev.n_send + c->st_prev.n_ack) >= ND_STATS) {
        c->dump_stats(is_server ? "server" : "client");
    }
    return true;
}

bool socket_t::impl::nd_send(const void * data, size_t size) {
    nd_conn * c = nd.get();
    const uint8_t * src = (const uint8_t *)data;
    size_t rem = size;
    while (rem > 0) {
        if (c->tx_fill == ND_PAYLOAD && !nd_flush()) {
            return false;
        }
        const size_t take = std::min(rem, ND_PAYLOAD - c->tx_fill);
        memcpy(c->tx_buf + sizeof(nd_msg_hdr) + c->tx_fill, src, take);
        c->tx_fill += take;
        src += take;
        rem -= take;
    }
    if (!c->corked) {
        return nd_flush();
    }
    return true;
}

bool socket_t::impl::nd_repost_rx() {
    nd_conn * c = nd.get();
    while (!c->rx_repost.empty()) {
        const int slot = c->rx_repost.back();
        c->rx_repost.pop_back();
        if (!c->post_rx(slot)) {
            return false;
        }
    }
    return true;
}

bool socket_t::impl::nd_recv(void * data, size_t size) {
    nd_conn * c = nd.get();
    // a staged request has to reach the peer before we block waiting for its reply
    if (!nd_flush()) {
        return false;
    }
    uint8_t * dst = (uint8_t *)data;
    size_t rem = size;
    while (rem > 0) {
        // never block without handing the peer its buffers back first
        if (c->rx_ready.empty() && !nd_repost_rx()) {
            return false;
        }
        uint32_t idle_steps = 0;
        const uint64_t t0 = (ND_STATS && c->rx_ready.empty()) ? nd_now_us() : 0;
        while (c->rx_ready.empty()) {
            if (!nd_step()) {
                return false;
            }
            // completions that never turn into payload mean the queue is replaying
            // something stale; without this the loop would spin until the peer gives up
            if (++idle_steps > ND_MAX_IDLE_STEPS) {
                GGML_LOG_ERROR("ND consumed %u completions without receiving data\n", idle_steps);
                return false;
            }
        }
        if (ND_STATS && t0) { c->st.recv_us += nd_now_us() - t0; }
        nd_rx_item & it = c->rx_ready.front();
        const size_t take = std::min((size_t)(it.len - it.off), rem);
        memcpy(dst, c->rx_slot(it.slot) + sizeof(nd_msg_hdr) + it.off, take);
        dst += take;
        rem -= take;
        it.off += (uint32_t)take;
        if (ND_STATS) { c->st.bytes_rx += take; }
        if (it.off == it.len) {
            const int slot = it.slot;
            c->rx_ready.pop_front();
            c->rx_repost.push_back(slot);
            if (++c->rx_since_ack >= ND_WINDOW) {
                c->rx_since_ack = 0;
                // the ack hands credit back, so the buffers must be posted before it goes out
                if (!nd_repost_rx()) {
                    return false;
                }
                if (!nd_send_ack()) {
                    return false;
                }
            }
        }
    }
    return true;
}

#endif // GGML_RPC_ND

bool socket_t::impl::send_data(const void * data, size_t size) {
#ifdef GGML_RPC_RDMA
    if (use_rdma) {
        return rdma_send(data, size);
    }
#endif
#ifdef GGML_RPC_ND
    if (use_nd) {
        return nd_send(data, size);
    }
#endif
    size_t bytes_sent = 0;
    while (bytes_sent < size) {
        size_t size_to_send = std::min(size - bytes_sent, MAX_CHUNK_SIZE);
        ssize_t n = send(fd, (const char *)data + bytes_sent, size_to_send, 0);
        if (n < 0) {
            GGML_LOG_ERROR("send failed (bytes_sent=%zu, size_to_send=%zu)\n",
                           bytes_sent, size_to_send);
            return false;
        }
        bytes_sent += (size_t)n;
    }
    return true;
}

bool socket_t::impl::recv_data(void * data, size_t size) {
#ifdef GGML_RPC_RDMA
    if (use_rdma) {
        return rdma_recv(data, size);
    }
#endif
#ifdef GGML_RPC_ND
    if (use_nd) {
        return nd_recv(data, size);
    }
#endif
    size_t bytes_recv = 0;
    while (bytes_recv < size) {
        size_t size_to_recv = std::min(size - bytes_recv, MAX_CHUNK_SIZE);
        ssize_t n = recv(fd, (char *)data + bytes_recv, size_to_recv, 0);
        if (n < 0) {
            GGML_LOG_ERROR("recv failed (bytes_recv=%zu, size_to_recv=%zu)\n",
                           bytes_recv, size_to_recv);
            return false;
        }
        if (n == 0) {
            LOG_DBG("recv returned 0 (peer closed?)\n");
            return false;
        }
        bytes_recv += (size_t)n;
    }
    return true;
}

void socket_t::impl::cork() {
#ifdef GGML_RPC_ND
    if (use_nd) {
        nd->corked++;
    }
#endif
}

bool socket_t::impl::uncork() {
#ifdef GGML_RPC_ND
    if (use_nd) {
        if (nd->corked > 0 && --nd->corked == 0) {
            return nd_flush();
        }
    }
#endif
    return true;
}

void socket_t::impl::get_caps(uint8_t * local_caps) {
    memset(local_caps, 0, RPC_CONN_CAPS_SIZE);
#ifdef GGML_RPC_RDMA
    rdma_local = {};
    if (rdma_probe()) {
        rdma_caps rc = {};
        rc.qpn = rdma_local.qpn;
        rc.psn = rdma_local.psn;
        memcpy(rc.gid, rdma_local.gid, RDMA_GID_SIZE);
        memcpy(local_caps, &rc, sizeof(rc));
    } else {
        rdma.reset();
    }
#endif // GGML_RPC_RDMA
#ifdef GGML_RPC_ND
    struct sockaddr_in addr = {};
    LOG_DBG("ND negotiating as %s\n", is_server ? "server" : "client");
    if (!nd_setup_any(&addr)) {
        LOG_DBG("ND no usable adapter, offering TCP only\n");
        nd.reset();
        return;
    }
    // only the server advertises an address; the client just signals that it can dial in
    if (is_server && !nd_listen()) {
        nd.reset();
        return;
    }
    nd_caps nc = {};
    nc.magic = ND_CAPS_MAGIC;
    nc.ipv4  = is_server ? (uint32_t)addr.sin_addr.s_addr : 0;
    memcpy(local_caps, &nc, sizeof(nc));
    LOG_DBG("ND offering NetworkDirect (advertised address %s)\n",
            is_server ? nd_addr_str(addr).s : "none, client dials out");
#endif // GGML_RPC_ND
}

void socket_t::impl::update_caps(const uint8_t * remote_caps) {
#ifdef GGML_RPC_RDMA
    if (!rdma) {
        return;
    }
    rdma_caps rc = {};
    memcpy(&rc, remote_caps, sizeof(rc));
    if (rc.qpn == 0) {
        rdma.reset();
        return;
    }
    if (rdma_activate(rc.qpn, rc.psn, rc.gid)) {
        use_rdma = true;
    } else {
        GGML_LOG_ERROR("RDMA activate failed, staying on TCP\n");
        rdma.reset();
    }
#elif defined(GGML_RPC_ND)
    if (!nd) {
        return;
    }
    nd_caps nc = {};
    memcpy(&nc, remote_caps, sizeof(nc));
    if (nc.reserved0 != 0 || nc.magic != ND_CAPS_MAGIC) {
        LOG_DBG("ND peer did not offer NetworkDirect, staying on TCP\n");
        nd.reset();
        return;
    }
    bool ok;
    if (is_server) {
        ok = nd_accept();
    } else {
        struct sockaddr_in peer_addr = {};
        peer_addr.sin_family      = AF_INET;
        peer_addr.sin_addr.s_addr = nc.ipv4;
        ok = nc.ipv4 != 0 && nd_connect(peer_addr);
    }
    if (ok) {
        use_nd = true;
        GGML_LOG_INFO("NetworkDirect activated: window=%d rx_depth=%d chunk=%zu KiB\n",
                      ND_WINDOW, ND_RX_DEPTH, ND_PAYLOAD / 1024);
    } else {
        GGML_LOG_ERROR("NetworkDirect setup failed, staying on TCP\n");
        nd.reset();
    }
#else
    (void)remote_caps;
#endif // GGML_RPC_RDMA
}


/////////////////////////////////////////////////////////////////////////////

socket_t::socket_t(std::unique_ptr<impl> p) : pimpl(std::move(p)) {}

socket_t::~socket_t() = default;

bool socket_t::send_data(const void * data, size_t size) {
    return pimpl->send_data(data, size);
}

bool socket_t::recv_data(void * data, size_t size) {
    return pimpl->recv_data(data, size);
}

void socket_t::cork() {
    pimpl->cork();
}

bool socket_t::uncork() {
    return pimpl->uncork();
}

void socket_t::get_caps(uint8_t * local_caps) {
    return pimpl->get_caps(local_caps);
}

void socket_t::update_caps(const uint8_t * remote_caps) {
    return pimpl->update_caps(remote_caps);
}

static bool is_valid_fd(sockfd_t sockfd) {
#ifdef _WIN32
    return sockfd != INVALID_SOCKET;
#else
    return sockfd >= 0;
#endif
}

static bool set_no_delay(sockfd_t sockfd) {
    int flag = 1;
    // set TCP_NODELAY to disable Nagle's algorithm
    int ret = setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int));
    return ret == 0;
}

static bool set_reuse_addr(sockfd_t sockfd) {
    int flag = 1;
    int ret = setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (char *)&flag, sizeof(int));
    return ret == 0;
}

socket_ptr socket_t::accept() {
    auto client_socket_fd = ::accept(pimpl->fd, NULL, NULL);
    if (!is_valid_fd(client_socket_fd)) {
        return nullptr;
    }
    if (!set_no_delay(client_socket_fd)) {
        GGML_LOG_ERROR("Failed to set TCP_NODELAY\n");
        return nullptr;
    }
    return socket_ptr(new socket_t(std::make_unique<impl>(client_socket_fd, /*is_server =*/ true)));
}

socket_ptr socket_t::create_server(const char * host, int port) {
    auto sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (!is_valid_fd(sockfd)) {
        return nullptr;
    }
    if (!set_reuse_addr(sockfd)) {
        GGML_LOG_ERROR("Failed to set SO_REUSEADDR\n");
        return nullptr;
    }
    if (inet_addr(host) == INADDR_NONE) {
        GGML_LOG_ERROR("Invalid host address: %s\n", host);
        return nullptr;
    }
    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = inet_addr(host);
    serv_addr.sin_port = htons(port);

    if (bind(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) {
        return nullptr;
    }
    if (listen(sockfd, 1) < 0) {
        return nullptr;
    }
    return socket_ptr(new socket_t(std::make_unique<impl>(sockfd)));
}

socket_ptr socket_t::connect(const char * host, int port) {
    auto sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (!is_valid_fd(sockfd)) {
        return nullptr;
    }
    if (!set_no_delay(sockfd)) {
        GGML_LOG_ERROR("Failed to set TCP_NODELAY\n");
        return nullptr;
    }
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    struct hostent * server = gethostbyname(host);
    if (server == NULL) {
        GGML_LOG_ERROR("Cannot resolve host '%s'\n", host);
        return nullptr;
    }
    memcpy(&addr.sin_addr.s_addr, server->h_addr, server->h_length);
    if (::connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        return nullptr;
    }
    return socket_ptr(new socket_t(std::make_unique<impl>(sockfd)));
}

#ifdef _WIN32
static std::mutex g_rpc_transport_mu;
static bool g_rpc_transport_wsa_started = false;
#endif

bool rpc_transport_init() {
#ifdef _WIN32
    std::lock_guard<std::mutex> lock(g_rpc_transport_mu);
    if (g_rpc_transport_wsa_started) {
        return true;
    }
    WSADATA wsaData;
    int res = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (res != 0) {
        return false;
    }
#ifdef GGML_RPC_ND
    HRESULT hr = NdStartup();
    if (FAILED(hr)) {
        GGML_LOG_ERROR("NdStartup failed: 0x%08lx, NetworkDirect unavailable\n", (unsigned long)hr);
    } else {
        LOG_DBG("NdStartup ok\n");
    }
#endif
    g_rpc_transport_wsa_started = true;
    return true;
#else
    return true;
#endif
}

void rpc_transport_shutdown() {
#ifdef _WIN32
    std::lock_guard<std::mutex> lock(g_rpc_transport_mu);
    if (!g_rpc_transport_wsa_started) {
        return;
    }
#ifdef GGML_RPC_ND
    NdCleanup();
#endif
    WSACleanup();
    g_rpc_transport_wsa_started = false;
#endif
}
