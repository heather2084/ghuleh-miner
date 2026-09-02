/*
 * Mining coordination runtime for Primo ARM Miner.
 * Copyright (C) 2026 primo-arm-miner contributors.
 *
 * Coordinates work handoff, share submission, worker lifecycle, and
 * runtime statistics for the ARM-native mining threads.
 * Substantially rewritten on 2026-03-16 from earlier GPL-licensed mining
 * software ancestry. See LICENSE and PROVENANCE.md.
 */

#include <errno.h>
#include <inttypes.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "civiclight_algo.h"
#include "cpu_features.h"
#include "dev_fee.h"
#include "miner.h"
#ifdef PRIMO_RANDOMX
#include "randomx_algo.h"
#endif
#include "scrypt_neon.h"
#include "sha256_neon.h"
#include "stratum_internal.h"

// Global state
struct thr_info *thr_info = NULL;
uint32_t g_miner_work_generation = 0;
int abort_flag = 0;
static pthread_mutex_t stats_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile sig_atomic_t shutdown_signal = 0;

// Stratum locks (needed by stratum.cpp)
pthread_mutex_t stratum_sock_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t stratum_work_lock = PTHREAD_MUTEX_INITIALIZER;

// Statistics
static double global_hashrate = 0.0;
static time_t start_time = 0;
static time_t last_stats_time = 0;
static const int hashrate_warmup_sec = 10;
static int hashrate_reporter_stop_requested = 0;

struct miner_hashrate_report {
    int thread_count;
    double thread_hashrates[MAX_THREADS];
    double total_hashrate;
};

bool miner_should_abort(void)
{
    return __atomic_load_n(&abort_flag, __ATOMIC_ACQUIRE) != 0;
}

void miner_request_abort(void)
{
    __atomic_store_n(&abort_flag, 1, __ATOMIC_RELEASE);
}

void miner_clear_abort_flag(void)
{
    __atomic_store_n(&abort_flag, 0, __ATOMIC_RELEASE);
}

void miner_work_generation_reset(void)
{
    __atomic_store_n(&g_miner_work_generation, 0, __ATOMIC_RELEASE);
}

static bool miner_work_ensure_verus_payload(struct work *work)
{
    if (!work)
        return false;

    if (work->verus || opt_algo != ALGO_VERUS)
        return true;

    work->verus = (struct verus_work_payload *)calloc(1, sizeof(*work->verus));
    if (!work->verus) {
        applog(LOG_ERR, "Failed to allocate Verus work payload");
        return false;
    }

    return true;
}

bool miner_work_init(struct work *work)
{
    if (!work)
        return false;

    memset(work, 0, sizeof(*work));
    return miner_work_ensure_verus_payload(work);
}

void miner_work_reset(struct work *work)
{
    struct verus_work_payload *verus;

    if (!work)
        return;

    verus = work->verus;
    memset(work, 0, sizeof(*work));
    work->verus = verus;
    if (verus)
        memset(verus, 0, sizeof(*verus));
}

void miner_work_cleanup(struct work *work)
{
    if (!work)
        return;

    free(work->verus);
    work->verus = NULL;
}

bool miner_work_copy(struct work *dst, const struct work *src)
{
    struct verus_work_payload *dst_verus;

    if (!dst || !src)
        return false;

    dst_verus = dst->verus;
    *dst = *src;
    dst->verus = dst_verus;

    if (!src->verus) {
        if (dst->verus)
            memset(dst->verus, 0, sizeof(*dst->verus));
        return true;
    }

    if (!dst->verus) {
        dst->verus = (struct verus_work_payload *)calloc(1, sizeof(*dst->verus));
        if (!dst->verus) {
            applog(LOG_ERR, "Failed to allocate Verus work payload");
            memset(dst, 0, sizeof(*dst));
            dst->verus = dst_verus;
            if (dst->verus)
                memset(dst->verus, 0, sizeof(*dst->verus));
            return false;
        }
    }

    memcpy(dst->verus, src->verus, sizeof(*dst->verus));
    return true;
}

uint8_t *miner_work_solution(struct work *work)
{
    return (work && work->verus) ? work->verus->solution : NULL;
}

const uint8_t *miner_work_solution_const(const struct work *work)
{
    return (work && work->verus) ? work->verus->solution : NULL;
}

uint8_t *miner_work_extra(struct work *work)
{
    return (work && work->verus) ? work->verus->extra : NULL;
}

const uint8_t *miner_work_extra_const(const struct work *work)
{
    return (work && work->verus) ? work->verus->extra : NULL;
}

static int priority_to_nice_value(int priority_level)
{
    switch (priority_level) {
        case 0: return 15;
        case 1: return 5;
        case 2: return 0;
        case 3: return -1;
        case 4: return -10;
        case 5: return -15;
        default: return 0;
    }
}

static cpu_core_info_t *find_cpu_core_info(int cpu_id)
{
    for (int i = 0; i < g_num_cpus; i++) {
        if (g_cpu_cores[i].cpu_id == cpu_id)
            return &g_cpu_cores[i];
    }
    return NULL;
}

/* CPUs the platform actually lets this process run on. Android confines apps
 * with cpuset cgroups, so cores that exist in sysfs can still be off-limits —
 * pinning to one fails with EINVAL. The inherited affinity mask is the ground
 * truth for what a pin may legally request, so every pin target below is
 * chosen from this set. Falls back to "all detected CPUs" if the query fails
 * (non-Linux or exotic seccomp), which restores the old behavior. */
static void miner_query_allowed_cpus(cpu_set_t *allowed)
{
    if (sched_getaffinity(0, sizeof(*allowed), allowed) == 0)
        return;
    CPU_ZERO(allowed);
    for (int i = 0; i < g_num_cpus; i++) {
        if (g_cpu_cores[i].cpu_id >= 0 && g_cpu_cores[i].cpu_id < CPU_SETSIZE)
            CPU_SET(g_cpu_cores[i].cpu_id, allowed);
    }
}

/* Long-term pin policy, captured once before any thread self-pins. The
 * inherited affinity mask alone can't drive the hotplug reconciliation tick:
 * once a thread self-pins, sched_getaffinity returns the pin, not the
 * original grant — which is how the tick used to escape an external taskset
 * (it re-pinned "ideal" cores from the full topology). But the inherited mask
 * also legitimately omits hotplug-parked cores, which we DO want to adopt
 * when they online. Disambiguation: a core that exists (possible) but was
 * offline at capture is parked, not forbidden — include it; a core online at
 * capture yet missing from the inherited mask was deliberately excluded
 * (taskset/cgroup) — never pin it. Capture failure leaves the policy
 * unrestricted, i.e. the old behavior. */
static cpu_set_t g_policy_allowed_cpus;
static bool g_policy_allowed_valid = false;
static pthread_once_t g_policy_allowed_once = PTHREAD_ONCE_INIT;

/* Parse a kernel cpulist string (e.g. "0-3,6-7") from sysfs into a set. */
static bool miner_parse_cpulist_file(const char *path, cpu_set_t *set)
{
    char buf[256];
    FILE *f = fopen(path, "r");
    bool ok;

    CPU_ZERO(set);
    if (!f)
        return false;
    ok = fgets(buf, sizeof(buf), f) != NULL;
    fclose(f);
    if (!ok)
        return false;

    char *p = buf;
    while (*p) {
        while (*p == ',' || *p == ' ' || *p == '\n')
            p++;
        if (*p < '0' || *p > '9')
            break;
        long first = strtol(p, &p, 10);
        long last = first;
        if (*p == '-')
            last = strtol(p + 1, &p, 10);
        if (first < 0 || last < first)
            return false;
        for (long cpu = first; cpu <= last && cpu < CPU_SETSIZE; cpu++)
            CPU_SET((int)cpu, set);
    }
    return CPU_COUNT(set) > 0;
}

static void miner_capture_policy_allowed_cpus(void)
{
#ifdef __ANDROID__
    /* Android moves processes between cpuset cgroups at runtime (foreground/
     * background/vendor-specific), so the affinity inherited at startup is
     * NOT stable user intent — freezing it permanently exiles threads from
     * big cores that were merely cpuset-restricted at launch (field-hit:
     * SD680 stuck at ~2/3 hashrate with the tick "re-pinning" workers back
     * onto little cores). The kernel enforces real cgroup limits with EINVAL,
     * which the tick already handles by retrying, so the dynamic behavior is
     * both safe and self-healing here. External taskset containment is a
     * desktop-Linux workflow; leave the policy unrestricted on Android. */
    return;
#endif
    cpu_set_t inherited, online, possible;

    if (sched_getaffinity(0, sizeof(inherited), &inherited) != 0)
        return;
    if (!miner_parse_cpulist_file("/sys/devices/system/cpu/online", &online) ||
        !miner_parse_cpulist_file("/sys/devices/system/cpu/possible", &possible))
        return;

    CPU_ZERO(&g_policy_allowed_cpus);
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
        if (CPU_ISSET(cpu, &inherited) ||
            (CPU_ISSET(cpu, &possible) && !CPU_ISSET(cpu, &online)))
            CPU_SET(cpu, &g_policy_allowed_cpus);
    }
    g_policy_allowed_valid = true;
}

/* Topology-order pin target for this thread, restricted to the allowed set.
 * Returns -1 when no core in the preferred order is allowed. */
static int select_allowed_cpu_for_thread(int thread_id, const cpu_set_t *allowed)
{
    int allowed_count = 0;

    for (int i = 0; i < g_core_order_count; i++) {
        int cpu_id = g_core_order[i];
        if (cpu_id >= 0 && cpu_id < CPU_SETSIZE && CPU_ISSET(cpu_id, allowed))
            allowed_count++;
    }
    if (allowed_count == 0)
        return -1;

    int target_index = thread_id % allowed_count;
    for (int i = 0; i < g_core_order_count; i++) {
        int cpu_id = g_core_order[i];
        if (cpu_id < 0 || cpu_id >= CPU_SETSIZE || !CPU_ISSET(cpu_id, allowed))
            continue;
        if (target_index-- == 0)
            return cpu_id;
    }
    return -1;
}

