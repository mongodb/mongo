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

#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>

/*
 * Command-line arguments.
 */
#define SHARED_PARSE_OPTIONS "h:p"

static char home[PATH_MAX]; /* Program working dir */
static TEST_OPTS *opts, _opts;

extern int __wt_optind;
extern char *__wt_optarg;

static void usage(void) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));

/*
 * Configuration.
 */
#define ENV_CONFIG_DEFAULT                                    \
    "cache_size=20M,create,"                                  \
    "eviction_updates_target=20,eviction_updates_trigger=90," \
    "log=(enabled,file_max=10M,remove=true),session_max=100," \
    "statistics=(all),statistics_log=(wait=1,json,on_close)"

#define ENV_CONFIG_NOLOG                                      \
    "cache_size=20M,create,"                                  \
    "eviction_updates_target=20,eviction_updates_trigger=90," \
    "session_max=100,"                                        \
    "statistics=(all),statistics_log=(wait=1,json,on_close)"

#define BACKUP_BASE "backup."
#define CHECK_DIR "check"
#define NUM_BACKUPS 3
#define TABLE_CONFIG "key_format=S,value_format=S,log=(enabled=false)"
#define TABLE_NAME "table"
#define TABLE_URI ("table:" TABLE_NAME)

static const char *env_config;

/*
 * Error handling.
 */
static int handle_error(WT_EVENT_HANDLER *, WT_SESSION *, int, const char *);
static WT_EVENT_HANDLER child_event_handler = {handle_error, NULL, NULL, NULL, NULL};

/*
 * Other constants.
 */
#define EXPECT_ABORT "expect_abort"

#define SCENARIO_TEST_BACKUP 1
#define SCENARIO_TEST_FORCE_STOP 2

/*
 * handle_error --
 *     Function to handle errors.
 */
static int
handle_error(WT_EVENT_HANDLER *handler, WT_SESSION *session, int error, const char *errmsg)
{
    (void)(handler);
    (void)(session);
    (void)(error);

    /* Ignore the abort message if we expect the test to abort. */
    if (testutil_exists(NULL, EXPECT_ABORT))
        if (strstr(errmsg, "aborting WiredTiger library") != NULL)
            return (0);

    return (fprintf(stderr, "%s\n", errmsg) < 0 ? -1 : 0);
}

/*
 * handler_sigchld --
 *     Signal handler to catch if the child died.
 */
static void
handler_sigchld(int sig)
{
    pid_t pid;

    pid = wait(NULL);
    WT_UNUSED(sig);

    if (testutil_exists(NULL, EXPECT_ABORT))
        return;

    /*
     * The core file will indicate why the child exited. Choose EINVAL here.
     */
    testutil_die(EINVAL, "Child process %" PRIu64 " abnormally exited", (uint64_t)pid);
}

/*
 * populate_table --
 *     Populate the table with random data.
 */
static void
populate_table(WT_SESSION *session, const char *uri, uint32_t prefix, uint64_t num_keys)
{
    WT_CURSOR *cursor;
    uint64_t i;
    uint32_t k;
    char key[32], value[32];

    testutil_check(session->open_cursor(session, uri, NULL, NULL, &cursor));
    for (i = 0; i < num_keys; i++) {
        k = __wt_random(&opts->data_rnd);
        testutil_snprintf(key, sizeof(key), "%010" PRIu32 ":%010" PRIu32, prefix, k);
        testutil_snprintf(value, sizeof(value), "%010" PRIu32, ~k);
        cursor->set_key(cursor, key);
        cursor->set_value(cursor, value);
        testutil_check(cursor->insert(cursor));
    }
    testutil_check(cursor->close(cursor));
}

/*
 * verify_backup --
 *     Verify the backup's consistency.
 */
