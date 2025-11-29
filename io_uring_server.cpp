/*
 * FINAL PRODUCTION IO_URING SERVER
 * Architecture: Worker-per-Core, Shared-Nothing, Direct Descriptors
 * Requirements: Linux Kernel 6.1+, liburing 2.2+
 * Compile: g++ -O3 -march=native server.cpp -luring -lpthread -o server
 */

#define _GNU_SOURCE
#include <liburing.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <errno.h>
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <vector>
#include <iostream>
#include <algorithm>

// --- Configuration ---
#define MAX_CONNECTIONS 32768
#define RING_DEPTH      32768
#define BUF_RING_SZ     16384   // Must be power of 2
#define BLOCK_SZ        4096
#define BATCH_SIZE      128
#define BGID            1       // Buffer Group ID

// Magic Constants
#define NULL_IDX        0xFFFFFFFF
#define LISTENER_TAG    0xDEADBEEF00000000ULL

#define likely(x)       __builtin_expect((!!(x)), 1)
#define unlikely(x)     __builtin_expect((!!(x)), 0)

static_assert((BUF_RING_SZ & (BUF_RING_SZ - 1)) == 0, "Buffer Ring must be Power of 2");

// --- Structures ---

struct WriteReq {
    uint32_t next; // Freelist index
    uint16_t bid;
    uint16_t len;
    uint16_t written;
};

enum ConnState : uint8_t { 
    ST_FREE = 0, 
    ST_ESTABLISHED, 
    ST_CLOSING 
};

// 64-Byte Cache Aligned Connection
struct alignas(64) Connection {
    int32_t  fixed_idx;     // Kernel Fixed File Table Index
    uint32_t gen_id;        // Generation ID to detect stale events
    
    uint32_t w_head;        // Write Queue Head (Index)
    uint32_t w_tail;        // Write Queue Tail (Index)
    
    ConnState state;
    bool recv_active;       // Is a Multishot Recv currently armed?
    
    char pad[40];           // Padding to fill cache line
};

// --- Memory Helper ---

void* alloc_huge(size_t size) {
    // Try Hugepages first
    void* ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, 
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    
    if (ptr == MAP_FAILED) {
        // Fallback to normal pages + MADV_HUGEPAGE advice
        ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, 
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (ptr == MAP_FAILED) {
            perror("mmap failed");
            exit(1);
        }
        madvise(ptr, size, MADV_HUGEPAGE);
    }
    memset(ptr, 0, size); // Fault pages immediately
    return ptr;
}

// --- Request Pool (Simple Slab) ---

class ReqPool {
    WriteReq* base;
    uint32_t free_head;
    size_t count;

public:
    void init() {
        count = MAX_CONNECTIONS * 16; // 16 buffers per conn approx
        size_t size = count * sizeof(WriteReq);
        base = (WriteReq*)alloc_huge(size);
        
        for (uint32_t i = 0; i < count - 1; ++i) {
            base[i].next = i + 1;
        }
        base[count - 1].next = NULL_IDX;
        free_head = 0;
    }

    WriteReq* alloc(uint32_t& out_idx) {
        if (unlikely(free_head == NULL_IDX)) return nullptr;
        out_idx = free_head;
        WriteReq* req = &base[out_idx];
        free_head = req->next;
        req->next = NULL_IDX;
        return req;
    }

    void free(uint32_t idx) {
        base[idx].next = free_head;
        free_head = idx;
    }

    WriteReq* get(uint32_t idx) {
        return &base[idx];
    }
};

// --- Worker Context ---

struct Engine {
    int cpu_id;
    struct io_uring ring;
    
    struct io_uring_buf_ring* br;
    unsigned char* buf_base;
    
    // Pools
    Connection* conns; // Array mapped 1:1 to Fixed File Table
    ReqPool reqs;
};

// --- Buffer Ring Helpers ---

