/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_rts_visibility_has_stable_update --
 *     Check if an update chain has a stable update on it. Assume the update chain has already been
 *     processed so all we need to do is look for a valid, non-aborted entry.
 */
bool
__wt_rts_visibility_has_stable_update(WT_UPDATE *upd)
{
    while (upd != NULL && (upd->type == WT_UPDATE_INVALID || upd->txnid == WT_TXN_ABORTED))
        upd = upd->next;
    return (upd != NULL);
}

/*
 * __wt_rts_visibility_txn_visible_id --
 *     Check if the transaction id is visible or not.
 */
bool
__wt_rts_visibility_txn_visible_id(WT_SESSION_IMPL *session, uint64_t id)
{
    WT_CONNECTION_IMPL *conn;

    conn = S2C(session);

    /* If not recovery then assume all the data as visible. */
    if (!F_ISSET(conn, WT_CONN_RECOVERING))
        return (true);

    /*
     * Only full checkpoint writes the metadata with snapshot. If the recovered checkpoint snapshot
     * details are none then return false i.e, updates are visible.
     */
    if (conn->recovery_ckpt_snap_min == WT_TXN_NONE && conn->recovery_ckpt_snap_max == WT_TXN_NONE)
        return (true);

    return (
      __wt_txn_visible_id_snapshot(id, conn->recovery_ckpt_snap_min, conn->recovery_ckpt_snap_max,
        conn->recovery_ckpt_snapshot, conn->recovery_ckpt_snapshot_count));
}

/*
 * __rts_visibility_get_ref_max_durable_timestamp --
 *     Returns the ref aggregated max durable timestamp. The max durable timestamp is calculated
 *     between both start and stop durable timestamps except for history store, because most of the
 *     history store updates have stop timestamp either greater or equal to the start timestamp
 *     except for the updates written for the prepared updates on the data store. To abort the
 *     updates with no stop timestamp, we must include the newest stop timestamp also into the
 *     calculation of maximum durable timestamp of the history store.
 */
static wt_timestamp_t
__rts_visibility_get_ref_max_durable_timestamp(WT_SESSION_IMPL *session, WT_TIME_AGGREGATE *ta)
{
    if (WT_IS_HS(session->dhandle))
        return (WT_MAX(ta->newest_stop_durable_ts, ta->newest_stop_ts));
    return (WT_MAX(ta->newest_start_durable_ts, ta->newest_stop_durable_ts));
}

/*
 * __wt_rts_visibility_page_needs_abort --
 *     Check whether the page needs rollback, returning true if the page has modifications newer
 *     than the given timestamp.
 */
bool
__wt_rts_visibility_page_needs_abort(
  WT_SESSION_IMPL *session, WT_REF *ref, wt_timestamp_t rollback_timestamp)
{
    WT_ADDR *addr;
    WT_CELL_UNPACK_ADDR vpack;
    WT_MULTI *multi;
    WT_PAGE_MODIFY *mod;
    wt_timestamp_t durable_ts;
    uint64_t newest_txn;
    uint32_t i;
    char ts_string[WT_TS_INT_STRING_SIZE];
    const char *tag;
    bool prepared, result;

    addr = ref->addr;
    mod = ref->page == NULL ? NULL : ref->page->modify;
    durable_ts = WT_TS_NONE;
    newest_txn = WT_TXN_NONE;
    tag = "undefined state";
    prepared = result = false;

    /*
     * The rollback operation should be performed on this page when any one of the following is
     * greater than the given timestamp or during recovery if the newest transaction id on the page
     * is greater than or equal to recovered checkpoint snapshot min:
     * 1. The reconciled replace page max durable timestamp.
     * 2. The reconciled multi page max durable timestamp.
     * 3. For just-instantiated deleted pages that have not otherwise been modified, the durable
     *    timestamp in the page delete information. This timestamp isn't reflected in the address's
     *    time aggregate.
     * 4. The on page address max durable timestamp.
     * 5. The off page address max durable timestamp.
     */
    if (mod != NULL && mod->rec_result == WT_PM_REC_REPLACE) {
        tag = "reconciled replace block";
        durable_ts = __rts_visibility_get_ref_max_durable_timestamp(session, &mod->mod_replace.ta);
        prepared = mod->mod_replace.ta.prepare;
        result = (durable_ts > rollback_timestamp) || prepared;
    } else if (mod != NULL && mod->rec_result == WT_PM_REC_MULTIBLOCK) {
        tag = "reconciled multi block";
        /* Calculate the max durable timestamp by traversing all multi addresses. */
        for (multi = mod->mod_multi, i = 0; i < mod->mod_multi_entries; ++multi, ++i) {
            durable_ts = WT_MAX(
              durable_ts, __rts_visibility_get_ref_max_durable_timestamp(session, &multi->addr.ta));
            if (multi->addr.ta.prepare)
                prepared = true;
        }
        result = (durable_ts > rollback_timestamp) || prepared;
    } else if (mod != NULL && mod->instantiated && !__wt_page_is_modified(ref->page) &&
      ref->page_del != NULL) {
        tag = "page_del info";
        durable_ts = ref->page_del->durable_timestamp;
        prepared = ref->page_del->prepare_state == WT_PREPARE_INPROGRESS ||
          ref->page_del->prepare_state == WT_PREPARE_LOCKED;
        newest_txn = ref->page_del->txnid;
        result = (durable_ts > rollback_timestamp) || prepared ||
          WT_CHECK_RECOVERY_FLAG_TXNID(session, newest_txn);
    } else if (!__wt_off_page(ref->home, addr)) {
        tag = "on page cell";
        /* Check if the page is obsolete using the page disk address. */
        __wt_cell_unpack_addr(session, ref->home->dsk, (WT_CELL *)addr, &vpack);
        durable_ts = __rts_visibility_get_ref_max_durable_timestamp(session, &vpack.ta);
        prepared = vpack.ta.prepare;
        newest_txn = vpack.ta.newest_txn;
        result = (durable_ts > rollback_timestamp) || prepared ||
          WT_CHECK_RECOVERY_FLAG_TXNID(session, newest_txn);
    } else if (addr != NULL) {
        tag = "address";
        durable_ts = __rts_visibility_get_ref_max_durable_timestamp(session, &addr->ta);
        prepared = addr->ta.prepare;
        newest_txn = addr->ta.newest_txn;
        result = (durable_ts > rollback_timestamp) || prepared ||
          WT_CHECK_RECOVERY_FLAG_TXNID(session, newest_txn);
    }

    __wt_verbose_multi(session, WT_VERB_RECOVERY_RTS(session),
      WT_RTS_VERB_TAG_PAGE_ABORT_CHECK
      "ref=%p: page with %s, durable_timestamp=%s, newest_txn=%" PRIu64
      ", prepared_updates=%s, has_updates_need_abort=%s",
      (void *)ref, tag, __wt_timestamp_to_string(durable_ts, ts_string), newest_txn,
      prepared ? "true" : "false", result ? "true" : "false");

    return (result);
}
