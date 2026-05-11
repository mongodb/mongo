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
 * Command-line flags for testutil_parse_single_opt.
 *   b: build directory override
 *   G: enable disaggregated storage
 *   h: home directory for the WiredTiger database
 *   p: preserve the home directory after the test completes
 */
#define GETOPTS "b:Gh:p"

/*
 * Test that errors during disaggregated storage checkpoint correctly trigger WT_PANIC. A child
 * process opens a disaggregated connection with a failpoint that injects errors during shared
 * metadata queue processing. The error propagates through the checkpoint, and because the metadata
 * file was already updated, the checkpoint panics rather than risk data corruption.
 */

#define STDERR_FILE "stderr.txt"
#define URI1 "file:test1"
#define URI2 "file:test2"
#define TABLE_CONFIG "key_format=S,value_format=S,block_manager=disagg"
#define EXPECTED_PANIC_MSG "failed while processing shared metadata queue"

/*
 * File-scope so heap strings in opts stay reachable across the child's _exit and don't trip LSAN.
 */
static TEST_OPTS _opts;

/*
 * panic_event_handler --
 *     Event handler that exits cleanly on WT_PANIC so the parent can verify the panic.
 */
static int
panic_event_handler(WT_EVENT_HANDLER *handler, WT_SESSION *session, int error, const char *message)
{
    (void)(handler);
    (void)(session);

    /* Write all error messages to stderr so the parent can verify the panic reason. */
    fprintf(stderr, "%s\n", message);

    /* Exit cleanly on panic so the parent can check exit status. */
    if (error == WT_PANIC) {
        fflush(stderr);
        _exit(EXIT_SUCCESS);
    }
    return (0);
}

static WT_EVENT_HANDLER event_handler = {
  panic_event_handler, NULL, /* Message handler */
  NULL,                      /* Progress handler */
  NULL,                      /* Close handler */
  NULL                       /* General handler */
};

/*
 * subtest_run --
 *     Run in the child process. Open a disaggregated connection with the failpoint enabled, create
 *     tables, and trigger a checkpoint that should panic.
 */
static void WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn)) subtest_run(TEST_OPTS *opts)
{
    struct rlimit rlim;
    WT_CONNECTION *conn;
    WT_CURSOR *cursor;
    WT_SESSION *session;
    int i;
    char filename[512], key[64], value[64];

    /* No core files; the panic may trigger diagnostic assertions during cleanup. */
    memset(&rlim, 0, sizeof(rlim));
    testutil_check(setrlimit(RLIMIT_CORE, &rlim));

    /* Redirect stderr so the parent can verify the panic message. */
    testutil_snprintf(filename, sizeof(filename), "%s/%s", opts->home, STDERR_FILE);
    testutil_assert(freopen(filename, "a", stderr) != NULL);

    /*
     * Open the connection with disaggregated storage. The custom event handler catches
     * WT_PANIC and calls _exit(EXIT_SUCCESS) before the diagnostic abort check in
     * __wt_panic_func.
     */
    testutil_wiredtiger_open(opts, opts->home, "create", &event_handler, &conn, false, false);

    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    /* Create a table and insert data. */
    testutil_check(session->create(session, URI1, TABLE_CONFIG));
    testutil_check(session->open_cursor(session, URI1, NULL, NULL, &cursor));
    for (i = 0; i < 100; i++) {
        testutil_snprintf(key, sizeof(key), "key%d", i);
        testutil_snprintf(value, sizeof(value), "value%d", i);
        cursor->set_key(cursor, key);
        cursor->set_value(cursor, value);
        testutil_check(cursor->insert(cursor));
    }
    testutil_check(cursor->close(cursor));

    /* Set stable timestamp and do a successful checkpoint. */
    testutil_check(conn->set_timestamp(conn, "stable_timestamp=10"));
    testutil_check(session->checkpoint(session, NULL));

    /* Now enable the failpoint for the next checkpoint. */
    testutil_check(
      conn->reconfigure(conn, "timing_stress_for_test=[failpoint_disagg_checkpoint_queue_drain]"));

    /* Create a second table to ensure metadata queue entries exist for the next checkpoint. */
    testutil_check(session->create(session, URI2, TABLE_CONFIG));
    testutil_check(session->open_cursor(session, URI2, NULL, NULL, &cursor));
    for (i = 0; i < 10; i++) {
        testutil_snprintf(key, sizeof(key), "key%d", i);
        testutil_snprintf(value, sizeof(value), "value%d", i);
        cursor->set_key(cursor, key);
        cursor->set_value(cursor, value);
        testutil_check(cursor->insert(cursor));
    }
    testutil_check(cursor->close(cursor));

    testutil_check(conn->set_timestamp(conn, "stable_timestamp=20"));

    /*
     * This checkpoint should panic: the failpoint injects an error during shared metadata queue
     * processing, the checkpoint fails after metadata was written to the page log, and the panic
     * fires. The event handler catches WT_PANIC and calls _exit(EXIT_SUCCESS).
     */
    (void)session->checkpoint(session, NULL);

    /* If we get here, the panic did not fire. */
    _exit(EXIT_FAILURE);
}

