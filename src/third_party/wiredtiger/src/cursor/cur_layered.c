/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"
#include "cur_layered_private.h"

static int __clayered_copy_bounds(WTI_CURSOR_LAYERED *);
static int __clayered_update_ingest(WTI_CURSOR_LAYERED *, uint32_t);
static int __clayered_update_stable(WTI_CURSOR_LAYERED *, uint32_t, WTI_CLAYERED_ROLE);
static int __clayered_lookup(WTI_CLAYERED_OP *, WT_ITEM *);
static int __clayered_open_ingest(WT_SESSION_IMPL *, WTI_CURSOR_LAYERED *, WT_CURSOR **);
static int __clayered_reset_cursors(WTI_CURSOR_LAYERED *, bool);
static int __clayered_search_near(WT_CURSOR *, int *);
static void __clayered_update_state(WTI_CURSOR_LAYERED *, WTI_CLAYERED_ROLE);

/* Operations passed to __clayered_put. */
typedef enum {
    WTI_CLAYERED_PUT_INSERT,
    WTI_CLAYERED_PUT_UPDATE,
    WTI_CLAYERED_PUT_RESERVE,
} WTI_CLAYERED_PUT_OP;

/*
 * Increment the ingest or stable variant of a read statistic according to which constituent cursor
 * holds the result. Call only on the success path of a read, once the operation has positioned the
 * cursor; the assert enforces that.
 */
