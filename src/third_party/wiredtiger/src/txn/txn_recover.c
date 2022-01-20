/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/* Enable all recovery-related verbose messaging events. */
#define WT_VERB_RECOVERY_ALL        \
    WT_DECL_VERBOSE_MULTI_CATEGORY( \
      ((WT_VERBOSE_CATEGORY[]){WT_VERB_RECOVERY, WT_VERB_RECOVERY_PROGRESS}))

/* State maintained during recovery. */
typedef struct {
    const char *uri; /* File URI. */
    WT_CURSOR *c;    /* Cursor used for recovery. */
    WT_LSN ckpt_lsn; /* File's checkpoint LSN. */
} WT_RECOVERY_FILE;

typedef struct {
    WT_SESSION_IMPL *session;

    /* Files from the metadata, indexed by file ID. */
    WT_RECOVERY_FILE *files;
    size_t file_alloc; /* Allocated size of files array. */
    u_int max_fileid;  /* Maximum file ID seen. */
    u_int nfiles;      /* Number of files in the metadata. */

    WT_LSN ckpt_lsn;     /* Start LSN for main recovery loop. */
    WT_LSN max_ckpt_lsn; /* Maximum checkpoint LSN seen. */
    WT_LSN max_rec_lsn;  /* Maximum recovery LSN seen. */

    bool missing;       /* Were there missing files? */
    bool metadata_only; /*
                         * Set during the first recovery pass,
                         * when only the metadata is recovered.
                         */
} WT_RECOVERY;

/*
 * __recovery_cursor --
 *     Get a cursor for a recovery operation.
 */
static int
__recovery_cursor(
  WT_SESSION_IMPL *session, WT_RECOVERY *r, WT_LSN *lsnp, u_int id, bool duplicate, WT_CURSOR **cp)
{
    WT_CURSOR *c;
    const char *cfg[] = {WT_CONFIG_BASE(session, WT_SESSION_open_cursor), "overwrite", NULL};
    bool metadata_op;

    c = NULL;

    /*
     * File ids with the bit set to ignore this operation are skipped.
     */
    if (WT_LOGOP_IS_IGNORED(id))
        return (0);
    /*
     * Metadata operations have an id of 0. Match operations based on the id and the current pass of
     * recovery for metadata.
     *
     * Only apply operations in the correct metadata phase, and if the LSN is more recent than the
     * last checkpoint. If there is no entry for a file, assume it was dropped or missing after a
     * hot backup.
     */
    metadata_op = id == WT_METAFILE_ID;
    if (r->metadata_only != metadata_op)
        ;
    else if (id >= r->nfiles || r->files[id].uri == NULL) {
        /* If a file is missing, output a verbose message once. */
        if (!r->missing)
            __wt_verbose(
              session, WT_VERB_RECOVERY, "No file found with ID %u (max %u)", id, r->nfiles);
        r->missing = true;
    } else if (__wt_log_cmp(lsnp, &r->files[id].ckpt_lsn) >= 0) {
        /*
         * We're going to apply the operation. Get the cursor, opening one if none is cached.
         */
        if ((c = r->files[id].c) == NULL) {
            WT_RET(__wt_open_cursor(session, r->files[id].uri, NULL, cfg, &c));
            r->files[id].c = c;
        }
#ifndef WT_STANDALONE_BUILD
        /*
         * In the event of a clean shutdown, there shouldn't be any other table log records other
         * than metadata.
         */
        if (!metadata_op)
            S2C(session)->unclean_shutdown = true;
#endif
    }

    if (duplicate && c != NULL)
        WT_RET(__wt_open_cursor(session, r->files[id].uri, NULL, cfg, &c));

    *cp = c;
    return (0);
}

/*
 * Helper to a cursor if this operation is to be applied during recovery.
 */
#define GET_RECOVERY_CURSOR(session, r, lsnp, fileid, cp)                            \
    ret = __recovery_cursor(session, r, lsnp, fileid, false, cp);                    \
    __wt_verbose(session, WT_VERB_RECOVERY,                                          \
      "%s op %" PRIu32 " to file %" PRIu32 " at LSN %" PRIu32 "/%" PRIu32,           \
      ret != 0 ? "Error" : cursor == NULL ? "Skipping" : "Applying", optype, fileid, \
      (lsnp)->l.file, (lsnp)->l.offset);                                             \
    WT_ERR(ret);                                                                     \
    if (cursor == NULL)                                                              \
    break

/*
 * __txn_op_apply --
 *     Apply a transactional operation during recovery.
 */
