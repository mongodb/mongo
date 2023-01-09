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
#include "test_util.h"

#include <sys/resource.h>
#include <sys/wait.h>

/*
 * JIRA ticket reference: WT-2909 Test case description:
 *
 * This test attempts to check the integrity of checkpoints by injecting failures (by means of a
 * custom file system) and then trying to recover. To insulate the top level program from various
 * crashes that may occur when injecting failures, the "populate" code runs in another process, and
 * is expected to sometimes fail. Then the top level program runs recovery (with the normal file
 * system) and checks the results. Any failure at the top level indicates a checkpoint integrity
 * problem.
 *
 * Each subtest uses the same kind of schema and data, the only variance is when the faults are
 * injected. At the moment, this test only injects during checkpoints, and only injects write
 * failures. It varies in the number of successful writes that occur before an injected failure
 * (during a checkpoint operation), this can be indicated with "-o N". When N is not specified, the
 * test attempts to find the optimal range of N for testing. Clearly when N is large, then the
 * checkpoint may be successfully written, and the data represented by the checkpoint will be fully
 * present. When N is small, nothing of interest is written and no data is present. To find the
 * sweet spot where interesting failures occur, the test does a binary search to find the
 * approximate N that divides the "small" and "large" cases. This is not strictly deterministic, a
 * given N may give different results on different runs. But approximate optimal N can be
 * determined, allowing a series of additional tests clustered around this N.
 *
 * The data is stored in two tables, one having indices. Both tables have the same keys and are
 * updated with the same key in a single transaction.
 *
 * The keys are int (key_format 'i'); for column-store these are converted on the fly to uint64_t.
 *
 * Failure mode: If one table is out of step with the other, that is detected as a failure at the
 * top level. If an index is missing values (or has extra values), that is likewise a failure at the
 * top level. If the tables or the home directory cannot be opened, that is a top level error. The
 * tables must be present as an initial checkpoint is done without any injected fault.
 */

/*
 * This program does not run on Windows. The non-portable aspects at minimum are fork/exec the use
 * of environment variables (used by fail_fs), and file name and build locations of dynamically
 * loaded libraries.
 */
#define BIG_SIZE (1024 * 10)
#define BIG_CONTENTS "<Big String Contents>"
#define MAX_ARGS 20
#define MAX_OP_RANGE WT_THOUSAND
#define STDERR_FILE "stderr.txt"
#define STDOUT_FILE "stdout.txt"
#define TESTS_PER_CALIBRATION 2
#define TESTS_WITH_RECALIBRATION 5
#define VERBOSE_PRINT (10 * WT_THOUSAND)

static int check_results(TEST_OPTS *, uint64_t *);
static void check_values(WT_CURSOR *, int, int, int, char *);
static int create_big_string(char **);
static void cursor_count_items(WT_CURSOR *, uint64_t *);
static void disable_failures(void);
static void enable_failures(uint64_t, uint64_t);
static void generate_key(uint64_t, int *);
static void generate_value(uint32_t, uint64_t, char *, int *, int *, int *, char **);
static void run_check_subtest(TEST_OPTS *, const char *, uint64_t, bool, uint64_t *);
static int run_check_subtest_range(TEST_OPTS *, const char *, bool);
static void run_check_subtest_range_retry(TEST_OPTS *, const char *, bool);
static void run_process(TEST_OPTS *, const char *, char *[], int *);
static void subtest_main(int, char *[], bool);
static void subtest_populate(TEST_OPTS *, bool);

extern int __wt_optind;

/*
 * check_results --
 *     Check all the tables and verify the results.
 */