#define WT_STAT_CLAYERED_READ_CONSTITUENT_INCR(session, clayered, fld)                   \
    do {                                                                                 \
        if ((clayered)->current_cursor == (clayered)->ingest_cursor)                     \
            WT_STAT_CONN_DSRC_INCR(session, fld##_ingest);                               \
        else {                                                                           \
            WT_ASSERT(session, (clayered)->current_cursor == (clayered)->stable_cursor); \
            WT_STAT_CONN_DSRC_INCR(session, fld##_stable);                               \
        }                                                                                \
    } while (0)

/*
 * Tombstone value encoding.
 *
 * The ingest table holds recent changes; the full key space lives in the stable table, so a read
 * that misses in the ingest table falls through to the stable table (the key may have been evicted,
 * not deleted). A user delete therefore needs an explicit marker in the ingest table to stop that
 * fall-through: the reserved value __wt_tombstone, the two bytes {\x14\x14}.
 *
 * An application value equal to {\x14\x14} would then read back as a delete, so it is escaped by
 * appending one tombstone byte; decode strips one trailing byte from a stored value beginning with
 * the tombstone bytes. Because decode keys off that prefix alone, every value beginning with the
 * tombstone must be escaped, not only the marker itself: a raw {\x14\x14\x37} would otherwise
 * decode to {\x14\x14}, so it is stored as {\x14\x14\x37\x14}.
 *
 * The size test differs by direction: encode is inclusive (a value equal to the tombstone is in the
 * namespace), decode and classification are exclusive (encoding only ever appends, so a stored
 * escaped value is always longer than the tombstone).
 *
 * The stable table has no tombstone marker (deletes there are real removes) and need not escape,
 * but the same encoding is applied to both constituents so a value decodes identically whichever
 * served it. FIXME-WT-17933: that persists the escape byte on disk and locks it into the on-disk
 * format; limit encoding to the ingest table only.
 */

/*
 * __clayered_value_in_tombstone_namespace --
 *     Boundary test shared by the tombstone encode and decode paths.
 */
static WT_INLINE bool
__clayered_value_in_tombstone_namespace(const WT_ITEM *value, bool encode)
{
    size_t bound = encode ? __wt_tombstone.size : __wt_tombstone.size + 1;

    return (
      value->size >= bound && memcmp(value->data, __wt_tombstone.data, __wt_tombstone.size) == 0);
}

/*
 * __clayered_deleted_encode --
 *     Encode values that are in the encoded name space.
 */
static WT_INLINE int
__clayered_deleted_encode(
  WT_SESSION_IMPL *session, const WT_ITEM *value, WT_ITEM *final_value, WT_ITEM **tmpp)
{
    WT_ITEM *tmp;

    /*
     * If value requires encoding, get a scratch buffer of the right size and create a copy of the
     * data with the first byte of the tombstone appended.
     */
    if (__clayered_value_in_tombstone_namespace(value, true /* encode */)) {
        WT_RET(__wt_scr_alloc(session, value->size + 1, tmpp));
        tmp = *tmpp;

        memcpy(tmp->mem, value->data, value->size);
        memcpy((uint8_t *)tmp->mem + value->size, __wt_tombstone.data, 1);
        final_value->data = tmp->mem;
        final_value->size = value->size + 1;
    } else {
        final_value->data = value->data;
        final_value->size = value->size;
    }

    return (0);
}

/*
 * __clayered_deleted_decode --
 *     Decode values that start with the tombstone.
 */
static WT_INLINE void
__clayered_deleted_decode(WT_SESSION_IMPL *session, WT_ITEM *value)
{
    if (__clayered_value_in_tombstone_namespace(value, false /* decode */)) {
        /* Encoding only ever appends the tombstone byte, so that is the byte being stripped. */
        WT_ASSERT_ALWAYS(session,
          ((const uint8_t *)value->data)[value->size - 1] == *(const uint8_t *)__wt_tombstone.data,
          "layered tombstone decode found a non-tombstone trailing byte");
        --value->size;
    }
}

/*
 * __wt_clayered_stable_value_stat --
 *     Count and warn about a stable-table value that shares the tombstone's encoded namespace. Such
 *     values begin with the two tombstone bytes and are escaped like on the ingest table (see
 *     __clayered_deleted_encode); they are expected to be extremely rare. The stored form is
 *     classified by its length and trailing byte; a bare two-byte tombstone can only come from
 *     legacy unescaped data on disk. The raw bytes may carry application data, so the log records
 *     only the size and a content hash to fingerprint recurring values. This takes raw bytes so
 *     both the layered cursor and the verify page walk can share it.
 *
 * TODO(WT-17958): Revert WT-17957 when tombstone encoding is removed from the stable table.
 */
void
__wt_clayered_stable_value_stat(WT_SESSION_IMPL *session, const void *data, size_t size)
{
    uint8_t tombstone_byte;
    const uint8_t *bytes;
    const char *what;

    /* The value must begin with the whole tombstone to share its namespace. */
    if (size < __wt_tombstone.size || memcmp(data, __wt_tombstone.data, __wt_tombstone.size) != 0)
        return;

    bytes = (const uint8_t *)data;
    tombstone_byte = ((const uint8_t *)__wt_tombstone.data)[0];

    if (size == __wt_tombstone.size) {
        WT_STAT_CONN_DSRC_INCR(session, layered_curs_stable_value_tombstone);
        what = "equal to the tombstone";
    } else if (size == __wt_tombstone.size + 1 && bytes[size - 1] == tombstone_byte) {
        WT_STAT_CONN_DSRC_INCR(session, layered_curs_stable_value_tombstone_x3);
        what = "three tombstone bytes";
    } else if (bytes[size - 1] == tombstone_byte) {
        WT_STAT_CONN_DSRC_INCR(session, layered_curs_stable_value_tombstone_suffix);
        what = "ending with a tombstone byte";
    } else {
        WT_STAT_CONN_DSRC_INCR(session, layered_curs_stable_value_tombstone_prefix);
        what = "ending with a non-tombstone byte";
    }

    __wt_verbose_warning(session, WT_VERB_LAYERED,
      "stable table value in the tombstone namespace (%s), size 0x%" PRIx64
      ", content hash 0x%016" PRIx64,
      what, (uint64_t)size, __wt_hash_city64(data, size));
}

/*
 * __clayered_stable_read_value_stat --
 *     Account a value just read from the stable constituent; a no-op when the layered cursor is
 *     positioned on the ingest table.
 */
static WT_INLINE void
__clayered_stable_read_value_stat(WTI_CURSOR_LAYERED *clayered, const WT_ITEM *value)
{
    if (clayered->current_cursor == clayered->stable_cursor)
        __wt_clayered_stable_value_stat(CUR2S(clayered), value->data, value->size);
}

/*
 * __clayered_get_collator --
 *     Retrieve the collator for a layered cursor. Wrapped in a function, since in the future the
 *     collator might live in a constituent cursor instead of the handle.
 */
static void
__clayered_get_collator(WTI_CURSOR_LAYERED *clayered, WT_COLLATOR **collatorp)
{
    *collatorp = ((WT_LAYERED_TABLE *)clayered->dhandle)->collator;
}

/*
 * __clayered_cursor_compare --
 *     Compare two constituent cursors in a layered tree
 */
static int
__clayered_cursor_compare(WTI_CLAYERED_OP *op, WT_CURSOR *c1, WT_CURSOR *c2, int *cmpp)
{
    WTI_CURSOR_LAYERED *clayered = op->clayered;
    WT_SESSION_IMPL *session = CUR2S(clayered);

    WT_ASSERT_ALWAYS(session, F_ISSET(c1, WT_CURSTD_KEY_SET) && F_ISSET(c2, WT_CURSTD_KEY_SET),
      "Can only compare cursors with keys available in layered tree");

    return (__wt_compare(session, op->collator, &c1->key, &c2->key, cmpp));
}

/*
 * __clayered_assert_stable_mode --
 *     Assert that the stable cursor's btree access mode matches the current node role.
 */
static WT_INLINE void
__clayered_assert_stable_mode(WTI_CURSOR_LAYERED *clayered)
{
    if (clayered->stable_cursor == NULL)
        return;

    /* The stable cursor's btree must be read-write for a leader and read-only for a follower. */
    WT_ASSERT(CUR2S(clayered),
      (clayered->last_role == WTI_CLAYERED_ROLE_LEADER) !=
        F_ISSET(CUR2BT(clayered->stable_cursor), WT_BTREE_READONLY));
}

/* __clayered_enter() local flags. */
#define CLAYERED_ENTER_SKIP_STABLE 0x1u /* Follower writing without reading stable. */
#define CLAYERED_ENTER_ITERATION 0x2u   /* Cursor is performing iteration. */
#define CLAYERED_ENTER_RESET 0x4u       /* Reset constituent cursors if needed. */
#define CLAYERED_ENTER_ROLE_CHANGE 0x8u /* Leader/follower role changed since last access. */

/*
 * __clayered_enter_flags --
 *     Derive the enter-time control flags from the operation mode and resolved role.
 */
static WT_INLINE uint32_t
__clayered_enter_flags(
  WTI_CURSOR_LAYERED *clayered, WTI_CLAYERED_OP_MODE mode, WTI_CLAYERED_ROLE role)
{
    WT_SESSION_IMPL *session = CUR2S(clayered);
    uint32_t flags = 0;

    if (mode == WTI_CLAYERED_MODE_SEARCH)
        LF_SET(CLAYERED_ENTER_RESET);
    if (mode == WTI_CLAYERED_MODE_ITERATE || mode == WTI_CLAYERED_MODE_RANDOM)
        LF_SET(CLAYERED_ENTER_ITERATION);

    /*
     * Reads (search, search_near, iterate, random, scan) and non-overwrite writes always need the
     * stable cursor; an overwrite write needs it on the leader, or on a follower with a read
     * timestamp where the write-conflict check must consult the stable table.
     */
    if ((mode == WTI_CLAYERED_MODE_WRITE_OVERWRITE) && (role == WTI_CLAYERED_ROLE_FOLLOWER) &&
      !F_ISSET(session->txn, WT_TXN_SHARED_TS_READ))
        LF_SET(CLAYERED_ENTER_SKIP_STABLE);

    if (role != clayered->last_role)
        LF_SET(CLAYERED_ENTER_ROLE_CHANGE);

    return (flags);
}

/*
 * __clayered_op_init --
 *     Populate the per-operation state.
 */
static WT_INLINE void
__clayered_op_init(
  WTI_CURSOR_LAYERED *clayered, WTI_CLAYERED_OP *op, WTI_CLAYERED_ROLE role, uint32_t flags)
{
    WT_LAYERED_TABLE *table = (WT_LAYERED_TABLE *)clayered->dhandle;

    op->clayered = clayered;
    op->ingest = (role == WTI_CLAYERED_ROLE_FOLLOWER) ? clayered->ingest_cursor : NULL;
    /* NULL the stable slot when skipped: the persistent cursor may still be open from before. */
    op->stable = LF_ISSET(CLAYERED_ENTER_SKIP_STABLE) ? NULL : clayered->stable_cursor;
    op->truncate_list = &table->truncate_list;
    op->collator = table->collator;
}

/*
 * __clayered_enter --
 *     Start an operation on a layered cursor.
 */
static WT_INLINE int
__clayered_enter(WTI_CURSOR_LAYERED *clayered, WTI_CLAYERED_OP_MODE mode, WTI_CLAYERED_OP *op)
{
    WT_SESSION_IMPL *const session = CUR2S(clayered);
    WT_CONNECTION_IMPL *conn = S2C(session);
    WTI_CLAYERED_ROLE role =
      conn->layered_table_manager.leader ? WTI_CLAYERED_ROLE_LEADER : WTI_CLAYERED_ROLE_FOLLOWER;
    uint32_t flags = __clayered_enter_flags(clayered, mode, role);

    if (FLD_ISSET(flags, CLAYERED_ENTER_ROLE_CHANGE)) {
        WT_ASSERT_ALWAYS(session, !F_ISSET(&clayered->iface, WT_CURSTD_KEY_INT),
          "All the cursors should be left unpositioned before changing the role.");
    }

    /*
     * FIXME-WT-15058: When inside a read committed isolation, the file cursor code expects to
     * release the snapshot when the count of active cursors is zero. Reset the constituent cursors
     * to adhere to that behavior. Ideally we should not be changing the active cursors counter
     * outside of the file cursor code.
     */
    if (LF_ISSET(CLAYERED_ENTER_RESET) &&
      __wt_txn_read_committed_should_release_snapshot(session)) {
        WT_ASSERT(session, !F_ISSET(&clayered->iface, WT_CURSTD_KEY_INT | WT_CURSTD_VALUE_INT));
        WT_RET(__clayered_reset_cursors(clayered, false));
    }

    /* Manage the ingest cursor: a follower opens it on first use; the leader keeps it closed. */
    WT_RET(__clayered_update_ingest(clayered, flags));

    /* Manage the stable: open it, advance to a newer checkpoint, or reopen on role change. */
    WT_RET(__clayered_update_stable(clayered, flags, role));

    __clayered_update_state(clayered, role);
    __clayered_assert_stable_mode(clayered);

    __clayered_op_init(clayered, op, role, flags);

    if (!F_ISSET(clayered, WTI_CLAYERED_ACTIVE)) {
        /*
         * Opening this layered cursor has opened a number of btree cursors, ensure other code
         * doesn't think this is the first cursor in a session.
         */
        ++session->ncursors;
        WT_RET(__cursor_enter(session));
        F_SET(clayered, WTI_CLAYERED_ACTIVE);
    }

    return (0);
}

/*
 * __clayered_leave --
 *     Finish an operation on a layered cursor.
 */
static void
__clayered_leave(WTI_CURSOR_LAYERED *clayered)
{
    WT_SESSION_IMPL *session;

    session = CUR2S(clayered);

    if (F_ISSET(clayered, WTI_CLAYERED_ACTIVE)) {
        --session->ncursors;
        __cursor_leave(session);
        F_CLR(clayered, WTI_CLAYERED_ACTIVE);
    }
}

/*
 * __clayered_close_cursors --
 *     Close any btree cursors that are not needed.
 */
static int
__clayered_close_cursors(WTI_CURSOR_LAYERED *clayered)
{
    WT_CURSOR *c;

    /*
     * Note: There is no need to close the constituent cursors if it has been already done during
     * connection->close performing a close of all cursors in the session.
     *
     * FIXME-WT-17360: Consider removing this flag
     */
    if (F_ISSET(&clayered->iface, WT_CURSTD_CONSTITUENT_DEAD))
        return (0);

    clayered->current_cursor = NULL;
    if ((c = clayered->ingest_cursor) != NULL) {
        WT_RET(c->close(c));
        clayered->ingest_cursor = NULL;
    }
    if ((c = clayered->stable_cursor) != NULL) {
        WT_RET(c->close(c));
        clayered->stable_cursor = NULL;
        clayered->stable_checkpoint_meta_lsn = WT_DISAGG_LSN_NONE;
    }

    /* Some flags persist across closes of constituents. */
    F_CLR(clayered, ~(WTI_CLAYERED_ACTIVE | WTI_CLAYERED_RANDOM));
    return (0);
}

/*
 * __clayered_seed_random --
 *     Seed the constituent cursor's random state. The constituent itself is opened without random
 *     config because it overwrites the normal next method. The next method is required for
 *     cursor::search_near to work. Instead initialize the cbt->rnd and directly use the file
 *     cursor::next_random function.
 *
 * FIXME-WT-17343: There is an ugly cursor layering violation here. We directly use file cursors
 *     methods and initialize random state in the cursor structure.
 */
static void
__clayered_seed_random(
  WT_SESSION_IMPL *session, WTI_CURSOR_LAYERED *clayered, WT_CURSOR *constituent)
{
    WT_CURSOR_BTREE *cbt = (WT_CURSOR_BTREE *)constituent;

    if (clayered->next_random_seed != 0)
        __wt_random_init_seed(&cbt->rnd, clayered->next_random_seed);
    else
        __wt_random_init(session, &cbt->rnd);

    cbt->next_random_sample_size = clayered->next_random_sample_size;
}

/*
 * __clayered_open_stable_int --
 *     Open the stable cursor for the given URI.
 */
static int
__clayered_open_stable_int(WTI_CURSOR_LAYERED *clayered, const char *stable_uri)
{
    WT_SESSION_IMPL *session = CUR2S(clayered);
    const char *cfg[3] = {WT_CONFIG_BASE(CUR2S(clayered), WT_SESSION_open_cursor), NULL, NULL};

    /*
     * Forward the size summary to the active btree cursor. The file-cursor open path then sets the
     * flag, resets that btree's counters and enforces row-store. This open path also runs on every
     * follower checkpoint advance, so the request survives constituent reopens; that re-reset is
     * safe only when no size_stats walk is in progress (same non-overlap contract as the file
     * cursor). Leaders do not reopen the active btree mid-scan.
     */
    if (F_ISSET(clayered, WTI_CLAYERED_SIZE_STAT))
        cfg[1] = "debug=(size_stats=true)";

    WT_RET(__wt_open_cursor(session, stable_uri, &clayered->iface, cfg, &clayered->stable_cursor));
    if (F_ISSET((WT_CURSOR *)clayered, WT_CURSTD_OVERWRITE))
        F_SET(clayered->stable_cursor, WT_CURSTD_OVERWRITE);
    else
        F_CLR(clayered->stable_cursor, WT_CURSTD_OVERWRITE);
    F_SET(clayered->stable_cursor, WT_CURSTD_RAW);

    if (F_ISSET(&clayered->iface, WT_CURSTD_DEBUG_RESET_EVICT))
        F_SET(clayered->stable_cursor, WT_CURSTD_DEBUG_RESET_EVICT);

    if (F_ISSET(clayered, WTI_CLAYERED_RANDOM))
        __clayered_seed_random(session, clayered, clayered->stable_cursor);

    return (0);
}

/*
 * __clayered_open_stable_follower --
 *     Open the stable table cursor on the newest available checkpoint. In some cases it's fine to
 *     not have a checkpoint (e.g. when we open it for the first time) - leave the cursor
 *     uninitialized.
 */
static int
__clayered_open_stable_follower(WTI_CURSOR_LAYERED *clayered, bool checkpoint_expected)
{
    WT_DECL_ITEM(last_ckpt_uri);
    WT_DECL_RET;
    WT_LAYERED_TABLE *layered = (WT_LAYERED_TABLE *)clayered->dhandle;
    WT_SESSION_IMPL *session = CUR2S(clayered);
    const char *checkpoint_name = NULL;
    const char *stable_uri = layered->stable_uri;

    WT_RET(__wt_scr_alloc(session, 0, &last_ckpt_uri));

retry:
    /* Follower always opens a btree on the last checkpoint. */
    ret = __wt_meta_checkpoint_last_name(session, stable_uri, &checkpoint_name, NULL, NULL);
    if (!checkpoint_expected && ret == WT_NOTFOUND) {
        ret = 0;
        goto err;
    }
    WT_ERR(ret);

    /* Use a URI with a "/<checkpoint name> suffix. */
    WT_ERR(__wt_buf_fmt(session, last_ckpt_uri, "%s/%s", stable_uri, checkpoint_name));

    /*
     * This file is #included by a Catch2 unit test compiled as C++, where a void* does not
     * implicitly convert to char*; the explicit cast keeps that build working.
     */
    ret = __clayered_open_stable_int(clayered, (const char *)last_ckpt_uri->data);
    if (ret == EBUSY) {
        /* Retry to ensure we open the same checkpoint for the HS and the stable table. */
        __wt_free(session, checkpoint_name);
        goto retry;
    }

    WT_ERR(ret);

err:
    __wt_scr_free(session, &last_ckpt_uri);
    __wt_free(session, checkpoint_name);
    return (ret);
}

/*
 * __clayered_open_stable --
 *     Open the stable cursor for the current role.
 */
static int
__clayered_open_stable(
  WTI_CURSOR_LAYERED *clayered, bool checkpoint_expected, WTI_CLAYERED_ROLE role)
{
    WT_LAYERED_TABLE *layered = (WT_LAYERED_TABLE *)clayered->dhandle;

    return (role == WTI_CLAYERED_ROLE_LEADER ?
        __clayered_open_stable_int(clayered, layered->stable_uri) :
        __clayered_open_stable_follower(clayered, checkpoint_expected));
}

/*
 * __clayered_ingest_prepare_stalled --
 *     Determine if the ingest cursor is on a prepare conflict.
 */
static WT_INLINE bool
__clayered_ingest_prepare_stalled(const WT_CURSOR *current, const WT_CURSOR *ingest)
{
    return (ingest != NULL && current == ingest && !F_ISSET(ingest, WT_CURSTD_KEY_INT) &&
      ((WT_CURSOR_BTREE *)ingest)->ref != NULL);
}

/*
 * __clayered_can_advance_stable --
 *     Return true if the stable cursor can be advanced to a newer checkpoint at this time.
 */
static bool
__clayered_can_advance_stable(WTI_CURSOR_LAYERED *clayered, uint64_t conn_lsn, bool iteration)
{
    WT_SESSION_IMPL *session;
    WT_TXN_SHARED *txn_shared;

    session = CUR2S(clayered);

    /* A leader does not require advancing a stable table. */
    if (S2C(session)->layered_table_manager.leader)
        return (false);

    /* No need to advance if there is no newer checkpoint. */
    if (clayered->stable_checkpoint_meta_lsn == conn_lsn)
        return (false);

    /*
     * Do not advance while ingest is stalled on a prepare conflict with no key. Stable could lose
     * its position with no ingest key to recover it, skipping visible keys.
     */
    if (__clayered_ingest_prepare_stalled(clayered->current_cursor, clayered->ingest_cursor))
        return (false);

    /*
     * First, layered cursors are sometimes paired with read timestamps. When using read
     timestamps,
     * it's always safe to update cursors, even during iterations. That's because the view at a
     * timestamp is always consistent, the history store covers that.
     */
    txn_shared = WT_SESSION_TXN_SHARED(session);
    if (txn_shared != NULL && txn_shared->read_timestamp != WT_TS_NONE)
        return (true);
    else {
        /*
         * Layered cursor is positioned on the stable cursor. Changing it may lose the layered
         * cursor position.
         */
        if (F_ISSET(&clayered->iface, WT_CURSTD_KEY_INT) &&
          clayered->current_cursor == clayered->stable_cursor)
            return (false);

        /* if this is an iteration, we won't reopen the cursor, we're done. */
        if (iteration)
            return (false);

        /*
         * There are other points when it is appropriate to update cursors. If we don't currently
         * have a transactional snapshot, or if the snapshot has changed, we can update.
         *
         * Why shouldn't we update when in a transaction? We may have read some values, and we'd
         * expect to see the same values if we read them again. Reading from a newer checkpoint
         can
         * violate that.
         */
        if (!F_ISSET(session->txn, WT_TXN_HAS_SNAPSHOT) ||
          (__wt_session_gen(session, WT_GEN_HAS_SNAPSHOT) != clayered->snapshot_gen))
            return (true);
    }

    return (false);
}

/*
 * __clayered_reopen_stable --
 *     For the follower, advance the stable cursor to a newer checkpoint. Or reopen the stable table
 *     in the right format on a role change.
 */
static int
__clayered_reopen_stable(
  WT_SESSION_IMPL *session, WTI_CURSOR_LAYERED *clayered, WTI_CLAYERED_ROLE role)
{
    WT_CURSOR *old_stable;
    WT_DECL_RET;

    /*
     * We can't just close the stable cursor here, as we need to retain any position that the
     * current stable cursor has. It's easier to keep the old cursor open briefly while we copy the
     * position.
     */
    old_stable = clayered->stable_cursor;
    clayered->stable_cursor = NULL;

    WT_ERR(__clayered_open_stable(clayered, true, role));

    /*
     * If the old cursor has a position, copy it to the newly opened cursor. Prepared updates are
     * always ignored on the stable cursor, making it safe to check the WT_CURSTD_KEY_INT flag.
     */
    if (F_ISSET(old_stable, WT_CURSTD_KEY_INT)) {
        WT_ERR_NOTFOUND_OK(__wt_cursor_dup_position(old_stable, clayered->stable_cursor), true);
        /*
         * If the key is removed from the new checkpoint, the layered cursor must be positioned on
         * the ingest table.
         */
        WT_ASSERT_ALWAYS(session,
          ret == 0 || !F_ISSET(&clayered->iface, WT_CURSTD_KEY_INT) ||
            clayered->current_cursor == clayered->ingest_cursor,
          "upgrading a positioned stable cursor");
        /*
         * If the key is removed in the new checkpoint, clear the iteration flag to reposition it to
         * the correct location.
         */
        if (ret == WT_NOTFOUND)
            F_CLR(clayered, WTI_CLAYERED_ITERATE_NEXT | WTI_CLAYERED_ITERATE_PREV);
    } else if (F_ISSET(old_stable, WT_CURSTD_KEY_EXT)) {
        WT_ITEM_SET(clayered->stable_cursor->key, old_stable->key);
        if (F_ISSET(old_stable, WT_CURSTD_VALUE_EXT))
            WT_ITEM_SET(clayered->stable_cursor->value, old_stable->value);
    }

    /* Add any bounds for the new cursor. */
    WT_ERR(__clayered_copy_bounds(clayered));

    if (clayered->current_cursor == old_stable) {
        WT_CURSOR *cursor = (WT_CURSOR *)clayered;
        WT_CURSOR *new_stable = clayered->stable_cursor;
        if (F_ISSET(cursor, WT_CURSTD_KEY_INT)) {
            /* Reset the cursor key to point to the new stable cursor. */
            WT_ITEM_SET(cursor->key, new_stable->key);
            /* Clear the value as the new stable cursor may point to a different one. */
            F_CLR(cursor, WT_CURSTD_VALUE_INT);
        }
        clayered->current_cursor = new_stable;
    }

err:
    if (ret == 0) {
        /* Close the old cursor. */
        WT_TRET(old_stable->close(old_stable));
        WT_STAT_CONN_DSRC_INCR(session, layered_curs_reopen_stable);
    } else {
        /* Give up the advancement if we fail. */
        if (clayered->stable_cursor != NULL)
            WT_TRET(clayered->stable_cursor->close(clayered->stable_cursor));
        clayered->stable_cursor = old_stable;
    }

    return (ret);
}

/*
 * __clayered_update_state --
 *     Validate and update cursor state and refresh the transaction context.
 */
static void
__clayered_update_state(WTI_CURSOR_LAYERED *clayered, WTI_CLAYERED_ROLE role)
{
    WT_SESSION_IMPL *const session = CUR2S(clayered);
    const WT_TXN_SHARED *const txn_shared = WT_SESSION_TXN_SHARED(session);

    const uint64_t snapshot_gen = __wt_session_gen(session, WT_GEN_HAS_SNAPSHOT);
    const uint64_t read_timestamp = txn_shared != NULL ? txn_shared->read_timestamp : WT_TS_NONE;

    /*
     * If the transaction context has changed since the last call (different read timestamp or a new
     * snapshot), the parked alternate cursor's cached position may be stale. Clear the iteration
     * flags to force a re-search under the new context.
     *
     * FIXME-WT-17960: a context change while ingest is stalled on a prepare conflict leaves stable
     * parked under the old context with no anchor to re-search it from.
     */
    if (clayered->snapshot_gen != snapshot_gen || clayered->read_timestamp != read_timestamp) {
        WT_ASSERT(session,
          !__clayered_ingest_prepare_stalled(clayered->current_cursor, clayered->ingest_cursor));
        F_CLR(clayered, WTI_CLAYERED_ITERATE_NEXT | WTI_CLAYERED_ITERATE_PREV);
    }

    clayered->snapshot_gen = snapshot_gen;
    clayered->read_timestamp = read_timestamp;
    clayered->last_role = role;
}

/*
 * __clayered_open_ingest --
 *     Open an ingest cursor.
 */
static int
__clayered_open_ingest(WT_SESSION_IMPL *session, WTI_CURSOR_LAYERED *clayered, WT_CURSOR **cursorp)
{
    WT_CURSOR *c, *cursor;
    WT_LAYERED_TABLE *layered;
    const char *ckpt_cfg[2] = {WT_CONFIG_BASE(session, WT_SESSION_open_cursor), NULL};

    c = &clayered->iface;
    layered = (WT_LAYERED_TABLE *)clayered->dhandle;

    WT_RET(__wt_open_cursor(session, layered->ingest_uri, c, ckpt_cfg, &cursor));

    /*
     * We always open ingest in overwrite mode to be able to rewrite tombstones with insert().
     *
     * Inheriting the top cursor's OVERWRITE flag instead does not help: duplicate detection needs
     * both constituents but the ingest cursor sees only one, the ingest write must create or
     * replace (over an absent key or a tombstone) so it needs overwrite regardless, and a delete is
     * a value rather than a btree tombstone so native non-overwrite semantics give the wrong
     * answer. The stable-side pre-lookup therefore cannot be removed.
     */
    F_SET(cursor, WT_CURSTD_OVERWRITE | WT_CURSTD_RAW);

    if (F_ISSET(c, WT_CURSTD_DEBUG_RESET_EVICT))
        F_SET(cursor, WT_CURSTD_DEBUG_RESET_EVICT);

    if (F_ISSET(clayered, WTI_CLAYERED_RANDOM))
        __clayered_seed_random(session, clayered, cursor);

    *cursorp = cursor;

    return (0);
}

/*
 * __clayered_update_ingest --
 *     Manage the ingest cursor lifecycle by node role. A follower opens it on first use and never
 *     reopens it during normal operation. The leader keeps it closed: the ingest table is empty for
 *     reads and unused for writes, so an open ingest cursor only adds the per-operation
 *     cache/reopen and dhandle rwlock overhead. A step-up can leave behind an ingest cursor opened
 *     while a follower, so close it on the role change.
 */
static int
__clayered_update_ingest(WTI_CURSOR_LAYERED *clayered, uint32_t flags)
{
    WT_SESSION_IMPL *const session = CUR2S(clayered);

    if (S2C(session)->layered_table_manager.leader) {
        if (FLD_ISSET(flags, CLAYERED_ENTER_ROLE_CHANGE) && clayered->ingest_cursor != NULL) {
            WT_CURSOR *ingest = clayered->ingest_cursor;
            if (clayered->current_cursor == ingest)
                clayered->current_cursor = NULL;
            WT_RET(ingest->close(ingest));
            clayered->ingest_cursor = NULL;
        }
    } else if (clayered->ingest_cursor == NULL) {
        WT_RET(__clayered_open_ingest(session, clayered, &clayered->ingest_cursor));
        WT_RET(__clayered_copy_bounds(clayered));
    }

    return (0);
}

/*
 * __clayered_update_stable --
 *     Manage the stable cursor lifecycle: open it for the first time, advance it to a newer
 *     checkpoint, or reopen it in a different mode after a role change.
 */
static int
__clayered_update_stable(WTI_CURSOR_LAYERED *clayered, uint32_t flags, WTI_CLAYERED_ROLE role)
{
    WT_SESSION_IMPL *const session = CUR2S(clayered);
    WT_CONNECTION_IMPL *const conn = S2C(session);

    const uint64_t conn_lsn = role != WTI_CLAYERED_ROLE_LEADER ?
      __wt_atomic_load_uint64_acquire(&conn->disaggregated_storage.last_checkpoint_meta_lsn) :
      WT_DISAGG_LSN_NONE;

    if (clayered->stable_cursor == NULL) {
        /* Open stable the first time if needed. */
        bool follower_open_stable =
          (!FLD_ISSET(flags, CLAYERED_ENTER_SKIP_STABLE) && conn_lsn != WT_DISAGG_LSN_NONE);
        if (role == WTI_CLAYERED_ROLE_LEADER || follower_open_stable) {
            F_CLR(clayered, WTI_CLAYERED_ITERATE_NEXT | WTI_CLAYERED_ITERATE_PREV);
            WT_RET(__clayered_open_stable(clayered, false, role));
            WT_RET(__clayered_copy_bounds(clayered));
            clayered->stable_checkpoint_meta_lsn = conn_lsn;
            WT_STAT_CONN_DSRC_INCR(session, layered_curs_open_stable);
        }
    } else if (FLD_ISSET(flags, CLAYERED_ENTER_ROLE_CHANGE) ||
      __clayered_can_advance_stable(
        clayered, conn_lsn, FLD_ISSET(flags, CLAYERED_ENTER_ITERATION))) {
        /*
         * Reopen the cursor.
         *
         * We should always reopen the stable table when stepping up or down. That should be fine,
         * since in this case all the cursors should be unpositioned and no current transactions
         * should be running.
         *
         * The second case of reopening the stable table is when we want to open a new checkpoint on
         * a follower to evict more entries from the ingest table.
         *
         * FIXME-WT-14545: What is not checked here is the possibility that a step down and step up
         * have both occurred since the last check. We don't have a way to detect that (or its
         * opposite) at the moment. If we did, we'd want to issue a rollback if the stable cursor
         * has any changes.
         */
        WT_RET(__clayered_reopen_stable(session, clayered, role));
        clayered->stable_checkpoint_meta_lsn = conn_lsn;
    }

    return (0);
}

/*
 * __clayered_get_current --
 *     Find the smallest / largest of the cursors and copy its key/value.
 */
static int
__clayered_get_current(WTI_CLAYERED_OP *op, bool smallest)
{
    WTI_CURSOR_LAYERED *clayered = op->clayered;
    WT_SESSION_IMPL *session = CUR2S(clayered);
    WT_CURSOR *current;
    int cmp;
    bool ingest_positioned, stable_positioned;

    current = NULL;
    ingest_positioned = stable_positioned = false;

    /*
     * There are a couple of cases to deal with here: Some cursors don't have both ingest and stable
     * cursors. Some cursor positioning operations will only have one positioned cursor (e.g a walk
     * has exhausted one cursor but not the other).
     */
    if (op->ingest != NULL && F_ISSET(op->ingest, WT_CURSTD_KEY_INT))
        ingest_positioned = true;

    if (op->stable != NULL && F_ISSET(op->stable, WT_CURSTD_KEY_INT))
        stable_positioned = true;

    if (!ingest_positioned && !stable_positioned) {
        clayered->current_cursor = NULL;
        return (WT_NOTFOUND);
    }

    if (ingest_positioned && stable_positioned) {
        WT_RET(__wt_compare(session, op->collator, &op->ingest->key, &op->stable->key, &cmp));
        if (smallest ? cmp < 0 : cmp > 0)
            current = op->ingest;
        else if (cmp == 0)
            current = op->ingest;
        else
            current = op->stable;
    } else if (ingest_positioned)
        current = op->ingest;
    else if (stable_positioned)
        current = op->stable;

    clayered->current_cursor = current;

    return (0);
}

/*
 * __clayered_compare --
 *     WT_CURSOR->compare implementation for the layered cursor type.
 */
static int
__clayered_compare(WT_CURSOR *a, WT_CURSOR *b, int *cmpp)
{
    WT_COLLATOR *collator;
    WTI_CURSOR_LAYERED *clayered;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    /* There's no need to sync with the layered tree, avoid layered enter. */
    clayered = (WTI_CURSOR_LAYERED *)a;
    CURSOR_API_CALL(a, session, ret, compare, clayered->dhandle);

    /*
     * Confirm both cursors refer to the same source and have keys, then compare the keys.
     */
    if (strcmp(a->internal_uri, b->internal_uri) != 0)
        WT_ERR_MSG(session, EINVAL, "comparison method cursors must reference the same object");

    /* Both cursors are from the same tree - they share the same collator */
    __clayered_get_collator(clayered, &collator);

    WT_ERR(__wt_compare(session, collator, &a->key, &b->key, cmpp));

err:
    API_END_RET(session, ret);
}

/*
 * __clayered_reposition_truncate_iterate --
 *     Detect if the stable cursor position has been truncated. If so, position the cursor to
 *     next/prev visible position.
 */
static int
__clayered_reposition_truncate_iterate(WTI_CLAYERED_OP *op, WT_CURSOR *stable, bool forward)
{
    WTI_CURSOR_LAYERED *clayered = op->clayered;
    WT_SESSION_IMPL *session = CUR2S(clayered);
    WT_DECL_RET;
    int cmp;

    WT_ITEM start_key, stop_key;
    WT_CLEAR(start_key);
    WT_CLEAR(stop_key);

    /*
     * There could be overlapping truncates in the layered table truncate list. So we need to loop
     * until we find a non-truncated key or reach the end of the range.
     */
    for (;;) {
        WT_ERR_NOTFOUND_OK(__wt_truncate_delete_visible_check(session, op->truncate_list,
                             op->collator, &stable->key, &start_key, &stop_key),
          true);

        if (ret == WT_NOTFOUND) {
            ret = 0;
            break;
        }

        stable->set_key(stable, forward ? &stop_key : &start_key);
        WT_ERR(stable->search_near(stable, &cmp));

        /*
         * Advance until the stable cursor is strictly past the truncated boundary. The boundary
         * keys themselves are inside the range (inclusive), when cmp equals to 0 we are still on a
         * deleted key, step one position further.
         */
        while (forward ? cmp <= 0 : cmp >= 0) {
            /*
             * The cursor next()/prev() could return back WT_NOTFOUND, meaning we have reached the
             * end of the table.
             */
            WT_ERR(forward ? stable->next(stable) : stable->prev(stable));

            WT_ERR(__wt_compare(
              session, op->collator, &stable->key, forward ? &stop_key : &start_key, &cmp));
        }
    }

err:
    __wt_buf_free(session, &start_key);
    __wt_buf_free(session, &stop_key);
    return (ret);
}

/*
 * __clayered_truncate_leader --
 *     Discard a cursor range from the stable table.
 */
static int
__clayered_truncate_leader(WT_TRUNCATE_INFO *trunc_info)
{
    /*
     * On leader mode, the stable cursors will always be positioned on the table. So we can directly
     * reference them here.
     */
    WT_DECL_RET;

    WTI_CURSOR_LAYERED *clayered_start = (WTI_CURSOR_LAYERED *)trunc_info->start;
    WTI_CURSOR_LAYERED *clayered_stop = (WTI_CURSOR_LAYERED *)trunc_info->stop;

    trunc_info->start = clayered_start->stable_cursor;
    trunc_info->stop = clayered_stop->stable_cursor;

    WT_WITH_BTREE(
      trunc_info->session, CUR2BT(trunc_info->start), ret = __wt_btcur_range_truncate(trunc_info));

    return (ret);
}

/*
 * __clayered_position_near_key --
 *     Position a cursor on the given key, or at the nearest key in the requested direction if the
 *     key itself isn't present. Returns WT_NOTFOUND if nothing in that direction exists.
 */
static int
__clayered_position_near_key(WT_CURSOR *cursor, WT_ITEM *key, bool forward)
{
    __wt_cursor_set_raw_key(cursor, key);

    int cmp;
    WT_RET(cursor->search_near(cursor, &cmp));

    /* Check if we are on the wrong side of the key. */
    if (forward && cmp < 0)
        return (cursor->next(cursor));
    else if (!forward && cmp > 0)
        return (cursor->prev(cursor));

    return (0);
}

/*
 * __clayered_range_truncate_ingest --
 *     Apply a layered tombstone to each key in a cursor range on the ingest btree.
 */
static int
__clayered_range_truncate_ingest(
  WT_SESSION_IMPL *session, WT_LAYERED_TABLE *layered, WT_CURSOR *start, WT_CURSOR *stop)
{
    WT_DECL_RET;
    WT_CURSOR *cursor = start;
    int cmp;

    /* Early return if stop key is strictly less than start key, nothing to truncate. */
    WT_RET(start->compare(start, stop, &cmp));
    if (cmp > 0)
        return (0);

    do {
        /* Check the current position relative to the truncate end. */
        WT_RET(cursor->compare(cursor, stop, &cmp));

        /* Avoid stacking consecutive tombstones on the update chain. */
        if (!__wt_clayered_deleted(&cursor->value)) {
            WT_ITEM key;
            WT_RET(__wt_cursor_get_raw_key(cursor, &key));
            WT_RET(__wt_layered_table_truncate_detect_write_conflict(
              session, &layered->truncate_list, layered->collator, &key));
            cursor->set_value(cursor, &__wt_tombstone);
            WT_RET(cursor->update(cursor));
        }

        ret = cursor->next(cursor);
    } while (cmp < 0 && ret == 0);

    WT_RET_NOTFOUND_OK(ret);
    return (0);
}

/*
 * __clayered_truncate_follower --
 *     Discard a cursor range from the ingest table.
 */
static int
__clayered_truncate_follower(WT_TRUNCATE_INFO *trunc_info)
{
    /*
     * Set the keys on the ingest cursors. The ingest cursor may not have its key set if the layered
     * cursor was positioned via next/prev, or if search_near on an empty ingest table reset the
     * cursor position.
     */
    WT_ITEM start_key, stop_key;
    WT_RET(__wt_cursor_get_raw_key(trunc_info->start, &start_key));
    WT_RET(__wt_cursor_get_raw_key(trunc_info->stop, &stop_key));

    /* Position the ingest cursors. */
    WTI_CURSOR_LAYERED *clayered_start = (WTI_CURSOR_LAYERED *)trunc_info->start;
    WTI_CURSOR_LAYERED *clayered_stop = (WTI_CURSOR_LAYERED *)trunc_info->stop;
    WT_CURSOR *ingest_start = clayered_start->ingest_cursor;
    WT_CURSOR *ingest_stop = clayered_stop->ingest_cursor;

    const int ret_start = __clayered_position_near_key(ingest_start, &start_key, true);
    WT_RET_NOTFOUND_OK(ret_start);

    const int ret_stop = __clayered_position_near_key(ingest_stop, &stop_key, false);
    WT_RET_NOTFOUND_OK(ret_stop);

    WT_LAYERED_TABLE *layered_table = (WT_LAYERED_TABLE *)clayered_start->dhandle;

    WT_RET(__wt_layered_table_truncate_detect_non_ingest_write_conflict(trunc_info->session,
      &layered_table->truncate_list, layered_table->collator, &start_key, &stop_key));

    /*
     * If either positioning returned WT_NOTFOUND, the ingest table has no keys in the range and
     * there is nothing to remove from ingest. Still add the truncate-list entry so stable rows in
     * the range are hidden.
     */
    if (ret_start == 0 && ret_stop == 0)
        WT_RET(__clayered_range_truncate_ingest(
          trunc_info->session, layered_table, ingest_start, ingest_stop));

    WT_RET(__wt_insert_truncate_entry(trunc_info->session, layered_table, &start_key, &stop_key));

    return (0);
}

/*
 * __clayered_stable_replay_remove_int --
 *     Per-key delete function for ingest truncate replay. Allocates a pre-stamped tombstone and
 *     inserts it directly via __wt_row_modify, bypassing the session transaction entirely.
 */
static int
__clayered_stable_replay_remove_int(WT_CURSOR_BTREE *cbt, const WT_ITEM *value, u_int modify_type)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    WT_UPDATE *upd;

    WT_UNUSED(value);
    WT_UNUSED(modify_type);

    session = CUR2S(cbt);
    WT_RET(__wt_upd_alloc(session, NULL, WT_UPDATE_TOMBSTONE, &upd, NULL));
    upd->txnid = session->replay_trunc_ctx.txn_id;
    upd->upd_start_ts = session->replay_trunc_ctx.commit_ts;
    upd->upd_durable_ts = session->replay_trunc_ctx.durable_ts;
    F_SET(upd, WT_UPDATE_RESTORED_FROM_INGEST);

    ret = __wt_row_modify(cbt, &cbt->iface.key, NULL, &upd, WT_UPDATE_INVALID, false, false);
    if (ret != 0)
        __wt_free(session, upd);
    return (ret);
}

/*
 * __wt_clayered_range_truncate_stable_replay --
 *     Range truncate dispatch for ingest replay on the stable table.
 */
int
__wt_clayered_range_truncate_stable_replay(WT_TRUNCATE_INFO *trunc_info)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    session = trunc_info->session;

    /* Only valid on stable tables during step up to leader, routed via WT_SESSION_INGEST_REPLAY. */
    WT_ASSERT(session, F_ISSET(session, WT_SESSION_INGEST_REPLAY));
    WT_ASSERT(session, WT_URI_IS_STABLE(trunc_info->start->internal_uri));
    WT_ASSERT(session, S2C(session)->layered_table_manager.leader);

    /* Both boundary cursors must be fully positioned. */
    WT_ASSERT(session, F_ISSET(trunc_info->start, WT_CURSTD_KEY_INT));
    WT_RET(__wt_cursor_localkey(trunc_info->start));
    WT_ASSERT(session, trunc_info->stop != NULL);
    WT_ASSERT(session, F_ISSET(trunc_info->stop, WT_CURSTD_KEY_INT));
    WT_RET(__wt_cursor_localkey(trunc_info->stop));

    WT_WITH_BTREE(session, CUR2BT(trunc_info->start),
      ret = __wt_cursor_truncate((WT_CURSOR_BTREE *)trunc_info->start,
        (WT_CURSOR_BTREE *)trunc_info->stop, __clayered_stable_replay_remove_int));
    return (ret);
}

