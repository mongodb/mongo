/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __desc_read(WT_SESSION_IMPL *, WT_BLOCK *);

/*
 * __wt_block_truncate --
 *	Truncate a file.
 */
int
__wt_block_truncate(WT_SESSION_IMPL *session, const char *filename)
{
	WT_DECL_RET;
	WT_FH *fh;

	/* Open the underlying file handle. */
	WT_RET(__wt_open(session, filename, 0, 0, 1, &fh));

	/* Truncate the file. */
	WT_ERR(__wt_ftruncate(session, fh, (off_t)0));

	/* Write out the file's meta-data. */
	ret = __wt_desc_init(session, fh);

	/* Close the file handle. */
err:	WT_TRET(__wt_close(session, fh));

	return (ret);
}

/*
 * __wt_block_create --
 *	Create a file.
 */
int
__wt_block_create(WT_SESSION_IMPL *session, const char *filename)
{
	WT_DECL_RET;
	WT_FH *fh;

	/* Create the underlying file and open a handle. */
	WT_RET(__wt_open(session, filename, 1, 1, 1, &fh));

	/* Write out the file's meta-data. */
	ret = __wt_desc_init(session, fh);

	/* Close the file handle. */
	WT_TRET(__wt_close(session, fh));

	/* Undo any create on error. */
	if (ret != 0)
		(void)__wt_remove(session, filename);

	return (ret);
}

/*
 * __wt_block_open --
 *	Open a file.
 */
int
__wt_block_open(WT_SESSION_IMPL *session, const char *filename,
    const char *config, const char *cfg[], int forced_salvage, void *blockp)
{
	WT_BLOCK *block;
	WT_CONFIG_ITEM cval;
	WT_DECL_RET;

	WT_UNUSED(cfg);
	*(void **)blockp = NULL;

	/*
	 * Allocate the structure, connect (so error close works), copy the
	 * name.
	 */
	WT_RET(__wt_calloc_def(session, 1, &block));
	WT_ERR(__wt_strdup(session, filename, &block->name));

	/* Get the allocation size. */
	WT_ERR(__wt_config_getones(session, config, "allocation_size", &cval));
	block->allocsize = (uint32_t)cval.val;

	/* Check if configured for checksums. */
	WT_ERR(__wt_config_getones(session, config, "checksum", &cval));
	block->checksum = cval.val == 0 ? 0 : 1;

	/* Page compressor */
	WT_ERR(__wt_config_getones(session, config, "block_compressor", &cval));
	if (cval.len > 0) {
		WT_CONNECTION_IMPL *conn;
		WT_NAMED_COMPRESSOR *ncomp;

		conn = S2C(session);
		TAILQ_FOREACH(ncomp, &conn->compqh, q) {
			if (strncmp(ncomp->name, cval.str, cval.len) == 0) {
				block->compressor = ncomp->compressor;
				break;
			}
		}
		if (block->compressor == NULL)
			WT_ERR_MSG(session, EINVAL,
			    "unknown block_compressor '%.*s'",
			    (int)cval.len, cval.str);
	}

	/* Open the underlying file handle. */
	WT_ERR(__wt_open(session, filename, 0, 0, 1, &block->fh));

	/* Initialize the live checkpoint's lock. */
	__wt_spin_init(session, &block->live_lock);

	/*
	 * Read the description sector.
	 *
	 * Salvage is a special case -- if we're forcing the salvage, we don't
	 * even look at the description sector.
	 */
	if (!forced_salvage)
		WT_ERR(__desc_read(session, block));

	*(void **)blockp = block;
	return (0);

err:	(void)__wt_block_close(session, block);
	return (ret);
}

/*
 * __wt_block_close --
 *	Close a file.
 */
int
__wt_block_close(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
	WT_DECL_RET;

	WT_VERBOSE_RETVAL(session, block, ret, "close");

	ret = __wt_block_checkpoint_unload(session, block);

	if (block->name != NULL)
		__wt_free(session, block->name);

	if (block->fh != NULL)
		WT_TRET(__wt_close(session, block->fh));

	__wt_spin_destroy(session, &block->live_lock);

	__wt_free(session, block);

	return (ret);
}

/*
 * __wt_desc_init --
 *	Write a file's initial descriptor structure.
 */
int
__wt_desc_init(WT_SESSION_IMPL *session, WT_FH *fh)
{
	WT_BLOCK_DESC *desc;
	WT_DECL_RET;
	WT_ITEM *buf;

	/* Use a scratch buffer to get correct alignment for direct I/O. */
	WT_RET(__wt_scr_alloc(session, WT_BLOCK_DESC_SECTOR, &buf));
	memset(buf->mem, 0, WT_BLOCK_DESC_SECTOR);

	desc = buf->mem;
	desc->magic = WT_BLOCK_MAGIC;
	desc->majorv = WT_BLOCK_MAJOR_VERSION;
	desc->minorv = WT_BLOCK_MINOR_VERSION;

	/* Update the checksum. */
	desc->cksum = 0;
	desc->cksum = __wt_cksum(desc, WT_BLOCK_DESC_SECTOR);

	ret = __wt_write(session, fh, (off_t)0, WT_BLOCK_DESC_SECTOR, desc);

	__wt_scr_free(&buf);
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
	WT_DECL_RET;
	WT_ITEM *buf;
	uint32_t cksum;

	/* Use a scratch buffer to get correct alignment for direct I/O. */
	WT_RET(__wt_scr_alloc(session, WT_BLOCK_DESC_SECTOR, &buf));

	/* Read the first sector and verify the file's format. */
	WT_ERR(__wt_read(
	    session, block->fh, (off_t)0, WT_BLOCK_DESC_SECTOR, buf->mem));

	desc = buf->mem;
	WT_VERBOSE_ERR(session, block,
	    "open: magic %" PRIu32
	    ", major/minor: %" PRIu32 "/%" PRIu32
	    ", checksum %#" PRIx32,
	    desc->magic,
	    desc->majorv, desc->minorv,
	    desc->cksum);

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
	    cksum != __wt_cksum(desc, WT_BLOCK_DESC_SECTOR))
		WT_ERR_MSG(session, WT_ERROR,
		    "%s does not appear to be a WiredTiger file", block->name);

	if (desc->majorv > WT_BLOCK_MAJOR_VERSION ||
	    (desc->majorv == WT_BLOCK_MAJOR_VERSION &&
	    desc->minorv > WT_BLOCK_MINOR_VERSION))
		WT_ERR_MSG(session, WT_ERROR,
		    "%s is an unsupported version of a WiredTiger file",
		    block->name);

err:	__wt_scr_free(&buf);
	return (0);
}

/*
 * __wt_block_stat --
 *	Block statistics
 */
void
__wt_block_stat(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
	WT_BSTAT_SET(session, file_size, block->fh->file_size);
	WT_BSTAT_SET(session, file_magic, WT_BLOCK_MAGIC);
	WT_BSTAT_SET(session, file_major, WT_BLOCK_MAJOR_VERSION);
	WT_BSTAT_SET(session, file_minor, WT_BLOCK_MINOR_VERSION);
}
