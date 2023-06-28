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

#define SNAP_LIST_SIZE 512

/*
 * snap_init --
 *     Initialize the repeatable operation tracking.
 */
void
snap_init(TINFO *tinfo)
{
    /*
     * We maintain two snap lists, where the current one is indicated by tinfo->s, and keeps the
     * most recent operations.
     *
     * The other one is used when we are running timestamp transactions with rollback_to_stable.
     * When each thread notices that the stable timestamp has changed, it stashes the current snap
     * list and starts fresh with the other snap list. After we've completed a rollback_to_stable,
     * we can the secondary snap list to see the state of keys/values seen and updated at the time
     * of the rollback.
     */
    if (g.transaction_timestamps_config) {
        tinfo->s = &tinfo->snap_states[1];
        tinfo->snap_list = dcalloc(SNAP_LIST_SIZE, sizeof(SNAP_OPS));
        tinfo->snap_end = &tinfo->snap_list[SNAP_LIST_SIZE];
    }
    tinfo->s = &tinfo->snap_states[0];
    tinfo->snap_list = dcalloc(SNAP_LIST_SIZE, sizeof(SNAP_OPS));
    tinfo->snap_end = &tinfo->snap_list[SNAP_LIST_SIZE];
    tinfo->snap_current = tinfo->snap_list;
}

/*
 * snap_teardown --
 *     Tear down the repeatable operation tracking structures.
 */
void
snap_teardown(TINFO *tinfo)
{
    SNAP_OPS *snaplist;
    u_int i, snap_index;

    for (snap_index = 0; snap_index < WT_ELEMENTS(tinfo->snap_states); snap_index++)
        if ((snaplist = tinfo->snap_states[snap_index].snap_state_list) != NULL) {
            for (i = 0; i < SNAP_LIST_SIZE; ++i) {
                __wt_buf_free(NULL, &snaplist[i].key);
                __wt_buf_free(NULL, &snaplist[i].value);
            }
            free(snaplist);
        }
}

/*
 * snap_clear_one --
 *     Clear a single snap entry.
 */
static void
snap_clear_one(SNAP_OPS *snap)
{
    snap->repeatable = false;
}

/*
 * snap_clear --
 *     Clear the snap list.
 */
static void
snap_clear(TINFO *tinfo)
{
    SNAP_OPS *snap;

    for (snap = tinfo->snap_list; snap < tinfo->snap_end; ++snap)
        snap_clear_one(snap);
}

/*
 * snap_op_init --
 *     Initialize the repeatable operation tracking for each new operation.
 */
void
snap_op_init(TINFO *tinfo, uint64_t read_ts, bool repeatable_reads)
{
    uint64_t stable_ts;

    ++tinfo->opid;

    if (g.transaction_timestamps_config) {
        /*
         * If the stable timestamp has changed and we've advanced beyond it, preserve the current
         * snapshot history up to this point, we'll use it verify rollback_to_stable. Switch our
         * tracking to the other snap list. Use a barrier to ensure a cached value doesn't cause us
         * to ignore a stable timestamp transition.
         */
        WT_BARRIER();
        stable_ts = g.stable_timestamp;
        if (stable_ts != tinfo->stable_ts && read_ts > stable_ts) {
            tinfo->stable_ts = stable_ts;
            if (tinfo->s == &tinfo->snap_states[0])
                tinfo->s = &tinfo->snap_states[1];
            else
                tinfo->s = &tinfo->snap_states[0];
            tinfo->snap_current = tinfo->snap_list;

            /* Clear out older info from the snap list. */
            snap_clear(tinfo);
        }
    }

    tinfo->snap_first = tinfo->snap_current;

    tinfo->read_ts = read_ts;
    tinfo->repeatable_reads = repeatable_reads;
    tinfo->repeatable_wrap = false;
}

/*
 * snap_track --
 *     Add a single snapshot isolation returned value to the list.
 */
