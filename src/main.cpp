/*
 * Program entry point and benchmark-mode runtime for Primo ARM Miner.
 * Copyright (C) 2026 primo-arm-miner contributors.
 *
 * Substantially rewritten on 2026-03-16 from earlier GPL-licensed mining
 * software ancestry. See LICENSE and PROVENANCE.md.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/time.h>
#include <unistd.h>

#include "miner.h"
#include "cpu_features.h"
#ifdef PRIMO_RANDOMX
#include "randomx_algo.h"
#endif

static volatile sig_atomic_t benchmark_shutdown_signal = 0;

static void *benchmark_thread(void *userdata);
static void benchmark_signal_handler(int sig);
static void print_startup_banner(void);

static void benchmark_signal_handler(int sig)
{
    benchmark_shutdown_signal = sig;
    miner_request_abort();
}

static void log_pending_benchmark_signal(void)
{
    sig_atomic_t sig = benchmark_shutdown_signal;
    benchmark_shutdown_signal = 0;

    if (sig == SIGINT) {
        applog(LOG_INFO, "SIGINT received, exiting benchmark");
    } else if (sig == SIGTERM) {
        applog(LOG_INFO, "SIGTERM received, exiting benchmark");
    }
}

static void install_benchmark_signal_handlers(void)
{
    struct sigaction sa;
    struct sigaction ignore_sa;

    sa.sa_handler = benchmark_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    ignore_sa.sa_handler = SIG_IGN;
    sigemptyset(&ignore_sa.sa_mask);
    ignore_sa.sa_flags = 0;
    sigaction(SIGPIPE, &ignore_sa, NULL);
}

static void init_benchmark_work(struct work *work, int thr_id)
{
    uint8_t *solution;

    miner_work_reset(work);

    for (int i = 0; i < 48; i++) {
        work->data[i] = (uint32_t)((i + thr_id) * 0x12345678u);
    }

    memset(work->target, 0x00, sizeof(work->target));
    work->restart_generation = miner_work_generation_load();

    if (opt_algo == ALGO_VERUS) {
        solution = miner_work_solution(work);
        if (solution)
            solution[0] = 7;
        work->data[30] = thr_id * 0x10000000u;
#ifdef PRIMO_RANDOMX
    } else if (opt_algo == ALGO_RANDOMX) {
        // words 0-18 above are the synthetic 76-byte blob; the nonce counter
        // word sits outside it (see randomx_algo.h).
        work->data[RANDOMX_NONCE_WORD] = thr_id * 0x10000000u;
#endif
    } else {
        work->data[19] = thr_id * 0x10000000u;
    }
}

static bool benchmark_refresh_work_if_restarted(struct work *work,
                                                int thr_id,
                                                uint32_t nonce_seed,
                                                uint32_t *nonce_start)
{
    if (!miner_work_restart_requested(work->restart_generation))
        return false;

    init_benchmark_work(work, thr_id);
    *nonce_start = nonce_seed;
    return true;
}

static int run_benchmark(void)
{
    int ret = 1;
    bool benchmark_algo_ready = false;
    time_t start = 0;
    time_t end = 0;
    double total_time = 0.0;
    double total_hashrate = 0.0;
    uint64_t total_hashes_done = 0;
    char rate_str[32];

    miner_clear_abort_flag();
    install_benchmark_signal_handlers();
    detect_cpu_topology();

    applog(LOG_INFO, "Running benchmark mode...");
    applog(LOG_INFO, "Algorithm: %s", algo_names[opt_algo]);
    applog(LOG_INFO, "Threads: %d", opt_n_threads);
    applog(LOG_INFO, "Press Ctrl+C to stop");

    if (!miner_init_algorithm_runtime(&benchmark_algo_ready))
        goto cleanup;

    // Benchmark mode reuses the same thread descriptors as live mining.
    thr_info = (struct thr_info *)calloc(opt_n_threads, sizeof(struct thr_info));

    if (!thr_info) {
        applog(LOG_ERR, "Failed to allocate thread structures");
        goto cleanup;
    }

    miner_work_generation_reset();
    miner_runtime_begin();

    api_start_service();

    start = time(NULL);

    for (int i = 0; i < opt_n_threads; i++) {
        thr_info[i].id = i;

        if (pthread_create(&thr_info[i].pth, NULL, benchmark_thread, &thr_info[i])) {
            applog(LOG_ERR, "Failed to create thread %d", i);
            miner_request_abort();
            for (int j = 0; j < i; j++)
                pthread_join(thr_info[j].pth, NULL);
            goto cleanup;
        }
    }

    while (!miner_should_abort()) {
        sleep(opt_statsavg);

        time_t now = time(NULL);
        double elapsed = difftime(now, start);

        double interval_hashrate = 0.0;
        for (int i = 0; i < opt_n_threads; i++) {
            interval_hashrate += miner_thread_hashrate_load(&thr_info[i]);
        }
        miner_runtime_publish_global_hashrate(interval_hashrate);

        format_hashrate(interval_hashrate, rate_str, sizeof(rate_str));

        /* Match the live-mining status line: thermals matter most under
         * benchmark load, especially on passively cooled phones. */
        int temp = get_cpu_temp();
        if (temp >= 0)
            applog(LOG_INFO, "Hashrate: %s (%.0f seconds) | Temp: %dc", rate_str, elapsed, temp);
        else
            applog(LOG_INFO, "Hashrate: %s (%.0f seconds)", rate_str, elapsed);
    }

    for (int i = 0; i < opt_n_threads; i++) {
        pthread_join(thr_info[i].pth, NULL);
    }

    end = time(NULL);
    total_time = difftime(end, start);
    total_hashrate = 0.0;
    total_hashes_done = 0;
    for (int i = 0; i < opt_n_threads; i++) {
        total_hashes_done += miner_thread_hashes_done_load(&thr_info[i]);
    }

    if (total_time > 0.0)
        total_hashrate = (double)total_hashes_done / total_time;

    format_hashrate(total_hashrate, rate_str, sizeof(rate_str));

    applog(LOG_INFO, "");
    applog(LOG_INFO, "=== Benchmark Summary ===");
    applog(LOG_INFO, "Runtime: %.0f seconds", total_time);
    applog(LOG_INFO, "Average Hashrate: %s", rate_str);

    ret = 0;

