/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static int  __desc_read(WT_SESSION_IMPL *, WT_BLOCK *, int);
static int  __desc_update(WT_SESSION_IMPL *, WT_BLOCK *);

/*
 * __wt_block_create --
 *	Create a file.
 */
int
__wt_block_create(WT_SESSION_IMPL *session, const char *filename)
{
	WT_FH *fh;
	int exist, ret;

	/* Check to see if the file exists -- we don't want to overwrite it. */
	WT_RET(__wt_exist(session, filename, &exist));
	if (exist) {
		__wt_errx(session,
		    "the file %s already exists; to re-create it, remove it "
		    "first, then create it",
		    filename);
		return (WT_ERROR);
	}

	/* Open the underlying file handle. */
	WT_RET(__wt_open(session, filename, 0666, 1, &fh));

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
__wt_block_open(WT_SESSION_IMPL *session,
    const char *filename, const char *config, int salvage, void *retp)
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
	WT_ERR(__wt_open(session, filename, 0666, 1, &block->fh));

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
		if (block->compressor == NULL) {
			__wt_errx(session, "unknown block_compressor '%.*s'",
			    (int)cval.len, cval.str);
			WT_ERR(EINVAL);
		}
	}

	/*
	 * Normally we read the file's meta-data to see if this is a WiredTiger
	 * file.  But, if it's a salvage operation and force is set, we ignore
	 * the file's format entirely.
	 */
	cval.val = 0;
	if (salvage) {
		ret = __wt_config_getones(session, config, "force", &cval);
		if (ret != 0 && ret != WT_NOTFOUND)
			WT_RET(ret);
	}
	if (cval.val == 0)
		WT_ERR(__desc_read(session, block, salvage));

	/* If not an open for a salvage operation, read the freelist. */
	if (!salvage)
		WT_ERR(__wt_block_freelist_read(session, block));

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

	if (block->fh != NULL) {	/* write out the free-list */
		WT_TRET(__wt_block_freelist_write(session, block));
					/* update the file's description */
		WT_TRET(__desc_update(session, block));
		WT_RET(__wt_close(session, block->fh));
	}

	if (block->name != NULL)
		__wt_free(session, block->name);

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
	WT_BTREE_DESC *desc;
	uint32_t cksum;
	uint8_t buf[WT_BTREE_DESC_SECTOR];
	const char *msg;

	/*
	 * We currently always do the verification step, because it's cheap
	 * and we only do it the first time a file is opened.
	 *
	 * Read the first sector.
	 */
	WT_RET(__wt_read(session, block->fh, (off_t)0, sizeof(buf), buf));
	desc = (WT_BTREE_DESC *)buf;

	WT_VERBOSE(session, block,
	    "%s description: magic %" PRIu32
	    ", major/minor: %" PRIu32 "/%" PRIu32
	    ", checksum %#" PRIx32
	    ", free addr/size %" PRIu32 "/%" PRIu32
	    ", lsn %" PRIu64,
	    block->name, desc->magic,
	    desc->majorv, desc->minorv,
	    desc->cksum,
	    desc->free_addr, desc->free_size,
	    desc->lsn);

	cksum = desc->cksum;
	desc->cksum = 0;
	if (cksum != __wt_cksum(buf, sizeof(buf))) {
		msg = "checksum mismatch: file does not appear to be a "
		    "WiredTiger Btree file";
		goto err;
	}

	/*
	 * We fail the open if the checksum fails, or the magic number is wrong
	 * or the major/minor numbers are unsupported for this version.  This
	 * test is done even if the caller is verifying or salvaging the file:
	 * it makes sense for verify, and for salvage we don't overwrite files
	 * without some reason to believe they are WiredTiger files.  The user
	 * may have entered the wrong file name, and is now frantically pounding
	 * their interrupt key.
	 */
	if (desc->magic != WT_BTREE_MAGIC) {
		msg = "magic number mismatch: file does not appear to be a "
		    "WiredTiger Btree file";
		goto err;
	}
	if (desc->majorv > WT_BTREE_MAJOR_VERSION ||
	    (desc->majorv == WT_BTREE_MAJOR_VERSION &&
	    desc->minorv > WT_BTREE_MINOR_VERSION)) {
		msg = "file is an unsupported version of a WiredTiger Btree "
		    "file";
err:		__wt_errx(session, "%s%s", msg,
		    salvage ? "; to salvage this file, configure the salvage "
		    "operation with the force flag" : "");
		return (WT_ERROR);
	}

	block->lsn = desc->lsn;

	/* That's all we check for salvage. */
	if (salvage)
		return (0);

	if ((desc->free_addr != WT_ADDR_INVALID &&
	    WT_ADDR_TO_OFF(block, desc->free_addr) +
	    (off_t)desc->free_size > block->fh->file_size)) {
		__wt_errx(session,
		    "free address references non-existent pages");
		return (WT_ERROR);
	}
	block->free_addr = desc->free_addr;
	block->free_size = desc->free_size;
	block->free_cksum = desc->free_cksum;

	return (0);
}

/*
 * __wt_desc_init --
 *	Write an initial file's descriptor structure.
 */
int
__wt_desc_init(WT_SESSION_IMPL *session, WT_FH *fh)
{
	WT_BTREE_DESC *desc;
	uint8_t buf[WT_BTREE_DESC_SECTOR];

	memset(buf, 0, sizeof(buf));
	desc = (WT_BTREE_DESC *)buf;

	desc->magic = WT_BTREE_MAGIC;
	desc->majorv = WT_BTREE_MAJOR_VERSION;
	desc->minorv = WT_BTREE_MINOR_VERSION;

	desc->free_addr = WT_ADDR_INVALID;
	desc->free_size = 0;

	/* Update the checksum. */
	desc->cksum = 0;
	desc->cksum = __wt_cksum(buf, sizeof(buf));

	return (__wt_write(session, fh, (off_t)0, sizeof(buf), buf));
}

/*
 * __desc_update --
 *	Update the file's descriptor structure.
 */
static int
__desc_update(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
	WT_BTREE_DESC *desc;
	uint8_t buf[WT_BTREE_DESC_SECTOR];

	/* Read the first sector. */
	WT_RET(__wt_read(session, block->fh, (off_t)0, sizeof(buf), buf));
	desc = (WT_BTREE_DESC *)buf;

	/* See if anything has changed. */
	if (desc->free_addr == block->free_addr &&
	    desc->free_size == block->free_size &&
	    desc->lsn == block->lsn)
		return (0);

	WT_VERBOSE(session, block,
	    "updating free list [%" PRIu32 "-%" PRIu32 ", %" PRIu32 "]",
	    block->free_addr,
	    block->free_addr + (block->free_size / 512 - 1), block->free_size);

	desc->lsn = block->lsn;
	desc->free_addr = block->free_addr;
	desc->free_size = block->free_size;

	/* Update the checksum. */
	desc->cksum = 0;
	desc->cksum = __wt_cksum(buf, sizeof(buf));

	return (__wt_write(session, block->fh, (off_t)0, sizeof(buf), buf));
}