void
snap_track(TINFO *tinfo, thread_op op)
{
    SNAP_OPS *snap;
    TABLE *table;
    WT_ITEM *ip;
    u_int mask;

    table = tinfo->table;

    snap = tinfo->snap_current;
    snap->op = op;
    snap->opid = tinfo->opid;
    snap->id = table->id;
    snap->keyno = tinfo->keyno;
    snap->ts = WT_TS_NONE;
    snap->repeatable = false;
    snap->last = 0;
    snap->bitv = FIX_VALUE_WRONG;
    snap->key.data = snap->value.data = NULL;
    snap->key.size = snap->value.size = 0;

    switch (op) {
    case INSERT:
        if (table->type == ROW)
            testutil_check(__wt_buf_set(NULL, &snap->key, tinfo->key->data, tinfo->key->size));
        /* FALLTHROUGH */
    case MODIFY:
    case READ:
    case UPDATE:
        if (table->type == FIX) {
            /*
             * At this point we should have a value the right size for this table, even for mirror
             * tables. If we messed up, bail now rather than waiting for a repeatable read to fail.
             */
            mask = (1u << TV(BTREE_BITCNT)) - 1;
            testutil_assert((tinfo->bitv & mask) == tinfo->bitv);
            snap->bitv = tinfo->bitv;
        } else {
            ip = op == READ ? tinfo->value : tinfo->new_value;
            testutil_check(__wt_buf_set(NULL, &snap->value, ip->data, ip->size));
        }
        break;
    case REMOVE:
        break;
    case TRUNCATE:
        snap->last = tinfo->last;
        break;
    }

    /* Move to the next slot, wrap at the end of the circular buffer. */
    if (++tinfo->snap_current >= tinfo->snap_end)
        tinfo->snap_current = tinfo->snap_list;

    /*
     * It's possible to pass this transaction's buffer starting point and start replacing our own
     * entries. If that happens, we can't repeat operations because we don't know which ones were
     * previously modified.
     */
    if (tinfo->snap_current->opid == tinfo->opid)
        tinfo->repeatable_wrap = true;
}

typedef struct {
    SNAP_OPS *snap;
    TABLE *table;
    TINFO *tinfo;
    WT_CURSOR *cursor;
    WT_ITEM *key;
    uint64_t keyno;
} SEARCH_CALLBACK;

/*
 * snap_verify_callback --
 *     Callback from inside the WiredTiger library.
 */
