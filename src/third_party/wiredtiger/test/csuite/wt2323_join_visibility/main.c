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
 * JIRA ticket reference: WT-2323
 *
 * Test case description: We create two kinds of threads that race: One kind
 * is populating/updating records in a table with a few indices, the other
 * is reading from a join of that table. The hope in constructing this test
 * was to have the updates interleaved between reads of multiple indices by
 * the join, yielding an inconsistent view of the data.  In the main table,
 * we insert account records, with a positive or negative balance.  The
 * negative balance accounts always have a flag set to non-zero, positive
 * balances have the flag set to zero.  The join we do is:
 *
 *   select (*) from account where account.postal_code = '54321' and
 *                      account.balance < 0 and account.flags == 0
 *
 * which should always yield no results.
 *
 * Failure mode: This test never actually failed with any combination of
 * parameters, with N_INSERT up to 50000000.  It seems that a snapshot is
 * implicitly allocated in the session used by a join by the set_key calls
 * that occur before the first 'next' of the join cursor is done.  Despite
 * that, the test seems interesting enough to keep around, with the number
 * of inserts set low as a default.
 */

#define N_RECORDS 10000
#define N_INSERT 500000
#define N_INSERT_THREAD 2
#define N_JOIN_THREAD 2
#define S64 "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789::"
#define S1024 (S64 S64 S64 S64 S64 S64 S64 S64 S64 S64 S64 S64 S64 S64 S64 S64)

typedef struct {
    char posturi[256];
    char baluri[256];
    char flaguri[256];
    char joinuri[256];
    bool bloom;
    bool remove;
} SHARED_OPTS;

typedef struct {
    TEST_OPTS *testopts;
    SHARED_OPTS *sharedopts;
    int threadnum;
    int nthread;
    int done;
    int joins;
    int removes;
    int inserts;
    int notfounds;
    int rollbacks;
} THREAD_ARGS;

static void *thread_insert(void *);
static void *thread_join(void *);
static void test_join(TEST_OPTS *, SHARED_OPTS *, bool, bool);

/*
 * main --
 *     TODO: Add a comment describing this function.
 */
int
main(int argc, char *argv[])
{
    SHARED_OPTS *sharedopts, _sharedopts;
    TEST_OPTS *opts, _opts;
    const char *tablename;

    opts = &_opts;
    sharedopts = &_sharedopts;
    memset(opts, 0, sizeof(*opts));
    memset(sharedopts, 0, sizeof(*sharedopts));

    testutil_check(testutil_parse_opts(argc, argv, opts));
    testutil_make_work_dir(opts->home);

    tablename = strchr(opts->uri, ':');
    testutil_assert(tablename != NULL);
    tablename++;
    testutil_check(
      __wt_snprintf(sharedopts->posturi, sizeof(sharedopts->posturi), "index:%s:post", tablename));
    testutil_check(
      __wt_snprintf(sharedopts->baluri, sizeof(sharedopts->baluri), "index:%s:bal", tablename));
    testutil_check(
      __wt_snprintf(sharedopts->flaguri, sizeof(sharedopts->flaguri), "index:%s:flag", tablename));
    testutil_check(
      __wt_snprintf(sharedopts->joinuri, sizeof(sharedopts->joinuri), "join:%s", opts->uri));

    testutil_check(wiredtiger_open(opts->home, NULL, "create,cache_size=1G", &opts->conn));

    test_join(opts, sharedopts, true, true);
    test_join(opts, sharedopts, true, false);
    test_join(opts, sharedopts, false, true);
    test_join(opts, sharedopts, false, false);

    testutil_cleanup(opts);

    return (0);
}

/*
 * test_join --
 *     TODO: Add a comment describing this function.
 */
