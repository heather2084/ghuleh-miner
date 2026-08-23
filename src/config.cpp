/*
 * Command-line and JSON configuration handling for Primo ARM Miner.
 * Copyright (C) 2026 primo-arm-miner contributors.
 *
 * Preserves the existing compatibility surface while keeping the parsing
 * logic local to this project.
 * Substantially rewritten on 2026-03-16 from earlier GPL-licensed mining
 * software ancestry. See LICENSE and PROVENANCE.md.
 */

#include <getopt.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "miner.h"

enum cli_option_id {
    CLI_OPT_BENCHMARK = 1005,
    CLI_OPT_CPU_AFFINITY = 1020,
    CLI_OPT_CPU_PRIORITY = 1021,
    CLI_OPT_NICEHASH = 1022
};

// Global options (defined here, extern in miner.h)
bool opt_debug = false;
bool opt_quiet = false;
bool opt_benchmark = false;
bool opt_protocol = false;
bool opt_nicehash = false;
int opt_n_threads = 0;
int opt_timeout = 300;
int opt_retries = -1;
int opt_retry_pause = 30;
/* NULL by default: a static-init strdup could fail during C-runtime init and
 * leave a dangling NULL that the API path would deref. The API resolves the
 * "127.0.0.1" default at use time (see api_bind_address). */
char *opt_api_bind = NULL;
int opt_api_port = 4068;
/* Set when the user supplied --api-bind: an explicitly requested port must
 * fail loudly instead of silently probing upward (see api.cpp). */
bool opt_api_port_explicit = false;
int opt_statsavg = 30;
int opt_priority = 0;
bool opt_affinity_set = false;
unsigned long opt_affinity_mask = 0;

// Algorithm selection
algo_t opt_algo = ALGO_VERUS;
const char *algo_names[ALGO_COUNT] = {
    "verus",
    "sha256d",
    "scrypt",
    "randomx",
    "civiclight"
};

struct pool_infos pools[MAX_POOLS];
int num_pools = 0;
static int cur_pooln = 0;

enum {
    POOL_URL_FIELD_LEN = sizeof(((struct pool_infos *)0)->url),
    POOL_USER_FIELD_LEN = sizeof(((struct pool_infos *)0)->user),
    POOL_PASS_FIELD_LEN = sizeof(((struct pool_infos *)0)->pass)
};

struct pool_option_defaults {
    char url[POOL_URL_FIELD_LEN];
    char user[POOL_USER_FIELD_LEN];
    char pass[POOL_PASS_FIELD_LEN];
    bool url_set;
    bool user_set;
    bool pass_set;
};

int miner_get_current_pool_index(void)
{
    return __atomic_load_n(&cur_pooln, __ATOMIC_ACQUIRE);
}

void miner_set_current_pool_index(int pool_index)
{
    __atomic_store_n(&cur_pooln, pool_index, __ATOMIC_RELEASE);
}

bool miner_pool_is_usable(int pool_index)
{
    if (pool_index < 0 || pool_index >= num_pools || pool_index >= MAX_POOLS)
        return false;

    return pools[pool_index].configured &&
           !pools[pool_index].disabled &&
           pools[pool_index].url[0] != '\0';
}

int miner_get_pool_timeout(int pool_index)
{
    int timeout = opt_timeout;

    if (pool_index >= 0 && pool_index < num_pools && pool_index < MAX_POOLS &&
        pools[pool_index].configured && pools[pool_index].timeout > 0) {
        timeout = pools[pool_index].timeout;
    }

    return timeout > 0 ? timeout : 1;
}

static int normalize_thread_count(long requested, bool auto_detected)
{
    if (requested < 1) {
        if (auto_detected)
            applog(LOG_WARNING, "Failed to auto-detect CPU count, defaulting to 1 miner thread");
        return 1;
    }

    if (requested > MAX_THREADS) {
        if (auto_detected) {
            applog(LOG_WARNING, "Auto-detected %ld CPU(s), clamping miner threads to %d",
                   requested, MAX_THREADS);
        }
        return MAX_THREADS;
    }

    return (int)requested;
}

