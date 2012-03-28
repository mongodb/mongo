/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __desc_read(WT_SESSION_IMPL *, WT_BLOCK *, int);
static int __desc_update(WT_SESSION_IMPL *, WT_BLOCK *);

/*
 * __wt_block_truncate --
 *	Truncate a file.
 */
int
__wt_block_truncate(WT_SESSION_IMPL *session, const char *filename)
{
	WT_FH *fh;
	int ret;

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
	WT_FH *fh;
	int ret;

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
    const char *config, const char *cfg[], int salvage, void *retp)
{
	WT_BLOCK *block;
	WT_CONFIG_ITEM cval;
	WT_CONNECTION_IMPL *conn;
	WT_NAMED_COMPRESSOR *ncomp;
	int ret;

	conn = S2C(session);

	/*
	 * Allocate the structure, connect (so error close works), copy the
	 * name.
	 */
	WT_RET(__wt_calloc_def(session, 1, &block));
	WT_ERR(__wt_strdup(session, filename, &block->name));

	/* Initialize the free-list structures. */
	__wt_block_freelist_open(session, block);

	/* Open the underlying file handle. */
	WT_ERR(__wt_open(session, filename, 0, 0, 1, &block->fh));

	/* Get the allocation size. */
	WT_ERR(__wt_config_getones(session, config, "allocation_size", &cval));
	block->allocsize = (uint32_t)cval.val;

	/* Check if configured for checksums. */
	WT_ERR(__wt_config_getones(session, config, "checksum", &cval));
	block->checksum = cval.val == 0 ? 0 : 1;

	/* Page compressor */
	WT_RET(__wt_config_getones(session, config, "block_compressor", &cval));
	if (cval.len > 0) {
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

	/*
	 * Normally we read the file's meta-data to see if this is a WiredTiger
	 * file.  But, if it's a salvage operation and force is set, we ignore
	 * the file's format entirely.
	 */
	cval.val = 0;
	if (salvage) {
		ret = __wt_config_gets(session, cfg, "force", &cval);
		if (ret != 0 && ret != WT_NOTFOUND)
			WT_RET(ret);
	}
	if (cval.val == 0)
		WT_ERR(__desc_read(session, block, salvage));

	/* If not an open for a salvage operation, read the freelist. */
	if (!salvage && block->free_offset != WT_BLOCK_INVALID_OFFSET) {
		WT_ERR(__wt_block_extlist_read(session, block, &block->free,
		    block->free_offset, block->free_size, block->free_cksum));

		/* Insert the free-list itself into the linked list. */
		WT_ERR(__wt_block_free(session,
		    block, block->free_offset, (off_t)block->free_size));
	}

	F_SET(block, WT_BLOCK_OK);
	*(void **)retp = block;
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
	int ret;

	ret = 0;

	/*
	 * If the file was active, write out the free-list and update the
	 * file's description.
	 */
	if (F_ISSET(block, WT_BLOCK_OK)) {
		WT_TRET(__wt_block_extlist_write(session, block,
		    &block->free, &block->free_offset,
		    &block->free_size, &block->free_cksum));
		WT_TRET(__desc_update(session, block));
	}

	if (block->name != NULL)
		__wt_free(session, block->name);

	if (block->fh != NULL)
		WT_RET(__wt_close(session, block->fh));

	__wt_block_freelist_close(session, block);

	__wt_free(session, block->fragbits);

	__wt_free(session, block);
	return (ret);
}

/*
 * __desc_read --
 *	Read and verify the file's metadata.
 */
static int
__desc_read(WT_SESSION_IMPL *session, WT_BLOCK *block, int salvage)
{
	WT_BLOCK_DESC *desc;
	WT_ITEM *buf;
	uint32_t cksum;
	int ret;

	WT_RET(__wt_scr_alloc(session, WT_BLOCK_DESC_SECTOR, &buf));

	/*
	 * We currently always do the verification step, because it's cheap
	 * and we only do it the first time a file is opened.
	 *
	 * Read the first sector.
	 */
	WT_ERR(__wt_read(
	    session, block->fh, (off_t)0, WT_BLOCK_DESC_SECTOR, buf->mem));

	desc = (WT_BLOCK_DESC *)buf->mem;
	WT_VERBOSE(session, block,
	    "open: magic %" PRIu32
	    ", major/minor: %" PRIu32 "/%" PRIu32
	    ", checksum %#" PRIx32
	    ", free offset/size %" PRIu64 "/%" PRIu32
	    ", write-generation %" PRIu64,
	    desc->magic,
	    desc->majorv, desc->minorv,
	    desc->cksum,
	    desc->free_offset, desc->free_size,
	    desc->write_gen);

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
	    cksum != __wt_cksum(buf->mem, WT_BLOCK_DESC_SECTOR))
		WT_ERR_MSG(session, WT_ERROR, "%s %s%s",
		    "does not appear to be a WiredTiger file",
		    block->name,
		    salvage ? "; to salvage this file, configure the salvage "
		    "operation with the force flag" : "");

	if (desc->majorv > WT_BLOCK_MAJOR_VERSION ||
	    (desc->majorv == WT_BLOCK_MAJOR_VERSION &&
	    desc->minorv > WT_BLOCK_MINOR_VERSION))
		WT_ERR_MSG(session, WT_ERROR,
		    "%s is an unsupported version of a WiredTiger file",
		    block->name);

	block->write_gen = desc->write_gen;

	/* That's all we check for salvage. */
	if (!salvage) {
		if ((desc->free_offset != WT_BLOCK_INVALID_OFFSET &&
		    desc->free_offset + desc->free_size >
		    (uint64_t)block->fh->file_size))
			WT_RET_MSG(session, WT_ERROR,
			    "free list offset references "
			    "non-existent file space");

		block->free_offset = (off_t)desc->free_offset;
		block->free_size = desc->free_size;
		block->free_cksum = desc->free_cksum;
	}

err:	__wt_scr_free(&buf);
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
	WT_ITEM *buf;
	int ret;

	WT_RET(__wt_scr_alloc(session, WT_BLOCK_DESC_SECTOR, &buf));
	desc = (WT_BLOCK_DESC *)buf->mem;

	desc->magic = WT_BLOCK_MAGIC;
	desc->majorv = WT_BLOCK_MAJOR_VERSION;
	desc->minorv = WT_BLOCK_MINOR_VERSION;

	desc->free_offset = WT_BLOCK_INVALID_OFFSET;
	desc->free_size = desc->free_cksum = 0;

	/* Update the checksum. */
	desc->cksum = 0;
	desc->cksum = __wt_cksum(buf->mem, WT_BLOCK_DESC_SECTOR);

	ret = __wt_write(session, fh, (off_t)0, WT_BLOCK_DESC_SECTOR, buf->mem);

	__wt_scr_free(&buf);
	return (ret);
}

