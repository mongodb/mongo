/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __conn_dhandle_config_clear --
 *     Clear the underlying object's configuration information.
 */
static void
__conn_dhandle_config_clear(WT_SESSION_IMPL *session)
{
    WT_DATA_HANDLE *dhandle;
    const char **a;

    dhandle = session->dhandle;

    if (dhandle->cfg == NULL)
        return;
    for (a = dhandle->cfg; *a != NULL; ++a)
        __wt_free(session, *a);
    __wt_free(session, dhandle->cfg);
    __wt_free(session, dhandle->meta_base);
#ifdef HAVE_DIAGNOSTIC
    __wt_free(session, dhandle->orig_meta_base);
#endif
}

/*
 * __conn_dhandle_config_set --
 *     Set up a btree handle's configuration information.
 */
static int
__conn_dhandle_config_set(WT_SESSION_IMPL *session)
{
    WT_DATA_HANDLE *dhandle;
    WT_DECL_RET;
    char *metaconf, *tmp;
    const char *base, *cfg[4], *strip;

    dhandle = session->dhandle;
    base = NULL;
    tmp = NULL;

    /*
     * Read the object's entry from the metadata file, we're done if we don't find one.
     */
    if ((ret = __wt_metadata_search(session, dhandle->name, &metaconf)) != 0) {
        if (ret == WT_NOTFOUND)
            ret = __wt_set_return(session, ENOENT);
        WT_RET(ret);
    }

    /*
     * The defaults are included because persistent configuration information is stored in the
     * metadata file and it may be from an earlier version of WiredTiger. If defaults are included
     * in the configuration, we can add new configuration strings without upgrading the metadata
     * file or writing special code in case a configuration string isn't initialized, as long as the
     * new configuration string has an appropriate default value.
     *
     * The error handling is a little odd, but be careful: we're holding a chunk of allocated memory
     * in metaconf. If we fail before we copy a reference to it into the object's configuration
     * array, we must free it, after the copy, we don't want to free it.
     */
    WT_ERR(__wt_calloc_def(session, 3, &dhandle->cfg));
    switch (dhandle->type) {
    case WT_DHANDLE_TYPE_BTREE:
    case WT_DHANDLE_TYPE_TIERED:
        /*
         * We are stripping out all checkpoint related information from the config string. We save
         * the rest of the metadata string, that is essentially static and unchanging and then
         * concatenate the new checkpoint related information on each checkpoint. The reason is
         * performance and avoiding a lot of calls to the config parsing functions during a
         * checkpoint for information that changes in a very well known way.
         *
         * First collapse and overwrite checkpoint information because we do not know the name of or
         * how many checkpoints may be in this metadata. Similarly, for backup information, we want
         * an empty category to strip out since we don't know any backup ids. Set them empty and
         * call collapse to overwrite anything existing.
         */
        cfg[0] = metaconf;
        cfg[1] = "checkpoint=()";
        cfg[2] = "checkpoint_backup_info=()";
        cfg[3] = NULL;
        WT_ERR(__wt_strdup(session, WT_CONFIG_BASE(session, file_meta), &dhandle->cfg[0]));
        WT_ASSERT(session, dhandle->meta_base == NULL);
#ifdef HAVE_DIAGNOSTIC
        WT_ASSERT(session, dhandle->orig_meta_base == NULL);
#endif
        WT_ERR(__wt_config_collapse(session, cfg, &tmp));
        /*
         * Now strip out the checkpoint related items from the configuration string and that is now
         * our base metadata string.
         */
        cfg[0] = tmp;
        cfg[1] = NULL;
        if (dhandle->type == WT_DHANDLE_TYPE_TIERED)
            strip = "checkpoint=,checkpoint_backup_info=,checkpoint_lsn=,last=,tiers=()";
        else
            strip = "checkpoint=,checkpoint_backup_info=,checkpoint_lsn=";
        WT_ERR(__wt_config_merge(session, cfg, strip, &base));
        __wt_free(session, tmp);
        break;
    case WT_DHANDLE_TYPE_TABLE:
        WT_ERR(__wt_strdup(session, WT_CONFIG_BASE(session, table_meta), &dhandle->cfg[0]));
        break;
    case WT_DHANDLE_TYPE_TIERED_TREE:
        WT_ERR(__wt_strdup(session, WT_CONFIG_BASE(session, tier_meta), &dhandle->cfg[0]));
        break;
    }
    dhandle->cfg[1] = metaconf;
    dhandle->meta_base = base;
    dhandle->meta_base_length = base == NULL ? 0 : strlen(base);
#ifdef HAVE_DIAGNOSTIC
    /*  Save the original metadata value for further check to avoid writing corrupted data. */
    if (base == NULL)
        dhandle->orig_meta_base = NULL;
    else
        WT_ERR(__wt_strdup(session, base, &dhandle->orig_meta_base));
#endif
    return (0);

err:
    __wt_free(session, base);
    __wt_free(session, metaconf);
    __wt_free(session, tmp);
    return (ret);
}

/*
 * __conn_dhandle_destroy --
 *     Destroy a data handle.
 */