static void
test_join(TEST_OPTS *opts, SHARED_OPTS *sharedopts, bool bloom, bool sometimes_remove)
{
    THREAD_ARGS insert_args[N_INSERT_THREAD], join_args[N_JOIN_THREAD];
    WT_CURSOR *maincur;
    WT_SESSION *session;
    pthread_t insert_tid[N_INSERT_THREAD], join_tid[N_JOIN_THREAD];
    int i;

    memset(insert_args, 0, sizeof(insert_args));
    memset(join_args, 0, sizeof(join_args));

    sharedopts->bloom = bloom;
    sharedopts->remove = sometimes_remove;

    fprintf(stderr, "Running with bloom=%d, remove=%d\n", (int)bloom, (int)sometimes_remove);

    testutil_check(opts->conn->open_session(opts->conn, NULL, NULL, &session));

    /*
     * Note: id is repeated as id2. This makes it easier to identify the primary key in dumps of the
     * index files.
     */
    testutil_check(session->create(
      session, opts->uri, "key_format=i,value_format=iiSii,columns=(id,post,bal,extra,flag,id2)"));

    testutil_check(session->create(session, sharedopts->posturi, "columns=(post)"));
    testutil_check(session->create(session, sharedopts->baluri, "columns=(bal)"));
    testutil_check(session->create(session, sharedopts->flaguri, "columns=(flag)"));

    /*
     * Insert a single record with all items we need to call search() on, this makes our join logic
     * easier.
     */
    testutil_check(session->open_cursor(session, opts->uri, NULL, NULL, &maincur));
    maincur->set_key(maincur, N_RECORDS);
    maincur->set_value(maincur, 54321, 0, "", 0, N_RECORDS);
    testutil_check(maincur->insert(maincur));
    testutil_check(maincur->close(maincur));

    for (i = 0; i < N_INSERT_THREAD; ++i) {
        insert_args[i].threadnum = i;
        insert_args[i].nthread = N_INSERT_THREAD;
        insert_args[i].testopts = opts;
        insert_args[i].sharedopts = sharedopts;
        testutil_check(pthread_create(&insert_tid[i], NULL, thread_insert, &insert_args[i]));
    }

    for (i = 0; i < N_JOIN_THREAD; ++i) {
        join_args[i].threadnum = i;
        join_args[i].nthread = N_JOIN_THREAD;
        join_args[i].testopts = opts;
        join_args[i].sharedopts = sharedopts;
        testutil_check(pthread_create(&join_tid[i], NULL, thread_join, &join_args[i]));
    }

    /*
     * Wait for insert threads to finish. When they are done, signal join threads to complete.
     */
    for (i = 0; i < N_INSERT_THREAD; ++i)
        testutil_check(pthread_join(insert_tid[i], NULL));

    for (i = 0; i < N_JOIN_THREAD; ++i)
        join_args[i].done = 1;

    for (i = 0; i < N_JOIN_THREAD; ++i)
        testutil_check(pthread_join(join_tid[i], NULL));

    fprintf(stderr, "\n");
    for (i = 0; i < N_JOIN_THREAD; ++i) {
        fprintf(stderr, "  join thread %d did %d joins\n", i, join_args[i].joins);
    }
    for (i = 0; i < N_INSERT_THREAD; ++i)
        fprintf(stderr,
          "  insert thread %d did %d inserts, %d removes, %d notfound, %d rollbacks\n", i,
          insert_args[i].inserts, insert_args[i].removes, insert_args[i].notfounds,
          insert_args[i].rollbacks);

    testutil_drop(session, sharedopts->posturi, NULL);
    testutil_drop(session, sharedopts->baluri, NULL);
    testutil_drop(session, sharedopts->flaguri, NULL);
    testutil_drop(session, opts->uri, NULL);
    testutil_check(session->close(session, NULL));
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
    int bal, i, flag, key, post, ret;
    const char *extra = S1024;

    threadargs = (THREAD_ARGS *)arg;
    opts = threadargs->testopts;
    sharedopts = threadargs->sharedopts;
    __wt_random_init_seed(NULL, &rnd);

    testutil_check(opts->conn->open_session(opts->conn, NULL, NULL, &session));

    testutil_check(session->open_cursor(session, opts->uri, NULL, NULL, &maincur));

    for (i = 0; i < N_INSERT; i++) {
        /*
         * Insert threads may stomp on each other's records; that's okay.
         */
        key = (int)(__wt_random(&rnd) % N_RECORDS);
        maincur->set_key(maincur, key);
/* FIXME-WT-6180: disable lower isolation levels. */
#if 0
        if (sharedopts->remove)
            testutil_check(session->begin_transaction(session, "isolation=snapshot"));
#else
        testutil_check(session->begin_transaction(session, "isolation=snapshot"));
#endif
        if (sharedopts->remove && __wt_random(&rnd) % 5 == 0 && maincur->search(maincur) == 0) {
            /*
             * Another thread can be removing at the same time.
             */
            ret = maincur->remove(maincur);
            testutil_assert(
              ret == 0 || (N_INSERT_THREAD > 1 && (ret == WT_NOTFOUND || ret == WT_ROLLBACK)));
            if (ret == 0)
                threadargs->removes++;
            else if (ret == WT_NOTFOUND)
                threadargs->notfounds++;
            else if (ret == WT_ROLLBACK)
                threadargs->rollbacks++;
        } else {
            if (__wt_random(&rnd) % 2 == 0)
                post = 54321;
            else
                post = i % 100000;
            if (__wt_random(&rnd) % 2 == 0) {
                bal = -100;
                flag = 1;
            } else {
                bal = 1 + (i % 1000) * 100;
                flag = 0;
            }
            maincur->set_value(maincur, post, bal, extra, flag, key);
            ret = maincur->insert(maincur);
            testutil_assert(ret == 0 || (N_INSERT_THREAD > 1 && ret == WT_ROLLBACK));
            testutil_check(maincur->reset(maincur));
            if (ret == 0)
                threadargs->inserts++;
            else if (ret == WT_ROLLBACK)
                threadargs->rollbacks++;
        }
/* FIXME-WT-6180: disable lower isolation levels. */
#if 0
        if (sharedopts->remove) {
            if (ret == WT_ROLLBACK)
                testutil_check(session->rollback_transaction(session, NULL));
            else
                testutil_check(session->commit_transaction(session, NULL));
        }
#else
        if (ret == WT_ROLLBACK)
            testutil_check(session->rollback_transaction(session, NULL));
        else
            testutil_check(session->commit_transaction(session, NULL));
#endif
        if (i % 1000 == 0 && i != 0) {
            if (i % 10000 == 0)
                fprintf(stderr, "*");
            else
                fprintf(stderr, ".");
        }
    }
    testutil_check(maincur->close(maincur));
    testutil_check(session->close(session, NULL));
    return (NULL);
}