static json_t *opt_config = NULL;
static struct pool_option_defaults pool_defaults;
static bool config_uses_pool_array = false;
/* True only while the main CLI getopt loop runs (not config-file application);
 * used to detect repeated -o flags on the command line specifically. */
static bool cli_parse_phase = false;
static bool cli_url_seen = false;
static bool pool_config_user_set[MAX_POOLS];
static bool pool_config_pass_set[MAX_POOLS];
static void exit_with_usage(int status);
static void load_config_file(const char *path);

static void release_loaded_config(void)
{
    if (!opt_config)
        return;

    json_decref(opt_config);
    opt_config = NULL;
}

static struct option g_cli_options[] = {
    { "algo", 1, NULL, 'a' },
    { "api-bind", 1, NULL, 'b' },
    { "benchmark", 0, NULL, CLI_OPT_BENCHMARK },
    { "config", 1, NULL, 'c' },
    { "cpu-affinity", 1, NULL, CLI_OPT_CPU_AFFINITY },
    { "cpu-priority", 1, NULL, CLI_OPT_CPU_PRIORITY },
    { "debug", 0, NULL, 'D' },
    { "help", 0, NULL, 'h' },
    { "nicehash", 0, NULL, CLI_OPT_NICEHASH },
    { "pass", 1, NULL, 'p' },
    { "protocol-dump", 0, NULL, 'P' },
    { "quiet", 0, NULL, 'q' },
    { "retries", 1, NULL, 'r' },
    { "retry-pause", 1, NULL, 'R' },
    { "statsavg", 1, NULL, 'N' },
    { "threads", 1, NULL, 't' },
    { "timeout", 1, NULL, 'T' },
    { "url", 1, NULL, 'o' },
    { "user", 1, NULL, 'u' },
    { "userpass", 1, NULL, 'O' },
    { "version", 0, NULL, 'V' },
    { 0, 0, 0, 0 }
};

static void replace_config_string(char **slot, const char *value)
{
    free(*slot);
    *slot = value ? strdup(value) : NULL;
}

static int parse_cli_int_option(const char *arg, const char *option_name)
{
    char *end = NULL;
    long parsed;

    errno = 0;
    parsed = strtol(arg, &end, 10);
    if (errno != 0 || end == arg || !end || *end != '\0' ||
        parsed < INT_MIN || parsed > INT_MAX) {
        applog(LOG_ERR, "Invalid value for %s: %s", option_name, arg);
        exit_with_usage(1);
    }

    return (int)parsed;
}

static void copy_pool_string(char *dst, size_t dst_size, const char *src,
                             const char *what)
{
    if (dst_size == 0)
        return;

    if (!src) {
        dst[0] = '\0';
        return;
    }

    /* Silent truncation would alter a URL/wallet/password into a different,
     * valid-looking value — warn loudly instead (value itself not logged:
     * it may be a password). */
    if (snprintf(dst, dst_size, "%s", src) >= (int)dst_size)
        applog(LOG_WARNING,
               "config: pool %s longer than %zu characters was TRUNCATED — "
               "check your configuration",
               what, dst_size - 1);
}

static void clear_pool_option_defaults(void)
{
    memset(&pool_defaults, 0, sizeof(pool_defaults));
}

static void set_pool_default_url(const char *value)
{
    copy_pool_string(pool_defaults.url, sizeof(pool_defaults.url), value, "url");
    pool_defaults.url_set = value && value[0];
}

static void set_pool_default_user(const char *value)
{
    copy_pool_string(pool_defaults.user, sizeof(pool_defaults.user), value, "user/wallet");
    pool_defaults.user_set = value != NULL;
}

static void set_pool_default_user_span(const char *value, size_t value_len)
{
    size_t copy_len;

    if (!value) {
        set_pool_default_user(NULL);
        return;
    }

    copy_len = value_len;
    if (copy_len >= sizeof(pool_defaults.user)) {
        copy_len = sizeof(pool_defaults.user) - 1;
        /* Same loud-truncation policy as copy_pool_string (value not
         * logged; the -O form can carry a password). */
        applog(LOG_WARNING,
               "config: pool user/wallet longer than %zu characters was "
               "TRUNCATED — check your configuration",
               sizeof(pool_defaults.user) - 1);
    }

    memcpy(pool_defaults.user, value, copy_len);
    pool_defaults.user[copy_len] = '\0';
    pool_defaults.user_set = true;
}

