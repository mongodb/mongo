/*-
 * Public Domain 2014-present MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "format.h"

GLOBAL g;

TABLE *tables[V_MAX_TABLES_CONFIG + 1]; /* Table array */
u_int ntables;

static void format_die(void);
static void usage(void) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));

extern int __wt_optind;
extern char *__wt_optarg;

static void signal_handler(int signo) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));
static void signal_timer(int signo) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));

/*
 * signal_handler --
 *     Generic signal handler, report the signal and exit.
 */
static void
signal_handler(int signo)
{
    fprintf(stderr, "format caught signal %d, exiting without error\n", signo);
    fflush(stderr);
    exit(EXIT_SUCCESS);
}

/*
 * signal_timer --
 *     Alarm signal handler.
 */
static void
signal_timer(int signo)
{

    /* Note, format.sh checks for this message, so be cautious in changing the format. */
    fprintf(stderr, "format alarm timed out\n");
    fprintf(stderr, "format caught signal %d, exiting with error\n", signo);
    fprintf(stderr, "format attempting to create a core dump\n");
    fflush(stderr);
    __wt_abort(NULL);
    /* NOTREACHED */
}

/*
 * set_alarm --
 *     Set a timer.
 */
void
set_alarm(u_int seconds)
{
#ifdef HAVE_TIMER_CREATE
    struct itimerspec timer_val;
    timer_t timer_id;

    testutil_check(timer_create(CLOCK_REALTIME, NULL, &timer_id));
    memset(&timer_val, 0, sizeof(timer_val));
    timer_val.it_value.tv_sec = seconds;
    timer_val.it_value.tv_nsec = 0;
    testutil_check(timer_settime(timer_id, 0, &timer_val, NULL));
#endif
    (void)seconds;
}

/*
 * format_process_env --
 *     Set up the format process environment.
 */
static void
format_process_env(void)
{
/*
 * Windows and Linux support different sets of signals, be conservative about installing handlers.
 * If we time out unexpectedly, we want a core dump, otherwise, just exit.
 */
#ifdef SIGALRM
    (void)signal(SIGALRM, signal_timer);
#endif
#ifdef SIGHUP
    (void)signal(SIGHUP, signal_handler);
#endif
#ifdef SIGTERM
    (void)signal(SIGTERM, signal_handler);
#endif

#if 0
    /* Configure the GNU malloc for debugging. */
    (void)setenv("MALLOC_CHECK_", "2", 1);
#endif
#if 0
    /* Configure the FreeBSD malloc for debugging. */
    (void)setenv("MALLOC_OPTIONS", "AJ", 1);
#endif
}

/*
 * locks_init --
 *     Initialize locks to single-thread backups and timestamps.
 */
static void
locks_init(WT_CONNECTION *conn)
{
    WT_SESSION *session;

    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    lock_init(session, &g.backup_lock);
    lock_init(session, &g.prepare_commit_lock);
    testutil_check(session->close(session, NULL));
}

/*
 * locks_destroy --
 *     Discard locks to single-thread backups and timestamps.
 */
static void
locks_destroy(WT_CONNECTION *conn)
{
    WT_SESSION *session;

    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    lock_destroy(session, &g.backup_lock);
    lock_destroy(session, &g.prepare_commit_lock);
    testutil_check(session->close(session, NULL));
}

/*
 * TIMED_MAJOR_OP --
 *	Set a timer and perform a major operation (for example, verify or salvage).
 */
#define TIMED_MAJOR_OP(call)                          \
    do {                                              \
        if (GV(FORMAT_MAJOR_TIMEOUT) != 0)            \
            set_alarm(GV(FORMAT_MAJOR_TIMEOUT) * 60); \
        call;                                         \
        if (GV(FORMAT_MAJOR_TIMEOUT) != 0)            \
            set_alarm(0);                             \
    } while (0)

static bool syntax_check; /* Only checking configuration syntax. */

/*
 * main --
 *     Run a variety of multithreaded WiredTiger operations based on a set of configurations.
 */
