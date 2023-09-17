/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * __wt_curhs_get_btree --
 *     Convert a history store cursor to the underlying btree.
 */
static inline WT_BTREE *
__wt_curhs_get_btree(WT_CURSOR *cursor)
{
    WT_CURSOR_HS *hs_cursor;
    hs_cursor = (WT_CURSOR_HS *)cursor;

    return (CUR2BT(hs_cursor->file_cursor));
}

/*
 * __wt_curhs_get_cbt --
 *     Convert a history store cursor to the underlying btree cursor.
 */
static inline WT_CURSOR_BTREE *
__wt_curhs_get_cbt(WT_CURSOR *cursor)
{
    WT_CURSOR_HS *hs_cursor;
    hs_cursor = (WT_CURSOR_HS *)cursor;

    return ((WT_CURSOR_BTREE *)hs_cursor->file_cursor);
}

/*
 * __cursor_set_recno --
 *     The cursor value in the interface has to track the value in the underlying cursor, update
 *     them in parallel.
 */
static inline void
__cursor_set_recno(WT_CURSOR_BTREE *cbt, uint64_t v)
{
    cbt->iface.recno = cbt->recno = v;
}

/*
 * __cursor_copy_release --
 *     Release memory used by the key and value in cursor copy debug mode.
 */
static inline int
__cursor_copy_release(WT_CURSOR *cursor)
{
    if (FLD_ISSET(S2C(CUR2S(cursor))->debug_flags, WT_CONN_DEBUG_CURSOR_COPY)) {
        if (F_ISSET(cursor, WT_CURSTD_DEBUG_COPY_KEY)) {
            WT_RET(__wt_cursor_copy_release_item(cursor, &cursor->key));
            F_CLR(cursor, WT_CURSTD_DEBUG_COPY_KEY);
        }
        if (F_ISSET(cursor, WT_CURSTD_DEBUG_COPY_VALUE)) {
            WT_RET(__wt_cursor_copy_release_item(cursor, &cursor->value));
            F_CLR(cursor, WT_CURSTD_DEBUG_COPY_VALUE);
        }
    }
    return (0);
}

/*
 * __cursor_novalue --
 *     Release any cached value before an operation that could update the transaction context and
 *     free data a value is pointing to.
 */
static inline void
__cursor_novalue(WT_CURSOR *cursor)
{
    F_CLR(cursor, WT_CURSTD_VALUE_INT);
}

/*
 * __wt_cursor_bound_reset --
 *     Clear any bounds on the cursor if they are set.
 */
static inline void
__wt_cursor_bound_reset(WT_CURSOR *cursor)
{
    WT_SESSION_IMPL *session;

    session = CUR2S(cursor);

    /* Clear bounds if they are set. */
    if (WT_CURSOR_BOUNDS_SET(cursor)) {
        WT_STAT_CONN_DATA_INCR(session, cursor_bounds_reset);
        /* Clear upper bound, and free the buffer. */
        F_CLR(cursor, WT_CURSTD_BOUND_UPPER | WT_CURSTD_BOUND_UPPER_INCLUSIVE);
        __wt_buf_free(session, &cursor->upper_bound);
        WT_CLEAR(cursor->upper_bound);
        /* Clear lower bound, and free the buffer. */
        F_CLR(cursor, WT_CURSTD_BOUND_LOWER | WT_CURSTD_BOUND_LOWER_INCLUSIVE);
        __wt_buf_free(session, &cursor->lower_bound);
        WT_CLEAR(cursor->lower_bound);
    }
}

/*
 * __cursor_checkkey --
 *     Check if a key is set without making a copy.
 */
static inline int
__cursor_checkkey(WT_CURSOR *cursor)
{
    return (F_ISSET(cursor, WT_CURSTD_KEY_SET) ? 0 : __wt_cursor_kv_not_set(cursor, true));
}

/*
 * __cursor_checkvalue --
 *     Check if a value is set without making a copy.
 */
static inline int
__cursor_checkvalue(WT_CURSOR *cursor)
{
    return (F_ISSET(cursor, WT_CURSTD_VALUE_SET) ? 0 : __wt_cursor_kv_not_set(cursor, false));
}

/*
 * __wt_cursor_localkey --
 *     If the key points into the tree, get a local copy.
 */