static void
verify_backup(const char *backup_home)
{
    WT_CONNECTION *conn;
    WT_CURSOR *cursor;
    WT_DECL_RET;
    WT_SESSION *session;
    uint32_t k, v;
    char *key, *value;

    /* Copy the backup. */
    testutil_remove(CHECK_DIR);
    testutil_copy(backup_home, CHECK_DIR);

    /* Open the backup. */
    testutil_wiredtiger_open(opts, CHECK_DIR, env_config, NULL, &conn, true, false);
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    /* Verify self-consistency. */
    testutil_check(session->open_cursor(session, TABLE_URI, NULL, NULL, &cursor));
    while ((ret = cursor->next(cursor)) == 0) {
        testutil_check(cursor->get_key(cursor, &key));
        testutil_check(cursor->get_value(cursor, &value));
        testutil_assert(strlen(key) > 11);
        k = (uint32_t)atoll(key + 11);
        v = (uint32_t)atoll(value);
        testutil_assert(k == ~v);
    }
    testutil_assert(ret == WT_NOTFOUND);
    testutil_check(cursor->close(cursor));

    /* Cleanup. */
    testutil_check(session->close(session, NULL));
    testutil_check(conn->close(conn, NULL));
}

/*
 * do_work_before_failure --
 *     Do a bunch of work, including taking backups and checkpoints, before we inject the failure.
 */
static void
do_work_before_failure(WT_CONNECTION *conn, WT_SESSION *session)
{
    int i;

    for (i = 0; i < NUM_BACKUPS; i++) {
        populate_table(session, TABLE_URI, (uint32_t)i, 100 * WT_THOUSAND);
        testutil_check(session->checkpoint(session, NULL));
        populate_table(session, TABLE_URI, (uint32_t)i + 1, 100 * WT_THOUSAND);
        testutil_check(session->checkpoint(session, NULL));
        populate_table(session, TABLE_URI, (uint32_t)i + 2, 100 * WT_THOUSAND);

        if (i == 0) {
            printf("Create full backup %d\n", i);
            testutil_backup_create_full(conn, WT_HOME_DIR, i, true, 32, NULL);
        } else {
            printf("Create incremental backup %d from %d\n", i, i - 1);
            testutil_backup_create_incremental(
              conn, WT_HOME_DIR, i, i - 1, false /* verbose */, NULL, NULL, NULL);
        }
    }
}

/*
 * do_work_after_failure --
 *     Do work after an injected failure: Reopen the database, do more work, create another
 *     incremental backup, and verify it.
 */
static void
do_work_after_failure(bool backup_from_min)
{
    WT_CONNECTION *conn;
    WT_CURSOR *cursor;
    WT_DECL_RET;
    WT_SESSION *session;
    int i, id;
    char backup_home[64], *str;
    bool do_incr;

    /* Reopen the database and find available backup IDs. */
    testutil_wiredtiger_open(opts, WT_HOME_DIR, env_config, NULL, &conn, false, false);
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    do_incr = true;
    id = -1;
    ret = session->open_cursor(session, "backup:query_id", NULL, NULL, &cursor);
    if (ret == 0) {
        while ((ret = cursor->next(cursor)) == 0) {
            testutil_check(cursor->get_key(cursor, &str));
            testutil_assert(strncmp(str, "ID", 2) == 0);
            i = atoi(str + 2);
            if (id < 0)
                id = i;
            else if (backup_from_min)
                id = WT_MIN(id, i);
            else
                id = WT_MAX(id, i);
            printf("Found backup %d\n", i);
        }
        testutil_assert(ret == WT_NOTFOUND);
        testutil_check(cursor->close(cursor));
        testutil_assert(id >= 0);
    } else if (ret == EINVAL) {
        do_incr = false;
        id = 0;
    } else
        testutil_check(ret);

    /* Do more regular work. */
    populate_table(session, TABLE_URI, NUM_BACKUPS, 100 * WT_THOUSAND);

    /*
     * Create a backup. It might be a full or incremental depending on the state of the system and
     * querying IDs after recovery and if IDs exist.
     */
    testutil_snprintf(backup_home, sizeof(backup_home), BACKUP_BASE "%d", NUM_BACKUPS);
    if (!do_incr) {
        printf("Create full backup into %s\n", backup_home);
        testutil_backup_create_full(conn, WT_HOME_DIR, NUM_BACKUPS, true, 32, NULL);
    } else {
        printf("Create incremental backup %d from %d\n", NUM_BACKUPS, id);
        testutil_backup_create_incremental(
          conn, WT_HOME_DIR, NUM_BACKUPS, id, false /* verbose */, NULL, NULL, NULL);
    }

    /* Cleanup. */
    testutil_check(session->close(session, NULL));
    testutil_check(conn->close(conn, NULL));

    /* Verify the backup. */
    printf("Verify backup %d\n", NUM_BACKUPS);
    verify_backup(backup_home);
}