/* Affinity reconciliation. Android hotplugs/parks cores under light load
 * (Qualcomm core_ctl, MediaTek hotplug): a parked core is absent from the
 * allowed set at startup, so its thread starts on a fallback core — and the
 * mining load itself is what wakes the parked core moments later. Each
 * thread remembers its ideal (topology-order) core and periodically retries
 * the pin; when the core comes online the thread upgrades onto it. Also
 * heals pins broken by the kernel when a core goes offline mid-run (the
 * kernel resets such a thread's mask to "all online"). One sched_getcpu +
 * at most one sched_setaffinity per interval — noise-level cost. */
static const int k_repin_interval_sec = 20;
static int g_thread_ideal_cpu[MAX_THREADS];
static time_t g_thread_next_repin[MAX_THREADS];

/* Topology re-detection on hotplug. Cores parked at startup are invisible to
 * detect_cpu_topology() (offline cores expose no MIDR or cpufreq nodes, so
 * they could not be classified anyway). When the kernel's online mask
 * changes — Android waking parked cores under our own mining load, or
 * parking them again — re-scan and let every thread recompute its ideal
 * core. The lock serializes the refresh against ideal recomputation; the
 * generation counter tells threads their cached ideal is stale. */
static pthread_mutex_t g_topology_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_topology_generation = 1;
static int g_thread_topology_gen[MAX_THREADS];

static int select_affinity_cpu_for_thread(int thread_id, const cpu_set_t *allowed);

/* Ideal (topology-order) core for a thread, ignoring the CURRENT allowed
 * set — a core parked by hotplug right now is still the right long-term
 * home — but never outside the startup pin policy: cores the user/platform
 * deliberately excluded (taskset, cgroup) must not be adopted by the tick.
 * Single source of truth for startup pinning and the reconciliation tick. */
static int compute_ideal_cpu_for_thread(int thread_id)
{
    if (opt_affinity_set && g_num_cpus > 0) {
        cpu_set_t full_set;
        CPU_ZERO(&full_set);
        for (int i = 0; i < g_num_cpus; i++) {
            int cpu_id = g_cpu_cores[i].cpu_id;
            if (cpu_id >= 0 && cpu_id < CPU_SETSIZE)
                CPU_SET(cpu_id, &full_set);
        }
        if (g_policy_allowed_valid)
            CPU_AND(&full_set, &full_set, &g_policy_allowed_cpus);
        return select_affinity_cpu_for_thread(thread_id, &full_set);
    }
    if (g_core_order_count > 0) {
        if (g_policy_allowed_valid)
            return select_allowed_cpu_for_thread(thread_id, &g_policy_allowed_cpus);
        return get_cpu_for_thread(thread_id);
    }
    return -1;
}

/* One-time big-core frequency-cap diagnostic. A thread can be pinned to a big
 * core perfectly and still deliver LITTLE-core hashrate if Android has placed
 * the process in a scheduler cgroup whose uclamp.max caps the FREQUENCY
 * schedutil requests — so the prime core sits near its minimum clock under 100%
 * load. We detect this directly: after sustained load (this runs from the repin
 * tick, >= one interval in), the core a thread is running on should be near its
 * max frequency; if a big core is far below, say so once with the launch-side
 * remedy. Purely advisory: read-only, no effect on hashing. */
static void miner_check_bigcore_freq_cap(void)
{
    static volatile int freq_cap_logged = 0;
    if (freq_cap_logged)
        return;

    int cpu = sched_getcpu();
    if (cpu < 0)
        return;

    /* find_cpu_core_info() walks g_cpu_cores[]/g_num_cpus, which
     * cpu_topology_refresh() rewrites under g_topology_lock from a
     * concurrent repin tick on another thread — snapshot the fields we
     * need while holding the same lock instead of dereferencing `core`
     * unprotected. */
    pthread_mutex_lock(&g_topology_lock);
    cpu_core_info_t *core = find_cpu_core_info(cpu);
    bool is_big = core && core->is_big;
    int max_freq_khz = core ? core->max_freq_khz : 0;
    int implementer = core ? core->implementer : 0;
    int part_number = core ? core->part_number : 0;
    pthread_mutex_unlock(&g_topology_lock);

    if (!is_big || max_freq_khz <= 0)
        return; /* not on a big core (or unknown) — let another thread audit */

    int cur = get_cpu_cur_freq_khz(cpu);
    if (cur <= 0)
        return; /* cpufreq unreadable on this platform */

    /* 70% of max: a genuinely loaded core settles near 100%; a uclamp/cpuset
     * cap typically lands far lower (~40-55% of a ~2.8 GHz prime core). The gap
     * is wide enough that 70% does not false-positive on governor ramp jitter. */
    if ((long)cur * 100 >= (long)max_freq_khz * 70)
        return;

    /* Capped. Claim the one-shot just before logging so concurrent auditors do
     * not double-log; a thread that found nothing above never consumes it. */
    if (__sync_lock_test_and_set(&freq_cap_logged, 1) != 0)
        return;

    applog(LOG_WARNING,
           "CPU %d (%s) only %d/%d MHz under sustained load — big cores appear frequency-capped",
           cpu, cpu_part_name(implementer, part_number),
           cur / 1000, max_freq_khz / 1000);
#ifdef __ANDROID__
    applog(LOG_WARNING,
           "This is Android throttling a non-foreground app (uclamp.max / cpuset tier), not the miner. "
           "Launch via 'adb shell' or keep the app focused on screen (top-app) for full big-core speed.");
#else
    applog(LOG_WARNING,
           "Check the cpufreq governor and any external frequency cap (cpuset/uclamp/thermal) on this core.");
#endif
}

void miner_thread_repin_tick(int thread_id)
{
    if (thread_id < 0 || thread_id >= MAX_THREADS)
        return;

    time_t now = time(NULL);
    if (now < g_thread_next_repin[thread_id])
        return;
    g_thread_next_repin[thread_id] = now + k_repin_interval_sec;

    /* First interval has elapsed → frequencies have settled under load. Audit
     * once for an Android big-core frequency cap before the pin reconciliation. */
    miner_check_bigcore_freq_cap();

    pthread_mutex_lock(&g_topology_lock);
    if (cpu_topology_online_changed()) {
        cpu_topology_refresh();
        g_topology_generation++;
        applog(LOG_INFO,
               "CPU online set changed (hotplug) — re-detected topology: %d cores (%d big)",
               g_num_cpus, g_num_big_cores);
    }
    if (g_thread_topology_gen[thread_id] != g_topology_generation) {
        g_thread_topology_gen[thread_id] = g_topology_generation;
        g_thread_ideal_cpu[thread_id] = compute_ideal_cpu_for_thread(thread_id);
    }
    int ideal_cpu = g_thread_ideal_cpu[thread_id];
    pthread_mutex_unlock(&g_topology_lock);

    if (ideal_cpu < 0 || ideal_cpu >= CPU_SETSIZE)
        return;

    /* Compare the effective MASK, not just the current CPU: cpuset hotplug
     * propagation resets every task's affinity to the full online set when
     * any core comes online (verified on 6.1: pins to cores 4/5/6/0 all
     * became 0-7 the moment cpu7 onlined). A thread can then sit on its
     * ideal core by scheduler luck while being free to wander — re-assert
     * the single-core pin whenever the mask is anything but {ideal}. */
    cpu_set_t current;
    if (sched_getaffinity(0, sizeof(current), &current) == 0 &&
        CPU_COUNT(&current) == 1 && CPU_ISSET(ideal_cpu, &current))
        return; /* pin intact */

    int prev_cpu = sched_getcpu();
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(ideal_cpu, &cpuset);
    if (sched_setaffinity(0, sizeof(cpuset), &cpuset) != 0)
        return; /* still offline/forbidden — retry next interval */

    /* Report actual migrations; silent when merely re-asserting the mask on
     * a thread already sitting on its ideal core (the common case after a
     * hotplug wipe), so flapping cores don't spam the log. */
    if (prev_cpu != ideal_cpu) {
        /* Re-acquire the topology lock for this lookup rather than reusing
         * the read taken above before the unlock — a concurrent repin tick
         * on another thread may be mid-refresh right now. */
        pthread_mutex_lock(&g_topology_lock);
        cpu_core_info_t *core = find_cpu_core_info(ideal_cpu);
        bool core_known = core != NULL;
        bool core_is_big = core && core->is_big;
        pthread_mutex_unlock(&g_topology_lock);
        applog(LOG_INFO, "Thread %d: re-pinned to CPU %d%s after it became available",
               thread_id, ideal_cpu,
               core_known ? (core_is_big ? " (big)" : " (LITTLE)") : "");
    }
}

static int select_affinity_cpu_for_thread(int thread_id, const cpu_set_t *allowed)
{
    int mask_bits = (int)(sizeof(unsigned long) * 8);
    int selected_count = 0;

    if (!opt_affinity_set || g_num_cpus <= 0)
        return -1;

    // Preserve the user-selected CPU set, but iterate that set in the same
    // topology order used by automatic pinning so heterogeneous cores prefer
    // the faster cluster first when the mask spans big and LITTLE CPUs.
    if (g_core_order_count > 0) {
        for (int order_index = 0; order_index < g_core_order_count; order_index++) {
            int cpu_id = g_core_order[order_index];

            if (cpu_id < 0 || cpu_id >= mask_bits || !CPU_ISSET(cpu_id, allowed))
                continue;
            if ((opt_affinity_mask >> cpu_id) & 1UL)
                selected_count++;
        }

        if (selected_count > 0) {
            int target_index = thread_id % selected_count;

            for (int order_index = 0; order_index < g_core_order_count; order_index++) {
                int cpu_id = g_core_order[order_index];

                if (cpu_id < 0 || cpu_id >= mask_bits || !CPU_ISSET(cpu_id, allowed))
                    continue;
                if (!((opt_affinity_mask >> cpu_id) & 1UL))
                    continue;
                if (target_index-- == 0)
                    return cpu_id;
            }
        }
    }

    selected_count = 0;
    for (int cpu_id = 0; cpu_id < g_num_cpus && cpu_id < mask_bits; cpu_id++) {
        if (((opt_affinity_mask >> cpu_id) & 1UL) && CPU_ISSET(cpu_id, allowed))
            selected_count++;
    }

    if (selected_count > 0) {
        int target_index = thread_id % selected_count;

        for (int cpu_id = 0; cpu_id < g_num_cpus && cpu_id < mask_bits; cpu_id++) {
            if (!((opt_affinity_mask >> cpu_id) & 1UL) || !CPU_ISSET(cpu_id, allowed))
                continue;
            if (target_index-- == 0)
                return cpu_id;
        }
    }

    return -1;
}

