/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __desc_read(WT_SESSION_IMPL *, uint32_t allocsize, WT_BLOCK *);

/*
 * __wt_block_manager_drop --
 *     Drop a file.
 */
int
__wt_block_manager_drop(WT_SESSION_IMPL *session, const char *filename, bool durable)
{
    return (__wt_remove_if_exists(session, filename, durable));
}

/*
 * __wt_block_manager_drop_object --
 *     Drop a shared object file from the bucket directory and the cache directory.
 */
int
__wt_block_manager_drop_object(
  WT_SESSION_IMPL *session, WT_BUCKET_STORAGE *bstorage, const char *filename, bool durable)
{
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;

    WT_UNUSED(durable);

    WT_RET(__wt_scr_alloc(session, 0, &tmp));

    /* Generate the name of the shared object file with the bucket prefix. */
    WT_ERR(__wt_buf_fmt(session, tmp, "%s%s", bstorage->bucket_prefix, filename));
    WT_WITH_BUCKET_STORAGE(bstorage, session, ret = __wt_fs_remove(session, tmp->data, false));
    WT_ERR(ret);

err:
    __wt_scr_free(session, &tmp);
    return (ret);
}

/*
 * __wt_block_manager_create --
 *     Create a file.
 */
int
__wt_block_manager_create(WT_SESSION_IMPL *session, const char *filename, uint32_t allocsize)
{
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;
    WT_FH *fh;
    int suffix;
    bool exists;

    WT_ERR(__wt_scr_alloc(session, 0, &tmp));

    /*
     * Create the underlying file and open a handle.
     *
     * Since WiredTiger schema operations are (currently) non-transactional, it's possible to see a
     * partially-created file left from a previous create. Further, there's nothing to prevent users
     * from creating files in our space. Move any existing files out of the way and complain.
     */
    for (;;) {
        if ((ret = __wt_open(session, filename, WT_FS_OPEN_FILE_TYPE_DATA,
               WT_FS_OPEN_CREATE | WT_FS_OPEN_DURABLE | WT_FS_OPEN_EXCLUSIVE, &fh)) == 0)
            break;
        WT_ERR_TEST(ret != EEXIST, ret, false);

        for (suffix = 1;; ++suffix) {
            WT_ERR(__wt_buf_fmt(session, tmp, "%s.%d", filename, suffix));
            WT_ERR(__wt_fs_exist(session, tmp->data, &exists));
            if (!exists) {
                WT_ERR(__wt_fs_rename(session, filename, tmp->data, false));
                __wt_verbose_notice(session, WT_VERB_BLOCK,
                  "unexpected file %s found, renamed to %s", filename, (const char *)tmp->data);
                break;
            }
        }
    }

    /* Write out the file's meta-data. */
    ret = __wt_desc_write(session, fh, allocsize);

    /*
     * Ensure the truncated file has made it to disk, then the upper-level is never surprised.
     */
    WT_TRET(__wt_fsync(session, fh, true));

    /* Close the file handle. */
    WT_TRET(__wt_close(session, &fh));

    /* Undo any create on error. */
    if (ret != 0)
        WT_TRET(__wt_fs_remove(session, filename, false));

err:
    __wt_scr_free(session, &tmp);

    return (ret);
}

/*
 * __wt_block_close --
 *     Close a block handle.
 */
int
__wt_block_close(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    uint64_t bucket, hash;

    conn = S2C(session);

    if (block == NULL) /* Safety check, if failed to initialize. */
        return (0);

    __wt_verbose(session, WT_VERB_BLOCK, "close: %s", block->name == NULL ? "" : block->name);

    /* If we failed during allocation, the block won't have been linked. */
    if (block->linked) {
        hash = __wt_hash_city64(block->name, strlen(block->name));
        bucket = hash & (conn->hash_size - 1);
        WT_CONN_BLOCK_REMOVE(conn, block, bucket);
    }

    __wt_free(session, block->name);
    __wt_spin_destroy(session, &block->cache_lock);
    __wt_free(session, block->related);

    WT_TRET(__wt_close(session, &block->fh));

    __wt_spin_destroy(session, &block->live_lock);
    __wt_block_ckpt_destroy(session, &block->live);

    __wt_overwrite_and_free(session, block);

    return (ret);
}

/*
 * __wt_block_configure_first_fit --
 *     Configure first-fit allocation.
 */