static int
__conn_dhandle_destroy(WT_SESSION_IMPL *session, WT_DATA_HANDLE *dhandle)
{
    WT_DECL_RET;

    switch (dhandle->type) {
    case WT_DHANDLE_TYPE_BTREE:
        WT_WITH_DHANDLE(session, dhandle, ret = __wt_btree_discard(session));
        break;
    case WT_DHANDLE_TYPE_TABLE:
        ret = __wt_schema_close_table(session, (WT_TABLE *)dhandle);
        break;
    case WT_DHANDLE_TYPE_TIERED:
        WT_WITH_DHANDLE(session, dhandle, ret = __wt_tiered_discard(session, (WT_TIERED *)dhandle));
        break;
    case WT_DHANDLE_TYPE_TIERED_TREE:
        ret = __wt_tiered_tree_close(session, (WT_TIERED_TREE *)dhandle);
        break;
    }

    __wt_rwlock_destroy(session, &dhandle->rwlock);
    __wt_free(session, dhandle->name);
    __wt_free(session, dhandle->checkpoint);
    __conn_dhandle_config_clear(session);
    __wt_spin_destroy(session, &dhandle->close_lock);
    __wt_stat_dsrc_discard(session, dhandle);
    __wt_overwrite_and_free(session, dhandle);
    return (ret);
}

/*
 * __wt_conn_dhandle_alloc --
 *     Allocate a new data handle and return it linked into the connection's list.
 */
int
__wt_conn_dhandle_alloc(WT_SESSION_IMPL *session, const char *uri, const char *checkpoint)
{
    WT_BTREE *btree;
    WT_DATA_HANDLE *dhandle;
    WT_DECL_RET;
    WT_TABLE *table;
    WT_TIERED *tiered;
    WT_TIERED_TREE *tiered_tree;
    uint64_t bucket;

    /*
     * Ensure no one beat us to creating the handle now that we hold the write lock.
     */
    if ((ret = __wt_conn_dhandle_find(session, uri, checkpoint)) != WT_NOTFOUND)
        return (ret);

    if (WT_PREFIX_MATCH(uri, "file:")) {
        WT_RET(__wt_calloc_one(session, &dhandle));
        dhandle->type = WT_DHANDLE_TYPE_BTREE;
    } else if (WT_PREFIX_MATCH(uri, "table:")) {
        WT_RET(__wt_calloc_one(session, &table));
        dhandle = (WT_DATA_HANDLE *)table;
        dhandle->type = WT_DHANDLE_TYPE_TABLE;
    } else if (WT_PREFIX_MATCH(uri, "tier:")) {
        WT_RET(__wt_calloc_one(session, &tiered_tree));
        dhandle = (WT_DATA_HANDLE *)tiered_tree;
        dhandle->type = WT_DHANDLE_TYPE_TIERED_TREE;
    } else if (WT_PREFIX_MATCH(uri, "tiered:")) {
        WT_RET(__wt_calloc_one(session, &tiered));
        dhandle = (WT_DATA_HANDLE *)tiered;
        dhandle->type = WT_DHANDLE_TYPE_TIERED;
    } else
        WT_RET_PANIC(session, EINVAL, "illegal handle allocation URI %s", uri);

    /* Btree handles keep their data separate from the interface. */
    if (WT_DHANDLE_BTREE(dhandle)) {
        WT_ERR(__wt_calloc_one(session, &btree));
        dhandle->handle = btree;
        btree->dhandle = dhandle;
    }

    if (strcmp(uri, WT_METAFILE_URI) == 0)
        F_SET(dhandle, WT_DHANDLE_IS_METADATA);

    WT_ERR(__wt_rwlock_init(session, &dhandle->rwlock));
    dhandle->name_hash = __wt_hash_city64(uri, strlen(uri));
    WT_ERR(__wt_strdup(session, uri, &dhandle->name));
    WT_ERR(__wt_strdup(session, checkpoint, &dhandle->checkpoint));

    WT_ERR(__wt_spin_init(session, &dhandle->close_lock, "data handle close"));

    /*
     * We are holding the data handle list lock, which protects most threads from seeing the new
     * handle until that lock is released.
     *
     * However, the sweep server scans the list of handles without holding that lock, so we need a
     * write barrier here to ensure the sweep server doesn't see a partially filled in structure.
     */
    WT_WRITE_BARRIER();

    /*
     * Prepend the handle to the connection list, assuming we're likely to need new files again
     * soon, until they are cached by all sessions.
     */
    bucket = dhandle->name_hash & (S2C(session)->dh_hash_size - 1);
    WT_CONN_DHANDLE_INSERT(S2C(session), dhandle, bucket);

    session->dhandle = dhandle;
    return (0);

err:
    WT_TRET(__conn_dhandle_destroy(session, dhandle));
    return (ret);
}

/*
 * __wt_conn_dhandle_find --
 *     Find a previously opened data handle.
 */
