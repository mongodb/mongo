/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __desc_read(WT_SESSION_IMPL *, WT_BLOCK *);

/*
 * __wt_block_manager_drop --
 *	Drop a file.
 */
int
__wt_block_manager_drop(WT_SESSION_IMPL *session, const char *filename)
{
	 return (__wt_remove_if_exists(session, filename));
}

/*
 * __wt_block_manager_create --
 *	Create a file.
 */
int
__wt_block_manager_create(
    WT_SESSION_IMPL *session, const char *filename, uint32_t allocsize)
{
	WT_DECL_RET;
	WT_DECL_ITEM(tmp);
	WT_FH *fh;
	int suffix;
	bool exists;

	/*
	 * Create the underlying file and open a handle.
	 *
	 * Since WiredTiger schema operations are (currently) non-transactional,
	 * it's possible to see a partially-created file left from a previous
	 * create. Further, there's nothing to prevent users from creating files
	 * in our space. Move any existing files out of the way and complain.
	 */
	for (;;) {
		if ((ret = __wt_open(session, filename, WT_OPEN_FILE_TYPE_DATA,
		    WT_OPEN_CREATE | WT_OPEN_EXCLUSIVE, &fh)) == 0)
			break;
		WT_ERR_TEST(ret != EEXIST, ret);

		if (tmp == NULL)
			WT_ERR(__wt_scr_alloc(session, 0, &tmp));
		for (suffix = 1;; ++suffix) {
			WT_ERR(__wt_buf_fmt(
			    session, tmp, "%s.%d", filename, suffix));
			WT_ERR(__wt_fs_exist(session, tmp->data, &exists));
			if (!exists) {
				WT_ERR(__wt_fs_rename(
				    session, filename, tmp->data));
				WT_ERR(__wt_msg(session,
				    "unexpected file %s found, renamed to %s",
				    filename, (const char *)tmp->data));
				break;
			}
		}
	}

	/* Write out the file's meta-data. */
	ret = __wt_desc_write(session, fh, allocsize);

	/*
	 * Ensure the truncated file has made it to disk, then the upper-level
	 * is never surprised.
	 */
	WT_TRET(__wt_fsync(session, fh, true));

	/* Close the file handle. */
	WT_TRET(__wt_close(session, &fh));

	/*
	 * Some filesystems require that we sync the directory to be confident
	 * that the file will appear.
	 */
	if (ret == 0)
		WT_TRET(__wt_fs_directory_sync(session, filename));

	/* Undo any create on error. */
	if (ret != 0)
		WT_TRET(__wt_fs_remove(session, filename));

err:	__wt_scr_free(session, &tmp);

	return (ret);
}

/*
 * __block_destroy --
 *	Destroy a block handle.
 */
static int
__block_destroy(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	uint64_t bucket;

	conn = S2C(session);
	bucket = block->name_hash % WT_HASH_ARRAY_SIZE;
	WT_CONN_BLOCK_REMOVE(conn, block, bucket);

	__wt_free(session, block->name);

	if (block->fh != NULL)
		WT_TRET(__wt_close(session, &block->fh));

	__wt_spin_destroy(session, &block->live_lock);

	__wt_overwrite_and_free(session, block);

	return (ret);
}

/*
 * __wt_block_configure_first_fit --
 *	Configure first-fit allocation.
 */
void
__wt_block_configure_first_fit(WT_BLOCK *block, bool on)
{
	/*
	 * Switch to first-fit allocation so we rewrite blocks at the start of
	 * the file; use atomic instructions because checkpoints also configure
	 * first-fit allocation, and this way we stay on first-fit allocation
	 * as long as any operation wants it.
	 */
	if (on)
		(void)__wt_atomic_add32(&block->allocfirst, 1);
	else
		(void)__wt_atomic_sub32(&block->allocfirst, 1);
}

/*
 * __wt_block_open --
 *	Open a block handle.
 */