inline void buf_add(Engine* ctx, uint16_t bid) {
    // New liburing API standard
    io_uring_buf_ring_add(ctx->br, 
                          ctx->buf_base + ((size_t)bid * BLOCK_SZ), 
                          BLOCK_SZ, 
                          bid, 
                          io_uring_buf_ring_mask(BUF_RING_SZ), 
                          0);
}

inline void buf_commit(Engine* ctx, int count) {
    if (count > 0) {
        io_uring_buf_ring_advance(ctx->br, count);
    }
}

// --- IO Helpers ---

// Tags: High 32 bits = GenID, Low 32 bits = Fixed Index
inline uint64_t make_tag(uint32_t idx, uint32_t gen) {
    return ((uint64_t)gen << 32) | idx;
}

inline void op_accept(Engine* ctx, int listener_fd) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ctx->ring);
    // Multishot Accept + Direct File Index Allocation
    // Kernel will pick a free slot in the registered table and return it in CQE res.
    io_uring_prep_multishot_accept_direct(sqe, listener_fd, NULL, NULL, 0);
    io_uring_sqe_set_data64(sqe, LISTENER_TAG);
}

inline void op_recv(Engine* ctx, Connection* c) {
    if (c->recv_active) return;
    
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ctx->ring);
    // Multishot Recv using Fixed File
    io_uring_prep_recv_multishot(sqe, c->fixed_idx, NULL, 0, 0);
    sqe->flags |= IOSQE_FIXED_FILE;
    sqe->flags |= IOSQE_BUFFER_SELECT;
    sqe->buf_group = BGID;
    
    io_uring_sqe_set_data64(sqe, make_tag(c->fixed_idx, c->gen_id));
    c->recv_active = true;
}

inline void op_send(Engine* ctx, Connection* c) {
    if (c->w_head == NULL_IDX) return;
    
    WriteReq* req = ctx->reqs.get(c->w_head);
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ctx->ring);
    
    char* addr = (char*)ctx->buf_base + ((size_t)req->bid * BLOCK_SZ) + req->written;
    unsigned len = req->len - req->written;
    
    // Zero-Copy Send using Fixed File
    io_uring_prep_send_zc(sqe, c->fixed_idx, addr, len, MSG_NOSIGNAL, 0);
    sqe->flags |= IOSQE_FIXED_FILE;
    
    io_uring_sqe_set_data64(sqe, make_tag(c->fixed_idx, c->gen_id));
}

inline void op_close(Engine* ctx, Connection* c) {
    if (c->state == ST_FREE) return;
    
    // 1. Tell kernel to remove from fixed table
    struct io_uring_sqe* sqe = io_uring_get_sqe(&ctx->ring);
    io_uring_prep_close_direct(sqe, c->fixed_idx);
    io_uring_sqe_set_data64(sqe, 0); // Fire and forget
    
    // 2. Cleanup Write Queue buffers
    uint32_t curr = c->w_head;
    int recycled = 0;
    while (curr != NULL_IDX) {
        WriteReq* req = ctx->reqs.get(curr);
        buf_add(ctx, req->bid);
        recycled++;
        
        uint32_t to_free = curr;
        curr = req->next;
        ctx->reqs.free(to_free);
    }
    buf_commit(ctx, recycled);
    
    // 3. Reset Connection Slot
    c->state = ST_FREE;
    c->w_head = NULL_IDX;
    c->w_tail = NULL_IDX;
    c->recv_active = false;
    // Note: We don't "free" the connection object because it's tied 1:1 to the
    // fixed index table. The slot is now available for the kernel to reuse 
    // via IORING_FILE_INDEX_ALLOC.
}

// --- Main Engine ---

