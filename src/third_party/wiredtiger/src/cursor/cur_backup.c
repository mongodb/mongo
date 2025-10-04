/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __backup_all(WT_SESSION_IMPL *);
static int __backup_list_append(WT_SESSION_IMPL *, WT_CURSOR_BACKUP *, const char *, const char *);
static int __backup_list_uri_append(WT_SESSION_IMPL *, const char *, bool *);
static int __backup_start(
  WT_SESSION_IMPL *, WT_CURSOR_BACKUP *, WT_CURSOR_BACKUP *, const char *[]);
static int __backup_stop(WT_SESSION_IMPL *, WT_CURSOR_BACKUP *);

#define WT_CURSOR_BACKUP_CHECK_STOP(cursor) \
    WT_ERR(F_ISSET(((WT_CURSOR_BACKUP *)(cursor)), WT_CURBACKUP_FORCE_STOP) ? EINVAL : 0);

/*
 * __wt_verbose_dump_backup --
 *     Print out the current state of the in-memory incremental backup structure.
 */
int
__wt_verbose_dump_backup(WT_SESSION_IMPL *session)
{
    WT_BLKINCR *blk;
    WT_CONNECTION_IMPL *conn;
    int i;

    conn = S2C(session);
    WT_RET(__wt_msg(session, "%s", WT_DIVIDER));
    if (!F_ISSET_ATOMIC_32(conn, WT_CONN_INCR_BACKUP)) {
        WT_RET(__wt_msg(session, "No incremental backup information exists"));
        return (0);
    }
    for (i = 0; i < WT_BLKINCR_MAX; ++i) {
        blk = &conn->incr_backups[i];
        if (!F_ISSET(blk, WT_BLKINCR_VALID))
            WT_RET(__wt_msg(session, "Slot %d no backup information exists", i));
        else {
            WT_RET(__wt_msg(session, "Slot %d:", i));
            WT_RET(__wt_msg(session, "    ID: %s", blk->id_str));
            WT_RET(__wt_msg(session, "    granularity: %" PRIu64, blk->granularity));
            WT_RET(__wt_msg(session, "    flags %" PRIx32, blk->flags));
        }
    }
    return (0);
}

/*
 * __wt_backup_set_blkincr --
 *     Given an index set the incremental block element to the given granularity and id string.
 */
int
__wt_backup_set_blkincr(
  WT_SESSION_IMPL *session, uint64_t i, uint64_t granularity, const char *id, uint64_t id_len)
{
    WT_BLKINCR *blkincr;
    WT_CONNECTION_IMPL *conn;

    conn = S2C(session);
    WT_ASSERT(session, i < WT_BLKINCR_MAX);
    blkincr = &conn->incr_backups[i];
    /*
     * NOTE: The granularity exists in the connection because it cannot change today. We may be able
     * to relax that in the future so we also store it in the blkincr structure.
     */
    WT_ASSERT(session, conn->incr_granularity == 0 || conn->incr_granularity == granularity);
    /* Free any id already set. */
    __wt_free(session, blkincr->id_str);
    blkincr->granularity = conn->incr_granularity = granularity;
    WT_STAT_CONN_SET(session, backup_granularity, granularity);
    WT_RET(__wt_strndup(session, id, id_len, &blkincr->id_str));
    WT_CONN_SET_INCR_BACKUP(conn);
    F_SET(blkincr, WT_BLKINCR_VALID);
    return (0);
}

/*
 * __wt_backup_destroy --
 *     Destroy any backup information.
 */
void
__wt_backup_destroy(WT_SESSION_IMPL *session)
{
    WT_BLKINCR *blkincr;
    WT_CONNECTION_IMPL *conn;
    uint64_t i;

    conn = S2C(session);
    /* Free any incremental backup information. */
    for (i = 0; i < WT_BLKINCR_MAX; ++i) {
        blkincr = &conn->incr_backups[i];
        __wt_free(session, blkincr->id_str);
        F_CLR(blkincr, WT_BLKINCR_VALID);
    }
    conn->incr_granularity = 0;
    WT_STAT_CONN_SET(session, backup_incremental, 0);
    F_CLR_ATOMIC_32(conn, WT_CONN_INCR_BACKUP);
}

/*
 * __wt_backup_open --
 *     Restore any incremental backup information. We use the metadata's block information as the
 *     authority on whether incremental backup was in use on last shutdown.
 */
int
__wt_backup_open(WT_SESSION_IMPL *session)
{
    WT_CONFIG blkconf;
    WT_CONFIG_ITEM b, k, v;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    uint64_t i;
    char *config;

    conn = S2C(session);
    config = NULL;

    WT_RET(__wt_metadata_search(session, WT_METAFILE_URI, &config));
    WT_ERR(__wt_config_getones(session, config, "checkpoint_backup_info", &v));
    __wt_config_subinit(session, &blkconf, &v);
    /*
     * Walk each item in the metadata and set up our last known global incremental information.
     */
    F_CLR_ATOMIC_32(conn, WT_CONN_INCR_BACKUP);
    i = 0;
    while (__wt_config_next(&blkconf, &k, &v) == 0) {
        WT_ASSERT(session, i < WT_BLKINCR_MAX);
        /*
         * If we get here, we have at least one valid incremental backup. We want to set up its
         * general configuration in the global table.
         */
        WT_ERR(__wt_config_subgets(session, &v, "granularity", &b));
        WT_ERR(__wt_backup_set_blkincr(session, i++, (uint64_t)b.val, k.str, (uint32_t)k.len));
    }

err:
    if (ret != 0 && ret != WT_NOTFOUND)
        __wt_backup_destroy(session);
    __wt_free(session, config);
    return (ret == WT_NOTFOUND ? 0 : ret);
}