int
__wt_block_open(WT_SESSION_IMPL *session,
    const char *filename, const char *cfg[],
    bool forced_salvage, bool readonly, uint32_t allocsize, WT_BLOCK **blockp)
{
	WT_BLOCK *block;
	WT_CONFIG_ITEM cval;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	uint64_t bucket, hash;
	uint32_t flags;

	WT_RET(__wt_verbose(session, WT_VERB_BLOCK, "open: %s", filename));

	conn = S2C(session);
	*blockp = block = NULL;
	hash = __wt_hash_city64(filename, strlen(filename));
	bucket = hash % WT_HASH_ARRAY_SIZE;
	__wt_spin_lock(session, &conn->block_lock);
	TAILQ_FOREACH(block, &conn->blockhash[bucket], hashq) {
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
	 * Note: set the block's name-hash value before any work that can fail
	 * because cleanup calls the block destroy code which uses that hash
	 * value to remove the block from the underlying linked lists.
	 */
	WT_ERR(__wt_calloc_one(session, &block));
	block->ref = 1;
	block->name_hash = hash;
	block->allocsize = allocsize;
	WT_CONN_BLOCK_INSERT(conn, block, bucket);

	WT_ERR(__wt_strdup(session, filename, &block->name));

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
	if (readonly && FLD_ISSET(conn->direct_io, WT_DIRECT_IO_CHECKPOINT))
		LF_SET(WT_OPEN_DIRECTIO);
	if (!readonly && FLD_ISSET(conn->direct_io, WT_DIRECT_IO_DATA))
		LF_SET(WT_OPEN_DIRECTIO);
	WT_ERR(__wt_open(
	    session, filename, WT_OPEN_FILE_TYPE_DATA, flags, &block->fh));

	/* Set the file's size. */
	WT_ERR(__wt_filesize(session, block->fh, &block->size));

	/* Initialize the live checkpoint's lock. */
	WT_ERR(__wt_spin_init(session, &block->live_lock, "block manager"));

	/*
	 * Read the description information from the first block.
	 *
	 * Salvage is a special case: if we're forcing the salvage, we don't
	 * look at anything, including the description information.
	 */
	if (!forced_salvage)
		WT_ERR(__desc_read(session, block));

	*blockp = block;
	__wt_spin_unlock(session, &conn->block_lock);
	return (0);

err:	if (block != NULL)
		WT_TRET(__block_destroy(session, block));
	__wt_spin_unlock(session, &conn->block_lock);
	return (ret);
}

/*
 * __wt_block_close --
 *	Close a block handle.
 */
int
__wt_block_close(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;

	if (block == NULL)				/* Safety check */
		return (0);

	conn = S2C(session);

	WT_TRET(__wt_verbose(session, WT_VERB_BLOCK,
	    "close: %s", block->name == NULL ? "" : block->name ));

	__wt_spin_lock(session, &conn->block_lock);

			/* Reference count is initialized to 1. */
	if (block->ref == 0 || --block->ref == 0)
		WT_TRET(__block_destroy(session, block));

	__wt_spin_unlock(session, &conn->block_lock);

	return (ret);
}

/*
 * __wt_desc_write --
 *	Write a file's initial descriptor structure.
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
	 * Checksum a little-endian version of the header, and write everything
	 * in little-endian format. The checksum is (potentially) returned in a
	 * big-endian format, swap it into place in a separate step.
	 */
	desc = buf->mem;
	desc->magic = WT_BLOCK_MAGIC;
	desc->majorv = WT_BLOCK_MAJOR_VERSION;
	desc->minorv = WT_BLOCK_MINOR_VERSION;
	desc->cksum = 0;
	__wt_block_desc_byteswap(desc);
	desc->cksum = __wt_cksum(desc, allocsize);
#ifdef WORDS_BIGENDIAN
	desc->cksum = __wt_bswap32(desc->cksum);
#endif
	ret = __wt_write(session, fh, (wt_off_t)0, (size_t)allocsize, desc);

	__wt_scr_free(session, &buf);
	return (ret);
}

/*
 * __desc_read --
 *	Read and verify the file's metadata.
 */
