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
 * snap_init --
 *     Initialize the repeatable operation tracking.
 */
void
snap_init(TINFO *tinfo, uint64_t read_ts, bool repeatable_reads)
{
    ++tinfo->opid;

    tinfo->snap_first = tinfo->snap;

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
    WT_ITEM *ip;
    SNAP_OPS *snap;

    snap = tinfo->snap;
    snap->op = op;
    snap->opid = tinfo->opid;
    snap->keyno = tinfo->keyno;
    snap->ts = WT_TS_NONE;
    snap->repeatable = false;
    snap->last = op == TRUNCATE ? tinfo->last : 0;
    snap->ksize = snap->vsize = 0;

    if (op == INSERT && g.type == ROW) {
        ip = tinfo->key;
        if (snap->kmemsize < ip->size) {
            snap->kdata = drealloc(snap->kdata, ip->size);
            snap->kmemsize = ip->size;
        }
        memcpy(snap->kdata, ip->data, snap->ksize = ip->size);
    }

    if (op != REMOVE && op != TRUNCATE) {
        ip = tinfo->value;
        if (snap->vmemsize < ip->size) {
            snap->vdata = drealloc(snap->vdata, ip->size);
            snap->vmemsize = ip->size;
        }
        memcpy(snap->vdata, ip->data, snap->vsize = ip->size);
    }

    /* Move to the next slot, wrap at the end of the circular buffer. */
    if (++tinfo->snap >= &tinfo->snap_list[WT_ELEMENTS(tinfo->snap_list)])
        tinfo->snap = tinfo->snap_list;

    /*
     * It's possible to pass this transaction's buffer starting point and start replacing our own
     * entries. If that happens, we can't repeat operations because we don't know which ones were
     * previously modified.
     */
    if (tinfo->snap->opid == tinfo->opid)
        tinfo->repeatable_wrap = true;
}

/*
 * print_item_data --
 *     Display a single data/size pair, with a tag.
 */
static void
print_item_data(const char *tag, const uint8_t *data, size_t size)
{
    static const char hex[] = "0123456789abcdef";
    u_char ch;

    fprintf(stderr, "%s {", tag);
    if (g.type == FIX)
        fprintf(stderr, "0x%02x", data[0]);
    else
        for (; size > 0; --size, ++data) {
            ch = data[0];
            if (__wt_isprint(ch))
                fprintf(stderr, "%c", (int)ch);
            else
                fprintf(
                  stderr, "%x%x", (u_int)hex[(data[0] & 0xf0) >> 4], (u_int)hex[data[0] & 0x0f]);
        }
    fprintf(stderr, "}\n");
}

/*
 * snap_verify --
 *     Repeat a read and verify the contents.
 */
