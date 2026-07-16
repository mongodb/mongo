/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * Format: txn ID, start ts, start durable ts, start prepare ts, start prepared id, stop txn ID,
 * stop ts, stop durable ts, stop prepare ts, stop prepared id, type, prepare, flags, location,
 * value.
 */
#define WT_CURVERSION_METADATA_FORMAT WT_UNCHECKED_STRING(QQQQQQQQQQBBBB)

/*
 * __curversion_is_prepare_rollback_update --
 *     True if the update is a rolled-back prepared value update.
 */
static WT_INLINE bool
__curversion_is_prepare_rollback_update(WT_UPDATE *upd)
{
    uint8_t prepare_state;

    if (upd->txnid != WT_TXN_ABORTED || upd->type == WT_UPDATE_TOMBSTONE)
        return (false);

    WT_ACQUIRE_READ_WITH_BARRIER(prepare_state, upd->prepare_state);
    return (prepare_state == WT_PREPARE_INPROGRESS || prepare_state == WT_PREPARE_LOCKED);
}

/*
 * __curversion_stop_uses_prepare_ts --
 *     True if stop timestamp should be derived from stop prepare timestamp.
 */
static WT_INLINE bool
__curversion_stop_uses_prepare_ts(WT_CURSOR_VERSION *version_cursor)
{
    return (version_cursor->upd_stop_prepared && version_cursor->upd_stop_txnid != WT_TXN_ABORTED);
}

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
 * __curversion_tombstone_next_upd --
 *     After recording stop metadata from a tombstone, advance past it to find the value to return.
 *     Handles both the normal case (skip all aborted updates) and show_prepared_rollback mode
 *     (include rolled-back prepared value updates).
 */
static WT_INLINE WT_UPDATE *
__curversion_tombstone_next_upd(
  WT_SESSION_IMPL *session, WT_CURSOR_VERSION *version_cursor, WT_UPDATE *tombstone)
{
    /* Stop at a globally visible tombstone  nothing older is relevant. */
    if (__wt_txn_upd_visible_all(session, tombstone))
        return (NULL);

    bool show_prepared_rollback = F_ISSET(version_cursor, WT_CURVERSION_SHOW_PREPARED_ROLLBACK);
    WT_UPDATE *upd = tombstone->next;

    while (upd != NULL && upd->txnid == WT_TXN_ABORTED &&
      (!show_prepared_rollback || !__curversion_is_prepare_rollback_update(upd)))
        upd = upd->next;

    return (upd);
}

/*
 * __curversion_record_stop_time_point --
 *     Save an update's metadata as the previously-returned stop state.
 */
static void
__curversion_record_stop_time_point(
  WT_CURSOR_VERSION *version_cursor, WT_UPDATE *upd, bool version_prepared)
{
    version_cursor->upd_stop_txnid = upd->txnid;
    if (upd->txnid == WT_TXN_ABORTED) {
        version_cursor->curversion_stop_rollback_ts = upd->upd_rollback_ts;
        version_cursor->curversion_stop_saved_txnid = upd->upd_saved_txnid;
    } else {
        version_cursor->curversion_durable_stop_ts = upd->upd_durable_ts;
        version_cursor->curversion_stop_ts = upd->upd_start_ts;
    }
    version_cursor->upd_stop_prepare_ts = upd->prepare_ts;
    version_cursor->upd_stop_prepared_id = upd->prepared_id;
    version_cursor->upd_stop_prepared = version_prepared;
}

/*
 * __curversion_value_return_from_upd --
 *     Pack the metadata for an update returned from the in-memory update chain.
 */
static int
__curversion_value_return_from_upd(
  WT_CURSOR *cursor, WT_CURSOR_VERSION *version_cursor, WT_UPDATE *upd, bool version_prepared)
{
    /* Aborted updates encode rollback metadata in the start slots. */
    bool aborted = (upd->txnid == WT_TXN_ABORTED);
    uint64_t start_meta = aborted ? upd->upd_saved_txnid : (uint64_t)upd->upd_start_ts;
    uint64_t durable_meta =
      aborted ? (uint64_t)upd->upd_rollback_ts : (uint64_t)upd->upd_durable_ts;

    wt_timestamp_t stop_ts = __curversion_stop_uses_prepare_ts(version_cursor) ?
      version_cursor->upd_stop_prepare_ts :
      version_cursor->curversion_stop_ts;
    uint64_t stop_txnid = version_cursor->upd_stop_txnid;
    wt_timestamp_t stop_durable_ts = version_cursor->curversion_durable_stop_ts;
    wt_timestamp_t stop_prepare_ts = version_cursor->upd_stop_prepare_ts;
    uint64_t stop_prepared_id = version_cursor->upd_stop_prepared_id;

    return (__curversion_set_value_with_format(cursor, WT_CURVERSION_METADATA_FORMAT, upd->txnid,
      start_meta, durable_meta, upd->prepare_ts, upd->prepared_id, stop_txnid, stop_ts,
      stop_durable_ts, stop_prepare_ts, stop_prepared_id, upd->type, version_prepared, upd->flags,
      WT_CURVERSION_UPDATE_CHAIN));
}

