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

/*
 * JIRA ticket reference: WT-2853
 *
 * Test case description: create two threads: one is populating/updating records in a table with a
 * few indices, the other is reading from table and indices. The test is adapted from one that uses
 * cursor joins, this test does not, but simulates some of the access patterns.
 *
 * Failure mode: after a second or two of progress by both threads, they both appear to slow
 * dramatically, almost locking up. After some time (I've observed from a half minute to a few
 * minutes), the lock up ends and both threads seem to be inserting and reading at a normal fast
 * pace. That continues until the test ends (~30 seconds).
 */

static void *thread_insert(void *);
static void *thread_get(void *);

#define BLOOM false
#define GAP_DISPLAY 3 /* Threshold for seconds of gap to be displayed */
#define GAP_ERROR 7   /* Threshold for seconds of gap to be treated as error */
#define N_RECORDS 10000
#define N_INSERT 1000000
#define N_INSERT_THREAD 1
#define N_GET_THREAD 1
#define S64 "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789::"
#define S1024 (S64 S64 S64 S64 S64 S64 S64 S64 S64 S64 S64 S64 S64 S64 S64 S64)

#if GAP_ERROR < GAP_DISPLAY
#error "GAP_ERROR must be >= GAP_DISPLAY"
#endif

typedef struct {
    char posturi[256];
    char baluri[256];
    char flaguri[256];
    bool bloom;
    bool usecolumns;
} SHARED_OPTS;

typedef struct {
    TEST_OPTS *testopts;
    SHARED_OPTS *sharedopts;
    int threadnum;
    int nthread;
    int done;
    int njoins;
    int nfail;
} THREAD_ARGS;

/*
 * main --
 *     TODO: Add a comment describing this function.
 */