static int
snap_verify(WT_CURSOR *cursor, TINFO *tinfo, SNAP_OPS *snap)
{
    WT_DECL_RET;
    WT_ITEM *key, *value;
    uint64_t keyno;
    uint8_t bitfield;

    testutil_assert(snap->op != TRUNCATE);

    key = tinfo->key;
    value = tinfo->value;
    keyno = snap->keyno;

    /*
     * Retrieve the key/value pair by key. Row-store inserts have a unique generated key we saved,
     * else generate the key from the key number.
     */
    if (snap->op == INSERT && g.type == ROW) {
        key->data = snap->kdata;
        key->size = snap->ksize;
        cursor->set_key(cursor, key);
    } else {
        switch (g.type) {
        case FIX:
        case VAR:
            cursor->set_key(cursor, keyno);
            break;
        case ROW:
            key_gen(key, keyno);
            cursor->set_key(cursor, key);
            break;
        }
    }

    switch (ret = read_op(cursor, SEARCH, NULL)) {
    case 0:
        if (g.type == FIX) {
            testutil_check(cursor->get_value(cursor, &bitfield));
            *(uint8_t *)(value->data) = bitfield;
            value->size = 1;
        } else
            testutil_check(cursor->get_value(cursor, value));
        break;
    case WT_NOTFOUND:
        break;
    default:
        return (ret);
    }

    /* Check for simple matches. */
    if (ret == 0 && snap->op != REMOVE && value->size == snap->vsize &&
      memcmp(value->data, snap->vdata, value->size) == 0)
        return (0);
    if (ret == WT_NOTFOUND && snap->op == REMOVE)
        return (0);

    /*
     * In fixed length stores, zero values at the end of the key space are returned as not-found,
     * and not-found row reads are saved as zero values. Map back-and-forth for simplicity.
     */
    if (g.type == FIX) {
        if (ret == WT_NOTFOUND && snap->vsize == 1 && *(uint8_t *)snap->vdata == 0)
            return (0);
        if (snap->op == REMOVE && value->size == 1 && *(uint8_t *)value->data == 0)
            return (0);
    }

/* Things went pear-shaped. */
#ifdef HAVE_DIAGNOSTIC
    fprintf(stderr, "snapshot-isolation error: Dumping page to %s\n", g.home_pagedump);
    testutil_check(__wt_debug_cursor_page(cursor, g.home_pagedump));
#endif
    switch (g.type) {
    case FIX:
        testutil_die(ret, "snapshot-isolation: %" PRIu64
                          " search: "
                          "expected {0x%02x}, found {0x%02x}",
          keyno, snap->op == REMOVE ? 0 : *(uint8_t *)snap->vdata,
          ret == WT_NOTFOUND ? 0 : *(uint8_t *)value->data);
    /* NOTREACHED */
    case ROW:
        fprintf(
          stderr, "snapshot-isolation %.*s search mismatch\n", (int)key->size, (char *)key->data);

        if (snap->op == REMOVE)
            fprintf(stderr, "expected {deleted}\n");
        else
            print_item_data("expected", snap->vdata, snap->vsize);
        if (ret == WT_NOTFOUND)
            fprintf(stderr, "   found {deleted}\n");
        else
            print_item_data("   found", value->data, value->size);

        testutil_die(
          ret, "snapshot-isolation: %.*s search mismatch", (int)key->size, (char *)key->data);
    /* NOTREACHED */
    case VAR:
        fprintf(stderr, "snapshot-isolation %" PRIu64 " search mismatch\n", keyno);

        if (snap->op == REMOVE)
            fprintf(stderr, "expected {deleted}\n");
        else
            print_item_data("expected", snap->vdata, snap->vsize);
        if (ret == WT_NOTFOUND)
            fprintf(stderr, "   found {deleted}\n");
        else
            print_item_data("   found", value->data, value->size);

        testutil_die(ret, "snapshot-isolation: %" PRIu64 " search mismatch", keyno);
        /* NOTREACHED */
    }

    /* NOTREACHED */
    return (1);
}

/*
 * snap_ts_clear --
 *     Clear snapshots at or before a specified timestamp.
 */
static void
snap_ts_clear(TINFO *tinfo, uint64_t ts)
{
    SNAP_OPS *snap;
    int count;

    /* Check from the first slot to the last. */
    for (snap = tinfo->snap_list, count = WT_ELEMENTS(tinfo->snap_list); count > 0; --count, ++snap)
        if (snap->repeatable && snap->ts <= ts)
            snap->repeatable = false;
}

/*
 * snap_repeat_ok_match --
 *     Compare two operations and see if they modified the same record.
 */