/*
 * thread_join --
 *     TODO: Add a comment describing this function.
 */
static void *
thread_join(void *arg)
{
    SHARED_OPTS *sharedopts;
    TEST_OPTS *opts;
    THREAD_ARGS *threadargs;
    WT_CURSOR *balcur, *flagcur, *joincur, *postcur;
    WT_SESSION *session;
    int bal, flag, key, key2, post, ret;
    char cfg[128];
    char *extra;

    threadargs = (THREAD_ARGS *)arg;
    opts = threadargs->testopts;
    sharedopts = threadargs->sharedopts;

    testutil_check(opts->conn->open_session(opts->conn, NULL, NULL, &session));

    testutil_check(session->open_cursor(session, sharedopts->posturi, NULL, NULL, &postcur));
    testutil_check(session->open_cursor(session, sharedopts->baluri, NULL, NULL, &balcur));
    testutil_check(session->open_cursor(session, sharedopts->flaguri, NULL, NULL, &flagcur));

    for (threadargs->joins = 0; threadargs->done == 0; threadargs->joins++) {
        testutil_check(session->open_cursor(session, sharedopts->joinuri, NULL, NULL, &joincur));
        postcur->set_key(postcur, 54321);
        testutil_check(postcur->search(postcur));
        testutil_check(session->join(session, joincur, postcur, "compare=eq"));

        balcur->set_key(balcur, 0);
        testutil_check(balcur->search(balcur));
        if (sharedopts->bloom)
            testutil_check(
              __wt_snprintf(cfg, sizeof(cfg), "compare=lt,strategy=bloom,count=%d", N_RECORDS));
        else
            testutil_check(__wt_snprintf(cfg, sizeof(cfg), "compare=lt"));
        testutil_check(session->join(session, joincur, balcur, cfg));

        flagcur->set_key(flagcur, 0);
        testutil_check(flagcur->search(flagcur));
        if (sharedopts->bloom)
            testutil_check(
              __wt_snprintf(cfg, sizeof(cfg), "compare=eq,strategy=bloom,count=%d", N_RECORDS));
        else
            testutil_check(__wt_snprintf(cfg, sizeof(cfg), "compare=eq"));
        testutil_check(session->join(session, joincur, flagcur, cfg));

        /* Expect no values returned */
        ret = joincur->next(joincur);
        if (ret == 0) {
            /*
             * The values may already have been changed, but print them for informational purposes.
             */
            testutil_check(joincur->get_key(joincur, &key));
            testutil_check(joincur->get_value(joincur, &post, &bal, &extra, &flag, &key2));
            fprintf(stderr, "FAIL: iteration %d: key=%d/%d, postal_code=%d, balance=%d, flag=%d\n",
              threadargs->joins, key, key2, post, bal, flag);
            /* Save the results. */
            testutil_check(opts->conn->close(opts->conn, NULL));
            opts->conn = NULL;
            return (NULL);
        }
        testutil_assert(ret == WT_NOTFOUND);
        testutil_check(joincur->close(joincur));

        /*
         * Reset the cursors, potentially allowing the insert threads to proceed.
         */
        testutil_check(postcur->reset(postcur));
        testutil_check(balcur->reset(balcur));
        testutil_check(flagcur->reset(flagcur));
        if (threadargs->joins % 100 == 0)
            fprintf(stderr, "J");
    }
    testutil_check(postcur->close(postcur));
    testutil_check(balcur->close(balcur));
    testutil_check(flagcur->close(flagcur));
    testutil_check(session->close(session, NULL));
    return (NULL);
}