/*
 * Split the scannable nonce space evenly across threads. nonce_space is
 * normally the whole 2^32, but RandomX in nicehash mode owns only the low
 * 24 bits (the pool sets the rest per worker — see
 * RANDOMX_NICEHASH_NONCE_MASK), so the partition has to shrink with it or
 * threads would hand the scan loop indices outside their slice.
 */
static void miner_get_thread_nonce_range(int thread_id, uint64_t nonce_space,
                                         uint64_t *range_start_out,
                                         uint64_t *range_end_out)
{
    const uint64_t total_nonce_space = nonce_space;
    const uint64_t thread_index = (uint64_t)thread_id;
    const uint64_t thread_count = (uint64_t)opt_n_threads;
    uint64_t range_start = (thread_index * total_nonce_space) / thread_count;
    uint64_t range_end = ((thread_index + 1) * total_nonce_space) / thread_count;

    /* Test-only hook: PRIMO_NONCE_SPAN_TEST=<n> shrinks each thread's
     * partition to n nonces so exhaustion (and the Verus epoch roll) can be
     * exercised in seconds instead of minutes. Never set in production. */
    const char *span_env = getenv("PRIMO_NONCE_SPAN_TEST");
    if (span_env) {
        uint64_t span = strtoull(span_env, NULL, 10);
        if (span > 0 && range_start + span < range_end)
            range_end = range_start + span;
    }

    if (range_start_out)
        *range_start_out = range_start;
    if (range_end_out)
        *range_end_out = range_end;
}

static void recalculate_global_hashrate_locked(void)
{
    global_hashrate = 0.0;
    for (int thread_index = 0; thread_index < opt_n_threads; thread_index++) {
        global_hashrate += miner_thread_hashrate_load(&thr_info[thread_index]);
    }
}

static bool miner_hashrate_reporter_should_stop(void)
{
    return __atomic_load_n(&hashrate_reporter_stop_requested, __ATOMIC_ACQUIRE) != 0;
}

static void miner_hashrate_reporter_request_stop(void)
{
    __atomic_store_n(&hashrate_reporter_stop_requested, 1, __ATOMIC_RELEASE);
}

static void miner_hashrate_reporter_reset_stop_request(void)
{
    __atomic_store_n(&hashrate_reporter_stop_requested, 0, __ATOMIC_RELEASE);
}

static bool miner_snapshot_hashrate_report(time_t now, struct miner_hashrate_report *report_out)
{
    int ready_threads = 0;
    bool warmup_done;
    int thread_count;

    if (!report_out)
        return false;

    memset(report_out, 0, sizeof(*report_out));

    pthread_mutex_lock(&stats_lock);
    if (!thr_info || start_time == 0 || opt_n_threads <= 0 ||
        difftime(now, last_stats_time) < opt_statsavg) {
        pthread_mutex_unlock(&stats_lock);
        return false;
    }

    thread_count = opt_n_threads;
    if (thread_count > MAX_THREADS)
        thread_count = MAX_THREADS;

    for (int thread_index = 0; thread_index < thread_count; thread_index++) {
        double thread_hashrate = miner_thread_hashrate_load(&thr_info[thread_index]);
        report_out->thread_hashrates[thread_index] = thread_hashrate;
        if (thread_hashrate > 0.0)
            ready_threads++;
    }

    warmup_done = difftime(now, start_time) >= hashrate_warmup_sec;
    if (!warmup_done && ready_threads < thread_count) {
        pthread_mutex_unlock(&stats_lock);
        return false;
    }

    report_out->thread_count = thread_count;
    // Recompute rather than trusting the cache: it is only refreshed from
    // the scan loop, so with every thread in the exhaustion wait the total
    // froze at the last scanning rate while the per-thread column read 0.
    recalculate_global_hashrate_locked();
    report_out->total_hashrate = global_hashrate;
    last_stats_time = now;
    pthread_mutex_unlock(&stats_lock);

    return true;
}

static __attribute__((noinline, cold)) void miner_thread_governor_warmup(void)
{
    /* schedutil ramps CPU frequency in response to observed load. Without a
     * warm-up, threads start hashing while the governor is still at idle
     * frequency, producing lower (and variable) hashrate for the first few
     * seconds. A 50ms busy-spin triggers the ramp-up so the core reaches max
     * frequency before the first hash is computed. No root required. */
    struct timespec end, now;
    volatile uint32_t sink = 0;

    clock_gettime(CLOCK_MONOTONIC, &end);
    end.tv_nsec += 50 * 1000000L;
    if (end.tv_nsec >= 1000000000L) {
        end.tv_sec++;
        end.tv_nsec -= 1000000000L;
    }
    do {
        for (int i = 0; i < 1000; i++)
            sink += (uint32_t)i * 2654435761u;
        clock_gettime(CLOCK_MONOTONIC, &now);
    } while (now.tv_sec < end.tv_sec ||
             (now.tv_sec == end.tv_sec && now.tv_nsec < end.tv_nsec));
    (void)sink;
}