/*
 * __wt_backup_file_remove --
 *     Remove the incremental and meta-data backup files.
 */
int
__wt_backup_file_remove(WT_SESSION_IMPL *session)
{
    WT_DECL_RET;

    WT_TRET(__wt_remove_if_exists(session, WT_BACKUP_TMP, true));
    WT_TRET(__wt_remove_if_exists(session, WT_METADATA_BACKUP, true));
    return (ret);
}

/*
 * __curbackup_next --
 *     WT_CURSOR->next method for the backup cursor type.
 */
static int
__curbackup_next(WT_CURSOR *cursor)
{
    WT_CURSOR_BACKUP *cb;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    cb = (WT_CURSOR_BACKUP *)cursor;
    CURSOR_API_CALL(cursor, session, ret, next, NULL);
    WT_CURSOR_BACKUP_CHECK_STOP(cb);

    if (cb->list == NULL || cb->list[cb->next] == NULL) {
        F_CLR(cursor, WT_CURSTD_KEY_SET);
        WT_ERR(WT_NOTFOUND);
    }

    cb->iface.key.data = cb->list[cb->next];
    cb->iface.key.size = strlen(cb->list[cb->next]) + 1;
    /*
     * If incremental backup and the configuration list exists, move to the next config value in
     * lock-step. The list may not exist for special backup cursors like querying the IDs.
     */
    if (F_ISSET_ATOMIC_32(S2C(session), WT_CONN_INCR_BACKUP) && cb->cfg_list != NULL)
        cb->cfg_current = cb->cfg_list[cb->next];
    ++cb->next;

    F_SET(cursor, WT_CURSTD_KEY_INT);

err:
    API_END_RET(session, ret);
}

/*
 * __curbackup_reset --
 *     WT_CURSOR->reset method for the backup cursor type.
 */
static int
__curbackup_reset(WT_CURSOR *cursor)
{
    WT_CURSOR_BACKUP *cb;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    cb = (WT_CURSOR_BACKUP *)cursor;
    CURSOR_API_CALL_PREPARE_ALLOWED(cursor, session, reset, NULL);
    WT_CURSOR_BACKUP_CHECK_STOP(cb);

    cb->cfg_current = NULL;
    cb->next = 0;
    F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);

err:
    API_END_RET(session, ret);
}

/*
 * __backup_free --
 *     Free list resources for a backup cursor.
 */
static int
__backup_free(WT_SESSION_IMPL *session, WT_CURSOR_BACKUP *cb)
{
    int i;

    /*
     * Some elements of the cfg_list may be NULL while later ones are valid. So walk the entire list
     * and free any entries.
     */
    if (cb->cfg_list != NULL) {
        for (i = 0; i < (int)cb->list_next; ++i)
            __wt_free(session, cb->cfg_list[i]);
        __wt_free(session, cb->cfg_list);
    }
    if (cb->list != NULL) {
        for (i = 0; cb->list[i] != NULL; ++i)
            __wt_free(session, cb->list[i]);
        __wt_free(session, cb->list);
    }
    if (cb->incr_file != NULL)
        __wt_free(session, cb->incr_file);

    return (__wti_curbackup_free_incr(session, cb));
}

/*
 * __curbackup_close --
 *     WT_CURSOR->close method for the backup cursor type.
 */
static int
__curbackup_close(WT_CURSOR *cursor)
{
    WT_CONNECTION_IMPL *conn;
    WT_CURSOR_BACKUP *cb;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    cb = (WT_CURSOR_BACKUP *)cursor;
    CURSOR_API_CALL_PREPARE_ALLOWED(cursor, session, close, NULL);
err:

    conn = S2C(session);
    if (F_ISSET(cb, WT_CURBACKUP_FORCE_STOP)) {
        __wt_verbose(
          session, WT_VERB_BACKUP, "%s", "Releasing resources from forced stop incremental");
        __wt_backup_destroy(session);
    }

    /*
     * For either a force stop or a full backup starting an incremental force a checkpoint so that
     * the new information is visible in the metadata and old backup information does not reappear
     * if we crash and restart.
     */
    if (F_ISSET(cb, WT_CURBACKUP_FORCE_STOP) ||
      (F_ISSET(cb, WT_CURBACKUP_INCR) && cb->incr_src == NULL)) {
        /* Must be declared and initialized after session is set in the CURSOR_API macro. */
        const char *cfg[] = {WT_CONFIG_BASE(session, WT_SESSION_checkpoint), NULL};

        /* Mark the connection modified to make sure a checkpoint happens even on an idle system. */
        conn->modified = true;
        WT_TRET(__wt_checkpoint_db(session, cfg, true));
    }
    /* Clear the flag on force stop after the completion of the checkpoint. */
    if (F_ISSET(cb, WT_CURBACKUP_FORCE_STOP))
        F_CLR(&conn->log_mgr, WT_LOG_INCR_BACKUP);

    /*
     * When starting a hot backup, we serialize hot backup cursors and set the connection's
     * hot-backup flag. Once that's done, we set the cursor's backup-locker flag, implying the
     * cursor owns all necessary cleanup (including removing temporary files), regardless of error
     * or success. The cursor's backup-locker flag is never cleared (it's just discarded when the
     * cursor is closed), because that cursor will never not be responsible for cleanup.
     */
    if (F_ISSET(cb, WT_CURBACKUP_DUP)) {
        WT_TRET(__backup_free(session, cb));
        /* Make sure the original backup cursor is still open. */
        WT_ASSERT(session, F_ISSET(session, WT_SESSION_BACKUP_CURSOR));
        F_CLR(session, WT_SESSION_BACKUP_DUP);
        F_CLR(cb, WT_CURBACKUP_DUP);
        WT_STAT_CONN_SET(session, backup_dup_open, 0);
    } else if (F_ISSET(cb, WT_CURBACKUP_LOCKER))
        WT_TRET(__backup_stop(session, cb));

    __wt_cursor_close(cursor);
    session->bkp_cursor = NULL;
    WT_STAT_CONN_SET(session, backup_cursor_open, 0);

    API_END_RET(session, ret);
}