/*
 * __curversion_walk_to_next_update --
 *     Locate the next update worth returning after the one just returned. Returns NULL if the chain
 *     is exhausted or if the just-returned update was globally visible.
 */
static WT_UPDATE *
__curversion_walk_to_next_update(
  WT_SESSION_IMPL *session, WT_CURSOR_VERSION *version_cursor, WT_UPDATE *upd)
{
    WT_UPDATE *first_globally_visible = NULL;
    WT_UPDATE *next_upd;

    for (next_upd = upd; next_upd != NULL; next_upd = next_upd->next) {
        /* Skip aborted updates unless showing prepared rollbacks. */
        if (next_upd->txnid == WT_TXN_ABORTED &&
          (!F_ISSET(version_cursor, WT_CURVERSION_SHOW_PREPARED_ROLLBACK) ||
            !__curversion_is_prepare_rollback_update(next_upd)))
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
             * The previously returned update is not globally visible: snapshot isolation plus the
             * pinned global timestamp guarantee this. Aborted updates are never globally visible.
             */
            WT_ASSERT(session,
              version_cursor->upd_stop_txnid == WT_TXN_ABORTED ||
                !__wt_txn_visible_all(session, version_cursor->upd_stop_txnid,
                  version_cursor->curversion_durable_stop_ts));
            break;
        }
    }

    return (next_upd);
}

/*
 * __curversion_process_chain --
 *     Return the next version from the in-memory update chain, if any.
 */
static int
__curversion_process_chain(WT_CURSOR *cursor, WT_UPDATE **tombstonep, bool *upd_foundp, bool *donep)
{
    WT_SESSION_IMPL *session = CUR2S(cursor);
    WT_CURSOR_VERSION *version_cursor = (WT_CURSOR_VERSION *)cursor;
    WT_CURSOR_BTREE *cbt = (WT_CURSOR_BTREE *)version_cursor->file_cursor;
    WT_UPDATE *tombstone = NULL;
    uint8_t prepare_state;
    bool version_prepared;

    if (F_ISSET(version_cursor, WT_CURVERSION_UPDATE_EXHAUSTED))
        return (0);

    WT_UPDATE *upd = version_cursor->next_upd;
    if (upd == NULL) {
        version_cursor->next_upd = NULL;
        F_SET(version_cursor, WT_CURVERSION_UPDATE_EXHAUSTED);
        return (0);
    }

    if (version_cursor->start_timestamp != WT_TS_NONE && upd->upd_durable_ts != WT_TS_NONE &&
      upd->upd_durable_ts <= version_cursor->start_timestamp) {
        *donep = true;
        return (0);
    }

    if (upd->type == WT_UPDATE_TOMBSTONE) {
        tombstone = upd;

        /*
         * Record the tombstone's stop information and traverse on to a value-bearing update. If the
         * tombstone is the last update on the chain, the caller will fall back to the on-disk
         * value.
         */
        WT_ACQUIRE_READ_WITH_BARRIER(prepare_state, upd->prepare_state);
        version_prepared = !__curversion_is_prepare_rollback_update(upd) &&
          (prepare_state == WT_PREPARE_INPROGRESS || prepare_state == WT_PREPARE_LOCKED);
        __curversion_record_stop_time_point(version_cursor, upd, version_prepared);
        upd = __curversion_tombstone_next_upd(session, version_cursor, tombstone);
    }

    *tombstonep = tombstone;

    if (upd == NULL) {
        version_cursor->next_upd = NULL;
        F_SET(version_cursor, WT_CURVERSION_UPDATE_EXHAUSTED);
        return (0);
    }

    WT_ACQUIRE_READ_WITH_BARRIER(prepare_state, upd->prepare_state);
    version_prepared = !__curversion_is_prepare_rollback_update(upd) &&
      (prepare_state == WT_PREPARE_INPROGRESS || prepare_state == WT_PREPARE_LOCKED);

    /*
     * Copy the update value into the version cursor as we don't know the value format. If the
     * update is a modify, reconstruct the value.
     */
    if (upd->type != WT_UPDATE_MODIFY)
        __wt_upd_value_assign(cbt->upd_value, upd);
    else
        WT_RET(__wt_modify_reconstruct_from_upd_list(
          session, cbt, upd, cbt->upd_value, WT_OPCTX_TRANSACTION));

    /* Pack the metadata describing this version into the version cursor's value. */
    WT_RET(__curversion_value_return_from_upd(cursor, version_cursor, upd, version_prepared));

    __curversion_record_stop_time_point(version_cursor, upd, version_prepared);

    *upd_foundp = true;

    WT_UPDATE *next_upd = __curversion_walk_to_next_update(session, version_cursor, upd);
    version_cursor->next_upd = next_upd;
    if (next_upd == NULL)
        F_SET(version_cursor, WT_CURVERSION_UPDATE_EXHAUSTED);

    return (0);
}

