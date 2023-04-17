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

#include <sys/wait.h>
#include <signal.h>

/*
 * This test verifies that we can quickly interrupt compact, before it does much meaningful work.
 */
#define NUM_RECORDS1 (10)
#define NUM_RECORDS2 (100 * WT_THOUSAND)

/* The table URI. */
#define TABLE_URI "table:compact"

/* Constants and variables declaration. */
static const char conn_config[] = "create,cache_size=2GB,statistics=(all),verbose=[compact]";
static const char table_config[] =
  "allocation_size=4KB,leaf_page_max=4KB,key_format=Q,value_format=" WT_UNCHECKED_STRING(QS);

static char data_str[1024] = "";
static bool interrupted_compaction = false;
static bool skipped_compaction = false;
static bool do_interrupt_compaction = false;

/*
 * error_handler --
 *     Error handler.
 */
static int
error_handler(WT_EVENT_HANDLER *handler, WT_SESSION *session, int error, const char *message)
{
    (void)(handler);
    (void)(session);
    (void)(error);

    if (strstr(message, "compact interrupted") != NULL)
        interrupted_compaction = true;

    fprintf(stderr, "%s\n", message);
    return (0);
}

/*
 * message_handler --
 *     Message handler.
 */
static int
message_handler(WT_EVENT_HANDLER *handler, WT_SESSION *session, const char *message)
{
    (void)(handler);
    (void)(session);

    if (strstr(message, "skipping compaction") != NULL)
        skipped_compaction = true;

    fprintf(stderr, "%s\n", message);
    return (0);
}

/*
 * handle_general --
 *     General event handler.
 */
static int
handle_general(WT_EVENT_HANDLER *handler, WT_CONNECTION *conn, WT_SESSION *session,
  WT_EVENT_TYPE type, void *arg)
{
    (void)(handler);
    (void)(conn);
    (void)(session);
    (void)(arg);

    if (do_interrupt_compaction && type == WT_EVENT_COMPACT_CHECK)
        return (-1);

    return (0);
}

static WT_EVENT_HANDLER event_handler = {
  error_handler, message_handler, /* Message handlers */
  NULL,                           /* Progress handler */
  NULL,                           /* Close handler */
  handle_general                  /* General handler */
};

/*
 * populate --
 *     Populate the table.
 */
static void
populate(WT_SESSION *session, uint64_t start, uint64_t end)
{
    WT_CURSOR *cursor;
    WT_RAND_STATE rnd;

    uint64_t i, str_len, val;

    __wt_random_init_seed((WT_SESSION_IMPL *)session, &rnd);

    str_len = sizeof(data_str) / sizeof(data_str[0]);
    for (i = 0; i < str_len - 1; i++)
        data_str[i] = 'a' + (uint32_t)__wt_random(&rnd) % 26;
    data_str[str_len - 1] = '\0';

    testutil_check(session->open_cursor(session, TABLE_URI, NULL, NULL, &cursor));

    for (i = start; i < end; i++) {
        val = (uint64_t)__wt_random(&rnd);
        cursor->set_key(cursor, i + 1);
        cursor->set_value(cursor, val, data_str);
        testutil_check(cursor->insert(cursor));
    }

    testutil_check(cursor->close(cursor));
}

/*
 * remove_records --
 *     Remove a range of records.
 */
static void
remove_records(WT_SESSION *session, uint64_t start, uint64_t end)
{
    WT_CURSOR *cursor;
    uint64_t i;

    testutil_check(session->open_cursor(session, TABLE_URI, NULL, NULL, &cursor));

    for (i = start; i < end; i++) {
        cursor->set_key(cursor, i + 1);
        testutil_check(cursor->remove(cursor));
    }

    testutil_check(cursor->close(cursor));
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
    WT_CURSOR *stat;
    WT_SESSION *session;
    int64_t pages_reviewed;
    int ret;
    char home[1024];
    const char *desc, *pvalue;

    opts = &_opts;
    memset(opts, 0, sizeof(*opts));
    testutil_check(testutil_parse_opts(argc, argv, opts));

    /* Initialize the database with just a few records. */
    testutil_work_dir_from_path(home, sizeof(home), "WT_TEST.compact-quick-interrupt");
    testutil_make_work_dir(home);

    testutil_check(wiredtiger_open(home, &event_handler, conn_config, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    testutil_check(session->create(session, TABLE_URI, table_config));

    populate(session, 0, NUM_RECORDS1);
    testutil_check(session->checkpoint(session, NULL));

    /* At this point, check that compact does not have any meaningful work to do. */
    skipped_compaction = false;
    testutil_check(session->compact(session, TABLE_URI, NULL));
    testutil_assert(skipped_compaction);

    /*
     * Now populate the table a lot more, make some space, and then see if we can interrupt the
     * compaction quickly - even before the compaction can get any work done.
     */
    populate(session, NUM_RECORDS1, NUM_RECORDS1 + NUM_RECORDS2);
    testutil_check(session->checkpoint(session, NULL));
    remove_records(session, 0, NUM_RECORDS1 + NUM_RECORDS2 / 2);

    do_interrupt_compaction = true;
    interrupted_compaction = false;
    skipped_compaction = false;
    ret = session->compact(session, TABLE_URI, NULL);

    testutil_assert(ret == WT_ERROR);
    testutil_assert(interrupted_compaction);
    testutil_assert(!skipped_compaction);

    /* Check that we didn't get any work done. */
    testutil_check(session->open_cursor(session, "statistics:" TABLE_URI, NULL, NULL, &stat));
    stat->set_key(stat, WT_STAT_DSRC_BTREE_COMPACT_PAGES_REVIEWED);
    testutil_check(stat->search(stat));
    testutil_check(stat->get_value(stat, &desc, &pvalue, &pages_reviewed));
    testutil_check(stat->close(stat));
    testutil_assert(pages_reviewed == 0);

    /* Now actually compact. */
    do_interrupt_compaction = false;
    interrupted_compaction = false;
    skipped_compaction = false;
    testutil_check(session->compact(session, TABLE_URI, NULL));
    testutil_assert(!interrupted_compaction);
    testutil_assert(!skipped_compaction);

    /* Finish the test and clean up. */
    testutil_check(session->close(session, NULL));
    testutil_check(conn->close(conn, NULL));

    if (!opts->preserve)
        testutil_clean_work_dir(home);
    testutil_cleanup(opts);
    return (EXIT_SUCCESS);
}