int
__wt_conn_dhandle_find(WT_SESSION_IMPL *session, const char *uri, const char *checkpoint)
{
    WT_CONNECTION_IMPL *conn;
    WT_DATA_HANDLE *dhandle;
    uint64_t bucket;

    conn = S2C(session);

    /* We must be holding the handle list lock at a higher level. */
    WT_ASSERT(session, FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_HANDLE_LIST));

    bucket = __wt_hash_city64(uri, strlen(uri)) & (conn->dh_hash_size - 1);
    if (checkpoint == NULL) {
        TAILQ_FOREACH (dhandle, &conn->dhhash[bucket], hashq) {
            if (F_ISSET(dhandle, WT_DHANDLE_DEAD))
                continue;
            if (dhandle->checkpoint == NULL && strcmp(uri, dhandle->name) == 0) {
                session->dhandle = dhandle;
                return (0);
            }
        }
    } else
        TAILQ_FOREACH (dhandle, &conn->dhhash[bucket], hashq) {
            if (F_ISSET(dhandle, WT_DHANDLE_DEAD))
                continue;
            if (dhandle->checkpoint != NULL && strcmp(uri, dhandle->name) == 0 &&
              strcmp(checkpoint, dhandle->checkpoint) == 0) {
                session->dhandle = dhandle;
                return (0);
            }
        }

    return (WT_NOTFOUND);
}

/*
 * __wt_conn_dhandle_close --
 *     Sync and close the underlying btree handle.
 */
int
__wt_conn_dhandle_close(WT_SESSION_IMPL *session, bool final, bool mark_dead)
{
    WT_BM *bm;
    WT_BTREE *btree;
    WT_CONNECTION_IMPL *conn;
    WT_DATA_HANDLE *dhandle;
    WT_DECL_RET;
    bool discard, is_btree, is_mapped, marked_dead, no_schema_lock;

    conn = S2C(session);
    dhandle = session->dhandle;

    if (!F_ISSET(dhandle, WT_DHANDLE_OPEN))
        return (0);

    /*
     * The only data handle type that uses the "handle" field is btree. For other data handle types,
     * it should be NULL.
     */
    is_btree = WT_DHANDLE_BTREE(dhandle);
    btree = is_btree ? dhandle->handle : NULL;

    if (is_btree) {
        /* Turn off eviction. */
        WT_RET(__wt_evict_file_exclusive_on(session));

        /* Reset the tree's eviction priority (if any). */
        __wt_evict_priority_clear(session);

        /* Mark the advisory bit that the tree has been evicted. */
        F_SET(dhandle, WT_DHANDLE_EVICTED);
    }

    /*
     * If we don't already have the schema lock, make it an error to try to acquire it. The problem
     * is that we are holding an exclusive lock on the handle, and if we attempt to acquire the
     * schema lock we might deadlock with a thread that has the schema lock and wants a handle lock.
     */
    no_schema_lock = false;
    if (!FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_SCHEMA)) {
        no_schema_lock = true;
        FLD_SET(session->lock_flags, WT_SESSION_NO_SCHEMA_LOCK);
    }

    /*
     * We may not be holding the schema lock, and threads may be walking the list of open handles
     * (for example, checkpoint). Acquire the handle's close lock. We don't have the sweep server
     * acquire the handle's rwlock so we have to prevent races through the close code.
     */
    __wt_spin_lock(session, &dhandle->close_lock);

    discard = is_mapped = marked_dead = false;
    if (is_btree && !F_ISSET(btree, WT_BTREE_SALVAGE | WT_BTREE_UPGRADE | WT_BTREE_VERIFY)) {
        /*
         * If the handle is already marked dead, we're just here to discard it.
         */
        if (F_ISSET(dhandle, WT_DHANDLE_DEAD))
            discard = true;

        /*
         * Mark the handle dead (letting the tree be discarded later) if it's not already marked
         * dead, and it's not a memory-mapped tree. (We can't mark memory-mapped tree handles dead
         * because we close the underlying file handle to allow the file to be removed and
         * memory-mapped trees contain pointers into memory that become invalid if the mapping is
         * closed.)
         */
        bm = btree->bm;
        if (bm != NULL)
            is_mapped = bm->is_mapped(bm, session);
        if (!discard && mark_dead && (bm == NULL || !is_mapped))
            marked_dead = true;

        /*
         * Flush dirty data from any durable trees we couldn't mark dead. That involves writing a
         * checkpoint, which can fail if an update cannot be written, causing the close to fail: if
         * not the final close, return the EBUSY error to our caller for eventual retry.
         *
         * We can't discard non-durable trees yet: first we have to close the underlying btree
         * handle, then we can mark the data handle dead.
         *
         * If we are closing with timestamps enforced, then we have already checkpointed as of the
         * timestamp as needed and any remaining dirty data should be discarded.
         */
        if (!discard && !marked_dead) {
            if (F_ISSET(conn, WT_CONN_CLOSING_TIMESTAMP) || F_ISSET(conn, WT_CONN_IN_MEMORY) ||
              F_ISSET(btree, WT_BTREE_NO_CHECKPOINT))
                discard = true;
            else {
                WT_TRET(__wt_checkpoint_close(session, final));
                if (!final && ret == EBUSY)
                    WT_ERR(ret);
            }
        }
    }

    /*
     * We close the underlying handle before discarding pages from the cache for performance
     * reasons. However, the underlying block manager "owns" information about memory mappings, and
     * memory-mapped pages contain pointers into memory that becomes invalid if the mapping is
     * closed, so discard mapped files before closing, otherwise, close first.
     */
    if (discard && is_mapped)
        WT_TRET(__wt_evict_file(session, WT_SYNC_DISCARD));

    /* Close the underlying handle. */
    switch (dhandle->type) {
    case WT_DHANDLE_TYPE_BTREE:
        WT_TRET(__wt_btree_close(session));
        F_CLR(btree, WT_BTREE_SPECIAL_FLAGS);
        break;
    case WT_DHANDLE_TYPE_TABLE:
        WT_TRET(__wt_schema_close_table(session, (WT_TABLE *)dhandle));
        break;
    case WT_DHANDLE_TYPE_TIERED:
        WT_TRET(__wt_tiered_close(session));
        F_CLR(btree, WT_BTREE_SPECIAL_FLAGS);
        break;
    case WT_DHANDLE_TYPE_TIERED_TREE:
        WT_TRET(__wt_tiered_tree_close(session, (WT_TIERED_TREE *)dhandle));
        break;
    }

    /*
     * If marking the handle dead, do so after closing the underlying btree. (Don't do it before
     * that, the block manager asserts there are never two references to a block manager object, and
     * re-opening the handle can succeed once we mark this handle dead.)
     *
     * Check discard too, code we call to clear the cache expects the data handle dead flag to be
     * set when discarding modified pages.
     */
    if (marked_dead || discard)
        F_SET(dhandle, WT_DHANDLE_DEAD);

    /*
     * Discard from cache any trees not marked dead in this call (that is, including trees
     * previously marked dead). Done after marking the data handle dead for a couple reasons: first,
     * we don't need to hold an exclusive handle to do it, second, code we call to clear the cache
     * expects the data handle dead flag to be set when discarding modified pages.
     */
    if (discard && !is_mapped)
        WT_TRET(__wt_evict_file(session, WT_SYNC_DISCARD));

    /*
     * If we marked a handle dead it will be closed by sweep, via another call to this function.
     * Otherwise, we're done with this handle.
     */
    if (!marked_dead) {
        F_CLR(dhandle, WT_DHANDLE_OPEN);
        if (dhandle->checkpoint == NULL)
            --conn->open_btree_count;
    }
    WT_ASSERT(session, F_ISSET(dhandle, WT_DHANDLE_DEAD) || !F_ISSET(dhandle, WT_DHANDLE_OPEN));