/*
 * __curversion_disk_skip_no_stop --
 *     Decide whether to skip an on-disk record that has no stop side, when running in timestamp
 *     order mode.
 */
static bool
__curversion_disk_skip_no_stop(WT_CURSOR_VERSION *version_cursor, WT_TIME_WINDOW *tw)
{
    if (!F_ISSET(version_cursor, WT_CURVERSION_TIMESTAMP_ORDER))
        return (false);

    /* Prepared values on disk are always skipped. */
    if (WT_TIME_WINDOW_HAS_START_PREPARE(tw))
        return (true);

    /*
     * If the previous emission was a rolled-back prepared update its stop slot holds a rollback
     * timestamp instead of a real stop timestamp, so the comparison would be meaningless.
     */
    if (version_cursor->upd_stop_txnid == WT_TXN_ABORTED)
        return (false);

    wt_timestamp_t stop_ts = __curversion_stop_uses_prepare_ts(version_cursor) ?
      version_cursor->upd_stop_prepare_ts :
      version_cursor->curversion_stop_ts;
    return (tw->start_txn > version_cursor->upd_stop_txnid || tw->start_ts > stop_ts);
}

/*
 * __curversion_disk_check_with_stop --
 *     Apply the timestamp-order filter to an on-disk record that has a stop side.
 */
static void
__curversion_disk_check_with_stop(WT_SESSION_IMPL *session, WT_CURSOR_VERSION *version_cursor,
  WT_TIME_WINDOW *tw, bool *skipp, bool *donep)
{
    if (!F_ISSET(version_cursor, WT_CURVERSION_TIMESTAMP_ORDER))
        return;

    if (__wt_txn_tw_start_visible_all(session, tw))
        *donep = true;
    else if (WT_TIME_WINDOW_HAS_STOP_PREPARE(tw))
        *skipp = true;
    else {
        /* See the no-stop branch: skip the comparison after a rolled-back prepared emission. */
        if (version_cursor->upd_stop_txnid != WT_TXN_ABORTED) {
            if (__curversion_stop_uses_prepare_ts(version_cursor))
                *skipp = (tw->stop_txn > version_cursor->upd_stop_txnid ||
                  tw->stop_ts > version_cursor->upd_stop_prepare_ts);
            else
                *skipp = (tw->stop_txn > version_cursor->upd_stop_txnid ||
                  tw->stop_ts > version_cursor->curversion_stop_ts);
        }

        /* An update whose start equals stop never became visible. */
        if (!*skipp && tw->stop_txn == tw->start_txn &&
          tw->stop_prepare_ts == tw->start_prepare_ts && tw->stop_ts == tw->start_ts &&
          tw->durable_stop_ts == tw->durable_start_ts)
            *skipp = true;
    }
}

/*
 * Stop time point collected for an on-disk record. Bundled so helpers that need to inspect and
 * potentially mutate all six fields can take a single pointer instead of six separate out-params.
 */
typedef struct {
    uint64_t txn;
    wt_timestamp_t prepare_ts;
    uint64_t prepared_id;
    wt_timestamp_t ts;
    wt_timestamp_t durable_ts;
    bool prepared;
    bool version_prepared; /* true when the start side of this record is an in-progress prepare */
} WT_CURVERSION_DISK_STOP_TIME_POINT;

/*
 * __curversion_disk_finalize_prepared --
 *     Resolve the prepared-state and visibility decisions for an on-disk record, possibly mutating
 *     the stop fields when running in visible-only mode.
 */