static int
check_results(TEST_OPTS *opts, uint64_t *foundp)
{
    WT_CURSOR *maincur, *maincur2, *v0cur, *v1cur, *v2cur;
    WT_SESSION *session;
    uint64_t count, idxcount, nrecords, key64;
    uint32_t rndint;
    int key, key_got, ret, v0, v1, v2;
    char *big, *bigref;

    testutil_check(create_big_string(&bigref));
    nrecords = opts->nrecords;
    testutil_check(wiredtiger_open(opts->home, NULL,
      "create,log=(enabled),statistics=(all),statistics_log=(json,on_close,wait=1)", &opts->conn));
    testutil_check(opts->conn->open_session(opts->conn, NULL, NULL, &session));

    testutil_check(session->open_cursor(session, "table:subtest", NULL, NULL, &maincur));
    testutil_check(session->open_cursor(session, "table:subtest2", NULL, NULL, &maincur2));
    testutil_check(session->open_cursor(session, "index:subtest:v0", NULL, NULL, &v0cur));
    testutil_check(session->open_cursor(session, "index:subtest:v1", NULL, NULL, &v1cur));
    testutil_check(session->open_cursor(session, "index:subtest:v2", NULL, NULL, &v2cur));

    count = 0;
    while ((ret = maincur->next(maincur)) == 0) {
        testutil_check(maincur2->next(maincur2));
        if (opts->table_type == TABLE_ROW)
            testutil_check(maincur2->get_key(maincur2, &key_got));
        else {
            testutil_check(maincur2->get_key(maincur2, &key64));
            key_got = (int)key64;
        }
        testutil_check(maincur2->get_value(maincur2, &rndint));

        generate_key(count, &key);
        generate_value(rndint, count, bigref, &v0, &v1, &v2, &big);
        testutil_assert(key == key_got);

        /* Check the key/values in main table. */
        if (opts->table_type == TABLE_ROW)
            testutil_check(maincur->get_key(maincur, &key_got));
        else {
            testutil_check(maincur->get_key(maincur, &key64));
            key_got = (int)key64;
        }
        testutil_assert(key == key_got);
        check_values(maincur, v0, v1, v2, big);

        /* Check the values in the indices. */
        v0cur->set_key(v0cur, v0);
        testutil_check(v0cur->search(v0cur));
        check_values(v0cur, v0, v1, v2, big);
        v1cur->set_key(v1cur, v1);
        testutil_check(v1cur->search(v1cur));
        check_values(v1cur, v0, v1, v2, big);
        v2cur->set_key(v2cur, v2);
        testutil_check(v2cur->search(v2cur));
        check_values(v2cur, v0, v1, v2, big);

        count++;
        if (count % VERBOSE_PRINT == 0 && opts->verbose)
            printf("checked %" PRIu64 "/%" PRIu64 "\n", count, nrecords);
    }
    if (count % VERBOSE_PRINT != 0 && opts->verbose)
        printf("checked %" PRIu64 "/%" PRIu64 "\n", count, nrecords);

    /*
     * Always expect at least one entry, as populate does a checkpoint after the first insert.
     */
    testutil_assert(count > 0);
    testutil_assert(ret == WT_NOTFOUND);
    testutil_assert(maincur2->next(maincur2) == WT_NOTFOUND);
    cursor_count_items(v0cur, &idxcount);
    testutil_assert(count == idxcount);
    cursor_count_items(v1cur, &idxcount);
    testutil_assert(count == idxcount);
    cursor_count_items(v2cur, &idxcount);
    testutil_assert(count == idxcount);

    testutil_check(opts->conn->close(opts->conn, NULL));
    opts->conn = NULL;

    free(bigref);
    *foundp = count;
    return (0);
}

/*
 * check_values --
 *     Check that the values in the cursor match the given values.
 */
static void
check_values(WT_CURSOR *cursor, int v0, int v1, int v2, char *big)
{
    int v0_got, v1_got, v2_got;
    char *big_got;

    testutil_check(cursor->get_value(cursor, &v0_got, &v1_got, &v2_got, &big_got));
    testutil_assert(v0 == v0_got);
    testutil_assert(v1 == v1_got);
    testutil_assert(v2 == v2_got);
    testutil_assert(strcmp(big, big_got) == 0);
}

/*
 * create_big_string --
 *     Create and fill the "reference" big array.
 */
static int
create_big_string(char **bigp)
{
    size_t i, mod;
    char *big;

    if ((big = malloc(BIG_SIZE + 1)) == NULL)
        return (ENOMEM);
    mod = strlen(BIG_CONTENTS);
    for (i = 0; i < BIG_SIZE; i++) {
        big[i] = BIG_CONTENTS[i % mod];
    }
    big[BIG_SIZE] = '\0';
    *bigp = big;
    return (0);
}

/*
 * cursor_count_items --
 *     Count the number of items in the table by traversing through the cursor.
 */
