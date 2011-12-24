/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static int  __desc_read(WT_SESSION_IMPL *, int);
static int  __desc_update(WT_SESSION_IMPL *);

/*
 * __wt_bm_create --
 *	Create a new block manager file.
 */
int
__wt_bm_create(WT_SESSION_IMPL *session, const char *filename)
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
	ret = __wt_desc_write(session, fh);

	/* Close the file handle. */
	WT_TRET(__wt_close(session, fh));

	return (ret);
}

/*
 * __wt_bm_open --
 *	Open a block manager file.
 */
int
__wt_bm_open(WT_SESSION_IMPL *session)
{
	WT_BTREE *btree;
	WT_CONFIG_ITEM cval;
	int ret;

	btree = session->btree;

	__wt_block_freelist_init(session);

	/* Open the underlying file handle. */
	WT_RET(__wt_open(session, btree->filename, 0666, 1, &btree->fh));

	/*
	 * Normally we read the file's meta-data to see if this is a WiredTiger
	 * file.  But, if it's a salvage operation and force is set, we ignore
	 * the file's format entirely.
	 */
	cval.val = 0;
	if (F_ISSET(btree, WT_BTREE_SALVAGE)) {
		ret = __wt_config_getones(
		    session, btree->config, "force", &cval);
		if (ret != 0 && ret != WT_NOTFOUND)
			WT_RET(ret);
	}
	if (cval.val == 0)
		WT_ERR(__desc_read(
		    session, F_ISSET(btree, WT_BTREE_SALVAGE) ? 1 : 0));

	/* If this is an open for a salvage operation, that's all we do. */
	if (F_ISSET(btree, WT_BTREE_SALVAGE))
		return (0);

	/* Read the free list. */
	WT_ERR(__wt_block_freelist_read(session));

	return (0);

err:	(void)__wt_bm_close(session);
	return (ret);
}

/*
 * __wt_bm_close --
 *	Close a block manager file.
 */
int
__wt_bm_close(WT_SESSION_IMPL *session)
{
	WT_BTREE *btree;
	int ret;

	btree = session->btree;
	ret = 0;

	/*
	 * Write out the free list.
	 * Update the file's description.
	 */
	WT_TRET(__wt_block_freelist_write(session));

	if (btree->fh != NULL) {
		WT_TRET(__desc_update(session));
		WT_RET(__wt_close(session, btree->fh));
		btree->fh = NULL;
	}
	return (ret);
}

/*
 * __desc_read --
 *	Read and verify the file's metadata.
 */
static int
__desc_read(WT_SESSION_IMPL *session, int salvage)
{
	WT_BTREE *btree;
	WT_BTREE_DESC *desc;
	WT_CONFIG_ITEM cval;
	uint32_t cksum;
	uint8_t buf[WT_BTREE_DESC_SECTOR];
	const char *msg;

	/*
	 * We currently always do the verification step, because it's cheap
	 * and we only do it the first time a file is opened.
	 */
	btree = session->btree;

	/* Get the allocation size, we need it to validate the addresses. */
	WT_RET(__wt_config_getones(
	    session, btree->config, "allocation_size", &cval));
	btree->allocsize = (uint32_t)cval.val;

	/* Read the first sector. */
	WT_RET(__wt_read(session, btree->fh, (off_t)0, sizeof(buf), buf));
	desc = (WT_BTREE_DESC *)buf;

	WT_VERBOSE(session, block,
	    "%s description: magic %" PRIu32
	    ", major/minor: %" PRIu32 "/%" PRIu32
	    ", checksum %#" PRIx32
	    ", free addr/size %" PRIu32 "/%" PRIu32
	    ", lsn %" PRIu64,
	    btree->name, desc->magic,
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

	/* That's all we check for salvage. */
	if (salvage)
		return (0);

	if ((desc->free_addr != WT_ADDR_INVALID &&
	    WT_ADDR_TO_OFF(btree, desc->free_addr) +
	    (off_t)desc->free_size > btree->fh->file_size)) {
		__wt_errx(session,
		    "free address references non-existent pages");
		return (WT_ERROR);
	}
	btree->free_addr = desc->free_addr;
	btree->free_size = desc->free_size;
	btree->free_cksum = desc->free_cksum;

	btree->lsn = desc->lsn;			/* XXX */

	return (0);
}

/*
 * __wt_desc_write --
 *	Write the file's descriptor structure.
 */
int
__wt_desc_write(WT_SESSION_IMPL *session, WT_FH *fh)
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
__desc_update(WT_SESSION_IMPL *session)
{
	WT_BTREE *btree;
	WT_BTREE_DESC *desc;
	uint8_t buf[WT_BTREE_DESC_SECTOR];

	btree = session->btree;

	/* Read the first sector. */
	WT_RET(__wt_read(session, btree->fh, (off_t)0, sizeof(buf), buf));
	desc = (WT_BTREE_DESC *)buf;

	/* See if anything has changed. */
	if (desc->free_addr == btree->free_addr &&
	    desc->free_size == btree->free_size &&
	    desc->lsn == btree->lsn)
		return (0);

	WT_VERBOSE(session, block,
	    "updating free list [%" PRIu32 "-%" PRIu32 ", %" PRIu32 "]",
	    btree->free_addr,
	    btree->free_addr + (btree->free_size / 512 - 1), btree->free_size);

	desc->free_addr = btree->free_addr;
	desc->free_size = btree->free_size;
	desc->lsn = btree->lsn;				/* XXX */

	/* Update the checksum. */
	desc->cksum = 0;
	desc->cksum = __wt_cksum(buf, sizeof(buf));

	return (__wt_write(session, btree->fh, (off_t)0, sizeof(buf), buf));
}
