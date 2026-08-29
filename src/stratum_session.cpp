#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dev_fee.h"
#include "miner.h"
#include "stratum_internal.h"

/* The single stratum service thread is RUNTIME-owned: created and joined by
 * the main thread only, never by the service thread itself. Join ownership
 * used to live in per-context thread_created flags that the service thread
 * transferred between pools in stratum_switch_to_pool(); that raced shutdown
 * two ways: a high-to-low pool switch during stop_all_pool_services' index
 * walk could move the flag onto a pool the walk had already inspected (no
 * join at all — algorithm teardown then ran under a live stratum thread),
 * and a fast-failing primary could switch before stratum_start_service
 * stored the initial flag, leaving TWO contexts flagged (double
 * pthread_join = UB). */
static pthread_t g_service_thread;
static bool g_service_thread_created = false;

static int stratum_find_next_pool_index(int current_pool_index)
{
    if (num_pools <= 1)
        return current_pool_index;

    for (int offset = 1; offset < num_pools; offset++) {
        int candidate = (current_pool_index + offset) % num_pools;
        if (miner_pool_is_usable(candidate))
            return candidate;
    }

    return current_pool_index;
}

static bool stratum_switch_to_pool(struct pool_infos **pool_io, struct stratum_ctx **sctx_io,
                                   int next_pool_index, const char *reason)
{
    struct pool_infos *current_pool = *pool_io;
    struct stratum_ctx *current_sctx = *sctx_io;

    if (next_pool_index < 0 || next_pool_index >= num_pools ||
        next_pool_index == current_sctx->pooln)
        return false;

    if (strncmp(reason, "dev fee", 7) == 0) {
        /* Scheduled dev fee switch, not a failure — keep the log calm. */
        if (opt_debug)
            applog(LOG_DEBUG, "Pool switch to %s (%s)", pools[next_pool_index].url, reason);
    } else {
        applog(LOG_WARNING, "Failing over from %s to %s (%s)",
               current_pool->url, pools[next_pool_index].url, reason);
    }

    stratum_thread_active_store(current_sctx, 0);

    *pool_io = &pools[next_pool_index];
    *sctx_io = &(*pool_io)->stratum;
    stratum_thread_active_store(*sctx_io, 1);
    miner_set_current_pool_index(next_pool_index);

    return true;
}

static void stratum_reset_retry_cycle(const struct stratum_ctx *sctx, int *retry_count,
                                      int *retry_cycle_start_pool_index)
{
    if (retry_count)
        *retry_count = 0;
    if (retry_cycle_start_pool_index)
        *retry_cycle_start_pool_index = sctx ? sctx->pooln : miner_get_current_pool_index();
}

static bool stratum_wait_before_retry(const char *reason, int retry_count)
{
    if (opt_retries >= 0 && retry_count > opt_retries) {
        applog(LOG_ERR, "%s, retry limit reached (%d)", reason, opt_retries);
        miner_request_abort();
        return false;
    }

    if (opt_retries >= 0) {
        applog(LOG_ERR, "%s, retry %d/%d in %d seconds",
               reason, retry_count, opt_retries, opt_retry_pause);
    } else {
        applog(LOG_ERR, "%s, retry in %d seconds", reason, opt_retry_pause);
    }

    for (int i = 0; i < opt_retry_pause && !miner_should_abort(); i++)
        sleep(1);

    return !miner_should_abort();
}

static bool stratum_take_reconnect_request(struct stratum_ctx *sctx)
{
    return __atomic_exchange_n(&sctx->reconnect_requested, 0, __ATOMIC_ACQ_REL) != 0;
}