static void
__curversion_disk_finalize_prepared(WT_CURSOR_VERSION *version_cursor, WT_TIME_WINDOW *tw,
  WT_UPDATE *tombstone, WT_CURVERSION_DISK_STOP_TIME_POINT *stopp, bool *skipp, bool *donep)
{
    if (tombstone != NULL) {
        uint8_t prepare_state;
        WT_ACQUIRE_READ_WITH_BARRIER(prepare_state, tombstone->prepare_state);
        stopp->version_prepared =
          prepare_state == WT_PREPARE_INPROGRESS || prepare_state == WT_PREPARE_LOCKED;
    } else {
        if (version_cursor->start_timestamp != WT_TS_NONE) {
            if (WT_TIME_WINDOW_HAS_STOP(tw)) {
                /* Done if the on-disk stop durable timestamp is at or before the end timestamp. */
                if (!WT_TIME_WINDOW_HAS_STOP_PREPARE(tw) &&
                  tw->durable_stop_ts <= version_cursor->start_timestamp)
                    *donep = true;
            } else if (!WT_TIME_WINDOW_HAS_START_PREPARE(tw) &&
              tw->durable_start_ts <= version_cursor->start_timestamp)
                /* No stop side: done if the start durable timestamp is past the end timestamp. */
                *donep = true;
        }

        if (!*donep) {
            if (F_ISSET(version_cursor, WT_CURVERSION_VISIBLE_ONLY) &&
              WT_TIME_WINDOW_HAS_PREPARE(tw)) {
                if (!WT_TIME_WINDOW_HAS_STOP(tw) || stopp->txn == tw->start_txn)
                    *skipp = true;
                else {
                    stopp->txn = WT_TXN_MAX;
                    stopp->prepare_ts = WT_TS_MAX;
                    stopp->prepared_id = WT_PREPARED_ID_NONE;
                    stopp->ts = WT_TS_MAX;
                    stopp->durable_ts = WT_TS_NONE;
                    stopp->prepared = false;
                    stopp->version_prepared = false;
                }
            } else
                stopp->version_prepared = WT_TIME_WINDOW_HAS_PREPARE(tw);
        }
    }
}

/*
 * __curversion_value_return_from_disk_image --
 *     Pack the metadata for the on-disk value and record it as the previously-returned stop state.
 */
static int
__curversion_value_return_from_disk_image(
  WT_CURSOR *cursor, WT_TIME_WINDOW *tw, const WT_CURVERSION_DISK_STOP_TIME_POINT *stopp)
{
    WT_CURSOR_VERSION *version_cursor = (WT_CURSOR_VERSION *)cursor;
    bool has_start_prepare = WT_TIME_WINDOW_HAS_START_PREPARE(tw);
    wt_timestamp_t start_meta = has_start_prepare ? tw->start_prepare_ts : tw->start_ts;
    wt_timestamp_t durable_start_meta =
      has_start_prepare ? tw->start_prepare_ts : tw->durable_start_ts;

    WT_RET(__curversion_set_value_with_format(cursor, WT_CURVERSION_METADATA_FORMAT, tw->start_txn,
      start_meta, durable_start_meta, tw->start_prepare_ts, tw->start_prepared_id, stopp->txn,
      stopp->prepared ? stopp->prepare_ts : stopp->ts, stopp->durable_ts, stopp->prepare_ts,
      stopp->prepared_id, WT_UPDATE_STANDARD, stopp->version_prepared, 0,
      WT_CURVERSION_DISK_IMAGE));

    version_cursor->upd_stop_txnid = tw->start_txn;
    version_cursor->curversion_durable_stop_ts = durable_start_meta;
    version_cursor->curversion_stop_ts = start_meta;
    version_cursor->upd_stop_prepare_ts = tw->start_prepare_ts;
    version_cursor->upd_stop_prepared_id = tw->start_prepared_id;
    /*
     * upd_stop_prepared is intentionally not updated here. Disk records are stable so their start
     * time point is never an in-progress prepare; any prepared stop inherited from the update chain
     * still gates the globally-visible check for subsequent history-store iterations.
     */
    return (0);
}

/*
 * __curversion_stop_globally_visible --
 *     True if the previously-returned stop record is globally visible in timestamp-order mode,
 *     meaning no older versions need to be returned. Aborted stops are never globally visible.
 */
static WT_INLINE bool
__curversion_stop_globally_visible(WT_SESSION_IMPL *session, WT_CURSOR_VERSION *version_cursor)
{
    return (F_ISSET(version_cursor, WT_CURVERSION_TIMESTAMP_ORDER) &&
      !version_cursor->upd_stop_prepared && version_cursor->upd_stop_txnid != WT_TXN_ABORTED &&
      __wt_txn_visible_all(
        session, version_cursor->upd_stop_txnid, version_cursor->curversion_durable_stop_ts));
}

