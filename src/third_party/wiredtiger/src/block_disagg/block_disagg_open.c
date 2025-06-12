/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_block_disagg_manager_create --
 *     Create a file - it's a bit of a game with a new block manager. The file is created when
 *     adding a new table to the metadata, before a btree handle is open. The block storage manager
 *     is generally created when the btree handle is opened. The caller of this will need to check
 *     for and instantiate a storage source.
 */
int
__wt_block_disagg_manager_create(
  WT_SESSION_IMPL *session, WT_BUCKET_STORAGE *bstorage, const char *filename)
{
    WT_UNUSED(session);
    WT_UNUSED(bstorage);
    WT_UNUSED(filename);

    /*
     * The default block manager creates the physical underlying file here and writes an initial
     * block into it. At the moment we don't need to do that for our special storage source - it's
     * going to magically create the file on first access and doesn't have a block manager provided
     * special leading descriptor block.
     */
    return (0);
}

/*
 * __block_disagg_destroy --
 *     Destroy a block handle.
 */
static int
__block_disagg_destroy(WT_SESSION_IMPL *session, WT_BLOCK_DISAGG *block_disagg)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    uint64_t bucket, hash;

    conn = S2C(session);
    hash = __wt_hash_city64(block_disagg->name, strlen(block_disagg->name));
    bucket = hash & (conn->hash_size - 1);
    WT_CONN_BLOCK_REMOVE(conn, block_disagg, bucket);

    __wt_free(session, block_disagg->name);

    if (block_disagg->plhandle != NULL)
        WT_TRET(block_disagg->plhandle->plh_close(block_disagg->plhandle, &session->iface));

    __wt_overwrite_and_free(session, block_disagg);

    return (ret);
}

/*
 * __wti_block_disagg_open --
 *     Open a block handle.
 */
int
__wti_block_disagg_open(WT_SESSION_IMPL *session, const char *filename, const char *cfg[],
  bool forced_salvage, bool readonly, WT_BLOCK **blockp)
{
    WT_BLOCK *block;
    WT_BLOCK_DISAGG *block_disagg;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    uint64_t bucket, hash;

    WT_UNUSED(cfg);
    WT_UNUSED(forced_salvage);
    WT_UNUSED(readonly);

    *blockp = NULL;
    block_disagg = NULL;

    WT_ASSERT(session, filename != NULL);

    __wt_verbose(session, WT_VERB_BLOCK, "open: %s", filename);

    conn = S2C(session);
    hash = __wt_hash_city64(filename, strlen(filename));
    bucket = hash & (conn->hash_size - 1);
    __wt_spin_lock(session, &conn->block_lock);
    TAILQ_FOREACH (block, &conn->blockhash[bucket], hashq) {
        /* TODO: Should check to make sure this is the right type of block */
        if (strcmp(filename, block->name) == 0) {
            ++block->ref;
            *blockp = block;
            __wt_spin_unlock(session, &conn->block_lock);
            return (0);
        }
    }

    /*
     * Basic structure allocation, initialization.
     *
     * Note: set the block's name-hash value before any work that can fail because cleanup calls the
     * block destroy code which uses that hash value to remove the block from the underlying linked
     * lists.
     */
    WT_ERR(__wt_calloc_one(session, &block_disagg));
    block_disagg->ref = 1;
    WT_CONN_BLOCK_INSERT(conn, (WT_BLOCK *)block_disagg, bucket);

    WT_ERR(__wt_strdup(session, filename, &block_disagg->name));
    if (WT_STREQ(block_disagg->name, WT_HS_FILE_SHARED))
        F_SET(block_disagg, WT_BLOCK_DISAGG_HS);

    WT_ERR(S2BT(session)->page_log->pl_open_handle(
      S2BT(session)->page_log, &session->iface, S2BT(session)->id, &block_disagg->plhandle));

    WT_ASSERT_ALWAYS(session, block_disagg->plhandle != NULL, "disagg tables need a page log");

    *blockp = (WT_BLOCK *)block_disagg;
    __wt_spin_unlock(session, &conn->block_lock);
    return (0);

err:
    if (block_disagg != NULL)
        WT_TRET(__block_disagg_destroy(session, block_disagg));
    __wt_spin_unlock(session, &conn->block_lock);
    return (ret);
}

/*
 * __wti_block_disagg_close --
 *     Close a block handle.
 */
int
__wti_block_disagg_close(WT_SESSION_IMPL *session, WT_BLOCK_DISAGG *block_disagg)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;

    if (block_disagg == NULL) /* Safety check */
        return (0);

    conn = S2C(session);

    __wt_verbose(
      session, WT_VERB_BLOCK, "close: %s", block_disagg->name == NULL ? "" : block_disagg->name);

    __wt_spin_lock(session, &conn->block_lock);

    /* Reference count is initialized to 1. */
    if (block_disagg->ref == 0 || --block_disagg->ref == 0)
        ret = __block_disagg_destroy(session, block_disagg);

    __wt_spin_unlock(session, &conn->block_lock);

    return (ret);
}

/*
 * __wti_block_disagg_stat --
 *     Set the statistics for a live block handle.
 */
void
__wti_block_disagg_stat(
  WT_SESSION_IMPL *session, WT_BLOCK_DISAGG *block_disagg, WT_DSRC_STATS *stats)
{
    WT_UNUSED(block_disagg);

    /* Fill this out. */
    WT_STAT_WRITE(session, stats, block_magic, WT_BLOCK_MAGIC);
}

/*
 * __wti_block_disagg_manager_size --
 *     Return the size of a live block handle.
 */
int
__wti_block_disagg_manager_size(WT_BM *bm, WT_SESSION_IMPL *session, wt_off_t *sizep)
{
    WT_UNUSED(session);

    *sizep = bm->block->size;
    return (0);
}