static void set_pool_default_pass(const char *value)
{
    copy_pool_string(pool_defaults.pass, sizeof(pool_defaults.pass), value ? value : "", "password");
    pool_defaults.pass_set = value != NULL;
}

static void reset_pool_arrays(void)
{
    memset(pools, 0, sizeof(pools));
    memset(pool_config_user_set, 0, sizeof(pool_config_user_set));
    memset(pool_config_pass_set, 0, sizeof(pool_config_pass_set));
}

static void clear_configured_pool_set(void)
{
    reset_pool_arrays();
    num_pools = 0;
    config_uses_pool_array = false;
    miner_set_current_pool_index(0);
}

static void derive_pool_short_url(int pool_index)
{
    const char *url;
    const char *short_url;

    if (pool_index < 0 || pool_index >= MAX_POOLS)
        return;

    url = pools[pool_index].url;
    short_url = strstr(url, "://");
    if (short_url)
        short_url += 3;
    else
        short_url = url;

    copy_pool_string(pools[pool_index].short_url, sizeof(pools[pool_index].short_url), short_url, "url");
}

static void exit_with_usage(int status)
{
    if (status) {
        fprintf(stderr, "Try `%s --help' for more information.\n", PACKAGE_NAME);
    } else {
        printf("Usage: %s [OPTIONS]\n", PACKAGE_NAME);
        printf("Options:\n");
        printf("  -a, --algo=ALGO       Algorithm: verus, sha256d (BTC), scrypt (LTC), randomx (XMR),\n");
        printf("                        civiclight (CIVIC)\n");
        printf("  -o, --url=URL         Pool URL (stratum+tcp:// or stratum+ssl:// for TLS)\n");
        printf("  -O, --userpass=U:P    Username:password pair\n");
        printf("  -u, --user=USERNAME   Wallet address + worker name\n");
        printf("  -p, --pass=PASSWORD   Worker password (default: x)\n");
        printf("  -t, --threads=N       Number of threads (default: CPU count)\n");
        printf("  -b, --api-bind=IP:PORT API bind address (default: 127.0.0.1:4068)\n");
        printf("  -c, --config=FILE     JSON config file (default: ./config.json if present)\n");
        printf("  --benchmark           Run benchmark mode (also accepts legacy -benchmark)\n");
        printf("  -r, --retries=N       Number of retries (default: infinite)\n");
        printf("  -R, --retry-pause=N   Pause between retries in seconds (default: 30)\n");
        printf("  -T, --timeout=N       Network timeout in seconds (default: 300)\n");
        printf("  -D, --debug           Enable debug output\n");
        printf("  -P, --protocol-dump   Dump protocol messages (does not require -D)\n");
        printf("  -q, --quiet           Quiet mode\n");
        printf("  -N, --statsavg=N      Stats averaging window (default: 30)\n");
        printf("  --cpu-affinity=MASK   CPU affinity mask (-1/all, decimal, or hex like 0xf0)\n");
        printf("  --cpu-priority=N      Process priority (0-5, default: 0)\n");
        printf("  --nicehash            RandomX: pool owns the nonce MSB (auto when the pool\n");
        printf("                        advertises it; force only if yours does not)\n");
        printf("  -V, --version         Show version\n");
        printf("  -h, --help            Show this help\n");
        printf("\nSupported algorithms:\n");
        printf("  verus    - VerusHash v2.2 (VRSC)\n");
        printf("  sha256d  - Double SHA256 (BTC, BCH)\n");
        printf("  scrypt   - Scrypt N=1024 (LTC, DOGE)\n");
        printf("  randomx  - RandomX rx/0 (XMR) — aliases: rx, rx/0, xmr, monero\n");
        printf("  civiclight - CivicNet civiclight (CIVIC) — alias: civic\n");
    }
    exit(status);
}

