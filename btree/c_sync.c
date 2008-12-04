/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

/*
 * __wt_bt_fopen --
 *	Open an underlying file.
 */
int
__wt_bt_fopen(WT_BTREE *bt)
{
	IENV *ienv;
	IDB *idb;
	int ret;

	ienv = bt->db->ienv;
	idb = bt->db->idb;

	/* Try and open the fle. */
	if ((ret = __wt_open(ienv, idb->file_name, idb->mode,
	    F_ISSET(idb, WT_CREATE) ? WT_OPEN_CREATE : 0, &bt->fh)) != 0)
		return (ret);

	if ((ret = __wt_filesize(ienv, bt->fh, &bt->blocks)) != 0)
		goto err;

	return (0);

err:	(void)__wt_close(ienv, bt->fh);
	return (ret);
}

/*
 * __wt_bt_fclose --
 *	Close an underlying file.
 */
int
__wt_bt_fclose(WT_BTREE *bt)
{
	IENV *ienv;
	int ret;

	ienv = bt->db->ienv;

	return (__wt_close(ienv, bt->fh));
}

/*
 * __wt_bt_falloc --
 *	Allocate a chunk of a file.
 */
int
__wt_bt_falloc(WT_BTREE *bt, u_int32_t blocks, void *retp, u_int32_t *blockp)
{
	DB *db;
	IENV *ienv;
	int ret;
	void *p;

	db = bt->db;
	ienv = db->ienv;

	if (UINT32_MAX - bt->blocks < blocks) {
		__wt_db_errx(db,
		    "An additional %lu blocks are not available, the file"
		    " cannot grow that much larger",
		    (u_long)blocks);
		return (WT_ERROR);
	}

	/*
	 * Allocate the memory to hold it -- clear the memory, as code
	 * depends on values in the page being zero.
	 */
	if ((ret = __wt_calloc(ienv, 1, WT_BLOCKS_TO_BYTES(blocks), &p)) != 0)
		return (WT_ERROR);
	*(void **)retp = p;

	*blockp = bt->blocks;
	bt->blocks += blocks;

	return (0);
}

/*
 * __wt_bt_fread --
 *	Read a chunk of a file.
 */
int
__wt_bt_fread(
    WT_BTREE *bt, u_int32_t block, u_int32_t blocks, WT_PAGE_HDR **hdrp)
{
	DB *db;
	IENV *ienv;
	WT_PAGE_HDR *hdr;
	u_int32_t checksum;
	int ret;

	db = bt->db;
	ienv = db->ienv;

	/* Allocate the memory to hold it. */
	if ((ret = __wt_malloc(ienv, WT_BLOCKS_TO_BYTES(blocks), &hdr)) != 0)
		return (ret);

	/* Read the page. */
	if ((ret = __wt_read(ienv, bt->fh, block, blocks, hdr)) != 0)
		goto err;

	/* Verify the checksum. */
	checksum = hdr->checksum;
	hdr->checksum = 0;
	if (checksum != __wt_cksum(hdr, blocks)) {
		__wt_db_errx(db,
		    "Block %lu was read and had a checksum error",
		    (u_long)blocks);
		goto err;
	}

#ifdef HAVE_DIAGNOSTIC
	/* Verify the page. */
	if ((ret = __wt_bt_page_verify(db, block, hdr)) != 0)
		goto err;
#endif

	*hdrp = hdr;
	return (0);

err:	__wt_free(ienv, hdr);
	return (ret);
}

/*
 * __wt_bt_fwrite --
 *	Write a chunk of a file.
 */
int
__wt_bt_fwrite(
    WT_BTREE *bt, u_int32_t block, u_int32_t blocks, WT_PAGE_HDR *hdr)
{
	DB *db;
	IENV *ienv;
	int ret;

	db = bt->db;
	ienv = db->ienv;

#ifdef HAVE_DIAGNOSTIC
	/* Verify the page. */
	if ((ret = __wt_bt_page_verify(db, block, hdr)) != 0)
		return (ret);
#endif

	/* Update the checksum. */
	hdr->checksum = 0;
	hdr->checksum = __wt_cksum(hdr, blocks);

	/* Write the page. */
	return (__wt_write(ienv, bt->fh, block, blocks, hdr));
}

/*
 * __wt_bt_fdiscard --
 *	Discard a page of a file.
 */
int
__wt_bt_fdiscard(WT_BTREE *bt, u_int32_t block, WT_PAGE_HDR *hdr)
{
	DB *db;
	IENV *ienv;
	int ret;

	db = bt->db;
	ienv = db->ienv;

#ifdef HAVE_DIAGNOSTIC
	/* Verify the page. */
	if ((ret = __wt_bt_page_verify(db, block, hdr)) != 0)
		return (ret);
#endif

	__wt_free(ienv, hdr);
	return (0);
}