/*
 * __wt_curbackup_open --
 *     WT_SESSION->open_cursor method for the backup cursor type.
 */
int
__wt_curbackup_open(WT_SESSION_IMPL *session, const char *uri, WT_CURSOR *other, const char *cfg[],
  WT_CURSOR **cursorp)
{
    WT_CURSOR_STATIC_INIT(iface, __wt_cursor_get_key, /* get-key */
      __wti_cursor_get_value_notsup,                  /* get-value */
      __wti_cursor_get_raw_key_value_notsup,          /* get-raw-key-value */
      __wti_cursor_set_key_notsup,                    /* set-key */
      __wti_cursor_set_value_notsup,                  /* set-value */
      __wti_cursor_compare_notsup,                    /* compare */
      __wti_cursor_equals_notsup,                     /* equals */
      __curbackup_next,                               /* next */
      __wt_cursor_notsup,                             /* prev */
      __curbackup_reset,                              /* reset */
      __wt_cursor_notsup,                             /* search */
      __wt_cursor_search_near_notsup,                 /* search-near */
      __wt_cursor_notsup,                             /* insert */
      __wt_cursor_modify_notsup,                      /* modify */
      __wt_cursor_notsup,                             /* update */
      __wt_cursor_notsup,                             /* remove */
      __wt_cursor_notsup,                             /* reserve */
      __wt_cursor_config_notsup,                      /* reconfigure */
      __wt_cursor_notsup,                             /* largest_key */
      __wt_cursor_config_notsup,                      /* bound */
      __wt_cursor_notsup,                             /* cache */
      __wt_cursor_reopen_notsup,                      /* reopen */
      __wt_cursor_checkpoint_id,                      /* checkpoint ID */
      __curbackup_close);                             /* close */
    WT_CURSOR *cursor;
    WT_CURSOR_BACKUP *cb, *othercb;
    WT_DECL_RET;
    size_t uri_len;

    WT_VERIFY_OPAQUE_POINTER(WT_CURSOR_BACKUP);

    WT_RET(__wt_calloc_one(session, &cb));
    cursor = (WT_CURSOR *)cb;
    *cursor = iface;
    cursor->session = (WT_SESSION *)session;
    cursor->key_format = "S";  /* Return the file names as the key. */
    cursor->value_format = ""; /* No value, for now. */
    uri_len = strlen(uri);

    session->bkp_cursor = cb;
    othercb = (WT_CURSOR_BACKUP *)other;
    if (othercb != NULL)
        WT_CURSOR_BACKUP_CHECK_STOP(othercb);

    if (cfg != NULL && cfg[1] != NULL)
        __wt_verbose(session, WT_VERB_BACKUP, "Backup cursor config \"%s\"", cfg[1]);

    /* Special backup cursor to query incremental IDs. */
    if (WT_STRING_LIT_MATCH("backup:query_id", uri, uri_len)) {
        /* Top level cursor code does not allow a URI and cursor. We don't need to check here. */
        WT_ASSERT(session, othercb == NULL);
        if (!F_ISSET_ATOMIC_32(S2C(session), WT_CONN_INCR_BACKUP))
            WT_ERR_MSG(session, EINVAL, "Incremental backup is not configured");
        F_SET(cb, WT_CURBACKUP_QUERYID);
    } else if (WT_STRING_LIT_MATCH("backup:export", uri, uri_len))
        /* Special backup cursor for export operation. */
        F_SET(cb, WT_CURBACKUP_EXPORT);

    /*
     * Export cursors are for tiered storage. Do not allow backup cursors if tiered storage is in
     * use on the connection and it isn't an export cursor.
     */
    if (WT_CONN_TIERED_STORAGE_ENABLED(S2C(session)) && !F_ISSET(cb, WT_CURBACKUP_EXPORT))
        WT_ERR(ENOTSUP);

    /*
     * Start the backup and fill in the cursor's list. Acquire the schema lock, we need a consistent
     * view when creating a copy. We only need the locks when opening the top-level backup cursor.
     * We do not need them when opening a duplicate backup cursor.
     */
    WT_STAT_CONN_SET(session, backup_start, 1);
    if (othercb == NULL) {
        WT_WITH_CHECKPOINT_LOCK(
          session, WT_WITH_SCHEMA_LOCK(session, ret = __backup_start(session, cb, othercb, cfg)));
        WT_ERR(ret);
    } else
        WT_ERR(__backup_start(session, cb, othercb, cfg));
    WT_ERR(cb->incr_file == NULL ?
        __wt_cursor_init(cursor, uri, NULL, cfg, cursorp) :
        __wti_curbackup_open_incr(session, uri, other, cursor, cfg, cursorp));

    WT_STAT_CONN_SET(session, backup_cursor_open, 1);
    if (0) {
err:
        WT_TRET(__curbackup_close(cursor));
        *cursorp = NULL;
    }
    WT_STAT_CONN_SET(session, backup_start, 0);

    return (ret);
}

