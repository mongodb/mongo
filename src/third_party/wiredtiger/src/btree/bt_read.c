/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_las_remove_block --
 *	Remove all records matching a key prefix from the lookaside store.
 */
int
__wt_las_remove_block(WT_SESSION_IMPL *session,
    WT_CURSOR *cursor, uint32_t btree_id, const uint8_t *addr, size_t addr_size)
{
	WT_DECL_ITEM(las_addr);
	WT_DECL_ITEM(las_key);
	WT_DECL_RET;
	uint64_t las_counter, las_txnid;
	int64_t remove_cnt;
	uint32_t las_id;
	int exact;

	remove_cnt = 0;

	WT_ERR(__wt_scr_alloc(session, 0, &las_addr));
	WT_ERR(__wt_scr_alloc(session, 0, &las_key));

	/*
	 * Search for the block's unique prefix and step through all matching
	 * records, removing them.
	 */
	las_addr->data = addr;
	las_addr->size = addr_size;
	las_key->size = 0;
	cursor->set_key(
	    cursor, btree_id, las_addr, (uint64_t)0, (uint32_t)0, las_key);
	if ((ret = cursor->search_near(cursor, &exact)) == 0 && exact < 0)
		ret = cursor->next(cursor);
	for (; ret == 0; ret = cursor->next(cursor)) {
		WT_ERR(cursor->get_key(cursor,
		    &las_id, las_addr, &las_counter, &las_txnid, las_key));

		/*
		 * Confirm the search using the unique prefix; if not a match,
		 * we're done searching for records for this page.
		 */
		 if (las_id != btree_id ||
		     las_addr->size != addr_size ||
		     memcmp(las_addr->data, addr, addr_size) != 0)
			break;

		/*
		 * Cursor opened overwrite=true: won't return WT_NOTFOUND should
		 * another thread remove the record before we do, and the cursor
		 * remains positioned in that case.
		 */
		WT_ERR(cursor->remove(cursor));
		++remove_cnt;
	}
	WT_ERR_NOTFOUND_OK(ret);

err:	__wt_scr_free(session, &las_addr);
	__wt_scr_free(session, &las_key);

	/*
	 * If there were races to remove records, we can over-count.  All
	 * arithmetic is signed, so underflow isn't fatal, but check anyway so
	 * we don't skew low over time.
	 */
	if (remove_cnt > S2C(session)->las_record_cnt)
		S2C(session)->las_record_cnt = 0;
	else if (remove_cnt > 0)
		(void)__wt_atomic_subi64(
		    &S2C(session)->las_record_cnt, remove_cnt);

	return (ret);
}

/*
 * __col_instantiate --
 *	Update a column-store page entry based on a lookaside table update list.
 */
static int
__col_instantiate(WT_SESSION_IMPL *session,
    uint64_t recno, WT_REF *ref, WT_CURSOR_BTREE *cbt, WT_UPDATE *upd)
{
	/* Search the page and add updates. */
	WT_RET(__wt_col_search(session, recno, ref, cbt));
	WT_RET(__wt_col_modify(session, cbt, recno, NULL, upd, false));
	return (0);
}

/*
 * __row_instantiate --
 *	Update a row-store page entry based on a lookaside table update list.
 */
static int
__row_instantiate(WT_SESSION_IMPL *session,
    WT_ITEM *key, WT_REF *ref, WT_CURSOR_BTREE *cbt, WT_UPDATE *upd)
{
	/* Search the page and add updates. */
	WT_RET(__wt_row_search(session, key, ref, cbt, true));
	WT_RET(__wt_row_modify(session, cbt, key, NULL, upd, false));
	return (0);
}

/*
 * __las_page_instantiate --
 *	Instantiate lookaside update records in a recently read page.
 */
