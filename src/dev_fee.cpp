/*
 * Dev fee scheduling for Primo ARM Miner.
 * Copyright (C) 2026 primo-arm-miner contributors.
 *
 * See include/dev_fee.h for the model. All state lives here and is only
 * mutated from the single stratum service thread; reads from the message
 * loop happen on the same thread, so no locking is needed.
 */

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "dev_fee.h"
#include "stratum_internal.h"  /* stratum_set_url — keep sctx->url in step
                                * with the retargeted dev slot */

/* Ghuleh Miner: dev fee intentionally disabled at compile time.
 *
 * Upstream (primo-arm-miner) hardcodes its own developer's pool/wallet
 * targets here. Per FORK_PLAN.md §1, this fork ships with the fee off —
 * every target list below is empty, which devfee_target_valid() (below)
 * and dev_fee_target_for_algo() treat as "no fee configured for this
 * algorithm" for all four algorithms. No wallet, no pool, no dev fee.
 *
 * When/if a fee is added later, it is NOT meant to come back as hardcoded
 * entries here — ARCHITECTURE.md describes reading these targets at
 * startup from a signed remote config into config.json's "dev-fee" block
 * instead, so it can be turned on/off/adjusted fleet-wide without an APK
 * update, and never before an in-app notice period elapses. The scheduling
 * engine below (devfee_install_pool / devfee_runtime_begin /
 * devfee_seconds_until_transition / devfee_transition_due /
 * devfee_take_transition / devfee_advance_target / devfee_abort_slice) is
 * kept as-is — it's sound, algorithm-agnostic, and has nothing
 * PrimoLab-specific in it; only the target *data* changes. */
static const struct dev_fee_target k_dev_fee_targets[ALGO_COUNT][DEVFEE_MAX_TARGETS] = {
    /* ALGO_VERUS   */ { { "", "", "", 0.0 }, { "", "", "", 0.0 } },
    /* ALGO_SHA256D */ { { "", "", "", 0.0 }, { "", "", "", 0.0 } },
    /* ALGO_SCRYPT  */ { { "", "", "", 0.0 }, { "", "", "", 0.0 } },
    /* ALGO_RANDOMX */ { { "", "", "", 0.0 }, { "", "", "", 0.0 } },
};

/* The slice is always 60s; the per-algo percent sets the cycle length
 * (1% = one slice per 100 min, 2% = one per 50). PRIMO_DEVFEE_TEST=1
 * shrinks the cycle for functional testing (slice timing only — it cannot
 * change the fee percentage, targets, or duty ratio). */
static const int k_slice_seconds_default = 60;

struct devfee_state {
    bool enabled;
    bool in_slice;
    int dev_pool_index;
    int user_pool_index;     /* return target while a slice runs */
    time_t next_transition;  /* slice start when !in_slice, slice end when in_slice */
    int slice_seconds;
    int cycle_seconds;
    double percent;          /* active algo's duty cycle, for logging */
    int cur_target;          /* index into the algo's target list (0 = proxy) */
    int num_targets;         /* valid entries for the active algo */
};

static struct devfee_state g_devfee = { false, false, -1, -1, 0, 0, 0, 0.0, 0, 0 };

/* True when the list entry is usable. user == "" is the client-tag sentinel
 * (valid); only a missing/empty URL disables an entry. */
static bool devfee_target_valid(const struct dev_fee_target *target)
{
    return target->url && target->url[0] && target->user &&
           target->percent > 0.0;
}

static int devfee_count_targets(algo_t algo)
{
    int n = 0;
    while (n < DEVFEE_MAX_TARGETS &&
           devfee_target_valid(&k_dev_fee_targets[algo][n]))
        n++;
    return n;
}

const struct dev_fee_target *dev_fee_target_for_algo(algo_t algo)
{
    if (algo < 0 || algo >= ALGO_COUNT)
        return NULL;

    const struct dev_fee_target *target = &k_dev_fee_targets[algo][0];
    if (!devfee_target_valid(target))
        return NULL;

    return target;
}