err:
    __wt_spin_unlock(session, &dhandle->close_lock);

    if (no_schema_lock)
        FLD_CLR(session->lock_flags, WT_SESSION_NO_SCHEMA_LOCK);

    if (is_btree)
        __wt_evict_file_exclusive_off(session);

    return (ret);
}

/*
 * __conn_dhandle_config_parse --
 *     Parse out configuration settings relevant for the data handle.
 */
static int
__conn_dhandle_config_parse(WT_SESSION_IMPL *session)
{
    WT_CONFIG_ITEM cval, sval;
    WT_DATA_HANDLE *dhandle;
    WT_DECL_RET;
    const char **cfg;

    dhandle = session->dhandle;
    cfg = dhandle->cfg;

    /* Clear all the flags. */
    dhandle->ts_flags = 0;

    /* Debugging information */
    WT_RET(__wt_config_gets(session, cfg, "assert.write_timestamp", &cval));
    if (WT_STRING_MATCH("on", cval.str, cval.len))
        FLD_SET(dhandle->ts_flags, WT_DHANDLE_ASSERT_TS_WRITE);

    WT_RET(__wt_config_gets(session, cfg, "assert.read_timestamp", &cval));
    if (WT_STRING_MATCH("always", cval.str, cval.len))
        FLD_SET(dhandle->ts_flags, WT_DHANDLE_ASSERT_TS_READ_ALWAYS);
    else if (WT_STRING_MATCH("never", cval.str, cval.len))
        FLD_SET(dhandle->ts_flags, WT_DHANDLE_ASSERT_TS_READ_NEVER);

    /* Setup verbose options */
    WT_RET(__wt_config_gets(session, cfg, "verbose", &cval));
    if ((ret = __wt_config_subgets(session, &cval, "write_timestamp", &sval)) == 0 && sval.val != 0)
        FLD_SET(dhandle->ts_flags, WT_DHANDLE_VERB_TS_WRITE);
    WT_RET_NOTFOUND_OK(ret);

    /* Setup timestamp usage hints */
    WT_RET(__wt_config_gets(session, cfg, "write_timestamp_usage", &cval));
    if (WT_STRING_MATCH("always", cval.str, cval.len))
        FLD_SET(dhandle->ts_flags, WT_DHANDLE_TS_ALWAYS);
    else if (WT_STRING_MATCH("key_consistent", cval.str, cval.len))
        FLD_SET(dhandle->ts_flags, WT_DHANDLE_TS_KEY_CONSISTENT);
    else if (WT_STRING_MATCH("mixed_mode", cval.str, cval.len))
        FLD_SET(dhandle->ts_flags, WT_DHANDLE_TS_MIXED_MODE);
    else if (WT_STRING_MATCH("never", cval.str, cval.len))
        FLD_SET(dhandle->ts_flags, WT_DHANDLE_TS_NEVER);
    else if (WT_STRING_MATCH("ordered", cval.str, cval.len))
        FLD_SET(dhandle->ts_flags, WT_DHANDLE_TS_ORDERED);

    return (0);
}