static int
__txn_op_apply(WT_RECOVERY *r, WT_LSN *lsnp, const uint8_t **pp, const uint8_t *end)
{
    WT_CURSOR *cursor, *start, *stop;
    WT_DECL_RET;
    WT_ITEM key, start_key, stop_key, value;
    WT_SESSION_IMPL *session;
    wt_timestamp_t commit, durable, first_commit, prepare, read;
    uint64_t recno, start_recno, stop_recno, t_nsec, t_sec;
    uint32_t fileid, mode, optype, opsize;

    session = r->session;
    cursor = NULL;

    /* Peek at the size and the type. */
    WT_ERR(__wt_logop_read(session, pp, end, &optype, &opsize));
    end = *pp + opsize;

    /*
     * If it is an operation type that should be ignored, we're done. Note that file ids within
     * known operations also use the same macros to indicate that operation should be ignored.
     */
    if (WT_LOGOP_IS_IGNORED(optype)) {
        *pp += opsize;
        goto done;
    }

    switch (optype) {
    case WT_LOGOP_COL_MODIFY:
        WT_ERR(__wt_logop_col_modify_unpack(session, pp, end, &fileid, &recno, &value));
        GET_RECOVERY_CURSOR(session, r, lsnp, fileid, &cursor);
        cursor->set_key(cursor, recno);
        if ((ret = cursor->search(cursor)) != 0)
            WT_ERR_NOTFOUND_OK(ret, false);
        else {
            /*
             * Build/insert a complete value during recovery rather than using cursor modify to
             * create a partial update (for no particular reason than simplicity).
             */
            WT_ERR(__wt_modify_apply_item(
              CUR2S(cursor), cursor->value_format, &cursor->value, value.data));
            WT_ERR(cursor->insert(cursor));
        }
        break;

    case WT_LOGOP_COL_PUT:
        WT_ERR(__wt_logop_col_put_unpack(session, pp, end, &fileid, &recno, &value));
        GET_RECOVERY_CURSOR(session, r, lsnp, fileid, &cursor);
        cursor->set_key(cursor, recno);
        __wt_cursor_set_raw_value(cursor, &value);
        WT_ERR(cursor->insert(cursor));
        break;

    case WT_LOGOP_COL_REMOVE:
        WT_ERR(__wt_logop_col_remove_unpack(session, pp, end, &fileid, &recno));
        GET_RECOVERY_CURSOR(session, r, lsnp, fileid, &cursor);
        cursor->set_key(cursor, recno);
        WT_ERR(cursor->remove(cursor));
        break;

    case WT_LOGOP_COL_TRUNCATE:
        WT_ERR(
          __wt_logop_col_truncate_unpack(session, pp, end, &fileid, &start_recno, &stop_recno));
        GET_RECOVERY_CURSOR(session, r, lsnp, fileid, &cursor);

        /* Set up the cursors. */
        if (start_recno == WT_RECNO_OOB) {
            start = NULL;
            stop = cursor;
        } else if (stop_recno == WT_RECNO_OOB) {
            start = cursor;
            stop = NULL;
        } else {
            start = cursor;
            WT_ERR(__recovery_cursor(session, r, lsnp, fileid, true, &stop));
        }

        /* Set the keys. */
        if (start != NULL)
            start->set_key(start, start_recno);
        if (stop != NULL)
            stop->set_key(stop, stop_recno);

        WT_TRET(session->iface.truncate(&session->iface, NULL, start, stop, NULL));
        /* If we opened a duplicate cursor, close it now. */
        if (stop != NULL && stop != cursor)
            WT_TRET(stop->close(stop));
        WT_ERR(ret);
        break;

    case WT_LOGOP_ROW_MODIFY:
        WT_ERR(__wt_logop_row_modify_unpack(session, pp, end, &fileid, &key, &value));
        GET_RECOVERY_CURSOR(session, r, lsnp, fileid, &cursor);
        __wt_cursor_set_raw_key(cursor, &key);
        if ((ret = cursor->search(cursor)) != 0)
            WT_ERR_NOTFOUND_OK(ret, false);
        else {
            /*
             * Build/insert a complete value during recovery rather than using cursor modify to
             * create a partial update (for no particular reason than simplicity).
             */
            WT_ERR(__wt_modify_apply_item(
              CUR2S(cursor), cursor->value_format, &cursor->value, value.data));
            WT_ERR(cursor->insert(cursor));
        }
        break;

    case WT_LOGOP_ROW_PUT:
        WT_ERR(__wt_logop_row_put_unpack(session, pp, end, &fileid, &key, &value));
        GET_RECOVERY_CURSOR(session, r, lsnp, fileid, &cursor);
        __wt_cursor_set_raw_key(cursor, &key);
        __wt_cursor_set_raw_value(cursor, &value);
        WT_ERR(cursor->insert(cursor));
        break;

    case WT_LOGOP_ROW_REMOVE:
        WT_ERR(__wt_logop_row_remove_unpack(session, pp, end, &fileid, &key));
        GET_RECOVERY_CURSOR(session, r, lsnp, fileid, &cursor);
        __wt_cursor_set_raw_key(cursor, &key);
        WT_ERR(cursor->remove(cursor));
        break;

    case WT_LOGOP_ROW_TRUNCATE:
        WT_ERR(
          __wt_logop_row_truncate_unpack(session, pp, end, &fileid, &start_key, &stop_key, &mode));
        GET_RECOVERY_CURSOR(session, r, lsnp, fileid, &cursor);
        /* Set up the cursors. */
        start = stop = NULL;
        switch (mode) {
        case WT_TXN_TRUNC_ALL:
            /* Both cursors stay NULL. */
            break;
        case WT_TXN_TRUNC_BOTH:
            start = cursor;
            WT_ERR(__recovery_cursor(session, r, lsnp, fileid, true, &stop));
            break;
        case WT_TXN_TRUNC_START:
            start = cursor;
            break;
        case WT_TXN_TRUNC_STOP:
            stop = cursor;
            break;
        default:
            WT_ERR(__wt_illegal_value(session, mode));
        }

        /* Set the keys. */
        if (start != NULL)
            __wt_cursor_set_raw_key(start, &start_key);
        if (stop != NULL)
            __wt_cursor_set_raw_key(stop, &stop_key);

        WT_TRET(session->iface.truncate(&session->iface, NULL, start, stop, NULL));
        /* If we opened a duplicate cursor, close it now. */
        if (stop != NULL && stop != cursor)
            WT_TRET(stop->close(stop));
        WT_ERR(ret);
        break;
    case WT_LOGOP_TXN_TIMESTAMP:
        /*
         * Timestamp records are informational only. We have to unpack it to properly move forward
         * in the log record to the next operation, but otherwise ignore.
         */
        WT_ERR(__wt_logop_txn_timestamp_unpack(
          session, pp, end, &t_sec, &t_nsec, &commit, &durable, &first_commit, &prepare, &read));
        break;
    default:
        WT_ERR(__wt_illegal_value(session, optype));
    }

done:
    /* Reset the cursor so it doesn't block eviction. */
    if (cursor != NULL)
        WT_ERR(cursor->reset(cursor));
    return (0);

err:
    __wt_err(session, ret,
      "operation apply failed during recovery: operation type %" PRIu32 " at LSN %" PRIu32
      "/%" PRIu32,
      optype, lsnp->l.file, lsnp->l.offset);
    return (ret);
}

