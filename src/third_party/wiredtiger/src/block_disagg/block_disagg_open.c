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
    uint64_t bucket, hash, tableid;

    WT_UNUSED(cfg);
    WT_UNUSED(forced_salvage);
    WT_UNUSED(readonly);

    *blockp = NULL;
    block_disagg = NULL;

    WT_ASSERT(session, filename != NULL);

    __wt_verbose(session, WT_VERB_BLOCK, "open: %s (table_id: %" PRIu64 ")", filename,
      (uint64_t)S2BT(session)->id);

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

    tableid = S2BT(session)->id;
    block_disagg->tableid = tableid;

    WT_ERR(S2BT(session)->page_log->pl_open_handle(
      S2BT(session)->page_log, &session->iface, tableid, &block_disagg->plhandle));

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

    __wt_verbose(session, WT_VERB_BLOCK, "close: %s (table_id: %" PRIu64 ")",
      block_disagg->name == NULL ? "" : block_disagg->name, block_disagg->tableid);

    __wt_spin_lock(session, &conn->block_lock);

    /* Reference count is initialized to 1. */
    if (block_disagg->ref == 0 || --block_disagg->ref == 0)
        ret = __block_disagg_destroy(session, block_disagg);

    __wt_spin_unlock(session, &conn->block_lock);

    return (ret);
}

/*
 * __wt_block_disagg_ckpt_size --
 *     Return the size recorded in the most recent checkpoint for the given URIs metadata entry. For
 *     disaggregated storage there is no underlying file, so the checkpoint size in the metadata is
 *     used as the block_size. A missing metadata entry is not an error; *sizep will be zero.
 */
int
__wt_block_disagg_ckpt_size(WT_SESSION_IMPL *session, const char *uri, uint64_t *sizep)
{
    WT_DECL_RET;
    char *fileconf;

    fileconf = NULL;
    *sizep = 0;
    /* Reading checkpoint size requires the file's metadata config string, so look it up first. */
    ret = __wt_metadata_search(session, uri, &fileconf);
    if (ret == 0) {
        ret = __wt_ckpt_last_size(session, fileconf, sizep);
        __wt_free(session, fileconf);
    }
    WT_RET_NOTFOUND_OK(ret);
    return (0);
}

/*
 * __block_disagg_ckpt_size_dhandle --
 *     Return the checkpoint size for the current dhandle. A follower's checkpoint dhandle name
 *     keeps its checkpoint suffix, which is not a metadata key; strip it so the size lookup finds
 *     the table's metadata entry instead of silently returning zero.
 */
static int
__block_disagg_ckpt_size_dhandle(WT_SESSION_IMPL *session, uint64_t *sizep)
{
    WT_DECL_ITEM(name_buf);
    WT_DECL_RET;
    const char *uri;

    uri = session->dhandle->name;
    WT_ERR(__wt_btree_shared_base_name(session, &uri, NULL, &name_buf));
    WT_ERR(__wt_block_disagg_ckpt_size(session, uri, sizep));

err:
    __wt_scr_free(session, &name_buf);
    return (ret);
}

/*
 * __wti_block_disagg_stat --
 *     Set the statistics for a live block handle. For disaggregated storage there is no underlying
 *     file, so block_size is sourced from the most recent checkpoint in the metadata.
 */
int
__wti_block_disagg_stat(
  WT_SESSION_IMPL *session, WT_BLOCK_DISAGG *block_disagg, WT_DSRC_STATS *stats)
{
    uint64_t ckpt_size;

    WT_UNUSED(block_disagg);

    WT_STAT_WRITE(session, stats, block_magic, WT_BLOCK_MAGIC);
    WT_RET(__block_disagg_ckpt_size_dhandle(session, &ckpt_size));
    WT_STAT_WRITE(session, stats, block_size, (int64_t)ckpt_size);
    return (0);
}

/*
 * __wti_block_disagg_manager_size --
 *     Return the size of a live block handle. For disaggregated storage there is no underlying
 *     file, so we return the size of the most recent checkpoint instead.
 */
int
__wti_block_disagg_manager_size(WT_BM *bm, WT_SESSION_IMPL *session, wt_off_t *sizep)
{
    uint64_t ckpt_size;

    WT_UNUSED(bm);

    WT_RET(__block_disagg_ckpt_size_dhandle(session, &ckpt_size));
    *sizep = (wt_off_t)ckpt_size;
    return (0);
}