/*
 * __wt_layered_truncate --
 *     Discard a cursor range from the layered table.
 */
int
__wt_layered_truncate(WT_TRUNCATE_INFO *trunc_info)
{
    WT_SESSION_IMPL *session = trunc_info->session;

    /* These should have been initialized upstream. */
    WT_ASSERT(session, trunc_info->start != NULL);
    WT_ASSERT(session, trunc_info->stop != NULL);

    /*
     * On leader mode, we can directly perform truncate operation on the stable table. On follower
     * mode, we need to perform truncate on the ingest table and add an entry inside the truncate
     * list.
     */
    if (S2C(session)->layered_table_manager.leader)
        WT_RET(__clayered_truncate_leader(trunc_info));
    else {
        WT_ASSERT(session,
          !FLD_ISSET(S2C(session)->debug.flags, WT_CONN_DEBUG_DISAGG_SLOW_TRUNCATE_FOLLOWER));
        WT_RET(__clayered_truncate_follower(trunc_info));
    }

    return (0);
}

/*
 * __clayered_position_alternate --
 *     Position an alternate cursor to the right position according to the current one.
 */
static int
__clayered_position_alternate(
  WTI_CLAYERED_OP *op, WT_CURSOR *current, WT_CURSOR *alternate, bool forward)
{
    WTI_CURSOR_LAYERED *clayered = op->clayered;
    WT_SESSION_IMPL *session = CUR2S(clayered);
    int cmp;

    WT_ASSERT(session, F_ISSET(current, WT_CURSTD_KEY_SET));
    alternate->set_key(alternate, &current->key);
    WT_RET(alternate->search_near(alternate, &cmp));

    while (forward ? cmp < 0 : cmp > 0) {
        WT_RET(forward ? alternate->next(alternate) : alternate->prev(alternate));

        /*
         * With higher isolation levels, the cursor is now positioned as expected; break out to
         * check for committed truncate ranges before returning.
         *
         * With read-uncommitted isolation, a new record could have appeared in between the search
         * and stepping forward / back. In that case, keep going until we see a key in the expected
         * range.
         */
        if (session->txn->isolation != WT_ISO_READ_UNCOMMITTED)
            break;

        WT_RET(__clayered_cursor_compare(op, alternate, current, &cmp));
    }

    /*
     * If the alternate cursor points to stable cursor, advance past the keys that fall inside a
     * committed truncate range.
     */
    if (alternate == op->stable && F_ISSET(alternate, WT_CURSTD_KEY_INT))
        WT_RET(__clayered_reposition_truncate_iterate(op, alternate, forward));

    return (0);
}

/*
 * __clayered_constituent_iter --
 *     Move the cursor forward or backward.
 */
static int
__clayered_constituent_iter(WT_CURSOR *constituent, bool forward)
{
    return (forward ? constituent->next(constituent) : constituent->prev(constituent));
}

/*
 * __clayered_constituent_iter_helper --
 *     Advance a constituent cursor forward or backward, skipping truncated ranges on the stable
 *     cursor.
 */
static int
__clayered_constituent_iter_helper(WTI_CLAYERED_OP *op, WT_CURSOR *constituent, bool forward)
{
    WT_DECL_RET;

    WT_ERR_NOTFOUND_OK(__clayered_constituent_iter(constituent, forward), true);
    if (constituent == op->stable && ret == 0)
        WT_ERR_NOTFOUND_OK(__clayered_reposition_truncate_iterate(op, constituent, forward), true);

err:
    return (ret);
}

/*
 * __clayered_any_constituent_positioned --
 *     Return whether either constituent is positioned. The stable cursor is positioned when it
 *     carries an internal key; the ingest cursor keeps a page reference through a prepared conflict
 *     that clears its key, so its reference is the reliable signal. Both constituents must exist.
 */
static WT_INLINE bool
__clayered_any_constituent_positioned(WTI_CLAYERED_OP *op)
{
    WT_ASSERT(CUR2S(op->clayered), op->ingest != NULL && op->stable != NULL);
    return (F_ISSET(op->stable, WT_CURSTD_KEY_INT) || ((WT_CURSOR_BTREE *)op->ingest)->ref != NULL);
}

/*
 * __clayered_ingest_prepare_blocked --
 *     Return whether the ingest cursor is mid-walk but blocked by a prepared conflict: it holds a
 *     page reference yet its key was cleared. Only the ingest cursor ever sees prepared conflicts.
 */
static WT_INLINE bool
__clayered_ingest_prepare_blocked(WTI_CLAYERED_OP *op, WT_CURSOR *c_current)
{
    return (c_current == op->ingest && !F_ISSET(c_current, WT_CURSTD_KEY_INT) &&
      ((WT_CURSOR_BTREE *)c_current)->ref != NULL);
}

/*
 * __clayered_position_both --
 *     Start of a walk: position both constituents on the first (or last) key.
 */
static WT_INLINE int
__clayered_position_both(WTI_CLAYERED_OP *op, bool forward)
{
    WT_RET_NOTFOUND_OK(__clayered_constituent_iter_helper(op, op->ingest, forward));
    WT_RET_NOTFOUND_OK(__clayered_constituent_iter_helper(op, op->stable, forward));

    return (0);
}

/*
 * __clayered_select_current --
 *     Pick the constituent that leads iteration and derive its alternate.
 */
static WT_INLINE void
__clayered_select_current(WTI_CLAYERED_OP *op, WT_CURSOR **currentp, WT_CURSOR **alternatep)
{
    WTI_CURSOR_LAYERED *clayered = op->clayered;
    WT_SESSION_IMPL *session = CUR2S(clayered);
    WT_CURSOR *c_current;

    /*
     * With no current cursor the walk is blocked by a prepared conflict on the ingest cursor: pick
     * ingest, which still holds a page reference even though its key was cleared. Otherwise the
     * current cursor is expected to carry an internal key, unless it is the ingest cursor working
     * through a prepared conflict.
     */
    c_current = clayered->current_cursor != NULL ? clayered->current_cursor : op->ingest;
    WT_ASSERT(session, c_current == op->stable || c_current == op->ingest);
    WT_ASSERT(session,
      F_ISSET(c_current, WT_CURSTD_KEY_INT) || __clayered_ingest_prepare_blocked(op, c_current));

    *currentp = c_current;
    *alternatep = (c_current == op->stable) ? op->ingest : op->stable;
}

/*
 * __clayered_advance_positioned --
 *     Advance the current constituent, and the alternate when it shares the current key, for a walk
 *     that is already positioned.
 */
