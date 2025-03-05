/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wti_blkcache_tiered_open --
 *     Open a tiered object.
 */
int
__wti_blkcache_tiered_open(
  WT_SESSION_IMPL *session, const char *uri, uint32_t objectid, WT_BLOCK **blockp)
{
    WT_BLOCK *block;
    WT_BUCKET_STORAGE *bstorage;
    WT_CONFIG_ITEM pfx;
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;
    WT_TIERED *tiered;
    const char *cfg[2], *object_name, *object_uri, *object_val;
    bool exist, local_only, readonly;

    *blockp = NULL;

    tiered = (WT_TIERED *)session->dhandle;
    object_uri = object_val = NULL;

    WT_ASSERT(session, objectid <= tiered->current_id);
    WT_ASSERT(session, uri == NULL || WT_PREFIX_MATCH(uri, "tiered:"));
    WT_ASSERT(session, (uri == NULL && objectid != 0) || (uri != NULL && objectid == 0));

    /*
     * First look for the local file. This will be the fastest access and we retain recent objects
     * in the local database for awhile. If we're passed a name to open, then by definition it's a
     * local file.
     *
     * It is possible for another thread to race with us and increment the current ID field. But
     * this can't happen in the cases we care about.
     */
    if (uri != NULL)
        objectid = tiered->current_id;
    if (objectid == tiered->current_id) {
        /*
         * Assert that we are safe from racing with another thread changing the current ID.
         *
         * We only open the newest local file when we are in the process of opening a tiered table,
         * or if we are opening a new file during a tiered switch. In the latter case we hold the
         * checkpoint lock, preventing other threads from incrementing the current ID.
         */
        WT_ASSERT(session,
          !F_ISSET(session->dhandle, WT_DHANDLE_OPEN) ||
            __wt_spin_owned(session, &S2C(session)->checkpoint_lock));
        local_only = true;
        object_uri = tiered->tiers[WT_TIERED_INDEX_LOCAL].name;
        object_name = object_uri;
        WT_PREFIX_SKIP_REQUIRED(session, object_name, "file:");
        readonly = false;
    } else {
        local_only = false;
        WT_ERR(
          __wt_tiered_name(session, &tiered->iface, objectid, WT_TIERED_NAME_OBJECT, &object_uri));
        object_name = object_uri;
        WT_PREFIX_SKIP_REQUIRED(session, object_name, "object:");
        readonly = true;
    }

    /* Get the object's configuration. */
    WT_ERR(__wt_metadata_search(session, object_uri, (char **)&object_val));
    cfg[0] = object_val;
    cfg[1] = NULL;

    /* Check if the object exists. */
    exist = true;
    if (!local_only)
        WT_ERR(__wt_fs_exist(session, object_name, &exist));
    if (exist)
        WT_ERR(__wt_block_open(
          session, object_name, objectid, cfg, false, readonly, false, 0, NULL, &block));
    else {
        /* We expect a prefix. */
        WT_ERR(__wt_config_gets(session, cfg, "tiered_storage.bucket_prefix", &pfx));
        WT_ASSERT(session, pfx.len != 0);

        WT_ERR(__wt_scr_alloc(session, 0, &tmp));
        WT_ERR(__wt_buf_fmt(session, tmp, "%.*s%s", (int)pfx.len, pfx.str, object_name));

        bstorage = tiered->bstorage;
        WT_WITH_BUCKET_STORAGE(bstorage, session,
          ret =
            __wt_block_open(session, tmp->mem, objectid, cfg, false, true, true, 0, NULL, &block));
        block->remote = true;
        WT_ERR(ret);
    }

    *blockp = block;

err:
    if (!local_only)
        __wt_free(session, object_uri);
    __wt_free(session, object_val);
    __wt_scr_free(session, &tmp);
    return (ret);
}

/*
 * __blkcache_find_open_handle --
 *     If the block manager's handle array already has an entry for the given object, return it. If
 *     caller is going to read from the handle, increment the read count while we have the handle
 *     table locked.
 */
static void
__blkcache_find_open_handle(WT_BM *bm, uint32_t objectid, bool reading, WT_BLOCK **blockp)
{
    u_int i;

    /* Must be called with minimum of a read lock on bm->handle_array_lock. */

    *blockp = NULL;

    /* Fast path if the active handle is the one we're looking for */
    if (bm->block->objectid == objectid)
        *blockp = bm->block;
    else
        /* Look for matching object in handle array */
        for (i = 0; i < bm->handle_array_next; i++)
            if (bm->handle_array[i]->objectid == objectid) {
                *blockp = bm->handle_array[i];
                break;
            }

    if (reading && *blockp != NULL)
        __wti_blkcache_get_read_handle(*blockp);
}

/*
 * __wt_blkcache_get_handle --
 *     Get a cached block handle for an object, creating it if it doesn't exist.
 */
int
__wt_blkcache_get_handle(
  WT_SESSION_IMPL *session, WT_BM *bm, uint32_t objectid, bool reading, WT_BLOCK **blockp)
{
    WT_BLOCK *new_handle;
    WT_DECL_RET;

    *blockp = NULL;

    /*
     * Check the block handle array for the object. We don't have to check the name because we can
     * only reference objects in our name space.
     */
    __wt_readlock(session, &bm->handle_array_lock);
    __blkcache_find_open_handle(bm, objectid, reading, blockp);
    __wt_readunlock(session, &bm->handle_array_lock);

    if (*blockp != NULL)
        return (0);

    /* Open a handle for the object. */
    WT_RET(__wti_blkcache_tiered_open(session, NULL, objectid, &new_handle));

    /* We need a write lock to add a new entry to the handle array. */
    __wt_writelock(session, &bm->handle_array_lock);

    /*
     * Check to see if the object was added while we opened it. If the object was added, we should
     * get back the same handle we already have.
     */
    __blkcache_find_open_handle(bm, objectid, reading, blockp);
    WT_ASSERT(session, *blockp == NULL || *blockp == new_handle);

    if (*blockp == NULL) {
        /* Allocate space to store the new handle and insert it in the array. */
        WT_ERR(__wt_realloc_def(
          session, &bm->handle_array_allocated, bm->handle_array_next + 1, &bm->handle_array));

        if (reading)
            __wti_blkcache_get_read_handle(new_handle);

        bm->handle_array[bm->handle_array_next++] = new_handle;
        *blockp = new_handle;
        new_handle = NULL;
    }

err:
    __wt_writeunlock(session, &bm->handle_array_lock);

    if (new_handle != NULL)
        WT_TRET(__wti_bm_close_block(session, new_handle));

    return (ret);
}

/*
 * __wti_blkcache_get_read_handle --
 *     Update block handle when a read operation begins.
 */
void
__wti_blkcache_get_read_handle(WT_BLOCK *block)
{
    __wt_atomic_add32(&block->read_count, 1);
}

/*
 * __wt_blkcache_release_handle --
 *     Update block handle when a read operation completes.
 */
void
__wt_blkcache_release_handle(WT_SESSION_IMPL *session, WT_BLOCK *block, bool *last_release)
{
    WT_ASSERT(session, block->read_count > 0);
    *last_release = false;
    if (__wt_atomic_sub32(&block->read_count, 1) == 0)
        *last_release = true;
}