int
main(int argc, char *argv[])
{
    SHARED_OPTS *sharedopts, _sharedopts;
    TEST_OPTS *opts, _opts;
    THREAD_ARGS get_args[N_GET_THREAD], insert_args[N_INSERT_THREAD];
    WT_CURSOR *maincur;
    WT_SESSION *session;
    pthread_t get_tid[N_GET_THREAD], insert_tid[N_INSERT_THREAD];
    int i, key, nfail;
    char tableconf[128];
    const char *tablename;

    /*
     * Bypass this test for valgrind or slow test machines. This test is timing sensitive.
     */
    if (testutil_is_flag_set("TESTUTIL_BYPASS_VALGRIND") ||
      testutil_is_flag_set("TESTUTIL_SLOW_MACHINE"))
        return (EXIT_SUCCESS);

    opts = &_opts;
    sharedopts = &_sharedopts;
    memset(opts, 0, sizeof(*opts));
    opts->table_type = TABLE_ROW;
    memset(sharedopts, 0, sizeof(*sharedopts));
    memset(insert_args, 0, sizeof(insert_args));
    memset(get_args, 0, sizeof(get_args));
    nfail = 0;

    sharedopts->bloom = BLOOM;
    testutil_check(testutil_parse_opts(argc, argv, opts));
    if (opts->table_type == TABLE_FIX)
        testutil_die(ENOTSUP, "Fixed-length column store not supported");
    sharedopts->usecolumns = (opts->table_type == TABLE_COL);

    testutil_make_work_dir(opts->home);
    testutil_progress(opts, "start");

    testutil_check(wiredtiger_open(opts->home, NULL, "create,cache_size=1G", &opts->conn));
    testutil_progress(opts, "wiredtiger_open");

    testutil_check(opts->conn->open_session(opts->conn, NULL, NULL, &session));
    testutil_progress(opts, "sessions opened");

    /*
     * Note: id is repeated as id2. This makes it easier to identify the primary key in dumps of the
     * index files.
     */
    testutil_check(__wt_snprintf(tableconf, sizeof(tableconf),
      "key_format=%s,value_format=iiSii,columns=(id,post,bal,extra,flag,id2)",
      sharedopts->usecolumns ? "r" : "i"));
    testutil_check(session->create(session, opts->uri, tableconf));

    tablename = strchr(opts->uri, ':');
    testutil_assert(tablename != NULL);
    tablename++;
    testutil_check(
      __wt_snprintf(sharedopts->posturi, sizeof(sharedopts->posturi), "index:%s:post", tablename));
    testutil_check(
      __wt_snprintf(sharedopts->baluri, sizeof(sharedopts->baluri), "index:%s:bal", tablename));
    testutil_check(
      __wt_snprintf(sharedopts->flaguri, sizeof(sharedopts->flaguri), "index:%s:flag", tablename));

    testutil_check(session->create(session, sharedopts->posturi, "columns=(post)"));
    testutil_check(session->create(session, sharedopts->baluri, "columns=(bal)"));
    testutil_check(session->create(session, sharedopts->flaguri, "columns=(flag)"));

    /*
     * Insert a single record with all items we need to call search() on, this makes our join logic
     * easier.
     */
    testutil_check(session->open_cursor(session, opts->uri, NULL, NULL, &maincur));
    /*
     * Do not constant-fold this assignment: in gcc 10.3, if you pass the constant directly to
     * set_key, -Wduplicated-branches fails to notice the type difference between the two cases and
     * gives a spurious warning, and diagnostic builds fail.
     */
    key = N_RECORDS + 1;
    if (sharedopts->usecolumns)
        maincur->set_key(maincur, (uint64_t)key);
    else
        maincur->set_key(maincur, key);
    maincur->set_value(maincur, 54321, 0, "", 0, N_RECORDS + 1);
    testutil_check(maincur->insert(maincur));
    testutil_check(maincur->close(maincur));
    testutil_check(session->close(session, NULL));
    testutil_progress(opts, "setup complete");

    for (i = 0; i < N_INSERT_THREAD; ++i) {
        insert_args[i].threadnum = i;
        insert_args[i].nthread = N_INSERT_THREAD;
        insert_args[i].testopts = opts;
        insert_args[i].sharedopts = sharedopts;
        testutil_check(pthread_create(&insert_tid[i], NULL, thread_insert, &insert_args[i]));
    }

    for (i = 0; i < N_GET_THREAD; ++i) {
        get_args[i].threadnum = i;
        get_args[i].nthread = N_GET_THREAD;
        get_args[i].testopts = opts;
        get_args[i].sharedopts = sharedopts;
        testutil_check(pthread_create(&get_tid[i], NULL, thread_get, &get_args[i]));
    }
    testutil_progress(opts, "threads started");

    /*
     * Wait for insert threads to finish. When they are done, signal get threads to complete.
     */
    for (i = 0; i < N_INSERT_THREAD; ++i)
        testutil_check(pthread_join(insert_tid[i], NULL));

    for (i = 0; i < N_GET_THREAD; ++i)
        get_args[i].done = 1;

    for (i = 0; i < N_GET_THREAD; ++i)
        testutil_check(pthread_join(get_tid[i], NULL));

    testutil_progress(opts, "threads joined");
    fprintf(stderr, "\n");
    for (i = 0; i < N_GET_THREAD; ++i) {
        fprintf(stderr, "  thread %d did %d joins (%d fails)\n", i, get_args[i].njoins,
          get_args[i].nfail);
        nfail += get_args[i].nfail;
    }

    /*
     * Note that slow machines can be skipped for this test. See the bypass code earlier.
     */
    if (nfail != 0)
        fprintf(stderr,
          "ERROR: %d failures when a single commit took more than %d seconds.\n"
          "This may indicate a real problem or a particularly slow machine.\n",
          nfail, GAP_ERROR);
    testutil_assert(nfail == 0);
    testutil_progress(opts, "cleanup starting");
    testutil_cleanup(opts);
    return (0);
}

/*
 * thread_insert --
 *     TODO: Add a comment describing this function.
 */
