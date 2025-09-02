/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_modify_idempotent --
 *     Check if a modify operation is idempotent.
 */
bool
__wt_modify_idempotent(const void *modify)
{
    WT_MODIFY mod;
    size_t nentries;
    const uint8_t *p;

    /* Get the number of modify entries. */
    p = modify;
    memcpy(&nentries, p, sizeof(nentries));
    p += sizeof(nentries);

    WT_MODIFY_FOREACH_BEGIN (mod, p, nentries, 0) {
        /*
         * If the number of bytes being replaced doesn't match the number of bytes being written,
         * we're resizing and the operation isn't idempotent.
         */
        if (mod.size != mod.data.size)
            return (false);
    }
    WT_MODIFY_FOREACH_END;

    return (true);
}

/*
 * __wt_modify_pack --
 *     Pack a modify structure into a buffer.
 */
int
__wt_modify_pack(WT_CURSOR *cursor, WT_MODIFY *entries, int nentries, WT_ITEM **modifyp)
{
    WT_ITEM *modify;
    WT_SESSION_IMPL *session;
    size_t diffsz, len, *p;
    uint8_t *data;
    int i;

    session = CUR2S(cursor);
    *modifyp = NULL;

    /*
     * Build the in-memory modify value. It's the entries count, followed by the modify structure
     * offsets written in order, followed by the data (data at the end to minimize unaligned
     * reads/writes).
     */
    len = sizeof(size_t); /* nentries */
    for (i = 0, diffsz = 0; i < nentries; ++i) {
        len += 3 * sizeof(size_t);   /* WT_MODIFY fields */
        len += entries[i].data.size; /* data */
        diffsz += entries[i].size;   /* bytes touched */
    }

    WT_RET(__wt_scr_alloc(session, len, &modify));

    data = (uint8_t *)modify->mem + sizeof(size_t) + ((size_t)nentries * 3 * sizeof(size_t));
    p = modify->mem;
    *p++ = (size_t)nentries;
    for (i = 0; i < nentries; ++i) {
        *p++ = entries[i].data.size;
        *p++ = entries[i].offset;
        *p++ = entries[i].size;

        memcpy(data, entries[i].data.data, entries[i].data.size);
        data += entries[i].data.size;
    }
    modify->size = WT_PTRDIFF(data, modify->data);
    *modifyp = modify;

    /*
     * Update statistics. This is the common path called by WT_CURSOR::modify implementations.
     */
    WT_STAT_CONN_DSRC_INCR(session, cursor_modify);
    WT_STAT_CONN_DSRC_INCRV(session, cursor_modify_bytes, cursor->value.size);
    WT_STAT_CONN_DSRC_INCRV(session, cursor_modify_bytes_touch, diffsz);

    return (0);
}

/*
 * __modify_apply_one --
 *     Apply a single modify structure change to the buffer.
 */
static int
__modify_apply_one(WT_SESSION_IMPL *session, WT_ITEM *value, WT_MODIFY *modify, bool sformat)
{
    size_t data_size, offset, size;
    size_t item_offset;
    uint8_t *to;
    const uint8_t *data, *from;

    data = modify->data.data;
    data_size = modify->data.size;
    offset = modify->offset;
    size = modify->size;
    item_offset = WT_DATA_IN_ITEM(value) ? WT_PTRDIFF(value->data, value->mem) : 0;

    WT_ASSERT_ALWAYS(session,
      value->memsize >= item_offset + offset + data_size + (sformat ? 1 : 0), "buffer overflow");

    /*
     * Fast-path the common case, where we're overwriting a set of bytes that already exist in the
     * buffer.
     */
    if (value->size > offset + data_size && data_size == size) {
        memcpy((uint8_t *)value->data + offset, data, data_size);
        return (0);
    }

    /*
     * If appending bytes past the end of the value, initialize gap bytes and copy the new bytes
     * into place.
     */
    if (value->size <= offset) {
        if (value->size < offset)
            memset((uint8_t *)value->data + value->size,
              sformat ? ' ' : __wt_process.modify_pad_byte, offset - value->size);
        memcpy((uint8_t *)value->data + offset, data, data_size);
        value->size = offset + data_size;
        return (0);
    }

    /*
     * Correct the replacement size if it's nonsense, we can't replace more bytes than remain in the
     * value. (Nonsense sizes are permitted in the API because we don't want to handle the errors.)
     */
    if (value->size < offset + size)
        size = value->size - offset;

    WT_ASSERT(session, value->size + (data_size - size) + (sformat ? 1 : 0) <= value->memsize);

    if (data_size == size) { /* Overwrite */
        /* Copy in the new data. */
        memcpy((uint8_t *)value->data + offset, data, data_size);

        /*
         * The new data must overlap the buffer's end (else, we'd use the fast-path code above). Set
         * the buffer size to include the new data.
         */
        value->size = offset + data_size;
    } else { /* Shrink or grow */
        /* Move trailing data forward/backward to its new location. */
        from = (const uint8_t *)value->data + (offset + size);
        WT_ASSERT(session,
          WT_DATA_IN_ITEM(value) &&
            from + (value->size - (offset + size)) <= (uint8_t *)value->mem + value->memsize);
        to = (uint8_t *)value->data + (offset + data_size);
        WT_ASSERT(session,
          WT_DATA_IN_ITEM(value) &&
            to + (value->size - (offset + size)) <= (uint8_t *)value->mem + value->memsize);
        memmove(to, from, value->size - (offset + size));

        /* Copy in the new data. */
        memcpy((uint8_t *)value->data + offset, data, data_size);

        /*
         * Correct the size. This works because of how the C standard
         * defines unsigned arithmetic, and gcc7 complains about more
         * verbose forms:
         *
         *	if (data_size > size)
         *		value->size += (data_size - size);
         *	else
         *		value->size -= (size - data_size);
         *
         * because the branches are identical.
         */
        value->size += (data_size - size);
    }

    return (0);
}

