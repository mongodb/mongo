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
 * This test reproduces WT-10461 in which platforms with weak memory models such as ARM
 * can insert items into a skiplist using an incorrect next_stack. Upper levels of
 * the next_stack should always point to larger keys than lower levels of the stack
 * but we can violate this constraint if we have the following (simplified) scenario:
 *
 * 1. Four keys are added to the same insert list: A, B, C, and D. The keys are ordered
 *    such that A < B < C < D
 * 2. Keys A and D are already present in the insert list. Keys B and C are inserted
 *    at the same time with C inserted slightly earlier.
 * 3. As C is being inserted A's next_stack pointers - previously pointing at D - will
 *    be updated to point to C. These pointers are updated from the bottom of A's
 *    next_stack upwards.
 * 4. As B is preparing to be inserted it builds its own next_stack by choosing pointers
 *    from the top of A's next_stack and moving downwards.
 * 5. Provided that pointers in step 3 are written bottom up and pointers in step 4 are
 *    read top down the resulting pointers in B's next_stack will be consistent, but if
 *    pointers are read out of order in step 4 (due to weak memory models being more
 *    lenient about ordering of reads from memory) B can set an old pointer to key D
 *    in a lower level and then set a newer pointer to C in an upper level violating our
 *    constraint that upper levels in next_stacks must point to larger keys than lower
 *    levels.
 *
 * To reproduce the above we set up a scenario with a skip list containing keys "0" (A)
 * and "99999" (D). New keys are continually inserted in a decreasing order to represent
 * the insertion of C, while in a parallel thread we emulate the insertion of B by continually
 * calling __wt_search_insert for key "00". Note that we're not actually inserting B here,
 * just repeating the critical section of B's insertion where the out of order read can occur.
 * We run this __wt_search_insert in parallel across as many threads as possible to increase
 * the chance of the error firing.
 */

#include <time.h>
#include "test_util.h"

extern int __wt_optind;
extern char *__wt_optarg;

static const char *uri = "table:wt10014_skip_stress_test";

static volatile bool inserts_finished;
static volatile uint32_t active_search_insert_threads;

#define NUM_KEYS 10000
#define KEY_SIZE 6
#define CHECK_KEY_SIZE 3

/*
 * usage --
 *     Print a usage message.
 */
__attribute__((noreturn)) static void
usage(void)
{
    fprintf(stderr, "usage: %s [-h dir]\n", progname);
    exit(EXIT_FAILURE);
}

/*
 * insert_key --
 *     Helper function to insert a key. For this test we only care about keys and insert a dummy
 *     value.
 */
static void
insert_key(WT_CURSOR *cursor, const char *key)
{
    cursor->set_key(cursor, key);
    cursor->set_value(cursor, "");
    testutil_check(cursor->insert(cursor));
}

/*
 * thread_search_insert_run --
 *     Find the insert list under test and then continually build a list of skiplist pointers as if
 *     we were going to insert a new key. This function does not insert a new key though as we want
 *     to stress the construction of the next_stack built by the function. If out-of-order reads
 *     occur as a result of this function call it is caught by an assertion in __wt_search_insert.
 *
 * !!!! Note !!!! This function is not a proper usage of the WT API. It's white box and accesses
 *     internal WiredTiger functions in order to directly stress the __wt_search_insert function.
 */
static WT_THREAD_RET
thread_search_insert_run(void *arg)
{
    WT_CONNECTION *conn;
    WT_CURSOR *cursor;
    WT_CURSOR_BTREE *cbt;
    WT_ITEM check_key;
    WT_SESSION *session;

    conn = (WT_CONNECTION *)arg;
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    /*
     * Position the cursor on our insert list under stress. We know "0" is present as we inserted
     * during test setup.
     */
    testutil_check(session->open_cursor(session, uri, NULL, NULL, &cursor));
    cbt = (WT_CURSOR_BTREE *)cursor;
    cursor->set_key(cursor, "0");
    testutil_check(cursor->search(cursor));

    /*
     * We need the session to have a dhandle set so that __wt_search_insert can access
     * `session->dhandle->handle->collator`. This would already be set if we were calling
     * __wt_search_insert through the proper channels.
     */
    ((WT_SESSION_IMPL *)session)->dhandle = cbt->dhandle;

    /*
     * Set up our key to __wt_search_insert on. It'll always sit just after the first item in the
     * skiplist.
     */
    check_key.data = dmalloc(CHECK_KEY_SIZE);
    testutil_check(__wt_snprintf((char *)check_key.data, CHECK_KEY_SIZE, "00"));
    check_key.size = CHECK_KEY_SIZE;

    __wt_atomic_addv32(&active_search_insert_threads, 1);
    while (inserts_finished == false)
        WT_IGNORE_RET(
          __wt_search_insert((WT_SESSION_IMPL *)session, cbt, cbt->ins_head, &check_key));
    __wt_atomic_subv32(&active_search_insert_threads, 1);

    cursor->close(cursor);
    session->close(session, "");
    free((void *)check_key.data);
    return (WT_THREAD_RET_VALUE);
}