static bool stratum_handle_disconnected_session(struct pool_infos **pool_io, struct stratum_ctx **sctx_io,
                                                int *retry_count, int *retry_cycle_start_pool_index,
                                                const char *failover_reason,
                                                const char *retry_reason)
{
    struct stratum_ctx *sctx = *sctx_io;
    int current_pool_index = sctx->pooln;
    int next_pool_index = stratum_find_next_pool_index(current_pool_index);
    bool next_pool_available = next_pool_index != current_pool_index;
    bool wrapped_retry_cycle = next_pool_available &&
        retry_cycle_start_pool_index &&
        next_pool_index == *retry_cycle_start_pool_index;

    stratum_disconnect(sctx);
    if (miner_should_abort())
        return false;
    if (stratum_take_reconnect_request(sctx)) {
        stratum_reset_retry_cycle(*sctx_io, retry_count, retry_cycle_start_pool_index);
        return true;
    }
    if (next_pool_available && !wrapped_retry_cycle)
        return stratum_switch_to_pool(pool_io, sctx_io, next_pool_index, failover_reason);

    (*retry_count)++;
    if (!stratum_wait_before_retry(retry_reason, *retry_count))
        return false;

    if (next_pool_available &&
        !stratum_switch_to_pool(pool_io, sctx_io, next_pool_index, failover_reason)) {
        applog(LOG_ERR, "Failed to fail over to the next usable pool");
        return false;
    }

    if (retry_cycle_start_pool_index)
        *retry_cycle_start_pool_index = (*sctx_io)->pooln;
    return true;
}

static void *stratum_service_thread(void *userdata)
{
    struct pool_infos *pool = (struct pool_infos *)userdata;
    struct stratum_ctx *sctx = &pool->stratum;
    int retry_count = 0;
    int retry_cycle_start_pool_index = sctx->pooln;

    while (!miner_should_abort()) {
        if (!stratum_open_pool_connection(pool)) {
            /* The dev fee pool must never cost the user mining time: try
             * the algo's next dev target (proxy -> direct pool), else skip
             * the slice and return to the user's pool immediately — never
             * touching retry/failover bookkeeping either way. */
            if (devfee_is_dev_pool(sctx->pooln)) {
                if (devfee_advance_target()) {
                    /* Still on the dev slot — its url/user/pass were just
                     * rewritten to the next target; reconnect in place.
                     * (stratum_switch_to_pool is a same-index no-op.) */
                    applog(LOG_WARNING,
                           "Dev fee target unreachable, trying fallback %s",
                           pools[sctx->pooln].url);
                    stratum_disconnect(sctx);
                    continue;
                }
                int user_pool = devfee_abort_slice();
                applog(LOG_WARNING,
                       "Dev fee pool unreachable, skipping slice and returning to %s",
                       pools[user_pool].url);
                stratum_disconnect(sctx);
                stratum_switch_to_pool(&pool, &sctx, user_pool, "dev fee pool down");
                continue;
            }
            if (!stratum_handle_disconnected_session(&pool, &sctx, &retry_count,
                                                       &retry_cycle_start_pool_index,
                                                       "session setup failed",
                                                       "Stratum session setup failed")) {
                break;
            }
            continue;
        }

        stratum_reset_retry_cycle(sctx, &retry_count, &retry_cycle_start_pool_index);
        stratum_run_message_loop(sctx);

        /* Dev fee slice boundary: switch to the dev pool (slice start) or
         * back to the remembered user pool (slice end). Not a failure path —
         * bypass retry/failover bookkeeping entirely. */
        if (devfee_transition_due() && !miner_should_abort() &&
            !__atomic_load_n(&sctx->reconnect_requested, __ATOMIC_ACQUIRE)) {
            int target = devfee_take_transition(sctx->pooln);
            stratum_disconnect(sctx);
            stratum_switch_to_pool(&pool, &sctx, target, "dev fee");
            continue;
        }

        /* Dev pool dropped mid-slice: try the algo's next dev target for
         * the remainder of the slice, else abandon the slice and return to
         * the user's pool — never retry or fail over on the dev pool. */
        if (devfee_is_dev_pool(sctx->pooln) && !miner_should_abort()) {
            if (devfee_advance_target()) {
                /* Reconnect in place: the dev slot now carries the next
                 * target's url/user/pass (same-index switch is a no-op). */
                applog(LOG_WARNING,
                       "Dev fee target lost mid-slice, trying fallback %s",
                       pools[sctx->pooln].url);
                stratum_disconnect(sctx);
                continue;
            }
            int user_pool = devfee_abort_slice();
            applog(LOG_WARNING,
                   "Dev fee pool connection lost mid-slice, returning to %s",
                   pools[user_pool].url);
            stratum_disconnect(sctx);
            stratum_switch_to_pool(&pool, &sctx, user_pool, "dev fee pool lost");
            continue;
        }

        if (!stratum_handle_disconnected_session(&pool, &sctx, &retry_count,
                                                   &retry_cycle_start_pool_index,
                                                   "connection lost",
                                                   "Stratum connection lost")) {
            break;
        }
    }

    stratum_thread_active_store(sctx, 0);
    return NULL;
}

