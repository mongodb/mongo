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
 * wts_prepare_discover --
 *     Discover and process prepared transactions.
 */
void
wts_prepare_discover(WT_CONNECTION *conn)
{
    /*
     * Since RTS is not ran with precise checkpoint, we need to use prepare discover cursor to claim
     * all pending prepared transactions. When precise checkpoint is not configure, there's no need
     * to run prepare discover.
     */
    if (!GV(PRECISE_CHECKPOINT) || !GV(OPS_PREPARE))
        return;

    WT_CURSOR *cursor;
    WT_DECL_RET;
    WT_SESSION *session;
    uint64_t prepared_id, ts;
    uint32_t discover_count, rand_val;
    char buf[128];
    bool should_commit;
    /*
     * Individual object verification. Do a full checkpoint to reduce the possibility of returning
     * EBUSY from the following verify calls.
     */
    testutil_check(conn->open_session(conn, NULL, NULL, &session));

    /* Open the prepare discover cursor */
    ret = session->open_cursor(session, "prepared_discover:", NULL, NULL, &cursor);
    if (ret == WT_NOTFOUND) {
        /* No prepared transactions found - this is normal */
        testutil_check(session->close(session, NULL));
        return;
    }
    testutil_check(ret);

    /*
     * If the cursor was opened successfully, there should be at least one prepared transaction to
     * discover.
     */
    trace_msg(session, "Starting prepare discover operation %s", "");

    /* Iterate through all prepared transactions and claim pending prepared transactions. */
    discover_count = 0;
    while ((ret = cursor->next(cursor)) == 0) {
        discover_count++;
        testutil_check(cursor->get_key(cursor, &prepared_id));

        trace_msg(session, "Discovered prepared transaction with ID: %" PRIu64, prepared_id);

        /* Claim the prepared transaction */
        testutil_snprintf(buf, sizeof(buf), "claim_prepared_id=%" PRIx64, prepared_id);
        testutil_check(session->begin_transaction(session, buf));

        /* Randomly decide whether to commit or roll back */
        rand_val = mmrand(&g.extra_rnd, 0, 9);
        should_commit = (rand_val < 5); /* 50% chance to commit */

        if (should_commit) {
            /*
             * Commit with a timestamp greater than the prepare timestamp. We use the current
             * timestamp + 10 to ensure it's newer
             */
            ts = __wt_atomic_addv64(&g.timestamp, 10);
            testutil_check(session->timestamp_transaction_uint(session, WT_TS_TXN_TYPE_COMMIT, ts));
            testutil_check(
              session->timestamp_transaction_uint(session, WT_TS_TXN_TYPE_DURABLE, ts));

            testutil_check(session->commit_transaction(session, NULL));
            trace_msg(session, "Claimed and committed prepared id %" PRIu64, prepared_id);
        } else {
            /* Roll back the transaction */
            testutil_check(session->rollback_transaction(session, NULL));
            trace_msg(
              session, "Claimed and rolled back prepared transaction %" PRIu64, prepared_id);
        }
    }
    /* WT_NOTFOUND is expected when we reach the end of the cursor */
    testutil_assert(ret == WT_NOTFOUND);

    /* Report what we found and did */
    if (discover_count > 0) {
        trace_msg(session, "Prepare discover: found and claimed %" PRIu32 " prepared transactions",
          discover_count);
    }
    testutil_check(cursor->close(cursor));

    const char *checkpoint_name = "WiredTigerCheckpoint";
    session->checkpoint(session, NULL);
    wts_verify_mirrors(g.wts_conn, checkpoint_name, NULL);
    testutil_check(session->close(session, NULL));
}
