/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int __wt_db_page_clean(DB *);
static int __wt_db_page_discard(DB *, WT_PAGE *);
static int __wt_db_page_write(DB *, WT_PAGE *);

/*
 * __wt_db_page_open --
 *	Open an underlying file.
 */
int
__wt_db_page_open(IDB *idb)
{
	DB *db;
	IENV *ienv;
	off_t size;
	int ret;

	db = idb->db;
	ienv = db->ienv;

	/* Try and open the fle. */
	if ((ret = __wt_open(ienv, idb->file_name, idb->mode,
	    F_ISSET(idb, WT_CREATE) ? WT_OPEN_CREATE : 0, &idb->fh)) != 0)
		return (ret);

	if ((ret = __wt_filesize(ienv, idb->fh, &size)) != 0)
		goto err;

	/*
	 * Convert the size in bytes to "fragments".  If part of the write
	 * of a fragment failed, pretend it all failed, and truncate the
	 * file.
	 */
	idb->frags = (u_int32_t)(size / db->fragsize);

	return (0);

err:	(void)__wt_close(ienv, idb->fh);
	return (ret);
}

/*
 * __wt_db_page_sync --
 *	Flush an underlying file to disk.
 */
int
__wt_db_page_sync(DB *db)
{
	IDB *idb;
	WT_PAGE *page;
	int ret;

	idb = db->idb;

	TAILQ_FOREACH(page, &idb->hlru, lq)
		if (F_ISSET(page, WT_MODIFIED) &&
		    (ret = __wt_db_page_write(db, page)) != 0)
			return (ret);
	return (0);
}

/*
 * __wt_db_page_close --
 *	Close an underlying file.
 */
int
__wt_db_page_close(DB *db)
{
	IENV *ienv;
	IDB *idb;
	WT_PAGE *page;
	int ret, tret;

	ienv = db->ienv;
	idb = db->idb;
	ret = 0;

	while ((page = TAILQ_FIRST(&idb->hlru)) != NULL) {
		if (F_ISSET(page, WT_MODIFIED) &&
		    (tret = __wt_db_page_write(db, page)) != 0 &&
		    ret == 0)
			ret = tret;
		if ((tret = __wt_db_page_discard(db, page)) != 0 && ret == 0)
			ret = tret;
	}

	WT_ASSERT(ienv, idb->cache_frags== 0);

	return (__wt_close(ienv, idb->fh));
}

/*
 * __wt_db_page_alloc --
 *	Allocate fragments from a file.
 */
int
__wt_db_page_alloc(DB *db, u_int32_t frags, WT_PAGE **pagep)
{
	IDB *idb;
	IENV *ienv;
	WT_PAGE *page;
	int ret;

	ienv = db->ienv;
	idb = db->idb;

	*pagep = NULL;

	/* Check for an inability to grow the file. */
	if (UINT32_MAX - idb->frags < frags) {
		__wt_db_errx(db,
		    "Requested additional space is not available; the file"
		    " cannot grow that much");
		return (WT_ERROR);
	}

	/* Check for exceeding the size of the cache. */
	while (idb->cache_frags > idb->cache_frags_max)
		if ((ret = __wt_db_page_clean(db)) != 0)
			return (ret);

	/*
	 * Allocate memory for the in-memory page information.  It's a separate
	 * allocation call from the page because we hope to interact gracefully
	 * with the underlying heap memory allocator.
	 */
	if ((ret = __wt_calloc(ienv, 1, sizeof(WT_PAGE), &page)) != 0)
		return (ret);

	/*
	 * Allocate the memory to hold the page -- clear the memory, as code
	 * depends on values in the page being zero.
	 */
	if ((ret = __wt_calloc(
	    ienv, 1, (size_t)WT_FRAGS_TO_BYTES(db, frags), &page->hdr)) != 0)
		goto err;

	page->addr = idb->frags;
	idb->frags += frags;
	page->frags = frags;

	/* Initialize the rest of the in-memory page structure. */
	__wt_page_inmem(db, page, 1);

	TAILQ_INSERT_HEAD(&idb->hhq[WT_HASH(page->addr)], page, hq);
	TAILQ_INSERT_TAIL(&idb->hlru, page, lq);

	idb->cache_frags += frags;

	WT_STAT_INCR(db, CACHE_ALLOC, "pages allocated in the cache");

	*pagep = page;
	return (0);

err:	if (page->hdr != NULL)
		__wt_free(ienv, page->hdr);
	__wt_free(ienv, page);
	return (ret);
}

/*
 * __wt_db_page_in --
 *	Pin a fragment of a file, reading as necessary.
 */