static int
__las_page_instantiate(WT_SESSION_IMPL *session,
    WT_REF *ref, uint32_t read_id, const uint8_t *addr, size_t addr_size)
{
	WT_CURSOR *cursor;
	WT_CURSOR_BTREE cbt;
	WT_DECL_ITEM(current_key);
	WT_DECL_ITEM(las_addr);
	WT_DECL_ITEM(las_key);
	WT_DECL_ITEM(las_value);
	WT_DECL_RET;
	WT_PAGE *page;
	WT_UPDATE *first_upd, *last_upd, *upd;
	size_t incr, total_incr;
	uint64_t current_recno, las_counter, las_txnid, recno, upd_txnid;
	uint32_t las_id, upd_size, session_flags;
	int exact;
	const uint8_t *p;

	cursor = NULL;
	page = ref->page;
	first_upd = last_upd = upd = NULL;
	total_incr = 0;
	current_recno = recno = WT_RECNO_OOB;
	session_flags = 0;		/* [-Werror=maybe-uninitialized] */

	__wt_btcur_init(session, &cbt);
	__wt_btcur_open(&cbt);

	WT_ERR(__wt_scr_alloc(session, 0, &current_key));
	WT_ERR(__wt_scr_alloc(session, 0, &las_addr));
	WT_ERR(__wt_scr_alloc(session, 0, &las_key));
	WT_ERR(__wt_scr_alloc(session, 0, &las_value));

	/* Open a lookaside table cursor. */
	WT_ERR(__wt_las_cursor(session, &cursor, &session_flags));

	/*
	 * The lookaside records are in key and update order, that is, there
	 * will be a set of in-order updates for a key, then another set of
	 * in-order updates for a subsequent key. We process all of the updates
	 * for a key and then insert those updates into the page, then all the
	 * updates for the next key, and so on.
	 *
	 * Search for the block's unique prefix, stepping through any matching
	 * records.
	 */
	las_addr->data = addr;
	las_addr->size = addr_size;
	las_key->size = 0;
	cursor->set_key(
	    cursor, read_id, las_addr, (uint64_t)0, (uint32_t)0, las_key);
	if ((ret = cursor->search_near(cursor, &exact)) == 0 && exact < 0)
		ret = cursor->next(cursor);
	for (; ret == 0; ret = cursor->next(cursor)) {
		WT_ERR(cursor->get_key(cursor,
		    &las_id, las_addr, &las_counter, &las_txnid, las_key));

		/*
		 * Confirm the search using the unique prefix; if not a match,
		 * we're done searching for records for this page.
		 */
		if (las_id != read_id ||
		    las_addr->size != addr_size ||
		    memcmp(las_addr->data, addr, addr_size) != 0)
			break;

		/*
		 * If the on-page value has become globally visible, this record
		 * is no longer needed.
		 */
		if (__wt_txn_visible_all(session, las_txnid))
			continue;

		/* Allocate the WT_UPDATE structure. */
		WT_ERR(cursor->get_value(
		    cursor, &upd_txnid, &upd_size, las_value));
		WT_ERR(__wt_update_alloc(session,
		    (upd_size == WT_UPDATE_DELETED_VALUE) ? NULL : las_value,
		    &upd, &incr));
		total_incr += incr;
		upd->txnid = upd_txnid;

		switch (page->type) {
		case WT_PAGE_COL_FIX:
		case WT_PAGE_COL_VAR:
			p = las_key->data;
			WT_ERR(__wt_vunpack_uint(&p, 0, &recno));
			if (current_recno == recno)
				break;
			WT_ASSERT(session, current_recno < recno);

			if (first_upd != NULL) {
				WT_ERR(__col_instantiate(session,
				    current_recno, ref, &cbt, first_upd));
				first_upd = NULL;
			}
			current_recno = recno;
			break;
		case WT_PAGE_ROW_LEAF:
			if (current_key->size == las_key->size &&
			    memcmp(current_key->data,
			    las_key->data, las_key->size) == 0)
				break;

			if (first_upd != NULL) {
				WT_ERR(__row_instantiate(session,
				    current_key, ref, &cbt, first_upd));
				first_upd = NULL;
			}
			WT_ERR(__wt_buf_set(session,
			    current_key, las_key->data, las_key->size));
			break;
		WT_ILLEGAL_VALUE_ERR(session);
		}

		/* Append the latest update to the list. */
		if (first_upd == NULL)
			first_upd = last_upd = upd;
		else {
			last_upd->next = upd;
			last_upd = upd;
		}
		upd = NULL;
	}
	WT_ERR_NOTFOUND_OK(ret);

	/* Insert the last set of updates, if any. */
	if (first_upd != NULL)
		switch (page->type) {
		case WT_PAGE_COL_FIX:
		case WT_PAGE_COL_VAR:
			WT_ERR(__col_instantiate(session,
			    current_recno, ref, &cbt, first_upd));
			first_upd = NULL;
			break;
		case WT_PAGE_ROW_LEAF:
			WT_ERR(__row_instantiate(session,
			    current_key, ref, &cbt, first_upd));
			first_upd = NULL;
			break;
		WT_ILLEGAL_VALUE_ERR(session);
		}

	/* Discard the cursor. */
	WT_ERR(__wt_las_cursor_close(session, &cursor, session_flags));

	if (total_incr != 0) {
		__wt_cache_page_inmem_incr(session, page, total_incr);

		/*
		 * We've modified/dirtied the page, but that's not necessary and
		 * if we keep the page clean, it's easier to evict. We leave the
		 * lookaside table updates in place, so if we evict this page
		 * without dirtying it, any future instantiation of it will find
		 * the records it needs. If the page is dirtied before eviction,
		 * then we'll write any needed lookaside table records for the
		 * new location of the page.
		 */
		__wt_page_modify_clear(session, page);
	}

err:	WT_TRET(__wt_las_cursor_close(session, &cursor, session_flags));
	WT_TRET(__wt_btcur_close(&cbt, true));

	/*
	 * On error, upd points to a single unlinked WT_UPDATE structure,
	 * first_upd points to a list.
	 */
	__wt_free(session, upd);
	__wt_free_update_list(session, first_upd);

	__wt_scr_free(session, &current_key);
	__wt_scr_free(session, &las_addr);
	__wt_scr_free(session, &las_key);
	__wt_scr_free(session, &las_value);

	return (ret);
}

