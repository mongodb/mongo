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

static void format_die(void);
static void usage(void) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));

extern int __wt_optind;
extern char *__wt_optarg;

/*
 * signal_handler --
 *     Generic signal handler, report the signal and exit.
 */
static void signal_handler(int signo) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));
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
static void signal_timer(int signo) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));
static void
signal_timer(int signo)
{
    /*
     * Direct I/O configurations can result in really long run times depending on how the test
     * machine is configured. If a direct I/O run timed out, don't bother dropping core.
     */
    if (g.c_direct_io) {
        fprintf(stderr, "format direct I/O configuration timed out\n");
        fprintf(stderr, "format caught signal %d, exiting with error\n", signo);
        fflush(stderr);
        exit(EXIT_FAILURE);
    }

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

    /* Initialize lock to ensure single threading during failure handling */
    testutil_check(pthread_rwlock_init(&g.death_lock, NULL));

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
 * TIMED_MAJOR_OP --
 *	Set a timer and perform a major operation (for example, verify or salvage).
 */
#define TIMED_MAJOR_OP(call)                   \
    do {                                       \
        if (g.c_major_timeout != 0)            \
            set_alarm(g.c_major_timeout * 60); \
        call;                                  \
        if (g.c_major_timeout != 0)            \
            set_alarm(0);                      \
    } while (0)

int
main(int argc, char *argv[])
{
    uint64_t now, start;
    u_int ops_seconds;
    int ch, reps;
    const char *config, *home;
    bool one_flag, quiet_flag;

    custom_die = format_die; /* Local death handler. */

    config = NULL;

    (void)testutil_set_progname(argv);

    /* The monitoring program looks for this line in the log file, push it out quickly. */
    printf("%s: process %" PRIdMAX " running\n", progname, (intmax_t)getpid());
    fflush(stdout);

    format_process_env();

    __wt_random_init_seed(NULL, &g.rnd); /* Initialize the RNG. */

    /* Set values from the command line. */
    home = NULL;
    one_flag = quiet_flag = false;
    while ((ch = __wt_getopt(progname, argc, argv, "1BC:c:h:qRrT:t")) != EOF)
        switch (ch) {
        case '1': /* One run and quit */
            one_flag = true;
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
        case 'T': /* Trace specifics. */
            if (trace_config(__wt_optarg) != 0) {
                fprintf(stderr, "unexpected trace configuration \"%s\"\n", __wt_optarg);
                usage();
            }
        /* FALLTHROUGH */
        case 't': /* Trace  */
            g.trace = true;
            break;
        default:
            usage();
        }
    argv += __wt_optind;

    /* Set up paths. */
    path_setup(home);

    /*
     * If it's a reopen, use the already existing home directory's CONFIG file.
     *
     * If we weren't given a configuration file, set values from "CONFIG", if it exists. Small hack
     * to ignore any CONFIG file named ".", that just makes it possible to ignore any local CONFIG
     * file, used when running checks.
     */
    if (g.reopen) {
        if (access(g.home_config, R_OK) != 0)
            testutil_die(ENOENT, "%s", g.home_config);
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
        config_single(*argv, true);

    /*
     * Let the command line -1 and -q flags override values configured from other sources.
     * Regardless, don't go all verbose if we're not talking to a terminal.
     */
    if (one_flag)
        g.c_runs = 1;
    if (quiet_flag || !isatty(1))
        g.c_quiet = 1;

    /*
     * Calculate how long each operations loop should run. Take any timer value and convert it to
     * seconds, then allocate 15 seconds to do initialization, verification and/or salvage tasks
     * after the operations loop finishes. This is not intended to be exact in any way, just enough
     * to get us into an acceptable range of run times. The reason for this is because we want to
     * consume the legitimate run-time, but we also need to do the end-of-run checking in all cases,
     * even if we run out of time, otherwise it won't get done. So, in summary pick a reasonable
     * time and then don't check for timer expiration once the main operations loop completes.
     */
    ops_seconds = g.c_timer == 0 ? 0 : ((g.c_timer * 60) - 15) / FORMAT_OPERATION_REPS;

    testutil_check(__wt_thread_str(g.tidbuf, sizeof(g.tidbuf)));

    while (++g.run_cnt <= g.c_runs || g.c_runs == 0) {
        __wt_seconds(NULL, &start);
        track("starting up", 0ULL, NULL);

        config_run();

        if (g.reopen) {
            config_final();
            wts_open(g.home, &g.wts_conn, &g.wts_session, true);
            timestamp_init();
            set_oldest_timestamp();
        } else {
            wts_create(g.home);
            config_final();
            wts_open(g.home, &g.wts_conn, &g.wts_session, true);
            timestamp_init();

            trace_init();
        }

        /* Initialize locks to single-thread backups, failures, and timestamp updates. */
        lock_init(g.wts_session, &g.backup_lock);
        lock_init(g.wts_session, &g.ts_lock);
        lock_init(g.wts_session, &g.prepare_commit_lock);

        if (!g.reopen)
            TIMED_MAJOR_OP(wts_load()); /* Load and verify initial records */

        TIMED_MAJOR_OP(wts_verify(g.wts_conn, "verify"));
        TIMED_MAJOR_OP(wts_read_scan());

        wts_checkpoints();

        /* Operations. */
        for (reps = 1; reps <= FORMAT_OPERATION_REPS; ++reps)
            operations(ops_seconds, reps == FORMAT_OPERATION_REPS);

        /* Copy out the run's statistics. */
        TIMED_MAJOR_OP(wts_stats());

        /*
         * Verify the objects. Verify closes the underlying handle and discards the statistics, read
         * them first.
         */
        TIMED_MAJOR_OP(wts_verify(g.wts_conn, "post-ops verify"));

        lock_destroy(g.wts_session, &g.backup_lock);
        lock_destroy(g.wts_session, &g.ts_lock);
        lock_destroy(g.wts_session, &g.prepare_commit_lock);

        track("shutting down", 0ULL, NULL);
        wts_close(&g.wts_conn, &g.wts_session);

        /*
         * Salvage testing.
         */
        TIMED_MAJOR_OP(wts_salvage());

        trace_teardown();

        /* Overwrite the progress line with a completion line. */
        if (!g.c_quiet)
            printf("\r%78s\r", " ");
        __wt_seconds(NULL, &now);
        printf("%4" PRIu32 ": %s, %s (%" PRIu64 " seconds)\n", g.run_cnt, g.c_data_source,
          g.c_file_type, now - start);
        fflush(stdout);
    }

    config_print(false);

    config_clear();

    printf("%s: successful run completed\n", progname);

    return (EXIT_SUCCESS);
}

/*
 * die --
 *     Report an error, dumping the configuration.
 */
static void
format_die(void)
{
    /*
     * Turn off progress reports so we don't obscure the error message. The lock we're about to
     * acquire will act as a barrier to schedule the write. This is really a "best effort" more than
     * a guarantee, there's too much stuff in flight to be sure.
     */
    g.c_quiet = 1;

    /*
     * Single-thread error handling, our caller exits after calling us (we never release the lock).
     */
    (void)pthread_rwlock_wrlock(&g.death_lock);

    /* Write a failure message so format.sh knows we failed. */
    fprintf(stderr, "\n%s: run FAILED\n", progname);
    fflush(stderr);
    fflush(stdout);

    /* Flush the logs, they may contain debugging information. */
    trace_teardown();
    if (g.c_logging && g.wts_session != NULL)
        testutil_check(g.wts_session->log_flush(g.wts_session, "sync=off"));

    /* Display the configuration that failed. */
    if (g.run_cnt)
        config_print(true);

#ifdef HAVE_DIAGNOSTIC
    /*
     * We have a mismatch, optionally dump WiredTiger datastore pages. In doing so, we are calling
     * into the debug code directly which does not take locks, so it's possible we will simply drop
     * core. Turn off core dumps, those core files aren't interesting.
     *
     * The most important information is the key/value mismatch information. Then try to dump out
     * additional information. We dump the entire history store table including what is on disk,
     * which can potentially be very large. If it becomes a problem, this can be modified to just
     * dump out the page this key is on.
     */
    if (g.c_verify_failure_dump && g.page_dump_cursor != NULL) {
        set_core_off();

        fprintf(stderr, "snapshot-isolation error: Dumping page to %s\n", g.home_pagedump);
        testutil_check(__wt_debug_cursor_page(g.page_dump_cursor, g.home_pagedump));
        fprintf(stderr, "snapshot-isolation error: Dumping HS to %s\n", g.home_hsdump);
#if WIREDTIGER_VERSION_MAJOR >= 10
        testutil_check(__wt_debug_cursor_tree_hs(CUR2S(g.page_dump_cursor), g.home_hsdump));
#endif
    }
#endif
}

/*
 * usage --
 *     Display usage statement and exit failure.
 */
static void
usage(void)
{
    fprintf(stderr,
      "usage: %s [-1BqRt] [-C wiredtiger-config]\n    "
      "[-c config-file] [-h home] [-T trace-options] [name=value ...]\n",
      progname);
    fprintf(stderr, "%s",
      "\t-1 run once then quit\n"
      "\t-B maintain 3.3 release log and configuration option compatibility\n"
      "\t-C specify wiredtiger_open configuration arguments\n"
      "\t-c read test program configuration from a file (default 'CONFIG')\n"
      "\t-h home directory (default 'RUNDIR')\n"
      "\t-q run quietly\n"
      "\t-R run on an existing database\n"
      "\t-T all|local\n"
      "\t-t trace operations\n");

    config_error();
    exit(EXIT_FAILURE);
}