/*
 * run_test_backup --
 *     Run a test with incremental backup.
 */
static void
run_test_backup(void)
{
    WT_CONNECTION *conn;
    WT_SESSION *session;
    pid_t pid;
    int status;

    printf("\n%s: Test crashing during checkpoint after incremental backup\n", __func__);

    /* Execute the test scenario in its own directory. */
    testutil_mkdir("test_backup");
    testutil_check(chdir("test_backup"));

    testutil_remove(EXPECT_ABORT);
    testutil_assert_errno((pid = fork()) >= 0);

    if (pid == 0) { /* Child. */
        testutil_recreate_dir(WT_HOME_DIR);
        testutil_wiredtiger_open(
          opts, WT_HOME_DIR, env_config, &child_event_handler, &conn, false, false);
        testutil_check(conn->open_session(conn, NULL, NULL, &session));
        testutil_check(session->create(session, TABLE_URI, TABLE_CONFIG));

        /* Do some work, while creating checkpoints and doing backups. */
        do_work_before_failure(conn, session);

        /* Die before finishing the next checkpoint. */
        printf("Setting the failpoint...\n");
        testutil_check(
          session->reconfigure(session, "debug=(checkpoint_fail_before_turtle_update=true)"));
        testutil_sentinel(NULL, EXPECT_ABORT);
        testutil_check(session->checkpoint(session, NULL));
        testutil_remove(EXPECT_ABORT);

        /* We should die before we get here. */
        testutil_die(ENOTRECOVERABLE, "The child process was supposed be dead by now!");
    }

    /* Parent. */

    /*
     * Wait for the child to die. Depending on when the child died and on what operating system the
     * call may return the child process ID or an error. If we get an error indication check that it
     * is an interrupt.
     */
    testutil_assert(waitpid(pid, &status, 0) > 0 || errno == EINTR);
    printf("-- crash --\n");

    /* Save the database directory. */
    testutil_copy(WT_HOME_DIR, "save");

    /* Do more work, create another backup, and verify it. */
    do_work_after_failure(true);

    /* Go out of the test scenario directory. */
    testutil_check(chdir(".."));
}

/*
 * run_test_force_stop --
 *     Run a test with force stop.
 */