void miner_configure_current_thread(struct thr_info *thread_ctx)
{
    int thread_id;

    if (!thread_ctx)
        return;

    thread_id = thread_ctx->id;

    // Preserve the existing CLI priority scale (0..5).
    if (opt_priority > 0) {
        if (setpriority(PRIO_PROCESS, 0, priority_to_nice_value(opt_priority)) != 0 && opt_debug)
            applog(LOG_DEBUG, "setpriority failed: %s", strerror(errno));
    }

    // Pin thread to a specific CPU core. Remember which core we asked for so the
    // post-warmup readback below can tell whether the platform actually honored
    // it (Android cpuset cgroups can clamp a "successful" pin to another cluster).
    // Capture the pin policy first: this thread has not self-pinned yet, so its
    // affinity mask is still the inherited (user/platform-granted) one.
    pthread_once(&g_policy_allowed_once, miner_capture_policy_allowed_cpus);
    int intended_cpu = -1;
    cpu_set_t allowed;
    miner_query_allowed_cpus(&allowed);

    /* Seed the reconciliation tick: remember this thread's ideal core and
     * which topology generation it was computed against. */
    if (thread_id >= 0 && thread_id < MAX_THREADS) {
        pthread_mutex_lock(&g_topology_lock);
        g_thread_ideal_cpu[thread_id] = compute_ideal_cpu_for_thread(thread_id);
        g_thread_topology_gen[thread_id] = g_topology_generation;
        pthread_mutex_unlock(&g_topology_lock);
        g_thread_next_repin[thread_id] = time(NULL) + k_repin_interval_sec;
    }

    /* The topology walks below (core selection, core-info lookups for the
     * pin logs) take g_topology_lock like every other reader of
     * g_cpu_cores[]/g_core_order[]: a repin tick on an already-running
     * thread could in principle be mid-refresh while a late-starting thread
     * is still in here. In practice the first tick fires 20s after startup,
     * but hold the lock anyway so the documented invariant has no
     * exceptions in this TU. */
    if (opt_affinity_set && g_num_cpus > 0) {
        cpu_set_t cpuset;
        pthread_mutex_lock(&g_topology_lock);
        int selected_cpu = select_affinity_cpu_for_thread(thread_id, &allowed);
        pthread_mutex_unlock(&g_topology_lock);

        CPU_ZERO(&cpuset);
        if (selected_cpu >= 0)
            CPU_SET(selected_cpu, &cpuset);

        if (selected_cpu >= 0 &&
            sched_setaffinity(0, sizeof(cpuset), &cpuset) == 0) {
            intended_cpu = selected_cpu;
            pthread_mutex_lock(&g_topology_lock);
            cpu_core_info_t *core = find_cpu_core_info(selected_cpu);
            bool core_known = core != NULL;
            bool core_is_big = core && core->is_big;
            int core_freq_mhz = core ? core->max_freq_khz / 1000 : 0;
            pthread_mutex_unlock(&g_topology_lock);
            if (core_known) {
                applog(LOG_INFO, "Thread %d pinned to CPU %d via affinity mask 0x%lx (%s @ %d MHz)",
                       thread_id, selected_cpu, opt_affinity_mask,
                       core_is_big ? "big" : "LITTLE", core_freq_mhz);
            } else {
                applog(LOG_INFO, "Thread %d pinned to CPU %d via affinity mask 0x%lx",
                       thread_id, selected_cpu, opt_affinity_mask);
            }
        } else {
            applog(LOG_WARNING, "Thread %d: affinity-mask pin unavailable (mask 0x%lx, platform cpuset may exclude those CPUs) — leaving to scheduler",
                   thread_id, opt_affinity_mask);
        }
    } else if (g_core_order_count > 0) {
        /* Assign every thread its slot in a round-robin over the cores we are
         * actually allowed to run on (big-first topology order). Crucially this
         * is done uniformly — NOT "keep each thread's topology-native core and
         * separately remap only the displaced ones". The split approach mixes
         * two inconsistent mappings: a displaced thread (native core withheld)
         * could be placed on a core a non-displaced thread already holds while a
         * third allowed core sits idle (verified: -t7 with one core withheld
         * doubled two threads on one core and left another core unused). The
         * uniform round-robin gives a clean 1:1 whenever threads <= allowed
         * cores, and otherwise doubles up only the highest thread indices. It
         * matches compute_ideal_cpu_for_thread() and the repin tick, and is
         * identical to the native mapping when nothing is withheld. Android
         * foreground apps are routinely restricted to a core subset that varies
         * by vendor and screen state, so this path is common there. */
        pthread_mutex_lock(&g_topology_lock);
        int cpu_id = select_allowed_cpu_for_thread(thread_id, &allowed);
        int core_order_count = g_core_order_count;
        int allowed_count = 0;
        for (int i = 0; i < core_order_count; i++) {
            int oc = g_core_order[i];
            if (oc >= 0 && oc < CPU_SETSIZE && CPU_ISSET(oc, &allowed))
                allowed_count++;
        }
        pthread_mutex_unlock(&g_topology_lock);

        if (cpu_id < 0) {
            applog(LOG_WARNING,
                   "Thread %d: no platform-allowed core found; leaving to scheduler",
                   thread_id);
        } else if (allowed_count > 0 && thread_id >= allowed_count) {
            /* Fewer allowed cores than threads: this thread necessarily shares a
             * core with an earlier one. Name the collision so two half-rate
             * threads in the display are self-explaining. */
            applog(LOG_WARNING,
                   "Thread %d: only %d core(s) available; sharing CPU %d with thread %d",
                   thread_id, allowed_count, cpu_id, thread_id % allowed_count);
        }

        /* When the platform restricts us to fewer cores than the topology has,
         * say so ONCE (not per-thread: the round-robin reshuffles threads across
         * the allowed set, so an individual thread moving off its native core
         * does not by itself mean that core was withheld). This covers Linux
         * taskset/cgroup and Android cpuset alike. */
        if (cpu_id >= 0 && allowed_count > 0 && allowed_count < core_order_count) {
            static volatile int cpuset_hint_logged = 0;
            if (__sync_lock_test_and_set(&cpuset_hint_logged, 1) == 0) {
                applog(LOG_INFO,
                       "Platform allows this process only %d of %d CPUs — total hashrate is capped accordingly",
                       allowed_count, core_order_count);
#ifdef __ANDROID__
                applog(LOG_INFO,
                       "Android withholds CPUs from apps that are not the focused foreground app; "
                       "keep the app on screen (or launch via adb shell) to use all cores — "
                       "withheld cores are re-adopted automatically within ~%d s of becoming available",
                       k_repin_interval_sec);
#endif
            }
        }

        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        if (cpu_id >= 0)
            CPU_SET(cpu_id, &cpuset);
        if (cpu_id >= 0 && sched_setaffinity(0, sizeof(cpuset), &cpuset) == 0) {
            intended_cpu = cpu_id;
            pthread_mutex_lock(&g_topology_lock);
            cpu_core_info_t *core = find_cpu_core_info(cpu_id);
            bool core_known = core != NULL;
            bool core_is_big = core && core->is_big;
            int core_freq_mhz = core ? core->max_freq_khz / 1000 : 0;
            pthread_mutex_unlock(&g_topology_lock);
            if (core_known) {
                applog(LOG_INFO, "Thread %d pinned to CPU %d (%s @ %d MHz)",
                       thread_id, cpu_id,
                       core_is_big ? "big" : "LITTLE",
                       core_freq_mhz);
            } else {
                applog(LOG_INFO, "Thread %d pinned to CPU %d", thread_id, cpu_id);
            }
        } else if (cpu_id >= 0) {
            /* A single-core pin can still be rejected even after the allowed-set
             * remap above (Android can change the cpuset between the query and
             * the pin, e.g. on a screen-state transition). Fall back to the
             * allowed big-core set so the thread at least stays on a fast core
             * within whatever the platform permits, instead of silently
             * dropping to a default (often LITTLE) core. */
            int pin_errno = errno;
            cpu_set_t big_set;
            int big_count = 0;
            CPU_ZERO(&big_set);
            pthread_mutex_lock(&g_topology_lock);
            for (int i = 0; i < g_num_cpus; i++) {
                int big_cpu = g_cpu_cores[i].cpu_id;
                if (g_cpu_cores[i].is_big &&
                    big_cpu >= 0 && big_cpu < CPU_SETSIZE && CPU_ISSET(big_cpu, &allowed)) {
                    CPU_SET(big_cpu, &big_set);
                    big_count++;
                }
            }
            pthread_mutex_unlock(&g_topology_lock);
            if (big_count > 0 && sched_setaffinity(0, sizeof(big_set), &big_set) == 0) {
                applog(LOG_WARNING,
                       "Thread %d: per-core pin to CPU %d rejected (%s); pinned to allowed big-core set instead",
                       thread_id, cpu_id, strerror(pin_errno));
            } else {
                applog(LOG_WARNING,
                       "Thread %d: CPU pin to %d failed (%s); leaving placement to scheduler",
                       thread_id, cpu_id, strerror(pin_errno));
            }
        }
    } else {
        applog(LOG_INFO, "Thread %d started (no CPU pinning)", thread_id);
    }

    miner_thread_governor_warmup();

    /* Verify where we actually landed. The warm-up spin forces a reschedule, so
     * sched_getcpu() now reflects the real core. On Android a cpuset cgroup can
     * clamp a "successful" pin onto a different (slower) cluster, so this readback
     * is the ground truth when diagnosing low hashrate on a phone. */
    int actual_cpu = sched_getcpu();
    if (actual_cpu >= 0) {
        pthread_mutex_lock(&g_topology_lock);
        cpu_core_info_t *core = find_cpu_core_info(actual_cpu);
        bool core_known = core != NULL;
        bool core_is_big = core && core->is_big;
        pthread_mutex_unlock(&g_topology_lock);
        const char *cls = core_known ? (core_is_big ? "big" : "LITTLE") : "unknown";
        if (intended_cpu >= 0 && actual_cpu != intended_cpu) {
            applog(LOG_WARNING,
                   "Thread %d: requested CPU %d but running on CPU %d (%s) — platform overrode affinity",
                   thread_id, intended_cpu, actual_cpu, cls);
        } else if (core_known && !core_is_big) {
            applog(LOG_WARNING, "Thread %d running on LITTLE CPU %d — expect reduced hashrate",
                   thread_id, actual_cpu);
        } else if (opt_debug) {
            applog(LOG_DEBUG, "Thread %d running on CPU %d (%s)", thread_id, actual_cpu, cls);
        }
    }
}

static void miner_log_hashrate_report(const struct miner_hashrate_report *report)
{
    char line[MAX_THREADS * 24 + 64];
    size_t pos = 0;
    int temp;

    if (!report || report->thread_count <= 0)
        return;

    line[0] = '\0';

    for (int thread_index = 0; thread_index < report->thread_count; thread_index++) {
        char thr_str[32];
        int written;

        format_hashrate(report->thread_hashrates[thread_index], thr_str, sizeof(thr_str));
        if (pos >= sizeof(line) - 1)
            break;
        if (thread_index > 0) {
            written = snprintf(line + pos, sizeof(line) - pos, "  ");
            if (written < 0)
                break;
            if ((size_t)written >= sizeof(line) - pos) {
                pos = sizeof(line) - 1;
                break;
            }
            pos += (size_t)written;
        }
        written = snprintf(line + pos, sizeof(line) - pos, "#%d: %s", thread_index, thr_str);
        if (written < 0)
            break;
        if ((size_t)written >= sizeof(line) - pos) {
            pos = sizeof(line) - 1;
            break;
        }
        pos += (size_t)written;
    }

    char total_str[32];
    format_hashrate(report->total_hashrate, total_str, sizeof(total_str));
    if (pos < sizeof(line) - 1) {
        int written = snprintf(line + pos, sizeof(line) - pos, " | Total: %s", total_str);
        if (written >= 0) {
            if ((size_t)written >= sizeof(line) - pos)
                pos = sizeof(line) - 1;
            else
                pos += (size_t)written;
        }
    }

    temp = get_cpu_temp();
    if (temp > 0 && pos < sizeof(line) - 1)
        snprintf(line + pos, sizeof(line) - pos, " | Temp: %dc", temp);

    applog(LOG_INFO, "%s", line);
}

static void *miner_hashrate_reporter_thread(void *userdata)
{
    (void)userdata;

    while (!miner_should_abort() && !miner_hashrate_reporter_should_stop()) {
        struct miner_hashrate_report report;
        time_t now = time(NULL);

        if (miner_snapshot_hashrate_report(now, &report))
            miner_log_hashrate_report(&report);

        for (int i = 0; i < 10; i++) {
            if (miner_should_abort() || miner_hashrate_reporter_should_stop())
                break;
            usleep(100000);
        }
    }

    return NULL;
}

static void log_pending_shutdown_signal(void)
{
    sig_atomic_t sig = shutdown_signal;
    shutdown_signal = 0;

    switch (sig) {
    case SIGINT:
        applog(LOG_INFO, "SIGINT received, exiting");
        break;
    case SIGTERM:
        applog(LOG_INFO, "SIGTERM received, exiting");
        break;
    default:
        break;
    }
}

void miner_runtime_begin(void)
{
    pthread_mutex_lock(&stats_lock);
    start_time = time(NULL);
    global_hashrate = 0.0;
    last_stats_time = 0;
    pthread_mutex_unlock(&stats_lock);
    miner_hashrate_reporter_reset_stop_request();
}

void miner_runtime_end(void)
{
    miner_hashrate_reporter_request_stop();
    pthread_mutex_lock(&stats_lock);
    start_time = 0;
    global_hashrate = 0.0;
    last_stats_time = 0;
    pthread_mutex_unlock(&stats_lock);
}

void miner_runtime_publish_global_hashrate(double hashrate)
{
    pthread_mutex_lock(&stats_lock);
    global_hashrate = hashrate;
    pthread_mutex_unlock(&stats_lock);
}

