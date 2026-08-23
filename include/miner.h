/*
 * Shared public types and runtime interfaces for Ghuleh Miner
 * (a fork of Primo ARM Miner).
 * Copyright (C) 2026 primo-arm-miner contributors.
 *
 * This header stays C-compatible because the mining back ends span
 * C and C++ translation units.
 * Substantially rewritten on 2026-03-16 from earlier GPL-licensed mining
 * software ancestry. See LICENSE and PROVENANCE.md.
 */

#ifndef MINER_H
#define MINER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include <curl/curl.h>
#include <jansson.h>
#include <pthread.h>

// Version
#define PACKAGE_NAME "ghuleh-miner"
#define PACKAGE_VERSION "1.1.0"
#define USER_AGENT PACKAGE_NAME "/" PACKAGE_VERSION

// Limits
// MAX_POOLS reserves one slot above the user-configurable count for the
// hidden dev fee pool (see dev_fee.h).
#define MAX_USER_POOLS 8
#define MAX_POOLS (MAX_USER_POOLS + 1)
#define MAX_THREADS 32
#define MAX_NONCES 2
#define MAX_PENDING_SUBMITS 256
#define VERUS_WORK_EXTRA_SIZE 1388
#define VERUS_WORK_SOLUTION_SIZE 1344

#if MAX_THREADS > UINT8_MAX
#error "MAX_THREADS must fit in work.thread_id"
#endif

// Color codes
#define CL_N    "\x1B[0m"
#define CL_RED  "\x1B[31m"
#define CL_GRN  "\x1B[32m"
#define CL_YLW  "\x1B[33m"
#define CL_BLU  "\x1B[34m"
#define CL_CYN  "\x1B[36m"
#define CL_GRY  "\x1B[90m"
#define CL_WHT  "\x1B[01;37m"

// Logging priorities used by applog().
enum {
    LOG_ERR = 0,
    LOG_WARNING,
    LOG_NOTICE,
    LOG_INFO,
    LOG_DEBUG
};

struct verus_work_payload {
    uint8_t extra[VERUS_WORK_EXTRA_SIZE];
    uint8_t solution[VERUS_WORK_SOLUTION_SIZE];
};

// Internal work structure shared by the mining and stratum layers.
struct work {
    // Hash input and target data presented to the scan loop. 68 words:
    // bytes 0-255 hold the largest RandomX-fork blob (RANDOMX_BLOB_MAX;
    // Zephyr's is 220 bytes — not just the 80/112-byte BTC/Verus headers),
    // and word 64 (bytes 256-259) is the RandomX nonce COUNTER, which must
    // live beyond the blob region (RANDOMX_NONCE_WORD).
    uint32_t data[68];
    uint32_t target[8];

    // Job identity and extranonce state copied from stratum.
    char job_id[128];
    size_t xnonce2_len;
    uint8_t xnonce2[32];

    // Share submission bookkeeping for algorithms that can return >1 nonce.
    uint8_t pooln;
    uint8_t valid_nonces;
    uint8_t submit_nonce_id;
    uint8_t thread_id;

    uint32_t nonces[MAX_NONCES];
    double sharediff[MAX_NONCES];
    double targetdiff;

    uint32_t height;
    uint32_t restart_generation;

    // RandomX (Monero): length of the pool blob at the head of data[]
    // (0 = default 76) and the full result hash per found nonce — Monero
    // submits the 32-byte hash, not just the nonce.
    uint16_t rx_blob_len;
    uint8_t rx_hash[MAX_NONCES][32];
    // Which bits of the 4-byte blob nonce this miner owns. 0 = the whole
    // 32-bit space (normal pools). In nicehash mode the pool/proxy owns the
    // top byte — it pre-sets blob byte 42 to hand each connected worker its
    // own slice — so the mask is RANDOMX_NICEHASH_NONCE_MASK and the bits
    // outside it must be preserved exactly as the job delivered them.
    uint32_t rx_nonce_mask;

    // Algorithm-specific scratch/state is attached only when required.
    struct verus_work_payload *verus;
};

// Stratum job structure
struct stratum_job {
    char *job_id;
    unsigned char prevhash[32];
    size_t coinbase_size;
    unsigned char *coinbase;
    unsigned char *xnonce2;
    int merkle_count;
    unsigned char **merkle;
    unsigned char version[4];
    unsigned char nbits[4];
    unsigned char ntime[4];
    uint32_t height;
    double diff;
    unsigned char solution[VERUS_WORK_SOLUTION_SIZE];