static void apply_pool_default_credentials(int pool_index, bool update_user, bool update_pass,
                                           bool only_missing)
{
    if (pool_index < 0 || pool_index >= MAX_POOLS)
        return;

    if (update_user && pool_defaults.user_set &&
        (!only_missing || !pool_config_user_set[pool_index])) {
        copy_pool_string(pools[pool_index].user, sizeof(pools[pool_index].user), pool_defaults.user, "user/wallet");
    }

    if (update_pass && pool_defaults.pass_set &&
        (!only_missing || !pool_config_pass_set[pool_index])) {
        copy_pool_string(pools[pool_index].pass, sizeof(pools[pool_index].pass), pool_defaults.pass, "password");
    }
}

static void sync_configured_pool_defaults(bool update_user, bool update_pass)
{
    for (int pool_index = 0; pool_index < num_pools; pool_index++) {
        if (pools[pool_index].configured)
            apply_pool_default_credentials(pool_index, update_user, update_pass, false);
    }
}

static void sync_configured_pool_timeouts(int timeout)
{
    if (num_pools == 0)
        return;

    for (int pool_index = 0; pool_index < num_pools; pool_index++) {
        if (!pools[pool_index].configured)
            continue;

        pools[pool_index].timeout = timeout;
    }
}

static void register_pool_index(int pool_index)
{
    if (pool_index < 0 || pool_index >= MAX_USER_POOLS)
        return;

    pools[pool_index].configured = true;
    if (pool_index + 1 > num_pools)
        num_pools = pool_index + 1;
}

static bool parse_json_boolish(json_t *value)
{
    if (!value)
        return false;

    if (json_is_true(value))
        return true;

    if (json_is_integer(value))
        return json_integer_value(value) != 0;

    return false;
}

/* Per-pool timeout from a config value. 0 = unset (miner_get_pool_timeout
 * then falls back to the global opt_timeout); a nonpositive configured
 * value means the same thing rather than silently becoming a 1s timeout. */
static int parse_pool_timeout_json(json_t *value)
{
    int parsed;

    if (!value || !json_is_integer(value))
        return 0;

    json_int_t raw = json_integer_value(value);
    if (raw > INT_MAX) {
        applog(LOG_WARNING, "pool timeout %lld out of range — ignored",
               (long long)raw);
        return 0;
    }
    parsed = (int)raw;
    return parsed > 0 ? parsed : 0;
}

static bool affinity_mask_has_hex_alpha(const char *arg)
{
    for (const unsigned char *p = (const unsigned char *)arg; p && *p; p++) {
        if (isxdigit(*p) && !isdigit(*p))
            return true;
    }

    return false;
}

static unsigned long parse_affinity_mask_argument(const char *arg)
{
    char *end = NULL;
    unsigned long parsed = 0;
    int base = 0;

    if (!arg || !arg[0]) {
        applog(LOG_ERR, "Invalid cpu-affinity value");
        exit_with_usage(1);
    }

    if (!strcmp(arg, "-1") || !strcasecmp(arg, "all"))
        return ULONG_MAX;

    if (!strncasecmp(arg, "0x", 2) || affinity_mask_has_hex_alpha(arg))
        base = 16;

    errno = 0;
    parsed = strtoul(arg, &end, base);
    if (errno != 0 || !end || *end != '\0') {
        applog(LOG_ERR, "Invalid cpu-affinity mask: %s", arg);
        exit_with_usage(1);
    }

    return parsed;
}

static void reset_getopt_state(void)
{
    optind = 0;
    optarg = NULL;
    optopt = 0;
}

struct cmdline_preparse_result {
    bool immediate_exit;
    const char *config_path;
};

static struct cmdline_preparse_result preparse_cmdline(int argc, char *argv[])
{
    struct cmdline_preparse_result result = { false, NULL };
    int key;
    int saved_opterr = opterr;

    reset_getopt_state();
    opterr = 0;

    while (1) {
        key = getopt_long(argc, argv, "a:b:c:Dhp:Pqr:R:t:T:o:u:O:VN:", g_cli_options, NULL);
        if (key < 0)
            break;

        if (key == 'c' && optarg && optarg[0]) {
            result.config_path = optarg;
            continue;
        }

        if (key == 'h' || key == 'V') {
            result.immediate_exit = true;
            break;
        }
    }