static void
cursor_count_items(WT_CURSOR *cursor, uint64_t *countp)
{
    WT_DECL_RET;

    *countp = 0;

    testutil_check(cursor->reset(cursor));
    while ((ret = cursor->next(cursor)) == 0)
        (*countp)++;
    testutil_assert(ret == WT_NOTFOUND);
}

/*
 * disable_failures --
 *     Disable failures in the fail file system.
 */
static void
disable_failures(void)
{
    testutil_check(setenv("WT_FAIL_FS_ENABLE", "0", 1));
}

/*
 * enable_failures --
 *     Enable failures in the fail file system.
 */
static void
enable_failures(uint64_t allow_writes, uint64_t allow_reads)
{
    char value[100];

    testutil_check(setenv("WT_FAIL_FS_ENABLE", "1", 1));
    testutil_check(__wt_snprintf(value, sizeof(value), "%" PRIu64, allow_writes));
    testutil_check(setenv("WT_FAIL_FS_WRITE_ALLOW", value, 1));
    testutil_check(__wt_snprintf(value, sizeof(value), "%" PRIu64, allow_reads));
    testutil_check(setenv("WT_FAIL_FS_READ_ALLOW", value, 1));
}

/*
 * generate_key --
 *     Generate a key used by the "subtest" and "subtest2" tables.
 */
static void
generate_key(uint64_t i, int *keyp)
{
    *keyp = (int)i + 1;
}

/*
 * generate_value --
 *     Generate values for the "subtest" table.
 */
static void
generate_value(uint32_t rndint, uint64_t i, char *bigref, int *v0p, int *v1p, int *v2p, char **bigp)
{
    *v0p = (int)(i * 7);
    *v1p = (int)(i * 10007);
    *v2p = (int)(i * 100000007);
    *bigp = &bigref[rndint % BIG_SIZE];
}

/*
 * run_check_subtest --
 *     Run the subtest with the given parameters and check the results.
 */
static void
run_check_subtest(
  TEST_OPTS *opts, const char *debugger, uint64_t nops, bool close_test, uint64_t *nresultsp)
{
    int estatus, narg;
    char rarg[20], sarg[20], *subtest_args[MAX_ARGS];

    narg = 0;
    if (debugger != NULL) {
        subtest_args[narg++] = (char *)debugger;
        subtest_args[narg++] = (char *)"--";
    }

    subtest_args[narg++] = (char *)opts->argv0;
    /* "subtest" must appear before arguments */
    if (close_test)
        subtest_args[narg++] = (char *)"subtest_close";
    else
        subtest_args[narg++] = (char *)"subtest";
    subtest_args[narg++] = (char *)"-h";
    subtest_args[narg++] = opts->home;
    if (opts->build_dir != NULL) {
        subtest_args[narg++] = (char *)"-b";
        subtest_args[narg++] = opts->build_dir;
    }
    subtest_args[narg++] = (char *)"-v"; /* subtest is always verbose */
    subtest_args[narg++] = (char *)"-p";
    subtest_args[narg++] = (char *)"-o";
    testutil_check(__wt_snprintf(sarg, sizeof(sarg), "%" PRIu64, nops));
    subtest_args[narg++] = sarg; /* number of operations */
    subtest_args[narg++] = (char *)"-n";
    testutil_check(__wt_snprintf(rarg, sizeof(rarg), "%" PRIu64, opts->nrecords));
    subtest_args[narg++] = rarg; /* number of records */
    subtest_args[narg++] = (char *)"-t";
    subtest_args[narg++] = (char *)(opts->table_type == TABLE_ROW ? "r" : "c");
    subtest_args[narg++] = NULL;
    testutil_assert(narg <= MAX_ARGS);
    if (opts->verbose)
        printf("running a separate process with %" PRIu64 " operations until fail...\n", nops);
    testutil_clean_work_dir(opts->home);
    run_process(opts, debugger != NULL ? debugger : opts->argv0, subtest_args, &estatus);
    if (opts->verbose)
        printf("process exited %d\n", estatus);

    /*
     * Verify results in parent process.
     */
    testutil_check(check_results(opts, nresultsp));
}

/*
 * run_check_subtest_range --
 *     Run successive tests via binary search that determines the approximate crossover point
 *     between when data is recoverable or not. Once that is determined, run the subtest in a range
 *     near that crossover point. The theory is that running at the crossover point will tend to
 *     trigger "interesting" failures at the borderline when the checkpoint is about to, or has,
 *     succeeded. If any of those failures creates a WiredTiger home directory that cannot be
 *     recovered, the top level test will fail.
 */