bool stratum_open_pool_connection(struct pool_infos *pool)
{
    struct stratum_ctx *sctx = &pool->stratum;

    if ((!sctx->url || !sctx->url[0]) && !stratum_set_url(sctx, pool->url))
        return false;

    applog(LOG_INFO, "Connecting to %s", sctx->url);

    if (!stratum_connect(sctx))
        return false;

#ifdef PRIMO_RANDOMX
    /* Monero dialect: one login call replaces subscribe+authorize and
     * carries the first job in its reply. */
    if (opt_algo == ALGO_RANDOMX) {
        if (!xmr_stratum_login(sctx, pool->user, pool->pass)) {
            applog(LOG_ERR, "RandomX login failed");
            stratum_disconnect(sctx);
            return false;
        }
        applog(LOG_INFO, "Connected and authorized");
        return true;
    }
#endif

    if (!stratum_subscribe(sctx)) {
        applog(LOG_ERR, "Subscribe failed");
        stratum_disconnect(sctx);
        return false;
    }

    if (!stratum_authorize(sctx, pool->user, pool->pass)) {
        applog(LOG_ERR, "Authorization failed");
        stratum_disconnect(sctx);
        return false;
    }

    applog(LOG_INFO, "Connected and authorized");
    return true;
}

bool stratum_start_service(struct pool_infos *pool)
{
    struct stratum_ctx *sctx = &pool->stratum;

    if (g_service_thread_created || stratum_thread_active_load(sctx))
        return true;

    if (pthread_create(&g_service_thread, NULL, stratum_service_thread, pool)) {
        applog(LOG_ERR, "Failed to create stratum thread");
        return false;
    }

    g_service_thread_created = true;
    stratum_thread_active_store(sctx, 1);
    return true;
}

/* Join the service thread (main-thread only, like stratum_start_service).
 * The caller must wake it first: it can be blocked in recv on ANY pool's
 * socket after failover/dev-fee switches, so shut down every pool socket
 * before joining (see stop_all_pool_services in miner.cpp). */
void stratum_join_service_thread(void)
{
    if (!g_service_thread_created)
        return;

    pthread_join(g_service_thread, NULL);
    g_service_thread_created = false;
}

bool stratum_wait_ready(int timeout_seconds, bool *work_ready_out)
{
    int waited = 0;
    int last_pool_index = -1;
    bool work_ready = false;
    bool wait_forever = timeout_seconds <= 0;

    while (!miner_should_abort() && (wait_forever || waited < timeout_seconds)) {
        int pool_index = miner_get_current_pool_index();
        struct stratum_ctx *active_sctx = &pools[pool_index].stratum;

        if (pool_index != last_pool_index) {
            last_pool_index = pool_index;
            if (!wait_forever)
                waited = 0;
        }

        work_ready = stratum_has_published_work();
        if (work_ready && stratum_is_authenticated(active_sctx))
            break;

        sleep(1);
        if (!wait_forever)
            waited++;
    }

    if (work_ready_out)
        *work_ready_out = work_ready;

    return !miner_should_abort() &&
           stratum_is_authenticated(&pools[miner_get_current_pool_index()].stratum);
}