    opterr = saved_opterr;
    reset_getopt_state();
    return result;
}

static void log_loaded_config_path(const char *path)
{
    char cwd[PATH_MAX];
    char resolved[PATH_MAX];

    if (!path || !path[0])
        return;

    if (path[0] != '/' && getcwd(cwd, sizeof(cwd))) {
        snprintf(resolved, sizeof(resolved), "%s/%s", cwd, path);
        applog(LOG_INFO, "Using config %s", resolved);
        return;
    }

    applog(LOG_INFO, "Using config %s", path);
}

static void maybe_load_default_config(void)
{
    if (access("config.json", R_OK) != 0)
        return;

    load_config_file("config.json");
    log_loaded_config_path("config.json");
}

static int find_first_usable_pool_index(void)
{
    for (int pool_index = 0; pool_index < num_pools; pool_index++) {
        if (miner_pool_is_usable(pool_index))
            return pool_index;
    }

    return -1;
}

static bool parse_algorithm_name(const char *name, algo_t *algo_out)
{
    if (strcasecmp(name, "verus") == 0) {
        *algo_out = ALGO_VERUS;
        return true;
    }
    if (strcasecmp(name, "sha256d") == 0 || strcasecmp(name, "sha256") == 0 ||
        strcasecmp(name, "btc") == 0 || strcasecmp(name, "bitcoin") == 0) {
        *algo_out = ALGO_SHA256D;
        return true;
    }
    if (strcasecmp(name, "scrypt") == 0 || strcasecmp(name, "ltc") == 0 ||
        strcasecmp(name, "litecoin") == 0) {
        *algo_out = ALGO_SCRYPT;
        return true;
    }
    if (strcasecmp(name, "randomx") == 0 || strcasecmp(name, "rx") == 0 ||
        strcasecmp(name, "rx/0") == 0 || strcasecmp(name, "xmr") == 0 ||
        strcasecmp(name, "monero") == 0) {
#ifdef PRIMO_RANDOMX
        *algo_out = ALGO_RANDOMX;
        return true;
#else
        applog(LOG_ERR, "randomx support was not built in (PRIMO_RANDOMX=0)");
        return false;
#endif
    }
    if (strcasecmp(name, "civiclight") == 0 || strcasecmp(name, "civic") == 0) {
        *algo_out = ALGO_CIVICLIGHT;
        return true;
    }
    return false;
}

static bool validate_ipv4_address(const char *address)
{
    struct in_addr parsed;

    return address && address[0] && inet_pton(AF_INET, address, &parsed) == 1;
}

static int parse_api_port_value(const char *arg, bool *ok)
{
    char *end = NULL;
    long parsed;

    if (ok)
        *ok = false;
    if (!arg || !arg[0])
        return 0;

    errno = 0;
    parsed = strtol(arg, &end, 10);
    if (errno != 0 || !end || *end != '\0' || parsed < 0 || parsed > 65535)
        return 0;

    if (ok)
        *ok = true;
    return (int)parsed;
}

static bool api_bind_argument_is_port_only(const char *arg)
{
    if (!arg || !arg[0])
        return false;

    for (const unsigned char *p = (const unsigned char *)arg; *p; p++) {
        if (!isdigit(*p))
            return false;
    }

    return true;
}