/*
 * __curversion_process_on_disk --
 *     Return the on-disk value as the next version, if there is one and it should be returned.
 */
static int
__curversion_process_on_disk(
  WT_CURSOR *cursor, WT_UPDATE *tombstone, WT_PAGE *page, bool *upd_foundp, bool *donep)
{
    WT_CURVERSION_DISK_STOP_TIME_POINT stop;
    WT_DECL_RET;

    WT_SESSION_IMPL *session = CUR2S(cursor);
    WT_CURSOR_VERSION *version_cursor = (WT_CURSOR_VERSION *)cursor;
    WT_CURSOR_BTREE *cbt = (WT_CURSOR_BTREE *)version_cursor->file_cursor;
    WT_TIME_WINDOW *tw = &cbt->upd_value->tw;
    stop.version_prepared = false;

    bool skip = false;

    if (*upd_foundp || F_ISSET(version_cursor, WT_CURVERSION_ON_DISK_EXHAUSTED))
        return (0);

    /* No older versions are needed once the previously-returned stop is globally visible. */
    if (__curversion_stop_globally_visible(session, version_cursor)) {
        *donep = true;
        return (0);
    }

    /* Validate the page has an accessible on-disk slot before doing further work. */
    switch (page->type) {
    case WT_PAGE_ROW_LEAF:
        if (cbt->ins != NULL) {
            F_SET(version_cursor, WT_CURVERSION_ON_DISK_EXHAUSTED);
            F_SET(version_cursor, WT_CURVERSION_HS_EXHAUSTED);
            return (WT_NOTFOUND);
        }
        break;
    case WT_PAGE_COL_VAR:
        /* Empty page doesn't have any on page value. */
        if (page->entries == 0) {
            F_SET(version_cursor, WT_CURVERSION_ON_DISK_EXHAUSTED);
            F_SET(version_cursor, WT_CURVERSION_HS_EXHAUSTED);
            return (WT_NOTFOUND);
        }
        break;
    default:
        return (__wt_illegal_value(session, page->type));
    }

    /*
     * Get the ondisk value. It is possible to see an overflow-removed value if a concurrent
     * checkpoint freed the underlying overflow blocks. In that case the value either already came
     * back via the update chain or will come from the history store, so it is safe to skip.
     */
    ret = __wt_value_return_buf(cbt, cbt->ref, &cbt->upd_value->buf, tw);
    if (ret == WT_RESTART) {
        F_SET(version_cursor, WT_CURVERSION_ON_DISK_EXHAUSTED);
        return (0);
    }
    WT_RET(ret);

    if (!WT_TIME_WINDOW_HAS_STOP(tw)) {
        if (__curversion_disk_skip_no_stop(version_cursor, tw)) {
            F_SET(version_cursor, WT_CURVERSION_ON_DISK_EXHAUSTED);
            return (0);
        }
        stop.durable_ts = version_cursor->curversion_durable_stop_ts;
        stop.prepare_ts = version_cursor->upd_stop_prepare_ts;
        stop.prepared_id = version_cursor->upd_stop_prepared_id;
        stop.ts = version_cursor->curversion_stop_ts;
        stop.txn = version_cursor->upd_stop_txnid;
        stop.prepared = __curversion_stop_uses_prepare_ts(version_cursor);
    } else {
        __curversion_disk_check_with_stop(session, version_cursor, tw, &skip, donep);
        if (!skip && !*donep) {
            stop.durable_ts = tw->durable_stop_ts;
            stop.prepare_ts = tw->stop_prepare_ts;
            stop.prepared_id = tw->stop_prepared_id;
            stop.ts = tw->stop_ts;
            stop.txn = tw->stop_txn;
            stop.prepared = WT_TIME_WINDOW_HAS_STOP_PREPARE(tw);
        }
    }

    /*
     * Only resolve prepare state when nothing has already marked the record done or skipped; then
     * perform a single combined done/skip check before emitting the value.
     */
    if (!*donep && !skip)
        __curversion_disk_finalize_prepared(version_cursor, tw, tombstone, &stop, &skip, donep);
    if (*donep || skip) {
        if (skip)
            F_SET(version_cursor, WT_CURVERSION_ON_DISK_EXHAUSTED);
        return (0);
    }

    WT_RET(__curversion_value_return_from_disk_image(cursor, tw, &stop));

    *upd_foundp = true;
    F_SET(version_cursor, WT_CURVERSION_ON_DISK_EXHAUSTED);
    return (0);
}

/*
 * __curversion_value_return_from_hs --
 *     Pack the metadata for the history-store value and record it as the previously-returned stop
 *     state.
 */