/*
 * __txn_commit_apply --
 *     Apply a commit record during recovery.
 */
static int
__txn_commit_apply(WT_RECOVERY *r, WT_LSN *lsnp, const uint8_t **pp, const uint8_t *end)
{
    /* The logging subsystem zero-pads records. */
    while (*pp < end && **pp)
        WT_RET(__txn_op_apply(r, lsnp, pp, end));

    return (0);
}

/*
 * __txn_log_recover --
 *     Roll the log forward to recover committed changes.
 */
static int
__txn_log_recover(WT_SESSION_IMPL *session, WT_ITEM *logrec, WT_LSN *lsnp, WT_LSN *next_lsnp,
  void *cookie, int firstrecord)
{
    WT_DECL_RET;
    WT_RECOVERY *r;
    uint64_t txnid_unused;
    uint32_t rectype;
    const uint8_t *end, *p;

    r = cookie;
    p = WT_LOG_SKIP_HEADER(logrec->data);
    end = (const uint8_t *)logrec->data + logrec->size;
    WT_UNUSED(firstrecord);

    /* First, peek at the log record type. */
    WT_RET(__wt_logrec_read(session, &p, end, &rectype));

    /*
     * Record the highest LSN we process during the metadata phase. If not the metadata phase, then
     * stop at that LSN.
     */
    if (r->metadata_only)
        WT_ASSIGN_LSN(&r->max_rec_lsn, next_lsnp);
    else if (__wt_log_cmp(lsnp, &r->max_rec_lsn) >= 0)
        return (0);

    switch (rectype) {
    case WT_LOGREC_CHECKPOINT:
        if (r->metadata_only)
            WT_RET(__wt_txn_checkpoint_logread(session, &p, end, &r->ckpt_lsn));
        break;

    case WT_LOGREC_COMMIT:
        if ((ret = __wt_vunpack_uint(&p, WT_PTRDIFF(end, p), &txnid_unused)) != 0)
            WT_RET_MSG(session, ret, "txn_log_recover: unpack failure");
        WT_RET(__txn_commit_apply(r, lsnp, &p, end));
        break;
    }

    return (0);
}

/*
 * __recovery_retrieve_timestamp --
 *     Retrieve a timestamp from the metadata.
 */
static int
__recovery_retrieve_timestamp(
  WT_RECOVERY *r, const char *system_uri, const char *timestamp_name, wt_timestamp_t *timestampp)
{
    WT_CONFIG_ITEM cval;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    char *sys_config;

    sys_config = NULL;

    session = r->session;

    /* Search the metadata for the system information. */
    WT_ERR_NOTFOUND_OK(__wt_metadata_search(session, system_uri, &sys_config), false);
    if (sys_config != NULL) {
        WT_CLEAR(cval);
        WT_ERR_NOTFOUND_OK(__wt_config_getones(session, sys_config, timestamp_name, &cval), false);
        if (cval.len != 0) {
            __wt_verbose(session, WT_VERB_RECOVERY, "Recovery %s %.*s", timestamp_name,
              (int)cval.len, cval.str);
            WT_ERR(__wt_txn_parse_timestamp_raw(session, timestamp_name, timestampp, &cval));
        }
    }

err:
    __wt_free(session, sys_config);
    return (ret);
}

/*
 * __recovery_set_checkpoint_timestamp --
 *     Set the checkpoint timestamp as retrieved from the metadata file.
 */
static int
__recovery_set_checkpoint_timestamp(WT_RECOVERY *r)
{
    WT_CONNECTION_IMPL *conn;
    WT_SESSION_IMPL *session;
    wt_timestamp_t ckpt_timestamp;
    char ts_string[WT_TS_INT_STRING_SIZE];

    session = r->session;
    conn = S2C(session);
    /*
     * Read the system checkpoint information from the metadata file and save the stable timestamp
     * of the last checkpoint for later query. This gets saved in the connection.
     */
    ckpt_timestamp = 0;

    WT_RET(
      __recovery_retrieve_timestamp(r, WT_SYSTEM_CKPT_URI, WT_SYSTEM_CKPT_TS, &ckpt_timestamp));

    /*
     * Set the recovery checkpoint timestamp and the metadata checkpoint timestamp so that the
     * checkpoint after recovery writes the correct value into the metadata.
     */
    conn->txn_global.meta_ckpt_timestamp = conn->txn_global.recovery_timestamp = ckpt_timestamp;

    __wt_verbose_multi(session, WT_VERB_RECOVERY_ALL, "Set global recovery timestamp: %s",
      __wt_timestamp_to_string(conn->txn_global.recovery_timestamp, ts_string));

    return (0);
}

/*
 * __recovery_set_oldest_timestamp --
 *     Set the oldest timestamp as retrieved from the metadata file.
 */
