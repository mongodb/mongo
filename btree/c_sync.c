/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int __wt_cache_drain(WT_TOC *, void (*)(const char *, u_int64_t));

/*
 * __wt_cache_sync --
 *	Flush a database's underlying cache to disk.
 */
int
__wt_cache_sync(
    WT_TOC *toc, void (*f)(const char *, u_int64_t), u_int32_t flags)
{
	ENV *env;
	IDB *idb;
	int ret;

	env = toc->env;
	idb = toc->db->idb;
	ret = 0;

	/* Write dirty pages from the cache. */
	WT_RET(__wt_cache_drain(toc, f));

	/* Optionally flush the file to the backing disk. */
	if (!LF_ISSET(WT_OSWRITE))
		WT_TRET(__wt_fsync(env, idb->fh));

	return (ret);
}

typedef struct __wt_sync_list {
	u_int32_t addr;				/* Address */
	u_int32_t size;				/* Bytes */
	u_int8_t  level;			/* Level */
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
 * __wt_cache_drain --
 *	Flush all modified pages for the caller's DB handle.
 */
static int
__wt_cache_drain(WT_TOC *toc, void (*f)(const char *, u_int64_t))
{
	ENV *env;
	DB *db;
	WT_CACHE *cache;
	WT_CACHE_ENTRY *e;
	WT_PAGE *page;
	WT_SYNC_LIST *drain, *drain_base;
	u_int64_t fcnt;
	u_int32_t drain_elem, drain_len, i, j;
	int ret;

	env = toc->env;
	db = toc->db;
	cache = env->ienv->cache;
	fcnt = 0;
	ret = 0;

	drain_base = NULL;
	drain_elem = drain_len = 0;
	WT_CACHE_FOREACH_PAGE_ALL(cache, e, i, j) {
		/*
		 * Skip pages we either don't care about or which are in play
		 * in some way.
		 */
		if (e->db != db || e->state != WT_OK)
			continue;
		page = e->page;

		/*
		 * Sync is just another reader of the cache, so has to obey the
		 * standard reader protocol.  Get a hazard reference: if the
		 * page isn't available for any reason, discard the reference
		 * and continue.
		 */
		__wt_hazard_set(toc, page);
		if (e->state != WT_OK)
			goto next;

		/*
		 * Leaf pages are reconciled immediately -- we could wait, but
		 * no reason not to just do it now (plus, it gives the OS a
		 * start on lazily writing leaf pages to disk, in the face of
		 * a future fsync).
		 */
		if (page->hdr->level == WT_LLEAF) {
			if (f != NULL && ++fcnt % 10 == 0)
				f(toc->name, fcnt);
			if (page->modified)
				WT_ERR(__wt_bt_rec_page(toc, page));
			goto next;
		}

		/* Allocate more space as necessary, and remember this page. */
		if (drain_elem * sizeof(drain_base[0]) >= drain_len) {
			WT_ERR(__wt_realloc(env, &drain_len,
			    (drain_elem + 100) * sizeof(drain_base[0]),
			    &drain_base));
			drain = drain_base + drain_elem;
		}
		drain->addr = page->addr;
		drain->size = page->size;
		drain->level = page->hdr->level;
		++drain;
		++drain_elem;

next:		__wt_hazard_clear(toc, page);
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

	/* Reconcile any modified pages. */
	for (drain = drain_base; drain_elem > 0; ++drain, --drain_elem) {
		if (f != NULL && ++fcnt % 10 == 0)
			f(toc->name, fcnt);

		/*
		 * If we find the page in the cache, check to see if it needs
		 * to be flushed; if we don't find it in the cache, it must
		 * have been reconciled and written by some other thread.
		 */
		switch (ret = __wt_page_in(
		    toc, drain->addr, drain->size, &page, WT_CACHE_ONLY)) {
		case 0:				/* In the cache */
			break;
		case WT_NOTFOUND:		/* Not in the cache */
			continue;
		default:			/* Something bad */
			goto err;
		}

		/* If the page is dirty, reconcile it. */
		if (page->modified)
			WT_ERR(__wt_bt_rec_page(toc, page));

		__wt_page_out(toc, page);
	}

done:	/* Wrap up reporting. */
	if (f != NULL)
		f(toc->name, fcnt);

err:	__wt_free(env, drain_base, drain_len);
	return (ret);
}