void miner_record_thread_share_result(int thread_id, bool accepted)
{
    if (!thr_info || thread_id < 0 || thread_id >= opt_n_threads)
        return;

    if (accepted)
        miner_thread_accepted_inc(&thr_info[thread_id]);
    else
        miner_thread_rejected_inc(&thr_info[thread_id]);
}

bool miner_init_algorithm_runtime(bool *algorithm_ready_out)
{
    bool algorithm_ready = false;

    if (opt_algo == ALGO_VERUS) {
        algorithm_ready = verus_init_runtime();
    } else if (opt_algo == ALGO_SHA256D) {
        int selftest_result;

        /* Vector test of the generic C path first (it is the reference the
         * scan-path cross-check below hashes against), then the dual-nonce
         * mining path itself — the only place sha256d_dual_asm is ever
         * validated. Refuse to mine on mismatch rather than submit garbage. */
        selftest_result = sha256_neon_selftest();
        if (selftest_result != 0) {
            applog(LOG_ERR, "SHA256 self-test FAILED (error %d)!", selftest_result);
            goto out;
        }

        selftest_result = sha256d_scan_selftest();
        if (selftest_result != 0) {
            applog(LOG_ERR, "SHA256d mining-path self-test FAILED (error %d)!", selftest_result);
            goto out;
        }

        applog(LOG_INFO, "SHA256d self-test passed");
        algorithm_ready = true;
    } else if (opt_algo == ALGO_SCRYPT) {
        int selftest_result;

        if (scrypt_init(opt_n_threads) != 0) {
            applog(LOG_ERR, "Failed to initialize scrypt scratchpads");
            goto out;
        }

        selftest_result = scrypt_selftest();
        if (selftest_result != 0) {
            applog(LOG_ERR, "Scrypt self-test FAILED (error %d)!", selftest_result);
            goto out;
        }

        applog(LOG_INFO, "Scrypt self-test passed");
        algorithm_ready = true;
#ifdef PRIMO_RANDOMX
    } else if (opt_algo == ALGO_RANDOMX) {
        /* Runs the reference-vector self-test internally; refuses to mine
         * on mismatch (broken build / JIT / fast-math contamination). */
        algorithm_ready = randomx_init_runtime(opt_n_threads) != 0;
#endif
    } else if (opt_algo == ALGO_CIVICLIGHT) {
        /* Runs the v1/v2 known-answer self-test internally; refuses to
         * mine on mismatch. */
        algorithm_ready = civiclight_init_runtime(opt_n_threads) != 0;
    } else {
        applog(LOG_ERR, "No init handler for algorithm %d", (int)opt_algo);
    }

out:
    if (algorithm_ready_out)
        *algorithm_ready_out = algorithm_ready;
    return algorithm_ready;
}

void miner_cleanup_algorithm_runtime(bool algorithm_ready)
{
    if (!algorithm_ready)
        return;

    if (opt_algo == ALGO_SCRYPT)
        scrypt_cleanup();
#ifdef PRIMO_RANDOMX
    if (opt_algo == ALGO_RANDOMX)
        randomx_cleanup_runtime();
#endif
    if (opt_algo == ALGO_CIVICLIGHT)
        civiclight_cleanup_runtime();
}

void miner_get_api_snapshot(struct miner_api_snapshot *snapshot)
{
    if (!snapshot)
        return;

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->cpu_temp = get_cpu_temp();

    pthread_mutex_lock(&stats_lock);
    snapshot->runtime_active = (start_time != 0);
    snapshot->start_time = start_time;
    if (thr_info)
        recalculate_global_hashrate_locked();  // same staleness as the report path
    snapshot->global_hashrate = global_hashrate;

    if (thr_info) {
        int snapshot_threads = opt_n_threads;
        if (snapshot_threads > MAX_THREADS)
            snapshot_threads = MAX_THREADS;
        snapshot->thread_count = snapshot_threads;

        for (int thread_index = 0; thread_index < snapshot->thread_count; thread_index++) {
            struct miner_thread_api_stats *thread_stats = &snapshot->threads[thread_index];
            cpu_core_info_t *core;
            int cpu_id;

            thread_stats->hashrate = miner_thread_hashrate_load(&thr_info[thread_index]);
            thread_stats->hashes_done_total = miner_thread_hashes_done_load(&thr_info[thread_index]);
            thread_stats->accepted = miner_thread_accepted_load(&thr_info[thread_index]);
            thread_stats->rejected = miner_thread_rejected_load(&thr_info[thread_index]);

            /* Report the thread's actual pin target, not the native topology
             * mapping: with Android cpuset-withheld cores or --cpu-affinity
             * the round-robin over the ALLOWED set diverges from
             * get_cpu_for_thread(), and the API would name the wrong core
             * (and thus the wrong big/LITTLE class and max freq). Threads
             * seeded against the current topology generation carry a valid
             * cached ideal; otherwise compute it the same way pinning will. */
            pthread_mutex_lock(&g_topology_lock);
            if (g_thread_topology_gen[thread_index] == g_topology_generation)
                cpu_id = g_thread_ideal_cpu[thread_index];
            else
                cpu_id = compute_ideal_cpu_for_thread(thread_index);
            if (cpu_id < 0)
                cpu_id = get_cpu_for_thread(thread_index);
            core = find_cpu_core_info(cpu_id);
            if (core) {
                thread_stats->cpu_max_freq_mhz = core->max_freq_khz / 1000;
                thread_stats->cpu_is_big = core->is_big;
            }
            pthread_mutex_unlock(&g_topology_lock);
            thread_stats->cpu_id = cpu_id;

            snapshot->total_hashes_done += thread_stats->hashes_done_total;
        }
    }
    pthread_mutex_unlock(&stats_lock);
}

int miner_topology_num_cpus(void)
{
    pthread_mutex_lock(&g_topology_lock);
    int n = g_num_cpus;
    pthread_mutex_unlock(&g_topology_lock);
    return n;
}

int miner_topology_max_cpu_freq_mhz(void)
{
    pthread_mutex_lock(&g_topology_lock);
    int max_freq_khz = 0;
    for (int i = 0; i < g_num_cpus; i++) {
        if (g_cpu_cores[i].max_freq_khz > max_freq_khz)
            max_freq_khz = g_cpu_cores[i].max_freq_khz;
    }
    pthread_mutex_unlock(&g_topology_lock);
    return max_freq_khz / 1000;
}

static bool work_is_new_job(const struct work *current_work,
                            const uint32_t *previous_verus_header,
                            bool have_previous_verus_header,
                            bool is_first_work,
                            const char *previous_job_id)
{
    if (opt_algo == ALGO_VERUS) {
        // 36 bytes = version (word 0) + prevhash (words 1-8)
        return is_first_work ||
               !have_previous_verus_header ||
               memcmp(&current_work->data[0], previous_verus_header, 36) != 0;
    }

    return is_first_work || strcmp(current_work->job_id, previous_job_id) != 0;
}

// Verus nonce-space widening: the 32-bit search counter (seeded from header
// word 30, rolled in the solution tail by scanhash_verus) exhausts in minutes
// at modern rates (e.g. ~12 min on an M1 at 5.9 MH/s), and on pools that
// don't refresh the job preimage between blocks the threads then stall until
// the next block. The header's 32-byte nonce field (words 27-34) belongs to
// the miner past extranonce1 — so on exhaustion we bump an "epoch" in word 32
// and rescan the partition: a new epoch changes the hashed preimage, making
// the whole 2^32 counter space fresh. Word 32 specifically because it is
// hashed on BOTH job flavors: classic jobs hash the full header (word 32 is
// in the Haraka midstate), and v7+ merged-mining jobs — today's VRSC mainnet,
// where consensus CLEARS the header nonce field — carry it via the hashed
// nonce_space bytes [7..10] (scanhash_verus mirrors the daemon's
// canonicalization and copies pdata word 32 there; the win path mirrors it
// back into the submitted header, see try_record_share).
// Epochs are monotonic within a job (reset only on a new block), so every
// (preimage, epoch, counter) triple is unique — duplicate-share-safe by
// construction, including across coinbase/ntime refreshes.
static bool verus_nonce_epoch_can_roll(const struct work *work, uint32_t current_epoch)
{
    if (opt_algo != ALGO_VERUS)
        return false;
    if (current_epoch == UINT32_MAX)  // unreachable in practice; never wrap
        return false;
    return miner_work_solution_const(work) != NULL;
}

// Share-difficulty helper used by the hashing back ends.
extern "C" void bn_store_share_difficulty(uint32_t* hash, uint32_t* target, struct work* work, int nonce)
{
    if (nonce < 0 || nonce >= MAX_NONCES)
        return;

    // Compute target/hash as floating point for share difficulty display.
    // Both are LE 256-bit: word 0 = LSW, word 7 = MSW.
    const double word_base = 4294967296.0;
    double target_value = 0.0;
    double hash_value = 0.0;

    for (int word_index = 7; word_index >= 0; word_index--) {
        target_value = target_value * word_base + (double)target[word_index];
        hash_value = hash_value * word_base + (double)hash[word_index];
    }

    work->sharediff[nonce] = (hash_value > 0.0) ? target_value / hash_value : 0.0;
}

int scanhash_dispatch(int thr_id, struct work *work, uint32_t max_hashes, unsigned long *hashes_done)
{
    switch (opt_algo) {
        case ALGO_VERUS:
            return scanhash_verus(thr_id, work, max_hashes, hashes_done);
        case ALGO_SHA256D:
            return scanhash_sha256d(thr_id, work, max_hashes, hashes_done);
        case ALGO_SCRYPT:
            return scanhash_scrypt(thr_id, work, max_hashes, hashes_done);
#ifdef PRIMO_RANDOMX
        case ALGO_RANDOMX:
            return scanhash_randomx(thr_id, work, max_hashes, hashes_done);
#endif
        case ALGO_CIVICLIGHT:
            return scanhash_civic(thr_id, work, max_hashes, hashes_done);
        default:
            applog(LOG_ERR, "Unsupported algorithm id %d in scanhash_dispatch", (int)opt_algo);
            miner_request_abort();
            return 0;
    }
}