/*
 * __wt_conn_dhandle_open --
 *     Open the current data handle.
 */
int
__wt_conn_dhandle_open(WT_SESSION_IMPL *session, const char *cfg[], uint32_t flags)
{
    WT_BTREE *btree;
    WT_DATA_HANDLE *dhandle;
    WT_DECL_RET;

    dhandle = session->dhandle;
    btree = dhandle->handle;

    WT_ASSERT(session, F_ISSET(dhandle, WT_DHANDLE_EXCLUSIVE) && !LF_ISSET(WT_DHANDLE_LOCK_ONLY));

    WT_ASSERT(session, !F_ISSET(S2C(session), WT_CONN_CLOSING_NO_MORE_OPENS));

    /* Turn off eviction. */
    if (WT_DHANDLE_BTREE(dhandle))
        WT_RET(__wt_evict_file_exclusive_on(session));

    /*
     * If the handle is already open, it has to be closed so it can be reopened with a new
     * configuration.
     *
     * This call can return EBUSY if there's an update in the tree that's not yet globally visible.
     * That's not a problem because it can only happen when we're switching from a normal handle to
     * a "special" one, so we're returning EBUSY to an attempt to verify or do other special
     * operations. The reverse won't happen because when the handle from a verify or other special
     * operation is closed, there won't be updates in the tree that can block the close.
     */
    if (F_ISSET(dhandle, WT_DHANDLE_OPEN))
        WT_ERR(__wt_conn_dhandle_close(session, false, false));

    /* Discard any previous configuration, set up the new configuration. */
    __conn_dhandle_config_clear(session);
    WT_ERR(__conn_dhandle_config_set(session));
    WT_ERR(__conn_dhandle_config_parse(session));

    switch (dhandle->type) {
    case WT_DHANDLE_TYPE_BTREE:
        /* Set any special flags on the btree handle. */
        F_SET(btree, LF_MASK(WT_BTREE_SPECIAL_FLAGS));

        /*
         * Allocate data-source statistics memory. We don't allocate that memory when allocating the
         * data handle because not all data handles need statistics (for example, handles used for
         * checkpoint locking). If we are reopening the handle, then it may already have statistics
         * memory, check to avoid the leak.
         */
        if (dhandle->stat_array == NULL)
            WT_ERR(__wt_stat_dsrc_init(session, dhandle));

        WT_ERR(__wt_btree_open(session, cfg));
        break;
    case WT_DHANDLE_TYPE_TABLE:
        WT_ERR(__wt_schema_open_table(session));
        break;
    case WT_DHANDLE_TYPE_TIERED:
        /* Set any special flags on the btree handle. */
        F_SET(btree, LF_MASK(WT_BTREE_SPECIAL_FLAGS));

        /*
         * Allocate data-source statistics memory. We don't allocate that memory when allocating the
         * data handle because not all data handles need statistics (for example, handles used for
         * checkpoint locking). If we are reopening the handle, then it may already have statistics
         * memory, check to avoid the leak.
         */
        if (dhandle->stat_array == NULL)
            WT_ERR(__wt_stat_dsrc_init(session, dhandle));

        WT_ERR(__wt_tiered_open(session, cfg));
        break;
    case WT_DHANDLE_TYPE_TIERED_TREE:
        WT_ERR(__wt_tiered_tree_open(session, cfg));
        break;
    }

    /*
     * Bulk handles require true exclusive access, otherwise, handles marked as exclusive are
     * allowed to be relocked by the same session.
     */
    if (F_ISSET(dhandle, WT_DHANDLE_EXCLUSIVE) && !LF_ISSET(WT_BTREE_BULK)) {
        dhandle->excl_session = session;
        dhandle->excl_ref = 1;
    }
    F_SET(dhandle, WT_DHANDLE_OPEN);

    /*
     * Checkpoint handles are read-only, so eviction calculations based on the number of btrees are
     * better to ignore them.
     */
    if (dhandle->checkpoint == NULL)
        ++S2C(session)->open_btree_count;

    if (0) {
err:
        if (btree != NULL)
            F_CLR(btree, WT_BTREE_SPECIAL_FLAGS);
    }

    if (WT_DHANDLE_BTREE(dhandle) && session->dhandle != NULL) {
        __wt_evict_file_exclusive_off(session);

        /*
         * We want to close the Btree for an object that lives in the local directory. It will
         * actually be part of the corresponding tiered Btree.
         */
        if (dhandle->type == WT_DHANDLE_TYPE_BTREE && WT_SUFFIX_MATCH(dhandle->name, ".wtobj"))
            WT_TRET(__wt_btree_close(session));
    }

    if (ret == ENOENT && F_ISSET(dhandle, WT_DHANDLE_IS_METADATA)) {
        F_SET(S2C(session), WT_CONN_DATA_CORRUPTION);
        return (WT_ERROR);
    }

    return (ret);
}

/*
 * __conn_btree_apply_internal --
 *     Apply a function to an open data handle.
 */
