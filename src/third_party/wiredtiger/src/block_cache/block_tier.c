/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_blkcache_tiered_open --
 *     Open a tiered object.
 */
int
__wt_blkcache_tiered_open(
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
     * FIXME-WT-7590 we will need some kind of locking while we're looking at the tiered structure.
     * This can be called at any time, because we are opening the objects lazily.
     */
    if (uri != NULL)
        objectid = tiered->current_id;
    if (objectid == tiered->current_id) {
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
        WT_ERR(
          __wt_block_open(session, object_name, objectid, cfg, false, readonly, false, 0, &block));
    else {
        /* We expect a prefix. */
        WT_ERR(__wt_config_gets(session, cfg, "tiered_storage.bucket_prefix", &pfx));
        WT_ASSERT(session, pfx.len != 0);

        WT_ERR(__wt_scr_alloc(session, 0, &tmp));
        WT_ERR(__wt_buf_fmt(session, tmp, "%.*s%s", (int)pfx.len, pfx.str, object_name));

        bstorage = tiered->bstorage;
        WT_WITH_BUCKET_STORAGE(bstorage, session,
          ret = __wt_block_open(session, tmp->mem, objectid, cfg, false, true, true, 0, &block));
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
 * __wt_blkcache_get_handle --
 *     Get a cached block handle for an object, creating it if it doesn't exist.
 */
int
__wt_blkcache_get_handle(
  WT_SESSION_IMPL *session, WT_BLOCK *current, uint32_t objectid, WT_BLOCK **blockp)
{
    WT_DECL_RET;
    u_int i;

    *blockp = NULL;

    /* We should never be looking for our own object. */
    WT_ASSERT(session, current->objectid != objectid);

    /*
     * Check the local cache for the object. We don't have to check the name because we can only
     * reference objects in our name space.
     */
    for (i = 0; i < current->related_next; ++i)
        if (current->related[i]->objectid == objectid) {
            *blockp = current->related[i];
            return (0);
        }

    /* Lock the block cache layer.  */
    __wt_spin_lock(session, &current->cache_lock);

    /* Check to make sure the object wasn't cached while we locked. */
    for (i = 0; i < current->related_next; ++i)
        if (current->related[i]->objectid == objectid) {
            *blockp = current->related[i];
            break;
        }

    /* Open the object. */
    if (*blockp == NULL) {
        /* Allocate space to store a reference (do first for less complicated cleanup). */
        WT_ERR(__wt_realloc_def(
          session, &current->related_allocated, current->related_next + 1, &current->related));

        /* Get a reference to the object, opening it as necessary. */
        WT_ERR(__wt_blkcache_tiered_open(session, NULL, objectid, blockp));

        /* Save a reference in the block in which we started for fast subsequent access. */
        current->related[current->related_next++] = *blockp;
    }

err:
    __wt_spin_unlock(session, &current->cache_lock);
    return (ret);
}