static void parse_api_bind_argument(const char *arg)
{
    const char *separator;
    bool port_ok = false;
    int parsed_port = 0;

    if (!arg || !arg[0]) {
        applog(LOG_ERR, "Invalid api-bind value");
        exit_with_usage(1);
    }

    separator = strchr(arg, ':');
    if (separator) {
        char bind_host[INET_ADDRSTRLEN];
        size_t host_len = (size_t)(separator - arg);

        if (separator == arg || separator[1] == '\0' || strchr(separator + 1, ':')) {
            applog(LOG_ERR, "Invalid api-bind value: %s", arg);
            exit_with_usage(1);
        }
        if (host_len >= sizeof(bind_host)) {
            applog(LOG_ERR, "API bind address is too long: %s", arg);
            exit_with_usage(1);
        }

        memcpy(bind_host, arg, host_len);
        bind_host[host_len] = '\0';
        if (!validate_ipv4_address(bind_host)) {
            applog(LOG_ERR, "API bind address is not a valid IPv4 address: %s", bind_host);
            exit_with_usage(1);
        }

        parsed_port = parse_api_port_value(separator + 1, &port_ok);
        if (!port_ok) {
            applog(LOG_ERR, "Invalid API port: %s", separator + 1);
            exit_with_usage(1);
        }

        replace_config_string(&opt_api_bind, bind_host);
        opt_api_port = parsed_port;
        opt_api_port_explicit = true;
        return;
    }

    if (api_bind_argument_is_port_only(arg)) {
        parsed_port = parse_api_port_value(arg, &port_ok);
        if (!port_ok) {
            applog(LOG_ERR, "Invalid API port: %s", arg);
            exit_with_usage(1);
        }
        opt_api_port = parsed_port;
        opt_api_port_explicit = true;
        return;
    }

    if (validate_ipv4_address(arg)) {
        replace_config_string(&opt_api_bind, arg);
        return;
    }

    applog(LOG_ERR, "API bind address is not a valid IPv4 address: %s", arg);
    exit_with_usage(1);
}

static void load_config_file(const char *path)
{
    json_error_t err;

    release_loaded_config();

    opt_config = json_load_file(path, 0, &err);
    if (!opt_config) {
        applog(LOG_ERR, "%s: %s", path, err.text[0] ? err.text : strerror(errno));
        exit(1);
    }
    if (!json_is_object(opt_config)) {
        applog(LOG_ERR, "%s: root value is not a JSON object", path);
        exit(1);
    }
}

void pool_init_defaults(void)
{
    clear_configured_pool_set();
    clear_pool_option_defaults();
}

