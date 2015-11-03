/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __desc_read(WT_SESSION_IMPL *, WT_BLOCK *);

/*
 * __wt_block_manager_truncate --
 *	Truncate a file.
 */
int
__wt_block_manager_truncate(
    WT_SESSION_IMPL *session, const char *filename, uint32_t allocsize)
{
	WT_DECL_RET;
	WT_FH *fh;

	/* Open the underlying file handle. */
	WT_RET(__wt_open(
	    session, filename, false, false, WT_FILE_TYPE_DATA, &fh));

	/* Truncate the file. */
	WT_ERR(__wt_block_truncate(session, fh, (wt_off_t)0));

	/* Write out the file's meta-data. */
	WT_ERR(__wt_desc_init(session, fh, allocsize));

	/*
	 * Ensure the truncated file has made it to disk, then the upper-level
	 * is never surprised.
	 */
	WT_ERR(__wt_fsync(session, fh));

	/* Close the file handle. */
err:	WT_TRET(__wt_close(session, &fh));

	return (ret);
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
	char *path;

	/*
	 * Create the underlying file and open a handle.
	 *
	 * Since WiredTiger schema operations are (currently) non-transactional,
	 * it's possible to see a partially-created file left from a previous
	 * create. Further, there's nothing to prevent users from creating files
	 * in our space. Move any existing files out of the way and complain.
	 */
	for (;;) {
		if ((ret = __wt_open(session,
		    filename, true, true, WT_FILE_TYPE_DATA, &fh)) == 0)
			break;
		WT_ERR_TEST(ret != EEXIST, ret);

		if (tmp == NULL)
			WT_ERR(__wt_scr_alloc(session, 0, &tmp));
		for (suffix = 1;; ++suffix) {
			WT_ERR(__wt_buf_fmt(
			    session, tmp, "%s.%d", filename, suffix));
			WT_ERR(__wt_exist(session, tmp->data, &exists));
			if (!exists) {
				WT_ERR(
				    __wt_rename(session, filename, tmp->data));
				WT_ERR(__wt_msg(session,
				    "unexpected file %s found, renamed to %s",
				    filename, (char *)tmp->data));
				break;
			}
		}
	}

	/* Write out the file's meta-data. */
	ret = __wt_desc_init(session, fh, allocsize);

	/*
	 * Ensure the truncated file has made it to disk, then the upper-level
	 * is never surprised.
	 */
	WT_TRET(__wt_fsync(session, fh));

	/* Close the file handle. */
	WT_TRET(__wt_close(session, &fh));

	/*
	 * Some filesystems require that we sync the directory to be confident
	 * that the file will appear.
	 */
	if (ret == 0 && (ret = __wt_filename(session, filename, &path)) == 0) {
		ret = __wt_directory_sync(session, path);
		__wt_free(session, path);
	}

	/* Undo any create on error. */
	if (ret != 0)
		WT_TRET(__wt_remove(session, filename));

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
#ifdef HAVE_POSIX_FADVISE
	if (conn->direct_io && block->os_cache_max)
		WT_ERR_MSG(session, EINVAL,
		    "os_cache_max not supported in combination with direct_io");
#else
	if (block->os_cache_max)
		WT_ERR_MSG(session, EINVAL,
		    "os_cache_max not supported if posix_fadvise not "
		    "available");
#endif

	/* Configuration: optional immediate write scheduling flag. */
	WT_ERR(__wt_config_gets(session, cfg, "os_cache_dirty_max", &cval));
	block->os_cache_dirty_max = (size_t)cval.val;
#ifdef HAVE_SYNC_FILE_RANGE
	if (conn->direct_io && block->os_cache_dirty_max)
		WT_ERR_MSG(session, EINVAL,
		    "os_cache_dirty_max not supported in combination with "
		    "direct_io");
#else
	if (block->os_cache_dirty_max) {
		/*
		 * Ignore any setting if it is not supported.
		 */
		block->os_cache_dirty_max = 0;
		WT_ERR(__wt_verbose(session, WT_VERB_BLOCK,
		    "os_cache_dirty_max ignored when sync_file_range not "
		    "available"));
	}
#endif

	/* Open the underlying file handle. */
	WT_ERR(__wt_open(session, filename, false, false,
	    readonly ? WT_FILE_TYPE_CHECKPOINT : WT_FILE_TYPE_DATA,
	    &block->fh));

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
 * __wt_desc_init --
 *	Write a file's initial descriptor structure.
 */
int
__wt_desc_init(WT_SESSION_IMPL *session, WT_FH *fh, uint32_t allocsize)
{
	WT_BLOCK_DESC *desc;
	WT_DECL_ITEM(buf);
	WT_DECL_RET;

	/* Use a scratch buffer to get correct alignment for direct I/O. */
	WT_RET(__wt_scr_alloc(session, allocsize, &buf));
	memset(buf->mem, 0, allocsize);

	desc = buf->mem;
	desc->magic = WT_BLOCK_MAGIC;
	desc->majorv = WT_BLOCK_MAJOR_VERSION;
	desc->minorv = WT_BLOCK_MINOR_VERSION;

	/* Update the checksum. */
	desc->cksum = 0;
	desc->cksum = __wt_cksum(desc, allocsize);

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
	uint32_t cksum;

	/* Use a scratch buffer to get correct alignment for direct I/O. */
	WT_RET(__wt_scr_alloc(session, block->allocsize, &buf));

	/* Read the first allocation-sized block and verify the file format. */
	WT_ERR(__wt_read(session,
	    block->fh, (wt_off_t)0, (size_t)block->allocsize, buf->mem));

	desc = buf->mem;
	WT_ERR(__wt_verbose(session, WT_VERB_BLOCK,
	    "%s: magic %" PRIu32
	    ", major/minor: %" PRIu32 "/%" PRIu32
	    ", checksum %#" PRIx32,
	    block->name, desc->magic,
	    desc->majorv, desc->minorv,
	    desc->cksum));

	/*
	 * We fail the open if the checksum fails, or the magic number is wrong
	 * or the major/minor numbers are unsupported for this version.  This
	 * test is done even if the caller is verifying or salvaging the file:
	 * it makes sense for verify, and for salvage we don't overwrite files
	 * without some reason to believe they are WiredTiger files.  The user
	 * may have entered the wrong file name, and is now frantically pounding
	 * their interrupt key.
	 */
	cksum = desc->cksum;
	desc->cksum = 0;
	if (desc->magic != WT_BLOCK_MAGIC ||
	    cksum != __wt_cksum(desc, block->allocsize))
		WT_ERR_MSG(session, WT_ERROR,
		    "%s does not appear to be a WiredTiger file", block->name);

	if (desc->majorv > WT_BLOCK_MAJOR_VERSION ||
	    (desc->majorv == WT_BLOCK_MAJOR_VERSION &&
	    desc->minorv > WT_BLOCK_MINOR_VERSION))
		WT_ERR_MSG(session, WT_ERROR,
		    "unsupported WiredTiger file version: this build only "
		    "supports major/minor versions up to %d/%d, and the file "
		    "is version %d/%d",
		    WT_BLOCK_MAJOR_VERSION, WT_BLOCK_MINOR_VERSION,
		    desc->majorv, desc->minorv);

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
	stats->allocation_size = block->allocsize;
	stats->block_checkpoint_size = (int64_t)block->live.ckpt_size;
	stats->block_magic = WT_BLOCK_MAGIC;
	stats->block_major = WT_BLOCK_MAJOR_VERSION;
	stats->block_minor = WT_BLOCK_MINOR_VERSION;
	stats->block_reuse_bytes = (int64_t)block->live.avail.bytes;
	stats->block_size = block->fh->size;
}

/*
 * __wt_block_manager_size --
 *	Set the size statistic for a file.
 */
int
__wt_block_manager_size(
    WT_SESSION_IMPL *session, const char *filename, WT_DSRC_STATS *stats)
{
	WT_DECL_RET;
	wt_off_t filesize;

	ret = __wt_filesize_name(session, filename, &filesize);
	if (ret != 0)
		WT_RET_MSG(session, ret, "%s: file size", filename);

	stats->block_size = filesize;

	return (0);
}