int
main(int argc, char *argv[])
{
    READ_SCAN_ARGS scan_args;
    WT_DECL_RET;
    uint64_t now, start;
    u_int leader_ops_seconds, ops_seconds, reps;
    int ch;
    const char *config, *home;
    bool is_backup, quiet_flag, verify_only;

    custom_die = format_die; /* Local death handler. */

    config = NULL;

    (void)testutil_set_progname(argv);

    format_process_env();

    /*
     * If built in a branch that doesn't support all current options, configure for backward
     * compatibility.
     */
#if WIREDTIGER_VERSION_MAJOR < 10
    g.backward_compatible = true;
#endif

    /* Set values from the command line. */
    home = NULL;
    quiet_flag = syntax_check = verify_only = false;
    while ((ch = __wt_getopt(progname, argc, argv, "1BC:c:h:qRSrT:v")) != EOF)
        switch (ch) {
        case '1':
            /* Ignored for backward compatibility. */
            break;
        case 'B': /* Backward compatibility */
            g.backward_compatible = true;
            break;
        case 'C': /* wiredtiger_open config */
            g.config_open = __wt_optarg;
            break;
        case 'c': /* Read configuration from a file */
            config = __wt_optarg;
            break;
        case 'h':
            home = __wt_optarg;
            break;
        case 'q': /* Quiet */
            quiet_flag = true;
            break;
        case 'R': /* Reopen (start running on an existing database) */
            g.reopen = true;
            break;
        case 'S': /* Configuration syntax check */
            syntax_check = true;
            break;
        case 'T': /* Trace  */
            trace_config(__wt_optarg);
            break;
        case 'v': /* Verify only */
            verify_only = true;
            break;
        default:
            usage();
        }
    argv += __wt_optind;

    /* format.sh looks for this line in the log file, push it out quickly. */
    if (!syntax_check) {
        printf("%s: process %" PRIdMAX " running\n", progname, (intmax_t)getpid());
        fflush(stdout);
    }

    /*
     * Initialize the RNGs. This is needed early because some random decisions are made while
     * reading configuration. There may be random seeds in the configuration, however, so we will
     * reinitialize the RNGs later.
     */
    __wt_random_init(NULL, &g.data_rnd);
    __wt_random_init(NULL, &g.extra_rnd);

    /* Initialize lock to ensure single threading during failure handling. */
    testutil_check(pthread_rwlock_init(&g.death_lock, NULL));

    /* Initialize lock to ensure single threading for lane operations in predictable replay. */
    testutil_check(pthread_rwlock_init(&g.lane_lock, NULL));

    /*
     * Initialize the tables array and default to multi-table testing if not in backward-compatible
     * mode.
     */
    tables[0] = dcalloc(1, sizeof(TABLE));
    tables[0]->id = 1;
    g.multi_table_config = !g.backward_compatible;

    /* Set up paths. */
    path_setup(home);

    /*
     * If it's a reopen, use the already existing home directory's CONFIG file. Otherwise, if we
     * weren't given a configuration file, set values from "CONFIG", if it exists. Small hack to
     * ignore any CONFIG file named ".", that just makes it possible to ignore any local CONFIG
     * file, used when running checks.
     */
    if (g.reopen) {
        if (access(g.home_config, R_OK) != 0)
            testutil_die(ENOENT, "%s", g.home_config);

        /*
         * The test uses an existing config file in the home directory and ignores the provided
         * config file during reopen. Log the new config file that is used.
         */
        if (config != NULL && strcmp(config, g.home_config) != 0) {
            printf(
              "Provided configuration file is ignored. %s config file is used.\n", g.home_config);
            fflush(stdout);
        }
        config = g.home_config;
    }
    if (config == NULL && access("CONFIG", R_OK) == 0)
        config = "CONFIG";
    if (config != NULL && strcmp(config, ".") != 0)
        config_file(config);

    /*
     * Remaining arguments are individual configurations that modify the base configuration. Note
     * there's no restriction on command-line arguments when re-playing or re-opening a database,
     * which can lead to a lot of hurt if you're not careful.
     */
    for (; *argv != NULL; ++argv)
        config_single(NULL, *argv, true);

    /*
     * Let the command line -q flag override values configured from other sources. Regardless, don't
     * go all verbose if we're not talking to a terminal.
     */
    if (quiet_flag || !isatty(1))
        GV(QUIET) = 1;

    /* Configure the run. */
    config_run();
    g.configured = true;

    /* If checking a CONFIG file syntax, we're done. */
    if (syntax_check)
        exit(0);

    __wt_seconds(NULL, &start);
    track("starting up", 0ULL);

    /* Create and open, or reopen the database. */
    if (g.reopen) {
        if (GV(RUNS_IN_MEMORY))
            testutil_die(0, "reopen impossible after in-memory run");
        /*
         * If we are reopening from a backup then we do not want to verify the metadata. That is, if
         * it is a backup, send false to wts_open. If we are a normal reopen then send in true to
         * wts_open.
         */
        is_backup = false;
        if (access(g.home_backup, F_OK) == 0)
            is_backup = true;
        wts_open(g.home, &g.wts_conn, !is_backup);
        timestamp_init();
        /* Update the oldest and stable timestamps if they have been previously set. */
        ret = timestamp_query("get=oldest_timestamp", &g.oldest_timestamp);
        testutil_assert(ret == 0 || ret == WT_NOTFOUND);
        ret = timestamp_query("get=stable_timestamp", &g.stable_timestamp);
        testutil_assert(ret == 0 || ret == WT_NOTFOUND);
    } else {
        wts_create_home();
        config_print(false);
        trace_init();
        wts_create_database();
        wts_open(g.home, &g.wts_conn, true);
        timestamp_init();
    }

    wts_prepare_discover(g.wts_conn);
    locks_init(g.wts_conn);

    /*
     * Initialize key/value information. Load and verify initial records (at least a brief scan if
     * not doing a full verify).
     */
    tables_apply(key_init, NULL);
    tables_apply(val_init, NULL);
    if (!g.reopen)
        TIMED_MAJOR_OP(wts_load());
    TIMED_MAJOR_OP(wts_verify(g.wts_conn, true));
    if (verify_only)
        goto skip_operations;
    scan_args.conn = g.wts_conn;
    scan_args.rnd = &g.extra_rnd;
    TIMED_MAJOR_OP(tables_apply(wts_read_scan, &scan_args));

    /* Optionally start checkpoints. */
    wts_checkpoints();

    /*
     * Calculate how long each operations loop should run. Take any timer value and convert it to
     * seconds, then allocate 15 seconds to do initialization, verification and/or salvage tasks
     * after the operations loop finishes. This is not intended to be exact in any way, just enough
     * to get us into an acceptable range of run times. The reason for this is because we want to
     * consume the legitimate run-time, but we also need to do the end-of-run checking in all cases,
     * even if we run out of time, otherwise it won't get done. So, in summary pick a reasonable
     * time and then don't check for timer expiration once the main operations loop completes.
     */
    ops_seconds = GV(RUNS_TIMER) == 0 ? 0 : ((GV(RUNS_TIMER) * 60) - 15) / FORMAT_OPERATION_REPS;
    if (!disagg_is_mode_switch())
        for (reps = 1; reps <= FORMAT_OPERATION_REPS; ++reps)
            operations(ops_seconds, reps, FORMAT_OPERATION_REPS);
    else {
        /*
         * In disagg "switch" mode, we alternate between leader and follower roles. The follower
         * runs only for a short, fixed duration (i.e. DISAGG_SWITCH_FOLLOWER_OPS_SEC), as it's
         * expected to generate minimal cache activity. Content written in follower mode is not
         * evictable, extended time in this role can lead to cache overflow. The leader occupies the
         * remaining time.
         */
        leader_ops_seconds = ops_seconds != 0 ? (ops_seconds - DISAGG_SWITCH_FOLLOWER_OPS_SEC) : 0;

        for (reps = 1; reps <= (FORMAT_OPERATION_REPS * 2); ++reps) {
            ops_seconds = g.disagg_leader ? leader_ops_seconds : DISAGG_SWITCH_FOLLOWER_OPS_SEC;
            operations(ops_seconds, reps, (FORMAT_OPERATION_REPS * 2));
            testutil_check(disagg_switch_roles());
        }
    }

    /* Copy out the run's statistics. */
    TIMED_MAJOR_OP(wts_stats());

skip_operations:
    locks_destroy(g.wts_conn);

    /*
     * Verify the objects. Verify closes the underlying handle and discards the statistics, read
     * them first. However, for verify_only we've already completed the verification above. Skip
     * this one.
     */
    if (!verify_only)
        TIMED_MAJOR_OP(wts_verify(g.wts_conn, true));

    track("shutting down", 0ULL);
    wts_close(&g.wts_conn);

    /* Salvage testing. */
    if (!verify_only)
        TIMED_MAJOR_OP(tables_apply(wts_salvage, NULL));

    trace_teardown();

    /* Overwrite the progress line with a completion line. */
    if (!GV(QUIET))
        printf("\r%78s\r", " ");
    __wt_seconds(NULL, &now);
    printf("%s: successful run completed (%" PRIu64 " seconds)\n ", progname, now - start);
    fflush(stdout);

    config_clear();

    return (EXIT_SUCCESS);
}