/*
 * __desc_update --
 *	Update the file's descriptor structure.
 */
static int
__desc_update(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
	WT_BLOCK_DESC *desc;
	WT_ITEM *buf;
	int ret;

	WT_RET(__wt_scr_alloc(session, WT_BLOCK_DESC_SECTOR, &buf));

	/* Read the first sector. */
	WT_ERR(__wt_read(
	    session, block->fh, (off_t)0, WT_BLOCK_DESC_SECTOR, buf->mem));
	desc = (WT_BLOCK_DESC *)buf->mem;

	/* See if anything has changed. */
	if (desc->free_offset == (uint64_t)block->free_offset &&
	    desc->free_size == block->free_size &&
	    desc->write_gen == block->write_gen)
		return (0);

	WT_VERBOSE(session, block,
	    "resetting free list [offset %" PRIuMAX ", size %" PRIu32 "]",
	    (uintmax_t)block->free_offset, block->free_size);

	desc->free_offset = (uint64_t)block->free_offset;
	desc->free_size = block->free_size;
	desc->free_cksum = block->free_cksum;

	desc->write_gen = block->write_gen;

	/* Update the checksum. */
	desc->cksum = 0;
	desc->cksum = __wt_cksum(buf->mem, WT_BLOCK_DESC_SECTOR);

	/* Write the first sector. */
	ret = __wt_write(session,
	    block->fh, (off_t)0, WT_BLOCK_DESC_SECTOR, buf->mem);

err:	__wt_scr_free(&buf);
	return (ret);
}