static int
__conn_btree_apply_internal(WT_SESSION_IMPL *session, WT_DATA_HANDLE *dhandle,
  int (*file_func)(WT_SESSION_IMPL *, const char *[]),
  int (*name_func)(WT_SESSION_IMPL *, const char *, bool *), const char *cfg[])
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    uint64_t time_diff, time_start, time_stop;
    bool skip;

    conn = S2C(session);
    time_start = time_stop = 0;

    /* Always apply the name function, if supplied. */
    skip = false;
    if (name_func != NULL)
        WT_RET(name_func(session, dhandle->name, &skip));

    /* If there is no file function, don't bother locking the handle */
    if (file_func == NULL || skip)
        return (0);

    /*
     * We need to pull the handle into the session handle cache and make sure it's referenced to
     * stop other internal code dropping the handle (e.g in LSM when cleaning up obsolete chunks).
     */
    if ((ret = __wt_session_get_dhandle(session, dhandle->name, dhandle->checkpoint, NULL, 0)) != 0)
        return (ret == EBUSY ? 0 : ret);

    if (WT_SESSION_IS_CHECKPOINT(session))
        time_start = __wt_clock(session);
    WT_SAVE_DHANDLE(session, ret = file_func(session, cfg));
    /* We need to gather this information before releasing the dhandle. */
    if (WT_SESSION_IS_CHECKPOINT(session)) {
        time_stop = __wt_clock(session);
        time_diff = WT_CLOCKDIFF_US(time_stop, time_start);
        if (F_ISSET(S2BT(session), WT_BTREE_SKIP_CKPT)) {
            ++conn->ckpt_skip;
            conn->ckpt_skip_time += time_diff;
        } else {
            ++conn->ckpt_apply;
            conn->ckpt_apply_time += time_diff;
        }
    }
    WT_TRET(__wt_session_release_dhandle(session));
    return (ret);
}

/*
 * __wt_conn_btree_apply --
 *     Apply a function to all open btree handles with the given URI.
 */
int
__wt_conn_btree_apply(WT_SESSION_IMPL *session, const char *uri,
  int (*file_func)(WT_SESSION_IMPL *, const char *[]),
  int (*name_func)(WT_SESSION_IMPL *, const char *, bool *), const char *cfg[])
{
    WT_CONNECTION_IMPL *conn;
    WT_DATA_HANDLE *dhandle;
    WT_DECL_RET;
    uint64_t bucket;
    uint64_t time_diff, time_start, time_stop;

    conn = S2C(session);
    time_start = time_stop = 0;
    /*
     * If we're given a URI, then we walk only the hash list for that name. If we don't have a URI
     * we walk the entire dhandle list.
     */
    if (uri != NULL) {
        bucket = __wt_hash_city64(uri, strlen(uri)) & (conn->dh_hash_size - 1);

        for (dhandle = NULL;;) {
            WT_WITH_HANDLE_LIST_READ_LOCK(
              session, WT_DHANDLE_NEXT(session, dhandle, &conn->dhhash[bucket], hashq));
            if (dhandle == NULL)
                return (0);

            if (!F_ISSET(dhandle, WT_DHANDLE_OPEN) || F_ISSET(dhandle, WT_DHANDLE_DEAD) ||
              dhandle->checkpoint != NULL || strcmp(uri, dhandle->name) != 0)
                continue;
            WT_ERR(__conn_btree_apply_internal(session, dhandle, file_func, name_func, cfg));
        }
    } else {
        if (WT_SESSION_IS_CHECKPOINT(session)) {
            conn->ckpt_apply = conn->ckpt_skip = 0;
            conn->ckpt_apply_time = conn->ckpt_skip_time = 0;
            time_start = __wt_clock(session);
            F_SET(conn, WT_CONN_CKPT_GATHER);
        }
        for (dhandle = NULL;;) {
            WT_WITH_HANDLE_LIST_READ_LOCK(
              session, WT_DHANDLE_NEXT(session, dhandle, &conn->dhqh, q));
            if (dhandle == NULL)
                goto done;

            if (!F_ISSET(dhandle, WT_DHANDLE_OPEN) || F_ISSET(dhandle, WT_DHANDLE_DEAD) ||
              !WT_DHANDLE_BTREE(dhandle) || dhandle->checkpoint != NULL || WT_IS_METADATA(dhandle))
                continue;
            WT_ERR(__conn_btree_apply_internal(session, dhandle, file_func, name_func, cfg));
        }
done:
        if (WT_SESSION_IS_CHECKPOINT(session)) {
            F_CLR(conn, WT_CONN_CKPT_GATHER);
            time_stop = __wt_clock(session);
            time_diff = WT_CLOCKDIFF_US(time_stop, time_start);
            WT_STAT_CONN_SET(session, txn_checkpoint_handle_applied, conn->ckpt_apply);
            WT_STAT_CONN_SET(session, txn_checkpoint_handle_skipped, conn->ckpt_skip);
            WT_STAT_CONN_SET(session, txn_checkpoint_handle_walked, conn->dhandle_count);
            WT_STAT_CONN_SET(session, txn_checkpoint_handle_duration, time_diff);
            WT_STAT_CONN_SET(session, txn_checkpoint_handle_duration_apply, conn->ckpt_apply_time);
            WT_STAT_CONN_SET(session, txn_checkpoint_handle_duration_skip, conn->ckpt_skip_time);
        }
        return (0);
    }

err:
    F_CLR(conn, WT_CONN_CKPT_GATHER);
    WT_DHANDLE_RELEASE(dhandle);
    return (ret);
}