int
__wt_db_page_in(DB *db,
    u_int32_t addr, u_int32_t frags, WT_PAGE **pagep, u_int32_t flags)
{
	IENV *ienv;
	IDB *idb;
	WT_PAGE_HQH *hashq;
	WT_PAGE *page;
	WT_PAGE_HDR *hdr;
	size_t bytes;
	int ret;

	DB_FLAG_CHK(db, "__wt_db_page_in", flags, WT_APIMASK_DB_FILE_READ);

	ienv = db->ienv;
	idb = db->idb;

	*pagep = NULL;

	/* Check for the page in the cache. */
	hashq = &idb->hhq[WT_HASH(addr)];
	TAILQ_FOREACH(page, hashq, hq)
		if (page->addr == addr)
			break;
	if (page != NULL) {
		TAILQ_REMOVE(hashq, page, hq);
		TAILQ_REMOVE(&idb->hlru, page, lq);
		TAILQ_INSERT_HEAD(hashq, page, hq);
		TAILQ_INSERT_TAIL(&idb->hlru, page, lq);
		WT_STAT_INCR(db, CACHE_HIT, "reads found in the cache");

		*pagep = page;
		return (0);
	}

	WT_STAT_INCR(db, CACHE_MISS, "reads not found in the cache");

	/* Check for exceeding the size of the cache. */
	while (idb->cache_frags > idb->cache_frags_max)
		if ((ret = __wt_db_page_clean(db)) != 0)
			return (ret);

	/* Allocate the memory to hold a new page. */
	bytes = (size_t)WT_FRAGS_TO_BYTES(db, frags);
	if ((ret = __wt_malloc(ienv, bytes, &hdr)) != 0)
		return (ret);

	/* Read the page. */
	if ((ret = __wt_read(ienv,
	    idb->fh, WT_FRAGS_TO_BYTES(db, addr), bytes, hdr)) != 0)
		goto err;

	/* Verify the checksum. */
	if (!LF_ISSET(WT_NO_CHECKSUM)) {
		u_int32_t checksum = hdr->checksum;
		hdr->checksum = 0;
		if (checksum != __wt_cksum(hdr, bytes)) {
			__wt_db_errx(db,
			    "fragment %lu was read and had a checksum error",
			    (u_long)addr);
			ret = WT_ERROR;
			goto err;
		}
	}

	/* Allocate an in-memory page structure. */
	if ((ret = __wt_calloc(ienv, 1, sizeof(WT_PAGE), &page)) != 0)
		goto err;

	page->addr = addr;
	page->frags = frags;
	page->hdr = hdr;
	__wt_page_inmem(db, page, 0);

	/* Insert at the head of the hash queue and the tail of LRU queue. */
	TAILQ_INSERT_HEAD(hashq, page, hq);
	TAILQ_INSERT_TAIL(&idb->hlru, page, lq);

	WT_ASSERT(ienv, __wt_db_verify_page(db, page, NULL) == 0);

	idb->cache_frags += frags;

	*pagep = page;

	return (0);

err:	if (page != NULL)
		__wt_free(ienv, page);
	__wt_free(ienv, hdr);
	return (ret);
}

/*
 * __wt_db_page_out --
 *	Unpin a fragment of a file, writing as necessary.
 */
int
__wt_db_page_out(DB *db, WT_PAGE *page, u_int32_t flags)
{
	IDB *idb;
	WT_PAGE_HQH *hashq;

	idb = db->idb;

	DB_FLAG_CHK(db, "__wt_db_page_out", flags, WT_APIMASK_DB_FILE_WRITE);

	/*
	 * Re-insert the page at the head of the hash queue and at the tail of
	 * the LRU queue.
	 */
	hashq = &idb->hhq[WT_HASH(page->addr)];
	TAILQ_REMOVE(hashq, page, hq);
	TAILQ_INSERT_HEAD(hashq, page, hq);
	TAILQ_REMOVE(&idb->hlru, page, lq);
	TAILQ_INSERT_TAIL(&idb->hlru, page, lq);

	/* If the page is dirty, set the modified flag. */
	if (LF_ISSET(WT_MODIFIED))
		F_SET(page, WT_MODIFIED);
		WT_STAT_INCR(db, CACHE_DIRTY, "dirty pages in the cache");

	return (0);
}

/*
 * __wt_db_page_clean --
 *	Clear some space out of the cache.
 */
static int
__wt_db_page_clean(DB *db)
{
	IDB *idb;
	WT_PAGE *page;
	int ret;

	idb = db->idb;

	TAILQ_FOREACH(page, &idb->hlru, lq) {
		if (F_ISSET(page, WT_MODIFIED)) {
			WT_STAT_INCR(db, CACHE_WRITE_EVICT,
			    "dirty pages evicted from the cache");
			if ((ret = __wt_db_page_write(db, page)) != 0)
				return (ret);
		} else
			WT_STAT_INCR(db, CACHE_EVICT,
			    "clean pages evicted from the cache");
		return (__wt_db_page_discard(db, page));
	}
	return (0);
}

/*
 * __wt_db_page_write --
 *	Write a page to the backing file.
 */
static int
__wt_db_page_write(DB *db, WT_PAGE *page)
{
	IDB *idb;
	IENV *ienv;
	WT_PAGE_HDR *hdr;
	size_t bytes;

	ienv = db->ienv;
	idb = db->idb;

	WT_STAT_INCR(db, CACHE_WRITE, "writes from the cache");
	WT_STAT_DECR(db, CACHE_DIRTY, NULL);

	/* Clear the modified flag. */
	F_CLR(page, WT_MODIFIED);

	/* Verify the page. */
	WT_ASSERT(ienv, __wt_db_verify_page(db, page, NULL) == 0);

	/* Update the checksum. */
	bytes = (size_t)WT_FRAGS_TO_BYTES(db, page->frags);

	hdr = page->hdr;
	hdr->checksum = 0;
	hdr->checksum = __wt_cksum(hdr, bytes);

	return (__wt_write(ienv, idb->fh,
	    (off_t)WT_FRAGS_TO_BYTES(db, page->addr), bytes, hdr));
}

/*
 * __wt_db_page_discard --
 *	Discard a page of a file.
 */
static int
__wt_db_page_discard(DB *db, WT_PAGE *page)
{
	IENV *ienv;
	IDB *idb;

	ienv = db->ienv;
	idb = db->idb;

	WT_ASSERT(ienv, __wt_db_verify_page(db, page, NULL) == 0);

	if (idb->cache_frags < page->frags) {
		__wt_db_errx(db, "allocated cache size went negative");
		return (WT_ERROR);
	}
	idb->cache_frags -= page->frags;

	TAILQ_REMOVE(&idb->hhq[WT_HASH(page->addr)], page, hq);
	TAILQ_REMOVE(&idb->hlru, page, lq);

	__wt_free(ienv, page->hdr);
	__wt_free(ienv, page);
	return (0);
}
