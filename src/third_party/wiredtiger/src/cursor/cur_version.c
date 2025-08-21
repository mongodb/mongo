/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

#define WT_CURVERSION_METADATA_FORMAT WT_UNCHECKED_STRING(QQQQQQBBBB)
/*
 * __curversion_set_key --
 *     WT_CURSOR->set_key implementation for version cursors.
 */
static void
__curversion_set_key(WT_CURSOR *cursor, ...)
{
    WT_CURSOR *file_cursor;
    WT_CURSOR_VERSION *version_cursor;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    uint64_t flags;
    va_list ap;

    session = CUR2S(cursor);

    /* Reset the cursor every time for a new key. */
    if ((ret = cursor->reset(cursor)) != 0)
        WT_IGNORE_RET(__wt_panic(session, ret, "failed to reset cursor"));

    version_cursor = (WT_CURSOR_VERSION *)cursor;
    file_cursor = version_cursor->file_cursor;
    va_start(ap, cursor);
    flags = file_cursor->flags;
    /* Pass on the raw flag. */
    if (F_ISSET(cursor, WT_CURSTD_RAW))
        LF_SET(WT_CURSTD_RAW);
    if ((ret = __wti_cursor_set_keyv(file_cursor, flags, ap)) != 0)
        WT_IGNORE_RET(__wt_panic(session, ret, "failed to set key"));
    va_end(ap);
}

/*
 * __curversion_get_key --
 *     WT_CURSOR->get_key implementation for version cursors.
 */
static int
__curversion_get_key(WT_CURSOR *cursor, ...)
{
    WT_CURSOR *file_cursor;
    WT_CURSOR_VERSION *version_cursor;
    WT_DECL_RET;
    uint64_t flags;
    va_list ap;

    version_cursor = (WT_CURSOR_VERSION *)cursor;
    file_cursor = version_cursor->file_cursor;
    va_start(ap, cursor);
    flags = file_cursor->flags;
    /* Pass on the raw flag. */
    if (F_ISSET(cursor, WT_CURSTD_RAW))
        flags |= WT_CURSTD_RAW;
    WT_ERR(__wti_cursor_get_keyv(file_cursor, flags, ap));

err:
    va_end(ap);
    return (ret);
}

/*
 * __curversion_get_value --
 *     WT_CURSOR->get_value implementation for version cursors.
 */
static int
__curversion_get_value(WT_CURSOR *cursor, ...)
{
    WT_CURSOR *file_cursor;
    WT_CURSOR_VERSION *version_cursor;
    WT_DECL_ITEM(data);
    WT_DECL_ITEM(metadata);
    WT_DECL_PACK_VALUE(pv);
    WT_DECL_RET;
    WT_PACK pack;
    WT_SESSION_IMPL *session;
    const uint8_t *end, *p;
    va_list ap;

    version_cursor = (WT_CURSOR_VERSION *)cursor;
    file_cursor = version_cursor->file_cursor;
    va_start(ap, cursor);

    CURSOR_API_CALL(cursor, session, ret, get_value, NULL);
    WT_ERR(__cursor_checkvalue(cursor));
    WT_ERR(__cursor_checkvalue(file_cursor));

    if (F_ISSET(cursor, WT_CURSTD_RAW)) {
        /* Extract metadata and value separately as raw data. */
        metadata = va_arg(ap, WT_ITEM *);
        metadata->data = cursor->value.data;
        metadata->size = cursor->value.size;
        data = va_arg(ap, WT_ITEM *);
        data->data = file_cursor->value.data;
        data->size = file_cursor->value.size;
    } else {
        /*
         * Unpack the metadata. We cannot use the standard get value function here because variable
         * arguments cannot be partially extracted by different function calls.
         */
        WT_ASSERT(session, cursor->value.data != NULL);
        p = (uint8_t *)cursor->value.data;
        end = p + cursor->value.size;

        WT_ERR(__pack_init(session, &pack, WT_CURVERSION_METADATA_FORMAT));
        while ((ret = __pack_next(&pack, &pv)) == 0) {
            WT_ERR(__unpack_read(session, &pv, &p, (size_t)(end - p)));
            WT_UNPACK_PUT(session, pv, ap);
        }
        WT_ERR_NOTFOUND_OK(ret, false);

        WT_ASSERT(session, p <= end);
        WT_ERR(__wti_cursor_get_valuev(file_cursor, ap));
    }

err:
    va_end(ap);
    API_END_RET(session, ret);
}

/*
 * __curversion_set_value_with_format --
 *     Set version cursor value with the given format.
 */
static int
__curversion_set_value_with_format(WT_CURSOR *cursor, const char *fmt, ...)
{
    WT_DECL_RET;
    va_list ap;

    va_start(ap, fmt);
    ret = __wti_cursor_set_valuev(cursor, fmt, ap);
    va_end(ap);

    return (ret);
}

/*
 * __curversion_next_single_key --
 *     Iterate the updates of a single key.
 */