static int
__recovery_set_oldest_timestamp(WT_RECOVERY *r)
{
    WT_CONNECTION_IMPL *conn;
    WT_SESSION_IMPL *session;
    wt_timestamp_t oldest_timestamp;
    char ts_string[WT_TS_INT_STRING_SIZE];

    session = r->session;
    conn = S2C(session);
    /*
     * Read the system checkpoint information from the metadata file and save the oldest timestamp
     * of the last checkpoint for later query. This gets saved in the connection.
     */
    oldest_timestamp = 0;

    WT_RET(__recovery_retrieve_timestamp(
      r, WT_SYSTEM_OLDEST_URI, WT_SYSTEM_OLDEST_TS, &oldest_timestamp));
    conn->txn_global.oldest_timestamp = oldest_timestamp;
    conn->txn_global.has_oldest_timestamp = oldest_timestamp != WT_TS_NONE;

    __wt_verbose_multi(session, WT_VERB_RECOVERY_ALL, "Set global oldest timestamp: %s",
      __wt_timestamp_to_string(conn->txn_global.oldest_timestamp, ts_string));

    return (0);
}

/*
 * __recovery_set_checkpoint_snapshot --
 *     Set the checkpoint snapshot details as retrieved from the metadata file.
 */
static int
__recovery_set_checkpoint_snapshot(WT_SESSION_IMPL *session)
{
    WT_CONFIG list;
    WT_CONFIG_ITEM cval;
    WT_CONFIG_ITEM k;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    uint32_t counter;
    char *sys_config;

    sys_config = NULL;
    conn = S2C(session);
    counter = 0;

    /* Initialize the recovery checkpoint snapshot variables to default values. */
    conn->recovery_ckpt_snap_min = WT_TXN_NONE;
    conn->recovery_ckpt_snap_max = WT_TXN_NONE;
    conn->recovery_ckpt_snapshot_count = 0;

    /*
     * WiredTiger versions 10.0.1 onward have a valid checkpoint snapshot on-disk. There was a bug
     * in some versions of WiredTiger that are tagged with the 10.0.0 release, which saved the wrong
     * checkpoint snapshot (see WT-8395), so we ignore the snapshot when it was created with one of
     * those versions. Versions of WiredTiger prior to 10.0.0 never saved a checkpoint snapshot.
     * Additionally the turtle file doesn't always exist (for example, backup doesn't include the
     * turtle file), so there isn't always a WiredTiger version available. If there is no version
     * available, assume that the snapshot is valid, otherwise restoring from a backup won't work.
     */
    if (__wt_version_defined(conn->recovery_version) &&
      __wt_version_lte(conn->recovery_version, (WT_VERSION){10, 0, 0}))
        return (0);

    /*
     * Read the system checkpoint information from the metadata file and save the snapshot related
     * details of the last checkpoint for later query. This gets saved in the connection.
     */
    WT_ERR_NOTFOUND_OK(
      __wt_metadata_search(session, WT_SYSTEM_CKPT_SNAPSHOT_URI, &sys_config), false);
    if (sys_config != NULL) {
        WT_CLEAR(cval);
        if (__wt_config_getones(session, sys_config, WT_SYSTEM_CKPT_SNAPSHOT_MIN, &cval) == 0 &&
          cval.len != 0)
            conn->recovery_ckpt_snap_min = (uint64_t)cval.val;

        if (__wt_config_getones(session, sys_config, WT_SYSTEM_CKPT_SNAPSHOT_MAX, &cval) == 0 &&
          cval.len != 0)
            conn->recovery_ckpt_snap_max = (uint64_t)cval.val;

        if (__wt_config_getones(session, sys_config, WT_SYSTEM_CKPT_SNAPSHOT_COUNT, &cval) == 0 &&
          cval.len != 0)
            conn->recovery_ckpt_snapshot_count = (uint32_t)cval.val;

        if (__wt_config_getones(session, sys_config, WT_SYSTEM_CKPT_SNAPSHOT, &cval) == 0 &&
          cval.len != 0) {
            __wt_config_subinit(session, &list, &cval);
            WT_ERR(__wt_calloc_def(
              session, conn->recovery_ckpt_snapshot_count, &conn->recovery_ckpt_snapshot));
            while (__wt_config_subget_next(&list, &k) == 0)
                conn->recovery_ckpt_snapshot[counter++] = (uint64_t)k.val;
        }

        /*
         * Make sure that checkpoint snapshot does not have any unexpected value. The recovered
         * snapshot array should contain the values between recovered snapshot min and recovered
         * snapshot max.
         */
        WT_ASSERT(session,
          conn->recovery_ckpt_snapshot == NULL ||
            (conn->recovery_ckpt_snapshot_count == counter &&
              conn->recovery_ckpt_snapshot[0] == conn->recovery_ckpt_snap_min &&
              conn->recovery_ckpt_snapshot[counter - 1] < conn->recovery_ckpt_snap_max));
    }

err:
    __wt_free(session, sys_config);
    return (ret);
}

/*
 * __recovery_set_ckpt_base_write_gen --
 *     Set the base write gen as retrieved from the metadata file.
 */
static int
__recovery_set_ckpt_base_write_gen(WT_RECOVERY *r)
{
    WT_CONFIG_ITEM cval;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    char *sys_config;

    sys_config = NULL;
    session = r->session;

    /* Search the metadata for checkpoint base write gen information. */
    WT_ERR_NOTFOUND_OK(
      __wt_metadata_search(session, WT_SYSTEM_BASE_WRITE_GEN_URI, &sys_config), false);
    if (sys_config != NULL) {
        WT_CLEAR(cval);
        WT_ERR(__wt_config_getones(session, sys_config, WT_SYSTEM_BASE_WRITE_GEN, &cval));
        if (cval.len != 0)
            S2C(session)->last_ckpt_base_write_gen = (uint64_t)cval.val;
    }

err:
    __wt_free(session, sys_config);
    return (ret);
}

