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

#include "schema_disagg_abort.h"

/*
 * Disaggregated schema epoch crash recovery test.
 *
 * Worker threads create, drop, and publish layered tables on a leader while it checkpoints. The
 * test crashes the leader mid-run and confirms that, after recovery, the tables match the schema
 * operations that were made durable before the crash.
 */

extern int __wt_optind;
extern char *__wt_optarg;

static TEST_OPTS _opts;

static void sig_handler(int) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));
static void usage(void) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));

/*
 * usage --
 *     Print the command-line usage and exit.
 */
static void
usage(void)
{
    fprintf(stderr,
      "usage: %s [-b build-dir] [-h dir] [-p] [-s pool] [-T threads] [-t time] [-v]\n", progname);
    fprintf(stderr, "%s",
      "\t-b build directory (required for PALite extension)\n"
      "\t-h home directory\n"
      "\t-p preserve directory contents\n"
      "\t-s URI pool size per thread\n"
      "\t-T number of schema threads\n"
      "\t-t timeout in seconds\n"
      "\t-v verify only\n");
    exit(EXIT_FAILURE);
}

/*
 * sig_handler --
 *     Reap the leader child and fail the test if it exits before the parent kills it.
 */
static void
sig_handler(int sig)
{
    pid_t pid;

    WT_UNUSED(sig);
    pid = wait(NULL);
    testutil_die(EINVAL, "Child process %" PRIu64 " abnormally exited", (uint64_t)pid);
}

/*
 * create_test_dirs --
 *     Create the directory structure needed for a fresh test run.
 */
static void
create_test_dirs(TEST_CONFIG *cfg)
{
    char buf[PATH_MAX];

    testutil_recreate_dir(cfg->home);
    testutil_snprintf(buf, sizeof(buf), "%s/%s", cfg->home, RECORDS_DIR);
    testutil_mkdir(buf);
    testutil_snprintf(buf, sizeof(buf), "%s/%s", cfg->home, WT_HOME_DIR);
    testutil_mkdir(buf);
}

/*
 * fork_and_kill_child --
 *     Fork the leader child, wait for it to complete its first checkpoint, sleep the timeout, then
 *     SIGKILL it to simulate a crash.
 */
static void
fork_and_kill_child(TEST_CONFIG *cfg, uint32_t timeout)
{
    struct sigaction sa;
    pid_t child_pid;
    int status;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    testutil_assert_errno(sigaction(SIGCHLD, &sa, NULL) == 0);

    testutil_assert_errno((child_pid = fork()) >= 0);
    if (child_pid == 0) {
        run_workload(cfg);
        /* NOTREACHED */
    }

    while (!testutil_exists(cfg->home, READY_FILE))
        testutil_sleep_wait(1, child_pid);

    sleep(timeout);

    sa.sa_handler = SIG_DFL;
    testutil_assert_errno(sigaction(SIGCHLD, &sa, NULL) == 0);

    testutil_assert_errno(kill(child_pid, SIGKILL) == 0);
    testutil_assert_errno(waitpid(child_pid, &status, 0) != -1);
}

/*
 * open_leader_for_recovery --
 *     Configure disaggregated leader options and open the database to trigger recovery.
 */
static void
open_leader_for_recovery(TEST_CONFIG *cfg, WT_CONNECTION **connp)
{
    cfg->opts->disagg.is_enabled = true;
    cfg->opts->disagg.mode = "leader";
    cfg->opts->disagg.page_log = "palite";
    cfg->opts->disagg.page_log_home = cfg->page_log_home;
    cfg->opts->disagg.drain_threads = 1;

    testutil_wiredtiger_open(cfg->opts, WT_HOME_DIR, "create,disaggregated=(lose_all_my_data=true)",
      NULL, connp, true, false);
}

/*
 * main --
 *     Parse arguments, run the workload, then verify schema and data state after recovery.
 */
