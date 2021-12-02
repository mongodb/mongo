/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __hs_verify_id --
 *     Verify the history store for a single btree. Given a cursor to the tree, walk all history
 *     store keys. This function assumes any caller has already opened a cursor to the history
 *     store.
 */
static int
__hs_verify_id(
  WT_SESSION_IMPL *session, WT_CURSOR *hs_cursor, WT_CURSOR_BTREE *ds_cbt, uint32_t this_btree_id)
{
    WT_DECL_ITEM(prev_key);
    WT_DECL_RET;
    WT_ITEM key;
    wt_timestamp_t hs_start_ts;
    uint64_t hs_counter;
    uint32_t btree_id;
    int cmp;

    WT_CLEAR(key);

    WT_ERR(__wt_scr_alloc(session, 0, &prev_key));

#ifndef HAVE_DIAGNOSTIC
    WT_UNUSED(this_btree_id);
#endif

    /*
     * If using standard cursors, we need to skip the non-globally visible tombstones in the data
     * table to verify the corresponding entries in the history store are too present in the data
     * store. Though this is not required currently as we are directly searching btree cursors,
     * leave it here in case we switch to standard cursors.
     */
    F_SET(&ds_cbt->iface, WT_CURSTD_IGNORE_TOMBSTONE);

    /*
     * The caller is responsible for positioning the history store cursor at the first record to
     * verify. When we return after moving to a new key the caller is responsible for keeping the
     * cursor there or deciding they're done.
     */
    for (; ret == 0; ret = hs_cursor->next(hs_cursor)) {
        /*
         * If the btree id does not match the preview one, we're done. It is up to the caller to set
         * up for the next tree and call us, if they choose. For a full history store walk, the
         * caller sends in WT_BTREE_ID_INVALID and this function will set and use the first btree id
         * it finds and will return once it walks off that tree, leaving the cursor set to the first
         * key of that new tree.
         *
         * We should never cross the btree id, assert if we do so.
         */
        WT_ERR(hs_cursor->get_key(hs_cursor, &btree_id, &key, &hs_start_ts, &hs_counter));
        WT_ASSERT(session, btree_id == this_btree_id);

        /*
         * If we have already checked against this key, keep going to the next key. We only need to
         * check the key once.
         */
        WT_ERR(__wt_compare(session, NULL, &key, prev_key, &cmp));
        if (cmp == 0)
            continue;

        WT_WITH_PAGE_INDEX(session, ret = __wt_row_search(ds_cbt, &key, false, NULL, false, NULL));
        WT_ERR(ret);

        if (ds_cbt->compare != 0) {
            F_SET(S2C(session), WT_CONN_DATA_CORRUPTION);
            WT_ERR_PANIC(session, WT_PANIC,
              "the associated history store key %s was not found in the data store %s",
              __wt_buf_set_printable(session, key.data, key.size, false, prev_key),
              session->dhandle->name);
        }

        WT_ERR(__cursor_reset(ds_cbt));

        /*
         * Copy the key memory into our scratch buffer. The key will get invalidated on our next
         * cursor iteration.
         */
        WT_ERR(__wt_buf_set(session, prev_key, key.data, key.size));
    }
    WT_ERR_NOTFOUND_OK(ret, true);
err:
    F_CLR(&ds_cbt->iface, WT_CURSTD_IGNORE_TOMBSTONE);
    WT_ASSERT(session, key.mem == NULL && key.memsize == 0);
    __wt_scr_free(session, &prev_key);
    return (ret);
}

/*
 * __wt_hs_verify_one --
 *     Verify the history store for the btree that is set up in this session. This must be called
 *     when we are known to have exclusive access to the btree.
 */