static bool submit_ready_share(struct work *work)
{
    struct pool_infos *pool = &pools[work->pooln];
    struct stratum_ctx *sctx = &pool->stratum;

    // FIX (2026-09-02): this used to drop the share right here whenever the
    // work generation counter had ticked forward at all since this share's
    // batch started scanning -- but that counter bumps on EVERY restart-
    // worthy job (routine new blocks included, not just reconnects/
    // failovers), and on this coin's pool blocks land every 15-90s. That
    // made this function throw away the large majority of legitimately
    // found shares before they ever reached the pool, well before the pool
    // itself got any say -- measured on live traffic as ~1 accepted share
    // per ~17 min here vs. CivicLight's own reference miner (EasyMiner)
    // landing ~1 every ~14s at a similar hashrate against the same pool.
    // A share found against a specific job_id is still exactly as valid as
    // it was the moment it was found, regardless of what job showed up
    // after -- the pool keeps a window of recent job_ids for exactly this
    // reason and is the right place to decide accept vs. reject, not a
    // local generation counter whose real job is telling the SCAN LOOP when
    // to stop hashing against outdated work (see civiclight.cpp and the
    // other algorithms' scan loops, which still check it correctly for
    // that). The stratum_is_authenticated() check just below still catches
    // the genuine reconnect/failover case this comment used to describe --
    // a share left over from right before a reconnect will very often still
    // land inside that not-yet-authenticated window and get skipped here;
    // if it doesn't, the pool will simply reply "Rejected" for it like any
    // other stale submission, which is the normal, harmless outcome.

    // Don't submit if not authenticated
    if (!stratum_is_authenticated(sctx)) {
        if (opt_debug)
            applog(LOG_DEBUG, "Not submitting work - not authenticated");
        return false;
    }

    // Let the stratum layer select the correct protocol-specific submit path.
    // Share results come back asynchronously on the stratum receive loop.
    if (stratum_submit(pool, work)) {
        stratum_record_share_submit(work->pooln, work->sharediff[work->submit_nonce_id]);
        if (opt_debug)
            applog(LOG_DEBUG, "Share submitted (diff %.3f)", work->sharediff[work->submit_nonce_id]);
        return true;
    }

    applog(LOG_WARNING, "Failed to submit share");
    return false;
}

static void wait_for_work_restart_after_nonce_exhaustion(int thread_id,
                                                         uint32_t restart_generation,
                                                         uint64_t work_update_snapshot,
                                                         struct timespec *rate_window_start,
                                                         unsigned long *rate_window_hashes,
                                                         double *rate_window_scan_sec)
{
    if (opt_debug)
        applog(LOG_DEBUG, "Thread %d: nonce range exhausted, waiting for new work", thread_id);

    // Publish 0 H/s while idle: the stored rate is only refreshed from the
    // scan loop, so without this the display and API freeze at the last
    // scanning rate for the whole wait — a stalled miner looked fully
    // productive, which masked the nonce-exhaustion stall in the field.
    miner_thread_hashrate_store(&thr_info[thread_id], 0.0);

    while (!miner_work_restart_requested(restart_generation) && !miner_should_abort()) {
        // Non-clean same-height/same-target job updates are published WITHOUT
        // bumping the restart generation (deliberate — active scans keep
        // their position). But an exhausted thread must wake for them: the
        // refreshed preimage can make this partition fresh hash space again
        // (the exhaustion-escape reset above decides). The snapshot is taken
        // at work-copy time so an update landing between copy and this wait
        // is never slept through.
        if (stratum_work_update_count() != work_update_snapshot)
            break;
        usleep(50000);  // 50ms
    }

    // Reset the sample window after idle time so the next update reflects
    // active hashing instead of time spent waiting on a new job.
    clock_gettime(CLOCK_MONOTONIC, rate_window_start);
    *rate_window_hashes = 0;
    *rate_window_scan_sec = 0.0;
}

