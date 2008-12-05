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
	DB *db;
	IDB *idb;
	IENV *ienv;
	off_t size;
	int ret;

	db = bt->db;
	ienv = db->ienv;
	idb = db->idb;

	/* Try and open the fle. */
	if ((ret = __wt_open(ienv, idb->file_name, idb->mode,
	    F_ISSET(idb, WT_CREATE) ? WT_OPEN_CREATE : 0, &bt->fh)) != 0)
		return (ret);

	if ((ret = __wt_filesize(ienv, bt->fh, &size)) != 0)
		goto err;

	/*
	 * Convert the size in bytes to "fragments".  If part of the write
	 * of a fragment failed, pretend it all failed, and truncate the
	 * file.
	 */
	bt->frags = size / db->fragsize;

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
__wt_bt_falloc(WT_BTREE *bt, u_int32_t frags, void *retp, u_int32_t *addrp)
{
	DB *db;
	IENV *ienv;
	int ret;
	void *p;

	db = bt->db;
	ienv = db->ienv;

	if (UINT32_MAX - bt->frags < frags) {
		__wt_db_errx(db,
		    "Requested additional space is not available; the file"
		    " cannot grow that much");
		return (WT_ERROR);
	}

	/*
	 * Allocate the memory to hold it -- clear the memory, as code
	 * depends on values in the page being zero.
	 */
	if ((ret = __wt_calloc(ienv, 1, WT_FRAGS_TO_BYTES(db, frags), &p)) != 0)
		return (WT_ERROR);
	*(void **)retp = p;

	*addrp = bt->frags;
	bt->frags += frags;

	return (0);
}

/*
 * __wt_bt_fread --
 *	Read a chunk of a file.
 */
int
__wt_bt_fread(
    WT_BTREE *bt, u_int32_t addr, u_int32_t frags, WT_PAGE_HDR **hdrp)
{
	DB *db;
	IENV *ienv;
	WT_PAGE_HDR *hdr;
	size_t bytes;
	u_int32_t checksum;
	int ret;

	db = bt->db;
	ienv = db->ienv;

	/* Allocate the memory to hold it. */
	bytes = WT_FRAGS_TO_BYTES(db, frags);
	if ((ret = __wt_malloc(ienv, bytes, &hdr)) != 0)
		return (ret);

	/* Read the page. */
	if ((ret = __wt_read(ienv,
	    bt->fh, (off_t)WT_FRAGS_TO_BYTES(db, addr), bytes, hdr)) != 0)
		goto err;

	/* Verify the checksum. */
	checksum = hdr->checksum;
	hdr->checksum = 0;
	if (checksum != __wt_cksum(hdr, bytes)) {
		__wt_db_errx(db,
		    "Block %lu was read and had a checksum error",
		    (u_long)bytes);
		goto err;
	}

#ifdef HAVE_DIAGNOSTIC
	/* Verify the page. */
	if ((ret = __wt_bt_page_verify(db, addr, hdr)) != 0)
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
    WT_BTREE *bt, u_int32_t addr, u_int32_t frags, WT_PAGE_HDR *hdr)
{
	DB *db;
	IENV *ienv;
	size_t bytes;
	int ret;

	db = bt->db;
	ienv = db->ienv;

#ifdef HAVE_DIAGNOSTIC
	/* Verify the page. */
	if ((ret = __wt_bt_page_verify(db, addr, hdr)) != 0)
		return (ret);
#endif

	bytes = WT_FRAGS_TO_BYTES(db, frags);

	/* Update the checksum. */
	hdr->checksum = 0;
	hdr->checksum = __wt_cksum(hdr, bytes);

	/* Write the page. */
	return (__wt_write(
	    ienv, bt->fh, (off_t)WT_FRAGS_TO_BYTES(db, addr), bytes, hdr));
}

/*
 * __wt_bt_fdiscard --
 *	Discard a page of a file.
 */
int
__wt_bt_fdiscard(WT_BTREE *bt, u_int32_t addr, WT_PAGE_HDR *hdr)
{
	DB *db;
	IENV *ienv;
	int ret;

	db = bt->db;
	ienv = db->ienv;

#ifdef HAVE_DIAGNOSTIC
	/* Verify the page. */
	if ((ret = __wt_bt_page_verify(db, addr, hdr)) != 0)
		return (ret);
#endif

	__wt_free(ienv, hdr);
	return (0);
}