/*
 * __backup_add_id --
 *     Add the identifier for block based incremental backup.
 */
static int
__backup_add_id(WT_SESSION_IMPL *session, WT_CONFIG_ITEM *cval)
{
    WT_BLKINCR *blk;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    u_int i;
    const char *ckpt;

    conn = S2C(session);
    blk = NULL;
    for (i = 0; i < WT_BLKINCR_MAX; ++i) {
        blk = &conn->incr_backups[i];
        /* If it isn't already in use, we can use it. */
        if (!F_ISSET(blk, WT_BLKINCR_INUSE)) {
            __wt_verbose_debug2(session, WT_VERB_BACKUP, "Free blk[%u] entry", i);
            break;
        }
        __wt_verbose_debug2(
          session, WT_VERB_BACKUP, "Entry blk[%u] has flags 0x%" PRIx8, i, blk->flags);
    }
    /*
     * We didn't find an entry. This should not happen.
     */
    if (i == WT_BLKINCR_MAX)
        WT_RET_PANIC(session, WT_NOTFOUND, "Could not find an incremental backup slot to use");

    /* Use the slot. */
    if (blk->id_str != NULL)
        __wt_verbose_debug2(
          session, WT_VERB_BACKUP, "Freeing and reusing backup slot with old id %s", blk->id_str);

    /* Set up with the information. */
    WT_ERR(__wt_backup_set_blkincr(session, i, conn->incr_granularity, cval->str, cval->len));
    /*
     * Get the most recent checkpoint name. For now just use the one that is part of the metadata.
     * We only care whether or not a checkpoint exists, so immediately free it.
     */
    ret = __wt_meta_checkpoint_last_name(session, WT_METAFILE_URI, &ckpt, NULL, NULL);
    __wt_free(session, ckpt);
    WT_ERR_NOTFOUND_OK(ret, true);
    if (ret == WT_NOTFOUND) {
        /*
         * If we don't find any checkpoint, backup files need to be full copy.
         */
        __wt_verbose(session, WT_VERB_BACKUP,
          "Backup id %s: Did not find any metadata checkpoint for %s.", blk->id_str,
          WT_METAFILE_URI);
        F_SET(blk, WT_BLKINCR_FULL);
    } else {
        __wt_verbose(session, WT_VERB_BACKUP, "Backup id %s using backup slot %u", blk->id_str, i);
        F_CLR(blk, WT_BLKINCR_FULL);
    }
    return (0);

err:
    __wt_free(session, blk->id_str);
    return (ret);
}

/*
 * __backup_find_id --
 *     Find the source identifier for block based incremental backup. Error if it is not a valid id.
 */
static int
__backup_find_id(WT_SESSION_IMPL *session, WT_CONFIG_ITEM *cval, WT_BLKINCR **incrp)
{
    WT_BLKINCR *blk;
    WT_CONNECTION_IMPL *conn;
    u_int i;

    conn = S2C(session);
    WT_RET(__wt_name_check(session, cval->str, cval->len, false));
    for (i = 0; i < WT_BLKINCR_MAX; ++i) {
        blk = &conn->incr_backups[i];
        /* If it isn't valid, skip it. */
        if (!F_ISSET(blk, WT_BLKINCR_VALID))
            continue;
        if (WT_CONFIG_MATCH(blk->id_str, *cval)) {
            if (F_ISSET(blk, WT_BLKINCR_INUSE))
                WT_RET_MSG(session, EINVAL, "Incremental backup structure already in use");
            if (incrp != NULL)
                *incrp = blk;
            __wt_verbose_debug2(
              session, WT_VERB_BACKUP, "Found src id %s at backup slot %u", blk->id_str, i);
            return (0);
        }
    }
    __wt_verbose_debug2(
      session, WT_VERB_BACKUP, "Search %.*s not found", (int)cval->len, cval->str);
    return (WT_NOTFOUND);
}

/*
 * __backup_log_append --
 *     Append log files needed for backup.
 */
static int
__backup_log_append(WT_SESSION_IMPL *session, WT_CURSOR_BACKUP *cb, bool active)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    u_int i, logcount;
    char **logfiles;

    conn = S2C(session);
    logfiles = NULL;
    logcount = 0;
    ret = 0;

    if (conn->log_mgr.log) {
        WT_ERR(__wt_log_get_backup_files(session, &logfiles, &logcount, &cb->maxid, active));
        for (i = 0; i < logcount; i++)
            WT_ERR(__backup_list_append(session, cb, logfiles[i], NULL));
    }