static int
__curversion_next_single_key(WT_CURSOR *cursor)
{
    WT_CURSOR *file_cursor, *hs_cursor;
    WT_CURSOR_BTREE *cbt;
    WT_CURSOR_VERSION *version_cursor;
    WT_DECL_ITEM(hs_value);
    WT_DECL_ITEM(key);
    WT_DECL_RET;
    WT_PAGE *page;
    WT_SESSION_IMPL *session;
    WT_TIME_WINDOW *twp;
    WT_UPDATE *first_globally_visible, *next_upd, *tombstone, *upd;
    wt_timestamp_t durable_start_ts, durable_stop_ts, stop_prepare_ts, stop_ts;
    size_t max_memsize;
    uint64_t hs_upd_type, raw, stop_txn;
    uint8_t *p, prepare_state;
    bool stop_prepared, upd_found, version_prepared;

    session = CUR2S(cursor);
    version_cursor = (WT_CURSOR_VERSION *)cursor;
    file_cursor = version_cursor->file_cursor;
    hs_cursor = version_cursor->hs_cursor;
    cbt = (WT_CURSOR_BTREE *)file_cursor;
    page = cbt->ref->page;
    twp = NULL;
    upd_found = false;
    first_globally_visible = tombstone = upd = NULL;

    /* Temporarily clear the raw flag. We need to pack the data according to the format. */
    raw = F_MASK(cursor, WT_CURSTD_RAW);
    F_CLR(cursor, WT_CURSTD_RAW);

    /* The cursor should be positioned, otherwise the next call will fail. */
    if (!F_ISSET(file_cursor, WT_CURSTD_KEY_INT))
        WT_ERR_SUB(session, WT_ROLLBACK, WT_NONE,
          "rolling back version_cursor->next due to no initial position");

    if (!F_ISSET(version_cursor, WT_CURVERSION_UPDATE_EXHAUSTED)) {
        upd = version_cursor->next_upd;

        if (upd == NULL) {
            version_cursor->next_upd = NULL;
            F_SET(version_cursor, WT_CURVERSION_UPDATE_EXHAUSTED);
        } else {
            if (version_cursor->start_timestamp != WT_TS_NONE &&
              upd->upd_durable_ts <= version_cursor->start_timestamp)
                goto done;

            if (upd->type == WT_UPDATE_TOMBSTONE) {
                tombstone = upd;

                /*
                 * If the update is a tombstone, we still want to record the stop information but we
                 * also need traverse to the next update to get the full value. If the tombstone was
                 * the last update in the update list, retrieve the ondisk value.
                 */
                WT_ACQUIRE_READ_WITH_BARRIER(prepare_state, upd->prepare_state);
                version_prepared =
                  prepare_state == WT_PREPARE_INPROGRESS || prepare_state == WT_PREPARE_LOCKED;
                version_cursor->upd_stop_txnid = upd->txnid;
                version_cursor->upd_durable_stop_ts = upd->upd_durable_ts;
                version_cursor->upd_stop_ts = upd->upd_start_ts;
                version_cursor->upd_stop_prepare_ts = upd->prepare_ts;
                version_cursor->upd_stop_prepared = version_prepared;

                /* No need to check the next update if the tombstone is globally visible. */
                if (__wt_txn_upd_visible_all(session, upd))
                    upd = NULL;
                else
                    upd = upd->next;

                /* Make sure the next update is not an aborted update. */
                while (upd != NULL && upd->txnid == WT_TXN_ABORTED)
                    upd = upd->next;
            }

            if (upd == NULL) {
                version_cursor->next_upd = NULL;
                F_SET(version_cursor, WT_CURVERSION_UPDATE_EXHAUSTED);
            } else {
                WT_ACQUIRE_READ_WITH_BARRIER(prepare_state, upd->prepare_state);
                version_prepared =
                  prepare_state == WT_PREPARE_INPROGRESS || prepare_state == WT_PREPARE_LOCKED;

                /*
                 * Copy the update value into the version cursor as we don't know the value format.
                 * If the update is a modify, reconstruct the value.
                 */
                if (upd->type != WT_UPDATE_MODIFY)
                    __wt_upd_value_assign(cbt->upd_value, upd);
                else
                    WT_ERR(__wt_modify_reconstruct_from_upd_list(
                      session, cbt, upd, cbt->upd_value, WT_OPCTX_TRANSACTION));

                /*
                 * Set the version cursor's value, which also contains all the record metadata for
                 * that particular version of the update.
                 */
                WT_ERR(__curversion_set_value_with_format(cursor, WT_CURVERSION_METADATA_FORMAT,
                  upd->txnid, version_prepared ? upd->prepare_ts : upd->upd_start_ts,
                  upd->upd_durable_ts, version_cursor->upd_stop_txnid,
                  version_cursor->upd_stop_prepared ? version_cursor->upd_stop_prepare_ts :
                                                      version_cursor->upd_stop_ts,
                  version_cursor->upd_durable_stop_ts, upd->type, version_prepared, upd->flags,
                  WT_CURVERSION_UPDATE_CHAIN));

                version_cursor->upd_stop_txnid = upd->txnid;
                version_cursor->upd_durable_stop_ts = upd->upd_durable_ts;
                version_cursor->upd_stop_ts = upd->upd_start_ts;
                version_cursor->upd_stop_prepare_ts = upd->prepare_ts;
                version_cursor->upd_stop_prepared = version_prepared;

                upd_found = true;

                /* Walk to the next non-obsolete update. */
                for (next_upd = upd; next_upd != NULL; next_upd = next_upd->next) {
                    if (next_upd->txnid == WT_TXN_ABORTED)
                        continue;

                    if (first_globally_visible != NULL) {
                        next_upd = NULL;
                        break;
                    }

                    /* We have traversed all the non-obsolete updates. */
                    if ((F_ISSET(version_cursor, WT_CURVERSION_TIMESTAMP_ORDER) ||
                          WT_UPDATE_DATA_VALUE(next_upd)) &&
                      __wt_txn_upd_visible_all(session, next_upd))
                        first_globally_visible = next_upd;

                    if (next_upd != upd) {
                        /*
                         * If we are here, the previous update is not globally visible. We need
                         * snapshot isolation and have pinned the global timestamp when we start the
                         * version cursor.
                         */
                        WT_ASSERT(session,
                          !__wt_txn_visible_all(session, version_cursor->upd_stop_txnid,
                            version_cursor->upd_durable_stop_ts));

                        /* Ignore the update with the same transaction id and timestamps. */
                        if (F_ISSET(version_cursor, WT_CURVERSION_TIMESTAMP_ORDER) &&
                          next_upd->txnid == version_cursor->upd_stop_txnid &&
                          next_upd->prepare_ts == version_cursor->upd_stop_prepare_ts &&
                          next_upd->upd_start_ts == version_cursor->upd_stop_ts &&
                          next_upd->upd_durable_ts == version_cursor->upd_durable_stop_ts)
                            continue;

                        break;
                    }
                }
                version_cursor->next_upd = next_upd;
                if (next_upd == NULL)
                    F_SET(version_cursor, WT_CURVERSION_UPDATE_EXHAUSTED);
            }
        }
    }

    if (!upd_found && !F_ISSET(version_cursor, WT_CURVERSION_ON_DISK_EXHAUSTED)) {
        /*
         * We have already seen an update that is globally visible on the update chain. No need to
         * return more updates.
         */
        if (F_ISSET(version_cursor, WT_CURVERSION_TIMESTAMP_ORDER) &&
          !version_cursor->upd_stop_prepared &&
          __wt_txn_visible_all(
            session, version_cursor->upd_stop_txnid, version_cursor->upd_durable_stop_ts))
            goto done;

        switch (page->type) {
        case WT_PAGE_ROW_LEAF:
            if (cbt->ins != NULL) {
                F_SET(version_cursor, WT_CURVERSION_ON_DISK_EXHAUSTED);
                F_SET(version_cursor, WT_CURVERSION_HS_EXHAUSTED);
                WT_ERR(WT_NOTFOUND);
            }
            break;
        case WT_PAGE_COL_FIX:
            /*
             * If search returned an insert, we might be past the end of page in the append list, so
             * there's no on-disk value.
             */
            if (cbt->recno >= cbt->ref->ref_recno + page->entries) {
                F_SET(version_cursor, WT_CURVERSION_ON_DISK_EXHAUSTED);
                F_SET(version_cursor, WT_CURVERSION_HS_EXHAUSTED);
                WT_ERR(WT_NOTFOUND);
            }
            break;
        case WT_PAGE_COL_VAR:
            /* Empty page doesn't have any on page value. */
            if (page->entries == 0) {
                F_SET(version_cursor, WT_CURVERSION_ON_DISK_EXHAUSTED);
                F_SET(version_cursor, WT_CURVERSION_HS_EXHAUSTED);
                WT_ERR(WT_NOTFOUND);
            }
            break;
        default:
            WT_ERR(__wt_illegal_value(session, page->type));
        }

        /*
         * Get the ondisk value. It is possible to see an overflow removed value if checkpoint has
         * visited this page and freed the underlying overflow blocks. In this case, checkpoint
         * reconciliation must have also appended the value to the update chain and moved it to the
         * history store if it is not obsolete. Therefore, we should have either already returned it
         * when walking the update chain if we are not racing with checkpoint removing the overflow
         * value concurrently or we shall return it later when we scan the history store if we do
         * race with checkpoint. If it is already obsolete, there is no need for us to return it as
         * the version cursor only ensures to return values that are not obsolete. We can safely
         * ignore the overflow removed value here.
         */
        WT_ERR_ERROR_OK(
          __wt_value_return_buf(cbt, cbt->ref, &cbt->upd_value->buf, &cbt->upd_value->tw),
          WT_RESTART, true);
        if (ret == 0) {
            if (!WT_TIME_WINDOW_HAS_STOP(&cbt->upd_value->tw)) {
                if (F_ISSET(version_cursor, WT_CURVERSION_TIMESTAMP_ORDER)) {
                    /* Always skip prepared update on disk. */
                    if (WT_TIME_WINDOW_HAS_START_PREPARE(&cbt->upd_value->tw))
                        goto skip_on_page;

                    if (version_cursor->upd_stop_prepared) {
                        if (cbt->upd_value->tw.start_txn > version_cursor->upd_stop_txnid ||
                          cbt->upd_value->tw.start_ts > version_cursor->upd_stop_prepare_ts)
                            goto skip_on_page;
                    } else {
                        if (cbt->upd_value->tw.start_txn > version_cursor->upd_stop_txnid ||
                          cbt->upd_value->tw.start_ts > version_cursor->upd_stop_ts ||
                          cbt->upd_value->tw.durable_start_ts > version_cursor->upd_durable_stop_ts)
                            goto skip_on_page;
                    }

                    if (cbt->upd_value->tw.start_txn == version_cursor->upd_stop_txnid &&
                      cbt->upd_value->tw.start_prepare_ts == version_cursor->upd_stop_prepare_ts &&
                      cbt->upd_value->tw.start_ts == version_cursor->upd_stop_ts &&
                      cbt->upd_value->tw.durable_start_ts == version_cursor->upd_durable_stop_ts)
                        goto skip_on_page;
                }
                durable_stop_ts = version_cursor->upd_durable_stop_ts;
                stop_prepare_ts = version_cursor->upd_stop_prepare_ts;
                stop_ts = version_cursor->upd_stop_ts;
                stop_txn = version_cursor->upd_stop_txnid;
                stop_prepared = version_cursor->upd_stop_prepared;
            } else {
                if (F_ISSET(version_cursor, WT_CURVERSION_TIMESTAMP_ORDER)) {
                    if (__wt_txn_tw_start_visible_all(session, &cbt->upd_value->tw))
                        goto done;

                    if (WT_TIME_WINDOW_HAS_STOP_PREPARE(&cbt->upd_value->tw))
                        goto skip_on_page;

                    if (version_cursor->upd_stop_prepared) {
                        if (cbt->upd_value->tw.stop_txn > version_cursor->upd_stop_txnid ||
                          cbt->upd_value->tw.stop_ts > version_cursor->upd_stop_prepare_ts)
                            goto skip_on_page;
                    } else {
                        if (cbt->upd_value->tw.stop_txn > version_cursor->upd_stop_txnid ||
                          cbt->upd_value->tw.stop_ts > version_cursor->upd_stop_ts ||
                          cbt->upd_value->tw.durable_stop_ts > version_cursor->upd_durable_stop_ts)
                            goto skip_on_page;
                    }

                    /* The update is not visible if start equals stop. */
                    if (cbt->upd_value->tw.stop_txn == cbt->upd_value->tw.start_txn &&
                      cbt->upd_value->tw.stop_prepare_ts == cbt->upd_value->tw.start_prepare_ts &&
                      cbt->upd_value->tw.stop_ts == cbt->upd_value->tw.start_ts &&
                      cbt->upd_value->tw.durable_stop_ts == cbt->upd_value->tw.durable_start_ts)
                        goto skip_on_page;
                }
                durable_stop_ts = cbt->upd_value->tw.durable_stop_ts;
                stop_prepare_ts = cbt->upd_value->tw.stop_prepare_ts;
                stop_ts = cbt->upd_value->tw.stop_ts;
                stop_txn = cbt->upd_value->tw.stop_txn;
                stop_prepared = WT_TIME_WINDOW_HAS_STOP_PREPARE(&cbt->upd_value->tw);
            }

            if (tombstone != NULL) {
                WT_ACQUIRE_READ_WITH_BARRIER(prepare_state, tombstone->prepare_state);
                version_prepared =
                  prepare_state == WT_PREPARE_INPROGRESS || prepare_state == WT_PREPARE_LOCKED;
            } else {
                if (version_cursor->start_timestamp != WT_TS_NONE) {
                    if (WT_TIME_WINDOW_HAS_STOP(&cbt->upd_value->tw)) {
                        /*
                         * We are done if we have an on-disk stop durable timestamp that is smaller
                         * than or equal to the end timestamp.
                         */
                        if (!WT_TIME_WINDOW_HAS_STOP_PREPARE(&cbt->upd_value->tw) &&
                          cbt->upd_value->tw.durable_stop_ts <= version_cursor->start_timestamp)
                            goto done;
                    } else {
                        /*
                         * We are done if we don't have a valid on-disk stop durable timestamp and
                         * the on disk start durable timestamp is smaller than or equal to the end
                         * timestamp.
                         */
                        if (!WT_TIME_WINDOW_HAS_START_PREPARE(&cbt->upd_value->tw) &&
                          cbt->upd_value->tw.durable_start_ts <= version_cursor->start_timestamp)
                            goto done;
                    }
                }

                if (F_ISSET(version_cursor, WT_CURVERSION_VISIBLE_ONLY) &&
                  WT_TIME_WINDOW_HAS_PREPARE(&(cbt->upd_value->tw))) {
                    if (!WT_TIME_WINDOW_HAS_STOP(&cbt->upd_value->tw))
                        goto skip_on_page;

                    if (stop_txn == cbt->upd_value->tw.start_txn)
                        goto skip_on_page;

                    stop_txn = WT_TXN_MAX;
                    stop_prepare_ts = WT_TS_MAX;
                    stop_ts = WT_TS_MAX;
                    durable_stop_ts = WT_TS_NONE;
                    stop_prepared = false;
                    version_prepared = false;
                } else
                    version_prepared = WT_TIME_WINDOW_HAS_PREPARE(&(cbt->upd_value->tw));
            }

            WT_ERR(__curversion_set_value_with_format(cursor, WT_CURVERSION_METADATA_FORMAT,
              cbt->upd_value->tw.start_txn,
              WT_TIME_WINDOW_HAS_START_PREPARE(&(cbt->upd_value->tw)) ?
                cbt->upd_value->tw.start_prepare_ts :
                cbt->upd_value->tw.start_ts,
              WT_TIME_WINDOW_HAS_START_PREPARE(&(cbt->upd_value->tw)) ?
                cbt->upd_value->tw.start_prepare_ts :
                cbt->upd_value->tw.durable_start_ts,
              stop_txn, stop_prepared ? stop_prepare_ts : stop_ts, durable_stop_ts,
              WT_UPDATE_STANDARD, version_prepared, 0, WT_CURVERSION_DISK_IMAGE));

            version_cursor->upd_stop_txnid = cbt->upd_value->tw.start_txn;
            version_cursor->upd_durable_stop_ts =
              WT_TIME_WINDOW_HAS_START_PREPARE(&(cbt->upd_value->tw)) ?
              cbt->upd_value->tw.start_prepare_ts :
              cbt->upd_value->tw.durable_start_ts;
            version_cursor->upd_stop_ts = WT_TIME_WINDOW_HAS_START_PREPARE(&(cbt->upd_value->tw)) ?
              cbt->upd_value->tw.start_prepare_ts :
              cbt->upd_value->tw.start_ts;

            upd_found = true;
        } else
            ret = 0;
skip_on_page:
        F_SET(version_cursor, WT_CURVERSION_ON_DISK_EXHAUSTED);
    }

    if (!upd_found && version_cursor->hs_cursor != NULL &&
      !F_ISSET(version_cursor, WT_CURVERSION_HS_EXHAUSTED)) {
        /*
         * We have already seen an update that is globally visible on the update chain. No need to
         * return more updates.
         */
        if (F_ISSET(version_cursor, WT_CURVERSION_TIMESTAMP_ORDER) &&
          !version_cursor->upd_stop_prepared &&
          __wt_txn_visible_all(
            session, version_cursor->upd_stop_txnid, version_cursor->upd_durable_stop_ts))
            goto done;

        /* Ensure we can see all the content in the history store. */
        F_SET(hs_cursor, WT_CURSTD_HS_READ_COMMITTED);

        if (!F_ISSET(hs_cursor, WT_CURSTD_KEY_INT)) {
            if (page->type == WT_PAGE_ROW_LEAF)
                hs_cursor->set_key(
                  hs_cursor, 4, S2BT(session)->id, &file_cursor->key, WT_TS_MAX, UINT64_MAX);
            else {
                /* Ensure enough room for a column-store key without checking. */
                WT_ERR(__wt_scr_alloc(session, WT_INTPACK64_MAXSIZE, &key));

                p = key->mem;
                WT_ERR(__wt_vpack_uint(&p, 0, cbt->recno));
                key->size = WT_PTRDIFF(p, key->data);
                hs_cursor->set_key(hs_cursor, 4, S2BT(session)->id, key, WT_TS_MAX, UINT64_MAX);
            }
            WT_ERR(__wt_curhs_search_near_before(session, hs_cursor));
        } else
            WT_ERR(hs_cursor->prev(hs_cursor));

        WT_ERR(__wt_scr_alloc(session, 0, &hs_value));

        /*
         * If there are no history store records for the given key or if we have iterated through
         * all the records already, we have exhausted the history store.
         */
        WT_ASSERT(session, ret == 0);

        for (;;) {
            __wt_hs_upd_time_window(hs_cursor, &twp);
            WT_ERR(hs_cursor->get_value(
              hs_cursor, &durable_stop_ts, &durable_start_ts, &hs_upd_type, hs_value));

            /*
             * Reconstruct the history store value if needed. Since we save the value inside the
             * version cursor every time we traverse a version, we can simply apply the modify onto
             * the latest value.
             */
            if (hs_upd_type == WT_UPDATE_MODIFY) {
                __wt_modify_max_memsize_format(hs_value->data, file_cursor->value_format,
                  cbt->upd_value->buf.size, &max_memsize);
                WT_ERR(__wt_buf_set_and_grow(session, &cbt->upd_value->buf,
                  cbt->upd_value->buf.data, cbt->upd_value->buf.size, max_memsize));
                WT_ERR(__wt_modify_apply_item(
                  session, file_cursor->value_format, &cbt->upd_value->buf, hs_value->data));
            } else {
                WT_ASSERT(session, hs_upd_type == WT_UPDATE_STANDARD);
                cbt->upd_value->buf.data = hs_value->data;
                cbt->upd_value->buf.size = hs_value->size;
            }

            if (!F_ISSET(version_cursor, WT_CURVERSION_TIMESTAMP_ORDER))
                break;

            /* Skip all the updates that are duplicate to the previous updates returned. */
            if (twp->stop_txn <= version_cursor->upd_stop_txnid &&
              twp->stop_ts <= version_cursor->upd_stop_ts &&
              twp->durable_stop_ts <= version_cursor->upd_durable_stop_ts)
                break;

            WT_ERR(hs_cursor->prev(hs_cursor));
        }

        if (version_cursor->start_timestamp != WT_TS_NONE) {
            /*
             * We are done if the durable stop timestamp is smaller or equal to the end timestamp.
             */
            if (twp->stop_ts != WT_TS_MAX &&
              twp->durable_stop_ts <= version_cursor->start_timestamp)
                goto done;

            /*
             * TODO: for history store, it is hard to determine if the stop durable timestamp is
             * from a tombstone or the previous full value. Always return the value for now if its
             * stop durable timestamp is larger than the end timestamp.
             */
            if (twp->stop_ts == WT_TS_MAX &&
              twp->durable_start_ts <= version_cursor->start_timestamp)
                goto done;
        }

        WT_ERR(__curversion_set_value_with_format(cursor, WT_CURVERSION_METADATA_FORMAT,
          twp->start_txn,
          WT_TIME_WINDOW_HAS_START_PREPARE(twp) ? twp->start_prepare_ts : twp->start_ts,
          WT_TIME_WINDOW_HAS_START_PREPARE(twp) ? twp->start_prepare_ts : twp->durable_start_ts,
          twp->stop_txn, WT_TIME_WINDOW_HAS_STOP_PREPARE(twp) ? twp->stop_prepare_ts : twp->stop_ts,
          WT_TIME_WINDOW_HAS_STOP_PREPARE(twp) ? twp->stop_prepare_ts : twp->durable_stop_ts,
          hs_upd_type, 0, 0, WT_CURVERSION_HISTORY_STORE));

        version_cursor->upd_stop_txnid = twp->start_txn;
        version_cursor->upd_durable_stop_ts =
          WT_TIME_WINDOW_HAS_START_PREPARE(twp) ? twp->start_prepare_ts : twp->durable_start_ts;
        version_cursor->upd_stop_ts =
          WT_TIME_WINDOW_HAS_START_PREPARE(twp) ? twp->start_prepare_ts : twp->start_ts;

        upd_found = true;
    }

done:
    if (!upd_found)
        ret = WT_NOTFOUND;
    else {
        cbt->upd_value->type = WT_UPDATE_STANDARD;
        __wt_value_return(cbt, cbt->upd_value);
    }

err:
    __wt_scr_free(session, &key);
    __wt_scr_free(session, &hs_value);
    F_SET(cursor, raw);
    return (ret);
}