void *miner_thread(void *userdata)
{
    struct thr_info *thread_ctx = (struct thr_info *)userdata;
    int thread_id = thread_ctx->id;
    struct work work;
    uint32_t previous_verus_header[9];
    unsigned long hashes_done = 0;
    uint64_t thread_nonce_start = 0;
    uint64_t thread_nonce_end = 0;
    /* Size of the space thread_nonce_start/end partition. Re-derived from
     * every work unit: a RandomX pool switch (user pool <-> dev-fee proxy)
     * can turn nicehash mode on or off mid-session. */
    uint64_t nonce_space = UINT64_C(1) << 32;

    // High-resolution timing for accurate hashrate
    struct timespec rate_window_start, rate_sample_time;
    clock_gettime(CLOCK_MONOTONIC, &rate_window_start);
    unsigned long rate_window_hashes = 0;
    // Time actually spent inside scanhash this window. The displayed rate is
    // hashes/scan_time, not hashes/wall-clock: wall-clock includes per-chunk
    // overhead (stratum_copy_work, submit, job handling) where the CPU isn't
    // hashing, which under-reported the live rate ~3% vs the benchmark path and
    // vs ccminer (both of which report pure hashing rate).
    double rate_window_scan_sec = 0.0;
    double chunk_hashrate = 0.0;

    // Track nonce position separately (not relying on work structure which gets overwritten)
    uint64_t next_nonce_index = 0;
    // Verus nonce-space epoch (header word 32): bumped when the partition
    // exhausts under an unchanged preimage, giving the thread a fresh 2^32
    // counter space instead of stalling. See verus_nonce_epoch_can_roll().
    uint32_t verus_nonce_epoch = 0;
    bool first_work = true;
    bool have_previous_verus_header = false;
    char previous_job_id[128] = "";
    // Non-nonce header words as last scanned — detects a changed hashing
    // preimage (coinbase/merkle/ntime refresh) after partition exhaustion,
    // which makes the partition fresh hash space again (see below).
    uint32_t last_scanned_header[30];
    // Dual EMA: fast (α=0.3) for chunk_size adaptation, slow (α=0.1) for stable display.
    // Thermal oscillations at 61°C vary rate by ±3% on ~10–30s cycles; α=0.1 attenuates
    // these by ~3× vs α=0.3 while still converging to the true rate within ~90 seconds.
    double display_hashrate = 0.0;

    if (!miner_work_init(&work))
        return NULL;
    memset(previous_verus_header, 0, sizeof(previous_verus_header));
    memset(last_scanned_header, 0, sizeof(last_scanned_header));
    miner_get_thread_nonce_range(thread_id, nonce_space, &thread_nonce_start, &thread_nonce_end);
    next_nonce_index = thread_nonce_start;

    miner_configure_current_thread(thread_ctx);

    while (!miner_should_abort()) {
        // Snapshot the publication counter BEFORE copying: any update that
        // lands after this is visible to the exhaustion wait below.
        uint64_t work_update_snapshot = stratum_work_update_count();
        // Get work from stratum
        if (!stratum_copy_work(&work)) {
            sleep(1);
            continue;
        }

        work.valid_nonces = 0;  // Reset nonce count before each scan

        // Nonce pointer - different offset for different algorithms
        // Verus: offset 30 (EQNONCE_OFFSET)
        // SHA256d/Scrypt: offset 19 (standard 80-byte header, nonce at bytes 76-79)
        // RandomX: the blob nonce is at BYTE 39 (unaligned); the per-thread
        //   counter lives in word RANDOMX_NONCE_WORD, clear of any blob bytes,
        //   and scanhash_randomx patches the blob itself (see randomx_algo.h).
        int nonce_offset = (opt_algo == ALGO_VERUS) ? 30 : 19;
#ifdef PRIMO_RANDOMX
        if (opt_algo == ALGO_RANDOMX)
            nonce_offset = RANDOMX_NONCE_WORD;
#endif
        uint32_t *nonce_word = &work.data[nonce_offset];

#ifdef PRIMO_RANDOMX
        // Nicehash mode shrinks the scannable space to the low 24 bits (the
        // pool owns the rest — see RANDOMX_NICEHASH_NONCE_MASK), and a pool
        // switch (user pool <-> dev-fee proxy) can flip it either way
        // mid-session. Re-derive the partition when it changes and restart
        // at the new slice start: an index from the old space would fall
        // outside this thread's slice.
        if (opt_algo == ALGO_RANDOMX) {
            uint64_t space = work.rx_nonce_mask ? (uint64_t)work.rx_nonce_mask + 1
                                                : (UINT64_C(1) << 32);
            if (space != nonce_space) {
                nonce_space = space;
                miner_get_thread_nonce_range(thread_id, nonce_space,
                                             &thread_nonce_start, &thread_nonce_end);
                next_nonce_index = thread_nonce_start;
                if (opt_debug)
                    applog(LOG_DEBUG, "Thread %d: nonce space now %llu, partition [%llu,%llu)",
                           thread_id, (unsigned long long)nonce_space,
                           (unsigned long long)thread_nonce_start,
                           (unsigned long long)thread_nonce_end);
            }
        }
#endif

        // Handle nonce progression:
        // Only reset scan_nonce when the block actually changes (new prevhash).
        // Coinbase refreshes (same prevhash, updated merkle/ntime) don't need a
        // nonce reset — the nonce space is independent of coinbase content, and
        // continuing from the current position avoids duplicate-share risk.
        //
        // For Verus: compare only version(4) + prevhash(32) = 36 bytes (words 0-8).
        //   Including the merkle root (old 68-byte compare) was wrong — it caused
        //   scan_nonce and the EMA interval to reset on every coinbase refresh,
        //   fragmenting the EMA accumulation window and showing spuriously low rates
        //   during active pool periods that send frequent job updates.
        // For SHA256d/Scrypt: job_id changes only on true block changes (pools
        //   keep the same job_id for coinbase refreshes), so strcmp is sufficient.
        bool new_job = work_is_new_job(&work, previous_verus_header, have_previous_verus_header,
                                       first_work, previous_job_id);

        if (new_job) {
            // Partition the full 2^32 nonce space exactly using 64-bit arithmetic.
            // Each thread owns [start, end) with no gaps and no overlap.
            next_nonce_index = thread_nonce_start;
            verus_nonce_epoch = 0;
            if (opt_algo == ALGO_VERUS) {
                memcpy(previous_verus_header, &work.data[0], sizeof(previous_verus_header));
                have_previous_verus_header = true;
            }
            snprintf(previous_job_id, sizeof(previous_job_id), "%s", work.job_id);
            first_work = false;

            // Reset hashrate interval on new job to prevent stale measurements
            clock_gettime(CLOCK_MONOTONIC, &rate_window_start);
            rate_window_hashes = 0;
            rate_window_scan_sec = 0.0;
        }
        // Otherwise keep next_nonce_index where it was (already advanced locally)

        // Exhaustion escape: the job-change heuristics above deliberately
        // ignore coinbase/merkle/ntime refreshes (same block, same job_id /
        // prevhash) to avoid resetting scan position and the rate EMA. But
        // once this thread has EXHAUSTED its partition, such a refresh makes
        // the whole partition fresh hash space — without a reset the thread
        // would idle until the next block even though the generation bump
        // keeps waking it. Duplicate-safe: a changed preimage cannot
        // reproduce an already-submitted share (different hashes).
        // RandomX is exempt: XMR pools rotate job_id per job (new_job fires)
        // and a 2^32 counter at RandomX rates never exhausts anyway.
        size_t preimage_words = (opt_algo == ALGO_VERUS) ? 30 : 19;
#ifdef PRIMO_RANDOMX
        if (opt_algo == ALGO_RANDOMX)
            preimage_words = 0;
#endif
        if (!new_job && preimage_words &&
            next_nonce_index >= thread_nonce_end &&
            memcmp(work.data, last_scanned_header, preimage_words * 4) != 0) {
            next_nonce_index = thread_nonce_start;
        }
        if (preimage_words)
            memcpy(last_scanned_header, work.data, preimage_words * 4);

        if (next_nonce_index >= thread_nonce_end) {
            // Verus: don't stall — bump the word-32 epoch and rescan the
            // partition as fresh space (see verus_nonce_epoch_can_roll()).
            // Other algos wait for new work.
            if (verus_nonce_epoch_can_roll(&work, verus_nonce_epoch)) {
                verus_nonce_epoch++;
                next_nonce_index = thread_nonce_start;
                if (opt_debug)
                    applog(LOG_DEBUG, "Thread %d: nonce partition exhausted, rolling epoch to %u",
                           thread_id, verus_nonce_epoch);
            } else {
                wait_for_work_restart_after_nonce_exhaustion(thread_id, work.restart_generation,
                                                             work_update_snapshot,
                                                             &rate_window_start, &rate_window_hashes,
                                                             &rate_window_scan_sec);
                continue;
            }
        }

        // Set the nonce position for this scan
        *nonce_word = (uint32_t)next_nonce_index;
        if (opt_algo == ALGO_VERUS) {
            // Stamp the epoch every chunk: stratum_copy_work above rewrote
            // the local header from the published work (word 32 = 0 there).
            work.data[32] = verus_nonce_epoch;
        }
        work.thread_id = (uint8_t)thread_id;

        // Adaptive chunk size targeting ~5 seconds per chunk.
        // Default 3M ensures A55 threads (516 kH/s) complete in ~5.8s — before
        // the 10s warmup fires.  Once smoothed_hashrate is calibrated, the chunk
        // scales to whichever speed the core actually runs at (A76 ≈ 5.5M, A55 ≈
        // 2.6M), keeping display lag consistently under ~5 seconds.
        uint32_t chunk_size;
        if (opt_algo == ALGO_RANDOMX) {
            // RandomX runs ~10-200 H/s per thread — four to five orders of
            // magnitude below the fast-hash algos, so the chunk bounds scale
            // down accordingly (same ~5s display-latency target). The scan
            // loop also checks restart/abort every hash, so a generous chunk
            // costs nothing in job-switch latency.
            if (chunk_hashrate > 1.0) {
                // Clamp in double space before the uint32_t cast: casting an
                // out-of-range double to an unsigned integer type is UB in
                // C/C++ (harmless here on AArch64's saturating FCVTZU, but
                // not guaranteed by the standard - clamp first so the cast
                // is always in-range regardless of target/optimizer).
                double target_size = chunk_hashrate * 5.0;
                if (target_size > 4096.0) target_size = 4096.0;
                if (target_size < 16.0)   target_size = 16.0;
                chunk_size = (uint32_t)target_size;
            } else {
                chunk_size = 256;  // ~3s on a big core, ~15s worst-case light mode
            }
        } else if (chunk_hashrate > 100000.0) {
            double target_size = chunk_hashrate * 5.0;
            if (target_size > (double)0x1000000) target_size = (double)0x1000000;  // cap: 16M
            if (target_size < (double)0x100000)  target_size = (double)0x100000;   // min:  1M
            chunk_size = (uint32_t)target_size;
        } else {
            chunk_size = 0x300000;  // 3M default (fits in A55 warmup window)
        }
        uint64_t remaining_nonces = thread_nonce_end - next_nonce_index;
        uint32_t scan_count = chunk_size;
        if ((uint64_t)scan_count > remaining_nonces)
            scan_count = (uint32_t)remaining_nonces;

        // Mine!
        if (opt_debug)
            applog(LOG_DEBUG, "Thread %d: scanning with nonce start %08x", thread_id, *nonce_word);

        struct timespec scan_t0, scan_t1;
        clock_gettime(CLOCK_MONOTONIC, &scan_t0);
        int nonces_found = scanhash_dispatch(thread_id, &work, scan_count, &hashes_done);
        clock_gettime(CLOCK_MONOTONIC, &scan_t1);
        if ((uint64_t)hashes_done > (uint64_t)scan_count) {
            applog(LOG_ERR,
                   "Thread %d: scan backend exceeded requested nonce count (%lu > %u)",
                   thread_id, hashes_done, scan_count);
            miner_request_abort();
            break;
        }
        rate_window_hashes += hashes_done;
        rate_window_scan_sec += (scan_t1.tv_sec - scan_t0.tv_sec) +
                                (scan_t1.tv_nsec - scan_t0.tv_nsec) / 1e9;
        miner_thread_hashes_done_add(thread_ctx, hashes_done);

        /* Hotplug/cpuset reconciliation: upgrade onto the ideal core once
         * Android brings it online (rate-limited internally). */
        miner_thread_repin_tick(thread_id);

        // Advance through the absolute thread partition using the backend's reported progress.
        next_nonce_index += (uint64_t)hashes_done;

        if (opt_debug && nonces_found > 0)
            applog(LOG_DEBUG, "Thread %d: found %d nonces (nonce epoch %u)",
                   thread_id, nonces_found, verus_nonce_epoch);

        // Submit found nonces immediately.
        for (int i = 0; i < nonces_found && i < MAX_NONCES; i++) {
            work.submit_nonce_id = i;
            submit_ready_share(&work);
        }

        // If nonce range exhausted, wait for new work instead of
        // scanning into other threads' ranges (causes duplicate shares).
        // Verus epoch-rollable work falls through instead: the loop top
        // rolls the epoch with no idle time, so the rate window stays valid.
        if (nonces_found == 0 && next_nonce_index >= thread_nonce_end &&
            !verus_nonce_epoch_can_roll(&work, verus_nonce_epoch)) {
            wait_for_work_restart_after_nonce_exhaustion(thread_id, work.restart_generation,
                                                         work_update_snapshot,
                                                         &rate_window_start, &rate_window_hashes,
                                                         &rate_window_scan_sec);
            continue;  // Skip hashrate update, go back to get_work
        }

        // Calculate hashrate using high-resolution timer and EMA smoothing
        clock_gettime(CLOCK_MONOTONIC, &rate_sample_time);
        double interval_sec = (rate_sample_time.tv_sec - rate_window_start.tv_sec) +
                              (rate_sample_time.tv_nsec - rate_window_start.tv_nsec) / 1e9;

        // Update rate every ~2 seconds for responsive but stable readings
        // Use shorter threshold (1s) for first update to show initial hashrate faster
        double update_threshold = (chunk_hashrate < 1.0) ? 1.0 : 2.0;
        if (interval_sec >= update_threshold && rate_window_hashes > 0 && rate_window_scan_sec > 0.0) {
            // Pure hashing rate: hashes / time-spent-in-scanhash (not wall-clock).
            double current_rate = (double)rate_window_hashes / rate_window_scan_sec;

            // Fast EMA (α=0.3): tracks actual speed for chunk_size adaptation.
            // Converges quickly to real rate, intentionally tracks thermal changes.
            if (chunk_hashrate < 1.0) {
                chunk_hashrate = current_rate;  // First sample
            } else {
                chunk_hashrate = 0.3 * current_rate + 0.7 * chunk_hashrate;
            }

            // Slow EMA (α=0.1, τ≈47s): attenuates short-term thermal oscillations
            // by ~3× for a stable displayed hashrate.  Initialized from first sample
            // so the display is immediate — only the smoothing is slow.
            if (display_hashrate < 1.0) {
                display_hashrate = current_rate;   // First sample: initialize directly
            } else {
                display_hashrate = 0.1 * current_rate + 0.9 * display_hashrate;
            }

            pthread_mutex_lock(&stats_lock);
            miner_thread_hashrate_store(thread_ctx, display_hashrate);
            rate_window_hashes = 0;
            rate_window_scan_sec = 0.0;
            rate_window_start = rate_sample_time;
            recalculate_global_hashrate_locked();
            pthread_mutex_unlock(&stats_lock);
        }
    }

    miner_work_cleanup(&work);
    applog(LOG_INFO, "Thread %d stopped", thread_id);
    return NULL;
}