err:
    WT_TRET(__wt_fs_directory_list_free(session, &logfiles, logcount));
    return (ret);
}

/*
 * __backup_config --
 *     Backup configuration.
 *
 * NOTE: this function handles all of the backup configuration except for the incremental use of
 *     force_stop. That is handled at the beginning of __backup_start because we want to deal with
 *     that setting without any of the other cursor setup.
 */
static int
__backup_config(WT_SESSION_IMPL *session, WT_CURSOR_BACKUP *cb, const char *cfg[],
  WT_CURSOR_BACKUP *othercb, bool *foundp)
{
    WT_CONFIG targetconf;
    WT_CONFIG_ITEM cval, k, v;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;
    const char *uri;
    bool consolidate, incremental_config, is_dup, log_config, target_list;

    *foundp = false;

    conn = S2C(session);
    incremental_config = log_config = false;
    is_dup = othercb != NULL;

    if (!is_dup) {
        WT_ASSERT_SPINLOCK_OWNED(session, &conn->checkpoint_lock);
        WT_ASSERT_SPINLOCK_OWNED(session, &conn->schema_lock);
    }

    /*
     * Per-file offset incremental hot backup configurations take a starting checkpoint and optional
     * maximum transfer size, and the subsequent duplicate cursors take a file object.
     */
    WT_RET_NOTFOUND_OK(__wt_config_gets(session, cfg, "incremental.enabled", &cval));
    if (cval.val) {
        /* Granularity can only be set once at the beginning */
        if (!F_ISSET_ATOMIC_32(conn, WT_CONN_INCR_BACKUP)) {
            WT_RET(__wt_config_gets(session, cfg, "incremental.granularity", &cval));
            if (conn->incr_granularity != 0)
                WT_RET_MSG(session, EINVAL, "Cannot change the incremental backup granularity");
            conn->incr_granularity = (uint64_t)cval.val;
            __wt_verbose(session, WT_VERB_BACKUP, "Backup config set granularity value %" PRIu64,
              conn->incr_granularity);
        }
        WT_CONN_SET_INCR_BACKUP(conn);
        incremental_config = true;
    }

    /*
     * Consolidation can be on a per incremental basis or a per-file duplicate cursor basis.
     */
    WT_RET(__wt_config_gets(session, cfg, "incremental.consolidate", &cval));
    consolidate = F_MASK(cb, WT_CURBACKUP_CONSOLIDATE);
    if (cval.val) {
        if (is_dup)
            WT_RET_MSG(session, EINVAL,
              "Incremental consolidation can only be specified on a primary backup cursor");
        F_SET(cb, WT_CURBACKUP_CONSOLIDATE);
        incremental_config = true;
    }

    /*
     * Specifying an incremental file means we're opening a duplicate backup cursor.
     */
    WT_RET(__wt_config_gets(session, cfg, "incremental.file", &cval));
    if (cval.len != 0) {
        if (!is_dup)
            WT_RET_MSG(session, EINVAL,
              "Incremental file name can only be specified on a duplicate backup cursor");
        WT_RET(__wt_strndup(session, cval.str, cval.len, &cb->incr_file));
        incremental_config = true;
    }

    /*
     * See if we have a source identifier. We must process the source identifier before processing
     * the 'this' identifier. That will mark which source is in use so that we can use any slot that
     * is not in use as a new source starting point for this identifier.
     */
    WT_RET(__wt_config_gets(session, cfg, "incremental.src_id", &cval));
    if (cval.len != 0) {
        if (is_dup)
            WT_RET_MSG(session, EINVAL,
              "Incremental source identifier can only be specified on a primary backup cursor");
        WT_RET(__backup_find_id(session, &cval, &cb->incr_src));
        F_SET(cb->incr_src, WT_BLKINCR_INUSE);
        incremental_config = true;
    }
    /*
     * Use WT_ERR from here out because we need to clear the in use flag on error now.
     */

    /*
     * Look for a new checkpoint name to retain and mark as a starting point.
     */
    WT_ERR(__wt_config_gets(session, cfg, "incremental.this_id", &cval));
    if (cval.len != 0) {
        if (is_dup)
            WT_ERR_MSG(session, EINVAL,
              "Incremental identifier can only be specified on a primary backup cursor");
        WT_ERR_NOTFOUND_OK(__backup_find_id(session, &cval, NULL), true);
        if (ret == 0)
            WT_ERR_MSG(session, EINVAL, "Incremental identifier already exists");

        WT_ERR(__backup_add_id(session, &cval));
        incremental_config = true;
    }

    /*
     * If we find a non-empty target configuration string, we have a job, otherwise it's not our
     * problem.
     */
    WT_ERR(__wt_config_gets(session, cfg, "target", &cval));
    __wt_config_subinit(session, &targetconf, &cval);
    for (target_list = false; (ret = __wt_config_next(&targetconf, &k, &v)) == 0;
         target_list = true) {
        /* If it is our first time through, allocate. */
        if (!target_list) {
            *foundp = true;
            WT_ERR(__wt_scr_alloc(session, 512, &tmp));
        }

        WT_ERR(__wt_buf_fmt(session, tmp, "%.*s", (int)k.len, k.str));
        uri = tmp->data;
        if (v.len != 0)
            WT_ERR_MSG(session, EINVAL, "%s: invalid backup target: URIs may need quoting", uri);

        /*
         * Handle log targets. We do not need to go through the schema worker, just call the
         * function to append them.
         */
        if (WT_PREFIX_MATCH(uri, "log:")) {
            log_config = true;
            WT_ERR(__backup_log_append(session, session->bkp_cursor, false));
        } else if (is_dup)
            WT_ERR_MSG(
              session, EINVAL, "duplicate backup cursor cannot be used for non-log target");
        else {
            /*
             * If backing up individual tables, we have to include indexes, which may involve
             * opening those indexes. Acquire the table lock in write mode for that case.
             */
            WT_WITH_TABLE_WRITE_LOCK(session,
              ret = __wt_schema_worker(session, uri, NULL, __backup_list_uri_append, cfg, 0));
            WT_ERR(ret);
        }
    }
    WT_ERR_NOTFOUND_OK(ret, false);

    /*
     * Compatibility checking.
     *
     * Duplicate backup cursors are only for log targets or block-based incremental backups. But log
     * targets don't make sense with block-based incremental backup.
     */
    if (!is_dup && log_config && F_ISSET(&conn->log_mgr, WT_LOG_REMOVE))
        WT_ERR_MSG(session, EINVAL,
          "incremental log file backup not possible when automatic log removal configured");
    if (is_dup && (!incremental_config && !log_config))
        WT_ERR_MSG(session, EINVAL,
          "duplicate backup cursor must be for block-based incremental or logging backup");
    if (incremental_config && (log_config || target_list))
        WT_ERR_MSG(
          session, EINVAL, "block-based incremental backup incompatible with a list of targets");

    if (incremental_config) {
        if (is_dup && !F_ISSET(othercb, WT_CURBACKUP_INCR))
            WT_ERR_MSG(session, EINVAL,
              "Incremental duplicate cursor must have an incremental primary backup cursor");
        if (is_dup && othercb->incr_src == NULL)
            WT_ERR_MSG(
              session, EINVAL, "Incremental primary cursor must have a known source identifier");
        F_SET(cb, WT_CURBACKUP_INCR);
    }

err:
    if (ret != 0 && cb->incr_src != NULL) {
        F_CLR(cb->incr_src, WT_BLKINCR_INUSE);
        F_CLR(cb, WT_CURBACKUP_CONSOLIDATE);
        F_SET(cb, consolidate);
    }
    __wt_scr_free(session, &tmp);
    return (ret);
}