static int
__clayered_advance_positioned(WTI_CLAYERED_OP *op, uint32_t iter_flag, bool forward)
{
    WTI_CURSOR_LAYERED *clayered = op->clayered;
    WT_CURSOR *c_alternate, *c_current;
    bool current_moved;

    current_moved = false;

    /* This path handles an already-positioned walk: at least one constituent must be positioned. */
    WT_ASSERT(CUR2S(clayered), __clayered_any_constituent_positioned(op));

    __clayered_select_current(op, &c_current, &c_alternate);

    /*
     * If the current cursor was blocked by a prepared conflict on a previous call, drive it again:
     * this rechecks the key it is currently blocked on rather than stepping past it. Once the
     * conflict resolves, the current cursor has a key, which is needed to position the alternate.
     */
    if (__clayered_ingest_prepare_blocked(op, c_current)) {
        /*
         * The alternate (stable) cursor is deliberately not repositioned here. A set iteration flag
         * guarantees the read context is unchanged since the last positioned step - a new snapshot
         * or read timestamp clears the flag in __clayered_update_state - so the stable cursor still
         * holds its correct position, and prepared updates never move it. A context change would
         * clear the flag and route us through the alternate-positioning branch below instead.
         *
         * That correct position may be exhausted: on a walk where the stable cursor ran out of keys
         * before ingest, it is legitimately unpositioned here. The assert below therefore checks
         * the alternate's identity and the iteration flag rather than requiring it to carry a key.
         */
        WT_ASSERT(CUR2S(clayered),
          c_alternate == op->stable &&
            F_ISSET(clayered, WTI_CLAYERED_ITERATE_NEXT | WTI_CLAYERED_ITERATE_PREV));
        WT_RET_NOTFOUND_OK(__clayered_constituent_iter_helper(op, c_current, forward));
        current_moved = true;
    } else if (!F_ISSET(clayered, iter_flag)) {
        /*
         * The current cursor is positioned but `iter_flag` is not set, so the alternate cannot be
         * trusted and must be positioned from the current key. A prepared conflict is never handled
         * here: that case always has `iter_flag` set and is taken by the branch above.
         */
        WT_ASSERT(CUR2S(clayered), !__clayered_ingest_prepare_blocked(op, c_current));
        WT_RET_NOTFOUND_OK(__clayered_position_alternate(op, c_current, c_alternate, forward));
    }

    /*
     * When both constituents are positioned on the same key, advance the alternate too so the key
     * is not returned twice. This only arises when the current cursor is the ingest cursor, whose
     * value shadows the stable copy; both must carry a key for the comparison to be valid.
     */
    if (F_ISSET(c_alternate, WT_CURSTD_KEY_INT) && F_ISSET(c_current, WT_CURSTD_KEY_INT) &&
      c_current == op->ingest) {
        int cmp;

        WT_RET(__clayered_cursor_compare(op, c_alternate, c_current, &cmp));
        if (cmp == 0)
            WT_RET_NOTFOUND_OK(__clayered_constituent_iter_helper(op, c_alternate, forward));
    }

    /* Move the current cursor if we haven't done so. */
    if (!current_moved)
        WT_RET_NOTFOUND_OK(__clayered_constituent_iter_helper(op, c_current, forward));

    return (0);
}

/*
 * __clayered_iterate_finish --
 *     Post-move bookkeeping: maintain the iteration-direction flags, and restart a fresh walk that
 *     hit a prepared conflict on its first key.
 */
static WT_INLINE int
__clayered_iterate_finish(
  WTI_CURSOR_LAYERED *clayered, uint32_t iter_flag, bool fresh_start, int ret)
{
    if (ret == WT_PREPARE_CONFLICT && fresh_start)
        /*
         * Prepare conflict on the very first key of a fresh walk: ingest is blocked before stable
         * has advanced. Reset ingest so the next call restarts cleanly.
         */
        WT_TRET(__clayered_reset_cursors(clayered, false));
    else if (ret == 0 || ret == WT_PREPARE_CONFLICT) {
        if (!F_ISSET(clayered, iter_flag)) {
            F_CLR(clayered, WTI_CLAYERED_ITERATE_NEXT | WTI_CLAYERED_ITERATE_PREV);
            F_SET(clayered, iter_flag);
        }
    } else {
        /*
         * The constituent-stepping helpers map WT_NOTFOUND to 0 (WT_ERR_NOTFOUND_OK with keep ==
         * false), so an error reaching here is never WT_NOTFOUND.
         */
        WT_ASSERT(CUR2S(clayered), ret != WT_NOTFOUND);
        F_CLR(clayered, WTI_CLAYERED_ITERATE_NEXT | WTI_CLAYERED_ITERATE_PREV);
    }
    return (ret);
}

/*
 * __clayered_iterate_constituents --
 *     Move the constituents to the next (or prev) position. If the cursor is unpositioned, position
 *     the constituents.
 *
 * If only one constituent is available, this logic can be simplified to calling "next" on that
 *     constituent.
 *
 * If the cursor is unpositioned, both constituents are positioned on the first (or last) key.
 *
 * A layered cursor is considered positioned when the customer-visible `iface` cursor has the
 *     WT_CURSTD_KEY_INT flag set. In that case, `current_cursor` is expected to be set to the
 *     constituent used to produce the key/value pair for `iface`, and WT_CURSTD_KEY_INT should be
 *     set for it as well. In this case the `iface` key could be used to position the other
 *     (alternate) constituent correctly.
 *
 * For the `alternate_cursor`, the correct position is either the same key as `current_cursor` or
 *     the next available key if the same key does not exist.
 *
 * Subsequent cursor iteration calls may skip the positioning step entirely, since they guarantee
 *     that both constituents are properly positioned on exit. To detect these cases,
 *     WTI_CLAYERED_ITERATE_NEXT/PREV are used.
 *
 * If both `current_cursor` and `alternate_cursor` are positioned on the same key, both should be
 *     advanced to the next position. Otherwise, only `current_cursor` should be advanced.
 *     `__clayered_get_current` will determine which constituent cursor to return from.
 *
 * Prepared transactions need special handling. A prepared update is only ever seen on the ingest
 *     cursor - the stable cursor ignores prepared updates - and a prepared conflict clears the
 *     ingest cursor's WT_CURSTD_KEY_INT while leaving its page reference intact. A walk blocked
 *     this way is therefore detected through the reference rather than the key, and on the next
 *     call the ingest cursor is driven again so it rechecks the key it is currently blocked on
 *     before the alternate is positioned.
 */
static int
__clayered_iterate_constituents(WTI_CLAYERED_OP *op, uint32_t iter_flag)
{
    WT_DECL_RET;
    bool forward, fresh_start;

    forward = (iter_flag == WTI_CLAYERED_ITERATE_NEXT);

    /*
     * A layered cursor can legitimately have a single constituent: the ingest cursor is NULL when
     * accessing the stable table alone is sufficient (e.g. on the leader), and the stable cursor
     * may likewise be absent. With only one constituent there is no alternate to track, so step it
     * directly and return without touching the iteration flags.
     */
    WT_ASSERT(CUR2S(op->clayered), op->stable != NULL || op->ingest != NULL);
    if (op->ingest == NULL || op->stable == NULL)
        return (__clayered_constituent_iter_helper(
          op, op->ingest != NULL ? op->ingest : op->stable, forward));

    /* When neither constituent is positioned, it is the start of the cursor walk. */
    fresh_start = !__clayered_any_constituent_positioned(op);
    if (fresh_start)
        ret = __clayered_position_both(op, forward);
    else
        ret = __clayered_advance_positioned(op, iter_flag, forward);

    return (__clayered_iterate_finish(op->clayered, iter_flag, fresh_start, ret));
}

/*
 * __clayered_iterate_int --
 *     Inner loop for moving a layered cursor to the next or previous position. Callers are
 *     responsible for enter/leave and for clearing the iteration flag when the transaction context
 *     has changed.
 */
static int
__clayered_iterate_int(WTI_CLAYERED_OP *op, uint32_t iter_flag)
{
    WTI_CURSOR_LAYERED *clayered = op->clayered;
    WT_DECL_RET;
    bool deleted;

    do {
        WT_ERR(__clayered_iterate_constituents(op, iter_flag));
        WT_ERR(__clayered_get_current(op, iter_flag == WTI_CLAYERED_ITERATE_NEXT));
        if (clayered->current_cursor == op->ingest)
            deleted = __wt_clayered_deleted(&clayered->current_cursor->value);
        else
            deleted = false;
    } while (deleted);

err:
    if (ret != 0 && ret != WT_PREPARE_CONFLICT)
        WT_TRET(__clayered_reset_cursors(clayered, false));

    return (ret);
}

/*
 * __clayered_iterate --
 *     Move a layered cursor to the next or previous position.
 */