static inline int
__wt_cursor_localkey(WT_CURSOR *cursor)
{
    if (F_ISSET(cursor, WT_CURSTD_KEY_INT)) {
        if (!WT_DATA_IN_ITEM(&cursor->key))
            WT_RET(__wt_buf_set(CUR2S(cursor), &cursor->key, cursor->key.data, cursor->key.size));
        F_CLR(cursor, WT_CURSTD_KEY_INT);
        F_SET(cursor, WT_CURSTD_KEY_EXT);
    }
    return (0);
}

/*
 * __cursor_localvalue --
 *     If the value points into the tree, get a local copy.
 */
static inline int
__cursor_localvalue(WT_CURSOR *cursor)
{
    if (F_ISSET(cursor, WT_CURSTD_VALUE_INT)) {
        if (!WT_DATA_IN_ITEM(&cursor->value))
            WT_RET(
              __wt_buf_set(CUR2S(cursor), &cursor->value, cursor->value.data, cursor->value.size));
        F_CLR(cursor, WT_CURSTD_VALUE_INT);
        F_SET(cursor, WT_CURSTD_VALUE_EXT);
    }
    return (0);
}

/*
 * __cursor_needkey --
 *     Check if we have a key set. There's an additional semantic here: if we're pointing into the
 *     tree, get a local copy of whatever we're referencing in the tree, there's an obvious race
 *     with the cursor moving and the reference.
 */
static inline int
__cursor_needkey(WT_CURSOR *cursor)
{
    WT_RET(__wt_cursor_localkey(cursor));
    return (__cursor_checkkey(cursor));
}

/*
 * __cursor_needvalue --
 *     Check if we have a value set. There's an additional semantic here: if we're pointing into the
 *     tree, get a local copy of whatever we're referencing in the tree, there's an obvious race
 *     with the cursor moving and the reference.
 */
static inline int
__cursor_needvalue(WT_CURSOR *cursor)
{
    WT_RET(__cursor_localvalue(cursor));
    return (__cursor_checkvalue(cursor));
}

/*
 * __cursor_pos_clear --
 *     Reset the cursor's location.
 */
static inline void
__cursor_pos_clear(WT_CURSOR_BTREE *cbt)
{
    /*
     * Most of the cursor's location information that needs to be set on successful return is always
     * set by a successful return, for example, we don't initialize the compare return value because
     * it's always set by the row-store search. The other stuff gets cleared here, and it's a
     * minimal set of things we need to clear. It would be a lot simpler to clear everything, but we
     * call this function a lot.
     */
    cbt->recno = WT_RECNO_OOB;

    cbt->ins = NULL;
    cbt->ins_head = NULL;
    cbt->ins_stack[0] = NULL;

    F_CLR(cbt, WT_CBT_POSITION_MASK);
}

/*
 * __cursor_enter --
 *     Activate a cursor.
 */
static inline int
__cursor_enter(WT_SESSION_IMPL *session)
{
    /*
     * If there are no other cursors positioned in the session, check whether the cache is full.
     */
    if (session->ncursors == 0)
        WT_RET(__wt_cache_eviction_check(session, false, false, NULL));
    ++session->ncursors;
    return (0);
}

/*
 * __cursor_leave --
 *     Deactivate a cursor.
 */
static inline void
__cursor_leave(WT_SESSION_IMPL *session)
{
    /* Decrement the count of active cursors in the session. */
    WT_ASSERT(session, session->ncursors > 0);
    --session->ncursors;
}

/*
 * __cursor_reset --
 *     Reset the cursor, it no longer holds any position.
 */
static inline int
__cursor_reset(WT_CURSOR_BTREE *cbt)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    cursor = &cbt->iface;
    session = CUR2S(cbt);

#ifdef HAVE_DIAGNOSTIC
    __wt_cursor_key_order_reset(cbt); /* Clear key-order checks. */