static void
snap_verify_callback(WT_CURSOR *cursor, int ret, void *arg)
{
    SEARCH_CALLBACK *callback;
    SNAP_OPS *snap;
    TABLE *table;
    TINFO *tinfo;
    WT_ITEM *key, *value;
    uint64_t keyno;
    uint8_t bitv;
    char ret_buf[10], snap_buf[10];

    /* We only handle success and not-found. */
    if (ret != 0 && ret != WT_NOTFOUND)
        return;

    callback = arg;

    /*
     * It's possible that our cursor operation triggered another internal cursor operation (on the
     * history store or metadata). Make sure it's the cursor we started with.
     */
    if (cursor != callback->cursor)
        return;

    snap = callback->snap;
    table = callback->table;
    tinfo = callback->tinfo;
    key = callback->key;
    keyno = callback->keyno;

    value = tinfo->value;
    bitv = FIX_VALUE_WRONG; /* -Wconditional-uninitialized */

    /* Get the value and check for matches. */
    if (ret == 0) {
        if (table->type == FIX) {
            testutil_check(cursor->get_value(cursor, &bitv));
            if (snap->op != REMOVE && bitv == snap->bitv)
                return;
        } else {
            testutil_check(cursor->get_value(cursor, value));
            if (snap->op != REMOVE && value->size == snap->value.size &&
              (value->size == 0 || memcmp(value->data, snap->value.data, snap->value.size) == 0))
                return;
        }
    }

    /* Check for missing records matching delete operations. */
    if (ret == WT_NOTFOUND && snap->op == REMOVE)
        return;

    /*
     * In fixed length stores, zero values at the end of the key space are returned as not-found,
     * and not-found row reads are saved as zero values. Map back-and-forth for simplicity.
     */
    if (table->type == FIX) {
        if (ret == WT_NOTFOUND && snap->bitv == 0)
            return;
        if (snap->op == REMOVE && bitv == 0)
            return;
    }

    /*
     * Things went pear-shaped.
     *
     * Dump the WiredTiger handle ID, it's useful in selecting trace records from the log. We have
     * an open cursor on the handle, so while this is pretty ugly, I don't think it's unsafe.
     */
    fprintf(stderr, "%s: WiredTiger trace ID: %u\n", table->uri,
      (u_int)((WT_BTREE *)((WT_CURSOR_BTREE *)cursor)->dhandle->handle)->id);
    switch (table->type) {
    case FIX:
        if (snap->op == REMOVE)
            strcpy(snap_buf, "remove");
        else
            testutil_check(__wt_snprintf(snap_buf, sizeof(snap_buf), "0x%02x", (u_int)snap->bitv));
        if (ret == WT_NOTFOUND)
            strcpy(ret_buf, "notfound");
        else
            testutil_check(__wt_snprintf(ret_buf, sizeof(ret_buf), "0x%02x", (u_int)bitv));
        fprintf(stderr, "snapshot-isolation: %" PRIu64 " search: expected {%s}, found {%s}\n",
          keyno, snap_buf, ret_buf);
        break;
    case ROW:
        fprintf(
          stderr, "snapshot-isolation %.*s search mismatch\n", (int)key->size, (char *)key->data);

        if (snap->op == REMOVE)
            fprintf(stderr, "expected {deleted}\n");
        else
            fprintf(stderr, "expected {%.*s}\n", (int)snap->value.size, (char *)snap->value.data);
        if (ret == WT_NOTFOUND)
            fprintf(stderr, "   found {deleted}\n");
        else
            fprintf(stderr, "   found {%.*s}\n", (int)value->size, (char *)value->data);
        break;
    case VAR:
        fprintf(stderr, "snapshot-isolation %" PRIu64 " search mismatch\n", keyno);

        if (snap->op == REMOVE)
            fprintf(stderr, "expected {deleted}\n");
        else
            fprintf(stderr, "expected {%.*s}\n", (int)snap->value.size, (char *)snap->value.data);
        if (ret == WT_NOTFOUND)
            fprintf(stderr, "   found {deleted}\n");
        else
            fprintf(stderr, "   found {%.*s}\n", (int)value->size, (char *)value->data);
        break;
    }
    fflush(stderr);

    /* We have a mismatch, dump WiredTiger datastore pages. */
    cursor_dump_page(cursor, "snapshot-isolation error");
    testutil_assert(0);
}

/*
 * snap_verify --
 *     Repeat a read and verify the contents.
 */