static int
run_check_subtest_range(TEST_OPTS *opts, const char *debugger, bool close_test)
{
    uint64_t cutoff, high, low, mid, nops, nresults;
    int i;
    bool got_failure, got_success;

    if (opts->verbose)
        printf("Determining best range of operations until failure, with close_test %s.\n",
          (close_test ? "enabled" : "disabled"));

    run_check_subtest(opts, debugger, 1, close_test, &cutoff);
    low = 0;
    high = MAX_OP_RANGE;
    mid = (low + high) / 2;
    while (low < mid - 5 || high > mid + 5) {
        run_check_subtest(opts, debugger, mid, close_test, &nresults);
        if (nresults > cutoff)
            high = mid;
        else
            low = mid;
        mid = (low + high) / 2;
    }
    /*
     * mid is the number of ops that is the crossover point. Run some tests near that point to try
     * to trigger weird failures. If mid is too low or too high, it indicates there is a fundamental
     * problem with the test.
     */
    testutil_assert(mid > 1 && mid < MAX_OP_RANGE - 1);
    if (opts->verbose)
        printf("Retesting around %" PRIu64 " operations.\n", mid);

    got_failure = false;
    got_success = false;
    for (i = 0; i < TESTS_PER_CALIBRATION && (!got_failure || !got_success); i++)
        for (nops = mid - 10; nops < mid + 10; nops++) {
            run_check_subtest(opts, debugger, nops, close_test, &nresults);
            if (nresults > cutoff)
                got_failure = true;
            else
                got_success = true;
        }

    /*
     * Check that it really ran with a crossover point. If not, perhaps we calibrated the range
     * incorrectly. Tell caller to try again.
     */
    if (!got_failure || !got_success) {
        fprintf(stderr,
          "Warning: did not find a reliable test range.\n"
          "midpoint=%" PRIu64 ", close_test=%d, got_failure=%d, got_success=%d\n",
          mid, (int)close_test, (int)got_failure, (int)got_success);
        return (EAGAIN);
    }
    return (0);
}

/*
 * run_check_subtest_range_retry --
 *     Repeatedly run the subtest range test, retrying some number of times as long as EBUSY is
 *     returned, a warning that the test did not adequately cover "both sides" of the test
 *     threshold. Such warning returns should be rare and are not hard failures, no WiredTiger bug
 *     is demonstrated. Rerunning the subtest range test will determine a new calibration for the
 *     range.
 */
static void
run_check_subtest_range_retry(TEST_OPTS *opts, const char *debugger, bool close_test)
{
    WT_DECL_RET;
    int tries;

    for (tries = 0; tries < TESTS_WITH_RECALIBRATION; tries++) {
        if (tries != 0) {
            fprintf(stderr, "Retrying after sleep...\n");
            sleep(5);
        }
        if ((ret = run_check_subtest_range(opts, debugger, close_test)) == 0)
            break;
        testutil_assert(ret == EAGAIN);
    }
    if (tries == TESTS_WITH_RECALIBRATION)
        /*
         * If we couldn't successfully perform the test, we want to know about it.
         */
        testutil_die(ret, "too many retries");
}

/*
 * run_process --
 *     Run a program with arguments, wait until it completes.
 */
static void
run_process(TEST_OPTS *opts, const char *prog, char *argv[], int *statusp)
{
    int pid;
    char **arg;

    if (opts->verbose) {
        printf("running: ");
        for (arg = argv; *arg != NULL; arg++)
            printf("%s ", *arg);
        printf("\n");
    }
    testutil_assert_errno((pid = fork()) >= 0);
    if (pid == 0) {
        (void)execv(prog, argv);
        _exit(EXIT_FAILURE);
    }

    testutil_assert_errno(waitpid(pid, statusp, 0) != -1);
}

/*
 * subtest_error_handler --
 *     Error event handler.
 */
static int
subtest_error_handler(
  WT_EVENT_HANDLER *handler, WT_SESSION *session, int error, const char *message)
{
    (void)(handler);
    (void)(session);
    (void)(message);

    /* Exit on panic, there's no checking to be done. */
    if (error == WT_PANIC)
        exit(1);
    return (0);
}