/*
 * __modify_fast_path --
 *     Process a set of modifications, applying any that can be made in place, and check if the
 *     remaining ones are sorted and non-overlapping.
 */
static void
__modify_fast_path(WT_ITEM *value, const uint8_t *p, size_t nentries, size_t *nappliedp,
  bool *overlapp, size_t *dataszp, size_t *destszp)
{
    WT_MODIFY current, prev;
    size_t datasz, destoff;
    bool fastpath, first;

    *overlapp = true;

    datasz = destoff = 0;
    WT_CLEAR(current);
    WT_CLEAR(prev); /* [-Werror=maybe-uninitialized] */

    /*
     * If the modifications are sorted and don't overlap in the old or new values, we can do a fast
     * application of all the modifications in a single pass.
     *
     * The requirement for ordering is unfortunate, but modifications are performed in order, and
     * applications specify byte offsets based on that. In other words, byte offsets are cumulative,
     * modifications that shrink or grow the data affect subsequent modification's byte offsets.
     */
    fastpath = first = true;
    *nappliedp = 0;
    WT_MODIFY_FOREACH_BEGIN (current, p, nentries, 0) {
        datasz += current.data.size;

        if (fastpath && current.data.size == current.size &&
          current.offset + current.size <= value->size) {
            memcpy((uint8_t *)value->data + current.offset, current.data.data, current.data.size);
            ++(*nappliedp);
            continue;
        }
        fastpath = false;

        /* Step over the bytes before the current block. */
        if (first)
            destoff = current.offset;
        else {
            /* Check that entries are sorted and non-overlapping. */
            if (current.offset < prev.offset + prev.size ||
              current.offset < prev.offset + prev.data.size)
                return;
            destoff += current.offset - (prev.offset + prev.size);
        }

        /*
         * If the source is past the end of the current value, we have to deal with padding bytes.
         * Don't try to fast-path padding bytes; it's not common and adds branches to the loop
         * applying the changes.
         */
        if (current.offset + current.size > value->size)
            return;

        /*
         * If copying this block overlaps with the next one, we can't build the value in reverse
         * order.
         */
        if (current.size != current.data.size && current.offset + current.size > destoff)
            return;

        /* Step over the current modification. */
        destoff += current.data.size;

        prev = current;
        first = false;
    }
    WT_MODIFY_FOREACH_END;

    /* Step over the final unmodified block. */
    destoff += value->size - (current.offset + current.size);

    *overlapp = false;
    *dataszp = datasz;
    *destszp = destoff;
    return;
}

/*
 * __modify_apply_no_overlap --
 *     Apply a single set of WT_MODIFY changes to a buffer, where the changes are in sorted order
 *     and none of the changes overlap.
 */
static void
__modify_apply_no_overlap(WT_SESSION_IMPL *session, WT_ITEM *value, const uint8_t *p,
  size_t nentries, size_t napplied, size_t datasz, size_t destsz)
{
    WT_MODIFY current;
    size_t sz;
    uint8_t *to;
    const uint8_t *from;

    from = (const uint8_t *)value->data + value->size;
    to = (uint8_t *)value->data + destsz;
    WT_MODIFY_FOREACH_REVERSE (current, p, nentries, napplied, datasz) {
        /* Move the current unmodified block into place if necessary. */
        sz = WT_PTRDIFF(to, value->data) - (current.offset + current.data.size);
        from -= sz;
        to -= sz;
        WT_ASSERT(session, from >= (const uint8_t *)value->data && to >= (uint8_t *)value->data);
        WT_ASSERT(session, from + sz <= (const uint8_t *)value->data + value->size);

        if (to != from)
            memmove(to, from, sz);

        from -= current.size;
        to -= current.data.size;
        memcpy(to, current.data.data, current.data.size);
    }
    WT_MODIFY_FOREACH_END;

    value->size = destsz;
}