static int
__clayered_iterate(WTI_CURSOR_LAYERED *clayered, uint32_t iter_flag)
{
    WTI_CLAYERED_OP op;
    WT_CURSOR *iface;
    WT_DECL_RET;

    iface = &clayered->iface;

    WT_ERR(__clayered_enter(clayered, WTI_CLAYERED_MODE_ITERATE, &op));
    WT_ERR(__clayered_iterate_int(&op, iter_flag));

    WT_ITEM_SET(iface->key, clayered->current_cursor->key);
    WT_ITEM_SET(iface->value, clayered->current_cursor->value);
    __clayered_stable_read_value_stat(clayered, &iface->value);
    __clayered_deleted_decode(CUR2S(clayered), &iface->value);
    F_CLR(iface, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
    F_SET(iface, WT_CURSTD_KEY_INT | WT_CURSTD_VALUE_INT);

err:
    if (ret != 0)
        F_CLR(iface, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
    __clayered_leave(clayered);
    return (ret);
}

/*
 * __clayered_next --
 *     WT_CURSOR->next method for the layered cursor type.
 */
static int
__clayered_next(WT_CURSOR *cursor)
{
    WTI_CURSOR_LAYERED *clayered;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    clayered = (WTI_CURSOR_LAYERED *)cursor;

    CURSOR_API_CALL(cursor, session, ret, next, clayered->dhandle);

    CURSOR_API_CHECK_SYSTEM_OVERLOAD(session, ret);

    __cursor_novalue(cursor);
    WT_ERR(__cursor_copy_release(cursor));

    WT_STAT_CONN_DSRC_INCR(session, layered_curs_next);
    WT_ERR(__clayered_iterate(clayered, WTI_CLAYERED_ITERATE_NEXT));
    WT_STAT_CLAYERED_READ_CONSTITUENT_INCR(session, clayered, layered_curs_next);

err:
    API_END_RET(session, ret);
}

/*
 * __clayered_prev --
 *     WT_CURSOR->prev method for the layered cursor type.
 */
static int
__clayered_prev(WT_CURSOR *cursor)
{
    WTI_CURSOR_LAYERED *clayered;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    clayered = (WTI_CURSOR_LAYERED *)cursor;

    CURSOR_API_CALL(cursor, session, ret, prev, clayered->dhandle);

    CURSOR_API_CHECK_SYSTEM_OVERLOAD(session, ret);

    __cursor_novalue(cursor);
    WT_ERR(__cursor_copy_release(cursor));

    WT_STAT_CONN_DSRC_INCR(session, layered_curs_prev);
    WT_ERR(__clayered_iterate(clayered, WTI_CLAYERED_ITERATE_PREV));
    WT_STAT_CLAYERED_READ_CONSTITUENT_INCR(session, clayered, layered_curs_prev);

err:
    API_END_RET(session, ret);
}

/*
 * __clayered_reset_cursors --
 *     Reset any positioned constituent cursors.
 */
static int
__clayered_reset_cursors(WTI_CURSOR_LAYERED *clayered, bool skip_ingest)
{
    WT_CURSOR *c;
    WT_DECL_RET;

    /*
     * Reset constituents that are positioned. Check both KEY_SET and the btree ref, because a
     * prepare conflict clears KEY_SET while leaving the btree cursor positioned (ref != NULL).
     */
    c = clayered->stable_cursor;
    if (c != NULL && F_ISSET(c, WT_CURSTD_KEY_SET))
        WT_TRET(c->reset(c));

    c = clayered->ingest_cursor;
    if (!skip_ingest && c != NULL && ((WT_CURSOR_BTREE *)c)->ref != NULL)
        WT_TRET(c->reset(c));

    clayered->current_cursor = NULL;
    F_CLR(clayered, WTI_CLAYERED_ITERATE_NEXT | WTI_CLAYERED_ITERATE_PREV);

    return (ret);
}

/*
 * __clayered_reconfigure --
 *     WT_CURSOR->reconfigure method for the layered cursor type.
 */
static int
__clayered_reconfigure(WT_CURSOR *cursor, const char *config)
{
    WTI_CURSOR_LAYERED *clayered;

    clayered = (WTI_CURSOR_LAYERED *)cursor;
    WT_RET(__wti_cursor_reconfigure(cursor, config));

    if (clayered->stable_cursor != NULL) {
        if (F_ISSET(cursor, WT_CURSTD_OVERWRITE))
            F_SET(clayered->stable_cursor, WT_CURSTD_OVERWRITE);
        else
            F_CLR(clayered->stable_cursor, WT_CURSTD_OVERWRITE);
    }

    return (0);
}

/*
 * __clayered_reset --
 *     WT_CURSOR->reset method for the layered cursor type.
 */
static int
__clayered_reset(WT_CURSOR *cursor)
{
    WTI_CURSOR_LAYERED *clayered;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    /*
     * Don't use the normal __clayered_enter path: that is wasted work when all we want to do is
     * give up our position.
     */
    clayered = (WTI_CURSOR_LAYERED *)cursor;
    CURSOR_API_CALL_PREPARE_ALLOWED(cursor, session, reset, clayered->dhandle);
    WT_ERR(__cursor_copy_release(cursor));

    WT_TRET(__clayered_reset_cursors(clayered, false));

    /* Reset any bounds on the top level cursor, and propagate that to constituents */
    if (API_USER_ENTRY(session)) {
        __wt_cursor_bound_reset(cursor);
        WT_TRET(__clayered_copy_bounds(clayered));
    }

err:
    F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
    API_END_RET(session, ret);
}

/*
 * __clayered_copy_constituent_bound --
 *     Copy the top level bound into a single constituent cursor
 */
static int
__clayered_copy_constituent_bound(WTI_CURSOR_LAYERED *clayered, WT_CURSOR *constituent)
{
    WT_CURSOR *base_cursor;
    WT_SESSION_IMPL *session;

    session = CUR2S(clayered);
    base_cursor = (WT_CURSOR *)clayered;

    if (constituent == NULL)
        return (0);

    /*
     * It doesn't matter if the bound in question is already set on the constituent. It is legal to
     * reset it. Note that the inclusive flag is additive to upper/lower, so no need to check it as
     * well.
     */
    if (F_ISSET(base_cursor, WT_CURSTD_BOUND_UPPER))
        WT_RET(__wt_buf_set(session, &constituent->upper_bound, base_cursor->upper_bound.data,
          base_cursor->upper_bound.size));
    else {
        __wt_buf_free(session, &constituent->upper_bound);
        WT_CLEAR(constituent->upper_bound);
    }
    if (F_ISSET(base_cursor, WT_CURSTD_BOUND_LOWER))
        WT_RET(__wt_buf_set(session, &constituent->lower_bound, base_cursor->lower_bound.data,
          base_cursor->lower_bound.size));
    else {
        __wt_buf_free(session, &constituent->lower_bound);
        WT_CLEAR(constituent->lower_bound);
    }
    /* Copy across all the bound configurations. */
    F_CLR(constituent, WT_CURSTD_BOUND_ALL);
    F_SET(constituent, F_MASK(base_cursor, WT_CURSTD_BOUND_ALL));
    return (0);
}

/*
 * __clayered_copy_bounds --
 *     A method for copying (or clearing) bounds on constituent cursors within a layered cursor
 */
static int
__clayered_copy_bounds(WTI_CURSOR_LAYERED *clayered)
{
    WT_RET(__clayered_copy_constituent_bound(clayered, clayered->ingest_cursor));
    WT_RET(__clayered_copy_constituent_bound(clayered, clayered->stable_cursor));
    return (0);
}

/*
 * __clayered_bound --
 *     WT_CURSOR->bound method for the layered cursor type.
 */
static int
__clayered_bound(WT_CURSOR *cursor, const char *config)
{
    WT_COLLATOR *collator;
    WTI_CURSOR_LAYERED *clayered;
    WT_DECL_CONF(WT_CURSOR, bound, conf);
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    clayered = (WTI_CURSOR_LAYERED *)cursor;

    /*
     * The bound interface operates on an unpositioned cursor, so skip entering the layered cursor
     * for this API.
     */
    CURSOR_API_CALL(cursor, session, ret, bound, clayered->dhandle);

    WT_ERR(__wt_conf_compile_api_call(session, WT_CONFIG_REF(session, WT_CURSOR_bound),
      WT_CONFIG_ENTRY_WT_CURSOR_bound, config, &_conf, sizeof(_conf), &conf));

    __clayered_get_collator(clayered, &collator);
    /* Setup bounds on this top level cursor */
    WT_ERR(__wti_cursor_bound(cursor, conf, collator));

    /*
     * Copy those bounds into the constituents. Note that the constituent cursors may not be open
     * yet, and that would be fine, the layered cursor open interface handles setting up configured
     * bounds as well.
     */
    WT_ERR(__clayered_copy_bounds(clayered));

err:
    if (ret != 0) {
        /* Free any bounds we set on the top level cursor before the error */
        if (F_ISSET(cursor, WT_CURSTD_BOUND_UPPER)) {
            __wt_buf_free(session, &cursor->upper_bound);
            WT_CLEAR(cursor->upper_bound);
        }
        if (F_ISSET(cursor, WT_CURSTD_BOUND_LOWER)) {
            __wt_buf_free(session, &cursor->lower_bound);
            WT_CLEAR(cursor->lower_bound);
        }
        F_CLR(cursor, WT_CURSTD_BOUND_ALL);
        /* Ensure the bounds are cleaned up on any constituents */
        WT_TRET(__clayered_copy_bounds(clayered));
    }
    API_END_RET(session, ret);
}

/*
 * __clayered_cache --
 *     WT_CURSOR->cache method for the layered cursor type.
 */
static int
__clayered_cache(WT_CURSOR *cursor)
{
    WTI_CURSOR_LAYERED *clayered;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    clayered = (WTI_CURSOR_LAYERED *)cursor;
    session = CUR2S(cursor);

    WT_TRET(__wti_cursor_cache(cursor, clayered->dhandle));
    WT_TRET(__wt_session_release_dhandle(session));

    API_RET_STAT(session, ret, cursor_cache);
}

/*
 * __clayered_reopen_int --
 *     Helper for __clayered_reopen, called with the session data handle set.
 */
static int
__clayered_reopen_int(WT_CURSOR *cursor)
{
    WT_DATA_HANDLE *dhandle;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    bool is_dead;

    session = CUR2S(cursor);
    dhandle = session->dhandle;

    /*
     * Lock the handle: we're only interested in open handles, any other state disqualifies the
     * cache.
     */
    ret = __wt_session_lock_dhandle(session, 0, &is_dead);
    if (!is_dead && ret == 0 && !WT_DHANDLE_CAN_REOPEN(dhandle)) {
        WT_RET(__wt_session_release_dhandle(session));
        ret = __wt_set_return(session, EBUSY);
    }

    /*
     * The data handle may not be available, fail the reopen, and flag the cursor so that the handle
     * won't be unlocked when subsequently closed.
     */
    if (is_dead || ret == EBUSY) {
        F_SET(cursor, WT_CURSTD_DEAD);
        ret = WT_NOTFOUND;
    }
    __wti_cursor_reopen(cursor, dhandle);

    /*
     * The layered handle may have been reopened since we last accessed it. Reset fields in the
     * cursor that point to memory owned by the handle.
     */
    if (ret == 0) {
        WT_LAYERED_TABLE *layered = (WT_LAYERED_TABLE *)session->dhandle;
        cursor->internal_uri = session->dhandle->name;
        cursor->key_format = layered->key_format;
        cursor->value_format = layered->value_format;

        WT_STAT_CONN_DSRC_INCR(session, cursor_reopen);
    }
    return (ret);
}

/*
 * __clayered_reopen --
 *     WT_CURSOR->reopen method for the layered cursor type.
 */
static int
__clayered_reopen(WT_CURSOR *cursor, bool sweep_check_only)
{
    WT_DATA_HANDLE *dhandle;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    bool can_sweep;

    session = CUR2S(cursor);
    dhandle = ((WTI_CURSOR_LAYERED *)cursor)->dhandle;

    if (sweep_check_only) {
        /*
         * The sweep check returns WT_NOTFOUND if the cursor should be swept. Generally if the
         * associated data handle cannot be reopened it should be swept. But a handle being operated
         * on by this thread should not be swept. The situation where a handle cannot be reopened
         * but also cannot be swept can occur if this thread is in the middle of closing a cursor
         * for a handle that is marked as dropped. During the close, a few iterations of the session
         * cursor sweep are run. The sweep calls this function to see if a cursor should be swept,
         * and it may thus be asking about the very cursor being closed.
         */
        can_sweep = !WT_DHANDLE_CAN_REOPEN(dhandle) && dhandle != session->dhandle;
        return (can_sweep ? WT_NOTFOUND : 0);
    }

    /*
     * Temporarily set the session's data handle to the data handle in the cursor. Reopen may be
     * called either as part of an open API call, or during cursor sweep as part of a different API
     * call, so we need to restore the original data handle that was in our session after the reopen
     * completes.
     */
    WT_WITH_DHANDLE(session, dhandle, ret = __clayered_reopen_int(cursor));
    API_RET_STAT(session, ret, cursor_reopen);
}

/*
 * __clayered_lookup_constituent --
 *     The cursor-agnostic parts of layered table lookups.
 */
static int
__clayered_lookup_constituent(WTI_CLAYERED_OP *op, WT_CURSOR *c, WT_ITEM *value)
{
    WTI_CURSOR_LAYERED *clayered = op->clayered;
    WT_CURSOR *cursor;
    WT_DECL_RET;

    cursor = &clayered->iface;

    c->set_key(c, &cursor->key);
    if ((ret = c->search(c)) == 0) {
        WT_RET(c->get_value(c, value));
        clayered->current_cursor = c;
    }

    return (ret);
}

/*
 * __clayered_lookup_ingest_and_truncate --
 *     Check the ingest constituent and the truncate list for a key, without touching stable.
 *     Returns 0 with the value populated for a live ingest entry, or WT_NOTFOUND if the key is
 *     confirmed deleted there (a tombstone in ingest, or covered by the truncate list). The out
 *     parameter is set to whether an entry was found locally at all, tombstone or not; if it comes
 *     back false alongside WT_NOTFOUND, neither ingest nor the truncate list said anything about
 *     this key.
 */
static int
__clayered_lookup_ingest_and_truncate(WTI_CLAYERED_OP *op, WT_ITEM *value, bool *found_localp)
{
    WTI_CURSOR_LAYERED *clayered = op->clayered;
    WT_SESSION_IMPL *session = CUR2S(clayered);
    WT_CURSOR *cursor = &clayered->iface;
    WT_DECL_RET;
    bool found_local = false;

    WT_ERR_NOTFOUND_OK(__clayered_lookup_constituent(op, op->ingest, value), true);
    if (ret == 0) {
        found_local = true;
        if (__wt_clayered_deleted(value))
            ret = WT_NOTFOUND;
    }

    /* Only consult the truncate list when ingest has no entry for this key. */
    if (!found_local) {
        WT_ERR_NOTFOUND_OK(__wt_truncate_delete_visible_check(
                             session, op->truncate_list, op->collator, &cursor->key, NULL, NULL),
          true);
        if (ret == 0) {
            found_local = true;
            ret = WT_NOTFOUND;
        }
    }

err:
    *found_localp = found_local;
    return (ret);
}

/*
 * __clayered_lookup --
 *     Position a layered cursor.
 */
static int
__clayered_lookup(WTI_CLAYERED_OP *op, WT_ITEM *value)
{
    WTI_CURSOR_LAYERED *clayered = op->clayered;
    WT_SESSION_IMPL *session = CUR2S(clayered);
    WT_DECL_RET;
    bool found = false;

    if (op->ingest != NULL)
        WT_ERR_NOTFOUND_OK(__clayered_lookup_ingest_and_truncate(op, value, &found), true);
    else
        /* Be sure we'll make a search attempt further down.  */
        WT_ASSERT(session, op->stable != NULL);

    /* If the key didn't exist in ingest and the cursor is setup for reading, check stable. */
    if (!found && op->stable != NULL)
        WT_ERR_NOTFOUND_OK(__clayered_lookup_constituent(op, op->stable, value), true);

err:
    if (ret != 0) {
        WT_TRET(__clayered_reset_cursors(clayered, false));
        /* Reset the buffer if the key was deleted on the ingest table. */
        value->data = NULL;
        value->size = 0;
    }

    return (ret);
}

/*
 * __clayered_search --
 *     WT_CURSOR->search method for the layered cursor type.
 */
static int
__clayered_search(WT_CURSOR *cursor)
{
    WTI_CLAYERED_OP op;
    WTI_CURSOR_LAYERED *clayered;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    clayered = (WTI_CURSOR_LAYERED *)cursor;

    CURSOR_API_CALL(cursor, session, ret, search, clayered->dhandle);
    F_CLR(clayered, WTI_CLAYERED_ITERATE_NEXT | WTI_CLAYERED_ITERATE_PREV);
    WT_ERR(__cursor_copy_release(cursor));
    WT_ERR(__cursor_needkey(cursor));
    __cursor_novalue(cursor);
    WT_ERR(__clayered_enter(clayered, WTI_CLAYERED_MODE_SEARCH, &op));

    CURSOR_API_CHECK_SYSTEM_OVERLOAD(session, ret);

    WT_STAT_CONN_DSRC_INCR(session, layered_curs_search);
    WT_ERR(__clayered_lookup(&op, &cursor->value));
    WT_ITEM_SET(cursor->key, clayered->current_cursor->key);
    WT_STAT_CLAYERED_READ_CONSTITUENT_INCR(session, clayered, layered_curs_search);

err:
    __clayered_leave(clayered);
    if (ret == 0) {
        __clayered_stable_read_value_stat(clayered, &cursor->value);
        __clayered_deleted_decode(session, &cursor->value);
        F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
        F_SET(cursor, WT_CURSTD_KEY_INT | WT_CURSTD_VALUE_INT);
    }
    API_END_RET(session, ret);
}

/*
 * __clayered_search_near_move_ingest_to_opposite_side --
 *     search near helper function to move the ingest btree to the opposite side of the search key.
 */
static int
__clayered_search_near_move_ingest_to_opposite_side(
  WTI_CLAYERED_OP *op, int stable_cmp, int *ingest_cmp)
{
    WTI_CURSOR_LAYERED *clayered = op->clayered;
    WT_SESSION_IMPL *session = CUR2S(clayered);
    WT_CURSOR *cursor, *c_ingest;
    WT_DECL_RET;

    cursor = &clayered->iface;
    c_ingest = op->ingest;

    /*
     * When reading with read-uncommitted isolation, concurrent key insertions may occur. Continue
     * the walk until the search key is reached or passed.
     */
    if (stable_cmp > 0) {
        /* Stable is larger. Move ingest forward to find a larger key in ingest. */
        do {
            WT_ERR_NOTFOUND_OK(c_ingest->next(c_ingest), true);

            if (session->txn->isolation != WT_ISO_READ_UNCOMMITTED) {
                *ingest_cmp = stable_cmp;
                break;
            }

            if (ret == 0)
                WT_ERR(
                  __wt_compare(session, op->collator, &c_ingest->key, &cursor->key, ingest_cmp));
        } while (ret == 0 && *ingest_cmp < 0);
    } else {
        /* Stable is smaller. Move ingest backward to find a smaller key in ingest. */
        do {
            WT_ERR_NOTFOUND_OK(c_ingest->prev(c_ingest), true);

            if (session->txn->isolation != WT_ISO_READ_UNCOMMITTED) {
                *ingest_cmp = stable_cmp;
                break;
            }

            if (ret == 0)
                WT_ERR(
                  __wt_compare(session, op->collator, &c_ingest->key, &cursor->key, ingest_cmp));
        } while (ret == 0 && *ingest_cmp > 0);
    }
err:
    return (ret);
}

#ifdef HAVE_DIAGNOSTIC
/*
 * __clayered_search_near_assert_side --
 *     Verify a repositioned cursor lies on the expected side of the search key: a forward step must
 *     land on a larger key, a backward step on a smaller one.
 */
static int
__clayered_search_near_assert_side(WTI_CLAYERED_OP *op, WT_CURSOR *c, int expected)
{
    WTI_CURSOR_LAYERED *clayered = op->clayered;
    WT_SESSION_IMPL *session = CUR2S(clayered);
    int cmp;

    WT_RET(__wt_compare(session, op->collator, &c->key, &clayered->iface.key, &cmp));
    WT_ASSERT(session, expected > 0 ? cmp > 0 : cmp < 0);
    return (0);
}
#endif

/*
 * __clayered_search_near_skip_truncated --
 *     A stable key found by search_near can be logically deleted by a committed fast-truncate
 *     range. Step the stable cursor forward past any truncated ranges; if forward exhausts, step
 *     backward instead, updating the exact-match indicator to match.
 */
static int
__clayered_search_near_skip_truncated(WTI_CLAYERED_OP *op, int *stable_cmpp)
{
    WTI_CURSOR_LAYERED *clayered = op->clayered;
    WT_SESSION_IMPL *session = CUR2S(clayered);
    WT_DECL_RET;

    /* Nothing to do unless the stable key falls in a committed fast-truncate range. */
    WT_ERR_NOTFOUND_OK(__wt_truncate_delete_visible_check(
                         session, op->truncate_list, op->collator, &op->stable->key, NULL, NULL),
      true);
    if (ret == WT_NOTFOUND)
        return (0);

    /*
     * The cursor is still resolving a position, so iface holds the external search key and must not
     * be marked positioned (KEY_INT).
     */
    WT_ASSERT(session, !F_ISSET(&clayered->iface, WT_CURSTD_KEY_INT));

    WT_ERR_NOTFOUND_OK(__clayered_constituent_iter_helper(op, op->stable, true), true);
    if (ret == 0) {
#ifdef HAVE_DIAGNOSTIC
        WT_ERR(__clayered_search_near_assert_side(op, op->stable, 1));
#endif
        *stable_cmpp = 1;
        return (0);
    }

    WT_ERR_NOTFOUND_OK(__clayered_constituent_iter_helper(op, op->stable, false), true);
    if (ret == 0) {
#ifdef HAVE_DIAGNOSTIC
        WT_ERR(__clayered_search_near_assert_side(op, op->stable, -1));
#endif
        *stable_cmpp = -1;
    }

err:
    return (ret);
}

/*
 * __clayered_search_near_choose_closest --
 *     Both constituents are positioned near the search key; pick the one that best matches it.
 */
static int
__clayered_search_near_choose_closest(
  WTI_CLAYERED_OP *op, int *ingest_cmpp, int stable_cmp, WT_CURSOR **closestp)
{
    WT_SESSION_IMPL *session = CUR2S(op->clayered);
    WT_DECL_RET;
    int ingest_cmp = *ingest_cmpp;

    /* An exact match on either constituent wins. */
    if (ingest_cmp == 0) {
        *closestp = op->ingest;
        return (0);
    }
    if (stable_cmp == 0) {
        *closestp = op->stable;
        return (0);
    }

    /*
     * The cursors are on opposite sides of the search key. Move the ingest cursor to the stable
     * cursor's side so the two are comparable; otherwise a closer ingest key on the far side would
     * be overlooked. If the ingest cursor runs out, the stable cursor is the closest.
     */
    if ((ingest_cmp ^ stable_cmp) < 0) {
        if ((ret = __clayered_search_near_move_ingest_to_opposite_side(
               op, stable_cmp, &ingest_cmp)) == WT_NOTFOUND) {
            *closestp = op->stable;
            return (0);
        }
        WT_RET(ret);
        *ingest_cmpp = ingest_cmp;
    }

    /* A concurrent insert (read-uncommitted only) can land the ingest cursor on the search key. */
    if (ingest_cmp == 0) {
        WT_ASSERT(session, session->txn->isolation == WT_ISO_READ_UNCOMMITTED);
        *closestp = op->ingest;
        return (0);
    }

    /*
     * Both cursors are now on the same side of the search key: pick the smaller key when both are
     * larger, the larger key when both are smaller. On a tie, prefer the ingest cursor.
     */
    int cmp;
    WT_RET(__clayered_cursor_compare(op, op->ingest, op->stable, &cmp));
    if (ingest_cmp > 0) {
        WT_ASSERT(session, stable_cmp > 0);
        *closestp = cmp <= 0 ? op->ingest : op->stable;
    } else {
        WT_ASSERT(session, stable_cmp < 0);
        *closestp = cmp >= 0 ? op->ingest : op->stable;
    }
    return (0);
}

/*
 * __clayered_search_near_resolve_deleted --
 *     Compute the exact-match indicator for the chosen constituent, stepping past an ingest
 *     tombstone to a live neighbor when the closest match is a deleted record.
 */
static int
__clayered_search_near_resolve_deleted(
  WTI_CLAYERED_OP *op, WT_CURSOR *closest, int ingest_cmp, int stable_cmp, int *cmpp)
{
    WTI_CURSOR_LAYERED *clayered = op->clayered;
    WT_SESSION_IMPL *session = CUR2S(clayered);
    WT_DECL_RET;

    /* The stable cursor never holds a tombstone. */
    if (closest == op->stable) {
        *cmpp = stable_cmp;
        return (0);
    }

    *cmpp = ingest_cmp;
    if (!__wt_clayered_deleted(&closest->value))
        return (0);

    /* Advance past the deleted record; if that exhausts the tree, step backward instead. */
    WT_ASSERT(session, !F_ISSET(&clayered->iface, WT_CURSTD_KEY_INT));
    if ((ret = __clayered_iterate_int(op, WTI_CLAYERED_ITERATE_NEXT)) == 0) {
#ifdef HAVE_DIAGNOSTIC
        WT_RET(__clayered_search_near_assert_side(op, clayered->current_cursor, 1));
#endif
        *cmpp = 1;
        return (0);
    }
    WT_RET_NOTFOUND_OK(ret);

    WT_ASSERT(session, clayered->current_cursor == NULL);
    WT_ASSERT(session, !F_ISSET(&clayered->iface, WT_CURSTD_KEY_INT));
    WT_RET(__clayered_iterate_int(op, WTI_CLAYERED_ITERATE_PREV));
#ifdef HAVE_DIAGNOSTIC
    WT_RET(__clayered_search_near_assert_side(op, clayered->current_cursor, -1));
#endif
    *cmpp = -1;
    return (0);
}

/*
 * __clayered_search_near_reset_other --
 *     Release the page pinned by the constituent that is not the current cursor. Skip this when
 *     search-near iterated internally, which leaves both constituents positioned.
 */
static int
__clayered_search_near_reset_other(WTI_CLAYERED_OP *op)
{
    WTI_CURSOR_LAYERED *clayered = op->clayered;
    WT_SESSION_IMPL *session = CUR2S(clayered);

    WT_ASSERT(
      session, clayered->current_cursor == op->ingest || clayered->current_cursor == op->stable);

    if (F_ISSET(clayered, WTI_CLAYERED_ITERATE_NEXT | WTI_CLAYERED_ITERATE_PREV))
        return (0);
    if (op->stable != NULL && clayered->current_cursor == op->ingest)
        return (op->stable->reset(op->stable));
    if (op->ingest != NULL && clayered->current_cursor == op->stable)
        return (op->ingest->reset(op->ingest));
    return (0);
}

/*
 * __clayered_search_near_int --
 *     search near method for the layered cursor type.
 */
static int
__clayered_search_near_int(WTI_CLAYERED_OP *op, int *exactp)
{
    WTI_CURSOR_LAYERED *clayered = op->clayered;
    WT_SESSION_IMPL *session = CUR2S(clayered);
    WT_CURSOR *closest, *cursor;
    WT_DECL_RET;
    int ingest_cmp, stable_cmp;
    bool ingest_found, match_deleted, stable_found;

    closest = NULL;
    cursor = &clayered->iface;
    ingest_cmp = stable_cmp = 0;
    ingest_found = match_deleted = stable_found = false;

    /*
     * search_near is somewhat fiddly: we can't just use a nearby key from one constituent because
     * there could be a closer key in the other.
     *
     * The semantics are:
     * * An exact match always wins.
     * * Otherwise, when both constituents are positioned on opposite sides of the search key, the
     *   ingest cursor is realigned to the stable cursor's side so a closer ingest key on that side
     *   is not overlooked; the result then follows the stable cursor's side.
     * * On the same side, the key closest to the search term wins, with ties resolved to ingest.
     *
     * FIXME-WT-17967: evaluate simplifying the side-selection above.
     */
    if (op->ingest != NULL) {
        op->ingest->set_key(op->ingest, &cursor->key);
        WT_ERR_NOTFOUND_OK(op->ingest->search_near(op->ingest, &ingest_cmp), true);
        if (ret == 0) {
            ingest_found = true;
            match_deleted = __wt_clayered_deleted(&op->ingest->value);
        }
    }

    /* If there wasn't an exact match or the value is deleted, check the stable table as well */
    if ((!ingest_found || ingest_cmp != 0 || match_deleted) && op->stable != NULL) {
        op->stable->set_key(op->stable, &cursor->key);
        WT_ERR_NOTFOUND_OK(op->stable->search_near(op->stable, &stable_cmp), true);
        if (ret == 0)
            WT_ERR_NOTFOUND_OK(__clayered_search_near_skip_truncated(op, &stable_cmp), true);
        stable_found = ret != WT_NOTFOUND;
    }

    if (!ingest_found && !stable_found) {
        ret = WT_NOTFOUND;
        goto err;
    } else if (!stable_found)
        closest = op->ingest;
    else if (!ingest_found)
        closest = op->stable;
    else
        /* Both constituents are positioned - choose the one with the best match. */
        WT_ERR(__clayered_search_near_choose_closest(op, &ingest_cmp, stable_cmp, &closest));

    WT_ASSERT_ALWAYS(session, closest != NULL, "Layered search near should have found something");

    clayered->current_cursor = closest;

    int cmp;
    WT_ERR(__clayered_search_near_resolve_deleted(op, closest, ingest_cmp, stable_cmp, &cmp));
    if (exactp != NULL)
        *exactp = cmp;

    /* Drop the page pinned by the constituent that is not the current cursor. */
    WT_ERR(__clayered_search_near_reset_other(op));

err:
    if (ret != 0)
        WT_TRET(__clayered_reset_cursors(clayered, false));

    return (ret);
}

/*
 * __clayered_search_near --
 *     WT_CURSOR->search_near method for the layered cursor type.
 */
static int
__clayered_search_near(WT_CURSOR *cursor, int *exactp)
{
    WTI_CLAYERED_OP op;
    WTI_CURSOR_LAYERED *clayered;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    clayered = (WTI_CURSOR_LAYERED *)cursor;

    CURSOR_API_CALL(cursor, session, ret, search_near, clayered->dhandle);
    F_CLR(clayered, WTI_CLAYERED_ITERATE_NEXT | WTI_CLAYERED_ITERATE_PREV);
    WT_ERR(__cursor_copy_release(cursor));
    WT_ERR(__cursor_needkey(cursor));
    __cursor_novalue(cursor);
    WT_ERR(__clayered_enter(clayered, WTI_CLAYERED_MODE_SEARCH, &op));

    CURSOR_API_CHECK_SYSTEM_OVERLOAD(session, ret);

    WT_STAT_CONN_DSRC_INCR(session, layered_curs_search_near);
    WT_ERR(__clayered_search_near_int(&op, exactp));
    WT_STAT_CLAYERED_READ_CONSTITUENT_INCR(session, clayered, layered_curs_search_near);

    WT_ITEM_SET(cursor->key, clayered->current_cursor->key);
    WT_ITEM_SET(cursor->value, clayered->current_cursor->value);

err:
    __clayered_leave(clayered);
    if (ret == 0) {
        __clayered_stable_read_value_stat(clayered, &cursor->value);
        __clayered_deleted_decode(session, &cursor->value);
        F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
        F_SET(cursor, WT_CURSTD_KEY_INT | WT_CURSTD_VALUE_INT);
    }

    API_END_RET(session, ret);
}

/*
 * __clayered_reserve_constituent --
 *     Reserve a key in the constituent cursor.
 */
static int
__clayered_reserve_constituent(WTI_CLAYERED_OP *op, WT_CURSOR *constituent)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    CURSOR_UPDATE_API_CALL_BTREE(constituent, session, ret, reserve);

    CURSOR_API_CHECK_SYSTEM_OVERLOAD(session, ret);

    /*
     * Pass overwrite=true for ingest table, it may not contain the key yet (it lives only in the
     * stable table), so we need overwrite mode to allow the reserve to succeed without the key
     * being present in the update tree.
     */
    WT_ERR(__wt_btcur_reserve((WT_CURSOR_BTREE *)constituent, constituent == op->ingest));

err:
    CURSOR_UPDATE_API_END_STAT(session, ret, cursor_reserve);

    return (ret);
}

/*
 * __clayered_put --
 *     Put an entry into the desired tree.
 */
static WT_INLINE int
__clayered_put(
  WTI_CLAYERED_OP *op, const WT_ITEM *key, const WT_ITEM *value, WTI_CLAYERED_PUT_OP put_op)
{
    WTI_CURSOR_LAYERED *clayered = op->clayered;
    WT_SESSION_IMPL *session = CUR2S(clayered);
    WT_CURSOR *c = op->ingest != NULL ? op->ingest : op->stable;
    WT_ASSERT(session, c != NULL);

    if (c == op->ingest) {
        /*
         * FIXME-WT-17425: Investigate whether this function can be called below the cursor layer.
         * Doing so would remove the cursor write operation dependency on the truncate list.
         */
        WT_RET(__wt_layered_table_truncate_detect_write_conflict(
          session, op->truncate_list, op->collator, key));

        /*
         * Clear the stable cursor position. Don't clear the ingest cursor: we're about to use it
         * anyway. Keep the cursor position if we are in the middle of a cursor traversal.
         */
        if (!F_ISSET(clayered, WTI_CLAYERED_ITERATE_NEXT | WTI_CLAYERED_ITERATE_PREV))
            WT_RET(__clayered_reset_cursors(clayered, true));
    }

    c->set_key(c, key);
    /* Reserve does not require a value. */
    if (put_op != WTI_CLAYERED_PUT_RESERVE)
        c->set_value(c, value);

    /* On the leader the destination is the stable table; account tombstone-namespace values. */
    if (c != op->ingest && put_op != WTI_CLAYERED_PUT_RESERVE)
        __wt_clayered_stable_value_stat(session, value->data, value->size);

    switch (put_op) {
    case WTI_CLAYERED_PUT_INSERT:
        WT_RET(c->insert(c));
        break;
    case WTI_CLAYERED_PUT_UPDATE:
        WT_RET(c->update(c));
        break;
    case WTI_CLAYERED_PUT_RESERVE:
        WT_RET(__clayered_reserve_constituent(op, c));
        break;
    }

    /* If necessary, set the position for future scans. */
    if (put_op != WTI_CLAYERED_PUT_INSERT)
        clayered->current_cursor = c;

    return (0);
}

/*
 * __clayered_cell_check --
 *     Detect a write conflict on a positioned constituent cursor.
 */
static int
__clayered_cell_check(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt)
{
    WT_TIME_WINDOW tw;

    /* Ignore prepared updates, matching the leader. */
    if (__wt_read_cell_time_window(cbt, &tw) && WT_TIME_WINDOW_HAS_PREPARE(&tw))
        return (0);

    /* Ensure the pinned page is valid. */
    WT_ASSERT(session, cbt->ref != NULL && cbt->ref->page != NULL);

    /* Use the in-memory update chain if present (ingest). */
    WT_UPDATE *upd = NULL;
    if (cbt->ins != NULL)
        upd = cbt->ins->upd;
    else if (CUR2BT(cbt)->type == BTREE_ROW && cbt->ref->page->modify != NULL &&
      cbt->ref->page->modify->mod_row_update != NULL) {
        WT_ASSERT(session, cbt->slot != UINT32_MAX);
        upd = cbt->ref->page->modify->mod_row_update[cbt->slot];
    }

    return (__wt_txn_modify_check(session, cbt, upd, NULL, WT_UPDATE_STANDARD));
}

/*
 * __clayered_constituent_check --
 *     Check a constituent for a write conflict on the key.
 */
static int
__clayered_constituent_check(
  WT_SESSION_IMPL *session, WTI_CURSOR_LAYERED *clayered, WT_CURSOR *c, const WT_ITEM *key)
{
    if (c == NULL)
        return (0);

    WT_CURSOR_BTREE *cbt = (WT_CURSOR_BTREE *)c;
    WT_DECL_RET;

    if (F_ISSET(&clayered->iface, WT_CURSTD_KEY_INT) && clayered->current_cursor == c) {
        /* Positioned on the key: check the held cell and keep the position. */
        WT_WITH_DHANDLE(session, cbt->dhandle, ret = __clayered_cell_check(session, cbt));
    } else {
        WT_WITH_DHANDLE(session, cbt->dhandle, {
            /* Release any page held from a prior op before searching. */
            ret = __wt_btcur_reset(cbt);
            if (ret == 0) {
                WT_WITH_PAGE_INDEX(
                  session, ret = __wt_row_search(cbt, (WT_ITEM *)key, false, NULL, false, NULL));
                if (ret == 0 && cbt->compare == 0)
                    ret = __clayered_cell_check(session, cbt);
            }
            WT_TRET(__wt_btcur_reset(cbt));
        });
    }

    return (ret);
}

/*
 * __clayered_modify_check --
 *     Detect a write conflict for a follower write: a committed update invisible to this
 *     transaction in either constituent.
 */
static int
__clayered_modify_check(WT_SESSION_IMPL *session, WTI_CURSOR_LAYERED *clayered, const WT_ITEM *key)
{
    /* No read timestamp means every update is visible; nothing to probe. */
    if (!F_ISSET(session->txn, WT_TXN_SHARED_TS_READ))
        return (0);

    /* The leader's underlying stable cursor runs the check itself. */
    if (S2C(session)->layered_table_manager.leader)
        return (0);

    /*
     * The probe searches and resets the constituent cursors, destroying the walk-consistent
     * positions an in-progress iteration relies on. Clear the iteration flags so the next
     * next()/prev() re-seats both constituents.
     */
    F_CLR(clayered, WTI_CLAYERED_ITERATE_NEXT | WTI_CLAYERED_ITERATE_PREV);

    /* Probe the ingest and stable tables. */
    WT_RET(__clayered_constituent_check(session, clayered, clayered->ingest_cursor, key));
    WT_RET(__clayered_constituent_check(session, clayered, clayered->stable_cursor, key));

    return (0);
}

/*
 * __clayered_remove_from_ingest --
 *     Remove an entry from the ingest table.
 */
static WT_INLINE int
__clayered_remove_from_ingest(WTI_CLAYERED_OP *op, const WT_ITEM *key, bool positioned)
{
    WTI_CURSOR_LAYERED *clayered = op->clayered;
    WT_SESSION_IMPL *session = CUR2S(clayered);
    WT_CURSOR *const c_ingest = op->ingest;
    WT_DECL_RET;
    WT_ITEM value;
    bool blind_remove;
    bool found_local;

    WT_CLEAR(value);
    found_local = true;

    /*
     * A NULL operation stable cursor has two meanings: this operation may have deliberately hidden
     * an available stable cursor for an overwrite follower write, or the follower may not have a
     * checkpoint yet. Only the former can assume a key missed by ingest exists in stable.
     */
    blind_remove = op->stable == NULL && clayered->stable_cursor != NULL &&
      F_ISSET(&clayered->iface, WT_CURSTD_OVERWRITE);

    WT_RET(__clayered_modify_check(session, clayered, key));

    /* The cached value can be stale once VALUE_INT is cleared (localized at a txn boundary). */
    bool hold_value =
      clayered->current_cursor != NULL && F_ISSET(clayered->current_cursor, WT_CURSTD_VALUE_INT);

    if (blind_remove || !positioned || !hold_value) {
        /* Cached value isn't reliable (unpositioned or not holding the value ref); re-read it. */
        WT_ASSERT(session, F_ISSET(&clayered->iface, WT_CURSTD_KEY_EXT));
        if (blind_remove) {
            ret = __clayered_lookup_ingest_and_truncate(op, &value, &found_local);
            if (ret == WT_NOTFOUND && found_local) {
                /* A local deletion marker violates the caller's live-key guarantee. */
                WT_ASSERT_ALWAYS(
                  session, false, "overwrite=true should guarantee the key exists for remove()");
                return (0);
            }

            if (ret == WT_NOTFOUND && !found_local)
                ret = 0;
        } else
            ret = __clayered_lookup(op, &value);
        if (ret != 0) {
            WT_TRET(__clayered_reset_cursors(clayered, false));
            return (ret);
        }
    } else if (clayered->current_cursor == c_ingest) {
        WT_ASSERT(session, F_ISSET(c_ingest, WT_CURSTD_KEY_INT));
        /* Skip an existing tombstone: no consecutive tombstones on an update chain. */
        WT_ITEM_SET(value, c_ingest->value);
        if (__wt_clayered_deleted(&value))
            return (WT_NOTFOUND);
    }

    /*
     * If ingest wasn't confirmed positioned on this key (found_local is false, whether because the
     * key was only in stable, or -- for overwrite=true -- because neither ingest nor the truncate
     * list had an entry for it), current_cursor can still be whatever an unrelated earlier
     * operation on this cursor left it as -- WT_CURSOR::set_key doesn't clear it. Never trust it in
     * that case: always set the key explicitly rather than risk writing under a stale one.
     */
    if (!found_local || clayered->current_cursor != c_ingest)
        c_ingest->set_key(c_ingest, key);

    /*
     * Clear the stable cursor position. Don't clear the ingest cursor: we're about to use it
     * anyway. Keep the cursor position if we are in the middle of a cursor traversal.
     */
    if (!F_ISSET(clayered, WTI_CLAYERED_ITERATE_NEXT | WTI_CLAYERED_ITERATE_PREV))
        WT_RET(__clayered_reset_cursors(clayered, true));

    /*
     * FIXME-WT-17425: Investigate whether this function can be called below the cursor layer. Doing
     * so would remove the write cursor operations dependency on the truncate list.
     */
    WT_RET(__wt_layered_table_truncate_detect_write_conflict(
      session, op->truncate_list, op->collator, key));
    c_ingest->set_value(c_ingest, &__wt_tombstone);
    WT_ERR(c_ingest->update(c_ingest));
    clayered->current_cursor = c_ingest;

err:
    if (ret != 0)
        WT_TRET(__clayered_reset_cursors(clayered, false));
    return (ret);
}

/*
 * __clayered_remove_from_stable --
 *     Remove an entry from the stable table.
 */
static WT_INLINE int
__clayered_remove_from_stable(WTI_CLAYERED_OP *op, const WT_ITEM *key, bool positioned)
{
    WTI_CURSOR_LAYERED *clayered = op->clayered;
    WT_SESSION_IMPL *session = CUR2S(clayered);
    WT_CURSOR *const c_stable = op->stable;

    /* There is no content on the ingest table. We must be positioned on the stable table. */
    if (!positioned)
        c_stable->set_key(c_stable, key);
    else
        WT_ASSERT(session, F_ISSET(c_stable, WT_CURSTD_KEY_INT));

    WT_RET(c_stable->remove(c_stable));
    clayered->current_cursor = c_stable;

    return (0);
}

/*
 * __clayered_remove_int --
 *     Remove an entry from the desired tree.
 */
static WT_INLINE int
__clayered_remove_int(WTI_CLAYERED_OP *op, const WT_ITEM *key, bool positioned)
{
    return (op->ingest == NULL ? __clayered_remove_from_stable(op, key, positioned) :
                                 __clayered_remove_from_ingest(op, key, positioned));
}

/*
 * __clayered_copy_duplicate_kv --
 *     Copy the duplicate key value from the constitute cursor.
 */
static int
__clayered_copy_duplicate_kv(WTI_CLAYERED_OP *op)
{
    WTI_CURSOR_LAYERED *clayered = op->clayered;
    WT_SESSION_IMPL *session = CUR2S(clayered);
    WT_CURSOR *cursor = &clayered->iface;

    WT_ASSERT(session,
      F_ISSET(clayered->current_cursor, WT_CURSTD_KEY_INT) &&
        F_ISSET(clayered->current_cursor, WT_CURSTD_VALUE_INT));
    F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
    WT_ITEM_SET(cursor->key, clayered->current_cursor->key);
    WT_ITEM_SET(cursor->value, clayered->current_cursor->value);
    F_SET(cursor, WT_CURSTD_KEY_INT | WT_CURSTD_VALUE_INT);
    WT_RET(__wt_cursor_localkey(cursor));
    WT_RET(__cursor_localvalue(cursor));
    WT_RET(__clayered_reset_cursors(clayered, false));

    return (0);
}

/*
 * __clayered_needs_pre_lookup --
 *     Return whether a write must look up the key before modifying the ingest table.
 */
static WT_INLINE bool
__clayered_needs_pre_lookup(WTI_CLAYERED_OP *op)
{
    /*
     * The ingest cursor is always in overwrite mode so insert() can write over an ingest tombstone,
     * which means non-overwrite duplicate detection has to happen here instead. This lookup also
     * covers the cases that need both constituents consulted, currently a subset of having an
     * ingest cursor.
     */
    return (op->ingest != NULL && !F_ISSET(&op->clayered->iface, WT_CURSTD_OVERWRITE));
}

/*
 * __clayered_insert --
 *     WT_CURSOR->insert method for the layered cursor type.
 */
static int
__clayered_insert(WT_CURSOR *cursor)
{
    WTI_CLAYERED_OP op;
    WTI_CURSOR_LAYERED *clayered;
    WT_DECL_ITEM(buf);
    WT_DECL_RET;
    WT_ITEM value;
    WT_SESSION_IMPL *session;

    WT_CLEAR(value);
    clayered = (WTI_CURSOR_LAYERED *)cursor;

    CURSOR_UPDATE_API_CALL(cursor, session, ret, insert, clayered->dhandle);

    CURSOR_API_CHECK_SYSTEM_OVERLOAD(session, ret);

    /* Insert doesn't keep the cursor positioned. Always clear the iteration flags. */
    F_CLR(clayered, WTI_CLAYERED_ITERATE_NEXT | WTI_CLAYERED_ITERATE_PREV);
    WT_ERR(__cursor_copy_release(cursor));
    WT_ERR(__cursor_needkey(cursor));
    WT_ERR(__cursor_needvalue(cursor));
    WT_ERR(__clayered_enter(clayered,
      F_ISSET(cursor, WT_CURSTD_OVERWRITE) ? WTI_CLAYERED_MODE_WRITE_OVERWRITE :
                                             WTI_CLAYERED_MODE_WRITE,
      &op));

    /*
     * It isn't necessary to copy the key out after the lookup in this case because any non-failed
     * lookup results in an error, and a failed lookup leaves the original key intact.
     */
    if (__clayered_needs_pre_lookup(&op)) {
        WT_ERR_NOTFOUND_OK(__clayered_lookup(&op, &value), true);
        if (ret == 0) {
            WT_ERR(__clayered_copy_duplicate_kv(&op));
            WT_ERR(__clayered_reset_cursors(clayered, false));
            WT_ERR(WT_DUPLICATE_KEY);
        } else if (ret != WT_NOTFOUND) { /* Not found is a happy path. */
            goto err;
        }
    }

    WT_ERR(__clayered_modify_check(session, clayered, &cursor->key));

    /* FIXME-WT-17933: on the leader this encodes into the stable table. */
    WT_ERR(__clayered_deleted_encode(session, &cursor->value, &value, &buf));
    ret = __clayered_put(&op, &cursor->key, &value, WTI_CLAYERED_PUT_INSERT);
    if (ret == WT_DUPLICATE_KEY) {
        WT_ASSERT(session, op.ingest == NULL);
        /*
         * The btree cursor already holds a local copy of the existing value from duplicate
         * detection. Copy it directly without a second search.
         */
        F_CLR(cursor, WT_CURSTD_VALUE_SET);
        WT_ITEM_SET(cursor->value, op.stable->value);
        F_SET(cursor, WT_CURSTD_VALUE_INT);
        WT_ERR(__cursor_localvalue(cursor));
        WT_ERR(WT_DUPLICATE_KEY);
    }
    WT_ERR(ret);

    /*
     * WT_CURSOR.insert doesn't leave the cursor positioned, and the application may want to free
     * the memory used to configure the insert; don't read that memory again (matching the
     * underlying file object cursor insert semantics).
     */
    F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);

    WT_STAT_CONN_DSRC_INCR(session, layered_curs_insert);
err:
    __wt_scr_free(session, &buf);
    __clayered_leave(clayered);
    CURSOR_UPDATE_API_END(session, ret);
    return (ret);
}