/*
 * run --
 *     Setup the skiplist under stress, spin up search_insert threads to run in parallel, and then
 *     start inserting keys in decreasing order.
 */
static void
run(const char *working_dir)
{
    wt_thread_t *thr;
    uint32_t num_search_insert_threads;

    WT_CONNECTION *conn;
    WT_SESSION *session;
    WT_CURSOR *cursor;
    char *key;

    int status;
    char command[1024], home[1024];

    inserts_finished = false;
    active_search_insert_threads = 0;

    testutil_work_dir_from_path(home, sizeof(home), working_dir);
    testutil_check(__wt_snprintf(command, sizeof(command), "rm -rf %s; mkdir %s", home, home));
    if ((status = system(command)) < 0)
        testutil_die(status, "system: %s", command);

    testutil_check(wiredtiger_open(home, NULL, "create,debug_mode=(stress_skiplist=1)", &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    /*
     * We want this whole test to run on a single insert list. Set a very large memory_page_max to
     * prevent the page from splitting.
     */
    testutil_check(
      session->create(session, uri, "key_format=S,value_format=S,memory_page_max=1TB"));
    testutil_check(session->open_cursor(session, uri, NULL, NULL, &cursor));

    /* Insert keys A and D from the description at the top of the file. */
    insert_key(cursor, "0");
    insert_key(cursor, "99999");

    /*
     * Wait for search_insert threads to be up and running. We have one insert thread and everything
     * else is a search_insert thread.
     */
    num_search_insert_threads = (uint32_t)(sysconf(_SC_NPROCESSORS_ONLN) - 1);
    thr = dmalloc(num_search_insert_threads * sizeof(wt_thread_t));
    for (uint32_t i = 0; i < num_search_insert_threads; i++)
        testutil_check(__wt_thread_create(NULL, &thr[i], thread_search_insert_run, conn));

    while (active_search_insert_threads != num_search_insert_threads)
        ;

    key = dmalloc(KEY_SIZE);
    testutil_check(session->begin_transaction(session, NULL));
    for (uint32_t i = NUM_KEYS; i > 0; i--) {
        /* All keys use leading zeros. Otherwise "2" is consider larger than "11". */
        testutil_check(__wt_snprintf(key, KEY_SIZE, "%0*u", KEY_SIZE - 1, i));
        insert_key(cursor, key);
    }
    testutil_check(session->commit_transaction(session, NULL));

    inserts_finished = true;
    for (uint32_t i = 0; i < num_search_insert_threads; i++)
        testutil_check(__wt_thread_join(NULL, &thr[i]));

    free(thr);
    free(key);
    testutil_check(cursor->close(cursor));
    testutil_check(session->close(session, ""));
    testutil_check(conn->close(conn, ""));
    testutil_clean_test_artifacts(home);
    testutil_clean_work_dir(home);
}

/*
 * main --
 *     Stress test for out-of-order reads in __wt_search_insert on platforms with weak memory
 *     ordering.
 */
int
main(int argc, char *argv[])
{
    struct timespec now, start;
    int ch;
    const char *working_dir;

    working_dir = "WT_TEST.wt10461_skip_list_stress";

    while ((ch = __wt_getopt(progname, argc, argv, "h:")) != EOF)
        switch (ch) {
        case 'h':
            working_dir = __wt_optarg;
            break;
        default:
            usage();
        }

    argc -= __wt_optind;
    if (argc != 0)
        usage();

    __wt_epoch(NULL, &start);
    for (int j = 0;; j++) {
        printf("Run %d\n", j);
        run(working_dir);

        __wt_epoch(NULL, &now);
        if (WT_TIMEDIFF_SEC(now, start) >= (15 * WT_MINUTE))
            break;
    }
    return 0;
}
