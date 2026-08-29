#include <stdio.h>
#include <string.h>

#include "dev_fee.h"
#include "stratum_internal.h"

struct stratum_runtime_state {
    struct work current_work;
    bool work_ready;
    char last_job_id[128];
    uint64_t work_updates_total;
    uint64_t work_updates_clean;
    uint64_t work_restart_total;
    uint64_t work_restart_clean;
    uint64_t work_restart_height;
    uint64_t work_restart_target;
    uint32_t share_accepted_total;
    uint32_t share_rejected_total;
};

static struct stratum_runtime_state g_runtime_state;

static void stratum_copy_runtime_stats_locked(struct stratum_runtime_stats *stats_out)
{
    if (!stats_out)
        return;

    stats_out->work_updates_total = g_runtime_state.work_updates_total;
    stats_out->work_updates_clean = g_runtime_state.work_updates_clean;
    stats_out->work_restart_total = g_runtime_state.work_restart_total;
    stats_out->work_restart_clean = g_runtime_state.work_restart_clean;
    stats_out->work_restart_height = g_runtime_state.work_restart_height;
    stats_out->work_restart_target = g_runtime_state.work_restart_target;
    stats_out->share_accepted_total = g_runtime_state.share_accepted_total;
    stats_out->share_rejected_total = g_runtime_state.share_rejected_total;
}

static void stratum_fill_api_work_snapshot(struct stratum_api_work_snapshot *snapshot,
    const struct work *work)
{
    if (!snapshot || !work)
        return;

    snprintf(snapshot->job_id, sizeof(snapshot->job_id), "%s", work->job_id);
    snapshot->xnonce2_len = work->xnonce2_len;
    memcpy(snapshot->xnonce2, work->xnonce2, sizeof(snapshot->xnonce2));
    snapshot->targetdiff = work->targetdiff;
    snapshot->height = work->height;
}

void stratum_publish_work(const struct work *new_work, bool clean)
{
    char job_id_copy[sizeof(g_runtime_state.current_work.job_id)];
    uint32_t work_height;
    uint32_t restart_generation;
    double work_diff;
    bool log_new_work = false;

    pthread_mutex_lock(&stratum_work_lock);

    /* The restart decision and the update counters MUST be computed/bumped in
     * the same critical section that installs the work. They used to be
     * advanced by the commit path in an earlier hold of this lock; an
     * exhausted miner thread waking on the advanced update counter could then
     * copy the still-old current work, re-snapshot the counter, and sleep
     * straight through this (non-restart) publication. */
    bool height_changed = !g_runtime_state.work_ready ||
        (new_work->height != g_runtime_state.current_work.height);
    bool target_changed = !g_runtime_state.work_ready ||
        (memcmp(new_work->target, g_runtime_state.current_work.target,
                sizeof(new_work->target)) != 0);
    bool should_restart = !g_runtime_state.work_ready || clean || height_changed || target_changed;

    if (!miner_work_copy(&g_runtime_state.current_work, new_work)) {
        pthread_mutex_unlock(&stratum_work_lock);
        applog(LOG_ERR, "Failed to publish work update");
        return;
    }

    g_runtime_state.work_updates_total++;
    if (clean)
        g_runtime_state.work_updates_clean++;
    if (should_restart) {
        g_runtime_state.work_restart_total++;
        if (clean)
            g_runtime_state.work_restart_clean++;
        if (height_changed)
            g_runtime_state.work_restart_height++;
        if (target_changed)
            g_runtime_state.work_restart_target++;
    }

    if (should_restart)
        restart_generation = miner_work_generation_bump();
    else
        restart_generation = miner_work_generation_load();

    g_runtime_state.current_work.restart_generation = restart_generation;
    g_runtime_state.work_ready = true;

    snprintf(job_id_copy, sizeof(job_id_copy), "%s", new_work->job_id);
    work_height = g_runtime_state.current_work.height;
    work_diff = g_runtime_state.current_work.targetdiff;

    if (strcmp(g_runtime_state.last_job_id, job_id_copy) != 0) {
        log_new_work = true;
        snprintf(g_runtime_state.last_job_id, sizeof(g_runtime_state.last_job_id), "%s", job_id_copy);
    }
    pthread_mutex_unlock(&stratum_work_lock);

    if (!opt_quiet && log_new_work) {
        // %.3f rounds anything under 0.0005 down to a display of "0.000",
        // which is indistinguishable from a genuine 0.0 -- and those mean
        // very different things for diff_to_target() (a tiny-but-nonzero
        // diff still yields a real, hard target; a true 0.0 or a rounding
        // artifact both saturate to the degenerate near-maximal target).
        // %g shows real precision so this stops being ambiguous.
        if (work_height > 0)
            applog(LOG_INFO, "New work: block %u, job %s, diff %g",
                   work_height, job_id_copy, work_diff);
        else
            applog(LOG_INFO, "New work: job %s, diff %g", job_id_copy, work_diff);
    }
}

/* Monotonic count of ALL accepted work updates (published or not restart-
 * worthy). Exhausted miner threads wake on this, not just the restart
 * generation: a non-clean same-height/same-target refresh doesn't bump the
 * generation, but its changed preimage can make an exhausted partition
 * scannable again (see the exhaustion-escape reset in miner_thread). */