/*
 * __recovery_setup_file --
 *     Set up the recovery slot for a file, track the largest file ID, and update the base write gen
 *     based on the file's configuration.
 */
static int
__recovery_setup_file(WT_RECOVERY *r, const char *uri, const char *config)
{
    WT_CONFIG_ITEM cval;
    WT_DECL_RET;
    WT_LSN lsn;
    uint32_t fileid, lsnfile, lsnoffset;

    WT_RET(__wt_config_getones(r->session, config, "id", &cval));
    fileid = (uint32_t)cval.val;

    /* Track the largest file ID we have seen. */
    if (fileid > r->max_fileid)
        r->max_fileid = fileid;

    if (r->nfiles <= fileid) {
        WT_RET(__wt_realloc_def(r->session, &r->file_alloc, fileid + 1, &r->files));
        r->nfiles = fileid + 1;
    }

    if (r->files[fileid].uri != NULL)
        WT_RET_PANIC(r->session, WT_PANIC,
          "metadata corruption: files %s and %s have the same file ID %u", uri,
          r->files[fileid].uri, fileid);
    WT_RET(__wt_strdup(r->session, uri, &r->files[fileid].uri));
    if ((ret = __wt_config_getones(r->session, config, "checkpoint_lsn", &cval)) != 0)
        WT_RET_MSG(
          r->session, ret, "Failed recovery setup for %s: cannot parse config '%s'", uri, config);
    /* If there is no checkpoint logged for the file, apply everything. */
    if (cval.type != WT_CONFIG_ITEM_STRUCT)
        WT_INIT_LSN(&lsn);
    /* NOLINTNEXTLINE(cert-err34-c) */
    else if (sscanf(cval.str, "(%" SCNu32 ",%" SCNu32 ")", &lsnfile, &lsnoffset) == 2)
        WT_SET_LSN(&lsn, lsnfile, lsnoffset);
    else
        WT_RET_MSG(r->session, EINVAL,
          "Failed recovery setup for %s: cannot parse checkpoint LSN '%.*s'", uri, (int)cval.len,
          cval.str);
    WT_ASSIGN_LSN(&r->files[fileid].ckpt_lsn, &lsn);

    __wt_verbose(r->session, WT_VERB_RECOVERY,
      "Recovering %s with id %" PRIu32 " @ (%" PRIu32 ", %" PRIu32 ")", uri, fileid, lsn.l.file,
      lsn.l.offset);

    if ((!WT_IS_MAX_LSN(&lsn) && !WT_IS_INIT_LSN(&lsn)) &&
      (WT_IS_MAX_LSN(&r->max_ckpt_lsn) || __wt_log_cmp(&lsn, &r->max_ckpt_lsn) > 0))
        WT_ASSIGN_LSN(&r->max_ckpt_lsn, &lsn);

    /* Update the base write gen based on this file's configuration. */
    if ((ret = __wt_metadata_update_base_write_gen(r->session, config)) != 0)
        WT_RET_MSG(r->session, ret, "Failed recovery setup for %s: cannot update write gen", uri);
    return (0);
}

/*
 * __recovery_close_cursors --
 *     Close the logging recovery cursors.
 */
static int
__recovery_close_cursors(WT_RECOVERY *r)
{
    WT_CURSOR *c;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    u_int i;

    session = r->session;
    for (i = 0; i < r->nfiles; i++) {
        __wt_free(session, r->files[i].uri);
        if ((c = r->files[i].c) != NULL)
            WT_TRET(c->close(c));
    }

    r->nfiles = 0;
    __wt_free(session, r->files);
    return (ret);
}

/*
 * __recovery_file_scan_prefix --
 *     Scan the files matching the prefix referenced from the metadata and gather information about
 *     them for recovery.
 */
static int
__recovery_file_scan_prefix(WT_RECOVERY *r, const char *prefix, const char *ignore_suffix)
{
    WT_CURSOR *c;
    WT_DECL_RET;
    int cmp;
    const char *uri, *config;

    /* Scan through all entries in the metadata matching the prefix. */
    c = r->files[0].c;
    c->set_key(c, prefix);
    if ((ret = c->search_near(c, &cmp)) != 0) {
        /* Is the metadata empty? */
        WT_RET_NOTFOUND_OK(ret);
        return (0);
    }
    if (cmp < 0 && (ret = c->next(c)) != 0) {
        /* No matching entries? */
        WT_RET_NOTFOUND_OK(ret);
        return (0);
    }
    for (; ret == 0; ret = c->next(c)) {
        WT_RET(c->get_key(c, &uri));
        if (!WT_PREFIX_MATCH(uri, prefix))
            break;
        if (ignore_suffix != NULL && WT_SUFFIX_MATCH(uri, ignore_suffix))
            continue;
        WT_RET(c->get_value(c, &config));
        WT_RET(__recovery_setup_file(r, uri, config));
    }
    WT_RET_NOTFOUND_OK(ret);
    return (0);
}

/*
 * __recovery_file_scan --
 *     Scan the files referenced from the metadata and gather information about them for recovery.
 */