void worker_thread(int cpu_id) {
    // 1. Pinning
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

    // 2. Context Init
    Engine ctx;
    ctx.cpu_id = cpu_id;
    ctx.conns = (Connection*)alloc_huge(MAX_CONNECTIONS * sizeof(Connection));
    ctx.reqs.init();
    
    // Initialize Connection array
    for(int i=0; i<MAX_CONNECTIONS; ++i) {
        ctx.conns[i].fixed_idx = i;
        ctx.conns[i].gen_id = 1;
        ctx.conns[i].state = ST_FREE;
        ctx.conns[i].w_head = NULL_IDX;
        ctx.conns[i].recv_active = false;
    }

    // 3. Setup io_uring
    struct io_uring_params p = {};
    p.flags = IORING_SETUP_SQPOLL | IORING_SETUP_SingleIssuer | 
              IORING_SETUP_COOP_TASKRUN | IORING_SETUP_DEFER_TASKRUN;
    p.sq_thread_idle = 2000;

    if (io_uring_queue_init_params(RING_DEPTH, &ctx.ring, &p) < 0) {
        p.flags &= ~IORING_SETUP_SQPOLL; // Fallback
        if (io_uring_queue_init_params(RING_DEPTH, &ctx.ring, &p) < 0) {
            perror("io_uring_init"); exit(1);
        }
    }

    // 4. Register Sparse File Table
    // This allows IORING_FILE_INDEX_ALLOC to work within range [0, MAX_CONNECTIONS)
    if (io_uring_register_files_sparse(&ctx.ring, MAX_CONNECTIONS) < 0) {
        perror("register_files_sparse"); exit(1);
    }

    // 5. Setup Buffer Ring
    size_t rsz = sizeof(struct io_uring_buf_ring) + BUF_RING_SZ * sizeof(struct io_uring_buf);
    ctx.br = (struct io_uring_buf_ring*)alloc_huge(rsz);
    
    size_t dsz = (size_t)BUF_RING_SZ * BLOCK_SZ;
    ctx.buf_base = (unsigned char*)alloc_huge(dsz);

    struct io_uring_buf_reg reg = {
        .ring_addr = (unsigned long)ctx.br,
        .ring_entries = BUF_RING_SZ,
        .bgid = BGID
    };
    if (io_uring_register_buf_ring(&ctx.ring, &reg, 0) < 0) {
        perror("buf_ring register"); exit(1);
    }
    io_uring_buf_ring_init(ctx.br);
    for (int i=0; i<BUF_RING_SZ; ++i) buf_add(&ctx, i);
    buf_commit(&ctx, BUF_RING_SZ);

    // 6. Listener Setup
    int sfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    int opt = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
    setsockopt(sfd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
    
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8080);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    
    if (bind(sfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) exit(1);
    if (listen(sfd, MAX_CONNECTIONS) < 0) exit(1);

    // Start
    op_accept(&ctx, sfd);
    io_uring_submit(&ctx.ring);

    printf("Worker %d running.\n", cpu_id);

    struct io_uring_cqe* cqes[BATCH_SIZE];

    while(true) {
        int ret = io_uring_submit_and_wait(&ctx.ring, 1);
        if (unlikely(ret < 0)) continue;

        unsigned count = io_uring_peek_batch_cqe(&ctx.ring, cqes, BATCH_SIZE);
        int bufs_recycled = 0;

        for (unsigned i = 0; i < count; ++i) {
            struct io_uring_cqe* cqe = cqes[i];
            uint64_t ud = cqe->user_data;
            int res = cqe->res;

            // --- ACCEPT HANDLER ---
            if (unlikely(ud == LISTENER_TAG)) {
                if (likely(res >= 0)) {
                    // res is the Fixed File Index allocated by Kernel
                    int idx = res;
                    if (idx < MAX_CONNECTIONS) {
                        Connection* c = &ctx.conns[idx];
                        // If slot was not free, we have a logic error or state desync
                        if (c->state == ST_FREE) {
                            c->state = ST_ESTABLISHED;
                            c->gen_id++; // Bump generation
                            c->w_head = NULL_IDX;
                            c->w_tail = NULL_IDX;
                            c->recv_active = false;
                            
                            op_recv(&ctx, c);
                        } else {
                            // Slot collision (should not happen with sparse alloc)
                            // Force close to cleanup kernel state
                            struct io_uring_sqe* s = io_uring_get_sqe(&ctx.ring);
                            io_uring_prep_close_direct(s, idx);
                            io_uring_sqe_set_data64(s, 0);
                        }
                    } else {
                        // Index out of bounds (Table full?)
                        struct io_uring_sqe* s = io_uring_get_sqe(&ctx.ring);
                        io_uring_prep_close_direct(s, idx);
                        io_uring_sqe_set_data64(s, 0);
                    }
                }
                // Multishot accept doesn't need re-arm unless error/flag dropped
                if (!(cqe->flags & IORING_CQE_F_MORE)) {
                    op_accept(&ctx, sfd);
                }
            }
            // --- IO HANDLER ---
            else if (ud != 0) {
                uint32_t idx = (uint32_t)ud;
                uint32_t gen = (uint32_t)(ud >> 32);
                
                Connection* c = &ctx.conns[idx];

                // Stale check
                if (c->state == ST_FREE || c->gen_id != gen) {
                    // This is an event for a closed connection, ignore
                    io_uring_cqe_seen(&ctx.ring, cqe);
                    continue;
                }

                // Error
                if (unlikely(res < 0)) {
                    if (res != -EAGAIN && res != -ENOBUFS) op_close(&ctx, c);
                }
                // Recv
                else if (cqe->flags & IORING_CQE_F_BUFFER) {
                    uint16_t bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
                    if (res == 0) { // EOF
                        buf_add(&ctx, bid); bufs_recycled++;
                        op_close(&ctx, c);
                    } else {
                        // Echo Logic
                        uint32_t req_idx;
                        WriteReq* req = ctx.reqs.alloc(req_idx);
                        if (req) {
                            req->bid = bid;
                            req->len = res;
                            req->written = 0;
                            
                            if (c->w_tail != NULL_IDX) {
                                ctx.reqs.get(c->w_tail)->next = req_idx;
                            } else {
                                c->w_head = req_idx;
                                op_send(&ctx, c);
                            }
                            c->w_tail = req_idx;
                        } else {
                            // Drop packet
                            buf_add(&ctx, bid); bufs_recycled++;
                        }
                    }
                    
                    if (!(cqe->flags & IORING_CQE_F_MORE) && c->state != ST_FREE) {
                        c->recv_active = false;
                        op_recv(&ctx, c);
                    }
                }
                // Send
                else {
                    if (c->w_head != NULL_IDX) {
                        WriteReq* req = ctx.reqs.get(c->w_head);
                        req->written += res;
                        
                        if (req->written >= req->len) {
                            buf_add(&ctx, req->bid); bufs_recycled++;
                            
                            uint32_t old_head = c->w_head;
                            c->w_head = req->next;
                            if (c->w_head == NULL_IDX) c->w_tail = NULL_IDX;
                            
                            ctx.reqs.free(old_head);
                            
                            op_send(&ctx, c);
                        } else {
                            op_send(&ctx, c); // Partial write
                        }
                    }
                }
            }
            io_uring_cqe_seen(&ctx.ring, cqe);
        }
        buf_commit(&ctx, bufs_recycled);
    }
}

int main() {
    struct rlimit rl = { MAX_CONNECTIONS * 2, MAX_CONNECTIONS * 2 };
    setrlimit(RLIMIT_NOFILE, &rl);

    int cores = sysconf(_SC_NPROCESSORS_ONLN);
    std::vector<pid_t> pids;

    for (int i = 0; i < cores; ++i) {
        pid_t pid = fork();
        if (pid == 0) {
            worker_thread(i);
            return 0;
        }
        pids.push_back(pid);
    }

    while (wait(NULL) > 0);
    return 0;
}