int
main(int argc, char *argv[])
{
    TEST_CONFIG cfg;
    WT_CONNECTION *conn;
    uint32_t rand_value, timeout;
    int ch;
    char cwd_start[PATH_MAX];
    bool pool_size_set, rand_th, rand_time, verify_only;

    (void)testutil_set_progname(argv);

    memset(&cfg, 0, sizeof(cfg));
    cfg.opts = &_opts;
    memset(cfg.opts, 0, sizeof(*cfg.opts));

    cfg.nth = MIN_TH;
    cfg.pool_size = MAX_POOL_SIZE / 8; /* Default: 8 slots per thread. */
    pool_size_set = false;
    rand_th = rand_time = true;
    timeout = MIN_TIME;
    verify_only = false;

    testutil_parse_begin_opt(argc, argv, "b:h:pP:s:T:t:v", cfg.opts);

    while ((ch = __wt_getopt(progname, argc, argv, "b:h:pP:s:T:t:v")) != EOF)
        switch (ch) {
        case 's':
            pool_size_set = true;
            cfg.pool_size = (uint32_t)atoi(__wt_optarg);
            if (cfg.pool_size < MIN_POOL_SIZE || cfg.pool_size > MAX_POOL_SIZE) {
                fprintf(
                  stderr, "Pool size must be between %d and %d\n", MIN_POOL_SIZE, MAX_POOL_SIZE);
                usage();
            }
            break;
        case 'T':
            rand_th = false;
            cfg.nth = (uint32_t)atoi(__wt_optarg);
            break;
        case 't':
            rand_time = false;
            timeout = (uint32_t)atoi(__wt_optarg);
            break;
        case 'v':
            verify_only = true;
            break;
        default:
            if (testutil_parse_single_opt(cfg.opts, ch) != 0)
                usage();
        }
    argc -= __wt_optind;
    if (argc != 0)
        usage();
    if (verify_only && rand_th) {
        fprintf(stderr, "Verify requires -T\n");
        exit(EXIT_FAILURE);
    }
    if (verify_only && !pool_size_set) {
        fprintf(stderr, "Verify requires -s\n");
        exit(EXIT_FAILURE);
    }

    cfg.opts->disagg.is_enabled = true;
    testutil_parse_end_opt(cfg.opts);
    testutil_work_dir_from_path(cfg.home, sizeof(cfg.home), cfg.opts->home);
    testutil_assert_errno(getcwd(cwd_start, sizeof(cwd_start)) != NULL);

    if (!verify_only) {
        create_test_dirs(&cfg);

        if (rand_time) {
            timeout = __wt_random(&cfg.opts->extra_rnd) % MAX_TIME;
            if (timeout < MIN_TIME)
                timeout = MIN_TIME;
        }

        rand_value = __wt_random(&cfg.opts->data_rnd);
        if (rand_th) {
            cfg.nth = rand_value % MAX_TH;
            if (cfg.nth < MIN_TH)
                cfg.nth = MIN_TH;
        }

        printf("Parent: Create %" PRIu32 " schema threads; pool %" PRIu32 " slots; sleep %" PRIu32
               " seconds\n",
          cfg.nth, cfg.pool_size, timeout);
        printf("CONFIG: %s -s %" PRIu32 " -T %" PRIu32 " -t %" PRIu32 " " TESTUTIL_SEED_FORMAT "\n",
          progname, cfg.pool_size, cfg.nth, timeout, cfg.opts->data_seed, cfg.opts->extra_seed);

        testutil_snprintf(cfg.page_log_home, sizeof(cfg.page_log_home), "%s/%s/%s", cwd_start,
          cfg.home, WT_HOME_DIR);

        fork_and_kill_child(&cfg, timeout);
    }

    if (chdir(cfg.home) != 0)
        testutil_die(errno, "parent chdir: %s", cfg.home);

    if (!verify_only)
        testutil_copy_data();

    if (cfg.page_log_home[0] == '\0')
        testutil_snprintf(cfg.page_log_home, sizeof(cfg.page_log_home), "%s/%s/%s", cwd_start,
          cfg.home, WT_HOME_DIR);

    printf("Open leader database, run recovery and verify content\n");

    open_leader_for_recovery(&cfg, &conn);
    verify_schema_state(conn, &cfg);
    testutil_check(conn->close(conn, "debug=(skip_checkpoint=true)"));

    if (chdir(cwd_start) != 0)
        testutil_die(errno, "root chdir: %s", cfg.home);

    if (!cfg.opts->preserve)
        testutil_remove(cfg.home);

    testutil_cleanup(cfg.opts);
    return (EXIT_SUCCESS);
}