/*
 * __conn_dhandle_close_one --
 *     Lock and, if necessary, close a data handle.
 */
static int
__conn_dhandle_close_one(
  WT_SESSION_IMPL *session, const char *uri, const char *checkpoint, bool removed, bool mark_dead)
{
    WT_DECL_RET;

    /*
     * Lock the handle exclusively. If this is part of schema-changing operation (indicated by
     * metadata tracking being enabled), hold the lock for the duration of the operation.
     */
    WT_RET(__wt_session_get_dhandle(
      session, uri, checkpoint, NULL, WT_DHANDLE_EXCLUSIVE | WT_DHANDLE_LOCK_ONLY));
    if (WT_META_TRACKING(session))
        WT_RET(__wt_meta_track_handle_lock(session, false));

    /*
     * We have an exclusive lock, which means there are no cursors open at this point. Close the
     * handle, if necessary.
     */
    if (F_ISSET(session->dhandle, WT_DHANDLE_OPEN)) {
        __wt_meta_track_sub_on(session);
        ret = __wt_conn_dhandle_close(session, false, mark_dead);

        /*
         * If the close succeeded, drop any locks it acquired. If there was a failure, this function
         * will fail and the whole transaction will be rolled back.
         */
        if (ret == 0)
            ret = __wt_meta_track_sub_off(session);
    }
    if (removed)
        F_SET(session->dhandle, WT_DHANDLE_DROPPED);

    if (!WT_META_TRACKING(session))
        WT_TRET(__wt_session_release_dhandle(session));

    return (ret);
}

/*
 * __wt_conn_dhandle_close_all --
 *     Close all data handles handles with matching name (including all checkpoint handles).
 */
int
__wt_conn_dhandle_close_all(WT_SESSION_IMPL *session, const char *uri, bool removed, bool mark_dead)
{
    WT_CONNECTION_IMPL *conn;
    WT_DATA_HANDLE *dhandle;
    WT_DECL_RET;
    uint64_t bucket;

    conn = S2C(session);

    WT_ASSERT(session, FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_HANDLE_LIST_WRITE));
    WT_ASSERT(session, session->dhandle == NULL);

    /*
     * Lock the live handle first. This ordering is important: we rely on locking the live handle to
     * fail fast if the tree is busy (e.g., with cursors open or in a checkpoint).
     */
    WT_ERR(__conn_dhandle_close_one(session, uri, NULL, removed, mark_dead));

    bucket = __wt_hash_city64(uri, strlen(uri)) & (conn->dh_hash_size - 1);
    TAILQ_FOREACH (dhandle, &conn->dhhash[bucket], hashq) {
        if (strcmp(dhandle->name, uri) != 0 || dhandle->checkpoint == NULL ||
          F_ISSET(dhandle, WT_DHANDLE_DEAD))
            continue;

        WT_ERR(__conn_dhandle_close_one(
          session, dhandle->name, dhandle->checkpoint, removed, mark_dead));
    }

err:
    session->dhandle = NULL;
    return (ret);
}

/*
 * __conn_dhandle_remove --
 *     Remove a handle from the shared list.
 */
static int
__conn_dhandle_remove(WT_SESSION_IMPL *session, bool final)
{
    WT_CONNECTION_IMPL *conn;
    WT_DATA_HANDLE *dhandle;
    uint64_t bucket;

    conn = S2C(session);
    dhandle = session->dhandle;
    bucket = dhandle->name_hash & (conn->dh_hash_size - 1);

    WT_ASSERT(session, FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_HANDLE_LIST_WRITE));
    WT_ASSERT(session, dhandle != conn->cache->walk_tree);

    /* Check if the handle was reacquired by a session while we waited. */
    if (!final && (dhandle->session_inuse != 0 || dhandle->session_ref != 0))
        return (__wt_set_return(session, EBUSY));

    WT_CONN_DHANDLE_REMOVE(conn, dhandle, bucket);
    return (0);
}

/*
 * __wt_conn_dhandle_discard_single --
 *     Close/discard a single data handle.
 */
int
__wt_conn_dhandle_discard_single(WT_SESSION_IMPL *session, bool final, bool mark_dead)
{
    WT_DATA_HANDLE *dhandle;
    WT_DECL_RET;
    int tret;
    bool set_pass_intr;

    dhandle = session->dhandle;

    if (F_ISSET(dhandle, WT_DHANDLE_OPEN)) {
        tret = __wt_conn_dhandle_close(session, final, mark_dead);
        if (final && tret != 0) {
            __wt_err(session, tret, "Final close of %s failed", dhandle->name);
            WT_TRET(tret);
        } else if (!final)
            WT_RET(tret);
    }

    /*
     * Kludge: interrupt the eviction server in case it is holding the handle list lock.
     */
    set_pass_intr = false;
    if (!FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_HANDLE_LIST)) {
        set_pass_intr = true;
        (void)__wt_atomic_addv32(&S2C(session)->cache->pass_intr, 1);
    }

    /* Try to remove the handle, protected by the data handle lock. */
    WT_WITH_HANDLE_LIST_WRITE_LOCK(session, tret = __conn_dhandle_remove(session, final));
    if (set_pass_intr)
        (void)__wt_atomic_subv32(&S2C(session)->cache->pass_intr, 1);
    WT_TRET(tret);

    /*
     * After successfully removing the handle, clean it up.
     */
    if (ret == 0 || final) {
        WT_TRET(__conn_dhandle_destroy(session, dhandle));
        session->dhandle = NULL;
    }

    return (ret);
}

