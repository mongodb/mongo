/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * Entry point: parse the command line, then dispatch to the parent or node role. See
 * schema_disagg_abort.h for the overall test structure.
 */

#include "schema_disagg_abort.h"

#include "subproc.h"

extern int __wt_optind;
extern char *__wt_optarg;

static void usage(void) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));

/*
 * println --
 *     Print one progress line, immediately flushed so it survives a SIGKILL and interleaves
 *     usefully with the other processes' output.
 */
void
println(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

/*
 * timestamp_name --
 *     Return the configuration name for a timestamp bit.
 */
static const char *
timestamp_name(uint8_t bit)
{
    switch (bit) {
    case TS_OLDEST:
        return ("oldest_timestamp");
    case TS_STABLE:
        return ("stable_timestamp");
    case TS_STABLE_SCHEMA_EPOCH:
        return ("stable_disaggregated_schema_epoch");
    case TS_STEPDOWN_TIMESTAMP:
        return ("step_down_timestamp");
    case TS_STEPDOWN_SCHEMA_EPOCH:
        return ("step_down_disaggregated_schema_epoch");
    case TS_LAST_SCHEMA_EPOCH:
        return ("last_disaggregated_schema_epoch");
    case TS_LAST_CHECKPOINT:
        return ("last_checkpoint");
    default:
        testutil_die(EINVAL, "unknown timestamp bit: %#" PRIx8, bit);
    }
}

/*
 * query_ts --
 *     Return one of the connection's timestamps as an integer. A timestamp that was never set reads
 *     as zero.
 */
uint64_t
query_ts(WT_CONNECTION *conn, uint8_t bit)
{
    char config[64], hex_ts[64];
    testutil_snprintf(config, sizeof(config), "get=%s", timestamp_name(bit));
    testutil_check(conn->query_timestamp(conn, hex_ts, config));

    uint64_t ts = 0;
    (void)sscanf(hex_ts, "%" SCNx64, &ts);
    return (ts);
}

/*
 * set_ts --
 *     Set the selected connection timestamps to given value.
 */
void
set_ts(const TEST_CONFIG *cfg, WT_CONNECTION *conn, uint8_t mask, uint64_t ts)
{
    if (cfg->epoch_less)
        mask &= (uint8_t)~TS_SCHEMA_EPOCHS;
    testutil_assert(mask != 0);

    char config[256];
    size_t len = 0;
    for (uint8_t bit = 1; bit != 0; bit = (uint8_t)(bit << 1)) {
        if ((mask & bit) == 0)
            continue;
        testutil_snprintf_len_incr(
          config + len, sizeof(config) - len, &len, "%s=%" PRIx64 ",", timestamp_name(bit), ts);
    }
    testutil_check(conn->set_timestamp(conn, config));
}

/*
 * adopted_lsn_publish --
 *     Report the latest adopted checkpoint LSN for a stepping-down peer to wait on.
 */
void
adopted_lsn_publish(uint32_t node_id, uint64_t lsn)
{
    /* Write to a temporary file first, so a reader never sees a partial value */
    char tmp[64];
    testutil_snprintf(tmp, sizeof(tmp), ADOPTED_LSN_FILE ".%" PRIu32, node_id);

    FILE *fp;
    testutil_assert_errno((fp = fopen(tmp, "w")) != NULL);
    testutil_assert(fprintf(fp, "%" PRIu64 "\n", lsn) > 0);
    testutil_check(fclose(fp));
    /* Publish the LSN. */
    testutil_assert_errno(rename(tmp, ADOPTED_LSN_FILE) == 0);
}

/*
 * adopted_lsn_read --
 *     Return the peer's last reported adopted checkpoint LSN; zero when none yet.
 */
uint64_t
adopted_lsn_read(void)
{
    FILE *fp = fopen(ADOPTED_LSN_FILE, "r");
    if (fp == NULL)
        return (0);

    uint64_t lsn = 0;
    testutil_ignore_ret(fscanf(fp, "%" SCNu64, &lsn));
    testutil_check(fclose(fp));
    return (lsn);
}

/*
 * usage --
 *     Print the command-line usage and exit. The -A/-i/-R/-W options and the "-r node" value are
 *     internal: the parent uses them to spawn its nodes.
 */
static void
usage(void)
{
    fprintf(stderr,
      "usage: %s [-b build-dir] [-e] [-h dir] [-k [l|f]N] [-p] [-r l|f|lf] [-s N] [-T threads] "
      "[-t time] [-q] [-u pool] [-v]\n",
      progname);
    fprintf(stderr, "%s",
      "\t-b build directory (required for PALite extension)\n"
      "\t-e run legacy schema operations without epochs (single node only)\n"
      "\t-h home directory\n"
      "\t-k kill after N seconds: lN the current leader, fN the current follower (two nodes),\n"
      "\t   plain N the lone node (single node); may be given more than once\n"
      "\t-p preserve directory contents\n"
      "\t-r roles to run: l leader, f follower, lf both; default: a random single node\n"
      "\t-s switch roles every N seconds\n"
      "\t-T number of schema threads\n"
      "\t-t total run time in seconds; the nodes stop gracefully unless killed\n"
      "\t-q give every create a fresh table name, so no name is ever reused\n"
      "\t-u URI pool size per thread\n"
      "\t-v verify only\n");
    exit(EXIT_FAILURE);
}

/*
 * parse_uint_in_range --
 *     Parse a numeric option value, enforcing an inclusive range.
 */
static uint32_t
parse_uint_in_range(const char *arg, uint32_t min, uint32_t max, const char *what)
{
    const uint32_t value = (uint32_t)atoi(arg);
    if (value < min || value > max) {
        fprintf(stderr, "%s must be between %" PRIu32 " and %" PRIu32 "\n", what, min, max);
        usage();
    }
    return (value);
}

/*
 * parse_roles --
 *     Translate the -r option value: the public topology letters, or the internal "node" value the
 *     parent spawns children with.
 */
static void
parse_roles(TEST_CONFIG *cfg, const char *arg)
{
    if (strcmp(arg, "node") == 0) {
        cfg->role = ROLE_NODE;
        return;
    }
    for (const char *p = arg; *p != '\0'; p++)
        if (*p == 'l')
            cfg->with_leader = true;
        else if (*p == 'f')
            cfg->with_follower = true;
        else
            usage();
    if (!cfg->with_leader && !cfg->with_follower)
        usage();
}

/* Command-line prefixes of the kill targets, indexed by KILL_TARGET. */
static const char *const kill_prefix[KILL_TARGETS] = {"", "l", "f"};

/*
 * parse_kill_spec --
 *     Translate one -k option value: "lN" kills the current leader at N seconds, "fN" the current
 *     follower, a plain "N" the lone node.
 */
static void
parse_kill_spec(TEST_CONFIG *cfg, const char *arg)
{
    KILL_TARGET target = KILL_LONE;
    const char *num = arg;

    if (arg[0] == 'l') {
        target = KILL_LEADER;
        ++num;
    } else if (arg[0] == 'f') {
        target = KILL_FOLLOWER;
        ++num;
    }

    const uint32_t value = (uint32_t)atoi(num);
    if (value == 0 || cfg->kill_time[target] != 0)
        usage();
    cfg->kill_time[target] = value;
}

/*
 * parse_args --
 *     Parse the command line into the configuration and derive the path fields. Reports whether the
 *     thread count and total time were left to be randomized.
 */
static void
parse_args(TEST_CONFIG *cfg, int argc, char *argv[], bool *rand_thp, bool *rand_timep)
{
    bool pool_size_set = false;

    *rand_thp = *rand_timep = true;

    testutil_parse_begin_opt(argc, argv, "A:b:eh:i:k:pP:r:R:s:t:T:u:vW:q", cfg->opts);

    int ch;
    while ((ch = __wt_getopt(progname, argc, argv, "A:b:eh:i:k:pP:r:R:s:t:T:u:vW:q")) != EOF)
        switch (ch) {
        case 'A':
            if (strcmp(__wt_optarg, "l") == 0)
                cfg->start_leader = true;
            else if (strcmp(__wt_optarg, "f") == 0)
                cfg->start_leader = false;
            else
                usage();
            break;
        case 'e':
            cfg->epoch_less = true;
            break;
        case 'i':
            cfg->node_id = parse_uint_in_range(__wt_optarg, 0, MAX_NODES - 1, "Node id");
            break;
        case 'k':
            parse_kill_spec(cfg, __wt_optarg);
            break;
        case 'r':
            parse_roles(cfg, __wt_optarg);
            break;
        case 'R':
            cfg->pipe_read_fd = atoi(__wt_optarg);
            break;
        case 's':
            cfg->switch_interval = parse_uint_in_range(__wt_optarg, 1, UINT32_MAX, "Interval");
            break;
        case 't':
            *rand_timep = false;
            cfg->total_time = (uint32_t)atoi(__wt_optarg);
            break;
        case 'T':
            *rand_thp = false;
            cfg->thread_count = parse_uint_in_range(__wt_optarg, 1, MAX_TH, "Thread count");
            break;
        case 'q':
            cfg->unique_tables = true;
            break;
        case 'u':
            pool_size_set = true;
            cfg->pool_size =
              parse_uint_in_range(__wt_optarg, MIN_POOL_SIZE, MAX_POOL_SIZE, "Pool size");
            break;
        case 'v':
            cfg->verify_only = true;
            break;
        case 'W':
            cfg->pipe_write_fd = atoi(__wt_optarg);
            break;
        default:
            if (testutil_parse_single_opt(cfg->opts, ch) != 0)
                usage();
        }
    if (argc - __wt_optind != 0)
        usage();
    if (cfg->verify_only && *rand_thp) {
        fprintf(stderr, "Verify requires -T\n");
        exit(EXIT_FAILURE);
    }
    if (cfg->verify_only && !pool_size_set) {
        fprintf(stderr, "Verify requires -u\n");
        exit(EXIT_FAILURE);
    }

    cfg->opts->disagg.is_enabled = true;
    testutil_parse_end_opt(cfg->opts);
    testutil_work_dir_from_path(cfg->home, sizeof(cfg->home), cfg->opts->home);

    char cwd[PATH_MAX];
    testutil_assert_errno(getcwd(cwd, sizeof(cwd)) != NULL);
    testutil_snprintf(
      cfg->page_log_home, sizeof(cfg->page_log_home), "%s/%s/%s", cwd, cfg->home, PAGE_LOG_DIR);
}

/*
 * randomize_run_parameters --
 *     Choose random values for the parameters not fixed on the command line. The data random stream
 *     is consumed unconditionally to keep it in sync between runs with and without -T.
 */
static void
randomize_run_parameters(TEST_CONFIG *cfg, bool rand_th, bool rand_time)
{
    if (rand_time) {
        cfg->total_time = __wt_random(&cfg->opts->extra_rnd) % MAX_TIME;
        if (cfg->total_time < MIN_TIME)
            cfg->total_time = MIN_TIME;
    }

    const uint32_t rand_value = __wt_random(&cfg->opts->data_rnd);
    if (rand_th) {
        cfg->thread_count = rand_value % MAX_TH;
        if (cfg->thread_count < MIN_TH)
            cfg->thread_count = MIN_TH;
    }

    /* No -r: run a random single node. */
    if (!cfg->with_leader && !cfg->with_follower) {
        if ((__wt_random(&cfg->opts->data_rnd) & 1) != 0)
            cfg->with_leader = true;
        else
            cfg->with_follower = true;
    }
}

/*
 * validate_run_parameters --
 *     Reject option combinations that make no sense for the resolved topology.
 */
static void
validate_run_parameters(const TEST_CONFIG *cfg)
{
    const bool multi_node = cfg->with_leader && cfg->with_follower;

    if (cfg->kill_time[KILL_LONE] != 0 && multi_node) {
        fprintf(stderr, "-k N targets the lone node; use -k lN / -k fN with -r lf\n");
        usage();
    }
    if ((cfg->kill_time[KILL_LEADER] != 0 || cfg->kill_time[KILL_FOLLOWER] != 0) && !multi_node) {
        fprintf(stderr, "-k lN / -k fN target roles; use plain -k N with a single node\n");
        usage();
    }
    if (cfg->epoch_less && multi_node) {
        fprintf(
          stderr, "-e supports single-node roles only; schema epochs are required with -r lf\n");
        usage();
    }
}

/*
 * roles_arg --
 *     Return the -r fragment reproducing the resolved topology.
 */
static const char *
roles_arg(const TEST_CONFIG *cfg)
{
    if (cfg->with_leader && cfg->with_follower)
        return ("lf");
    return (cfg->with_leader ? "l" : "f");
}

/*
 * print_run_banner --
 *     Report the effective run parameters, including the CONFIG line that reproduces the run.
 */
static void
print_run_banner(const TEST_CONFIG *cfg)
{
    println("Parent: roles %s; %" PRIu32 " schema threads; pool %" PRIu32
            " slots; switch every %" PRIu32 "s; kill leader@%" PRIu32 " follower@%" PRIu32
            " lone@%" PRIu32 "; stop at %" PRIu32 "s",
      roles_arg(cfg), cfg->thread_count, cfg->pool_size, cfg->switch_interval,
      cfg->kill_time[KILL_LEADER], cfg->kill_time[KILL_FOLLOWER], cfg->kill_time[KILL_LONE],
      cfg->total_time);

    char switch_arg[32] = "", kill_args[64] = "";
    if (cfg->switch_interval != 0)
        testutil_snprintf(switch_arg, sizeof(switch_arg), " -s %" PRIu32, cfg->switch_interval);
    size_t len = 0;
    for (int k = 0; k < KILL_TARGETS; k++)
        if (cfg->kill_time[k] != 0)
            testutil_snprintf_len_incr(kill_args, sizeof(kill_args), &len, " -k %s%" PRIu32,
              kill_prefix[k], cfg->kill_time[k]);

    println("CONFIG: %s%s -r %s%s%s -u %" PRIu32 " -T %" PRIu32 " -t %" PRIu32
            " " TESTUTIL_SEED_FORMAT,
      progname, cfg->epoch_less ? " -e" : "", roles_arg(cfg), switch_arg, kill_args, cfg->pool_size,
      cfg->thread_count, cfg->total_time, cfg->opts->data_seed, cfg->opts->extra_seed);
}

/*
 * main --
 *     Parse arguments and run the requested role.
 */
int
main(int argc, char *argv[])
{
    static TEST_OPTS s_opts;

    (void)testutil_set_progname(argv);

    TEST_CONFIG cfg = {0};
    cfg.opts = &s_opts;
    cfg.thread_count = MIN_TH;
    /*
     * Default: 32 slots per thread. A wide pool keeps the generator's picks off the few slots gated
     * behind a checkpoint, so it rarely comes up empty and has to wait.
     */
    cfg.pool_size = MAX_POOL_SIZE / 2;
    cfg.total_time = MIN_TIME;
    cfg.pipe_read_fd = cfg.pipe_write_fd = -1;
    cfg.self_pipe_read_fd = cfg.self_pipe_write_fd = -1;

    bool rand_th, rand_time;
    parse_args(&cfg, argc, argv, &rand_th, &rand_time);

    /* The node role gets its full configuration from the command line; just run it. */
    if (cfg.role == ROLE_NODE)
        return (node_main(&cfg));

    if (!cfg.verify_only) {
        randomize_run_parameters(&cfg, rand_th, rand_time);
        validate_run_parameters(&cfg);
        print_run_banner(&cfg);
    }

    char self_path[PATH_MAX];
    subproc_self_path(argv[0], self_path, sizeof(self_path));

    parent_main(&cfg, self_path);
    return (EXIT_SUCCESS);
}