/*
 * __evict_force_check --
 *	Check if a page matches the criteria for forced eviction.
 */
static int
__evict_force_check(WT_SESSION_IMPL *session, WT_REF *ref)
{
	WT_BTREE *btree;
	WT_PAGE *page;

	btree = S2BT(session);
	page = ref->page;

	/* Leaf pages only. */
	if (WT_PAGE_IS_INTERNAL(page))
		return (0);

	/*
	 * It's hard to imagine a page with a huge memory footprint that has
	 * never been modified, but check to be sure.
	 */
	if (page->modify == NULL)
		return (0);

	/* Pages are usually small enough, check that first. */
	if (page->memory_footprint < btree->splitmempage)
		return (0);
	else if (page->memory_footprint < btree->maxmempage)
		return (__wt_leaf_page_can_split(session, page));

	/* Trigger eviction on the next page release. */
	__wt_page_evict_soon(page);

	/* Bump the oldest ID, we're about to do some visibility checks. */
	WT_RET(__wt_txn_update_oldest(session, 0));

	/* If eviction cannot succeed, don't try. */
	return (__wt_page_can_evict(session, ref, NULL));
}

/*
 * __page_read --
 *	Read a page from the file.
 */
static int
__page_read(WT_SESSION_IMPL *session, WT_REF *ref)
{
	const WT_PAGE_HEADER *dsk;
	WT_BTREE *btree;
	WT_DECL_RET;
	WT_ITEM tmp;
	WT_PAGE *page;
	size_t addr_size;
	uint32_t previous_state;
	const uint8_t *addr;

	btree = S2BT(session);
	page = NULL;

	/*
	 * Don't pass an allocated buffer to the underlying block read function,
	 * force allocation of new memory of the appropriate size.
	 */
	WT_CLEAR(tmp);

	/*
	 * Attempt to set the state to WT_REF_READING for normal reads, or
	 * WT_REF_LOCKED, for deleted pages.  If successful, we've won the
	 * race, read the page.
	 */
	if (__wt_atomic_casv32(&ref->state, WT_REF_DISK, WT_REF_READING))
		previous_state = WT_REF_DISK;
	else if (__wt_atomic_casv32(&ref->state, WT_REF_DELETED, WT_REF_LOCKED))
		previous_state = WT_REF_DELETED;
	else
		return (0);

	/*
	 * Get the address: if there is no address, the page was deleted, but a
	 * subsequent search or insert is forcing re-creation of the name space.
	 */
	__wt_ref_info(ref, &addr, &addr_size, NULL);
	if (addr == NULL) {
		WT_ASSERT(session, previous_state == WT_REF_DELETED);

		WT_ERR(__wt_btree_new_leaf_page(session, &page));
		ref->page = page;
		goto done;
	}

	/*
	 * There's an address, read or map the backing disk page and build an
	 * in-memory version of the page.
	 */
	WT_ERR(__wt_bt_read(session, &tmp, addr, addr_size));
	WT_ERR(__wt_page_inmem(session, ref, tmp.data, tmp.memsize,
	    WT_DATA_IN_ITEM(&tmp) ?
	    WT_PAGE_DISK_ALLOC : WT_PAGE_DISK_MAPPED, &page));

	/*
	 * Clear the local reference to an allocated copy of the disk image on
	 * return; the page steals it, errors in this code should not free it.
	 */
	tmp.mem = NULL;

	/*
	 * If reading for a checkpoint, there's no additional work to do, the
	 * page on disk is correct as written.
	 */
	if (session->dhandle->checkpoint != NULL)
		goto done;

	/* If the page was deleted, instantiate that information. */
	if (previous_state == WT_REF_DELETED)
		WT_ERR(__wt_delete_page_instantiate(session, ref));

	/*
	 * Instantiate updates from the database's lookaside table. The page
	 * flag was set when the page was written, potentially a long time ago.
	 * We only care if the lookaside table is currently active, check that
	 * before doing any work.
	 */
	dsk = tmp.data;
	if (F_ISSET(dsk, WT_PAGE_LAS_UPDATE) && __wt_las_is_written(session)) {
		WT_STAT_FAST_CONN_INCR(session, cache_read_lookaside);
		WT_STAT_FAST_DATA_INCR(session, cache_read_lookaside);

		WT_ERR(__las_page_instantiate(
		    session, ref, btree->id, addr, addr_size));
	}

done:	WT_PUBLISH(ref->state, WT_REF_MEM);
	return (0);

err:	/*
	 * If the function building an in-memory version of the page failed,
	 * it discarded the page, but not the disk image.  Discard the page
	 * and separately discard the disk image in all cases.
	 */
	if (ref->page != NULL)
		__wt_ref_out(session, ref);
	WT_PUBLISH(ref->state, previous_state);

	__wt_buf_free(session, &tmp);

	return (ret);
}