/*
 * __wt_conn_dhandle_discard --
 *     Close/discard all data handles.
 */
int
__wt_conn_dhandle_discard(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DATA_HANDLE *dhandle, *dhandle_tmp;
    WT_DECL_RET;

    conn = S2C(session);

    /*
     * Empty the session cache: any data handles created in a connection method may be cached here,
     * and we're about to close them.
     */
    __wt_session_close_cache(session);

/*
 * Close open data handles: first, everything apart from metadata and the history store (as closing
 * a normal file may write metadata and read history store entries). Then close whatever is left
 * open.
 */
restart:
    TAILQ_FOREACH (dhandle, &conn->dhqh, q) {
        if (WT_IS_METADATA(dhandle) || strcmp(dhandle->name, WT_HS_URI) == 0 ||
          WT_PREFIX_MATCH(dhandle->name, WT_SYSTEM_PREFIX))
            continue;

        WT_WITH_DHANDLE(session, dhandle,
          WT_TRET(__wt_conn_dhandle_discard_single(session, true, F_ISSET(conn, WT_CONN_PANIC))));
        goto restart;
    }

    /* Shut down the history store table after all eviction is complete. */
    __wt_hs_close(session);

    /*
     * Closing the files may have resulted in entries on our default session's list of open data
     * handles, specifically, we added the metadata file if any of the files were dirty. Clean up
     * that list before we shut down the metadata entry, for good.
     */
    __wt_session_close_cache(session);
    F_SET(session, WT_SESSION_NO_DATA_HANDLES);

    /*
     * The connection may have an open metadata cursor handle. We cannot close it before now because
     * it's potentially used when discarding other open data handles. Close it before discarding the
     * underlying metadata handle.
     */
    WT_TRET(__wt_metadata_cursor_close(session));

    /* Close the remaining handles. */
    WT_TAILQ_SAFE_REMOVE_BEGIN(dhandle, &conn->dhqh, q, dhandle_tmp)
    {
        WT_WITH_DHANDLE(session, dhandle,
          WT_TRET(__wt_conn_dhandle_discard_single(session, true, F_ISSET(conn, WT_CONN_PANIC))));
    }
    WT_TAILQ_SAFE_REMOVE_END

    return (ret);
}

/*
 * __wt_dhandle_update_write_gens --
 *     Update the open dhandles write generation, run write generation and base write generation
 *     number.
 */
void
__wt_dhandle_update_write_gens(WT_SESSION_IMPL *session)
{
    WT_BTREE *btree;
    WT_CONNECTION_IMPL *conn;
    WT_DATA_HANDLE *dhandle;

    conn = S2C(session);

    for (dhandle = NULL;;) {
        WT_WITH_HANDLE_LIST_WRITE_LOCK(session, WT_DHANDLE_NEXT(session, dhandle, &conn->dhqh, q));
        if (dhandle == NULL)
            break;
        /* There can be other dhandle types such as tier: that do not have a btree. Skip those. */
        if (!WT_BTREE_PREFIX(dhandle->name))
            continue;
        btree = (WT_BTREE *)dhandle->handle;

        WT_ASSERT(session, btree != NULL);

        /*
         * Initialize the btree write generation numbers after rollback to stable so that the
         * transaction ids of the pages will be reset when loaded from disk to memory.
         */
        btree->write_gen = btree->base_write_gen = btree->run_write_gen =
          WT_MAX(btree->write_gen, conn->base_write_gen);
    }
}

/*
 * __wt_verbose_dump_handles --
 *     Dump information about all data handles.
 */
int
__wt_verbose_dump_handles(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DATA_HANDLE *dhandle;

    conn = S2C(session);

    WT_RET(__wt_msg(session, "%s", WT_DIVIDER));
    WT_RET(__wt_msg(session, "Data handle dump:"));
    for (dhandle = NULL;;) {
        WT_WITH_HANDLE_LIST_READ_LOCK(session, WT_DHANDLE_NEXT(session, dhandle, &conn->dhqh, q));
        if (dhandle == NULL)
            break;
        WT_RET(__wt_msg(session, "Name: %s", dhandle->name));
        if (dhandle->checkpoint != NULL)
            WT_RET(__wt_msg(session, "Checkpoint: %s", dhandle->checkpoint));
        WT_RET(__wt_msg(session, "  Sessions referencing handle: %" PRIu32, dhandle->session_ref));
        WT_RET(__wt_msg(session, "  Sessions using handle: %" PRId32, dhandle->session_inuse));
        WT_RET(__wt_msg(session, "  Exclusive references to handle: %" PRIu32, dhandle->excl_ref));
        if (dhandle->excl_ref != 0)
            WT_RET(
              __wt_msg(session, "  Session with exclusive use: %p", (void *)dhandle->excl_session));
        WT_RET(__wt_msg(session, "  Flags: 0x%08" PRIx32, dhandle->flags));
    }
    return (0);
}