/*
 * __clayered_update --
 *     WT_CURSOR->update method for the layered cursor type.
 */
static int
__clayered_update(WT_CURSOR *cursor)
{
    WTI_CLAYERED_OP op;
    WTI_CURSOR_LAYERED *clayered;
    WT_DECL_ITEM(buf);
    WT_DECL_RET;
    WT_ITEM value;
    WT_SESSION_IMPL *session;

    WT_CLEAR(value);
    clayered = (WTI_CURSOR_LAYERED *)cursor;

    CURSOR_UPDATE_API_CALL(cursor, session, ret, update, clayered->dhandle);

    CURSOR_API_CHECK_SYSTEM_OVERLOAD(session, ret);

    /*
     * Update keeps the cursor positioned. Retain the iteration flags if we are in the middle of a
     * cursor traversal.
     */
    if (!F_ISSET(cursor, WT_CURSTD_KEY_INT))
        F_CLR(clayered, WTI_CLAYERED_ITERATE_NEXT | WTI_CLAYERED_ITERATE_PREV);
    WT_ERR(__cursor_copy_release(cursor));
    WT_ERR(__cursor_needkey(cursor));
    WT_ERR(__cursor_needvalue(cursor));
    WT_ERR(__clayered_enter(clayered,
      F_ISSET(cursor, WT_CURSTD_OVERWRITE) ? WTI_CLAYERED_MODE_WRITE_OVERWRITE :
                                             WTI_CLAYERED_MODE_WRITE,
      &op));

    WT_ERR(__clayered_modify_check(session, clayered, &cursor->key));

    if (__clayered_needs_pre_lookup(&op)) {
        WT_ERR(__clayered_lookup(&op, &value));
        /*
         * Copy the key out, since the insert resets non-primary chunk cursors which our lookup may
         * have landed on.
         */
        WT_ERR(__cursor_needkey(cursor));
    }

    /* FIXME-WT-17933: on the leader this encodes into the stable table. */
    WT_ERR(__clayered_deleted_encode(session, &cursor->value, &value, &buf));
    WT_ERR(__clayered_put(&op, &cursor->key, &value, WTI_CLAYERED_PUT_UPDATE));

    /*
     * Set the cursor to reference the internal key/value of the positioned cursor.
     */
    F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
    WT_ITEM_SET(cursor->key, clayered->current_cursor->key);
    WT_ITEM_SET(cursor->value, clayered->current_cursor->value);
    WT_ASSERT(session, F_MASK(clayered->current_cursor, WT_CURSTD_KEY_SET) == WT_CURSTD_KEY_INT);
    WT_ASSERT(
      session, F_MASK(clayered->current_cursor, WT_CURSTD_VALUE_SET) == WT_CURSTD_VALUE_INT);
    F_SET(cursor, WT_CURSTD_KEY_INT | WT_CURSTD_VALUE_INT);

    WT_STAT_CONN_DSRC_INCR(session, layered_curs_update);

err:
    __wt_scr_free(session, &buf);
    __clayered_leave(clayered);
    CURSOR_UPDATE_API_END(session, ret);
    return (ret);
}

