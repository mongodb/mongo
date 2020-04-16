/*-
 * Copyright (c) 2014-2020 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_backup_load_incr --
 *     Load the incremental.
 */
int
__wt_backup_load_incr(
  WT_SESSION_IMPL *session, WT_CONFIG_ITEM *blkcfg, WT_ITEM *bitstring, uint64_t nbits)
{
    if (blkcfg->len != 0)
        WT_RET(__wt_nhex_to_raw(session, blkcfg->str, blkcfg->len, bitstring));
    if (bitstring->size != (nbits >> 3))
        WT_RET_MSG(session, WT_ERROR, "corrupted modified block list");
    return (0);
}

/*
 * __curbackup_incr_blkmod --
 *     Get the block modifications for a tree from its metadata and fill in the backup cursor's
 *     information with it.
 */
static int
__curbackup_incr_blkmod(WT_SESSION_IMPL *session, WT_BTREE *btree, WT_CURSOR_BACKUP *cb)
{
    WT_CONFIG blkconf;
    WT_CONFIG_ITEM b, k, v;
    WT_DECL_RET;
    char *config;

    WT_ASSERT(session, btree != NULL);
    WT_ASSERT(session, btree->dhandle != NULL);
    WT_ASSERT(session, cb->incr_src != NULL);

    WT_RET(__wt_metadata_search(session, btree->dhandle->name, &config));
    WT_ERR(__wt_config_getones(session, config, "checkpoint_backup_info", &v));
    __wt_config_subinit(session, &blkconf, &v);
    while ((ret = __wt_config_next(&blkconf, &k, &v)) == 0) {
        /*
         * First see if we have information for this source identifier.
         */
        if (WT_STRING_MATCH(cb->incr_src->id_str, k.str, k.len) == 0)
            continue;

        /*
         * We found a match. If we have a name, then there should be granularity and nbits. The
         * granularity should be set to something. But nbits may be 0 if there are no blocks
         * currently modified.
         */
        WT_ERR(__wt_config_subgets(session, &v, "granularity", &b));
        cb->granularity = (uint64_t)b.val;
        WT_ERR(__wt_config_subgets(session, &v, "nbits", &b));
        cb->nbits = (uint64_t)b.val;
        WT_ERR(__wt_config_subgets(session, &v, "offset", &b));
        cb->offset = (uint64_t)b.val;

        /*
         * We found a match. Load the block information into the cursor.
         */
        ret = __wt_config_subgets(session, &v, "blocks", &b);
        if (ret != WT_NOTFOUND) {
            WT_ERR(__wt_backup_load_incr(session, &b, &cb->bitstring, cb->nbits));
            cb->bit_offset = 0;
            cb->incr_init = true;
        }
    }
    WT_ERR_NOTFOUND_OK(ret, false);

err:
    __wt_free(session, config);
    return (ret == WT_NOTFOUND ? 0 : ret);
}

/*
 * __curbackup_incr_next --
 *     WT_CURSOR->next method for the btree cursor type when configured with incremental_backup.
 */
static int
__curbackup_incr_next(WT_CURSOR *cursor)
{
    WT_BTREE *btree;
    WT_CURSOR_BACKUP *cb;
    WT_DECL_ITEM(buf);
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    wt_off_t size;
    uint32_t raw;
    const char *file;

    cb = (WT_CURSOR_BACKUP *)cursor;
    btree = cb->incr_cursor == NULL ? NULL : ((WT_CURSOR_BTREE *)cb->incr_cursor)->btree;
    raw = F_MASK(cursor, WT_CURSTD_RAW);
    CURSOR_API_CALL(cursor, session, get_value, btree);
    F_CLR(cursor, WT_CURSTD_RAW);

    if (!cb->incr_init && (btree == NULL || F_ISSET(cb, WT_CURBACKUP_FORCE_FULL))) {
        /*
         * We don't have this object's incremental information or it's a forced file copy. If this
         * is a log file, use the full pathname that may include the log path.
         */
        file = cb->incr_file;
        if (WT_PREFIX_MATCH(file, WT_LOG_FILENAME)) {
            WT_ERR(__wt_scr_alloc(session, 0, &buf));
            WT_ERR(__wt_log_filename(session, UINT32_MAX, file, buf));
            file = buf->data;
        }
        WT_ERR(__wt_fs_size(session, file, &size));

        cb->nbits = 0;
        cb->offset = 0;
        cb->bit_offset = 0;
        /*
         * By setting this to true, the next call will detect we're done in the code for the
         * incremental cursor below and return WT_NOTFOUND.
         */
        cb->incr_init = true;
        __wt_cursor_set_key(cursor, 0, size, WT_BACKUP_FILE);
    } else {
        if (cb->incr_init) {
            /* Look for the next chunk that had modifications.  */
            while (cb->bit_offset < cb->nbits)
                if (__bit_test(cb->bitstring.mem, cb->bit_offset))
                    break;
                else
                    ++cb->bit_offset;

            /* We either have this object's incremental information or we're done. */
            if (cb->bit_offset >= cb->nbits)
                WT_ERR(WT_NOTFOUND);
        } else {
            /*
             * We don't have this object's incremental information, and it's not a full file copy.
             * Get a list of the block modifications for the file. The block modifications are from
             * the incremental identifier starting point. Walk the list looking for one with a
             * source of our id.
             */
            WT_ERR(__curbackup_incr_blkmod(session, btree, cb));
            /*
             * If there is no block modification information for this file, there is no information
             * to return to the user.
             */
            if (cb->bitstring.mem == NULL)
                WT_ERR(WT_NOTFOUND);
        }
        __wt_cursor_set_key(cursor, cb->offset + cb->granularity * cb->bit_offset++,
          cb->granularity, WT_BACKUP_RANGE);
    }

err:
    F_SET(cursor, raw);
    __wt_scr_free(session, &buf);
    API_END_RET(session, ret);
}