    // RandomX (Monero) job fields: raw hashing blob and the expanded 64-bit
    // compact target. The seed_hash itself isn't stored here - it only ever
    // needs to reach randomx_set_seed() (xmr_stratum_handle_job() calls that
    // directly off the freshly-parsed job view, before this struct is even
    // updated), so there was never a consumer for a copy of it in the job.
    unsigned char rx_blob[256];   /* >= RANDOMX_BLOB_MAX */
    uint16_t rx_blob_len;
    uint64_t rx_target64;
};

// Pending share submit metadata keyed by JSON-RPC id
struct pending_submit {
    uint32_t id;
    double sharediff;
    int thread_id;
    time_t queued_at;
    bool used;
};

// Per-pool stratum session state.
struct stratum_ctx {
    char *url;
    CURL *curl;
    char *curl_url;
    /* CURLOPT_RESOLVE entries from the DNS fallback resolver (see
     * dns_fallback.h); must outlive the easy handle, freed on close. */
    struct curl_slist *resolve_list;
    char curl_err_str[CURL_ERROR_SIZE];
    curl_socket_t sock;
    size_t sockbuf_size;
    char *sockbuf;

    double next_diff;

    char *session_id;
    size_t xnonce1_size;
    unsigned char *xnonce1;
    size_t xnonce2_size;
    struct stratum_job job;

    int pooln;
    int is_verus_protocol;
    /* RandomX: pool advertised the "nicehash" extension in its login reply
     * (or --nicehash forced it), so it owns the nonce MSB. Re-evaluated on
     * every login — a reconnect or a switch to a pool without the extension
     * must not inherit the previous pool's mode. */
    int xmr_nicehash;
    /* TLS session (stratum+ssl://): I/O goes through curl_easy_send/recv on
     * the CONNECT_ONLY handle instead of raw send/recv on the socket. Set
     * from the URL scheme on every connect (stratum_build_curl_url). */
    int use_tls;
    int srvtime_diff;
    int authenticated;  // Set to 1 after successful authorization
    int reconnect_requested;

    // Thread-safe submit/response correlation
    pthread_mutex_t submit_lock;
    uint32_t next_submit_id;
    struct pending_submit pending_submits[MAX_PENDING_SUBMITS];
    int thread_active;
};

// Pool configuration, connection state, and share counters.
struct pool_infos {
    bool configured;
    bool disabled;
    int timeout;

    char name[64];
    char url[512];
    char short_url[64];
    char user[192];
    char pass[384];

    struct stratum_ctx stratum;

    uint32_t accepted_count;
    uint32_t rejected_count;
    time_t last_share_time;
    double best_share;
};

// Mining-thread runtime state.
struct thr_info {
    int id;
    pthread_t pth;
    double hashrate;
    uint64_t hashes_done_total;
    uint32_t accepted;
    uint32_t rejected;
};

// Read-only runtime snapshot used by the compatibility API server.
struct miner_thread_api_stats {
    double hashrate;
    uint64_t hashes_done_total;
    uint32_t accepted;
    uint32_t rejected;
    int cpu_id;
    int cpu_max_freq_mhz;
    bool cpu_is_big;
};

struct miner_api_snapshot {
    bool runtime_active;
    time_t start_time;
    double global_hashrate;
    uint64_t total_hashes_done;
    int cpu_temp;
    int thread_count;
    struct miner_thread_api_stats threads[MAX_THREADS];
};

// Global options
extern bool opt_debug;
extern bool opt_quiet;
extern bool opt_benchmark;
extern bool opt_protocol;
/* RandomX: force nicehash nonce mode even if the pool does not advertise the
 * "nicehash" extension. Normally auto-detected at login — see
 * RANDOMX_NICEHASH_NONCE_MASK. */
extern bool opt_nicehash;
extern int opt_n_threads;
extern int opt_timeout;
extern int opt_retries;
extern int opt_retry_pause;
extern char *opt_api_bind;
extern int opt_api_port;
extern bool opt_api_port_explicit;
extern int opt_statsavg;
extern int opt_priority;
extern bool opt_affinity_set;
extern unsigned long opt_affinity_mask;

extern struct pool_infos pools[MAX_POOLS];
extern int num_pools;

