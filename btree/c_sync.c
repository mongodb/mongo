/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2010 WiredTiger, Inc.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int __wt_cache_sync_drain(WT_TOC *, void (*)(const char *, uint64_t));

/*
 * __wt_cache_sync --
 *	Flush a database's underlying cache to disk.
 */
int
__wt_cache_sync(
    WT_TOC *toc, void (*f)(const char *, uint64_t), uint32_t flags)
{
	ENV *env;
	IDB *idb;
	int ret;

	env = toc->env;
	idb = toc->db->idb;
	ret = 0;

	/* Write dirty pages from the cache. */
	WT_RET(__wt_cache_sync_drain(toc, f));

	/* Optionally flush the file to the backing disk. */
	if (!LF_ISSET(WT_OSWRITE))
		WT_TRET(__wt_fsync(env, idb->fh));

	return (ret);
}

typedef struct __wt_sync_list {
	uint32_t addr;				/* Address */
	uint8_t  level;			/* Level */
} WT_SYNC_LIST;

/*
 * __wt_sync_compare_level --
 *	Qsort function: sort WT_SYNC_LIST list based on the page's level.
 */
static int
__wt_sync_compare_level(const void *a, const void *b)
{
	u_int a_level, b_level;

	a_level = ((WT_SYNC_LIST *)a)->level;
	b_level = ((WT_SYNC_LIST *)b)->level;

	/*
	 * Sort in descending order, the bigger a page's level, the sooner
	 * we want to write it.
	 */
	return (a_level > b_level ? 1 : (a_level < b_level ? -1 : 0));
}

/*
 * __wt_cache_sync_drain --
 *	Flush all modified pages for the caller's DB handle.
 */
static int
__wt_cache_sync_drain(WT_TOC *toc, void (*f)(const char *, uint64_t))
{
	ENV *env;
	DB *db;
	WT_CACHE *cache;
	WT_CACHE_ENTRY *e;
	WT_PAGE *page;
	WT_SYNC_LIST *drain, *drain_base;
	uint64_t fcnt;
	uint32_t drain_elem, drain_len, i, j;
	int ret;

	env = toc->env;
	db = toc->db;
	cache = env->ienv->cache;
	fcnt = 0;
	ret = 0;

	/* Single-thread reconciliation. */
	__wt_lock(env, cache->mtx_reconcile);

	drain_base = NULL;
	drain_elem = drain_len = 0;
	for (i = 0; i < cache->hb_size; ++i)
		for (j = WT_CACHE_ENTRY_CHUNK, e = cache->hb[i];;) {
			/*
			 * Skip pages we either don't care about or which are
			 * in play in some way.
			 */
			if (e->db != db || e->state != WT_OK)
				goto loop;

			/*
			 * Sync is just another reader of the cache, and must
			 * obey the reader protocol.  Get a hazard reference:
			 * if the page isn't available for any reason, forget
			 * about it.
			 */
			if (!__wt_hazard_set(toc, e, NULL))
				goto loop;

			/* Ignore clean pages. */
			page = e->page;
			if (!WT_PAGE_MODIFY_ISSET(page))
				goto next;

			/*
			 * Leaf pages are reconciled immediately -- we could
			 * wait, but no reason not to just do it now (plus,
			 * it gives the OS a start on lazily writing leaf pages
			 * to disk, in the face of a future fsync).
			 */
			if (page->hdr->level == WT_LLEAF) {
				if (f != NULL && ++fcnt % 10 == 0)
					f(toc->name, fcnt);
				(void)__wt_bt_rec_page(toc, page);
				goto next;
			}

			/*
			 * Allocate more space as necessary, and remember this
			 * page.
			 */
			if (drain_elem * sizeof(drain_base[0]) >= drain_len) {
				WT_ERR(__wt_realloc(env, &drain_len,
				    (drain_elem + 100) * sizeof(drain_base[0]),
				    &drain_base));
				drain = drain_base + drain_elem;
			}
			drain->addr = page->addr;
			drain->level = page->hdr->level;
			++drain;
			++drain_elem;

next:			__wt_hazard_clear(toc, page);
loop:			WT_CACHE_ENTRY_NEXT(e, j);
	}

	/*
	 * If there aren't any non-leaf pages to write, we're surprised, but
	 * done.
	 */
	if (drain_elem == 0)
		goto done;

	/*
	 * Sort the pages for writing -- we write them in level order so that
	 * reconciliation updates naturally flow up the tree, and we won't
	 * check a page for writing until any pages that could have modified
	 * it have already been written.
	 */
	qsort(drain_base,
	    (size_t)drain_elem, sizeof(drain_base[0]), __wt_sync_compare_level);

	/* Reconcile the pages. */
	for (drain = drain_base; drain_elem > 0; ++drain, --drain_elem) {
		if (f != NULL && ++fcnt % 10 == 0)
			f(toc->name, fcnt);

		/*
		 * If we find the page in the cache, check to see if it needs
		 * to be flushed; if we don't find it in the cache, it must
		 * have been reconciled and written by some other thread.
		 */
		switch (ret = __wt_page_in(
		    toc, drain->addr, 0, &page, WT_CACHE_ONLY)) {
		case 0:				/* In the cache */
			break;
		case WT_NOTFOUND:		/* Not in the cache */
			continue;
		default:			/* Something bad */
			goto err;
		}

		(void)__wt_bt_rec_page(toc, page);
		__wt_page_out(toc, page);
	}

done:	/* Wrap up reporting. */
	if (f != NULL)
		f(toc->name, fcnt);

err:	__wt_unlock(env, cache->mtx_reconcile);

	__wt_free(env, drain_base, drain_len);
	return (ret);
}