static WT_EVENT_HANDLER event_handler = {
  subtest_error_handler, NULL, /* Message handler */
  NULL,                        /* Progress handler */
  NULL,                        /* Close handler */
  NULL                         /* General handler */
};

/*
 * subtest_main --
 *     The main program for the subtest
 */
static void
subtest_main(int argc, char *argv[], bool close_test)
{
    struct rlimit rlim;
    TEST_OPTS *opts, _opts;
    WT_SESSION *session;
    char buf[1024], config[1024], filename[1024], tableconf[128];

    opts = &_opts;
    memset(opts, 0, sizeof(*opts));
    opts->table_type = TABLE_ROW;
    memset(&rlim, 0, sizeof(rlim));

    /* No core files during fault injection tests. */
    testutil_check(setrlimit(RLIMIT_CORE, &rlim));
    testutil_check(testutil_parse_opts(argc, argv, opts));
    testutil_make_work_dir(opts->home);

    /* Redirect stderr, stdout. */
    testutil_check(__wt_snprintf(filename, sizeof(filename), "%s/%s", opts->home, STDERR_FILE));
    testutil_assert(freopen(filename, "a", stderr) != NULL);
    testutil_check(__wt_snprintf(filename, sizeof(filename), "%s/%s", opts->home, STDOUT_FILE));
    testutil_assert(freopen(filename, "a", stdout) != NULL);

#ifndef WT_FAIL_FS_LIB
#define WT_FAIL_FS_LIB "ext/test/fail_fs/.libs/libwiredtiger_fail_fs.so"
#endif
    testutil_build_dir(opts, buf, 1024);
    testutil_check(__wt_snprintf(config, sizeof(config),
      "create,cache_size=250M,log=(enabled),transaction_sync=(enabled,method=none),extensions=(%s/"
      "%s=(early_load,config={environment=true,verbose=true})),statistics=(all),statistics_log=("
      "json,on_close,wait=1)",
      buf, WT_FAIL_FS_LIB));
    testutil_check(wiredtiger_open(opts->home, &event_handler, config, &opts->conn));

    testutil_check(opts->conn->open_session(opts->conn, NULL, NULL, &session));
    testutil_check(__wt_snprintf(tableconf, sizeof(tableconf),
      "key_format=%s,value_format=iiiS,columns=(id,v0,v1,v2,big)",
      opts->table_type == TABLE_ROW ? "i" : "r"));
    testutil_check(session->create(session, "table:subtest", tableconf));

    testutil_check(__wt_snprintf(tableconf, sizeof(tableconf), "key_format=%s,value_format=i",
      opts->table_type == TABLE_ROW ? "i" : "r"));
    testutil_check(session->create(session, "table:subtest2", tableconf));

    testutil_check(session->create(session, "index:subtest:v0", "columns=(v0)"));
    testutil_check(session->create(session, "index:subtest:v1", "columns=(v1)"));
    testutil_check(session->create(session, "index:subtest:v2", "columns=(v2)"));

    testutil_check(session->close(session, NULL));

    subtest_populate(opts, close_test);

    testutil_cleanup(opts);
}

/*
 * This macro is used as a substitute for testutil_check, except that it is aware of when a failure
 * may be expected due to the effects of the fail_fs. This macro is used only in subtest_populate(),
 * it uses local variables.
 */