extern struct thr_info *thr_info;

bool miner_should_abort(void);
void miner_request_abort(void);
void miner_clear_abort_flag(void);
void miner_work_generation_reset(void);
int start_mining(void);
int miner_get_current_pool_index(void);
void miner_set_current_pool_index(int pool_index);
bool miner_pool_is_usable(int pool_index);
int miner_get_pool_timeout(int pool_index);
bool miner_work_init(struct work *work);
void miner_work_reset(struct work *work);
void miner_work_cleanup(struct work *work);
bool miner_work_copy(struct work *dst, const struct work *src);
uint8_t *miner_work_solution(struct work *work);
const uint8_t *miner_work_solution_const(const struct work *work);
uint8_t *miner_work_extra(struct work *work);
const uint8_t *miner_work_extra_const(const struct work *work);
void miner_runtime_begin(void);
void miner_runtime_end(void);
void miner_runtime_publish_global_hashrate(double hashrate);
void miner_record_thread_share_result(int thread_id, bool accepted);
bool miner_init_algorithm_runtime(bool *algorithm_ready_out);
void miner_cleanup_algorithm_runtime(bool algorithm_ready);
void miner_configure_current_thread(struct thr_info *thread_ctx);

/* Hotplug/cpuset affinity reconciliation: rate-limited internally; call from
 * the worker loop once per scan chunk. Re-detects topology when the kernel's
 * online-CPU mask changes and upgrades the thread onto its ideal core once
 * that core is available. */
void miner_thread_repin_tick(int thread_id);

static inline double miner_thread_hashrate_load(const struct thr_info *thread)
{
    double hashrate;
    __atomic_load(&thread->hashrate, &hashrate, __ATOMIC_RELAXED);
    return hashrate;
}

static inline void miner_thread_hashrate_store(struct thr_info *thread, double hashrate)
{
    __atomic_store(&thread->hashrate, &hashrate, __ATOMIC_RELAXED);
}

static inline uint64_t miner_thread_hashes_done_load(const struct thr_info *thread)
{
    return __atomic_load_n(&thread->hashes_done_total, __ATOMIC_RELAXED);
}

static inline void miner_thread_hashes_done_store(struct thr_info *thread, uint64_t total_hashes)
{
    __atomic_store_n(&thread->hashes_done_total, total_hashes, __ATOMIC_RELAXED);
}

static inline void miner_thread_hashes_done_add(struct thr_info *thread, uint64_t delta)
{
    __atomic_add_fetch(&thread->hashes_done_total, delta, __ATOMIC_RELAXED);
}

static inline uint32_t miner_thread_accepted_load(const struct thr_info *thread)
{
    return __atomic_load_n(&thread->accepted, __ATOMIC_RELAXED);
}

static inline void miner_thread_accepted_inc(struct thr_info *thread)
{
    __atomic_add_fetch(&thread->accepted, 1u, __ATOMIC_RELAXED);
}

static inline uint32_t miner_thread_rejected_load(const struct thr_info *thread)
{
    return __atomic_load_n(&thread->rejected, __ATOMIC_RELAXED);
}

static inline void miner_thread_rejected_inc(struct thr_info *thread)
{
    __atomic_add_fetch(&thread->rejected, 1u, __ATOMIC_RELAXED);
}

/* Used only by the generation accessor inlines below.
 * Prefer miner_work_generation_load/bump/reset over direct access. */
extern uint32_t g_miner_work_generation;

static inline uint32_t miner_work_generation_load(void)
{
    return __atomic_load_n(&g_miner_work_generation, __ATOMIC_ACQUIRE);
}

static inline uint32_t miner_work_generation_bump(void)
{
    return __atomic_add_fetch(&g_miner_work_generation, 1, __ATOMIC_ACQ_REL);
}

static inline bool miner_work_restart_requested(uint32_t local_generation)
{
    return miner_work_generation_load() != local_generation;
}

static inline int stratum_thread_active_load(const struct stratum_ctx *sctx)
{
    return __atomic_load_n(&sctx->thread_active, __ATOMIC_ACQUIRE);
}

static inline void stratum_thread_active_store(struct stratum_ctx *sctx, int active)
{
    __atomic_store_n(&sctx->thread_active, active, __ATOMIC_RELEASE);
}