/* The proxy login: a non-identifying <version>-<platform> tag (e.g.
 * "1.0.8-cli"). The proxy substitutes the real wallet server-side; the tag
 * doubles as version-distribution telemetry, disclosed in SECURITY.md.
 * Platform comes from PRIMO_PLATFORM (MinerService sets "apk"); anything
 * else is a CLI run. */
static void devfee_client_tag(char *buf, size_t len)
{
    const char *platform = getenv("PRIMO_PLATFORM");
    if (!platform || strcmp(platform, "apk") != 0)
        platform = "cli";
    snprintf(buf, len, "%s-%s", PACKAGE_VERSION, platform);
}

/* Write target t's url/user/pass into the hidden dev pool slot. Only called
 * from the service thread while the slot is disconnected (install time, a
 * slice start, or an in-slice failover), so rewriting in place is safe —
 * the next stratum_open_pool_connection reads the fields fresh. */
static void devfee_set_pool_target(int t)
{
    const struct dev_fee_target *target = &k_dev_fee_targets[opt_algo][t];
    struct pool_infos *pool = &pools[g_devfee.dev_pool_index];
    char tag[64];

    snprintf(pool->url, sizeof(pool->url), "%s", target->url);
    if (target->user[0]) {
        snprintf(pool->user, sizeof(pool->user), "%s", target->user);
    } else {
        devfee_client_tag(tag, sizeof(tag));
        snprintf(pool->user, sizeof(pool->user), "%s", tag);
    }
    snprintf(pool->pass, sizeof(pool->pass), "%s",
             target->pass && target->pass[0] ? target->pass : "x");
    /* stratum_open_pool_connection caches sctx->url and only reads pool->url
     * when it is empty, so a retarget of pool->url alone would reconnect to
     * the stale target. Force sctx->url to track pool->url here (safe: same
     * service thread, slot disconnected). */
    stratum_set_url(&pool->stratum, pool->url);
    g_devfee.cur_target = t;
}

int devfee_install_pool(void)
{
    const struct dev_fee_target *target = dev_fee_target_for_algo(opt_algo);
    int pool_index;

    g_devfee.enabled = false;
    g_devfee.dev_pool_index = -1;

    if (!target || opt_benchmark)
        return -1;

    if (num_pools >= MAX_POOLS) {
        /* Cannot happen with MAX_USER_POOLS < MAX_POOLS, but stay safe. */
        applog(LOG_WARNING, "No free pool slot for the dev fee; fee disabled");
        return -1;
    }

    pool_index = num_pools;
    memset(&pools[pool_index], 0, sizeof(pools[pool_index]));
    snprintf(pools[pool_index].name, sizeof(pools[pool_index].name), "dev fee");
    g_devfee.dev_pool_index = pool_index;
    g_devfee.num_targets = devfee_count_targets(opt_algo);
    devfee_set_pool_target(0);
    pools[pool_index].configured = true;
    /* disabled keeps the slot out of failover rotation and out of the
     * "first usable pool" startup selection; the dev fee scheduler switches
     * to it explicitly by index. */
    pools[pool_index].disabled = true;
    num_pools = pool_index + 1;

    g_devfee.percent = target->percent;
    g_devfee.slice_seconds = k_slice_seconds_default;
    g_devfee.cycle_seconds =
        (int)((double)k_slice_seconds_default * 100.0 / target->percent);

    const char *test_env = getenv("PRIMO_DEVFEE_TEST");
    if (test_env && test_env[0] == '1') {
        /* Functional-test cadence for validating the pool-switch machinery.
         * Deliberately NOT 1% — a slice must be long enough to authorize
         * and receive work. Timing only; targets cannot be overridden. */
        g_devfee.slice_seconds = 20;
        g_devfee.cycle_seconds = 120;
        applog(LOG_WARNING, "Dev fee TEST cadence active: %ds slice per %ds cycle",
               g_devfee.slice_seconds, g_devfee.cycle_seconds);
    }

    g_devfee.enabled = true;
    applog(LOG_INFO, "Dev fee: %.1f%% (%ds per %d min) for %s",
           g_devfee.percent, g_devfee.slice_seconds,
           g_devfee.cycle_seconds / 60, algo_names[opt_algo]);

    return pool_index;
}