static void apply_option(int key, const char *arg)
{
    int value;
    const char *separator;
    algo_t parsed_algo;

    switch(key) {
    case 'a':
        if (!parse_algorithm_name(arg, &parsed_algo)) {
            applog(LOG_ERR, "Unknown algorithm: %s", arg);
            applog(LOG_ERR, "Supported: verus, sha256d, scrypt, randomx, civiclight");
            exit_with_usage(1);
        }
        opt_algo = parsed_algo;
        break;
    case 'b':
        parse_api_bind_argument(arg);
        break;
    /* No 'c' case: config files are loaded during preparse_cmdline(), the main
     * getopt loop skips 'c', and apply_config_object() skips the "config" key.
     * Routing 'c' here would re-load a config file mid-parse. */
    case 'D':
        opt_debug = true;
        break;
    case 'N':
        value = parse_cli_int_option(arg, "-N/--statsavg");
        if (value < 1)
            value = 1;
        opt_statsavg = value;
        break;
    case 'o':
        if (config_uses_pool_array) {
            applog(LOG_WARNING,
                   "CLI pool URL override (-o/--url) selected single-pool mode; ignoring configured pools[]");
            clear_configured_pool_set();
        }
        /* The CLI is single-pool: unlike ccminer, repeated -o flags do not
         * accumulate a failover list — each one replaces the previous URL.
         * Warn only for a second -o within the CLI phase, not for the
         * legitimate "CLI -o overrides config.json url" pattern. */
        if (cli_parse_phase) {
            if (cli_url_seen && strcmp(pool_defaults.url, arg) != 0) {
                applog(LOG_WARNING,
                       "Multiple -o/--url options given; using the last one (%s). "
                       "For failover pools use a config.json \"pools\" array.", arg);
            }
            cli_url_seen = true;
        }
        set_pool_default_url(arg);
        if (cur_pooln >= 0 && cur_pooln < MAX_POOLS) {
            copy_pool_string(pools[cur_pooln].url, sizeof(pools[cur_pooln].url), arg, "url");
            derive_pool_short_url(cur_pooln);
            apply_pool_default_credentials(cur_pooln, true, true, false);
        }
        break;
    case 'O':
        separator = strchr(arg, ':');
        if (!separator) {
            applog(LOG_ERR, "Invalid userpass format (use user:pass)");
            exit_with_usage(1);
        }
        set_pool_default_user_span(arg, (size_t)(separator - arg));
        set_pool_default_pass(separator + 1);
        sync_configured_pool_defaults(true, true);
        break;
    case 'p':
        set_pool_default_pass(arg);
        sync_configured_pool_defaults(false, true);
        break;
    case 'P':
        opt_protocol = true;
        break;
    case 'q':
        opt_quiet = true;
        break;
    case 'r':
        value = parse_cli_int_option(arg, "-r/--retries");
        if (value < -1)
            value = -1;
        opt_retries = value;
        break;
    case 'R':
        value = parse_cli_int_option(arg, "-R/--retry-pause");
        if (value < 1)
            value = 1;
        opt_retry_pause = value;
        break;
    case 't':
        value = parse_cli_int_option(arg, "-t/--threads");
        if (value < 1 || value > MAX_THREADS) {
            applog(LOG_ERR, "Invalid thread count: %d", value);
            exit_with_usage(1);
        }
        opt_n_threads = value;
        break;
    case 'T':
        value = parse_cli_int_option(arg, "-T/--timeout");
        if (value < 1)
            value = 1;
        opt_timeout = value;
        sync_configured_pool_timeouts(value);
        break;
    case 'u':
        set_pool_default_user(arg);
        sync_configured_pool_defaults(true, false);
        break;
    case 'V':
        printf("%s %s\n", PACKAGE_NAME, PACKAGE_VERSION);
        printf("Built for ARM Cortex-A53-tuned native NEON optimizations\n");
        exit(0);
    case 'h':
        exit_with_usage(0);
    case CLI_OPT_BENCHMARK:
        opt_benchmark = true;
        break;
    case CLI_OPT_NICEHASH:
        opt_nicehash = true;
        break;
    case CLI_OPT_CPU_AFFINITY:
        opt_affinity_mask = parse_affinity_mask_argument(arg);
        opt_affinity_set = true;
        break;
    case CLI_OPT_CPU_PRIORITY:
        value = parse_cli_int_option(arg, "--cpu-priority");
        if (value < 0)
            value = 0;
        if (value > 5)
            value = 5;
        opt_priority = value;
        break;
    default:
        exit_with_usage(1);
    }
}

static void apply_configured_pool(json_t *pool_config, size_t pool_index)
{
    json_t *name = json_object_get(pool_config, "name");
    json_t *url = json_object_get(pool_config, "url");
    json_t *user = json_object_get(pool_config, "user");
    json_t *pass = json_object_get(pool_config, "pass");
    json_t *disabled = json_object_get(pool_config, "disabled");
    json_t *timeout = json_object_get(pool_config, "timeout");
    const char *url_value = NULL;

    if (pool_index >= MAX_USER_POOLS)
        return;

    if (url && json_is_string(url))
        url_value = json_string_value(url);

    if (!url_value || !url_value[0]) {
        applog(LOG_WARNING, "Ignoring pool entry %zu with no URL", pool_index);
        return;
    }

    if (name && json_is_string(name))
        copy_pool_string(pools[pool_index].name, sizeof(pools[pool_index].name),
                         json_string_value(name), "name");

    copy_pool_string(pools[pool_index].url, sizeof(pools[pool_index].url), url_value, "url");

    derive_pool_short_url((int)pool_index);

    pool_config_user_set[pool_index] = user && json_is_string(user);
    pool_config_pass_set[pool_index] = pass && json_is_string(pass);

    if (pool_config_user_set[pool_index])
        copy_pool_string(pools[pool_index].user, sizeof(pools[pool_index].user),
                         json_string_value(user), "user/wallet");

    if (pool_config_pass_set[pool_index])
        copy_pool_string(pools[pool_index].pass, sizeof(pools[pool_index].pass),
                         json_string_value(pass), "password");

    apply_pool_default_credentials((int)pool_index, true, true, true);

    pools[pool_index].disabled = parse_json_boolish(disabled);
    pools[pool_index].timeout = parse_pool_timeout_json(timeout);
    register_pool_index((int)pool_index);
}

