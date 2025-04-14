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

#include <stdlib.h>

#define N_SESSIONS 10
#define N_SEQUENTIAL_DIFFS 100

/* Constants and variables declaration. */
static const char conn_config[] = "create,cache_size=1MB,statistics=(all)";

static TEST_OPTS opts;

/*
 * test_rng_seq --
 *     Test that 2 sequences are different.
 */
static void
test_rng_seq(void)
{
    int diffs = 0, diffs2048 = 0;
    WT_RAND_STATE rng1, rng2;

    __wt_random_init_seed(&rng1, 1);
    __wt_random_init_seed(&rng2, 2);

    for (int i = 0; i < N_SEQUENTIAL_DIFFS; i++) {
        uint32_t n1 = __wt_random(&rng1);
        uint32_t n2 = __wt_random(&rng2);
        if (opts.verbose)
            printf("test_rng_seq: %5u %5u   %10u %10u\n", n1 % 2048, n2 % 2048, n1, n2);
        if (n1 != n2)
            diffs++;
        if ((n1 % 2048) != (n2 % 2048))
            diffs2048++;
    }
    testutil_assert(diffs > N_SEQUENTIAL_DIFFS / 2);
    testutil_assert(diffs2048 > N_SEQUENTIAL_DIFFS / 2);
}

/*
 * test_rng_init --
 *     Test that initialization with different seeds yields different starting points.
 */
static void
test_rng_init(void)
{
    int diffs = 0, diffs2048 = 0;
    uint32_t last_number = 0;
    WT_RAND_STATE rng;

    for (uint32_t i = 0; i < N_SEQUENTIAL_DIFFS; i++) {
        __wt_random_init_seed(&rng, i);
        uint32_t n = __wt_random(&rng);
        if (opts.verbose)
            printf("test_rng_init: %5u %10u\n", n % 2048, n);
        if (i > 0) {
            if (n != last_number)
                diffs++;
            if ((n % 2048) != (last_number % 2048))
                diffs2048++;
        }
        last_number = n;
    }
    testutil_assert(diffs > N_SEQUENTIAL_DIFFS / 2);
    testutil_assert(diffs2048 > N_SEQUENTIAL_DIFFS / 2);
}

/*
 * main --
 *     The main method.
 */
int
main(int argc, char *argv[])
{
    WT_CONNECTION *conn;
    WT_SESSION_IMPL *(sessions[N_SESSIONS]);
    uint64_t numbers[N_SESSIONS], number = 0, prev_number = 0;
    int diffs, diffs2048;
    char home[1024];
    bool random_numbers_repeated = false;

    memset(&opts, 0, sizeof(opts));
    testutil_check(testutil_parse_opts(argc, argv, &opts));

    test_rng_seq();
    test_rng_init();

    /* Initialize database. */

    testutil_work_dir_from_path(home, sizeof(home), "WT_TEST.random_session");
    testutil_recreate_dir(home);

    testutil_check(wiredtiger_open(home, NULL, conn_config, &conn));

    /*
     * Test one session.
     *
     * The test generates a random number in N sessions sequentially, with only one session open at
     * a time.
     */

    /* Reset the thread's timeslice to raise the probability quicker execution. */
    __wt_sleep(0, 10);

    diffs = diffs2048 = 0;
    for (int i = 0; i < N_SESSIONS; i++) {
        WT_SESSION *session;
        testutil_check(conn->open_session(conn, NULL, NULL, &session));
        number = __wt_random(&((WT_SESSION_IMPL *)session)->rnd_random);
        testutil_check(session->close(session, NULL));
        if (opts.verbose)
            printf("single: %3d: %5u %10u\n", i, (u_int)number % 2048, (u_int)number);
        if (i > 0) {
            if (number != prev_number)
                diffs++;
            if ((number % 2048) != (prev_number % 2048))
                diffs2048++;
        }
        prev_number = number;
    }
    testutil_assert(diffs > N_SESSIONS / 2);
    testutil_assert(diffs2048 > N_SESSIONS / 2);

    /*
     * Test multiple sessions.
     *
     * The test generates random numbers with N sessions open simultaneously.
     */

    /* Reset the thread's timeslice to raise the probability quicker execution. */
    __wt_sleep(0, 10);

    /* Open sessions as quickly as possible. */
    for (int i = 0; i < N_SESSIONS; i++) {
        WT_SESSION *session;
        testutil_check(conn->open_session(conn, NULL, NULL, &session));
        sessions[i] = (WT_SESSION_IMPL *)session;
    }

    for (int cycle = 0; cycle < N_SEQUENTIAL_DIFFS; ++cycle) {
        /* Generate a random number. */
        for (int i = 0; i < N_SESSIONS; i++) {
            numbers[i] = number = __wt_random(&sessions[i]->rnd_random);
            if (opts.verbose)
                printf("multi: %3d:%3d: %5u %10u\n", cycle, i, (u_int)number % 2048, (u_int)number);
        }

        /* The very first session is special because it's reused after the 'single' test above. */
        diffs = diffs2048 = 0;
        for (int i = 1; i < N_SESSIONS; i++) {
            if (numbers[i] != numbers[0])
                diffs++;
            if ((numbers[i] % 2048) != (numbers[0] % 2048))
                diffs2048++;
        }
        testutil_assert(diffs > N_SESSIONS / 2);
        testutil_assert(diffs2048 > N_SESSIONS / 2);

        /* Check the number of same number across other sessions. */
        for (int i = 1; i < N_SESSIONS; i++) {
            diffs = diffs2048 = 0;
            for (int j = 1; j < N_SESSIONS; j++) {
                if (i == j)
                    continue;
                if (numbers[i] != numbers[j])
                    diffs++;
                if ((numbers[i] % 2048) != (numbers[j] % 2048))
                    diffs2048++;
            }
            testutil_assert(diffs > N_SESSIONS / 2);
            testutil_assert(diffs2048 > N_SESSIONS / 2);
        }
    }

    /* Close sessions. */
    for (int i = 0; i < N_SESSIONS; i++)
        testutil_check(sessions[i]->iface.close(&sessions[i]->iface, NULL));

    /* Finish the test and clean up. */

    testutil_check(conn->close(conn, NULL));

    if (!opts.preserve)
        testutil_remove(home);
    testutil_cleanup(&opts);

    testutil_assert(!random_numbers_repeated);

    return (EXIT_SUCCESS);
}