/*
 * __curversion_version_reset --
 *     reset the version information in the version cursor
 */
static WT_INLINE int
__curversion_version_reset(WT_CURSOR_VERSION *version_cursor)
{
    WT_CURSOR *hs_cursor;
    WT_DECL_RET;

    hs_cursor = version_cursor->hs_cursor;

    if (hs_cursor != NULL)
        WT_TRET(hs_cursor->reset(hs_cursor));
    version_cursor->next_upd = NULL;

    /* Clear the information used to track update metadata. */
    version_cursor->upd_stop_txnid = WT_TXN_MAX;
    version_cursor->upd_durable_stop_ts = WT_TS_NONE;
    version_cursor->upd_stop_ts = WT_TS_MAX;
    version_cursor->upd_stop_prepare_ts = WT_TS_MAX;
    version_cursor->upd_stop_prepared = false;

    F_CLR(version_cursor,
      WT_CURVERSION_UPDATE_EXHAUSTED | WT_CURVERSION_ON_DISK_EXHAUSTED |
        WT_CURVERSION_HS_EXHAUSTED);

    return (0);
}

/*
 * __curversion_skip_starting_updates --
 *     Skip aborted and invisible updates
 */
static int
__curversion_skip_starting_updates(WT_SESSION_IMPL *session, WT_CURSOR_VERSION *version_cursor)
{
    WT_CURSOR_BTREE *cbt;
    WT_PAGE *page;
    WT_ROW *rip;
    WT_UPDATE *upd;
    uint8_t prepare_state;

    cbt = (WT_CURSOR_BTREE *)version_cursor->file_cursor;
    upd = NULL;

    /*
     * If we position on a key, set next update of the version cursor to be the first update on the
     * key if any.
     */
    page = cbt->ref->page;
    switch (page->type) {
    case WT_PAGE_ROW_LEAF:
        if (cbt->ins != NULL)
            upd = cbt->ins->upd;
        else {
            rip = &page->pg_row[cbt->slot];
            upd = WT_ROW_UPDATE(page, rip);
        }
        break;
    case WT_PAGE_COL_FIX:
    case WT_PAGE_COL_VAR:
        if (cbt->ins != NULL)
            upd = cbt->ins->upd;
        break;
    default:
        WT_RET(__wt_illegal_value(session, page->type));
    }

    for (; upd != NULL; upd = upd->next) {
        /* Skip aborted updates. */
        if (upd->txnid == WT_TXN_ABORTED)
            continue;

        if (!F_ISSET(version_cursor, WT_CURVERSION_VISIBLE_ONLY))
            break;

        /* Skip invisible updates. */
        WT_ACQUIRE_READ_WITH_BARRIER(prepare_state, upd->prepare_state);
        if (prepare_state == WT_PREPARE_INPROGRESS || prepare_state == WT_PREPARE_LOCKED)
            continue;

        if (!__txn_visible_id(session, upd->txnid))
            continue;

        break;
    }

    version_cursor->next_upd = upd;
    version_cursor->upd_stop_txnid = WT_TXN_MAX;
    version_cursor->upd_durable_stop_ts = WT_TS_NONE;
    version_cursor->upd_stop_ts = WT_TS_MAX;
    version_cursor->upd_stop_prepare_ts = WT_TS_MAX;
    version_cursor->upd_stop_prepared = false;

    if (version_cursor->next_upd == NULL)
        F_SET(version_cursor, WT_CURVERSION_UPDATE_EXHAUSTED);

    return (0);
}