uint64_t stratum_work_update_count(void)
{
    pthread_mutex_lock(&stratum_work_lock);
    uint64_t n = g_runtime_state.work_updates_total;
    pthread_mutex_unlock(&stratum_work_lock);
    return n;
}

void stratum_update_share_stats(int pooln, bool accepted, uint32_t *accepted_out, uint32_t *rejected_out)
{
    pthread_mutex_lock(&stratum_work_lock);

    if (pooln >= 0 && pooln < num_pools) {
        if (accepted)
            pools[pooln].accepted_count++;
        else
            pools[pooln].rejected_count++;
    } else {
        applog(LOG_WARNING, "Share result received for invalid pool index %d", pooln);
    }

    /* Dev-fee slice results stay OUT of the user-facing totals (API summary
     * ACC/REJ, final stats) — they are not the user's shares. The hidden
     * pool's own per-pool counters above still record them, so dev-fee
     * validation in the logs keeps working. */
    if (!devfee_is_dev_pool(pooln)) {
        if (accepted)
            g_runtime_state.share_accepted_total++;
        else
            g_runtime_state.share_rejected_total++;
    }

    if (accepted_out)
        *accepted_out = g_runtime_state.share_accepted_total;
    if (rejected_out)
        *rejected_out = g_runtime_state.share_rejected_total;

    pthread_mutex_unlock(&stratum_work_lock);
}

bool stratum_copy_work(struct work *work_out)
{
    bool ready;

    pthread_mutex_lock(&stratum_work_lock);
    ready = g_runtime_state.work_ready;
    if (ready)
        ready = miner_work_copy(work_out, &g_runtime_state.current_work);
    pthread_mutex_unlock(&stratum_work_lock);

    return ready;
}

bool stratum_has_published_work(void)
{
    bool work_ready;

    pthread_mutex_lock(&stratum_work_lock);
    work_ready = g_runtime_state.work_ready;
    pthread_mutex_unlock(&stratum_work_lock);

    return work_ready;
}

void stratum_reset_runtime_state(void)
{
    struct verus_work_payload *preserved_verus;

    pthread_mutex_lock(&stratum_work_lock);
    /* Detach the Verus payload so the memset does not lose the pointer,
     * then restore it afterwards (zeroed).  This keeps current_work.verus
     * non-NULL across reconnects, so the next miner_work_copy call does
     * not need to calloc while holding stratum_work_lock. */
    preserved_verus = g_runtime_state.current_work.verus;
    memset(&g_runtime_state, 0, sizeof(g_runtime_state));
    g_runtime_state.current_work.verus = preserved_verus;
    if (preserved_verus)
        memset(preserved_verus, 0, sizeof(*preserved_verus));
    pthread_mutex_unlock(&stratum_work_lock);
}

void stratum_reset_work_state(void)
{
    pthread_mutex_lock(&stratum_work_lock);
    miner_work_reset(&g_runtime_state.current_work);
    g_runtime_state.work_ready = false;
    g_runtime_state.last_job_id[0] = '\0';
    miner_work_generation_bump();
    pthread_mutex_unlock(&stratum_work_lock);
}

void stratum_get_runtime_stats(struct stratum_runtime_stats *stats_out)
{
    if (!stats_out)
        return;

    pthread_mutex_lock(&stratum_work_lock);
    stratum_copy_runtime_stats_locked(stats_out);
    pthread_mutex_unlock(&stratum_work_lock);
}

void stratum_record_share_submit(int pooln, double sharediff)
{
    pthread_mutex_lock(&stratum_work_lock);

    if (pooln >= 0 && pooln < num_pools) {
        pools[pooln].last_share_time = time(NULL);
        if (sharediff > pools[pooln].best_share)
            pools[pooln].best_share = sharediff;
    }

    pthread_mutex_unlock(&stratum_work_lock);
}

void stratum_get_api_pool_snapshot(int pooln, struct stratum_api_pool_snapshot *snapshot)
{
    if (!snapshot)
        return;

    memset(snapshot, 0, sizeof(*snapshot));

    pthread_mutex_lock(&stratum_work_lock);
    stratum_copy_runtime_stats_locked(&snapshot->runtime_stats);

    if (pooln >= 0 && pooln < num_pools) {
        snapshot->accepted_count = pools[pooln].accepted_count;
        snapshot->rejected_count = pools[pooln].rejected_count;
        snapshot->last_share_time = pools[pooln].last_share_time;
        snapshot->best_share = pools[pooln].best_share;
    }

    if (g_runtime_state.work_ready && pooln == (int)g_runtime_state.current_work.pooln) {
        snapshot->work_ready = true;
        stratum_fill_api_work_snapshot(&snapshot->current_work, &g_runtime_state.current_work);
    }

    pthread_mutex_unlock(&stratum_work_lock);
}

bool stratum_update_active_work_height_locked(int pooln, const char *job_id, uint32_t height)
{
    if (height == 0 || !job_id || !job_id[0])
        return false;

    if (g_runtime_state.work_ready &&
        pooln == (int)g_runtime_state.current_work.pooln &&
        strcmp(g_runtime_state.current_work.job_id, job_id) == 0 &&
        g_runtime_state.current_work.height != height) {
        g_runtime_state.current_work.height = height;
        return true;
    }

    return false;
}