/*
 * __wt_page_in_func --
 *	Acquire a hazard pointer to a page; if the page is not in-memory,
 *	read it from the disk and build an in-memory version.
 */
int
__wt_page_in_func(WT_SESSION_IMPL *session, WT_REF *ref, uint32_t flags
#ifdef HAVE_DIAGNOSTIC
    , const char *file, int line
#endif
    )
{
	WT_BTREE *btree;
	WT_DECL_RET;
	WT_PAGE *page;
	u_int sleep_cnt, wait_cnt;
	bool busy, cache_work, evict_soon, stalled;
	int force_attempts;

	btree = S2BT(session);

	WT_STAT_FAST_CONN_INCR(session, cache_pages_requested);
	WT_STAT_FAST_DATA_INCR(session, cache_pages_requested);
	for (evict_soon = stalled = false,
	    force_attempts = 0, sleep_cnt = wait_cnt = 0;;) {
		switch (ref->state) {
		case WT_REF_DELETED:
			if (LF_ISSET(WT_READ_NO_EMPTY) &&
			    __wt_delete_page_skip(session, ref, false))
				return (WT_NOTFOUND);
			/* FALLTHROUGH */
		case WT_REF_DISK:
			if (LF_ISSET(WT_READ_CACHE))
				return (WT_NOTFOUND);

			/*
			 * The page isn't in memory, read it. If this thread is
			 * allowed to do eviction work, check for space in the
			 * cache.
			 */
			if (!LF_ISSET(WT_READ_NO_EVICT))
				WT_RET(__wt_cache_eviction_check(
				    session, 1, NULL));
			WT_RET(__page_read(session, ref));

			/*
			 * If configured to not trash the cache, leave the page
			 * generation unset, we'll set it before returning to
			 * the oldest read generation, so the page is forcibly
			 * evicted as soon as possible. We don't do that set
			 * here because we don't want to evict the page before
			 * we "acquire" it.
			 */
			evict_soon = LF_ISSET(WT_READ_WONT_NEED) ||
			    F_ISSET(session, WT_SESSION_NO_CACHE);
			continue;
		case WT_REF_READING:
			if (LF_ISSET(WT_READ_CACHE))
				return (WT_NOTFOUND);
			if (LF_ISSET(WT_READ_NO_WAIT))
				return (WT_NOTFOUND);

			/* Waiting on another thread's read, stall. */
			WT_STAT_FAST_CONN_INCR(session, page_read_blocked);
			stalled = true;
			break;
		case WT_REF_LOCKED:
			if (LF_ISSET(WT_READ_NO_WAIT))
				return (WT_NOTFOUND);

			/* Waiting on eviction, stall. */
			WT_STAT_FAST_CONN_INCR(session, page_locked_blocked);
			stalled = true;
			break;
		case WT_REF_SPLIT:
			return (WT_RESTART);
		case WT_REF_MEM:
			/*
			 * The page is in memory.
			 *
			 * Get a hazard pointer if one is required. We cannot
			 * be evicting if no hazard pointer is required, we're
			 * done.
			 */
			if (F_ISSET(btree, WT_BTREE_IN_MEMORY))
				goto skip_evict;

			/*
			 * The expected reason we can't get a hazard pointer is
			 * because the page is being evicted, yield, try again.
			 */
#ifdef HAVE_DIAGNOSTIC
			WT_RET(
			    __wt_hazard_set(session, ref, &busy, file, line));
#else
			WT_RET(__wt_hazard_set(session, ref, &busy));
#endif
			if (busy) {
				WT_STAT_FAST_CONN_INCR(
				    session, page_busy_blocked);
				break;
			}

			/*
			 * If eviction is configured for this file, check to see
			 * if the page qualifies for forced eviction and update
			 * the page's generation number. If eviction isn't being
			 * done on this file, we're done.
			 */
			if (LF_ISSET(WT_READ_NO_EVICT) ||
			    F_ISSET(session, WT_SESSION_NO_EVICTION) ||
			    F_ISSET(btree, WT_BTREE_NO_EVICTION))
				goto skip_evict;

			/*
			 * Forcibly evict pages that are too big.
			 */
			if (force_attempts < 10 &&
			    __evict_force_check(session, ref)) {
				++force_attempts;
				ret = __wt_page_release_evict(session, ref);
				/* If forced eviction fails, stall. */
				if (ret == EBUSY) {
					ret = 0;
					WT_STAT_FAST_CONN_INCR(session,
					    page_forcible_evict_blocked);
					stalled = true;
					break;
				}
				WT_RET(ret);

				/*
				 * The result of a successful forced eviction
				 * is a page-state transition (potentially to
				 * an in-memory page we can use, or a restart
				 * return for our caller), continue the outer
				 * page-acquisition loop.
				 */
				continue;
			}

			/*
			 * If we read the page and are configured to not trash
			 * the cache, and no other thread has already used the
			 * page, set the oldest read generation so the page is
			 * forcibly evicted as soon as possible.
			 *
			 * Otherwise, if we read the page, or, if configured to
			 * update the page's read generation and the page isn't
			 * already flagged for forced eviction, update the page
			 * read generation.
			 */
			page = ref->page;
			if (page->read_gen == WT_READGEN_NOTSET) {
				if (evict_soon)
					__wt_page_evict_soon(page);
				else
					__wt_cache_read_gen_new(session, page);
			} else if (!LF_ISSET(WT_READ_NO_GEN))
				__wt_cache_read_gen_bump(session, page);
skip_evict:
			/*
			 * Check if we need an autocommit transaction.
			 * Starting a transaction can trigger eviction, so skip
			 * it if eviction isn't permitted.
			 */
			return (LF_ISSET(WT_READ_NO_EVICT) ? 0 :
			    __wt_txn_autocommit_check(session));
		WT_ILLEGAL_VALUE(session);
		}

		/*
		 * We failed to get the page -- yield before retrying, and if
		 * we've yielded enough times, start sleeping so we don't burn
		 * CPU to no purpose.
		 */
		if (stalled)
			wait_cnt += WT_THOUSAND;
		else if (++wait_cnt < WT_THOUSAND) {
			__wt_yield();
			continue;
		}

		/*
		 * If stalling and this thread is allowed to do eviction work,
		 * check if the cache needs help. If we do work for the cache,
		 * substitute that for a sleep.
		 */
		if (!LF_ISSET(WT_READ_NO_EVICT)) {
			WT_RET(
			    __wt_cache_eviction_check(session, 1, &cache_work));
			if (cache_work)
				continue;
		}
		sleep_cnt = WT_MIN(sleep_cnt + WT_THOUSAND, 10000);
		WT_STAT_FAST_CONN_INCRV(session, page_sleep, sleep_cnt);
		__wt_sleep(0, sleep_cnt);
	}
}
