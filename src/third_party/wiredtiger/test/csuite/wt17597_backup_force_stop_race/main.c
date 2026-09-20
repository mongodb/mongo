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
 * Force stopping incremental backup releases the connection's backup identifiers, but the work
 * happens when the cursor is closed rather than when it is opened, and the close holds none of the
 * locks the open took. A checkpoint reads those identifiers while deciding whether a tree's block
 * modification list is still current, so a checkpoint running across a force stop can read an
 * identifier the close has already released.
 *
 * A backup thread here opens a force-stop cursor and holds it open long enough for a checkpoint to
 * start and reach the identifiers, then closes it. The backup_blkmod_delay stress point widens the
 * window inside the checkpoint so the two land on the identifier together.
 */

#define BACKUP_ID "ID1"
#define CYCLES_DEFAULT 30
#define NUM_RECORDS 1000

/* Time for which a force-stop cursor is held open, long enough for a checkpoint to get going. */
#define FORCE_STOP_HOLD_US (50 * WT_THOUSAND)

static const char conn_config[] =
  "create,cache_size=100MB,log=(enabled,file_max=10M),"
  "timing_stress_for_test=[backup_blkmod_delay]";
static const char table_config[] = "key_format=S,value_format=S,log=(enabled=true)";
static const char *const uri = "table:backup-force-stop-race";

static WT_CONNECTION *conn;

/*
 * Nothing is published through this flag, so the checkpoint thread only has to observe it
 * eventually and a relaxed access is enough. It still has to be atomic: the store races the load.
 */
static bool done;

/* Forward declarations. */
static void populate(WT_SESSION *);
static void run_test(const char *, uint64_t);
static void *thread_func_checkpoint(void *);

/*
 * populate --
 *     Insert enough data that the checkpoints which follow have work to do.
 */
static void
populate(WT_SESSION *session)
{
    WT_CURSOR *cursor;
    int i;
    char key[32], value[64];

    testutil_check(session->open_cursor(session, uri, NULL, NULL, &cursor));
    for (i = 0; i < NUM_RECORDS; ++i) {
        testutil_snprintf(key, sizeof(key), "key%06d", i);
        testutil_snprintf(value, sizeof(value), "value%06d", i);
        cursor->set_key(cursor, key);
        cursor->set_value(cursor, value);
        testutil_check(cursor->insert(cursor));
    }
    testutil_check(cursor->close(cursor));
}

/*
 * thread_func_checkpoint --
 *     Checkpoint in a loop, so that one is always in flight when a force stop completes.
 */
static void *
thread_func_checkpoint(void *arg)
{
    WT_SESSION *session;

    (void)arg;

    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    while (!__wt_atomic_load_bool_relaxed(&done))
        testutil_check(session->checkpoint(session, NULL));
    testutil_check(session->close(session, NULL));

    return (NULL);
}

/*
 * run_test --
 *     Force stop incremental backup underneath a checkpoint loop.
 */
static void
run_test(const char *home, uint64_t cycles)
{
    WT_CURSOR *cursor;
    WT_SESSION *session;
    pthread_t thread_checkpoint;
    uint64_t i;

    __wt_atomic_store_bool_relaxed(&done, false);

    testutil_recreate_dir(home);
    testutil_check(wiredtiger_open(home, NULL, conn_config, &conn));
    testutil_check(conn->open_session(conn, NULL, NULL, &session));
    testutil_check(session->create(session, uri, table_config));
    populate(session);

    testutil_check(pthread_create(&thread_checkpoint, NULL, thread_func_checkpoint, NULL));

    for (i = 0; i < cycles; ++i) {
        /* Establish the identifier a checkpoint will read. */
        testutil_check(session->open_cursor(session, "backup:", NULL,
          "incremental=(enabled=true,this_id=" BACKUP_ID ",granularity=1MB)", &cursor));
        testutil_check(cursor->close(cursor));

        /*
         * Opening the force-stop cursor only records the intent, so hold it open to let a
         * checkpoint start and reach the identifier before the close releases it.
         */
        testutil_check(
          session->open_cursor(session, "backup:", NULL, "incremental=(force_stop=true)", &cursor));
        __wt_sleep(0, FORCE_STOP_HOLD_US);
        testutil_check(cursor->close(cursor));
    }

    __wt_atomic_store_bool_relaxed(&done, true);
    (void)pthread_join(thread_checkpoint, NULL);

    testutil_check(session->close(session, NULL));
    testutil_check(conn->close(conn, NULL));
    conn = NULL;
}

/*
 * main --
 *     Methods implementation.
 */
int
main(int argc, char *argv[])
{
    TEST_OPTS *opts, _opts;
    uint64_t cycles;

    opts = &_opts;
    memset(opts, 0, sizeof(*opts));
    testutil_check(testutil_parse_opts(argc, argv, opts));

    cycles = opts->nops == 0 ? CYCLES_DEFAULT : opts->nops;

    printf("Running test with %" PRIu64 " force stop cycles ...\n", cycles);
    run_test(opts->home, cycles);

    if (!opts->preserve)
        testutil_remove(opts->home);

    testutil_cleanup(opts);
    return (EXIT_SUCCESS);
}