static int
__recovery_file_scan(WT_RECOVERY *r)
{
    /* Scan through all files and tiered entries in the metadata. */
    WT_RET(__recovery_file_scan_prefix(r, "file:", ".wtobj"));
    WT_RET(__recovery_file_scan_prefix(r, "tiered:", NULL));

    /*
     * Set the connection level file id tracker, as such upon creation of a new file we'll begin
     * from the latest file id.
     */
    S2C(r->session)->next_file_id = r->max_fileid;

    return (0);
}

/*
 * __hs_exists --
 *     Check whether the history store exists. This function looks for both the history store URI in
 *     the metadata file and for the history store data file itself. If we're running salvage, we'll
 *     attempt to salvage the history store here.
 */
static int
__hs_exists(WT_SESSION_IMPL *session, WT_CURSOR *metac, const char *cfg[], bool *hs_exists)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_SESSION *wt_session;

    conn = S2C(session);

    /*
     * We should check whether the history store file exists in the metadata or not. If it does not,
     * then we should skip rollback to stable for each table. This might happen if we're upgrading
     * from an older version. If it does exist in the metadata we should check that it exists on
     * disk to confirm that it wasn't deleted between runs.
     *
     * This needs to happen after we apply the logs as they may contain the metadata changes which
     * include the history store creation. As such the on disk metadata file won't contain the
     * history store but will after log application.
     */
    metac->set_key(metac, WT_HS_URI);
    WT_ERR_NOTFOUND_OK(metac->search(metac), true);
    if (ret == WT_NOTFOUND) {
        *hs_exists = false;
        ret = 0;
    } else {
        /* Given the history store exists in the metadata validate whether it exists on disk. */
        WT_ERR(__wt_fs_exist(session, WT_HS_FILE, hs_exists));
        if (*hs_exists) {
            /*
             * Attempt to configure the history store, this will detect corruption if it fails.
             */
            ret = __wt_hs_config(session, cfg);
            if (ret != 0) {
                if (F_ISSET(conn, WT_CONN_SALVAGE)) {
                    wt_session = &session->iface;
                    WT_ERR(wt_session->salvage(wt_session, WT_HS_URI, NULL));
                } else
                    WT_ERR(ret);
            }
        } else {
            /*
             * We're attempting to salvage the database with a missing history store, remove it from
             * the metadata and pretend it never existed. As such we won't run rollback to stable
             * later.
             */
            if (F_ISSET(conn, WT_CONN_SALVAGE)) {
                *hs_exists = false;
                metac->remove(metac);
            } else
                /* The history store file has likely been deleted, we cannot recover from this. */
                WT_ERR_MSG(session, WT_TRY_SALVAGE, "%s file is corrupted or missing", WT_HS_FILE);
        }
    }
err:
    /* Unpin the page from cache. */
    WT_TRET(metac->reset(metac));
    return (ret);
}

/*
 * __wt_txn_recover --
 *     Run recovery.
 */