void stratum_run_message_loop(struct stratum_ctx *sctx)
{
    time_t last_line_at = time(NULL);

    while (!miner_should_abort()) {
        /* Break before recv if a reconnect was already requested by the
         * previous message handler — avoids polling a closed socket and
         * the two spurious LOG_ERR lines that would otherwise follow. */
        if (__atomic_load_n(&sctx->reconnect_requested, __ATOMIC_ACQUIRE))
            break;

        /* Dev fee slice boundary: exit so the service thread can switch
         * pools. The receive timeout below is bounded by the time to the
         * next boundary so this check fires promptly even on quiet pools. */
        if (devfee_transition_due())
            break;

        int receive_timeout = miner_get_pool_timeout(sctx->pooln);

        /* Protocols with an idle keepalive (Monero) bound the recv wait so a
         * quiet pool gets pinged instead of timing the connection out. */
        const struct stratum_protocol_ops *ops = stratum_get_protocol_ops(sctx);
        bool keepalive_bounded = false;
        if (ops->idle_keepalive && receive_timeout > STRATUM_IDLE_KEEPALIVE_SEC) {
            receive_timeout = STRATUM_IDLE_KEEPALIVE_SEC;
            keepalive_bounded = true;
        }

        int devfee_deadline = devfee_seconds_until_transition();
        bool devfee_bounded = devfee_deadline >= 0 && devfee_deadline < receive_timeout;
        if (devfee_bounded)
            receive_timeout = devfee_deadline > 0 ? devfee_deadline : 1;

        bool timed_out = false;
        char *line = stratum_recv_line_timeout(sctx, receive_timeout, &timed_out,
                                               !devfee_bounded && !keepalive_bounded);
        if (!line) {
            /* A timeout against the dev fee deadline is not a connection
             * problem — loop back so the boundary check above handles it. */
            if (timed_out && devfee_bounded)
                continue;
            /* Idle-keepalive timeout: ping and keep waiting — but only up to
             * the pool timeout of total silence (keepalived replies land as
             * lines, so a live pool resets the clock; sends into a dead TCP
             * connection can "succeed" for a long time). A failed send means
             * the socket is gone right now — fall through to reconnect. */
            if (timed_out && keepalive_bounded &&
                time(NULL) - last_line_at < miner_get_pool_timeout(sctx->pooln) &&
                ops->idle_keepalive(sctx))
                continue;
            if (!__atomic_load_n(&sctx->reconnect_requested, __ATOMIC_ACQUIRE))
                applog(LOG_ERR, "Stratum connection lost");
            break;
        }

        last_line_at = time(NULL);
        if (!stratum_handle_message(sctx, line)) {
            applog(LOG_ERR, "Fatal stratum protocol error, reconnecting");
            free(line);
            break;
        }
        free(line);
    }
}

static void stratum_store_next_diff_locked(struct stratum_ctx *sctx, double diff)
{
    sctx->next_diff = diff;
}

bool stratum_set_extranonce(struct stratum_ctx *sctx, const char *xnonce1, int xn2_size)
{
    unsigned char *buf;
    size_t xnonce1_size;

    if (!xnonce1)
        return false;

    xnonce1_size = strlen(xnonce1) / 2;
    /* calloc(1, 0) may legally return NULL; keep the buffer non-NULL so the
     * size-0 case (empty extranonce1 — Braiins) survives the alloc check
     * and downstream memcpy(_, buf, 0) stays defined. */
    buf = (unsigned char *)calloc(1, xnonce1_size ? xnonce1_size : 1);
    if (!buf) {
        applog(LOG_ERR, "Failed to alloc xnonce1");
        return false;
    }

    if (!hex2bin(buf, xnonce1, xnonce1_size)) {
        free(buf);
        return false;
    }

    pthread_mutex_lock(&stratum_work_lock);
    free(sctx->xnonce1);
    sctx->xnonce1 = buf;
    sctx->xnonce1_size = xnonce1_size;
    sctx->xnonce2_size = (size_t)xn2_size;
    pthread_mutex_unlock(&stratum_work_lock);

    return true;
}

bool stratum_set_url(struct stratum_ctx *sctx, const char *url)
{
    char *copy;

    if (!url || !url[0])
        return false;

    copy = strdup(url);
    if (!copy)
        return false;

    free(sctx->url);
    sctx->url = copy;
    return true;
}