static int
__curversion_value_return_from_hs(WT_CURSOR *cursor, WT_TIME_WINDOW *twp, uint64_t hs_upd_type)
{
    WT_CURSOR_VERSION *version_cursor = (WT_CURSOR_VERSION *)cursor;
    bool has_start_prepare = WT_TIME_WINDOW_HAS_START_PREPARE(twp);
    wt_timestamp_t start_meta = has_start_prepare ? twp->start_prepare_ts : twp->start_ts;
    wt_timestamp_t durable_start_meta =
      has_start_prepare ? twp->start_prepare_ts : twp->durable_start_ts;

    WT_RET(__curversion_set_value_with_format(cursor, WT_CURVERSION_METADATA_FORMAT, twp->start_txn,
      start_meta, durable_start_meta, twp->start_prepare_ts, twp->start_prepared_id, twp->stop_txn,
      WT_TIME_WINDOW_HAS_STOP_PREPARE(twp) ? twp->stop_prepare_ts : twp->stop_ts,
      WT_TIME_WINDOW_HAS_STOP_PREPARE(twp) ? twp->stop_prepare_ts : twp->durable_stop_ts,
      twp->stop_prepare_ts, twp->stop_prepared_id, hs_upd_type, 0, 0, WT_CURVERSION_HISTORY_STORE));

    version_cursor->upd_stop_txnid = twp->start_txn;
    version_cursor->curversion_durable_stop_ts = durable_start_meta;
    version_cursor->curversion_stop_ts = start_meta;
    version_cursor->upd_stop_prepare_ts = twp->start_prepare_ts;
    version_cursor->upd_stop_prepared_id = twp->start_prepared_id;
    /*
     * upd_stop_prepared is intentionally not updated here history-store records are fully committed
     * so their start time point is never an in-progress prepare; any prepared stop inherited from
     * the update chain still gates the globally-visible check for the next iteration.
     */
    return (0);
}

/*
 * __curversion_process_hs --
 *     Return the next version from the history store, if any.
 */
static int
__curversion_process_hs(WT_CURSOR *cursor, WT_PAGE *page, WT_ITEM **keyp, WT_ITEM **hs_valuep,
  bool *upd_foundp, bool *donep)
{
    WT_SESSION_IMPL *session = CUR2S(cursor);
    WT_CURSOR_VERSION *version_cursor = (WT_CURSOR_VERSION *)cursor;
    WT_CURSOR *file_cursor = version_cursor->file_cursor;
    WT_CURSOR *hs_cursor = version_cursor->hs_cursor;
    WT_CURSOR_BTREE *cbt = (WT_CURSOR_BTREE *)file_cursor;
    WT_TIME_WINDOW *twp = NULL;
    uint64_t hs_upd_type;

    if (*upd_foundp || hs_cursor == NULL || F_ISSET(version_cursor, WT_CURVERSION_HS_EXHAUSTED))
        return (0);

    /* No older versions are needed once the previously-returned stop is globally visible. */
    if (__curversion_stop_globally_visible(session, version_cursor)) {
        *donep = true;
        return (0);
    }

    /* Ensure we can see all the content in the history store. */
    F_SET(hs_cursor, WT_CURSTD_HS_READ_COMMITTED);

    if (!F_ISSET(hs_cursor, WT_CURSTD_KEY_INT)) {
        if (page->type == WT_PAGE_ROW_LEAF)
            hs_cursor->set_key(
              hs_cursor, 4, S2BT(session)->id, &file_cursor->key, WT_TS_MAX, UINT64_MAX);
        else {
            /* Ensure enough room for a column-store key without checking. */
            WT_RET(__wt_scr_alloc(session, WT_INTPACK64_MAXSIZE, keyp));

            uint8_t *p = (*keyp)->mem;
            WT_RET(__wt_vpack_uint(&p, 0, cbt->recno));
            (*keyp)->size = WT_PTRDIFF(p, (*keyp)->data);
            hs_cursor->set_key(hs_cursor, 4, S2BT(session)->id, *keyp, WT_TS_MAX, UINT64_MAX);
        }
        WT_RET(__wt_curhs_search_near_before(session, hs_cursor));
    } else
        WT_RET(hs_cursor->prev(hs_cursor));

    WT_RET(__wt_scr_alloc(session, 0, hs_valuep));

    for (;;) {
        wt_timestamp_t durable_start_ts, durable_stop_ts;
        __wt_hs_upd_time_window(hs_cursor, &twp);
        WT_RET(hs_cursor->get_value(
          hs_cursor, &durable_stop_ts, &durable_start_ts, &hs_upd_type, *hs_valuep));

        /*
         * Reconstruct the history store value if needed. Because the current value is preserved
         * across iterations, modifies can be applied on top of it.
         */
        if (hs_upd_type == WT_UPDATE_MODIFY) {
            size_t max_memsize;
            __wt_modify_max_memsize_format((*hs_valuep)->data, file_cursor->value_format,
              cbt->upd_value->buf.size, &max_memsize);
            WT_RET(__wt_buf_set_and_grow(session, &cbt->upd_value->buf, cbt->upd_value->buf.data,
              cbt->upd_value->buf.size, max_memsize));
            WT_RET(__wt_modify_apply_item(
              session, file_cursor->value_format, &cbt->upd_value->buf, (*hs_valuep)->data));
        } else {
            WT_ASSERT(session, hs_upd_type == WT_UPDATE_STANDARD);
            cbt->upd_value->buf.data = (*hs_valuep)->data;
            cbt->upd_value->buf.size = (*hs_valuep)->size;
        }

        if (!F_ISSET(version_cursor, WT_CURVERSION_TIMESTAMP_ORDER))
            break;

        /* Skip all non-aborted updates that are duplicate to the previous updates returned. */
        if (version_cursor->upd_stop_txnid != WT_TXN_ABORTED &&
          twp->stop_txn <= version_cursor->upd_stop_txnid &&
          twp->stop_ts <= version_cursor->curversion_stop_ts &&
          twp->durable_stop_ts <= version_cursor->curversion_durable_stop_ts)
            break;

        WT_RET(hs_cursor->prev(hs_cursor));
    }

    if (version_cursor->start_timestamp != WT_TS_NONE) {
        /* Done if the durable stop timestamp is at or before the end timestamp. */
        if (twp->stop_ts != WT_TS_MAX && twp->durable_stop_ts <= version_cursor->start_timestamp) {
            *donep = true;
            return (0);
        }
        /*
         * FIXME-WT-16136: for the history store it is hard to tell whether a stop durable timestamp
         * belongs to a tombstone or to the previous full value. For now, always return the value
         * when its stop durable timestamp is past the end timestamp.
         */
        if (twp->stop_ts == WT_TS_MAX && twp->durable_start_ts <= version_cursor->start_timestamp) {
            *donep = true;
            return (0);
        }
    }

    WT_RET(__curversion_value_return_from_hs(cursor, twp, hs_upd_type));
    *upd_foundp = true;
    return (0);
}