int
__wt_txn_recover(WT_SESSION_IMPL *session, const char *cfg[])
{
    WT_CONNECTION_IMPL *conn;
    WT_CURSOR *metac;
    WT_DECL_RET;
    WT_RECOVERY r;
    WT_RECOVERY_FILE *metafile;
    wt_off_t hs_size;
    char *config;
    char ts_string[2][WT_TS_INT_STRING_SIZE];
    bool do_checkpoint, eviction_started, hs_exists, needs_rec, was_backup;
    bool rts_executed;

    conn = S2C(session);
    WT_CLEAR(r);
    WT_INIT_LSN(&r.ckpt_lsn);
    config = NULL;
    do_checkpoint = hs_exists = true;
    rts_executed = false;
    eviction_started = false;
    was_backup = F_ISSET(conn, WT_CONN_WAS_BACKUP);

    /* We need a real session for recovery. */
    WT_RET(
      __wt_open_internal_session(conn, "txn-recover", false, WT_SESSION_NO_LOGGING, 0, &session));
    r.session = session;
    WT_MAX_LSN(&r.max_ckpt_lsn);
    WT_MAX_LSN(&r.max_rec_lsn);
    conn->txn_global.recovery_timestamp = conn->txn_global.meta_ckpt_timestamp = WT_TS_NONE;

    F_SET(conn, WT_CONN_RECOVERING);
    WT_ERR(__wt_metadata_search(session, WT_METAFILE_URI, &config));
    WT_ERR(__recovery_setup_file(&r, WT_METAFILE_URI, config));
    WT_ERR(__wt_metadata_cursor_open(session, NULL, &metac));
    metafile = &r.files[WT_METAFILE_ID];
    metafile->c = metac;

    /*
     * If no log was found (including if logging is disabled), or if the last checkpoint was done
     * with logging disabled, recovery should not run. Scan the metadata to figure out the largest
     * file ID.
     */
    if (!FLD_ISSET(conn->log_flags, WT_CONN_LOG_EXISTED) || WT_IS_MAX_LSN(&metafile->ckpt_lsn)) {
        /*
         * Detect if we're going from logging disabled to enabled. We need to know this to verify
         * LSNs and start at the correct log file later. If someone ran with logging, then disabled
         * it and removed all the log files and then turned logging back on, we have to start
         * logging in the log file number that is larger than any checkpoint LSN we have from the
         * earlier time.
         */
        WT_ERR(__recovery_file_scan(&r));
        /*
         * The array can be re-allocated in recovery_file_scan. Reset our pointer after scanning all
         * the files.
         */
        metafile = &r.files[WT_METAFILE_ID];

        if (FLD_ISSET(conn->log_flags, WT_CONN_LOG_ENABLED) && WT_IS_MAX_LSN(&metafile->ckpt_lsn) &&
          !WT_IS_MAX_LSN(&r.max_ckpt_lsn))
            WT_ERR(__wt_log_reset(session, r.max_ckpt_lsn.l.file));
        else
            do_checkpoint = false;
        WT_ERR(__hs_exists(session, metac, cfg, &hs_exists));
        goto done;
    }

    /*
     * First, do a pass through the log to recover the metadata, and establish the last checkpoint
     * LSN. Skip this when opening a hot backup: we already have the correct metadata in that case.
     *
     * If we're running with salvage and we hit an error, we ignore it and continue. In salvage we
     * want to recover whatever part of the data we can from the last checkpoint up until whatever
     * problem we detect in the log file. In salvage, we ignore errors from scanning the log so
     * recovery can continue. Other errors remain errors.
     */
    if (!was_backup) {
        r.metadata_only = true;
        /*
         * If this is a read-only connection, check if the checkpoint LSN in the metadata file is up
         * to date, indicating a clean shutdown.
         */
        if (F_ISSET(conn, WT_CONN_READONLY)) {
            WT_ERR(__wt_log_needs_recovery(session, &metafile->ckpt_lsn, &needs_rec));
            if (needs_rec)
                WT_ERR_MSG(session, WT_RUN_RECOVERY, "Read-only database needs recovery");
        }
        if (WT_IS_INIT_LSN(&metafile->ckpt_lsn))
            ret = __wt_log_scan(session, NULL, NULL, WT_LOGSCAN_FIRST, __txn_log_recover, &r);
        else {
            /*
             * Start at the last checkpoint LSN referenced in the metadata. If we see the end of a
             * checkpoint while scanning, we will change the full scan to start from there.
             */
            WT_ASSIGN_LSN(&r.ckpt_lsn, &metafile->ckpt_lsn);
            ret = __wt_log_scan(session, &metafile->ckpt_lsn, NULL, WT_LOGSCAN_RECOVER_METADATA,
              __txn_log_recover, &r);
        }
        if (F_ISSET(conn, WT_CONN_SALVAGE))
            ret = 0;
        /*
         * If log scan couldn't find a file we expected to be around, this indicates a corruption of
         * some sort.
         */
        if (ret == ENOENT) {
            F_SET(conn, WT_CONN_DATA_CORRUPTION);
            ret = WT_ERROR;
        }

        WT_ERR(ret);
    }

    /* Scan the metadata to find the live files and their IDs. */
    WT_ERR(__recovery_file_scan(&r));

    /*
     * Check whether the history store exists.
     *
     * This will open a dhandle on the history store and initialize its write gen so we must ensure
     * that the connection-wide base write generation is stable at this point. Performing a recovery
     * file scan will involve updating the connection-wide base write generation so we MUST do this
     * before checking for the existence of a history store file.
     */
    WT_ERR(__hs_exists(session, metac, cfg, &hs_exists));

    /*
     * Clear this out. We no longer need it and it could have been re-allocated when scanning the
     * files.
     */
    WT_NOT_READ(metafile, NULL);

    /*
     * We no longer need the metadata cursor: close it to avoid pinning any resources that could
     * block eviction during recovery.
     */
    r.files[0].c = NULL;
    WT_ERR(metac->close(metac));

    /*
     * Now, recover all the files apart from the metadata. Pass WT_LOGSCAN_RECOVER so that old logs
     * get truncated.
     */
    r.metadata_only = false;
    __wt_verbose_multi(session, WT_VERB_RECOVERY_ALL,
      "Main recovery loop: starting at %" PRIu32 "/%" PRIu32 " to %" PRIu32 "/%" PRIu32,
      r.ckpt_lsn.l.file, r.ckpt_lsn.l.offset, r.max_rec_lsn.l.file, r.max_rec_lsn.l.offset);
    WT_ERR(__wt_log_needs_recovery(session, &r.ckpt_lsn, &needs_rec));
    /*
     * Check if the database was shut down cleanly. If not return an error if the user does not want
     * automatic recovery.
     */
    if (needs_rec &&
      (FLD_ISSET(conn->log_flags, WT_CONN_LOG_RECOVER_ERR) || F_ISSET(conn, WT_CONN_READONLY))) {
        if (F_ISSET(conn, WT_CONN_READONLY))
            WT_ERR_MSG(session, WT_RUN_RECOVERY, "Read-only database needs recovery");
        WT_ERR_MSG(session, WT_RUN_RECOVERY, "Database needs recovery");
    }

    if (F_ISSET(conn, WT_CONN_READONLY)) {
        do_checkpoint = false;
        goto done;
    }

    if (!hs_exists) {
        __wt_verbose_multi(session, WT_VERB_RECOVERY_ALL, "%s",
          "Creating the history store before applying log records. Likely recovering after an"
          "unclean shutdown on an earlier version");
        /*
         * Create the history store as we might need it while applying log records in recovery.
         */
        WT_ERR(__wt_hs_open(session, cfg));
    }

    /*
     * Recovery can touch more data than fits in cache, so it relies on regular eviction to manage
     * paging. Start eviction threads for recovery without history store cursors.
     */
    WT_ERR(__wt_evict_create(session));
    eviction_started = true;

    /*
     * Always run recovery even if it was a clean shutdown only if this is not a read-only
     * connection. We can consider skipping it in the future.
     */
    if (needs_rec)
        FLD_SET(conn->log_flags, WT_CONN_LOG_RECOVER_DIRTY);
    if (WT_IS_INIT_LSN(&r.ckpt_lsn))
        ret = __wt_log_scan(
          session, NULL, NULL, WT_LOGSCAN_FIRST | WT_LOGSCAN_RECOVER, __txn_log_recover, &r);
    else
        ret = __wt_log_scan(session, &r.ckpt_lsn, NULL, WT_LOGSCAN_RECOVER, __txn_log_recover, &r);
    if (F_ISSET(conn, WT_CONN_SALVAGE))
        ret = 0;
    WT_ERR(ret);

done:
    /* Close cached cursors, rollback-to-stable asserts exclusive access. */
    WT_ERR(__recovery_close_cursors(&r));
#ifndef WT_STANDALONE_BUILD
    /*
     * There is a known problem with upgrading from release 10.0.0 specifically. There are now fixes
     * that can properly upgrade from 10.0.0 without hitting the problem but only from a clean
     * shutdown of 10.0.0. Earlier releases are not affected by the upgrade issue.
     */
    if (conn->unclean_shutdown && __wt_version_eq(conn->recovery_version, (WT_VERSION){10, 0, 0}))
        WT_ERR_MSG(session, WT_ERROR,
          "Upgrading from a WiredTiger version 10.0.0 database that was not shutdown cleanly is "
          "not allowed. Perform a clean shutdown on version 10.0.0 and then upgrade.");
#endif

    WT_ERR(__recovery_set_checkpoint_timestamp(&r));
    WT_ERR(__recovery_set_oldest_timestamp(&r));
    WT_ERR(__recovery_set_checkpoint_snapshot(session));
    WT_ERR(__recovery_set_ckpt_base_write_gen(&r));

    /*
     * Set the history store file size as it may already exist after a restart.
     */
    if (hs_exists) {
        WT_ERR(__wt_block_manager_named_size(session, WT_HS_FILE, &hs_size));
        WT_STAT_CONN_SET(session, cache_hs_ondisk, hs_size);
    }

    /*
     * Perform rollback to stable only when the following conditions met.
     * 1. The connection is not read-only. A read-only connection expects that there shouldn't be
     *    any changes that need to be done on the database other than reading.
     * 2. The history store file was found in the metadata.
     */
    if (hs_exists && !F_ISSET(conn, WT_CONN_READONLY)) {
        /* Start the eviction threads for rollback to stable if not already started. */
        if (!eviction_started) {
            WT_ERR(__wt_evict_create(session));
            eviction_started = true;
        }

        WT_ASSERT(session,
          conn->txn_global.has_stable_timestamp == false &&
            conn->txn_global.stable_timestamp == WT_TS_NONE);

        /*
         * Set the stable timestamp from recovery timestamp and process the trees for rollback to
         * stable.
         */
        conn->txn_global.stable_timestamp = conn->txn_global.recovery_timestamp;
        conn->txn_global.has_stable_timestamp = false;

        if (conn->txn_global.recovery_timestamp != WT_TS_NONE)
            conn->txn_global.has_stable_timestamp = true;

        __wt_verbose_multi(session,
          WT_DECL_VERBOSE_MULTI_CATEGORY(((WT_VERBOSE_CATEGORY[]){WT_VERB_RECOVERY, WT_VERB_RTS})),
          "performing recovery rollback_to_stable with stable timestamp: %s and oldest timestamp: "
          "%s",
          __wt_timestamp_to_string(conn->txn_global.stable_timestamp, ts_string[0]),
          __wt_timestamp_to_string(conn->txn_global.oldest_timestamp, ts_string[1]));
        rts_executed = true;
        WT_ERR(__wt_rollback_to_stable(session, NULL, true));
    }

    /*
     * Sometimes eviction is triggered after doing a checkpoint. However, we don't want eviction to
     * make the tree dirty after checkpoint as this will interfere with WT_SESSION alter which
     * expects a clean tree.
     */
    if (eviction_started)
        WT_TRET(__wt_evict_destroy(session));

    if (do_checkpoint || rts_executed)
        /*
         * Forcibly log a checkpoint so the next open is fast and keep the metadata up to date with
         * the checkpoint LSN and removal.
         */
        WT_ERR(session->iface.checkpoint(&session->iface, "force=1"));

    /*
     * Update the open dhandles write generations and base write generation with the connection's
     * base write generation because the recovery checkpoint writes the pages to disk with new write
     * generation number which contains transaction ids that are needed to reset later. The
     * connection level base write generation number is updated at the end of the recovery
     * checkpoint.
     */
    __wt_dhandle_update_write_gens(session);

    /*
     * If we're downgrading and have newer log files, force log removal, no matter what the remove
     * setting is.
     */
    if (FLD_ISSET(conn->log_flags, WT_CONN_LOG_FORCE_DOWNGRADE))
        WT_ERR(__wt_log_truncate_files(session, NULL, true));
    FLD_SET(conn->log_flags, WT_CONN_LOG_RECOVER_DONE);

err:
    WT_TRET(__recovery_close_cursors(&r));
    __wt_free(session, config);
    FLD_CLR(conn->log_flags, WT_CONN_LOG_RECOVER_DIRTY);

    if (ret != 0) {
        FLD_SET(conn->log_flags, WT_CONN_LOG_RECOVER_FAILED);
        __wt_err(session, ret, "Recovery failed");
    }

    /*
     * Destroy the eviction threads that were started in support of recovery. They will be restarted
     * once the history store table is created.
     */
    if (eviction_started)
        WT_TRET(__wt_evict_destroy(session));

    WT_TRET(__wt_session_close_internal(session));
    F_CLR(conn, WT_CONN_RECOVERING);

    return (ret);
}
