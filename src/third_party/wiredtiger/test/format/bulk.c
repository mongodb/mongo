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

#include "format.h"

/*
 * bulk_begin_transaction --
 *     Begin a bulk-load transaction.
 */
static void
bulk_begin_transaction(WT_SESSION *session)
{
    uint64_t ts;

    /* Writes require snapshot isolation. */
    wt_wrap_begin_transaction(session, NULL);
    ts = __wt_atomic_addv64(&g.timestamp, 1);
    testutil_check(session->timestamp_transaction_uint(session, WT_TS_TXN_TYPE_READ, ts));
}

/*
 * bulk_commit_transaction --
 *     Commit a bulk-load transaction.
 */
static void
bulk_commit_transaction(WT_SESSION *session)
{
    uint64_t ts;

    ts = __wt_atomic_addv64(&g.timestamp, 1);
    testutil_check(session->timestamp_transaction_uint(session, WT_TS_TXN_TYPE_COMMIT, ts));
    testutil_check(session->commit_transaction(session, NULL));

    /* Update the oldest timestamp, otherwise updates are pinned in memory. */
    timestamp_once(session, false, false);
}

/*
 * bulk_rollback_transaction --
 *     Rollback a bulk-load transaction.
 */
static void
bulk_rollback_transaction(WT_SESSION *session)
{
    testutil_check(session->rollback_transaction(session, NULL));
}

/*
 * table_load --
 *     Load a single table.
 */