void
__wt_block_configure_first_fit(WT_BLOCK *block, bool on)
{
    /*
     * Switch to first-fit allocation so we rewrite blocks at the start of the file; use atomic
     * instructions because checkpoints also configure first-fit allocation, and this way we stay on
     * first-fit allocation as long as any operation wants it.
     */
    if (on)
        (void)__wt_atomic_add32(&block->allocfirst, 1);
    else
        (void)__wt_atomic_sub32(&block->allocfirst, 1);
}

/*
 * __wt_block_open --
 *     Open a block handle.
 */
int
__wt_block_open(WT_SESSION_IMPL *session, const char *filename, uint32_t objectid,
  const char *cfg[], bool forced_salvage, bool readonly, bool fixed, uint32_t allocsize,
  WT_BLOCK **blockp)
{
    WT_BLOCK *block;
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    uint64_t bucket, hash;
    uint32_t flags;

    *blockp = NULL;

    __wt_verbose(session, WT_VERB_BLOCK, "open: %s", filename);

    block = NULL;
    conn = S2C(session);

    /* Block objects can be shared (although there can be only one writer). */
    hash = __wt_hash_city64(filename, strlen(filename));
    bucket = hash & (conn->hash_size - 1);
    __wt_spin_lock(session, &conn->block_lock);
    TAILQ_FOREACH (block, &conn->blockhash[bucket], hashq)
        if (block->objectid == objectid && strcmp(filename, block->name) == 0) {
            ++block->ref;
            *blockp = block;
            __wt_spin_unlock(session, &conn->block_lock);
            return (0);
        }

    /* Basic structure allocation, initialization. */
    WT_ERR(__wt_calloc_one(session, &block));
    WT_ERR(__wt_strdup(session, filename, &block->name));
    block->objectid = objectid;
    block->ref = 1;
    WT_CONN_BLOCK_INSERT(conn, block, bucket);
    block->linked = true;

    /* Initialize the block cache layer lock. */
    WT_ERR(__wt_spin_init(session, &block->cache_lock, "block cache"));

    /* If not passed an allocation size, get one from the configuration. */
    if (allocsize == 0) {
        WT_ERR(__wt_config_gets(session, cfg, "allocation_size", &cval));
        allocsize = (uint32_t)cval.val;
    }
    block->allocsize = allocsize;

    WT_ERR(__wt_config_gets(session, cfg, "block_allocation", &cval));
    block->allocfirst = WT_STRING_MATCH("first", cval.str, cval.len);

    /* Configuration: optional OS buffer cache maximum size. */
    WT_ERR(__wt_config_gets(session, cfg, "os_cache_max", &cval));
    block->os_cache_max = (size_t)cval.val;

    /* Configuration: optional immediate write scheduling flag. */
    WT_ERR(__wt_config_gets(session, cfg, "os_cache_dirty_max", &cval));
    block->os_cache_dirty_max = (size_t)cval.val;

    /* Set the file extension information. */
    block->extend_len = conn->data_extend_len;

    /*
     * Open the underlying file handle.
     *
     * "direct_io=checkpoint" configures direct I/O for readonly data files.
     */
    flags = 0;
    WT_ERR(__wt_config_gets(session, cfg, "access_pattern_hint", &cval));
    if (WT_STRING_MATCH("random", cval.str, cval.len))
        LF_SET(WT_FS_OPEN_ACCESS_RAND);
    else if (WT_STRING_MATCH("sequential", cval.str, cval.len))
        LF_SET(WT_FS_OPEN_ACCESS_SEQ);

    if (fixed)
        LF_SET(WT_FS_OPEN_FIXED);
    if (readonly && FLD_ISSET(conn->direct_io, WT_DIRECT_IO_CHECKPOINT))
        LF_SET(WT_FS_OPEN_DIRECTIO);
    if (!readonly && FLD_ISSET(conn->direct_io, WT_DIRECT_IO_DATA))
        LF_SET(WT_FS_OPEN_DIRECTIO);
    /*
     * Tiered storage sets file permissions to readonly, but nobody else does. This flag means the
     * underlying file is read-only, and NOT that the handle access pattern is read-only.
     */
    if (readonly)
        LF_SET(WT_FS_OPEN_READONLY);
    WT_ERR(__wt_open(session, filename, WT_FS_OPEN_FILE_TYPE_DATA, flags, &block->fh));

    /* Set the file's size. */
    WT_ERR(__wt_filesize(session, block->fh, &block->size));
    /*
     * If we're opening a file and it only contains a header and we're doing incremental backup
     * indicate this so that the first checkpoint is sure to set all the bits as dirty to cover the
     * header so that the header gets copied.
     */
    if (block->size == allocsize && F_ISSET(conn, WT_CONN_INCR_BACKUP))
        block->created_during_backup = true;

    /* Initialize the live checkpoint's lock. */
    WT_ERR(__wt_spin_init(session, &block->live_lock, "block manager"));

    /*
     * Read the description information from the first block.
     *
     * Salvage is a special case: if we're forcing the salvage, we don't look at anything, including
     * the description information.
     */
    if (!forced_salvage)
        WT_ERR(__desc_read(session, allocsize, block));

    __wt_spin_unlock(session, &conn->block_lock);

    *blockp = block;
    return (0);

err:
    __wt_spin_unlock(session, &conn->block_lock);
    WT_TRET(__wt_block_close(session, block));

    return (ret);
}