#endif
    __cursor_pos_clear(cbt);

    /* If the cursor was active, deactivate it. */
    if (F_ISSET(cbt, WT_CBT_ACTIVE)) {
        if (!WT_READING_CHECKPOINT(session))
            __cursor_leave(session);
        F_CLR(cbt, WT_CBT_ACTIVE);
    }

    /*
     * When the count of active cursors in the session goes to zero, there are no active cursors,
     * and we can release any snapshot we're holding for read committed isolation.
     */
    if (session->ncursors == 0 && !WT_READING_CHECKPOINT(session))
        __wt_txn_read_last(session);

    /* If we're not holding a cursor reference, we're done. */
    if (cbt->ref == NULL)
        return (0);

    /*
     * If we were scanning and saw a lot of deleted records on this page, try to evict the page when
     * we release it.
     *
     * A visible stop timestamp could have been treated as a tombstone and accounted in the deleted
     * count. Such a page might not have any new updates and be clean, but could benefit from
     * reconciliation getting rid of the obsolete content. Hence mark the page dirty to force it
     * through reconciliation.
     */
    if (cbt->page_deleted_count > WT_BTREE_DELETE_THRESHOLD) {
        WT_RET(__wt_page_dirty_and_evict_soon(session, cbt->ref));
        WT_STAT_CONN_INCR(session, cache_eviction_force_delete);
    }
    cbt->page_deleted_count = 0;

    /*
     * Release any page references we're holding. This can trigger eviction (for example, forced
     * eviction of big pages), so it must happen after releasing our snapshot above. Additionally,
     * there's a debug mode where an application can force the eviction in order to test or stress
     * the system. Clear the reference so we never try the release twice.
     */
    if (F_ISSET(cursor, WT_CURSTD_DEBUG_RESET_EVICT))
        WT_TRET_BUSY_OK(__wt_page_release_evict(session, cbt->ref, 0));
    else
        ret = __wt_page_release(session, cbt->ref, 0);
    cbt->ref = NULL;

    return (ret);
}

/*
 * __wt_curindex_get_valuev --
 *     Internal implementation of WT_CURSOR->get_value for index cursors
 */
static inline int
__wt_curindex_get_valuev(WT_CURSOR *cursor, va_list ap)
{
    WT_CURSOR_INDEX *cindex;
    WT_ITEM *item;
    WT_SESSION_IMPL *session;

    cindex = (WT_CURSOR_INDEX *)cursor;
    session = CUR2S(cursor);
    WT_RET(__cursor_checkvalue(cursor));

    if (F_ISSET(cursor, WT_CURSOR_RAW_OK)) {
        WT_RET(__wt_schema_project_merge(
          session, cindex->cg_cursors, cindex->value_plan, cursor->value_format, &cursor->value));
        item = va_arg(ap, WT_ITEM *);
        item->data = cursor->value.data;
        item->size = cursor->value.size;
    } else
        WT_RET(__wt_schema_project_out(session, cindex->cg_cursors, cindex->value_plan, ap));
    return (0);
}

/*
 * __wt_curtable_get_valuev --
 *     Internal implementation of WT_CURSOR->get_value for table cursors.
 */
static inline int
__wt_curtable_get_valuev(WT_CURSOR *cursor, va_list ap)
{
    WT_CURSOR *primary;
    WT_CURSOR_TABLE *ctable;
    WT_ITEM *item;
    WT_SESSION_IMPL *session;

    ctable = (WT_CURSOR_TABLE *)cursor;
    session = CUR2S(cursor);
    primary = *ctable->cg_cursors;
    WT_RET(__cursor_checkvalue(primary));

    if (F_ISSET(cursor, WT_CURSOR_RAW_OK)) {
        WT_RET(__wt_schema_project_merge(
          session, ctable->cg_cursors, ctable->plan, cursor->value_format, &cursor->value));
        item = va_arg(ap, WT_ITEM *);
        item->data = cursor->value.data;
        item->size = cursor->value.size;
    } else
        WT_RET(__wt_schema_project_out(session, ctable->cg_cursors, ctable->plan, ap));
    return (0);
}

/*
 * __wt_cursor_dhandle_incr_use --
 *     Increment the in-use counter in the cursor's data source.
 */
static inline void
__wt_cursor_dhandle_incr_use(WT_SESSION_IMPL *session)
{
    WT_DATA_HANDLE *dhandle;

    dhandle = session->dhandle;

    /* If we open a handle with a time of death set, clear it. */
    if (__wt_atomic_addi32(&dhandle->session_inuse, 1) == 1 && dhandle->timeofdeath != 0)
        dhandle->timeofdeath = 0;
}

/*
 * __wt_cursor_dhandle_decr_use --
 *     Decrement the in-use counter in the cursor's data source.
 */
static inline void
__wt_cursor_dhandle_decr_use(WT_SESSION_IMPL *session)
{
    WT_DATA_HANDLE *dhandle;

    dhandle = session->dhandle;

    /*
     * If we close a handle with a time of death set, clear it. The ordering is important: after
     * decrementing the use count, there's a chance that the data handle can be freed.
     */
    WT_ASSERT(session, dhandle->session_inuse > 0);
    if (dhandle->timeofdeath != 0 && dhandle->session_inuse == 1)
        dhandle->timeofdeath = 0;
    (void)__wt_atomic_subi32(&dhandle->session_inuse, 1);
}