/*
 * main --
 *     Test that disaggregated storage checkpoint failures correctly trigger WT_PANIC.
 */
int
main(int argc, char *argv[])
{
    FILE *fp;
    TEST_OPTS *opts;
    pid_t pid;
    int ch, status;
    char buf[1024], filename[512];
    bool found_panic_msg;

    opts = &_opts;
    opts->table_type = TABLE_ROW;

    testutil_parse_begin_opt(argc, argv, GETOPTS, opts);
    while ((ch = __wt_getopt(opts->progname, argc, argv, GETOPTS)) != EOF)
        if (testutil_parse_single_opt(opts, ch) != 0)
            testutil_die(EINVAL, "unexpected option");
    testutil_parse_end_opt(opts);

    opts->disagg.page_log_home = opts->home; /* Set home directory for page log. */

    testutil_recreate_dir(opts->home);

    pid = fork();
    testutil_assert(pid >= 0);

    if (pid == 0) {
        /* Child process: run the subtest that triggers the panic. */
        subtest_run(opts);
        /* Not reached  subtest_run calls _exit. */
    }

    /* Parent: wait for the child. */
    testutil_assert(waitpid(pid, &status, 0) == pid);

    if (WIFSIGNALED(status)) {
        fprintf(stderr, "Child killed by signal %d\n", WTERMSIG(status));
        return (EXIT_FAILURE);
    }

    testutil_assert(WIFEXITED(status));
    if (WEXITSTATUS(status) == EXIT_SUCCESS) {
        printf("Child exited successfully: WT_PANIC detected as expected\n");
    } else if (WEXITSTATUS(status) == EXIT_FAILURE) {
        fprintf(stderr, "Child exited with failure: checkpoint did not panic\n");
        return (EXIT_FAILURE);
    } else {
        fprintf(stderr, "Child exited with unexpected status %d\n", WEXITSTATUS(status));
        return (EXIT_FAILURE);
    }

    /* Verify the child's stderr contains our specific panic message. */
    testutil_snprintf(filename, sizeof(filename), "%s/%s", opts->home, STDERR_FILE);
    fp = fopen(filename, "r");
    testutil_assert(fp != NULL);
    found_panic_msg = false;
    while (fgets(buf, sizeof(buf), fp) != NULL) {
        if (strstr(buf, EXPECTED_PANIC_MSG) != NULL) {
            found_panic_msg = true;
            break;
        }
    }
    fclose(fp);
    if (!found_panic_msg) {
        fprintf(stderr, "Expected panic message not found in child stderr (%s)\n", filename);
        return (EXIT_FAILURE);
    }
    printf("Verified panic message in child stderr\n");

    if (!opts->preserve)
        testutil_remove(opts->home);

    testutil_cleanup(opts);
    return (EXIT_SUCCESS);
}
