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
__wt_block_open(WT_SESSION_IMPL *session,
    const char *filename, const char *config, const char *cfg[], void *retp)
{
	WT_BLOCK *block;
	WT_CONFIG_ITEM cval;
	WT_CONNECTION_IMPL *conn;
	WT_NAMED_COMPRESSOR *ncomp;
	int ret;

	*(void **)retp = NULL;
	conn = S2C(session);

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

	/* Open the underlying file handle. */
	WT_ERR(__wt_open(session, filename, 0, 0, 1, &block->fh));

	/* Initialize the live snapshot lock. */
	__wt_spin_init(session, &block->live_lock);

	/*
	 * Read the description sector.
	 *
	 * Salvage is a special case -- if we're forcing the salvage, we don't
	 * even look at the description sector.
	 *
	 * XXX
	 * We shouldn't be looking at the WT_BTREE->flags field here.
	 */
	cval.val = 0;
	if (F_ISSET(session->btree, WT_BTREE_SALVAGE)) {
		ret = __wt_config_gets(session, cfg, "force", &cval);
		if (ret != 0 && ret != WT_NOTFOUND)
			WT_ERR(ret);
	}
	if (cval.val == 0)
		WT_ERR(__desc_read(session, block));

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

	WT_VERBOSE(session, block, "close");

	if (block->live_load) {
		__wt_errx(session, "snapshot never unloaded");
		ret = EINVAL;
	}

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
	uint8_t buf[WT_BLOCK_DESC_SECTOR];

	memset(buf, 0, sizeof(buf));
	desc = (WT_BLOCK_DESC *)buf;
	desc->magic = WT_BLOCK_MAGIC;
	desc->majorv = WT_BLOCK_MAJOR_VERSION;
	desc->minorv = WT_BLOCK_MINOR_VERSION;

	/* Update the checksum. */
	desc->cksum = 0;
	desc->cksum = __wt_cksum(desc, WT_BLOCK_DESC_SECTOR);

	return (__wt_write(session, fh, (off_t)0, WT_BLOCK_DESC_SECTOR, desc));
}

/*
 * __desc_read --
 *	Read and verify the file's metadata.
 */
static int
__desc_read(WT_SESSION_IMPL *session, WT_BLOCK *block)
{
	WT_BLOCK_DESC *desc;
	uint32_t cksum;
	uint8_t buf[WT_BLOCK_DESC_SECTOR];

	/* Read the first sector and verify the file's format. */
	memset(buf, 0, sizeof(buf));
	WT_RET(__wt_read(
	    session, block->fh, (off_t)0, WT_BLOCK_DESC_SECTOR, buf));

	desc = (WT_BLOCK_DESC *)buf;
	WT_VERBOSE(session, block,
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
		WT_RET_MSG(session, WT_ERROR,
		    "%s does not appear to be a WiredTiger file", block->name);

	if (desc->majorv > WT_BLOCK_MAJOR_VERSION ||
	    (desc->majorv == WT_BLOCK_MAJOR_VERSION &&
	    desc->minorv > WT_BLOCK_MINOR_VERSION))
		WT_RET_MSG(session, WT_ERROR,
		    "%s is an unsupported version of a WiredTiger file",
		    block->name);

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