int
__wt_hs_verify_one(WT_SESSION_IMPL *session)
{
    WT_CURSOR *hs_cursor;
    WT_CURSOR_BTREE ds_cbt;
    WT_DECL_RET;
    uint32_t btree_id;

    WT_RET(__wt_curhs_open(session, NULL, &hs_cursor));
    F_SET(hs_cursor, WT_CURSTD_HS_READ_COMMITTED);
    btree_id = S2BT(session)->id;

    hs_cursor->set_key(hs_cursor, 1, btree_id);
    WT_ERR(__wt_curhs_search_near_after(session, hs_cursor));

    /*
     * If we positioned the cursor there is something to verify.
     *
     * We are in verify and we are not able to open a standard cursor because the btree is flagged
     * as WT_BTREE_VERIFY. However, we have exclusive access to the btree so we can directly open
     * the btree cursor to work around it.
     */
    if (ret == 0) {
        __wt_btcur_init(session, &ds_cbt);
        __wt_btcur_open(&ds_cbt);
        ret = __hs_verify_id(session, hs_cursor, &ds_cbt, btree_id);
        WT_TRET(__wt_btcur_close(&ds_cbt, false));
    }

err:
    WT_TRET(hs_cursor->close(hs_cursor));
    return (ret == WT_NOTFOUND ? 0 : ret);
}

/*
 * __wt_hs_verify --
 *     Verify the history store. There can't be an entry in the history store without having the
 *     latest value for the respective key in the data store.
 */
int
__wt_hs_verify(WT_SESSION_IMPL *session)
{
    WT_CURSOR *ds_cursor, *hs_cursor;
    WT_DECL_ITEM(buf);
    WT_DECL_RET;
    WT_ITEM key;
    wt_timestamp_t hs_start_ts;
    uint64_t hs_counter;
    uint32_t btree_id;
    char *uri_data;
    bool stop;

    /* We should never reach here if working in context of the default session. */
    WT_ASSERT(session, S2C(session)->default_session != session);

    ds_cursor = hs_cursor = NULL;
    WT_CLEAR(key);
    btree_id = WT_BTREE_ID_INVALID;
    uri_data = NULL;

    WT_RET(__wt_curhs_open(session, NULL, &hs_cursor));
    F_SET(hs_cursor, WT_CURSTD_HS_READ_COMMITTED);
    WT_ERR(__wt_scr_alloc(session, 0, &buf));
    WT_ERR_NOTFOUND_OK(hs_cursor->next(hs_cursor), true);
    stop = ret == WT_NOTFOUND ? true : false;
    ret = 0;

    /*
     * We have the history store cursor positioned at the first record that we want to verify. The
     * internal function is expecting a btree cursor, so open and initialize that.
     */
    while (!stop) {
        /*
         * The cursor is positioned either from above or left over from the internal call on the
         * first key of a new btree id.
         */
        WT_ERR(hs_cursor->get_key(hs_cursor, &btree_id, &key, &hs_start_ts, &hs_counter));
        if ((ret = __wt_metadata_btree_id_to_uri(session, btree_id, &uri_data)) != 0) {
            F_SET(S2C(session), WT_CONN_DATA_CORRUPTION);
            WT_ERR_PANIC(session, ret,
              "Unable to find btree id %" PRIu32 " in the metadata file for the associated key %s",
              btree_id, __wt_buf_set_printable(session, key.data, key.size, false, buf));
        }
        WT_ERR(__wt_open_cursor(session, uri_data, NULL, NULL, &ds_cursor));
        F_SET(ds_cursor, WT_CURSOR_RAW_OK);
        ret = __hs_verify_id(session, hs_cursor, (WT_CURSOR_BTREE *)ds_cursor, btree_id);
        if (ret == WT_NOTFOUND)
            stop = true;
        WT_TRET(ds_cursor->close(ds_cursor));
        WT_ERR_NOTFOUND_OK(ret, false);
    }
err:
    __wt_scr_free(session, &buf);
    WT_ASSERT(session, key.mem == NULL && key.memsize == 0);
    __wt_free(session, uri_data);
    WT_TRET(hs_cursor->close(hs_cursor));
    return (ret);
}
