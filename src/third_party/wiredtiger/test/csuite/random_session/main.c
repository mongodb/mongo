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
#define N_RETRIES 10

/* Constants and variables declaration. */
static const char conn_config[] = "create,cache_size=1MB,statistics=(all)";

/*
 * compare_uint64_t --
 *     Compare uint64_t.
 */
static int
compare_uint64_t(const void *a, const void *b)
{
    return (*(uint64_t *)a > *(uint64_t *)b) - (*(uint64_t *)a < *(uint64_t *)b);
}

/*
 * main --
 *     The main method.
 */
int
main(int argc, char *argv[])
{
    TEST_OPTS *opts, _opts;
    WT_CONNECTION *conn;
    WT_SESSION_IMPL *(sessions[N_SESSIONS]);
    uint64_t numbers[N_SESSIONS], number = 0, prev_number = 0;
    char home[1024];
    bool random_numbers_repeated = false;

    opts = &_opts;
    memset(opts, 0, sizeof(*opts));
    testutil_check(testutil_parse_opts(argc, argv, opts));

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

    for (int retry = 1; retry <= N_RETRIES; ++retry) {
        /* Reset the thread's timeslice to raise the probability quicker execution. */
        __wt_sleep(0, 10);

        for (int i = 0; i < N_SESSIONS; i++) {
            WT_SESSION *session;
            testutil_check(conn->open_session(conn, NULL, NULL, &session));
            number = __wt_random(&((WT_SESSION_IMPL *)session)->rnd_random);
            testutil_check(session->close(session, NULL));
            if (retry > 1 && prev_number == number) {
                if (retry < N_RETRIES)
                    /* To eliminate flakiness, give it another go. */
                    goto retry_single;
                fprintf(stderr, "Random numbers repeated for %d cycles in single session\n", retry);
                random_numbers_repeated = true;
            }
            prev_number = number;
        }
        break;
retry_single:;
    }

    /*
     * Test multiple sessions.
     *
     * The test generates random numbers with N sessions open simultaneously.
     */

    for (int retry = 1; retry <= N_RETRIES; ++retry) {
        /* Reset the thread's timeslice to raise the probability quicker execution. */
        __wt_sleep(0, 10);

        /* Open sessions as quickly as possible. */
        for (int i = 0; i < N_SESSIONS; i++) {
            WT_SESSION *session;
            testutil_check(conn->open_session(conn, NULL, NULL, &session));
            sessions[i] = (WT_SESSION_IMPL *)session;
        }

        /* Generate a random number. */
        for (int i = 0; i < N_SESSIONS; i++) {
            numbers[i] = number = __wt_random(&sessions[i]->rnd_random);
            if (i > 0 && prev_number == number) {
                if (retry < N_RETRIES)
                    /* To eliminate flakiness, give it another go. */
                    goto retry_multi;
                fprintf(
                  stderr, "Random numbers repeated for %d cycles in subsequent sessions\n", retry);
                random_numbers_repeated = true;
            }
            prev_number = number;
        }

        /* Check if any random numbers repeat. */
        __wt_qsort(numbers, N_SESSIONS, sizeof(uint64_t), compare_uint64_t);
        for (int i = 1; i < N_SESSIONS; i++) {
            if (numbers[i] == numbers[i - 1]) {
                if (retry < N_RETRIES)
                    /* To eliminate flakiness, give it another go. */
                    goto retry_multi;
                fprintf(
                  stderr, "Random numbers repeated for %d cycles in multiple sessions\n", retry);
                random_numbers_repeated = true;
            }
        }
        break;

retry_multi:
        /* Close sessions. */
        for (int i = 0; i < N_SESSIONS; i++)
            testutil_check(sessions[i]->iface.close(&sessions[i]->iface, NULL));
    }

    /* Finish the test and clean up. */

    testutil_check(conn->close(conn, NULL));

    if (!opts->preserve)
        testutil_remove(home);
    testutil_cleanup(opts);

    testutil_assert(!random_numbers_repeated);

    return (EXIT_SUCCESS);
}