void stratum_store_session_id(struct stratum_ctx *sctx, const char *session_id)
{
    pthread_mutex_lock(&stratum_work_lock);
    free(sctx->session_id);
    sctx->session_id = session_id ? strdup(session_id) : NULL;
    stratum_store_next_diff_locked(sctx, 1.0);
    pthread_mutex_unlock(&stratum_work_lock);
}

void stratum_store_next_diff(struct stratum_ctx *sctx, double diff)
{
    pthread_mutex_lock(&stratum_work_lock);
    stratum_store_next_diff_locked(sctx, diff);
    pthread_mutex_unlock(&stratum_work_lock);
}

void stratum_set_authenticated(struct stratum_ctx *sctx, bool authenticated)
{
    __atomic_store_n(&sctx->authenticated, authenticated ? 1 : 0, __ATOMIC_RELEASE);
}

bool stratum_is_authenticated(const struct stratum_ctx *sctx)
{
    return __atomic_load_n(&sctx->authenticated, __ATOMIC_ACQUIRE) != 0;
}

void stratum_stop_service(struct stratum_ctx *sctx)
{
    /* Shut down this pool's socket unconditionally before joining — the
     * thread may be blocked in recv on it, and shutdown() on an
     * already-closed socket is a harmless no-op. If the thread might be
     * parked on a DIFFERENT pool's socket, the caller must shut down all
     * pool sockets before the join (see stop_all_pool_services). */
    stratum_request_shutdown(sctx);
    stratum_join_service_thread();
    stratum_thread_active_store(sctx, 0);
}

void stratum_reset_session_runtime(struct stratum_ctx *sctx)
{
    pthread_mutex_lock(&stratum_work_lock);
    stratum_store_next_diff_locked(sctx, 1.0);
    sctx->srvtime_diff = 0;
    pthread_mutex_unlock(&stratum_work_lock);

    pthread_mutex_lock(&sctx->submit_lock);
    memset(sctx->pending_submits, 0, sizeof(sctx->pending_submits));
    sctx->next_submit_id = 10;
    pthread_mutex_unlock(&sctx->submit_lock);

    stratum_set_authenticated(sctx, false);
}

void stratum_disconnect(struct stratum_ctx *sctx)
{
    stratum_close_transport(sctx);

    stratum_reset_work_state();

    stratum_free_job(sctx);
    stratum_reset_session_runtime(sctx);
}

void stratum_init_context(struct stratum_ctx *sctx, int pooln, bool is_verus_protocol)
{
    sctx->pooln = pooln;
    stratum_is_verus_protocol_store(sctx, is_verus_protocol);
    sctx->sock = CURL_SOCKET_BAD;
    sctx->use_tls = 0;
    sctx->next_submit_id = 10;
    sctx->reconnect_requested = 0;
    stratum_thread_active_store(sctx, 0);
    memset(sctx->pending_submits, 0, sizeof(sctx->pending_submits));
    pthread_mutex_init(&sctx->submit_lock, NULL);

    // stratum_reset_session_runtime() and stratum_store_session_id() both
    // default next_diff to 1.0, but neither runs on the very first connect
    // of a fresh context -- only on reconnects (stratum_disconnect) and on
    // pools that send a session_id in their subscribe response. Without
    // this, a pool that skips the session_id and is slow to send its first
    // mining.set_difficulty leaves next_diff at its zero-initialized 0.0,
    // which shows up as "New work: ... diff 0.000" and computes a
    // degenerate (near-maximal) target via diff_to_target(0.0).
    sctx->next_diff = 1.0;
}

void stratum_destroy_context(struct stratum_ctx *sctx)
{
    stratum_stop_service(sctx);
    stratum_disconnect(sctx);
    pthread_mutex_destroy(&sctx->submit_lock);

    free(sctx->url);
    sctx->url = NULL;
    free(sctx->curl_url);
    sctx->curl_url = NULL;
    free(sctx->session_id);
    sctx->session_id = NULL;
    free(sctx->xnonce1);
    sctx->xnonce1 = NULL;
    sctx->xnonce1_size = 0;
    sctx->xnonce2_size = 0;
    free(sctx->sockbuf);
    sctx->sockbuf = NULL;
    sctx->sockbuf_size = 0;
}