#define CHECK(expr, failmode)                                                 \
    {                                                                         \
        int _ret;                                                             \
        _ret = expr;                                                          \
        if (_ret != 0) {                                                      \
            if (!failmode || (_ret != WT_RUN_RECOVERY && _ret != EIO)) {      \
                fprintf(stderr, "  BAD RETURN %d for \"%s\"\n", _ret, #expr); \
                testutil_check(_ret);                                         \
            } else                                                            \
                failed = true;                                                \
        }                                                                     \
    }

/*
 * subtest_populate --
 *     Populate the tables.
 */
static void
subtest_populate(TEST_OPTS *opts, bool close_test)
{
    WT_CURSOR *maincur, *maincur2;
    WT_RAND_STATE rnd;
    WT_SESSION *session;
    uint64_t i, nrecords;
    uint32_t rndint;
    int key, v0, v1, v2;
    char *big, *bigref;
    bool failed;

    failed = false;
    __wt_random_init_seed(NULL, &rnd);
    CHECK(create_big_string(&bigref), false);
    nrecords = opts->nrecords;

    CHECK(opts->conn->open_session(opts->conn, NULL, NULL, &session), false);

    CHECK(session->open_cursor(session, "table:subtest", NULL, NULL, &maincur), false);

    CHECK(session->open_cursor(session, "table:subtest2", NULL, NULL, &maincur2), false);

    for (i = 0; i < nrecords && !failed; i++) {
        rndint = __wt_random(&rnd);
        generate_key(i, &key);
        generate_value(rndint, i, bigref, &v0, &v1, &v2, &big);
        CHECK(session->begin_transaction(session, NULL), false);
        if (opts->table_type == TABLE_ROW)
            maincur->set_key(maincur, key);
        else
            maincur->set_key(maincur, (uint64_t)key);
        maincur->set_value(maincur, v0, v1, v2, big);
        CHECK(maincur->insert(maincur), false);

        if (opts->table_type == TABLE_ROW)
            maincur2->set_key(maincur2, key);
        else
            maincur2->set_key(maincur2, (uint64_t)key);
        maincur2->set_value(maincur2, rndint);
        CHECK(maincur2->insert(maincur2), false);
        CHECK(session->commit_transaction(session, NULL), false);

        if (i == 0)
            /*
             * Force an initial checkpoint, that helps to distinguish a clear failure from just not
             * running long enough.
             */
            CHECK(session->checkpoint(session, NULL), false);

        if ((i + 1) % VERBOSE_PRINT == 0 && opts->verbose)
            printf("  %" PRIu64 "/%" PRIu64 "\n", (i + 1), nrecords);
        /* Attempt to isolate the failures to checkpointing. */
        if (i == (nrecords / 100)) {
            enable_failures(opts->nops, WT_MILLION);
            /* CHECK should expect failures. */
            CHECK(session->checkpoint(session, NULL), true);
            disable_failures();
            if (failed && opts->verbose)
                printf("checkpoint failed (expected).\n");
        }
    }

    /*
     * Closing handles after an extreme fail is likely to cause cascading failures (or crashes), so
     * recommended practice is to immediately exit. We're interested in testing both with and
     * without the recommended practice.
     */
    if (failed) {
        if (!close_test) {
            fprintf(stderr, "exit early.\n");
            exit(0);
        } else
            fprintf(stderr, "closing after failure.\n");
    }

    free(bigref);
    CHECK(maincur->close(maincur), false);
    CHECK(maincur2->close(maincur2), false);
    CHECK(session->close(session, NULL), false);
}

/*
 * main --
 *     The main program for the test. When invoked with "subtest" argument, run the subtest.
 *     Otherwise, run a separate process for each needed subtest, and check the results.
 */
int
main(int argc, char *argv[])
{
    TEST_OPTS *opts, _opts;
    uint64_t nresults;
    const char *debugger;

    opts = &_opts;
    memset(opts, 0, sizeof(*opts));
    opts->table_type = TABLE_ROW;
    debugger = NULL;

    testutil_check(testutil_parse_opts(argc, argv, opts));
    argc -= __wt_optind;
    argv += __wt_optind;
    if (opts->nrecords == 0)
        opts->nrecords = 50 * WT_THOUSAND;
    if (opts->table_type == TABLE_FIX)
        testutil_die(ENOTSUP, "Fixed-length column store not supported");

    while (argc > 0) {
        if (strcmp(argv[0], "subtest") == 0) {
            subtest_main(argc, argv, false);
            return (0);
        } else if (strcmp(argv[0], "subtest_close") == 0) {
            subtest_main(argc, argv, true);
            return (0);
        } else if (strcmp(argv[0], "gdb") == 0)
            debugger = "/usr/bin/gdb";
        else
            testutil_assert(false);
        argc--;
        argv++;
    }
    if (opts->verbose) {
        printf("Number of operations until failure: %" PRIu64 "  (change with -o N)\n", opts->nops);
        printf("Number of records: %" PRIu64 "  (change with -n N)\n", opts->nrecords);
    }
    if (opts->nops == 0) {
        run_check_subtest_range_retry(opts, debugger, false);
        run_check_subtest_range_retry(opts, debugger, true);
    } else
        run_check_subtest(opts, debugger, opts->nops, opts->nrecords, &nresults);

    testutil_clean_work_dir(opts->home);
    testutil_cleanup(opts);

    return (0);
}