static void apply_json_scalar_option(const struct option *option, json_t *value)
{
    if (option->has_arg && json_is_string(value)) {
        apply_option(option->val, json_string_value(value));
        return;
    }

    if (option->has_arg && json_is_integer(value)) {
        /* Range-check BEFORE narrowing: (int)4294967297 would silently
         * become 1 (e.g. "threads") instead of being rejected. */
        json_int_t raw = json_integer_value(value);
        if (raw < INT_MIN || raw > INT_MAX) {
            applog(LOG_ERR, "Invalid value for %s: %lld (out of range)",
                   option->name, (long long)raw);
            exit_with_usage(1);
        }
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", (int)raw);
        apply_option(option->val, buf);
        return;
    }

    if (!option->has_arg && json_is_true(value))
        apply_option(option->val, "");
}

static void apply_config_object(json_t *config)
{
    if (!json_is_object(config))
        return;

    for (int option_index = 0; g_cli_options[option_index].name; option_index++) {
        const struct option *option = &g_cli_options[option_index];
        json_t *value;

        if (!strcmp(option->name, "config"))
            continue;

        value = json_object_get(config, option->name);
        if (!value)
            continue;

        apply_json_scalar_option(option, value);
    }

    json_t *pools_arr = json_object_get(config, "pools");
    if (pools_arr && json_is_array(pools_arr)) {
        size_t pool_count = json_array_size(pools_arr);
        if (pool_count > 0)
            config_uses_pool_array = true;
        for (size_t pool_index = 0; pool_index < pool_count && pool_index < MAX_USER_POOLS; pool_index++) {
            json_t *pool_config = json_array_get(pools_arr, pool_index);
            if (!json_is_object(pool_config))
                continue;

            apply_configured_pool(pool_config, pool_index);
        }
    }
}

static void normalize_legacy_arguments(int argc, char *argv[])
{
    for (int arg_index = 1; arg_index < argc; arg_index++) {
        if (argv[arg_index] && strcmp(argv[arg_index], "-benchmark") == 0)
            argv[arg_index] = (char *)"--benchmark";
    }
}

static void ensure_cli_pool_is_registered(void)
{
    if (num_pools != 0 || !pool_defaults.url_set)
        return;

    copy_pool_string(pools[0].url, sizeof(pools[0].url), pool_defaults.url, "url");
    derive_pool_short_url(0);
    apply_pool_default_credentials(0, true, true, false);

    register_pool_index(0);
}

void parse_cmdline(int argc, char *argv[])
{
    struct cmdline_preparse_result preparse;
    const char *config_path;
    int key;

    normalize_legacy_arguments(argc, argv);
    pool_init_defaults();
    miner_set_current_pool_index(0);
    preparse = preparse_cmdline(argc, argv);

    if (!preparse.immediate_exit) {
        config_path = preparse.config_path;
        if (config_path) {
            load_config_file(config_path);
            log_loaded_config_path(config_path);
        } else {
            maybe_load_default_config();
        }
    }

    if (opt_config) {
        apply_config_object(opt_config);
        release_loaded_config();
    }

    reset_getopt_state();
    cli_parse_phase = true;
    cli_url_seen = false;
    while (1) {
        key = getopt_long(argc, argv, "a:b:c:Dhp:Pqr:R:t:T:o:u:O:VN:", g_cli_options, NULL);
        if (key < 0)
            break;
        if (key == 'c')
            continue;

        apply_option(key, optarg);
    }
    cli_parse_phase = false;

    if (opt_n_threads == 0)
        opt_n_threads = normalize_thread_count(sysconf(_SC_NPROCESSORS_ONLN), true);
    else
        opt_n_threads = normalize_thread_count(opt_n_threads, false);

    ensure_cli_pool_is_registered();

    if (opt_benchmark)
        return;

    if (num_pools == 0) {
        applog(LOG_ERR, "No pool URL specified");
        exit_with_usage(1);
    }

    key = find_first_usable_pool_index();
    if (key < 0) {
        applog(LOG_ERR, "No enabled pool URL specified");
        exit_with_usage(1);
    }

    miner_set_current_pool_index(key);
}