/*
 * __wt_desc_write --
 *     Write a file's initial descriptor structure.
 */
int
__wt_desc_write(WT_SESSION_IMPL *session, WT_FH *fh, uint32_t allocsize)
{
    WT_BLOCK_DESC *desc;
    WT_DECL_ITEM(buf);
    WT_DECL_RET;

    /* If in-memory, we don't read or write the descriptor structure. */
    if (F_ISSET(S2C(session), WT_CONN_IN_MEMORY))
        return (0);

    /* Use a scratch buffer to get correct alignment for direct I/O. */
    WT_RET(__wt_scr_alloc(session, allocsize, &buf));
    memset(buf->mem, 0, allocsize);

    /*
     * Checksum a little-endian version of the header, and write everything in little-endian format.
     * The checksum is (potentially) returned in a big-endian format, swap it into place in a
     * separate step.
     */
    desc = buf->mem;
    desc->magic = WT_BLOCK_MAGIC;
    desc->majorv = WT_BLOCK_MAJOR_VERSION;
    desc->minorv = WT_BLOCK_MINOR_VERSION;
    desc->checksum = 0;
    __wt_block_desc_byteswap(desc);
    desc->checksum = __wt_checksum(desc, allocsize);
#ifdef WORDS_BIGENDIAN
    desc->checksum = __wt_bswap32(desc->checksum);
#endif
    ret = __wt_write(session, fh, (wt_off_t)0, (size_t)allocsize, desc);

    __wt_scr_free(session, &buf);
    return (ret);
}

/*
 * __desc_read --
 *     Read and verify the file's metadata.
 */
