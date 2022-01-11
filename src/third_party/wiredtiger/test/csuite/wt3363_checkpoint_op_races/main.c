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

/*
 * [TEST_TAGS]
 * checkpoint
 * [END_TAGS]
 */

#include "test_util.h"

/*
 * JIRA ticket reference: WT-3363
 *
 * Test case description: There are a number of operations that we run that we expect not to
 * conflict with or block against a running checkpoint. This test aims to run repeated checkpoints
 * in a thread, while running an assortment of operations that we expect to execute quickly on
 * further threads. To ensure that we catch any blockages we introduce a very large delay into the
 * checkpoint and measure that no operation takes 1/2 the length of this delay.
 *
 * Failure mode: We monitor the execution time of all operations and if we find any operation taking
 * longer than 1/2 the delay time, we abort dumping a core file which can be used to determine what
 * operation was blocked.
 */
static WT_THREAD_RET do_checkpoints(void *);
static WT_THREAD_RET do_ops(void *);
static WT_THREAD_RET monitor(void *);

/*
 * Time delay to introduce into checkpoints in seconds. Should be at-least double the maximum time
 * that any one of the operations should take. Currently this is set to 10 seconds and we expect no
 * single operation to take longer than 5 seconds.
 */
#define MAX_EXECUTION_TIME 10
#define N_THREADS 10

/*
 * Number of seconds to execute for. Initially set to 15 minutes, as we need to run long enough to
 * be certain we have captured any blockages. In initial testing 5 minutes was enough to reproduce
 * the issue, so we run for 3x that here to ensure we reproduce before declaring success.
 */
#define RUNTIME 900.0

static WT_EVENT_HANDLER event_handler = {handle_op_error, handle_op_message, NULL, NULL};

/*
 * main --
 *     TODO: Add a comment describing this function.
 */
int
main(int argc, char *argv[])
{
    TEST_OPTS *opts, _opts;
    TEST_PER_THREAD_OPTS thread_args[N_THREADS];
    pthread_t ckpt_thread, mon_thread, threads[N_THREADS];
    int i;

    /*
     * This test should not run unless long tests flag is set. The test runs for 15 minutes.
     */
    if (!testutil_is_flag_set("TESTUTIL_ENABLE_TIMING_TESTS"))
        return (EXIT_SUCCESS);

    opts = &_opts;
    opts->unique_id = 0;
    memset(opts, 0, sizeof(*opts));

    testutil_check(testutil_parse_opts(argc, argv, opts));
    testutil_make_work_dir(opts->home);

    testutil_check(wiredtiger_open(opts->home, &event_handler,
      "create,cache_size=1G,timing_stress_for_test=[checkpoint_slow]", &opts->conn));

    testutil_check(pthread_create(&ckpt_thread, NULL, do_checkpoints, opts));

    for (i = 0; i < N_THREADS; ++i) {
        thread_args[i].testopts = opts;
        thread_args[i].thread_counter = 0;
        thread_args[i].threadnum = i;
        testutil_check(pthread_create(&threads[i], NULL, do_ops, &thread_args[i]));
    }

    /*
     * Pass the whole array of thread arguments to the monitoring thread. This thread will need to
     * monitor each threads counter to track if it is stuck.
     */
    testutil_check(pthread_create(&mon_thread, NULL, monitor, thread_args));

    for (i = 0; i < N_THREADS; ++i)
        testutil_check(pthread_join(threads[i], NULL));

    testutil_check(pthread_join(mon_thread, NULL));

    testutil_check(pthread_join(ckpt_thread, NULL));

    printf("Success\n");

    testutil_cleanup(opts);
    return (EXIT_SUCCESS);
}

/*
 * Function for repeatedly running checkpoint operations.
 */
/*
 * do_checkpoints --
 *     TODO: Add a comment describing this function.
 */
static WT_THREAD_RET
do_checkpoints(void *_opts)
{
    TEST_OPTS *opts;
    WT_DECL_RET;
    WT_SESSION *session;
    time_t now, start;

    opts = (TEST_OPTS *)_opts;
    (void)time(&start);
    (void)time(&now);

    while (difftime(now, start) < RUNTIME) {
        testutil_check(opts->conn->open_session(opts->conn, NULL, NULL, &session));

        if ((ret = session->checkpoint(session, "force")) != 0)
            if (ret != EBUSY && ret != ENOENT)
                testutil_die(ret, "session.checkpoint");

        testutil_check(session->close(session, NULL));

        /*
         * A short sleep to let operations process and avoid back to back checkpoints locking up
         * resources.
         */
        sleep(1);
        (void)time(&now);
    }

    return (WT_THREAD_RET_VALUE);
}

/*
 * Function to monitor running operations and abort to dump core in the event that we catch an
 * operation running long.
 */
/*
 * monitor --
 *     TODO: Add a comment describing this function.
 */
static WT_THREAD_RET
monitor(void *args)
{
    TEST_PER_THREAD_OPTS *thread_args;
    time_t now, start;
    int ctr, i, last_ops[N_THREADS];

    thread_args = (TEST_PER_THREAD_OPTS *)args;

    (void)time(&start);
    (void)time(&now);

    memset(last_ops, 0, sizeof(int) + N_THREADS);

    while (difftime(now, start) < RUNTIME) {
        /*
         * Checkpoints will run for slightly over MAX_EXECUTION_TIME. MAX_EXECUTION_TIME should
         * always be long enough that we can complete any single operation in 1/2 that time.
         */
        sleep(MAX_EXECUTION_TIME / 2);

        for (i = 0; i < N_THREADS; i++) {
            ctr = thread_args[i].thread_counter;

            /* Ignore any threads which may not have started yet. */
            if (ctr == 0)
                continue;

            /*
             * We track how many operations each thread has done. If we have slept and the counter
             * remains the same for a thread it is stuck and should drop a core so the cause of the
             * hang can be investigated.
             */
            if (ctr != last_ops[i])
                last_ops[i] = ctr;
            else {
                printf("Thread %d had a task running for more than %d seconds\n", i,
                  MAX_EXECUTION_TIME / 2);
                abort();
            }
        }
        (void)time(&now);
    }

    return (WT_THREAD_RET_VALUE);
}

/*
 * Worker thread. Executes random operations from the set of 6.
 */
/*
 * do_ops --
 *     TODO: Add a comment describing this function.
 */
static WT_THREAD_RET
do_ops(void *args)
{
    WT_RAND_STATE rnd;
    time_t now, start;

    __wt_random_init_seed(NULL, &rnd);
    (void)time(&start);
    (void)time(&now);

    while (difftime(now, start) < RUNTIME) {
        switch (__wt_random(&rnd) % 6) {
        case 0:
            op_bulk(args);
            break;
        case 1:
            op_create(args);
            break;
        case 2:
            op_cursor(args);
            break;
        case 3:
            op_drop(args);
            break;
        case 4:
            op_bulk_unique(args);
            break;
        case 5:
            op_create_unique(args);
            break;
        }
        (void)time(&now);
    }

    return (WT_THREAD_RET_VALUE);
}