/*
 * __wt_modify_apply_item --
 *     Apply a single set of WT_MODIFY changes to a WT_ITEM buffer. This function assumes the size
 *     of the value is larger than or equal to 0 except for the string format which must be larger
 *     than 0.
 */
int
__wt_modify_apply_item(
  WT_SESSION_IMPL *session, const char *value_format, WT_ITEM *value, const void *modify)
{
    WT_MODIFY mod;
    size_t datasz, destsz, napplied, nentries;
    size_t item_offset;
    const uint8_t *p;
    bool overlap, sformat;

    /*
     * Get the number of modify entries and set a second pointer to reference the replacement data.
     */
    p = modify;
    memcpy(&nentries, p, sizeof(nentries));
    p += sizeof(nentries);

    /*
     * Modifies can only be applied on a single value field. Make sure we are not applying modifies
     * to schema with multiple value fields.
     */
    WT_ASSERT(session, value_format[1] == '\0');
    sformat = value_format[0] == 'S';

    /*
     * Decrement the size to discard the trailing nul (done after growing the buffer to ensure it
     * can be restored without further checking).
     */
    if (sformat) {
        /* string size must be larger than 0 with trailing nul. */
        WT_ASSERT(session, value->size > 0);
        --value->size;
    }

    __modify_fast_path(value, p, nentries, &napplied, &overlap, &datasz, &destsz);

    if (napplied == nentries)
        goto done;

    if (!overlap) {
        item_offset = WT_DATA_IN_ITEM(value) ? WT_PTRDIFF(value->data, value->mem) : 0;
        WT_ASSERT_ALWAYS(
          session, value->memsize >= item_offset + destsz + (sformat ? 1 : 0), "buffer overflow");

        __modify_apply_no_overlap(session, value, p, nentries, napplied, datasz, destsz);
        goto done;
    }

    WT_MODIFY_FOREACH_BEGIN (mod, p, nentries, napplied) {
        WT_RET(__modify_apply_one(session, value, &mod, sformat));
    }
    WT_MODIFY_FOREACH_END;

done: /* Restore the trailing nul. */
    if (sformat)
        ((char *)value->data)[value->size++] = '\0';

    return (0);
}

/*
 * __wt_modify_apply_api --
 *     Apply a single set of WT_MODIFY changes to a buffer, the cursor API interface.
 */
int
__wt_modify_apply_api(WT_CURSOR *cursor, WT_MODIFY *entries, int nentries)
  WT_GCC_FUNC_ATTRIBUTE((visibility("default")))
{
    WT_DECL_ITEM(modify);
    WT_DECL_RET;
    size_t max_memsize;

    WT_ERR(__wt_modify_pack(cursor, entries, nentries, &modify));

    __wt_modify_max_memsize_unpacked(
      entries, nentries, cursor->value_format, cursor->value.size, &max_memsize);

    WT_ERR(__wt_buf_set_and_grow(
      CUR2S(cursor), &cursor->value, cursor->value.data, cursor->value.size, max_memsize));

    WT_ERR(
      __wt_modify_apply_item(CUR2S(cursor), cursor->value_format, &cursor->value, modify->data));

err:
    __wt_scr_free(CUR2S(cursor), &modify);
    return (ret);
}

/*
 * __wt_modify_reconstruct_from_upd_list --
 *     Takes an in-memory modify and populates an update value with the reconstructed full value.
 */
int
__wt_modify_reconstruct_from_upd_list(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt,
  WT_UPDATE *modify, WT_UPDATE_VALUE *upd_value, WT_OP_CONTEXT context)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;
    WT_TIME_WINDOW tw;
    WT_UPDATE *upd;
    WT_UPDATE_VECTOR modifies;
    size_t base_value_size, item_offset, max_memsize;
    uint64_t txnid;
    uint8_t prepare_state;
    bool onpage_retry, check_prepare;

    WT_ASSERT(session, modify->type == WT_UPDATE_MODIFY);

    cursor = &cbt->iface;
    /* While we have a pointer to our original modify, grab this information. */
    upd_value->tw.durable_start_ts = modify->upd_durable_ts;
    upd_value->tw.start_txn = modify->txnid;
    onpage_retry = true;
    txnid = WT_TS_NONE;

    /*
     * It is possible that a read-uncommitted reader can not reconstruct a full value. This is
     * because a snapshot isolation writer can abort the updates in parallel and leave the reader
     * with an update list that does not contain enough information to reconstruct the full value.
     * It is impossible to distinguish if an aborted modify or update happened prior to the call of
     * the function, or if it is being done in parallel. In this case, we will return back to the
     * user with a rollback error.
     */
    if (context == WT_OPCTX_TRANSACTION && session->txn->isolation == WT_ISO_READ_UNCOMMITTED)
        WT_RET_SUB(session, WT_ROLLBACK, WT_MODIFY_READ_UNCOMMITTED,
          "Read-uncommitted readers do not support reconstructing a record with modifies.");

    /* Initial modifies vector initialization. */
    __wt_update_vector_init(session, &modifies);