/*
 * format_die --
 *     Report an error, dumping the configuration.
 */
static void
format_die(void)
{
    /* If only checking configuration syntax, no need to message or drop core. */
    if (syntax_check)
        exit(1);

    /*
     * Turn off progress reports and tracing so we don't obscure the error message or drop core when
     * using a session that's being closed. The lock we're about to acquire will act as a barrier to
     * schedule the write. This is really a "best effort" more than a guarantee, there's too much
     * stuff in flight to be sure.
     */
    GV(QUIET) = 1;
    FLD_CLR(g.trace_flags, TRACE);

    /*
     * Single-thread error handling, our caller exits after calling us (we never release the lock).
     */
    (void)pthread_rwlock_wrlock(&g.death_lock);

    /* Write a failure message so format.sh knows we failed. */
    fprintf(stderr, "\n%s: run FAILED\n", progname);
    fflush(stderr);
    fflush(stdout);

    /* Display the configuration that failed. */
    if (g.configured)
        config_print(true);

    /* Now about to close shared resources, give them a chance to empty. */
    __wt_sleep(2, 0);
    trace_teardown();
}

/*
 * usage --
 *     Display usage statement and exit failure.
 */
static void
usage(void)
{
    fprintf(stderr,
      "usage: %s [-BqRv]\n    "
      "[-C wiredtiger-config] [-c config-file] [-h home] [-T trace-config] [name=value ...]\n",
      progname);
    fprintf(stderr, "%s",
      "\t-B maintain 3.3 release log and configuration option compatibility\n"
      "\t-C additional wiredtiger_open configuration arguments\n"
      "\t-c read test program configuration from a file (default 'CONFIG')\n"
      "\t-h run directory (default 'RUNDIR')\n"
      "\t-q quiet\n"
      "\t-R reopen an existing database\n"
      "\t-T trace operations in the WiredTiger log\n"
      "\t-v verify database and exit\n");

    config_error();
    exit(EXIT_FAILURE);
}