static inline int stratum_is_verus_protocol_load(const struct stratum_ctx *sctx)
{
    // Relaxed is correct here: is_verus_protocol is set once during handshake,
    // before any mining thread calls stratum_get_protocol_ops. No happens-before
    // chain requires acquire ordering, and __ATOMIC_RELAXED compiles to a plain
    // ldr on AArch64 (same as the original field access), avoiding the LTO
    // binary-layout shift that __ATOMIC_ACQUIRE caused for the Verus hot loop.
    return __atomic_load_n(&sctx->is_verus_protocol, __ATOMIC_RELAXED);
}

static inline void stratum_is_verus_protocol_store(struct stratum_ctx *sctx, bool is_verus_protocol)
{
    __atomic_store_n(&sctx->is_verus_protocol, is_verus_protocol ? 1 : 0, __ATOMIC_RELEASE);
}

// Utility functions
void applog(int prio, const char *fmt, ...);
// Share-difficulty helper (miner.cpp) used by the scanhash back ends.
void bn_store_share_difficulty(uint32_t *hash, uint32_t *target, struct work *work, int nonce);

/* True if the 256-bit value `hash` is <= `target`, both as little-endian
 * word arrays (word 7 = most significant). Shared full-compare for the
 * scanhash back ends; callers keep their own cheap word-7 prefilter ahead
 * of this so the hot reject path never reaches the loop. */
static inline bool hash_le_target(const uint32_t *hash, const uint32_t *target)
{
    for (int i = 7; i >= 0; i--) {
        if (hash[i] > target[i]) return false;
        if (hash[i] < target[i]) return true;
    }
    return true; /* exactly equal */
}
void format_hashrate(double hashrate, char *output, size_t output_size);
bool hex2bin(void *output, const char *hexstr, size_t len);
char *bin2hex(const unsigned char *in, size_t len);
void cbin2hex(char *out, const char *in, size_t len);
void diff_to_target(uint32_t* target, double diff);

// Config functions
void parse_cmdline(int argc, char *argv[]);
void pool_init_defaults(void);

// Compatibility API functions
bool api_start_service(void);
void api_stop_service(void);
void miner_get_api_snapshot(struct miner_api_snapshot *snapshot);
// Highest max cpufreq (MHz) across all detected cores. Takes the same
// topology lock cpu_topology_refresh() writes under, so callers outside
// miner.cpp (e.g. api.cpp) don't need direct access to g_cpu_cores/g_num_cpus.
int miner_topology_max_cpu_freq_mhz(void);
int miner_topology_num_cpus(void);
uint64_t stratum_work_update_count(void);

// Stratum functions
bool stratum_connect(struct stratum_ctx *sctx);
bool stratum_subscribe(struct stratum_ctx *sctx);
bool stratum_authorize(struct stratum_ctx *sctx, const char *user, const char *pass);
bool stratum_handle_message(struct stratum_ctx *sctx, const char *s);
void stratum_disconnect(struct stratum_ctx *sctx);
bool stratum_send_line(struct stratum_ctx *sctx, const char *s);

bool stratum_submit(struct pool_infos *pool, struct work *work);

// Mining functions
int scanhash_verus(int thr_id, struct work *work, uint32_t max_hashes, unsigned long *hashes_done);
int scanhash_sha256d(int thr_id, struct work *work, uint32_t max_hashes, unsigned long *hashes_done);
int scanhash_scrypt(int thr_id, struct work *work, uint32_t max_hashes, unsigned long *hashes_done);
int scanhash_dispatch(int thr_id, struct work *work, uint32_t max_hashes, unsigned long *hashes_done);
bool verus_init_runtime(void);
void *miner_thread(void *userdata);

// Algorithm selection. ALGO_RANDOMX is always in the enum (keeps table sizes
// stable) but is usable only when built with PRIMO_RANDOMX=1 (the default;
// see randomx_algo.h and the Makefile third_party/RandomX notes).
typedef enum {
    ALGO_VERUS = 0,
    ALGO_SHA256D,    // Bitcoin
    ALGO_SCRYPT,     // Litecoin
    ALGO_RANDOMX,    // Monero (rx/0)
    ALGO_CIVICLIGHT, // CIVIC (CivicNet) -- see civiclight_algo.h
    ALGO_COUNT
} algo_t;

extern algo_t opt_algo;
extern const char *algo_names[ALGO_COUNT];

#ifdef __cplusplus
}
#endif

#endif // MINER_H