/*
 * __curversion_next_single_key --
 *     Iterate the updates of a single key.
 */
static int
__curversion_next_single_key(WT_CURSOR *cursor)
{
    WT_CURSOR *file_cursor;
    WT_CURSOR_BTREE *cbt;
    WT_CURSOR_VERSION *version_cursor;
    WT_DECL_ITEM(hs_value);
    WT_DECL_ITEM(key);
    WT_DECL_RET;
    WT_PAGE *page;
    WT_SESSION_IMPL *session;
    WT_UPDATE *tombstone;
    uint64_t raw;
    bool done, upd_found;

    session = CUR2S(cursor);
    version_cursor = (WT_CURSOR_VERSION *)cursor;
    file_cursor = version_cursor->file_cursor;
    cbt = (WT_CURSOR_BTREE *)file_cursor;
    tombstone = NULL;
    done = false;
    upd_found = false;

    /* Temporarily clear the raw flag. We need to pack the data according to the format. */
    raw = F_MASK(cursor, WT_CURSTD_RAW);
    F_CLR(cursor, WT_CURSTD_RAW);

    /* The cursor should be positioned, otherwise the next call will fail. */
    if (!F_ISSET(file_cursor, WT_CURSTD_KEY_INT))
        WT_ERR_SUB(session, WT_ROLLBACK, WT_NONE,
          "rolling back version_cursor->next due to no initial position");

    /* It's unsafe to access the page before checking the cursor's position. */
    page = cbt->ref->page;

    WT_ERR(__curversion_process_chain(cursor, &tombstone, &upd_found, &done));
    if (!done)
        WT_ERR(__curversion_process_on_disk(cursor, tombstone, page, &upd_found, &done));
    if (!done)
        WT_ERR(__curversion_process_hs(cursor, page, &key, &hs_value, &upd_found, &done));

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
    version_cursor->curversion_durable_stop_ts = WT_TS_NONE;
    version_cursor->curversion_stop_ts = WT_TS_MAX;
    version_cursor->upd_stop_prepare_ts = WT_TS_MAX;
    version_cursor->upd_stop_prepared_id = WT_PREPARED_ID_NONE;
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
    case WT_PAGE_COL_VAR:
        if (cbt->ins != NULL)
            upd = cbt->ins->upd;
        break;
    default:
        WT_RET(__wt_illegal_value(session, page->type));
    }

    for (; upd != NULL; upd = upd->next) {
        /* Skip aborted updates unless showing prepared rollbacks. */
        if (upd->txnid == WT_TXN_ABORTED) {
            if (F_ISSET(version_cursor, WT_CURVERSION_SHOW_PREPARED_ROLLBACK) &&
              __curversion_is_prepare_rollback_update(upd))
                break;
            continue;
        }

        if (!F_ISSET(version_cursor, WT_CURVERSION_VISIBLE_ONLY))
            break;

        /* Skip invisible updates. */
        WT_ACQUIRE_READ_WITH_BARRIER(prepare_state, upd->prepare_state);
        if (prepare_state == WT_PREPARE_INPROGRESS || prepare_state == WT_PREPARE_LOCKED)
            continue;

        if (!__wt_txn_visible_id(session, upd->txnid))
            continue;

        break;
    }

    version_cursor->next_upd = upd;
    version_cursor->upd_stop_txnid = WT_TXN_MAX;
    version_cursor->curversion_durable_stop_ts = WT_TS_NONE;
    version_cursor->curversion_stop_ts = WT_TS_MAX;
    version_cursor->upd_stop_prepare_ts = WT_TS_MAX;
    version_cursor->upd_stop_prepared_id = WT_PREPARED_ID_NONE;
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

    /* Early return if the cursor is not configured to walk across keys. */
    if (!F_ISSET(version_cursor, WT_CURVERSION_CROSS_KEY)) {
        WT_ERR(__curversion_next_single_key(cursor));
        goto done;
    }

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
done:
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
    __wt_atomic_sub_uint32(&S2C(session)->version_cursor_count, 1);

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
    if (__wt_atomic_load_uint32_relaxed(&conn->version_cursor_count) == 0) {
        __wt_txn_pinned_timestamp(session, &pinned_ts);
        txn_global->version_cursor_pinned_timestamp = pinned_ts;
    }
    (void)__wt_atomic_add_uint32(&conn->version_cursor_count, 1);
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
    if (F_ISSET_ATOMIC_32(conn, WT_CONN_HS_OPEN) && !__wt_btree_stays_in_memory(file_btree)) {
        WT_ERR(__wt_curhs_open(session, file_btree->id, NULL, cursor, &version_cursor->hs_cursor));
        F_SET(version_cursor->hs_cursor, WT_CURSTD_HS_READ_COMMITTED);
    }

    /* Initialize information used to track update metadata. */
    version_cursor->upd_stop_txnid = WT_TXN_MAX;
    version_cursor->curversion_durable_stop_ts = WT_TS_MAX;
    version_cursor->curversion_stop_ts = WT_TS_MAX;
    version_cursor->upd_stop_prepare_ts = WT_TS_MAX;
    version_cursor->upd_stop_prepared_id = WT_PREPARED_ID_NONE;

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
      __wt_config_gets_def(session, cfg, "debug.dump_version.cross_key", 0, &cval), true);
    if (ret == 0) {
        if (cval.val)
            F_SET(version_cursor, WT_CURVERSION_CROSS_KEY);
    }

    WT_ERR_NOTFOUND_OK(
      __wt_config_gets_def(session, cfg, "debug.dump_version.show_prepared_rollback", 0, &cval),
      true);
    if (ret == 0) {
        if (cval.val) {
            if (!__wt_btree_stays_in_memory(file_btree))
                WT_ERR_MSG(session, EINVAL,
                  "debug.dump_version.show_prepared_rollback is only supported for in-memory "
                  "b-trees");
            F_SET(version_cursor, WT_CURVERSION_SHOW_PREPARED_ROLLBACK);
        }
    }

    WT_ERR_NOTFOUND_OK(
      __wt_config_gets_def(session, cfg, "debug.dump_version.start_timestamp", 0, &cval), true);
    if (ret == 0)
        WT_ERR(__wt_txn_parse_timestamp(
          session, "start timestamp", &version_cursor->start_timestamp, &cval));
    else
        ret = 0;

    version_cursor->upd_stop_txnid = WT_TXN_MAX;
    version_cursor->curversion_durable_stop_ts = WT_TS_MAX;
    version_cursor->curversion_stop_ts = WT_TS_MAX;
    version_cursor->upd_stop_prepare_ts = WT_TS_MAX;
    version_cursor->upd_stop_prepared_id = WT_PREPARED_ID_NONE;

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