/*
 * __curversion_next --
 *     WT_CURSOR->next method for version cursors. The next function will position the cursor on the
 *     next update of the key it is positioned at. We traverse through updates on the update chain,
 *     then the ondisk value, and finally from the history store.
 */
static int
__curversion_next(WT_CURSOR *cursor)
{
    WT_CURSOR *file_cursor;
    WT_CURSOR_BTREE *cbt;
    WT_CURSOR_VERSION *version_cursor;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    version_cursor = (WT_CURSOR_VERSION *)cursor;
    file_cursor = version_cursor->file_cursor;
    cbt = (WT_CURSOR_BTREE *)file_cursor;

    CURSOR_API_CALL(
      cursor, session, ret, next, ((WT_CURSOR_BTREE *)version_cursor->file_cursor)->dhandle);

    /* Place the cursor on the first key if it is not positioned. */
    if (!F_ISSET(file_cursor, WT_CURSTD_KEY_INT)) {
        F_SET(file_cursor, WT_CURSTD_KEY_ONLY);
        WT_ERR(__wt_btcur_next(cbt, false));
        WT_ASSERT(session, F_ISSET(file_cursor, WT_CURSTD_KEY_INT));
        WT_ERR(__curversion_skip_starting_updates(session, version_cursor));
    }

    for (;;) {
        WT_ERR_NOTFOUND_OK(__curversion_next_single_key(cursor), true);

        if (ret == 0)
            break;

        /* Move to the next key if we have visited all the versions of the current key. */
        WT_ERR(__curversion_version_reset(version_cursor));
        WT_ASSERT(session, F_ISSET(file_cursor, WT_CURSTD_KEY_INT));
        F_SET(file_cursor, WT_CURSTD_KEY_ONLY);
        WT_ERR(__wt_btcur_next(cbt, false));
        WT_ASSERT(session, F_ISSET(file_cursor, WT_CURSTD_KEY_INT));
        WT_ERR(__curversion_skip_starting_updates(session, version_cursor));
    };

err:
    if (ret != 0)
        WT_TRET(cursor->reset(cursor));
    API_END_RET(session, ret);
}