static void signal_handler(int sig)
{
    switch (sig) {
    case SIGINT:
        shutdown_signal = SIGINT;
        break;
    case SIGTERM:
        shutdown_signal = SIGTERM;
        break;
    }
    miner_request_abort();
}

struct mining_runtime {
    int initialized_pools;
    int started_threads;
    bool algorithm_runtime_ready;
    bool pool_services_stopped;
    bool hashrate_reporter_started;
    pthread_t hashrate_reporter_thread;
    struct pool_infos *pool;
};

static void init_mining_runtime(struct mining_runtime *runtime)
{
    memset(runtime, 0, sizeof(*runtime));
    runtime->pool = &pools[miner_get_current_pool_index()];
    stratum_reset_runtime_state();
    miner_runtime_end();
}

static int start_worker_threads(void)
{
    int started = 0;

    for (int i = 0; i < opt_n_threads; i++) {
        struct thr_info *thread_slot = &thr_info[i];
        thread_slot->id = i;

        if (pthread_create(&thread_slot->pth, NULL, miner_thread, thread_slot)) {
            applog(LOG_ERR, "Failed to create thread %d", i);
            miner_request_abort();
            break;
        }

        started++;
    }

    return started;
}

static void join_worker_threads(int started_threads)
{
    for (int i = 0; i < started_threads; i++) {
        pthread_join(thr_info[i].pth, NULL);
    }
}

static void destroy_pool_contexts(int initialized_pools)
{
    for (int i = 0; i < initialized_pools; i++) {
        stratum_destroy_context(&pools[i].stratum);
    }
}

static void stop_all_pool_services(int initialized_pools)
{
    /* Shut down EVERY pool socket before joining: the single service thread
     * may be blocked in recv on any of them (failover / dev-fee switches),
     * and joining while its socket is still open stalls for up to
     * opt_timeout seconds. */
    for (int i = 0; i < initialized_pools; i++)
        stratum_request_shutdown(&pools[i].stratum);

    stratum_join_service_thread();

    for (int i = 0; i < initialized_pools; i++)
        stratum_thread_active_store(&pools[i].stratum, 0);
}

static void stop_pool_runtime_services(struct mining_runtime *runtime)
{
    if (runtime->pool_services_stopped)
        return;

    stop_all_pool_services(runtime->initialized_pools);
    runtime->pool_services_stopped = true;
}

static void cleanup_mining_runtime(struct mining_runtime *runtime)
{
    api_stop_service();

    if (runtime->hashrate_reporter_started) {
        miner_hashrate_reporter_request_stop();
        pthread_join(runtime->hashrate_reporter_thread, NULL);
        runtime->hashrate_reporter_started = false;
    }

    if (runtime->started_threads > 0) {
        join_worker_threads(runtime->started_threads);
        runtime->started_threads = 0;
    }

    stop_pool_runtime_services(runtime);

    miner_cleanup_algorithm_runtime(runtime->algorithm_runtime_ready);
    runtime->algorithm_runtime_ready = false;

    destroy_pool_contexts(runtime->initialized_pools);
    runtime->initialized_pools = 0;

    miner_runtime_end();
    free(thr_info);
    thr_info = NULL;
}

static void install_miner_signal_handlers(void)
{
    struct sigaction sa;
    struct sigaction ignore_sa;

    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  // No SA_RESTART — allows select() to be interrupted
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    ignore_sa.sa_handler = SIG_IGN;
    sigemptyset(&ignore_sa.sa_mask);
    ignore_sa.sa_flags = 0;
    sigaction(SIGPIPE, &ignore_sa, NULL);
}

static void log_miner_startup(void)
{
    detect_cpu_topology();
    if (g_num_cpus > 0) {
        applog(LOG_INFO, "CPU topology: %d cores (%d big + %d LITTLE)",
               g_num_cpus, g_num_big_cores, g_num_little_cores);
    }

    applog(LOG_INFO, "Using algorithm: %s", algo_names[opt_algo]);
}

static bool allocate_mining_runtime_state(void)
{
    thr_info = (struct thr_info *)calloc(opt_n_threads, sizeof(*thr_info));

    if (thr_info) {
        return true;
    }

    applog(LOG_ERR, "Failed to allocate thread structures");
    return false;
}

static void init_pool_runtime(struct mining_runtime *runtime)
{
    /* Claims the hidden slot above the user pools (no-op when the fee is
     * disabled for this algo). Must run before contexts are initialized so
     * the dev slot gets a stratum context like any other pool. */
    devfee_install_pool();

    for (int i = 0; i < num_pools; i++) {
        stratum_init_context(&pools[i].stratum, i, opt_algo == ALGO_VERUS);
        runtime->initialized_pools++;
    }
}

static bool start_stratum_runtime(struct mining_runtime *runtime)
{
    bool work_ready = false;

    miner_runtime_begin();
    devfee_runtime_begin();

    if (!stratum_start_service(runtime->pool)) {
        return false;
    }

    // Startup should honor the same retry/retry-pause policy as steady-state reconnects.
    if (!stratum_wait_ready(0, &work_ready)) {
        if (!miner_should_abort()) {
            applog(LOG_ERR, "Failed to authenticate with pool - cannot start mining");
            miner_request_abort();
        }
        return false;
    }

    if (!work_ready) {
        applog(LOG_WARNING, "No work received yet, starting anyway...");
    }

    return true;
}

static bool run_mining_workers(struct mining_runtime *runtime)
{
    runtime->started_threads = start_worker_threads();
    if (runtime->started_threads != opt_n_threads) {
        return false;
    }

    if (!opt_quiet) {
        miner_hashrate_reporter_reset_stop_request();
        if (pthread_create(&runtime->hashrate_reporter_thread, NULL,
                           miner_hashrate_reporter_thread, NULL)) {
            applog(LOG_ERR, "Failed to create hashrate reporter thread");
            miner_hashrate_reporter_request_stop();
            miner_request_abort();
            return false;
        }
        runtime->hashrate_reporter_started = true;
    }

    join_worker_threads(runtime->started_threads);
    runtime->started_threads = 0;
    if (runtime->hashrate_reporter_started) {
        miner_hashrate_reporter_request_stop();
        pthread_join(runtime->hashrate_reporter_thread, NULL);
        runtime->hashrate_reporter_started = false;
    }
    return true;
}

static void finish_stratum_runtime(struct mining_runtime *runtime)
{
    // Shutdown the stratum socket to unblock the service thread's socket wait.
    // In multi-threaded programs, SIGINT is delivered to an arbitrary thread,
    // so the stratum thread's blocking receive path is never interrupted by the
    // signal directly. shutdown() makes that wait return immediately.
    stop_pool_runtime_services(runtime);
}

static void print_final_stats(void)
{
    struct stratum_runtime_stats runtime_stats;
    time_t runtime_start = 0;
    double instant_hashrate = 0.0;
    uint64_t total_hashes_done = 0;

    pthread_mutex_lock(&stats_lock);
    runtime_start = start_time;
    instant_hashrate = global_hashrate;
    pthread_mutex_unlock(&stats_lock);

    double elapsed = difftime(time(NULL), runtime_start);

    for (int i = 0; i < opt_n_threads; i++) {
        total_hashes_done += miner_thread_hashes_done_load(&thr_info[i]);
    }

    double avg_hashrate = (elapsed > 0.0) ? ((double)total_hashes_done / elapsed) : 0.0;
    char rate_str[32];
    char inst_rate_str[32];
    format_hashrate(avg_hashrate, rate_str, sizeof(rate_str));
    format_hashrate(instant_hashrate, inst_rate_str, sizeof(inst_rate_str));
    stratum_get_runtime_stats(&runtime_stats);

    applog(LOG_INFO, "");
    applog(LOG_INFO, "=== Final Statistics ===");
    applog(LOG_INFO, "Algorithm: %s", algo_names[opt_algo]);
    applog(LOG_INFO, "Runtime: %.0f seconds", elapsed);
    applog(LOG_INFO, "Hashrate: %s", rate_str);
    applog(LOG_INFO, "Instant: %s", inst_rate_str);
    uint32_t total_accepted = runtime_stats.share_accepted_total;
    uint32_t total_rejected = runtime_stats.share_rejected_total;
    applog(LOG_INFO, "Accepted: %u", total_accepted);
    applog(LOG_INFO, "Rejected: %u", total_rejected);
    if (total_accepted + total_rejected > 0) {
        applog(LOG_INFO, "Efficiency: %.1f%%",
               100.0 * total_accepted / (total_accepted + total_rejected));
    }
    applog(LOG_INFO,
           "Work updates: total=%" PRIu64 " clean=%" PRIu64
           " | restarts=%" PRIu64 " (clean=%" PRIu64
           ", height=%" PRIu64 ", target=%" PRIu64 ")",
           runtime_stats.work_updates_total, runtime_stats.work_updates_clean,
           runtime_stats.work_restart_total, runtime_stats.work_restart_clean,
           runtime_stats.work_restart_height, runtime_stats.work_restart_target);
}

int start_mining(void)
{
    struct mining_runtime runtime;
    int rc = 1;

    miner_clear_abort_flag();
    init_mining_runtime(&runtime);

    install_miner_signal_handlers();
    log_miner_startup();

    if (!allocate_mining_runtime_state()) {
        goto cleanup;
    }

    miner_work_generation_reset();

    if (!miner_init_algorithm_runtime(&runtime.algorithm_runtime_ready)) {
        goto cleanup;
    }

    init_pool_runtime(&runtime);

    if (!start_stratum_runtime(&runtime)) {
        goto cleanup;
    }

    api_start_service();

    if (!run_mining_workers(&runtime)) {
        goto cleanup;
    }

    finish_stratum_runtime(&runtime);
    print_final_stats();
    rc = 0;

cleanup:
    log_pending_shutdown_signal();
    cleanup_mining_runtime(&runtime);
    return rc;
}