static void
table_load(TABLE *base, TABLE *table)
{
    SAP sap;
    WT_CONNECTION *conn;
    WT_CURSOR *base_cursor, *cursor;
    WT_DECL_RET;
    WT_ITEM key, value;
    WT_SESSION *session;
    uint32_t committed_keyno, keyno, rows_current, v;
    uint8_t bitv;
    char config[100], track_buf[128];
    bool is_bulk, report_progress;

    conn = g.wts_conn;

    memset(&sap, 0, sizeof(sap));
    wt_wrap_open_session(conn, &sap, NULL, &session);

    testutil_check(__wt_snprintf(track_buf, sizeof(track_buf), "table %u %s load", table->id,
      base == NULL ? "bulk" : "mirror"));
    trace_msg(session, "=============== %s bulk load start", table->uri);

    /* Optionally open the base mirror. */
    base_cursor = NULL;
    if (base != NULL)
        wt_wrap_open_cursor(session, base->uri, NULL, &base_cursor);

    /* No bulk load with custom collators, insertion order won't match collation order. */
    is_bulk = TV(BTREE_REVERSE) == 0;
    wt_wrap_open_cursor(session, table->uri, is_bulk ? "bulk,append" : NULL, &cursor);

    /* Set up the key/value buffers. */
    key_gen_init(&key);
    val_gen_init(&value);

    if (g.transaction_timestamps_config)
        bulk_begin_transaction(session);

    /* The final number of rows in the table can change, get a local copy of the starting value. */
    rows_current = TV(RUNS_ROWS);

    for (committed_keyno = keyno = 0; ++keyno <= rows_current;) {
        /* Build a key; build a value, or take the next value from the base mirror. */
        if (table->type == ROW)
            key_gen(table, &key, keyno);
        if (base == NULL)
            val_gen(table, &g.data_rnd, &value, &bitv, keyno);
        else {
            testutil_check(read_op(base_cursor, NEXT, NULL));
            testutil_check(base_cursor->get_value(base_cursor, &value));
            val_to_flcs(table, &value, &bitv);
        }

        /* Insert the key/value pair into the new table. */
        switch (table->type) {
        case FIX:
            if (!is_bulk)
                cursor->set_key(cursor, keyno);
            cursor->set_value(cursor, bitv);
            if (FLD_ISSET(g.trace_flags, TRACE_BULK))
                trace_msg(session, "bulk %" PRIu32 " {0x%02" PRIx8 "}", keyno, bitv);
            break;
        case VAR:
            if (!is_bulk)
                cursor->set_key(cursor, keyno);
            cursor->set_value(cursor, &value);
            if (FLD_ISSET(g.trace_flags, TRACE_BULK))
                trace_msg(
                  session, "bulk %" PRIu32 " {%.*s}", keyno, (int)value.size, (char *)value.data);
            break;
        case ROW:
            cursor->set_key(cursor, &key);
            cursor->set_value(cursor, &value);
            if (FLD_ISSET(g.trace_flags, TRACE_BULK))
                trace_msg(session, "bulk %" PRIu32 " {%.*s}, {%.*s}", keyno, (int)key.size,
                  (char *)key.data, (int)value.size, (char *)value.data);
            break;
        }

        /*
         * We don't want to size the cache to ensure the initial data set can load in the in-memory
         * case, guaranteeing the load succeeds probably means future updates are also guaranteed to
         * succeed, which isn't what we want. If we run out of space in the initial load, reset the
         * row counter and continue.
         */
        if ((ret = cursor->insert(cursor)) != 0) {
            /*
             * We cannot fail when loading mirrored table. Otherwise, we will encounter data
             * mismatch in the future.
             */
            testutil_assertfmt(base == NULL && (ret == WT_CACHE_FULL || ret == WT_ROLLBACK),
              "WT_CURSOR.insert failed: %d", ret);

            /*
             * If this occurs with predictable replay, we may need to redo the bulk load with fewer
             * keys in each batch. For now, we just don't handle it.
             */
            testutil_assert(!GV(RUNS_PREDICTABLE_REPLAY));

            if (g.transaction_timestamps_config) {
                bulk_rollback_transaction(session);
                bulk_begin_transaction(session);
            }

            /*
             * Decrease inserts since they won't be successful if we're hitting cache limits, and
             * increase the delete percentage to get some extra space once the run starts. We can't
             * simply modify the values because they have to equal 100 when the database is reopened
             * (we are going to rewrite the CONFIG file, too).
             */
            if (TV(OPS_PCT_INSERT) > 5) {
                TV(OPS_PCT_DELETE) += TV(OPS_PCT_INSERT) - 5;
                TV(OPS_PCT_INSERT) = 5;
            }
            v = TV(OPS_PCT_WRITE) / 2;
            TV(OPS_PCT_DELETE) += v;
            TV(OPS_PCT_WRITE) -= v;

            break;
        }

        /*
         * When first starting up, report the progress for every 10 keys in the first 5K keys. After
         * 5K records, report every 5K keys.
         */
        report_progress =
          (keyno < (5 * WT_THOUSAND) && keyno % 10 == 0) || keyno % (5 * WT_THOUSAND) == 0;
        /* Report on progress. */
        if (report_progress)
            track(track_buf, keyno);

        /*
         * If we are loading a mirrored table, commit after each operation to ensure that we are not
         * generating excessive cache pressure and we can successfully load the same content as the
         * base table. Otherwise, commit if we report progress.
         */
        if (g.transaction_timestamps_config && (report_progress || base != NULL)) {
            bulk_commit_transaction(session);
            committed_keyno = keyno;
            bulk_begin_transaction(session);
        }
    }

    if (g.transaction_timestamps_config)
        bulk_commit_transaction(session);

    trace_msg(session, "=============== %s bulk load stop", table->uri);
    wt_wrap_close_session(session);

    /*
     * Ideally, the insert loop runs until the number of rows plus one, in which case row counts are
     * correct. If the loop exited early, reset the table's row count and rewrite the CONFIG file
     * (so reopens aren't surprised).
     */
    if (keyno != rows_current + 1) {
        testutil_assertfmt(
          base == NULL, "table %u: unable to load matching rows into a mirrored table", table->id);

        rows_current = g.transaction_timestamps_config ? committed_keyno : (keyno - 1);
        testutil_assert(rows_current > 0);

        testutil_check(__wt_snprintf(config, sizeof(config), "runs.rows=%" PRIu32, rows_current));
        config_single(table, config, false);
        config_print(false);
    }

    /* The number of rows in the table can change during normal ops, set the starting value. */
    table->rows_current = TV(RUNS_ROWS);

    key_gen_teardown(&key);
    val_gen_teardown(&value);
}

/*
 * wts_load --
 *     Bulk load the tables.
 */
void
wts_load(void)
{
    SAP sap;
    WT_CONNECTION *conn;
    WT_SESSION *session;
    u_int i;

    conn = g.wts_conn;

    if (ntables == 0)
        table_load(NULL, tables[0]);
    else {
        /* If it's a mirrored run, load the base mirror table. */
        if (g.base_mirror != NULL)
            table_load(NULL, g.base_mirror);

        /* Load any tables not yet loaded. */
        for (i = 1; i <= ntables; ++i)
            if (tables[i] != g.base_mirror)
                table_load(tables[i]->mirror ? g.base_mirror : NULL, tables[i]);
    }

    /* Checkpoint to ensure bulk loaded records are durable. */
    if (!GV(RUNS_IN_MEMORY)) {
        memset(&sap, 0, sizeof(sap));
        wt_wrap_open_session(conn, &sap, NULL, &session);
        testutil_check(session->checkpoint(session, NULL));
        wt_wrap_close_session(session);
    }
}
