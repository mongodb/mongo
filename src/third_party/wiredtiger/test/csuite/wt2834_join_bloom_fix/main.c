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
 * JIRA ticket reference: WT-2834
 * Test case description: We are creating bank 'account' records, each
 * having a postal_code, balance, and an 'overdrawn' flag.  We insert
 * records with various balances, and only set the overdrawn flag when the
 * balance is negative.  Then we set up a join to simulate this:
 *
 *   select (*) from account where account.postal_code = '54321' and
 *                      account.balance < 0 and not account.overdrawn
 *
 * Failure mode: We get results back from our join.
 */
#define N_RECORDS 100000
#define N_INSERT 1000000

void populate(TEST_OPTS *opts);

/*
 * main --
 *     TODO: Add a comment describing this function.
 */
int
main(int argc, char *argv[])
{
    TEST_OPTS *opts, _opts;
    WT_CURSOR *balancecur, *flagcur, *joincur, *postcur;
    WT_CURSOR *maincur;
    WT_SESSION *session;
    int balance, count, flag, key, key2, post, ret;
    char balanceuri[256];
    char cfg[128];
    char flaguri[256];
    char joinuri[256];
    char posturi[256];
    const char *tablename;

    opts = &_opts;
    memset(opts, 0, sizeof(*opts));
    testutil_check(testutil_parse_opts(argc, argv, opts));
    testutil_make_work_dir(opts->home);
    testutil_progress(opts, "start");

    testutil_check(wiredtiger_open(opts->home, NULL, "create,cache_size=250M", &opts->conn));
    testutil_progress(opts, "wiredtiger_open");
    testutil_check(opts->conn->open_session(opts->conn, NULL, NULL, &session));
    testutil_progress(opts, "sessions opened");

    /*
     * Note: repeated primary key 'id' as 'id2'. This makes it easier to dump an index and know
     * which record we're looking at.
     */
    testutil_check(session->create(
      session, opts->uri, "key_format=i,value_format=iiii,columns=(id,post,balance,flag,id2)"));

    tablename = strchr(opts->uri, ':');
    testutil_assert(tablename != NULL);
    tablename++;
    testutil_check(__wt_snprintf(posturi, sizeof(posturi), "index:%s:post", tablename));
    testutil_check(__wt_snprintf(balanceuri, sizeof(balanceuri), "index:%s:balance", tablename));
    testutil_check(__wt_snprintf(flaguri, sizeof(flaguri), "index:%s:flag", tablename));
    testutil_check(__wt_snprintf(joinuri, sizeof(joinuri), "join:%s", opts->uri));

    testutil_check(session->create(session, posturi, "columns=(post)"));
    testutil_check(session->create(session, balanceuri, "columns=(balance)"));
    testutil_check(session->create(session, flaguri, "columns=(flag)"));
    testutil_progress(opts, "setup complete");

    /*
     * Insert a single record with all items we are search for, this makes our logic easier.
     */
    testutil_check(session->open_cursor(session, opts->uri, NULL, NULL, &maincur));
    maincur->set_key(maincur, N_RECORDS);
    maincur->set_value(maincur, 54321, 0, "", 0, N_RECORDS);
    testutil_check(maincur->insert(maincur));
    testutil_check(maincur->close(maincur));
    testutil_check(session->close(session, NULL));

    testutil_progress(opts, "populate start");
    populate(opts);
    testutil_progress(opts, "populate end");

    testutil_check(opts->conn->open_session(opts->conn, NULL, NULL, &session));

    testutil_check(session->open_cursor(session, posturi, NULL, NULL, &postcur));
    testutil_check(session->open_cursor(session, balanceuri, NULL, NULL, &balancecur));
    testutil_check(session->open_cursor(session, flaguri, NULL, NULL, &flagcur));
    testutil_check(session->open_cursor(session, joinuri, NULL, NULL, &joincur));

    postcur->set_key(postcur, 54321);
    testutil_check(postcur->search(postcur));
    testutil_check(session->join(session, joincur, postcur, "compare=eq"));

    balancecur->set_key(balancecur, 0);
    testutil_check(balancecur->search(balancecur));
    testutil_check(
      __wt_snprintf(cfg, sizeof(cfg), "compare=lt,strategy=bloom,count=%d", N_RECORDS / 100));
    testutil_check(session->join(session, joincur, balancecur, cfg));

    flagcur->set_key(flagcur, 0);
    testutil_check(flagcur->search(flagcur));
    testutil_check(
      __wt_snprintf(cfg, sizeof(cfg), "compare=eq,strategy=bloom,count=%d", N_RECORDS / 100));
    testutil_check(session->join(session, joincur, flagcur, cfg));

    /* Expect no values returned */
    count = 0;
    while ((ret = joincur->next(joincur)) == 0) {
        /*
         * The values may already have been changed, but print them for informational purposes.
         */
        testutil_check(joincur->get_key(joincur, &key));
        testutil_check(joincur->get_value(joincur, &post, &balance, &flag, &key2));
        fprintf(stderr, "FAIL: key=%d/%d, postal_code=%d, balance=%d, flag=%d\n", key, key2, post,
          balance, flag);
        count++;
    }
    testutil_assert(ret == WT_NOTFOUND);
    testutil_assert(count == 0);

    testutil_progress(opts, "cleanup starting");
    testutil_cleanup(opts);
    return (EXIT_SUCCESS);
}

/*
 * populate --
 *     TODO: Add a comment describing this function.
 */
void
populate(TEST_OPTS *opts)
{
    WT_CURSOR *maincur;
    WT_RAND_STATE rnd;
    WT_SESSION *session;
    uint32_t key;
    int balance, i, flag, post;

    __wt_random_init_seed(NULL, &rnd);

    testutil_check(opts->conn->open_session(opts->conn, NULL, NULL, &session));

    testutil_check(session->open_cursor(session, opts->uri, NULL, NULL, &maincur));

    for (i = 0; i < N_INSERT; i++) {
        testutil_check(session->begin_transaction(session, NULL));
        key = (__wt_random(&rnd) % (N_RECORDS));
        maincur->set_key(maincur, key);
        if (__wt_random(&rnd) % 11 == 0)
            post = 54321;
        else
            post = i % 100000;
        if (__wt_random(&rnd) % 4 == 0) {
            balance = -100;
            flag = 1;
        } else {
            balance = 100 * (i + 1);
            flag = 0;
        }
        maincur->set_value(maincur, post, balance, flag, key);
        testutil_check(maincur->insert(maincur));
        testutil_check(session->commit_transaction(session, NULL));
    }
    testutil_check(maincur->close(maincur));
    testutil_check(session->close(session, NULL));
}