/*
 * __curversion_reset --
 *     WT_CURSOR::reset for version cursors.
 */
static int
__curversion_reset(WT_CURSOR *cursor)
{
    WT_CURSOR *file_cursor;
    WT_CURSOR_VERSION *version_cursor;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    version_cursor = (WT_CURSOR_VERSION *)cursor;
    file_cursor = version_cursor->file_cursor;
    CURSOR_API_CALL(cursor, session, ret, reset, NULL);

    if (file_cursor != NULL)
        WT_TRET(file_cursor->reset(file_cursor));
    WT_TRET(__curversion_version_reset(version_cursor));
    F_CLR(cursor, WT_CURSTD_KEY_SET);
    F_CLR(cursor, WT_CURSTD_VALUE_SET);

err:
    API_END_RET(session, ret);
}

/*
 * __curversion_search --
 *     WT_CURSOR->search method for version cursors.
 */
static int
__curversion_search(WT_CURSOR *cursor)
{
    WT_CURSOR *file_cursor;
    WT_CURSOR_BTREE *cbt;
    WT_CURSOR_VERSION *version_cursor;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    WT_TXN *txn;

    version_cursor = (WT_CURSOR_VERSION *)cursor;
    file_cursor = version_cursor->file_cursor;
    cbt = (WT_CURSOR_BTREE *)file_cursor;

    CURSOR_API_CALL(cursor, session, ret, search, cbt->dhandle);
    txn = session->txn;

    /*
     * We need to run with snapshot isolation to ensure that the globally visibility does not move.
     */
    if (txn->isolation != WT_ISO_SNAPSHOT)
        WT_ERR_SUB(session, WT_ROLLBACK, WT_NONE,
          "version cursor can only be called with snapshot isolation");

    WT_ERR(__cursor_checkkey(file_cursor));
    if (F_ISSET(file_cursor, WT_CURSTD_KEY_INT))
        WT_ERR_SUB(
          session, WT_ROLLBACK, WT_NONE, "version cursor cannot be called when it is positioned");

    /* Do a search and position on the key if it is found */
    F_SET(file_cursor, WT_CURSTD_KEY_ONLY);
    WT_ERR(__wt_btcur_search(cbt));
    WT_ASSERT(session, F_ISSET(file_cursor, WT_CURSTD_KEY_INT));

    WT_ERR(__curversion_skip_starting_updates(session, version_cursor));

    /* Point to the newest version. */
    WT_ERR(__curversion_next_single_key(cursor));

err:
    if (ret != 0)
        WT_TRET(cursor->reset(cursor));
    API_END_RET(session, ret);
}