/*
 * __wt_curbackup_free_incr --
 *     Free the duplicate backup cursor for a file-based incremental backup.
 */
void
__wt_curbackup_free_incr(WT_SESSION_IMPL *session, WT_CURSOR_BACKUP *cb)
{
    __wt_free(session, cb->incr_file);
    if (cb->incr_cursor != NULL)
        __wt_cursor_close(cb->incr_cursor);
    __wt_buf_free(session, &cb->bitstring);
}

/*
 * __wt_curbackup_open_incr --
 *     Initialize the duplicate backup cursor for a file-based incremental backup.
 */
int
__wt_curbackup_open_incr(WT_SESSION_IMPL *session, const char *uri, WT_CURSOR *other,
  WT_CURSOR *cursor, const char *cfg[], WT_CURSOR **cursorp)
{
    WT_CURSOR_BACKUP *cb, *other_cb;
    WT_DECL_ITEM(open_uri);
    WT_DECL_RET;

    cb = (WT_CURSOR_BACKUP *)cursor;
    other_cb = (WT_CURSOR_BACKUP *)other;
    cursor->key_format = WT_UNCHECKED_STRING(qqq);
    cursor->value_format = "";

    WT_ASSERT(session, other_cb->incr_src != NULL);

    /*
     * Inherit from the backup cursor but reset specific functions for incremental.
     */
    cursor->next = __curbackup_incr_next;
    cursor->get_key = __wt_cursor_get_key;
    cursor->get_value = __wt_cursor_get_value_notsup;
    cb->incr_src = other_cb->incr_src;

    /* All WiredTiger owned files are full file copies. */
    if (F_ISSET(other_cb->incr_src, WT_BLKINCR_FULL) ||
      WT_PREFIX_MATCH(cb->incr_file, "WiredTiger")) {
        __wt_verbose(session, WT_VERB_BACKUP, "Forcing full file copies for %s for id %s",
          cb->incr_file, other_cb->incr_src->id_str);
        F_SET(cb, WT_CURBACKUP_FORCE_FULL);
    }
    /*
     * Set up the incremental backup information, if we are not forcing a full file copy. We need an
     * open cursor on the file. Open the backup checkpoint, confirming it exists.
     */
    if (!F_ISSET(cb, WT_CURBACKUP_FORCE_FULL)) {
        WT_ERR(__wt_scr_alloc(session, 0, &open_uri));
        WT_ERR(__wt_buf_fmt(session, open_uri, "file:%s", cb->incr_file));
        __wt_free(session, cb->incr_file);
        WT_ERR(__wt_strdup(session, open_uri->data, &cb->incr_file));

        WT_ERR(__wt_curfile_open(session, cb->incr_file, NULL, cfg, &cb->incr_cursor));
        WT_ERR(__wt_cursor_init(cursor, uri, NULL, cfg, cursorp));
        WT_ERR(__wt_strdup(session, cb->incr_cursor->internal_uri, &cb->incr_cursor->internal_uri));
    } else
        WT_ERR(__wt_cursor_init(cursor, uri, NULL, cfg, cursorp));

err:
    __wt_scr_free(session, &open_uri);
    return (ret);
}