static void
run_test_force_stop(void)
{
    WT_CONNECTION *conn;
    WT_SESSION *session;
    pid_t pid;
    int status;

    printf("\n%s: Test crashing during force-stop after incremental backup\n", __func__);

    /* Execute the test scenario in its own directory. */
    testutil_mkdir("test_force_stop");
    testutil_check(chdir("test_force_stop"));

    testutil_remove(EXPECT_ABORT);
    testutil_assert_errno((pid = fork()) >= 0);

    if (pid == 0) { /* Child. */
        testutil_recreate_dir(WT_HOME_DIR);
        testutil_wiredtiger_open(
          opts, WT_HOME_DIR, env_config, &child_event_handler, &conn, false, false);
        testutil_check(conn->open_session(conn, NULL, NULL, &session));
        testutil_check(session->create(session, TABLE_URI, TABLE_CONFIG));

        /* Do some work, while creating checkpoints and doing backups. */
        do_work_before_failure(conn, session);

        /* Die before finishing the next checkpoint. */
        printf("Setting the failpoint...\n");
        testutil_check(
          session->reconfigure(session, "debug=(checkpoint_fail_before_turtle_update=true)"));
        testutil_sentinel(NULL, EXPECT_ABORT);
        testutil_backup_force_stop(session);
        testutil_remove(EXPECT_ABORT);

        /* We should die before we get here. */
        testutil_die(ENOTRECOVERABLE, "The child process was supposed be dead by now!");
    }

    /* Parent. */

    /*
     * Wait for the child to die. Depending on when the child died and on what operating system the
     * call may return the child process ID or an error. If we get an error indication check that it
     * is an interrupt.
     */
    testutil_assert(waitpid(pid, &status, 0) > 0 || errno == EINTR);
    printf("-- crash --\n");

    /* Save the database directory. */
    testutil_copy(WT_HOME_DIR, "save");

    /* Do more work, create another backup, and verify it. */
    do_work_after_failure(false);

    /* Go out of the test scenario directory. */
    testutil_check(chdir(".."));
}

/*
 * usage --
 *     Print usage help for the program.
 */
static void
usage(void)
{
    fprintf(stderr, "usage: %s%s [-n] [-s SCENARIO_NUM]\n\n", progname, opts->usage);
    fprintf(stderr, "Test-specific options:\n");
    fprintf(stderr, "    -n    disable log\n");
    fprintf(stderr, "    -s N  set the scenario number (use 0 for all scenarios)\n");
    exit(EXIT_FAILURE);
}

/*
 * main --
 *     The entry point for the test. The test checks that WiredTiger handles backup IDs correctly if
 *     it crashes during the checkpoint, right before the turtle file rename.
 */
int
main(int argc, char *argv[])
{
    struct sigaction sa;
    int ch, scenario;
    char start_cwd[PATH_MAX];

    (void)testutil_set_progname(argv);

    /* Automatically flush after each newline, so that we don't miss any messages if we crash. */
    __wt_stream_set_line_buffer(stderr);
    __wt_stream_set_line_buffer(stdout);

    opts = &_opts;
    memset(opts, 0, sizeof(*opts));

    env_config = ENV_CONFIG_DEFAULT;
    scenario = 0;

    /* Parse the command-line arguments. */
    testutil_parse_begin_opt(argc, argv, SHARED_PARSE_OPTIONS, opts);
    while ((ch = __wt_getopt(progname, argc, argv, "ns:" SHARED_PARSE_OPTIONS)) != EOF)
        switch (ch) {
        case 'n':
            env_config = ENV_CONFIG_NOLOG;
            break;
        case 's':
            scenario = atoi(__wt_optarg);
            break;
        default:
            if (testutil_parse_single_opt(opts, ch) != 0)
                usage();
        }
    argc -= __wt_optind;
    if (argc != 0)
        usage();

    testutil_parse_end_opt(opts);
    testutil_work_dir_from_path(home, sizeof(home), opts->home);

    /* Create the test directory. */
    testutil_recreate_dir(home);
    testutil_assert_errno(getcwd(start_cwd, sizeof(start_cwd)) != NULL);
    testutil_assert_errno(chdir(home) == 0);

    /* Configure the child death handling. */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler_sigchld;
    testutil_assert_errno(sigaction(SIGCHLD, &sa, NULL) == 0);

    /* Run the tests. */
    if (scenario == 0 || scenario == SCENARIO_TEST_BACKUP)
        run_test_backup();
    if (scenario == 0 || scenario == SCENARIO_TEST_FORCE_STOP)
        run_test_force_stop();

    /*
     * Clean up.
     */
    testutil_assert_errno(chdir(start_cwd) == 0);

    /* Delete the work directory. */
    if (!opts->preserve)
        testutil_remove(home);

    testutil_cleanup(opts);
    return (EXIT_SUCCESS);
}