/*
 * __cursor_kv_return --
 *     Return a page referenced key/value pair to the application.
 */
static inline int
__cursor_kv_return(WT_CURSOR_BTREE *cbt, WT_UPDATE_VALUE *upd_value)
{
    WT_RET(__wt_key_return(cbt));
    __wt_value_return(cbt, upd_value);

    return (0);
}

/*
 * __wt_cursor_func_init --
 *     Cursor call setup.
 */
static inline int
__wt_cursor_func_init(WT_CURSOR_BTREE *cbt, bool reenter)
{
    WT_SESSION_IMPL *session;

    session = CUR2S(cbt);

    if (reenter)
        WT_RET(__cursor_reset(cbt));

    /*
     * Any old insert position is now invalid. We rely on this being cleared to detect if a new
     * skiplist is installed after a search.
     */
    cbt->ins_stack[0] = NULL;

    /* If the transaction is idle, check that the cache isn't full. */
    WT_RET(__wt_txn_idle_cache_check(session));

    /* Activate the file cursor. */
    if (!F_ISSET(cbt, WT_CBT_ACTIVE)) {
        if (!WT_READING_CHECKPOINT(session))
            WT_RET(__cursor_enter(session));
        F_SET(cbt, WT_CBT_ACTIVE);
    }

    /*
     * If this is an ordinary transactional cursor, make sure we are set up to read.
     */
    if (!WT_READING_CHECKPOINT(session))
        __wt_txn_cursor_op(session);
    return (0);
}

/*
 * __cursor_row_slot_key_return --
 *     Return a row-store leaf page slot's key.
 */
static inline int
__cursor_row_slot_key_return(WT_CURSOR_BTREE *cbt, WT_ROW *rip, WT_CELL_UNPACK_KV *kpack)
{
    WT_CELL *cell;
    WT_ITEM *kb;
    WT_PAGE *page;
    WT_SESSION_IMPL *session;
    size_t key_size;
    uint8_t key_prefix;
    void *copy;
    const void *key_data;

    session = CUR2S(cbt);
    page = cbt->ref->page;

    kb = &cbt->iface.key;

    /*
     * The row-store key can change underfoot; explicitly take a copy.
     */
    copy = WT_ROW_KEY_COPY(rip);

    /*
     * Check for an immediately available key from an encoded or instantiated key, and if that's not
     * available, from the unpacked cell.
     */
    __wt_row_leaf_key_info(page, copy, NULL, &cell, &key_data, &key_size, &key_prefix);
    if (key_data == NULL) {
        if (__wt_cell_type(cell) != WT_CELL_KEY)
            goto slow;
        __wt_cell_unpack_kv(session, page->dsk, cell, kpack);
        key_data = kpack->data;
        key_size = kpack->size;
        key_prefix = kpack->prefix;
    }
    if (key_prefix == 0) {
        kb->data = key_data;
        kb->size = key_size;
        return (0);
    }

    /*
     * A prefix compressed key. As a cursor is running through the tree, we may have the fully-built
     * key immediately before the prefix-compressed key we want, so it's faster to build here.
     */
    if (cbt->rip_saved == NULL || cbt->rip_saved != rip - 1)
        goto slow;

    /*
     * Inline building simple prefix-compressed keys from a previous key.
     *
     * Grow the buffer as necessary as well as ensure data has been copied into local buffer space,
     * then append the suffix to the prefix already in the buffer. Don't grow the buffer
     * unnecessarily or copy data we don't need, truncate the item's CURRENT data length to the
     * prefix bytes before growing the buffer.
     */
    WT_ASSERT(session, cbt->row_key->size >= key_prefix);
    cbt->row_key->size = key_prefix;
    WT_RET(__wt_buf_grow(session, cbt->row_key, key_prefix + key_size));
    memcpy((uint8_t *)cbt->row_key->data + key_prefix, key_data, key_size);
    cbt->row_key->size = key_prefix + key_size;

    if (0) {
slow: /*
       * Call __wt_row_leaf_key_work() instead of __wt_row_leaf_key(): we already did the
       * __wt_row_leaf_key() fast-path checks inline.
       */
        WT_RET(__wt_row_leaf_key_work(session, page, rip, cbt->row_key, false));
    }

    kb->data = cbt->row_key->data;
    kb->size = cbt->row_key->size;
    cbt->rip_saved = rip;
    return (0);
}