static int
snap_verify(TINFO *tinfo, SNAP_OPS *snap)
{
    SEARCH_CALLBACK callback;
    TABLE *table;
    WT_CURSOR *cursor;
    WT_DECL_RET;
    WT_ITEM *key;
    uint64_t keyno;

    testutil_assert(snap->op != TRUNCATE);

    table = tables[ntables == 0 ? 0 : snap->id];
    cursor = table_cursor(tinfo, snap->id);
    key = NULL;
    keyno = snap->keyno;

    if (FLD_ISSET(g.trace_flags, TRACE_READ)) {
        if (snap->op == REMOVE)
            trace_uri_op(
              tinfo, table->uri, "repeat %" PRIu64 " ts=%" PRIu64 " {deleted}", keyno, snap->ts);
        else if (snap->op == INSERT && table->type == ROW)
            trace_uri_op(tinfo, table->uri, "repeat {%.*s} ts=%" PRIu64 " {%.*s}",
              (int)snap->key.size, (char *)snap->key.data, snap->ts, (int)snap->value.size,
              (char *)snap->value.data);
        else if (table->type == FIX)
            trace_uri_op(tinfo, table->uri, "repeat %" PRIu64 " ts=%" PRIu64 " {0x%02" PRIx8 "}",
              keyno, snap->ts, snap->bitv);
        else
            trace_uri_op(tinfo, table->uri, "repeat %" PRIu64 " ts=%" PRIu64 " {%.*s}", keyno,
              snap->ts, (int)snap->value.size, (char *)snap->value.data);
    }

    /*
     * Retrieve the key/value pair by key. Row-store inserts have a unique generated key we saved,
     * else generate the key from the key number.
     */
    switch (table->type) {
    case FIX:
    case VAR:
        cursor->set_key(cursor, keyno);
        break;
    case ROW:
        key = tinfo->key;
        if (snap->op == INSERT) {
            key->data = snap->key.data;
            key->size = snap->key.size;
        } else
            key_gen(table, key, keyno);
        cursor->set_key(cursor, key);
        break;
    }

    /*
     * Hook into the WiredTiger library with a callback function. That allows us to dump information
     * before any failing operation releases its underlying pages.
     */
    callback.snap = snap;
    callback.table = table;
    callback.tinfo = tinfo;
    callback.cursor = cursor;
    callback.key = key;
    callback.keyno = keyno;
    CUR2S(cursor)->format_private = snap_verify_callback;
    CUR2S(cursor)->format_private_arg = &callback;
    ret = read_op(cursor, SEARCH, NULL);
    CUR2S(cursor)->format_private = NULL;

    testutil_assert(ret == 0 || ret == WT_NOTFOUND || ret == WT_ROLLBACK);
    return (ret == WT_NOTFOUND ? 0 : ret);
}

/*
 * snap_ts_clear --
 *     Clear snapshots at or before a specified timestamp.
 */
static void
snap_ts_clear(TINFO *tinfo, uint64_t ts)
{
    SNAP_OPS *snap;

    /* Check from the first slot to the last. */
    for (snap = tinfo->snap_list; snap < tinfo->snap_end; ++snap)
        if (snap->repeatable && snap->ts <= ts)
            snap->repeatable = false;
}

/*
 * snap_repeat_match --
 *     Compare two operations and return if they modified the same record.
 */
static bool
snap_repeat_match(SNAP_OPS *current, SNAP_OPS *a)
{
    TABLE *table;
    bool reverse;

    /* Reads are never a problem, there's no modification. */
    if (a->op == READ)
        return (false);

    /* Check if the operations were in the same table. */
    if (a->id != current->id)
        return (false);

    /* Check for a matching single record insert, modify, remove or update. */
    if (a->keyno == current->keyno)
        return (true);

    /* Truncates are slightly harder, make sure the ranges don't overlap. */
    table = tables[ntables == 0 ? 0 : a->id];
    reverse = TV(BTREE_REVERSE) != 0;
    if (a->op == TRUNCATE) {
        if (reverse && (a->keyno == 0 || a->keyno >= current->keyno) &&
          (a->last == 0 || a->last <= current->keyno))
            return (true);
        if (!reverse && (a->keyno == 0 || a->keyno <= current->keyno) &&
          (a->last == 0 || a->last >= current->keyno))
            return (true);
    }

    return (false);
}

/*
 * snap_repeat_ok_commit --
 *     Return if an operation in the transaction can be repeated, where the transaction isn't yet
 *     committed (so all locks are in place), or has already committed successfully.
 */
static bool
snap_repeat_ok_commit(TINFO *tinfo, SNAP_OPS *current)
{
    SNAP_OPS *p;

    /*
     * Truncates can't be repeated, we don't know the exact range of records that were removed (if
     * any).
     */
    if (current->op == TRUNCATE)
        return (false);

    /*
     * For updates, check for subsequent changes to the record and don't repeat the read. For reads,
     * check for either subsequent or previous changes to the record and don't repeat the read. (The
     * reads are repeatable, but only at the commit timestamp, and the update will do the repeatable
     * read in that case.)
     */
    for (p = current;;) {
        /* Wrap at the end of the circular buffer. */
        if (++p >= tinfo->snap_end)
            p = tinfo->snap_list;
        if (p->opid != tinfo->opid)
            break;

        if (snap_repeat_match(current, p))
            return (false);
    }

    if (current->op != READ)
        return (true);
    for (p = current;;) {
        /* Wrap at the beginning of the circular buffer. */
        if (--p < tinfo->snap_list)
            p = &tinfo->snap_list[SNAP_LIST_SIZE - 1];
        if (p->opid != tinfo->opid)
            break;

        if (snap_repeat_match(current, p))
            return (false);
    }
    return (true);
}

