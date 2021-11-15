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

#include <signal.h>
#include <sys/wait.h>

/*
 * JIRA ticket reference: WT-4803 Test case description: This test is checking the functionality of
 * the history store file_max configuration. When the size of the history store file exceeds this
 * value, we expect to panic. Failure mode: If we receive a panic in the test cases we weren't
 * expecting to and vice versa.
 */

#define NUM_KEYS 2000

/*
 * This is a global flag that should be set before running test_hs_workload. It lets the child
 * process know whether it should be expecting a panic or not so that it can adjust its exit code as
 * needed.
 */
static bool expect_panic;

static int
handle_message(WT_EVENT_HANDLER *handler, WT_SESSION *session, int error, const char *message)
{
    WT_UNUSED(handler);
    WT_UNUSED(session);

    (void)fprintf(stderr, "%s: %s\n", message, session->strerror(session, error));

    if (error == WT_PANIC && strstr(message, "exceeds maximum size") != NULL) {
        fprintf(
          stderr, "Got history store error (expect_panic=%s)\n", expect_panic ? "true" : "false");

        /*
         * If we're expecting a panic, exit with zero to indicate to the parent that this test was
         * successful.
         *
         * If not, don't intercept. We'll naturally exit with non-zero if we're terminating due to
         * panic.
         */
        if (expect_panic)
            _exit(EXIT_SUCCESS);
    }

    return (0);
}

static WT_EVENT_HANDLER event_handler = {handle_message, NULL, NULL, NULL};

static void
hs_workload(TEST_OPTS *opts, const char *hs_file_max)
{
    WT_CURSOR *cursor;
    WT_SESSION *other_session, *session;
    int i;
    char buf[WT_MEGABYTE], open_config[128];

    /*
     * We're going to run this workload for different configurations of file_max. So clean out the
     * work directory each time.
     */
    testutil_make_work_dir(opts->home);

    testutil_check(__wt_snprintf(open_config, sizeof(open_config),
      "create,cache_size=50MB,history_store=(file_max=%s)", hs_file_max));

    testutil_check(wiredtiger_open(opts->home, &event_handler, open_config, &opts->conn));
    testutil_check(opts->conn->open_session(opts->conn, NULL, NULL, &session));
    testutil_check(session->create(session, opts->uri, "key_format=i,value_format=S"));
    testutil_check(session->open_cursor(session, opts->uri, NULL, NULL, &cursor));

    memset(buf, 0xA, WT_MEGABYTE);
    buf[WT_MEGABYTE - 1] = '\0';

    /* Populate the table. */
    for (i = 0; i < NUM_KEYS; ++i) {
        cursor->set_key(cursor, i);
        cursor->set_value(cursor, buf);
        testutil_check(cursor->insert(cursor));
    }

    /*
     * Open a snapshot isolation transaction in another session. This forces the cache to retain all
     * previous values. Then update all keys with a new value in the original session while keeping
     * that snapshot transaction open. With the large value buffer, small cache and lots of keys,
     * this will force a lot of history store usage.
     *
     * When the file_max setting is small, the maximum size should easily be reached and we should
     * panic. When the maximum size is large or not set, then we should succeed.
     */
    testutil_check(opts->conn->open_session(opts->conn, NULL, NULL, &other_session));
    testutil_check(other_session->begin_transaction(other_session, "isolation=snapshot"));

    memset(buf, 0xB, WT_MEGABYTE);
    buf[WT_MEGABYTE - 1] = '\0';

    for (i = 0; i < NUM_KEYS; ++i) {
        cursor->set_key(cursor, i);
        cursor->set_value(cursor, buf);
        testutil_check(cursor->update(cursor));
    }

    /*
     * Cleanup. We do not get here when the file_max size is small because we will have already hit
     * the maximum and exited. This code only executes on the successful path.
     */
    testutil_check(opts->conn->close(opts->conn, NULL));
}

static void
test_hs_workload(TEST_OPTS *opts, const char *hs_file_max)
{
    pid_t pid;
    int status;

    /*
     * Since it's possible that the workload will panic and abort, we will fork the process and
     * execute the workload in the child process.
     *
     * This way, we can safely check the exit code of the child process and confirm that it is what
     * we expected.
     */
    testutil_checksys((pid = fork()) < 0);

    if (pid == 0) {
        /* Child process from here. */
        hs_workload(opts, hs_file_max);

        /*
         * If we're expecting a panic during the workload, we shouldn't get to this point. Exit with
         * non-zero to indicate to parent that we should fail this test.
         */
        fprintf(stderr, "Successfully completed workload (expect_panic=%s)\n",
          expect_panic ? "true" : "false");

        if (expect_panic)
            _exit(EXIT_FAILURE);
        else
            _exit(EXIT_SUCCESS);
    }

    /* Parent process from here. */
    testutil_checksys(waitpid(pid, &status, 0) == -1);
    testutil_assert(status == 0);
}

int
main(int argc, char **argv)
{
    TEST_OPTS opts;

    memset(&opts, 0x0, sizeof(opts));
    testutil_check(testutil_parse_opts(argc, argv, &opts));

    /*
     * The history store is unbounded. We don't expect any failure since we can use as much as
     * needed.
     */
    expect_panic = false;
    test_hs_workload(&opts, "0");

    /*
     * The history store is limited to 5GB. This is more than enough for this workload so we don't
     * expect any failure.
     */
    expect_panic = false;
    test_hs_workload(&opts, "5GB");

    /*
     * The history store is limited to 100MB. This is insufficient for this workload so we're
     * expecting a failure.
     */
    expect_panic = true;
    test_hs_workload(&opts, "100MB");

    testutil_cleanup(&opts);

    return (EXIT_SUCCESS);
}