static void *
thread_insert(void *arg)
{
    SHARED_OPTS *sharedopts;
    TEST_OPTS *opts;
    THREAD_ARGS *threadargs;
    WT_CURSOR *maincur;
    WT_RAND_STATE rnd;
    WT_SESSION *session;
    uint64_t curtime, elapsed, prevtime; /* 1 second resolution enough */
    int bal, i, flag, key, post;
    const char *extra = S1024;

    threadargs = (THREAD_ARGS *)arg;
    opts = threadargs->testopts;
    sharedopts = threadargs->sharedopts;

    testutil_check(opts->conn->open_session(opts->conn, NULL, NULL, &session));

    __wt_random_init_seed((WT_SESSION_IMPL *)session, &rnd);
    __wt_seconds((WT_SESSION_IMPL *)session, &prevtime);

    testutil_check(session->open_cursor(session, opts->uri, NULL, NULL, &maincur));

    testutil_progress(opts, "insert start");
    for (i = 0; i < N_INSERT; i++) {
        /*
         * Insert threads may stomp on each other's records; that's okay.
         */
        key = (int)(__wt_random(&rnd) % N_RECORDS) + 1;
        testutil_check(session->begin_transaction(session, NULL));
        if (sharedopts->usecolumns)
            maincur->set_key(maincur, (uint64_t)key);
        else
            maincur->set_key(maincur, key);
        if (__wt_random(&rnd) % 2 == 0)
            post = 54321;
        else
            post = i % 100000;
        if (__wt_random(&rnd) % 2 == 0) {
            bal = -100;
            flag = 1;
        } else {
            bal = 100 * (i + 1);
            flag = 0;
        }
        maincur->set_value(maincur, post, bal, extra, flag, key);
        testutil_check(maincur->insert(maincur));
        testutil_check(maincur->reset(maincur));
        testutil_check(session->commit_transaction(session, NULL));
        if (i % 1000 == 0 && i != 0) {
            if (i % 10000 == 0)
                fprintf(stderr, "*");
            else
                fprintf(stderr, ".");
            __wt_seconds((WT_SESSION_IMPL *)session, &curtime);
            elapsed = curtime - prevtime;
            if (elapsed > GAP_DISPLAY) {
                testutil_progress(opts, "insert time gap");
                fprintf(stderr,
                  "\n"
                  "GAP: %" PRIu64 " secs after %d inserts\n",
                  elapsed, i);
            }
            if (elapsed > GAP_ERROR)
                threadargs->nfail++;
            prevtime = curtime;
        }
    }
    testutil_progress(opts, "insert end");
    testutil_check(maincur->close(maincur));
    testutil_check(session->close(session, NULL));
    return (NULL);
}

/*
 * thread_get --
 *     TODO: Add a comment describing this function.
 */
static void *
thread_get(void *arg)
{
    SHARED_OPTS *sharedopts;
    TEST_OPTS *opts;
    THREAD_ARGS *threadargs;
    WT_CURSOR *maincur, *postcur;
    WT_SESSION *session;
    uint64_t curtime, elapsed, prevtime; /* 1 second resolution enough */
    int bal, bal2, flag, flag2, key, key2, post, post2;
    char *extra;

    threadargs = (THREAD_ARGS *)arg;
    opts = threadargs->testopts;
    sharedopts = threadargs->sharedopts;

    testutil_check(opts->conn->open_session(opts->conn, NULL, NULL, &session));

    __wt_seconds((WT_SESSION_IMPL *)session, &prevtime);

    testutil_check(session->open_cursor(session, opts->uri, NULL, NULL, &maincur));
    testutil_check(session->open_cursor(session, sharedopts->posturi, NULL, NULL, &postcur));

    testutil_progress(opts, "get start");
    for (threadargs->njoins = 0; threadargs->done == 0; threadargs->njoins++) {
        testutil_check(session->begin_transaction(session, NULL));
        postcur->set_key(postcur, 54321);
        testutil_check(postcur->search(postcur));
        while (postcur->next(postcur) == 0) {
            testutil_check(postcur->get_key(postcur, &post));
            testutil_check(postcur->get_value(postcur, &post2, &bal, &extra, &flag, &key));
            testutil_assert((flag > 0 && bal < 0) || (flag == 0 && bal >= 0));

            if (sharedopts->usecolumns)
                maincur->set_key(maincur, (uint64_t)key);
            else
                maincur->set_key(maincur, key);
            fflush(stdout);
            testutil_check(maincur->search(maincur));
            testutil_check(maincur->get_value(maincur, &post2, &bal2, &extra, &flag2, &key2));
            testutil_check(maincur->reset(maincur));
            testutil_assert((flag2 > 0 && bal2 < 0) || (flag2 == 0 && bal2 >= 0));
        }
        /*
         * Reset the cursors, potentially allowing the insert threads to proceed.
         */
        testutil_check(postcur->reset(postcur));
        if (threadargs->njoins % 100 == 0)
            fprintf(stderr, "G");
        testutil_check(session->rollback_transaction(session, NULL));

        __wt_seconds((WT_SESSION_IMPL *)session, &curtime);
        elapsed = curtime - prevtime;
        if (elapsed > GAP_DISPLAY) {
            testutil_progress(opts, "get time gap");
            fprintf(stderr,
              "\n"
              "GAP: %" PRIu64 " secs after %d gets\n",
              elapsed, threadargs->njoins);
        }
        if (elapsed > GAP_ERROR)
            threadargs->nfail++;
        prevtime = curtime;
    }
    testutil_progress(opts, "get end");
    testutil_check(postcur->close(postcur));
    testutil_check(maincur->close(maincur));
    testutil_check(session->close(session, NULL));
    return (NULL);
}