/*
 * __backup_query_setup --
 *     Setup the names to return with a backup query cursor.
 */
static int
__backup_query_setup(WT_SESSION_IMPL *session, WT_CURSOR_BACKUP *cb)
{
    WT_BLKINCR *blkincr;
    u_int i;

    for (i = 0; i < WT_BLKINCR_MAX; ++i) {
        blkincr = &S2C(session)->incr_backups[i];
        /* If it isn't valid, skip it. */
        if (!F_ISSET(blkincr, WT_BLKINCR_VALID))
            continue;
        WT_RET(__backup_list_append(session, cb, blkincr->id_str, NULL));
    }
    return (0);
}

/*
 * __backup_start --
 *     Start a backup.
 */
static int
__backup_start(
  WT_SESSION_IMPL *session, WT_CURSOR_BACKUP *cb, WT_CURSOR_BACKUP *othercb, const char *cfg[])
{
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_FSTREAM *srcfs;
    const char *dest;
    bool exist, is_dup, target_list;

    conn = S2C(session);
    srcfs = NULL;
    dest = NULL;
    is_dup = othercb != NULL;

    if (!is_dup) {
        WT_ASSERT_SPINLOCK_OWNED(session, &conn->checkpoint_lock);
        WT_ASSERT_SPINLOCK_OWNED(session, &conn->schema_lock);
    }

    cb->next = 0;
    cb->cfg_current = NULL;
    cb->cfg_list = NULL;
    cb->list = NULL;
    cb->list_next = 0;

    WT_RET(__wt_inmem_unsupported_op(session, "backup cursor"));

    /*
     * Single thread hot backups: we're holding the schema lock, so we know we'll serialize with
     * other attempts to start a hot backup.
     */
    if (__wt_atomic_load64(&conn->hot_backup_start) != 0 && !is_dup)
        WT_RET_MSG(session, EINVAL, "there is already a backup cursor open");

    if (F_ISSET(session, WT_SESSION_BACKUP_DUP) && is_dup)
        WT_RET_MSG(session, EINVAL, "there is already a duplicate backup cursor open");

    /*
     * We want to check for forced stopping early before we do anything else. If it is set, we just
     * set a flag and we're done. Actions will be performed on cursor close.
     */
    WT_RET_NOTFOUND_OK(__wt_config_gets(session, cfg, "incremental.force_stop", &cval));
    if (!F_ISSET(cb, WT_CURBACKUP_QUERYID) && cval.val) {
        /*
         * If we're force stopping incremental backup, set the flag. The resources involved in
         * incremental backup will be released on cursor close and that is the only expected usage
         * for this cursor.
         */
        if (is_dup)
            WT_RET_MSG(session, EINVAL,
              "Incremental force stop can only be specified on a primary backup cursor");
        F_SET(cb, WT_CURBACKUP_FORCE_STOP);
        return (0);
    }

    if (!is_dup) {
        /*
         * The hot backup copy is done outside of WiredTiger, which means file blocks can't be freed
         * and re-allocated until the backup completes. The checkpoint code checks the backup flag,
         * and if a backup cursor is open checkpoints aren't discarded. We release the lock as soon
         * as we've set the flag, we don't want to block checkpoints, we just want to make sure no
         * checkpoints are deleted. The checkpoint code holds the lock until it's finished the
         * checkpoint, otherwise we could start a hot backup that would race with an already-started
         * checkpoint.
         *
         * We are holding the checkpoint and schema locks so schema operations will not see the
         * backup file list until it is complete and valid.
         */
        WT_WITH_HOTBACKUP_WRITE_LOCK(session, WT_CONN_HOTBACKUP_START(conn));

        /* We're the lock holder, we own cleanup. */
        F_SET(cb, WT_CURBACKUP_LOCKER);
        /*
         * If we are a query backup cursor there are no configuration settings and it will set up
         * its own list of strings to return. We don't have to do any of the other processing. A
         * query creates a list to return but does not create the backup file. After appending the
         * list of IDs we are done.
         */
        if (F_ISSET(cb, WT_CURBACKUP_QUERYID)) {
            ret = __backup_query_setup(session, cb);
            goto query_done;
        }
        /*
         * Create a temporary backup file. This must be opened before generating the list of targets
         * in backup_config. This file will later be renamed to the correct name depending on
         * whether or not we're doing an incremental backup. We need a temp file so that if we fail
         * or crash while filling it, the existence of a partial file doesn't confuse restarting in
         * the source database.
         */
        WT_ERR(__wt_fopen(session, WT_BACKUP_TMP, WT_FS_OPEN_CREATE, WT_STREAM_WRITE, &cb->bfs));
    }

    /*
     * If targets were specified, add them to the list. Otherwise it is a full backup, add all
     * database objects and log files to the list.
     */
    target_list = false;
    WT_ERR(__backup_config(session, cb, cfg, othercb, &target_list));
    /*
     * For a duplicate cursor, all the work is done in backup_config.
     */
    if (is_dup) {
        F_SET(cb, WT_CURBACKUP_DUP);
        F_SET(session, WT_SESSION_BACKUP_DUP);
        WT_STAT_CONN_SET(session, backup_dup_open, 1);
        goto done;
    }
    if (!target_list) {
        /*
         * It's important to first gather the log files to be copied (which internally starts a new
         * log file), followed by choosing a checkpoint to reference in the WiredTiger.backup file.
         *
         * Applications may have logic that takes a checkpoint, followed by performing a write that
         * should only appear in the new checkpoint. This ordering prevents choosing the prior
         * checkpoint, but including the write in the log files returned.
         *
         * It is also possible, and considered legal, to choose the new checkpoint, but not include
         * the log file that contains the log entry for taking the new checkpoint.
         */
        WT_ERR(__backup_log_append(session, cb, true));
        WT_ERR(__backup_all(session));
    }

    /* Add the hot backup and standard WiredTiger files to the list. */
    dest = F_ISSET(cb, WT_CURBACKUP_EXPORT) ? WT_EXPORT_BACKUP : WT_METADATA_BACKUP;
    WT_ERR(__backup_list_append(session, cb, dest, NULL));
    WT_ERR(__wt_fs_exist(session, WT_BASECONFIG, &exist));
    if (exist)
        WT_ERR(__backup_list_append(session, cb, WT_BASECONFIG, NULL));
    WT_ERR(__wt_fs_exist(session, WT_USERCONFIG, &exist));
    if (exist)
        WT_ERR(__backup_list_append(session, cb, WT_USERCONFIG, NULL));
    WT_ERR(__backup_list_append(session, cb, WT_WIREDTIGER, NULL));

query_done:
err:
    /* Close the hot backup file. */
    if (srcfs != NULL)
        WT_TRET(__wt_fclose(session, &srcfs));

    /*
     * Sync and rename the temp file into place.
     */
    WT_TRET(__wt_fs_exist(session, WT_BACKUP_TMP, &exist));
    if (ret == 0 && exist)
        ret = __wt_sync_and_rename(session, &cb->bfs, WT_BACKUP_TMP, dest);
    if (ret == 0) {
        WT_WITH_HOTBACKUP_WRITE_LOCK(session, conn->hot_backup_list = cb->list);
        F_SET(session, WT_SESSION_BACKUP_CURSOR);
    }
    /*
     * If the file hasn't been closed, do it now.
     */
    if (cb->bfs != NULL)
        WT_TRET(__wt_fclose(session, &cb->bfs));

done:
    return (ret);
}