/*
 * __clayered_remove --
 *     WT_CURSOR->remove method for the layered cursor type.
 */
static int
__clayered_remove(WT_CURSOR *cursor)
{
    WTI_CLAYERED_OP op;
    WTI_CURSOR_LAYERED *clayered;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    bool positioned;

    clayered = (WTI_CURSOR_LAYERED *)cursor;

    /* Remember if the cursor is currently positioned. */
    positioned = F_ISSET(cursor, WT_CURSTD_KEY_INT);

    CURSOR_REMOVE_API_CALL(cursor, session, ret, clayered->dhandle);
    /*
     * Remove keeps the cursor positioned. Retain the iteration flags if we are in the middle of a
     * cursor traversal.
     */
    if (!positioned)
        F_CLR(clayered, WTI_CLAYERED_ITERATE_NEXT | WTI_CLAYERED_ITERATE_PREV);
    WT_ERR(__cursor_copy_release(cursor));
    WT_ERR(__cursor_needkey(cursor));
    __cursor_novalue(cursor);

    WT_ERR(__clayered_enter(clayered,
      F_ISSET(cursor, WT_CURSTD_OVERWRITE) ? WTI_CLAYERED_MODE_WRITE_OVERWRITE :
                                             WTI_CLAYERED_MODE_WRITE,
      &op));

    CURSOR_API_CHECK_SYSTEM_OVERLOAD(session, ret);

    /*
     * Copy the key out, since the insert resets non-primary chunk cursors which our lookup may have
     * landed on.
     */
    WT_ERR(__cursor_needkey(cursor));
    WT_ERR(__clayered_remove_int(&op, &cursor->key, positioned));

    /*
     * If the cursor was positioned, it stays positioned with a key but no value, otherwise, there's
     * no position, key or value. This isn't just cosmetic, without a reset, iteration on this
     * cursor won't start at the beginning/end of the table.
     */
    F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
    if (positioned)
        F_SET(cursor, WT_CURSTD_KEY_INT);
    else
        WT_TRET(__clayered_reset_cursors(clayered, false));
    WT_STAT_CONN_DSRC_INCR(session, layered_curs_remove);

err:
    __clayered_leave(clayered);
    CURSOR_UPDATE_API_END(session, ret);
    return (ret);
}

/*
 * __clayered_reserve --
 *     WT_CURSOR->reserve method for the layered cursor type.
 */
static int
__clayered_reserve(WT_CURSOR *cursor)
{
    WTI_CLAYERED_OP op;
    WTI_CURSOR_LAYERED *clayered;
    WT_DECL_RET;
    WT_ITEM value;
    WT_SESSION_IMPL *session;

    WT_CLEAR(value);
    clayered = (WTI_CURSOR_LAYERED *)cursor;

    CURSOR_UPDATE_API_CALL(cursor, session, ret, reserve, clayered->dhandle);

    CURSOR_API_CHECK_SYSTEM_OVERLOAD(session, ret);

    /*
     * Since a search will be performed afterward that clears the iteration flags, no point to
     * retain the flags.
     */
    F_CLR(clayered, WTI_CLAYERED_ITERATE_NEXT | WTI_CLAYERED_ITERATE_PREV);
    WT_ERR(__cursor_copy_release(cursor));
    WT_ERR(__cursor_needkey(cursor));
    __cursor_novalue(cursor);
    WT_ERR(__wt_txn_context_check(session, true));

    WT_ERR(__clayered_enter(clayered, WTI_CLAYERED_MODE_WRITE, &op));

    WT_ERR(__clayered_modify_check(session, clayered, &cursor->key));

    /*
     * WT_CURSOR.reserve is update-without-overwrite so we should check whether the key exists. With
     * no ingest cursor the stable cursor reserves without overwrite and runs the check itself.
     */
    if (op.ingest != NULL)
        WT_ERR(__clayered_lookup(&op, &value));

    WT_ERR(__clayered_put(&op, &cursor->key, NULL, WTI_CLAYERED_PUT_RESERVE));

err:
    __clayered_leave(clayered);
    CURSOR_UPDATE_API_END(session, ret);

    /*
     * The application might do a WT_CURSOR.get_value call when we return, so we need a value and
     * the underlying functions didn't set one up. For various reasons, those functions may not have
     * done a search and any previous value in the cursor might race with WT_CURSOR.reserve (and in
     * cases like layered tables, the reserve never encountered the original key). For simplicity,
     * repeat the search here.
     */
    return (ret == 0 ? cursor->search(cursor) : ret);
}

/*
 * __clayered_largest_key --
 *     WT_CURSOR->largest_key implementation for layered tables.
 */
static int
__clayered_largest_key(WT_CURSOR *cursor)
{
    WTI_CLAYERED_OP op;
    WT_CURSOR *c_larger, *c_ingest, *c_stable;
    WTI_CURSOR_LAYERED *clayered;
    WT_DECL_ITEM(key);
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    int cmp;
    bool ingest_found, stable_found;

    clayered = (WTI_CURSOR_LAYERED *)cursor;
    ingest_found = stable_found = false;

    CURSOR_API_CALL(cursor, session, ret, largest_key, clayered->dhandle);
    F_CLR(clayered, WTI_CLAYERED_ITERATE_NEXT | WTI_CLAYERED_ITERATE_PREV);
    __cursor_novalue(cursor);
    WT_ERR(__cursor_copy_release(cursor));
    WT_ERR(__clayered_enter(clayered, WTI_CLAYERED_MODE_SCAN, &op));

    c_ingest = op.ingest;
    c_stable = op.stable;

    WT_ERR(__wt_scr_alloc(session, 0, &key));

    if (c_ingest != NULL) {
        WT_ERR_NOTFOUND_OK(c_ingest->largest_key(c_ingest), true);
        if (ret == 0)
            ingest_found = true;
    }

    if (c_stable != NULL) {
        WT_ERR_NOTFOUND_OK(c_stable->largest_key(c_stable), true);
        if (ret == 0)
            stable_found = true;
    }

    if (!ingest_found && !stable_found) {
        ret = WT_NOTFOUND;
        goto err;
    }

    if (ingest_found && !stable_found)
        c_larger = c_ingest;
    else if (!ingest_found && stable_found) {
        c_larger = c_stable;
    } else {
        if (c_stable == NULL)
            c_larger = c_ingest;
        else {
            WT_ERR(__wt_compare(session, op.collator, &c_ingest->key, &c_stable->key, &cmp));
            if (cmp <= 0)
                c_larger = c_stable;
            else
                c_larger = c_ingest;
        }
    }

    /* Copy the key as we will reset the cursor after that. */
    WT_ERR(__wt_buf_set(session, key, c_larger->key.data, c_larger->key.size));
    WT_TRET(__clayered_reset_cursors(clayered, false));
    F_CLR(cursor, WT_CURSTD_KEY_INT);
    WT_ERR(__wt_buf_set(session, &cursor->key, key->data, key->size));
    /* Set the key as external. */
    F_SET(cursor, WT_CURSTD_KEY_EXT);

err:
    __clayered_leave(clayered);
    __wt_scr_free(session, &key);
    if (ret != 0)
        WT_TRET(__clayered_reset_cursors(clayered, false));
    API_END_RET_STAT(session, ret, cursor_largest_key);
}

/*
 * __clayered_close_int --
 *     Close a layered cursor
 */
static int
__clayered_close_int(WT_CURSOR *cursor)
{
    WTI_CURSOR_LAYERED *clayered;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    bool dead;

    dead = F_ISSET(cursor, WT_CURSTD_DEAD);
    session = CUR2S(cursor);
    WT_ASSERT_ALWAYS(session, session->dhandle->type == WT_DHANDLE_TYPE_LAYERED,
      "Valid layered dhandle is required to close a cursor");
    clayered = (WTI_CURSOR_LAYERED *)cursor;

    WT_TRET(__clayered_close_cursors(clayered));

    __wt_cursor_close(cursor);

    if (session->dhandle != NULL) {
        /* Decrement the data-source's in-use counter. */
        __wt_cursor_dhandle_decr_use(session);

        /*
         * If the cursor was marked dead, we got here from reopening a cached cursor, which had a
         * handle that was dead at that time, so it did not obtain a lock on the handle.
         */
        if (!dead)
            WT_TRET(__wt_session_release_dhandle(session));
    }
    return (ret);
}

/*
 * __clayered_close --
 *     WT_CURSOR->close method for the layered cursor type.
 */
static int
__clayered_close(WT_CURSOR *cursor)
{
    WTI_CURSOR_LAYERED *clayered;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    /*
     * Don't use the normal __clayered_enter path: that is wasted work when closing, and the cursor
     * may never have been used.
     */
    clayered = (WTI_CURSOR_LAYERED *)cursor;
    CURSOR_API_CALL_PREPARE_ALLOWED(cursor, session, close, clayered->dhandle);
    WT_ERR(__cursor_copy_release(cursor));
err:
    if (ret == 0) {
        /*
         * Close constituent cursors before caching the layered cursor. A cursor-cache sweep
         * triggered during constituent close could otherwise find the layered cursor already in the
         * cache with constituent pointers still set, and double-close them.
         */
        WT_TRET(__clayered_close_cursors(clayered));

        bool released = false;
        WT_TRET(__wti_cursor_cache_release(session, cursor, &released));
        if (released)
            goto done;
    }
    /* For cached cursors, free any extra buffers retained now. */
    __wt_cursor_free_cached_memory(cursor);
    cursor->internal_uri = NULL;

    WT_TRET(__clayered_close_int(cursor));
done:
    API_END_RET(session, ret);
}

/*
 * __clayered_next_random --
 *     WT_CURSOR->next_random method for the layered cursor type.
 */
static int
__clayered_next_random(WT_CURSOR *cursor)
{
    WTI_CLAYERED_OP op;
    WT_CURSOR *c;
    WTI_CURSOR_LAYERED *clayered;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    int exact;

    clayered = (WTI_CURSOR_LAYERED *)cursor;

    CURSOR_API_CALL(cursor, session, ret, next, clayered->dhandle);
    F_CLR(clayered, WTI_CLAYERED_ITERATE_NEXT | WTI_CLAYERED_ITERATE_PREV);
    __cursor_novalue(cursor);
    WT_ERR(__cursor_copy_release(cursor));
    WT_ERR(__clayered_enter(clayered, WTI_CLAYERED_MODE_RANDOM, &op));

    /*
     * Pick a random row from stable, and fall back to ingest if stable is empty or not yet opened.
     * Followers defer the stable cursor open until the first checkpoint is picked up.
     *
     * FIXME-WT-14736: consider the relative size of ingest in the future.
     */
    c = op.stable;
    if (c != NULL)
        WT_ERR_NOTFOUND_OK(__wti_curfile_next_random(c), true);

    if (c == NULL || ret == WT_NOTFOUND) {
        c = op.ingest;
        if (c == NULL)
            WT_ERR(WT_NOTFOUND);
        WT_ERR(__wti_curfile_next_random(c));
    }

    /*
     * Promote the picked key to the layered cursor and resolve any tombstones via search_near.
     * WT_NOTFOUND here is valid: the tree has no documents visible to us.
     *
     * Copy the key into the layered cursor's own buffer because search_near below may reposition
     * the constituent and invalidate its key pointer.
     */
    F_CLR(cursor, WT_CURSTD_KEY_INT);
    WT_ERR(__wt_buf_set(session, &cursor->key, c->key.data, c->key.size));

    /* Set the key as external. */
    F_SET(cursor, WT_CURSTD_KEY_EXT);
    WT_ERR(__clayered_search_near_int(&op, &exact));

    WT_ITEM_SET(cursor->key, clayered->current_cursor->key);
    WT_ITEM_SET(cursor->value, clayered->current_cursor->value);

err:
    __clayered_leave(clayered);
    if (ret == 0) {
        __clayered_stable_read_value_stat(clayered, &cursor->value);
        __clayered_deleted_decode(session, &cursor->value);
        F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
        F_SET(cursor, WT_CURSTD_KEY_INT | WT_CURSTD_VALUE_INT);
    } else {
        WT_TRET(__clayered_reset_cursors(clayered, false));
        F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
    }

    API_END_RET(session, ret);
}