void devfee_runtime_begin(void)
{
    if (!g_devfee.enabled)
        return;

    g_devfee.in_slice = false;

    /* First slice at a uniformly random point within the first cycle,
     * re-drawn every start. A deterministic first transition (the old
     * "exactly one cycle in") lets a scheduled restart just before that
     * mark skip the fee forever; with a uniform draw the only winning
     * restart cadence is more often than one slice length, where reconnect
     * and warmup overhead cost far more than the fee. Long-run duty cycle
     * is unchanged. Deliberately NOT logged: randomization only deters
     * gaming while the scheduled time is unobservable — slices announce
     * themselves when they begin, never in advance. */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    unsigned seed = (unsigned)ts.tv_nsec ^ (unsigned)ts.tv_sec ^
                    ((unsigned)getpid() << 16) ^ (unsigned)time(NULL);
    int first_delay = g_devfee.slice_seconds;
    int span = g_devfee.cycle_seconds - g_devfee.slice_seconds;
    if (span > 0)
        first_delay += (int)(rand_r(&seed) % (unsigned)(span + 1));

    g_devfee.next_transition = time(NULL) + first_delay;
}

bool devfee_is_dev_pool(int pool_index)
{
    return g_devfee.enabled && pool_index == g_devfee.dev_pool_index;
}

int devfee_seconds_until_transition(void)
{
    if (!g_devfee.enabled)
        return -1;

    time_t now = time(NULL);
    if (now >= g_devfee.next_transition)
        return 0;

    return (int)(g_devfee.next_transition - now);
}

bool devfee_transition_due(void)
{
    return devfee_seconds_until_transition() == 0;
}

int devfee_take_transition(int current_pool_index)
{
    time_t now = time(NULL);

    if (!g_devfee.in_slice) {
        g_devfee.in_slice = true;
        g_devfee.user_pool_index = current_pool_index;
        g_devfee.next_transition = now + g_devfee.slice_seconds;
        /* Every slice starts at the head of the target list (the proxy);
         * a previous slice's in-slice failover never sticks. */
        if (g_devfee.cur_target != 0)
            devfee_set_pool_target(0);
        applog(LOG_NOTICE, "Dev fee: mining %ds slice (%.1f%% of runtime)",
               g_devfee.slice_seconds, g_devfee.percent);
        return g_devfee.dev_pool_index;
    }

    g_devfee.in_slice = false;
    g_devfee.next_transition = now + g_devfee.cycle_seconds;
    applog(LOG_NOTICE, "Dev fee: slice complete, returning to %s",
           pools[g_devfee.user_pool_index].url);
    return g_devfee.user_pool_index;
}

bool devfee_advance_target(void)
{
    if (!g_devfee.enabled || !g_devfee.in_slice)
        return false;
    if (g_devfee.cur_target + 1 >= g_devfee.num_targets)
        return false;

    /* Don't bother reconnecting into the tail of the slice: a fresh
     * connect+authorize eats several seconds before any work lands. The
     * floor scales with the slice so PRIMO_DEVFEE_TEST's 20s cadence keeps
     * the same behavior shape. */
    time_t now = time(NULL);
    if (g_devfee.next_transition - now < g_devfee.slice_seconds / 4)
        return false;

    devfee_set_pool_target(g_devfee.cur_target + 1);
    return true;
}

int devfee_abort_slice(void)
{
    /* Dev pool unreachable: never cost the user mining time. Skip this
     * slice entirely and try again a full cycle from now. */
    g_devfee.in_slice = false;
    g_devfee.next_transition = time(NULL) + g_devfee.cycle_seconds;
    return g_devfee.user_pool_index;
}