cleanup:
    log_pending_benchmark_signal();
    api_stop_service();
    miner_runtime_end();
    miner_cleanup_algorithm_runtime(benchmark_algo_ready);
    free(thr_info);
    thr_info = NULL;

    return ret;
}

static void *benchmark_thread(void *userdata)
{
    struct thr_info *mythr = (struct thr_info *)userdata;
    int thr_id = mythr->id;

    struct work work;
    unsigned long hashes_done = 0;
    unsigned long total_hashes = 0;
    uint32_t max_nonce = 0xffffffffu;
    uint32_t nonce_seed = thr_id * 0x10000000u;
    uint32_t nonce_start = nonce_seed;
    int nonce_offset = (opt_algo == ALGO_VERUS) ? 30 : 19;
#ifdef PRIMO_RANDOMX
    if (opt_algo == ALGO_RANDOMX)
        nonce_offset = RANDOMX_NONCE_WORD;
#endif

    struct timeval tv_start, tv_now;
    gettimeofday(&tv_start, NULL);
    if (!miner_work_init(&work))
        return NULL;
    init_benchmark_work(&work, thr_id);
    miner_configure_current_thread(mythr);

    while (!miner_should_abort()) {
        if (benchmark_refresh_work_if_restarted(&work, thr_id, nonce_seed, &nonce_start))
            continue;

        work.data[nonce_offset] = nonce_start;

        // Mine a fixed count of nonces so the per-thread benchmark loop stays
        // stable, sized per algorithm so a batch completes within a few
        // seconds — the hashrate is only sampled BETWEEN scanhash calls, so
        // an oversized batch shows 0.00 H/s until the first one finishes
        // (scrypt at ~2-5 kH/s/thread took ~100 s through the old shared
        // 500k batch; RandomX is 4-5 orders of magnitude slower still).
        uint32_t batch_size;
        switch (opt_algo) {
        case ALGO_RANDOMX: batch_size = 500;    break;  /* ~10-200 H/s/thread */
        case ALGO_SCRYPT:  batch_size = 10000;  break;  /* ~2-5 kH/s/thread */
        /* civiclight v2 (post-activation) runs the same memory-hard N=2048,
         * r=8 yespower cost as scrypt-class algorithms, likely similar
         * kH/s/thread order of magnitude -- reuse scrypt's batch size until
         * real on-device throughput is measured and this can be tuned. */
        case ALGO_CIVICLIGHT: batch_size = 10000; break;
        default:           batch_size = 500000; break;  /* verus/sha256d: MH/s */
        }
        scanhash_dispatch(thr_id, &work, batch_size, &hashes_done);

        nonce_start = work.data[nonce_offset];
        if (nonce_start > max_nonce - batch_size)
            nonce_start = nonce_seed;
        total_hashes += hashes_done;

        miner_thread_repin_tick(thr_id);

        gettimeofday(&tv_now, NULL);
        double elapsed = (tv_now.tv_sec - tv_start.tv_sec) +
                        (tv_now.tv_usec - tv_start.tv_usec) / 1000000.0;
        if (elapsed > 0.1) {
            miner_thread_hashrate_store(mythr, (double)total_hashes / elapsed);
        }
        miner_thread_hashes_done_store(mythr, total_hashes);
    }

    miner_work_cleanup(&work);
    return NULL;
}

static void print_startup_banner(void)
{
    if (!isatty(fileno(stdout)))
        return;

    printf("\n");
    printf("  \x1B[1;36m>>>\x1B[0m \x1B[1;37mPrimo ARM Miner\x1B[0m \x1B[90mv%s\x1B[0m\n", PACKAGE_VERSION);
    printf("  \x1B[90m    Native ARMv8 crypto \x1B[36m|\x1B[90m NEON \x1B[36m|\x1B[90m SHA2 \x1B[36m|\x1B[90m AES\x1B[0m\n");
    printf("\n");
}

int main(int argc, char *argv[])
{
    parse_cmdline(argc, argv);

    if (!opt_quiet)
        print_startup_banner();

    if (!opt_quiet) {
        applog(LOG_INFO, "%d miner thread(s) configured, using '%s' algorithm",
               opt_n_threads, algo_names[opt_algo]);
        int active_pool = miner_get_current_pool_index();
        if (!opt_benchmark && pools[active_pool].url[0]) {
            applog(LOG_INFO, "Starting on %s", pools[active_pool].url);
        }
    }

    int ret = opt_benchmark ? run_benchmark() : start_mining();

    applog(LOG_INFO, "Miner exited with code %d", ret);
    return ret;
}