/*
 * snap_repeat_ok_rollback --
 *     Return if an operation in the transaction can be repeated, after a transaction has rolled
 *     back.
 */
static bool
snap_repeat_ok_rollback(TINFO *tinfo, SNAP_OPS *current)
{
    SNAP_OPS *p;

    /* Ignore update operations, they can't be repeated after rollback. */
    if (current->op != READ)
        return (false);

    /*
     * Check for previous changes to the record and don't attempt to repeat the read in that case.
     */
    for (p = current;;) {
        /* Wrap at the beginning of the circular buffer. */
        if (--p < tinfo->snap_list)
            p = &tinfo->snap_list[SNAP_LIST_SIZE - 1];
        if (p->opid != tinfo->opid)
            break;

        if (snap_repeat_match(current, p))
            return (false);
    }
    return (true);
}

/*
 * snap_repeat_txn --
 *     Repeat each operation done within a snapshot isolation transaction.
 */
int
snap_repeat_txn(TINFO *tinfo)
{
    SNAP_OPS *current;

    /* If we wrapped the buffer, we can't repeat operations. */
    if (tinfo->repeatable_wrap)
        return (0);

    /* Check from the first operation we saved to the last. */
    for (current = tinfo->snap_first;; ++current) {
        /* Wrap at the end of the circular buffer. */
        if (current >= tinfo->snap_end)
            current = tinfo->snap_list;
        if (current->opid != tinfo->opid)
            break;

        /*
         * The transaction is not yet resolved, so the rules are as if the transaction has
         * committed. Note we are NOT checking if reads are repeatable based on the chosen
         * timestamp. This is because we expect snapshot isolation to work even in the presence of
         * other threads of control committing in our past, until the transaction resolves.
         */
        if (snap_repeat_ok_commit(tinfo, current))
            WT_RET(snap_verify(tinfo, current));
    }

    return (0);
}

/*
 * snap_repeat_update --
 *     Update the list of snapshot operations based on final transaction resolution.
 */
void
snap_repeat_update(TINFO *tinfo, bool committed)
{
    SNAP_OPS *current;

    /* If we wrapped the buffer, we can't repeat operations. */
    if (tinfo->repeatable_wrap)
        return;

    /* Check from the first operation we saved to the last. */
    for (current = tinfo->snap_first;; ++current) {
        /* Wrap at the end of the circular buffer. */
        if (current >= tinfo->snap_end)
            current = tinfo->snap_list;
        if (current->opid != tinfo->opid)
            break;

        /*
         * First, reads may simply not be repeatable because the read timestamp chosen wasn't older
         * than all concurrently running uncommitted updates.
         */
        if (!tinfo->repeatable_reads && current->op == READ)
            continue;

        /*
         * Second, check based on the transaction resolution (the rules are different if the
         * transaction committed or rolled back).
         */
        current->repeatable = committed ? snap_repeat_ok_commit(tinfo, current) :
                                          snap_repeat_ok_rollback(tinfo, current);

        /*
         * Repeat reads at the transaction's read timestamp and updates at the commit timestamp.
         */
        if (current->repeatable)
            current->ts = current->op == READ ? tinfo->read_ts : tinfo->commit_ts;
    }
}

/*
 * snap_repeat --
 *     Repeat one operation.
 */