static int
__desc_read(WT_SESSION_IMPL *session, uint32_t allocsize, WT_BLOCK *block)
{
    WT_BLOCK_DESC *desc;
    WT_DECL_ITEM(buf);
    WT_DECL_RET;
    uint32_t checksum_saved, checksum_tmp;
    bool checksum_matched;

    /* If in-memory, we don't read or write the descriptor structure. */
    if (F_ISSET(S2C(session), WT_CONN_IN_MEMORY))
        return (0);

    /*
     * If a data file is smaller than the allocation size, we're not going to be able to read the
     * descriptor block.
     *
     * If we're performing rollback to stable as part of recovery, we should treat this as if the
     * file has been deleted; that is, to log an error but continue on.
     *
     * In the general case, we should return a generic error and signal that we've detected data
     * corruption.
     *
     * FIXME-WT-5832: MongoDB relies heavily on the error codes reported when opening cursors (which
     * hits this logic if the relevant data handle isn't already open). However this code gets run
     * in rollback to stable as part of recovery where we want to skip any corrupted data files
     * temporarily to allow MongoDB to initiate salvage. This is why we've been forced into this
     * situation. We should address this as part of WT-5832 and clarify what error codes we expect
     * to be returning across the API boundary.
     */
    if (block->size < allocsize) {
        if (F_ISSET(session, WT_SESSION_ROLLBACK_TO_STABLE))
            ret = ENOENT;
        else {
            ret = WT_ERROR;
            F_SET(S2C(session), WT_CONN_DATA_CORRUPTION);
        }
        WT_RET_MSG(session, ret,
          "File %s is smaller than allocation size; file size=%" PRId64 ", alloc size=%" PRIu32,
          block->name, block->size, allocsize);
    }

    /* Use a scratch buffer to get correct alignment for direct I/O. */
    WT_RET(__wt_scr_alloc(session, allocsize, &buf));

    /* Read the first allocation-sized block and verify the file format. */
    WT_ERR(__wt_read(session, block->fh, (wt_off_t)0, (size_t)allocsize, buf->mem));

    /*
     * Handle little- and big-endian objects. Objects are written in little- endian format: save the
     * header checksum, and calculate the checksum for the header in its little-endian form. Then,
     * restore the header's checksum, and byte-swap the whole thing as necessary, leaving us with a
     * calculated checksum that should match the checksum in the header.
     */
    desc = buf->mem;
    checksum_saved = checksum_tmp = desc->checksum;
#ifdef WORDS_BIGENDIAN
    checksum_tmp = __wt_bswap32(checksum_tmp);
#endif
    desc->checksum = 0;
    checksum_matched = __wt_checksum_match(desc, allocsize, checksum_tmp);
    desc->checksum = checksum_saved;
    __wt_block_desc_byteswap(desc);

    /*
     * We fail the open if the checksum fails, or the magic number is wrong or the major/minor
     * numbers are unsupported for this version. This test is done even if the caller is verifying
     * or salvaging the file: it makes sense for verify, and for salvage we don't overwrite files
     * without some reason to believe they are WiredTiger files. The user may have entered the wrong
     * file name, and is now frantically pounding their interrupt key.
     */
    if (desc->magic != WT_BLOCK_MAGIC || !checksum_matched) {
        if (strcmp(block->name, WT_METAFILE) == 0 || strcmp(block->name, WT_HS_FILE) == 0)
            WT_ERR_MSG(session, WT_TRY_SALVAGE, "%s is corrupted", block->name);
        /*
         * If we're doing an import, we can't expect to be able to verify checksums since we don't
         * know the allocation size being used. This isn't an error so we should just return success
         * and let import get whatever information it needs.
         */
        if (F_ISSET(session, WT_SESSION_IMPORT_REPAIR))
            goto err;

        if (F_ISSET(session, WT_SESSION_ROLLBACK_TO_STABLE))
            ret = ENOENT;
        else
            WT_ERR_MSG(
              session, WT_ERROR, "%s does not appear to be a WiredTiger file", block->name);
    }

    if (desc->majorv > WT_BLOCK_MAJOR_VERSION ||
      (desc->majorv == WT_BLOCK_MAJOR_VERSION && desc->minorv > WT_BLOCK_MINOR_VERSION))
        WT_ERR_MSG(session, WT_ERROR,
          "unsupported WiredTiger file version: this build only supports major/minor versions up "
          "to %d/%d, and the file is version %" PRIu16 "/%" PRIu16,
          WT_BLOCK_MAJOR_VERSION, WT_BLOCK_MINOR_VERSION, desc->majorv, desc->minorv);

    __wt_verbose(session, WT_VERB_BLOCK, "%s: magic %" PRIu32 ", major/minor: %" PRIu32 "/%" PRIu32,
      block->name, desc->magic, desc->majorv, desc->minorv);

err:
    __wt_scr_free(session, &buf);
    return (ret);
}

/*
 * __wt_block_stat --
 *     Set the statistics for a live block handle.
 */
void
__wt_block_stat(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_DSRC_STATS *stats)
{
    /*
     * Reading from the live system's structure normally requires locking, but it's an 8B statistics
     * read, there's no need.
     */
    WT_STAT_WRITE(session, stats, allocation_size, block->allocsize);
    WT_STAT_WRITE(session, stats, block_checkpoint_size, (int64_t)block->live.ckpt_size);
    WT_STAT_WRITE(session, stats, block_magic, WT_BLOCK_MAGIC);
    WT_STAT_WRITE(session, stats, block_major, WT_BLOCK_MAJOR_VERSION);
    WT_STAT_WRITE(session, stats, block_minor, WT_BLOCK_MINOR_VERSION);
    WT_STAT_WRITE(session, stats, block_reuse_bytes, (int64_t)block->live.avail.bytes);
    WT_STAT_WRITE(session, stats, block_size, block->size);
}

/*
 * __wt_block_manager_size --
 *     Return the size of a live block handle.
 */
int
__wt_block_manager_size(WT_BM *bm, WT_SESSION_IMPL *session, wt_off_t *sizep)
{
    WT_UNUSED(session);

    *sizep = bm->block->size;
    return (0);
}

/*
 * __wt_block_manager_named_size --
 *     Return the size of a named file.
 */
int
__wt_block_manager_named_size(WT_SESSION_IMPL *session, const char *name, wt_off_t *sizep)
{
    return (__wt_fs_size(session, name, sizep));
}