retry:
    /* When retrying, the vector might already contain allocated memory that should be released. */
    __wt_update_vector_free(&modifies);

    check_prepare = false;
    /*
     * Handle the case that modify is a prepared update and we race with prepared rollback. This can
     * happen in reconciliation with the preserve prepared config.
     *
     * We may see a locked prepare state if we race with prepare rollback.
     */
    WT_ACQUIRE_READ(prepare_state, modify->prepare_state);
    if (prepare_state == WT_PREPARE_INPROGRESS || prepare_state == WT_PREPARE_LOCKED) {
        WT_ACQUIRE_READ(txnid, modify->txnid);
        /* The update may be already aborted. Get the saved transaction id. */
        if (txnid == WT_TXN_ABORTED)
            txnid = modify->upd_saved_txnid;

        check_prepare = true;
    }

    /* Find a complete update. */
    for (upd = modify; upd != NULL; upd = upd->next) {
        uint64_t temp_txnid;
        WT_SKIP_ABORTED_AND_SET_CHECK_PREPARED(temp_txnid, txnid, check_prepare, upd);

        if (WT_UPDATE_DATA_VALUE(upd))
            break;

        if (upd->type == WT_UPDATE_MODIFY)
            WT_ERR(__wt_update_vector_push(&modifies, upd));
    }

    /*
     * If there's no full update, the base item is the on-page item. If the update is a tombstone,
     * the base item is an empty item.
     */
    if (upd == NULL) {
        /*
         * Callers of this function set the cursor slot to an impossible value to check we don't try
         * and return on-page values when the update list should have been sufficient (which
         * happens, for example, if an update list was truncated, deleting the standard update
         * required by a previous modify update). Assert the case.
         */
        WT_ASSERT(session, cbt->slot != UINT32_MAX);

        WT_ERR_ERROR_OK(
          __wt_value_return_buf(cbt, cbt->ref, &upd_value->buf, &tw), WT_RESTART, true);

        /*
         * We race with checkpoint reconciliation removing the overflow items. Retry the read as the
         * value should now be appended to the update chain by checkpoint reconciliation.
         */
        if (onpage_retry && ret == WT_RESTART) {
            onpage_retry = false;
            goto retry;
        }

        /* We should not read overflow removed after retry. */
        WT_ASSERT(session, ret == 0);

        /*
         * Applying modifies on top of a tombstone is invalid. So if we're using the onpage value,
         * the stop time point should be unset.
         */
        WT_ASSERT(session, !WT_TIME_WINDOW_HAS_STOP(&tw));
        item_offset = WT_DATA_IN_ITEM(&upd_value->buf) ?
          WT_PTRDIFF(upd_value->buf.data, upd_value->buf.mem) :
          0;
        base_value_size = upd_value->buf.size + item_offset;
    } else {
        /* The base update must not be a tombstone. */
        WT_ASSERT(session, upd->type == WT_UPDATE_STANDARD);
        base_value_size = upd->size;
    }

    if (modifies.size > 0) {
        __wt_modifies_max_memsize(&modifies, cursor->value_format, base_value_size, &max_memsize);

        if (upd == NULL)
            WT_ERR(__wt_buf_set_and_grow(
              session, &upd_value->buf, upd_value->buf.data, upd_value->buf.size, max_memsize));
        else
            WT_ERR(
              __wt_buf_set_and_grow(session, &upd_value->buf, upd->data, upd->size, max_memsize));
    }
    /* Once we have a base item, roll forward through any visible modify updates. */
    while (modifies.size > 0) {
        __wt_update_vector_pop(&modifies, &upd);
        WT_ERR(__wt_modify_apply_item(session, cursor->value_format, &upd_value->buf, upd->data));
    }
    upd_value->type = WT_UPDATE_STANDARD;
err:
    __wt_update_vector_free(&modifies);
    return (ret);
}