/*
 * __curversion_close --
 *     WT_CURSOR->close method for version cursors.
 */
static int
__curversion_close(WT_CURSOR *cursor)
{
    WT_CURSOR *file_cursor, *hs_cursor;
    WT_CURSOR_VERSION *version_cursor;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    version_cursor = (WT_CURSOR_VERSION *)cursor;
    hs_cursor = version_cursor->hs_cursor;
    file_cursor = version_cursor->file_cursor;
    CURSOR_API_CALL(cursor, session, ret, close, NULL);

err:
    version_cursor->next_upd = NULL;
    if (file_cursor != NULL) {
        WT_TRET(file_cursor->close(file_cursor));
        version_cursor->file_cursor = NULL;
    }
    if (hs_cursor != NULL) {
        WT_TRET(hs_cursor->close(hs_cursor));
        version_cursor->hs_cursor = NULL;
    }
    __wt_free(session, cursor->value_format);
    __wt_cursor_close(cursor);
    __wt_atomic_sub32(&S2C(session)->version_cursor_count, 1);

    API_END_RET(session, ret);
}

/*
 * __wt_curversion_open --
 *     Initialize a version cursor.
 */
int
__wt_curversion_open(WT_SESSION_IMPL *session, const char *uri, WT_CURSOR *owner, const char *cfg[],
  WT_CURSOR **cursorp)
{
    WT_CURSOR_STATIC_INIT(iface, __curversion_get_key, /* get-key */
      __curversion_get_value,                          /* get-value */
      __wti_cursor_get_raw_key_value_notsup,           /* get-raw-key-value */
      __curversion_set_key,                            /* set-key */
      __wti_cursor_set_value_notsup,                   /* set-value */
      __wti_cursor_compare_notsup,                     /* compare */
      __wti_cursor_equals_notsup,                      /* equals */
      __curversion_next,                               /* next */
      __wt_cursor_notsup,                              /* prev */
      __curversion_reset,                              /* reset */
      __curversion_search,                             /* search */
      __wt_cursor_search_near_notsup,                  /* search-near */
      __wt_cursor_notsup,                              /* insert */
      __wt_cursor_modify_notsup,                       /* modify */
      __wt_cursor_notsup,                              /* update */
      __wt_cursor_notsup,                              /* remove */
      __wt_cursor_notsup,                              /* reserve */
      __wt_cursor_config_notsup,                       /* reconfigure */
      __wt_cursor_notsup,                              /* largest_key */
      __wt_cursor_config_notsup,                       /* bound */
      __wt_cursor_notsup,                              /* cache */
      __wt_cursor_reopen_notsup,                       /* reopen */
      __wt_cursor_checkpoint_id,                       /* checkpoint ID */
      __curversion_close);                             /* close */

    WT_BTREE *file_btree;
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_IMPL *conn;
    WT_CURSOR *cursor;
    WT_CURSOR_VERSION *version_cursor;
    WT_DECL_RET;
    WT_TXN_GLOBAL *txn_global;
    wt_timestamp_t pinned_ts;
    /* The file cursor is read only. */
    const char *file_cursor_cfg[] = {
      WT_CONFIG_BASE(session, WT_SESSION_open_cursor), "read_only=true", NULL};
    char *version_cursor_value_format;
    size_t format_len;

    conn = S2C(session);
    txn_global = &conn->txn_global;
    *cursorp = NULL;
    WT_RET(__wt_calloc_one(session, &version_cursor));
    cursor = (WT_CURSOR *)version_cursor;
    *cursor = iface;
    cursor->session = (WT_SESSION *)session;
    version_cursor_value_format = NULL;

    /* Freeze pinned timestamp when we open the first version cursor. */
    __wt_writelock(session, &txn_global->rwlock);
    if (conn->version_cursor_count == 0) {
        __wt_txn_pinned_timestamp(session, &pinned_ts);
        txn_global->version_cursor_pinned_timestamp = pinned_ts;
    }
    (void)__wt_atomic_add32(&conn->version_cursor_count, 1);
    __wt_writeunlock(session, &txn_global->rwlock);

    /* Open the file cursor to check the key and value format. */
    WT_ERR(__wt_open_cursor(session, uri, NULL, file_cursor_cfg, &version_cursor->file_cursor));
    cursor->key_format = version_cursor->file_cursor->key_format;
    format_len =
      strlen(WT_CURVERSION_METADATA_FORMAT) + strlen(version_cursor->file_cursor->value_format) + 1;
    WT_ERR(__wt_malloc(session, format_len, &version_cursor_value_format));
    WT_ERR(__wt_snprintf(version_cursor_value_format, format_len, "%s%s",
      WT_CURVERSION_METADATA_FORMAT, version_cursor->file_cursor->value_format));
    cursor->value_format = version_cursor_value_format;
    version_cursor_value_format = NULL;

    WT_ERR(__wt_strdup(session, uri, &cursor->uri));
    WT_ERR(__wt_cursor_init(cursor, cursor->uri, owner, cfg, cursorp));

    /* Reopen the file cursor with the version cursor as owner. */
    WT_ERR(version_cursor->file_cursor->close(version_cursor->file_cursor));
    WT_ERR(__wt_open_cursor(session, uri, cursor, file_cursor_cfg, &version_cursor->file_cursor));

    /* Open the history store cursor for btrees that may have data in the history store.*/
    file_btree = CUR2BT(version_cursor->file_cursor);
    if (F_ISSET_ATOMIC_32(conn, WT_CONN_HS_OPEN) && !F_ISSET(file_btree, WT_BTREE_IN_MEMORY)) {
        WT_ERR(__wt_curhs_open(session, file_btree->id, cursor, &version_cursor->hs_cursor));
        F_SET(version_cursor->hs_cursor, WT_CURSTD_HS_READ_COMMITTED);
    }

    /* Initialize information used to track update metadata. */
    version_cursor->upd_stop_txnid = WT_TXN_MAX;
    version_cursor->upd_durable_stop_ts = WT_TS_MAX;
    version_cursor->upd_stop_ts = WT_TS_MAX;

    WT_ERR_NOTFOUND_OK(
      __wt_config_gets_def(session, cfg, "debug.dump_version.visible_only", 0, &cval), true);
    if (ret == 0) {
        if (cval.val)
            F_SET(version_cursor, WT_CURVERSION_VISIBLE_ONLY);
    }

    WT_ERR_NOTFOUND_OK(
      __wt_config_gets_def(session, cfg, "debug.dump_version.raw_key_value", 0, &cval), true);
    if (ret == 0) {
        if (cval.val)
            F_SET(version_cursor->file_cursor, WT_CURSTD_RAW);
    }

    WT_ERR_NOTFOUND_OK(
      __wt_config_gets_def(session, cfg, "debug.dump_version.timestamp_order", 0, &cval), true);
    if (ret == 0) {
        if (cval.val)
            F_SET(version_cursor, WT_CURVERSION_TIMESTAMP_ORDER);
    }

    WT_ERR_NOTFOUND_OK(
      __wt_config_gets_def(session, cfg, "debug.dump_version.start_timestamp", 0, &cval), true);
    if (ret == 0)
        WT_ERR(__wt_txn_parse_timestamp(
          session, "start timestamp", &version_cursor->start_timestamp, &cval));
    else
        ret = 0;

    version_cursor->upd_stop_txnid = WT_TXN_MAX;
    version_cursor->upd_durable_stop_ts = WT_TS_MAX;
    version_cursor->upd_stop_ts = WT_TS_MAX;

    /* Mark the cursor as version cursor for python api. */
    F_SET(cursor, WT_CURSTD_VERSION_CURSOR);

    if (0) {
err:
        __wt_free(session, version_cursor_value_format);
        WT_TRET(cursor->close(cursor));
        *cursorp = NULL;
    }
    return (ret);
}