static void
snap_repeat(TINFO *tinfo, SNAP_OPS *snap)
{
    WT_DECL_RET;
    WT_SESSION *session;
#define MAX_RETRY_ON_ROLLBACK WT_THOUSAND
    u_int max_retry;

    session = tinfo->session;

    /* Start a transaction with a read-timestamp and verify the record. */
    for (max_retry = 0; max_retry < MAX_RETRY_ON_ROLLBACK; ++max_retry, __wt_yield()) {
        wt_wrap_begin_transaction(session, "isolation=snapshot");

        /* EINVAL means the timestamp has aged out of the system. */
        if ((ret = session->timestamp_transaction_uint(session, WT_TS_TXN_TYPE_READ, snap->ts)) ==
          EINVAL) {
            snap_ts_clear(tinfo, snap->ts);
            break;
        }
        testutil_check(ret);

        /*
         * The only expected error is rollback (as a read-only transaction, cache-full shouldn't
         * matter to us). Persist after rollback, as a repeatable read we should succeed, yield to
         * let eviction catch up.
         */
        if ((ret = snap_verify(tinfo, snap)) == 0)
            break;
        testutil_assertfmt(ret == WT_ROLLBACK, "operation failed: %d", ret);

        testutil_check(session->rollback_transaction(session, NULL));
    }
    testutil_assert(max_retry < MAX_RETRY_ON_ROLLBACK);

    testutil_check(session->rollback_transaction(session, NULL));
}

/*
 * snap_repeat_single --
 *     Repeat an historic operation.
 */
void
snap_repeat_single(TINFO *tinfo)
{
    SNAP_OPS *snap;
    uint64_t ts;
    u_int v;
    int count;

    /* Repeat an operation that's before any running operation. */
    ts = timestamp_maximum_committed();

    /*
     * Start at a random spot in the list of operations and look for a read to retry. Stop when
     * we've walked the entire list or found one.
     */
    v = mmrand(&tinfo->extra_rnd, 1, SNAP_LIST_SIZE) - 1;
    for (snap = &tinfo->snap_list[v], count = SNAP_LIST_SIZE; count > 0; --count, ++snap) {
        /* Wrap at the end of the circular buffer. */
        if (snap >= tinfo->snap_end)
            snap = tinfo->snap_list;

        if (snap->repeatable && snap->ts <= ts)
            break;
    }

    if (count == 0)
        return;

    snap_repeat(tinfo, snap);
}

/*
 * snap_repeat_rollback --
 *     Repeat all known operations after a rollback.
 */
void
snap_repeat_rollback(WT_SESSION *session, TINFO **tinfo_array, size_t tinfo_count)
{
    SNAP_OPS *snap;
    SNAP_STATE *state;
    TINFO *tinfo, **tinfop;
    uint32_t count;
    size_t i, statenum;
    char buf[64];

    count = 0;

    track("rollback_to_stable: checking", 0ULL);
    for (i = 0, tinfop = tinfo_array; i < tinfo_count; ++i, ++tinfop) {
        tinfo = *tinfop;
        testutil_assert(tinfo->session == NULL);
        tinfo->session = session;

        /*
         * For this thread, walk through both sets of snaps ("states"), looking for entries that are
         * repeatable and have relevant timestamps. One set will have the most current operations,
         * meaning they will likely be newer than the stable timestamp, and thus cannot be checked.
         * The other set typically has operations that are just before the stable timestamp, so are
         * candidates for checking.
         */
        for (statenum = 0; statenum < WT_ELEMENTS(tinfo->snap_states); statenum++) {
            state = &tinfo->snap_states[statenum];
            for (snap = state->snap_state_list; snap < state->snap_state_end; ++snap) {
                if (snap->repeatable && snap->ts <= g.stable_timestamp) {
                    snap_repeat(tinfo, snap);
                    ++count;
                    if (count % 100 == 0) {
                        testutil_check(__wt_snprintf(
                          buf, sizeof(buf), "rollback_to_stable: %" PRIu32 " ops repeated", count));
                        track(buf, 0ULL);
                    }
                }
                snap_clear_one(snap);
            }
        }

        tinfo->session = NULL;
    }

    /* Show the final result. */
    testutil_check(
      __wt_snprintf(buf, sizeof(buf), "rollback_to_stable: %" PRIu32 " ops repeated", count));
    track(buf, 0ULL);
}