/*
 * __backup_stop --
 *     Stop a backup.
 */
static int
__backup_stop(WT_SESSION_IMPL *session, WT_CURSOR_BACKUP *cb)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;

    conn = S2C(session);

    /* Release all btree names held by the backup. */
    WT_ASSERT(session, !F_ISSET(cb, WT_CURBACKUP_DUP));
    /* If it's not a dup backup cursor, make sure one isn't open. */
    WT_ASSERT(session, !F_ISSET(session, WT_SESSION_BACKUP_DUP));
    WT_WITH_HOTBACKUP_WRITE_LOCK(session, conn->hot_backup_list = NULL);
    if (cb->incr_src != NULL)
        F_CLR(cb->incr_src, WT_BLKINCR_INUSE);
    WT_TRET(__backup_free(session, cb));

    /* Remove any backup specific file. */
    WT_TRET(__wt_backup_file_remove(session));

    /* Remove the export file only when we close the backup cursor. */
    WT_TRET(__wt_remove_if_exists(session, WT_EXPORT_BACKUP, true));

    /* Checkpoint deletion and next hot backup can proceed. */
    WT_WITH_HOTBACKUP_WRITE_LOCK(session, __wt_atomic_store64(&conn->hot_backup_start, 0));
    F_CLR(session, WT_SESSION_BACKUP_CURSOR);

    return (ret);
}