static int
__desc_read(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
	WT_BLOCK_DESC *desc;
	WT_DECL_ITEM(buf);
	WT_DECL_RET;
	uint32_t cksum_calculate, cksum_tmp;

	/* If in-memory, we don't read or write the descriptor structure. */
	if (F_ISSET(S2C(session), WT_CONN_IN_MEMORY))
		return (0);

	/* Use a scratch buffer to get correct alignment for direct I/O. */
	WT_RET(__wt_scr_alloc(session, block->allocsize, &buf));

	/* Read the first allocation-sized block and verify the file format. */
	WT_ERR(__wt_read(session,
	    block->fh, (wt_off_t)0, (size_t)block->allocsize, buf->mem));

	/*
	 * Handle little- and big-endian objects. Objects are written in little-
	 * endian format: save the header checksum, and calculate the checksum
	 * for the header in its little-endian form. Then, restore the header's
	 * checksum, and byte-swap the whole thing as necessary, leaving us with
	 * a calculated checksum that should match the checksum in the header.
	 */
	desc = buf->mem;
	cksum_tmp = desc->cksum;
	desc->cksum = 0;
	cksum_calculate = __wt_cksum(desc, block->allocsize);
	desc->cksum = cksum_tmp;
	__wt_block_desc_byteswap(desc);

	/*
	 * We fail the open if the checksum fails, or the magic number is wrong
	 * or the major/minor numbers are unsupported for this version.  This
	 * test is done even if the caller is verifying or salvaging the file:
	 * it makes sense for verify, and for salvage we don't overwrite files
	 * without some reason to believe they are WiredTiger files.  The user
	 * may have entered the wrong file name, and is now frantically pounding
	 * their interrupt key.
	 */
	if (desc->magic != WT_BLOCK_MAGIC || desc->cksum != cksum_calculate)
		WT_ERR_MSG(session, WT_ERROR,
		    "%s does not appear to be a WiredTiger file", block->name);

	if (desc->majorv > WT_BLOCK_MAJOR_VERSION ||
	    (desc->majorv == WT_BLOCK_MAJOR_VERSION &&
	    desc->minorv > WT_BLOCK_MINOR_VERSION))
		WT_ERR_MSG(session, WT_ERROR,
		    "unsupported WiredTiger file version: this build only "
		    "supports major/minor versions up to %d/%d, and the file "
		    "is version %" PRIu16 "/%" PRIu16,
		    WT_BLOCK_MAJOR_VERSION, WT_BLOCK_MINOR_VERSION,
		    desc->majorv, desc->minorv);

	WT_ERR(__wt_verbose(session, WT_VERB_BLOCK,
	    "%s: magic %" PRIu32
	    ", major/minor: %" PRIu32 "/%" PRIu32
	    ", checksum %#" PRIx32,
	    block->name, desc->magic,
	    desc->majorv, desc->minorv,
	    desc->cksum));

err:	__wt_scr_free(session, &buf);
	return (ret);
}

/*
 * __wt_block_stat --
 *	Set the statistics for a live block handle.
 */
void
__wt_block_stat(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_DSRC_STATS *stats)
{
	WT_UNUSED(session);

	/*
	 * Reading from the live system's structure normally requires locking,
	 * but it's an 8B statistics read, there's no need.
	 */
	WT_STAT_WRITE(stats, allocation_size, block->allocsize);
	WT_STAT_WRITE(
	    stats, block_checkpoint_size, (int64_t)block->live.ckpt_size);
	WT_STAT_WRITE(stats, block_magic, WT_BLOCK_MAGIC);
	WT_STAT_WRITE(stats, block_major, WT_BLOCK_MAJOR_VERSION);
	WT_STAT_WRITE(stats, block_minor, WT_BLOCK_MINOR_VERSION);
	WT_STAT_WRITE(
	    stats, block_reuse_bytes, (int64_t)block->live.avail.bytes);
	WT_STAT_WRITE(stats, block_size, block->size);
}

/*
 * __wt_block_manager_size --
 *	Return the size of a live block handle.
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
 *	Return the size of a named file.
 */
int
__wt_block_manager_named_size(
    WT_SESSION_IMPL *session, const char *name, wt_off_t *sizep)
{
	return (__wt_fs_size(session, name, sizep));
}