static bool
snap_repeat_ok_match(SNAP_OPS *current, SNAP_OPS *a)
{
    /* Reads are never a problem, there's no modification. */
    if (a->op == READ)
        return (true);

    /* Check for a matching single record modification. */
    if (a->keyno == current->keyno)
        return (false);

    /* Truncates are slightly harder, make sure the ranges don't overlap. */
    if (a->op == TRUNCATE) {
        if (g.c_reverse && (a->keyno == 0 || a->keyno >= current->keyno) &&
          (a->last == 0 || a->last <= current->keyno))
            return (false);
        if (!g.c_reverse && (a->keyno == 0 || a->keyno <= current->keyno) &&
          (a->last == 0 || a->last >= current->keyno))
            return (false);
    }

    return (true);
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
        if (++p >= &tinfo->snap_list[WT_ELEMENTS(tinfo->snap_list)])
            p = tinfo->snap_list;
        if (p->opid != tinfo->opid)
            break;

        if (!snap_repeat_ok_match(current, p))
            return (false);
    }

    if (current->op != READ)
        return (true);
    for (p = current;;) {
        /* Wrap at the beginning of the circular buffer. */
        if (--p < tinfo->snap_list)
            p = &tinfo->snap_list[WT_ELEMENTS(tinfo->snap_list) - 1];
        if (p->opid != tinfo->opid)
            break;

        if (!snap_repeat_ok_match(current, p))
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
            p = &tinfo->snap_list[WT_ELEMENTS(tinfo->snap_list) - 1];
        if (p->opid != tinfo->opid)
            break;

        if (!snap_repeat_ok_match(current, p))
            return (false);
    }
    return (true);
}

/*
 * snap_repeat_txn --
 *     Repeat each operation done within a snapshot isolation transaction.
 */
int
snap_repeat_txn(WT_CURSOR *cursor, TINFO *tinfo)
{
    SNAP_OPS *current;

    /* If we wrapped the buffer, we can't repeat operations. */
    if (tinfo->repeatable_wrap)
        return (0);

    /* Check from the first operation we saved to the last. */
    for (current = tinfo->snap_first;; ++current) {
        /* Wrap at the end of the circular buffer. */
        if (current >= &tinfo->snap_list[WT_ELEMENTS(tinfo->snap_list)])
            current = tinfo->snap_list;
        if (current->opid != tinfo->opid)
            break;

        if (snap_repeat_ok_commit(tinfo, current))
            WT_RET(snap_verify(cursor, tinfo, current));
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
        if (current >= &tinfo->snap_list[WT_ELEMENTS(tinfo->snap_list)])
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
 * snap_repeat_single --
 *     Repeat an historic operation.
 */
void
snap_repeat_single(WT_CURSOR *cursor, TINFO *tinfo)
{
    SNAP_OPS *snap;
    WT_DECL_RET;
    WT_SESSION *session;
    int count;
    u_int v;
    char buf[64];

    session = cursor->session;

    /*
     * Start at a random spot in the list of operations and look for a read to retry. Stop when
     * we've walked the entire list or found one.
     */
    v = mmrand(&tinfo->rnd, 1, WT_ELEMENTS(tinfo->snap_list)) - 1;
    for (snap = &tinfo->snap_list[v], count = WT_ELEMENTS(tinfo->snap_list); count > 0;
         --count, ++snap) {
        /* Wrap at the end of the circular buffer. */
        if (snap >= &tinfo->snap_list[WT_ELEMENTS(tinfo->snap_list)])
            snap = tinfo->snap_list;

        if (snap->repeatable)
            break;
    }

    if (count == 0)
        return;

    /*
     * Start a new transaction. Set the read timestamp. Verify the record. Discard the transaction.
     */
    while ((ret = session->begin_transaction(session, "isolation=snapshot")) == WT_CACHE_FULL)
        __wt_yield();
    testutil_check(ret);

    /*
     * If the timestamp has aged out of the system, we'll get EINVAL when we try and set it.
     */
    testutil_check(__wt_snprintf(buf, sizeof(buf), "read_timestamp=%" PRIx64, snap->ts));

    ret = session->timestamp_transaction(session, buf);
    if (ret == 0) {
        logop(session, "%-10s%" PRIu64 " ts=%" PRIu64 " {%.*s}", "repeat", snap->keyno, snap->ts,
          (int)snap->vsize, (char *)snap->vdata);

        /* The only expected error is rollback. */
        ret = snap_verify(cursor, tinfo, snap);

        if (ret != 0 && ret != WT_ROLLBACK)
            testutil_check(ret);
    } else if (ret == EINVAL)
        snap_ts_clear(tinfo, snap->ts);
    else
        testutil_check(ret);

    /* Discard the transaction. */
    testutil_check(session->rollback_transaction(session, NULL));
}