/*
 * __backup_all --
 *     Backup all objects in the database.
 */
static int
__backup_all(WT_SESSION_IMPL *session)
{
    /* Build a list of the file objects that need to be copied. */
    return (__wt_meta_apply_all(session, NULL, __backup_list_uri_append, NULL));
}

/*
 * __backup_list_uri_append --
 *     Append a new file name to the list, allocate space as necessary. Called via the schema_worker
 *     function.
 */
static int
__backup_list_uri_append(WT_SESSION_IMPL *session, const char *name, bool *skip)
{
    WT_CURSOR_BACKUP *cb;
    WT_DECL_RET;
    char *value;

    cb = session->bkp_cursor;
    WT_UNUSED(skip);

    /*
     * While reading the metadata file, check there are no data sources that can't support hot
     * backup. This checks for a data source that's non-standard, which can't be backed up, but is
     * also sanity checking: if there's an entry backed by anything other than a file, we're
     * confused.
     */
    if (!WT_PREFIX_MATCH(name, "file:") && !WT_PREFIX_MATCH(name, "colgroup:") &&
      !WT_PREFIX_MATCH(name, "index:") && !WT_PREFIX_MATCH(name, "object:") &&
      !WT_PREFIX_MATCH(name, WT_SYSTEM_PREFIX) && !WT_PREFIX_MATCH(name, "table:") &&
      !WT_PREFIX_MATCH(name, "tier:") && !WT_PREFIX_MATCH(name, "tiered:"))
        WT_RET_MSG(session, ENOTSUP, "hot backup is not supported for objects of type %s", name);

    /* Add the metadata entry to the backup file. */
    WT_RET(__wt_metadata_search(session, name, &value));

    WT_ERR(__wt_live_restore_clean_metadata_string(session, value));

    WT_ERR(__wt_fprintf(session, cb->bfs, "%s\n%s\n", name, value));
    /*
     * We want to retain the system information in the backup metadata file above, but there is no
     * file object to copy so return now.
     */
    if (WT_PREFIX_MATCH(name, WT_SYSTEM_PREFIX))
        goto err;

    /* Add file type objects to the list of files to be copied. */
    if (WT_PREFIX_MATCH(name, "file:"))
        WT_ERR(__backup_list_append(session, cb, name, value));

err:
    __wt_free(session, value);
    return (ret);
}

/*
 * __backup_list_append --
 *     Append a new file name to the list, allocate space as necessary.
 */
static int
__backup_list_append(
  WT_SESSION_IMPL *session, WT_CURSOR_BACKUP *cb, const char *uri, const char *cfg_value)
{
    char **c, **p;
    const char *name;

    c = NULL;
    /* Leave a NULL at the end to mark the end of the list. */
    WT_RET(__wt_realloc_def(session, &cb->list_allocated, cb->list_next + 2, &cb->list));
    p = &cb->list[cb->list_next];
    p[0] = p[1] = NULL;
    if (F_ISSET_ATOMIC_32(S2C(session), WT_CONN_INCR_BACKUP)) {
        /*
         * Add a copy of the metadata config string for tables for incremental backup if one is
         * available. Keep that list in parallel to the file list. Not all files will have the
         * configuration available.
         */
        WT_RET(__wt_realloc_def(session, &cb->cfg_allocated, cb->list_next + 2, &cb->cfg_list));
        c = &cb->cfg_list[cb->list_next];
        c[0] = c[1] = NULL;
    }

    name = uri;

    /*
     * If it's a file in the database we need to remove the prefix.
     */
    if (WT_PREFIX_MATCH(uri, "file:"))
        name += strlen("file:");

    /*
     * !!!
     * Assumes metadata file entries map one-to-one to physical files.
     * To support a block manager where that's not the case, we'd need
     * to call into the block manager and get a list of physical files
     * that map to this logical "file".  I'm not going to worry about
     * that for now, that block manager might not even support physical
     * copying of files by applications.
     */
    WT_RET(__wt_strdup(session, name, p));
    if (F_ISSET_ATOMIC_32(S2C(session), WT_CONN_INCR_BACKUP)) {
        if (cfg_value != NULL)
            WT_RET(__wt_strdup(session, cfg_value, c));
        else
            *c = NULL;
    }
    ++cb->list_next;

    return (0);
}