/*
 * __clayered_modify_stable --
 *     Apply a set of modifications to the stable table.
 */
static int
__clayered_modify_stable(WTI_CLAYERED_OP *op, WT_MODIFY *entries, int nentries)
{
    WTI_CURSOR_LAYERED *clayered = op->clayered;
    WT_SESSION_IMPL *session = CUR2S(clayered);
    WT_CURSOR *cursor = &clayered->iface;
    WT_CURSOR *c_stable = op->stable;
    WT_DECL_RET;
    WT_DECL_ITEM(buf);

    c_stable->set_key(c_stable, &cursor->key);
    /* It's valid to build the modify on an empty value. */
    WT_ERR_NOTFOUND_OK(c_stable->search(c_stable), true);

    /*
     * Similarly, a delete-encoded value alters the original value and also cannot serve as the base
     * value for a modify. In these cases, perform a full update instead.
     */
    if (ret == 0 && __clayered_value_in_tombstone_namespace(&c_stable->value, false /* decode */)) {
        __clayered_deleted_decode(session, &c_stable->value);
        WT_ERR(__wt_modify_apply_api(c_stable, entries, nentries));
        /* FIXME-WT-17933: this encodes into the stable table. */
        WT_ERR(__clayered_deleted_encode(session, &c_stable->value, &c_stable->value, &buf));
        __wt_clayered_stable_value_stat(session, c_stable->value.data, c_stable->value.size);
        F_SET(c_stable, WT_CURSTD_VALUE_EXT);
        WT_ERR(c_stable->update(c_stable));
    } else
        WT_ERR(c_stable->modify(c_stable, entries, nentries));

    clayered->current_cursor = c_stable;

err:
    __wt_scr_free(session, &buf);
    return (ret);
}

/*
 * __clayered_modify_ingest --
 *     Apply a set of modifications to the ingest table.
 */
static int
__clayered_modify_ingest(WTI_CLAYERED_OP *op, WT_MODIFY *entries, int nentries)
{
    WTI_CURSOR_LAYERED *clayered = op->clayered;
    WT_SESSION_IMPL *session = CUR2S(clayered);
    WT_CURSOR *cursor = &clayered->iface;
    WT_CURSOR *c_ingest = op->ingest;
    WT_DECL_RET;
    WT_DECL_ITEM(buf);
    WT_ITEM value;

    WT_CLEAR(value);

    WT_ERR(__clayered_modify_check(session, clayered, &cursor->key));

    if (!F_ISSET(&clayered->iface, WT_CURSTD_KEY_INT) ||
      !F_ISSET(&clayered->iface, WT_CURSTD_VALUE_INT))
        WT_ERR(__clayered_lookup(op, &value));
    else
        WT_ITEM_SET(value, cursor->value);

    if (clayered->current_cursor != c_ingest) {
        /*
         * Cursor is positioned on the stable table. Compute a full value and write it to the ingest
         * table.
         */
        c_ingest->set_key(c_ingest, &cursor->key);
        __clayered_deleted_decode(session, &value);
        WT_ITEM_SET(c_ingest->value, value);
        WT_ERR(__wt_modify_apply_api(c_ingest, entries, nentries));
        WT_ERR(__clayered_deleted_encode(session, &c_ingest->value, &c_ingest->value, &buf));
        F_SET(c_ingest, WT_CURSTD_VALUE_EXT);
        WT_ERR(c_ingest->update(c_ingest));
    } else {
        /*
         * A tombstone is a special value in the ingest table, so it cannot be used as a base value
         * for a modify operation. Similarly, a delete-encoded value alters the original value and
         * also cannot serve as the base value for a modify. In these cases, perform a full update
         * instead.
         */
        if (__wt_clayered_deleted(&c_ingest->value) ||
          __clayered_value_in_tombstone_namespace(&c_ingest->value, false /* decode */)) {
            __clayered_deleted_decode(session, &c_ingest->value);
            WT_ERR(__wt_modify_apply_api(c_ingest, entries, nentries));
            WT_ERR(__clayered_deleted_encode(session, &c_ingest->value, &c_ingest->value, &buf));
            F_SET(c_ingest, WT_CURSTD_VALUE_EXT);
            WT_ERR(c_ingest->update(c_ingest));
        } else
            WT_ERR(c_ingest->modify(c_ingest, entries, nentries));
    }

    /*
     * Clear the stable cursor position. Keep the cursor position if we are in the middle of a
     * cursor traversal.
     */
    if (!F_ISSET(clayered, WTI_CLAYERED_ITERATE_NEXT | WTI_CLAYERED_ITERATE_PREV))
        WT_ERR(__clayered_reset_cursors(clayered, true));
    clayered->current_cursor = c_ingest;

err:
    __wt_scr_free(session, &buf);
    if (ret != 0)
        WT_TRET(__clayered_reset_cursors(clayered, false));
    return (ret);
}

/*
 * __clayered_modify_int --
 *     Dispatch a modify call based on whether an ingest cursor is present.
 */
static int
__clayered_modify_int(WTI_CLAYERED_OP *op, WT_MODIFY *entries, int nentries)
{
    if (op->ingest == NULL)
        WT_RET(__clayered_modify_stable(op, entries, nentries));
    else
        WT_RET(__clayered_modify_ingest(op, entries, nentries));

    return (0);
}

/*
 * __clayered_modify --
 *     WT_CURSOR->modify method for layered cursors.
 */
static int
__clayered_modify(WT_CURSOR *cursor, WT_MODIFY *entries, int nentries)
{
    WTI_CLAYERED_OP op;
    WT_CURSOR *current;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    WTI_CURSOR_LAYERED *clayered = (WTI_CURSOR_LAYERED *)cursor;

    CURSOR_UPDATE_API_CALL(cursor, session, ret, modify, clayered->dhandle);

    CURSOR_API_CHECK_SYSTEM_OVERLOAD(session, ret);

    /*
     * Modify keeps the cursor positioned. Retain the iteration flags if we are in the middle of a
     * cursor traversal.
     */
    if (!F_ISSET(cursor, WT_CURSTD_KEY_INT))
        F_CLR(clayered, WTI_CLAYERED_ITERATE_NEXT | WTI_CLAYERED_ITERATE_PREV);
    WT_ERR(__cursor_copy_release(cursor));
    WT_ERR(__cursor_needkey(cursor));
    __cursor_novalue(cursor);
    WT_ERR(__clayered_enter(clayered, WTI_CLAYERED_MODE_WRITE, &op));

    /* Check for a rational modify vector count. */
    if (nentries <= 0)
        WT_ERR_MSG(session, EINVAL, "Illegal modify vector with %d entries", nentries);

    WT_ERR(__clayered_modify_int(&op, entries, nentries));

    /*
     * Set the cursor to reference the internal key/value of the positioned cursor.
     */
    current = clayered->current_cursor;
    F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);

    /*
     * Assign the new key/value to the top-level cursor.
     */
    WT_ITEM_SET(cursor->key, current->key);
    WT_ITEM_SET(cursor->value, current->value);
    __clayered_deleted_decode(session, &cursor->value);
    WT_ASSERT(session, F_MASK(current, WT_CURSTD_KEY_SET) == WT_CURSTD_KEY_INT);
    F_SET(cursor, WT_CURSTD_KEY_INT);

    WT_ASSERT(session, F_ISSET(current, WT_CURSTD_VALUE_SET));
    F_SET(cursor, F_MASK(current, WT_CURSTD_VALUE_SET));

    /*
     * Modify maintains a position, key and value. Unlike update, it's not always an internal value.
     */
    WT_ASSERT(session, F_MASK(cursor, WT_CURSTD_KEY_SET) == WT_CURSTD_KEY_INT);
    WT_ASSERT(session, F_MASK(cursor, WT_CURSTD_VALUE_SET) != 0);

    WT_STAT_CONN_DSRC_INCR(session, layered_curs_modify);

err:
    __clayered_leave(clayered);
    CURSOR_UPDATE_API_END_STAT(session, ret, cursor_modify);
    return (ret);
}

/*
 * __wt_clayered_open --
 *     WT_SESSION->open_cursor method for layered cursors.
 */
int
__wt_clayered_open(WT_SESSION_IMPL *session, const char *uri, WT_CURSOR *owner, const char *cfg[],
  WT_CURSOR **cursorp)
{
    WT_CONFIG_ITEM cval;
    WT_CURSOR_STATIC_INIT(iface, __wt_cursor_get_key, /* get-key */
      __wt_cursor_get_value,                          /* get-value */
      __wt_cursor_get_raw_key_value,                  /* get-raw-key-value */
      __wt_cursor_set_key,                            /* set-key */
      __wt_cursor_set_value,                          /* set-value */
      __clayered_compare,                             /* compare */
      __wt_cursor_equals,                             /* equals */
      __clayered_next,                                /* next */
      __clayered_prev,                                /* prev */
      __clayered_reset,                               /* reset */
      __clayered_search,                              /* search */
      __clayered_search_near,                         /* search-near */
      __clayered_insert,                              /* insert */
      __clayered_modify,                              /* modify */
      __clayered_update,                              /* update */
      __clayered_remove,                              /* remove */
      __clayered_reserve,                             /* reserve */
      __clayered_reconfigure,                         /* reconfigure */
      __clayered_largest_key,                         /* largest_key */
      __clayered_bound,                               /* bound */
      __clayered_cache,                               /* cache */
      __clayered_reopen,                              /* reopen */
      __wt_cursor_checkpoint_id,                      /* checkpoint ID */
      __clayered_close);                              /* close */
    WT_CURSOR *cursor;
    WTI_CURSOR_LAYERED *clayered;
    WT_DECL_RET;
    WT_LAYERED_TABLE *layered;
    bool cacheable;

    WT_VERIFY_OPAQUE_POINTER(WTI_CURSOR_LAYERED);

    clayered = NULL;
    cursor = NULL;
    cacheable = F_ISSET(session, WT_SESSION_CACHE_CURSORS);

    if (!WT_PREFIX_MATCH(uri, "layered:"))
        return (__wt_unexpected_object_type(session, uri, "layered:"));

    WT_RET(__wt_inmem_unsupported_op(session, "Layered trees"));

    WT_RET(__wt_config_gets_def(session, cfg, "checkpoint", 0, &cval));
    if (cval.len != 0)
        WT_RET_MSG(session, EINVAL, "Layered trees do not support opening by checkpoint");

    WT_RET(__wt_config_gets_def(session, cfg, "bulk", 0, &cval));
    if (cval.val != 0)
        WT_RET_MSG(session, EINVAL, "Layered trees do not support bulk loading");

    /* Get the layered tree, and hold a reference to it until the cursor is closed. */
    WT_RET(__wt_session_get_dhandle(session, uri, NULL, cfg, 0));

    /*
     * Increment the data-source's in-use counter; done now because closing the cursor will
     * decrement it, and all failure paths from here close the cursor.
     */
    __wt_cursor_dhandle_incr_use(session);

    layered = (WT_LAYERED_TABLE *)session->dhandle;
    WT_ASSERT_ALWAYS(session, layered->ingest_uri != NULL && layered->key_format != NULL,
      "Layered handle not setup");

    WT_ERR(__wt_calloc_one(session, &clayered));
    clayered->dhandle = session->dhandle;

    cursor = (WT_CURSOR *)clayered;
    *cursor = iface;
    cursor->session = (WT_SESSION *)session;
    cursor->internal_uri = session->dhandle->name;
    cursor->key_format = layered->key_format;
    cursor->value_format = layered->value_format;

    WT_ERR(__wt_config_gets_def(session, cfg, "next_random", 0, &cval));
    if (cval.val != 0) {
        F_SET(clayered, WTI_CLAYERED_RANDOM);
        __wti_cursor_set_notsup(cursor);
        cursor->next = __clayered_next_random;

        WT_ERR(__wt_config_gets_def(session, cfg, "next_random_seed", 0, &cval));
        clayered->next_random_seed = (uint64_t)cval.val;

        WT_ERR(__wt_config_gets_def(session, cfg, "next_random_sample_size", 0, &cval));
        clayered->next_random_sample_size = (u_int)cval.val;
        cacheable = false;
    }

    /*
     * The size summary is a debug feature measured on the active btree behind this layered cursor;
     * the in-memory ingest table is not a meaningful size target. Remember the request so that
     * btree inherits debug=(size_stats) each time it is opened or reopened, and disable caching so
     * a size-stats cursor is never handed back to an open that did not ask for it.
     */
    WT_ERR(__wt_config_gets_def(session, cfg, "debug.size_stats", 0, &cval));
    if (cval.val != 0) {
        F_SET(clayered, WTI_CLAYERED_SIZE_STAT);
        cacheable = false;
    }

    /* Set the cache flag before finding a cursor handle. */
    if (cacheable)
        F_SET(cursor, WT_CURSTD_CACHEABLE);

    /* Try to find the cursor in the cache. */
    WT_ERR(__wt_cursor_init(cursor, uri, owner, cfg, cursorp));

    if (0) {
err:
        /* Our caller expects to release the data handles if we fail. */
        clayered->dhandle = NULL;
        __wt_cursor_dhandle_decr_use(session);
        if (clayered != NULL)
            WT_TRET(__clayered_close(cursor));
        WT_TRET(__wt_session_release_dhandle(session));

        *cursorp = NULL;
    }

    return (ret);
}

#ifdef HAVE_DIAGNOSTIC
/*
 * __layered_constituent_dump --
 *     Position the given constituent btree cursor on the supplied key (if it is not already
 *     positioned) and dump its page to a file named with the given output path joined to the
 *     supplied suffix. Layered search routines only leave the constituent that satisfied the lookup
 *     positioned; the other stays unpositioned. To compare both sides at the same key during
 *     failure triage we issue a search_near here so the unpositioned constituent gets a page
 *     reference before the debug dump.
 */
static int
__layered_constituent_dump(
  WT_CURSOR *constituent, const WT_ITEM *key, const char *ofile, const char *suffix, bool *dumped)
{
    WT_CURSOR_BTREE *cbt;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    size_t len;
    int exact;
    char *path;

    *dumped = false;
    path = NULL;

    if (constituent == NULL)
        return (0);

    cbt = (WT_CURSOR_BTREE *)constituent;
    session = CUR2S(constituent);

    /*
     * If the constituent isn't currently on a page, do a search_near with the layered cursor's key
     * so we land on a page that brackets the key in this constituent's btree. search_near may
     * return WT_NOTFOUND when the key isn't present but still leaves the cursor positioned; either
     * of {0, WT_NOTFOUND} is acceptable as long as a ref is set afterwards.
     */
    if (cbt->ref == NULL && key != NULL) {
        constituent->set_key(constituent, key);
        WT_RET_NOTFOUND_OK(constituent->search_near(constituent, &exact));
    }

    if (cbt->ref == NULL)
        return (0);

    if (ofile != NULL) {
        len = strlen(ofile) + strlen(suffix) + 2;
        WT_ERR(__wt_calloc_def(session, len, &path));
        WT_ERR(__wt_snprintf(path, len, "%s.%s", ofile, suffix));
    }

    ret = __wt_debug_btree_cursor_page(constituent, path);
    if (ret == 0)
        *dumped = true;

err:
    __wt_free(session, path);
    return (ret);
}

/*
 * __wt_debug_layered_cursor_page --
 *     Dump the in-memory information for a cursor-referenced page. A layered cursor has two
 *     underlying btree cursors (ingest + stable); we dump BOTH at the layered cursor's key, issuing
 *     a search_near on whichever constituent is not currently positioned. This produces one output
 *     file per constituent (suffixed with the constituent name) so triage can compare the two sides
 *     for the same key.
 */
int
__wt_debug_layered_cursor_page(void *cursor_arg, const char *ofile)
  WT_GCC_FUNC_ATTRIBUTE((visibility("default")))
{
    WTI_CURSOR_LAYERED *clayered = (WTI_CURSOR_LAYERED *)cursor_arg;
    bool dumped_ingest, dumped_stable;

    WT_RET(__layered_constituent_dump(
      clayered->ingest_cursor, &clayered->iface.key, ofile, "ingest", &dumped_ingest));
    WT_RET(__layered_constituent_dump(
      clayered->stable_cursor, &clayered->iface.key, ofile, "stable", &dumped_stable));

    if (!dumped_ingest && !dumped_stable)
        __wt_verbose_debug1(CUR2S(cursor_arg), WT_VERB_DEFAULT,
          "%s: layered cursor has no positioned constituent to dump", clayered->iface.uri);

    return (0);
}

/*
 * __wt_debug_layered_cursor_tree_hs --
 *     Dump the in-memory information for a cursor-referenced tree's history store page. Only the
 *     stable constituent has an associated history store; ingest btrees are in-memory and have no
 *     history store, so we always dispatch to the stable cursor.
 */
int
__wt_debug_layered_cursor_tree_hs(void *cursor_arg, const char *ofile)
  WT_GCC_FUNC_ATTRIBUTE((visibility("default")))
{
    WT_CURSOR_BTREE *cbt;
    WTI_CURSOR_LAYERED *clayered = (WTI_CURSOR_LAYERED *)cursor_arg;

    cbt = (WT_CURSOR_BTREE *)clayered->stable_cursor;
    if (cbt == NULL || cbt->ref == NULL || cbt->ref->page == NULL) {
        __wt_verbose_debug1(CUR2S(cursor_arg), WT_VERB_DEFAULT,
          "%s: stable constituent not positioned, no history store dump available",
          clayered->iface.uri);
        return (0);
    }

    return (__wt_debug_btree_cursor_tree_hs(clayered->stable_cursor, ofile));
}
#endif
