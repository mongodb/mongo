/*-
 * Copyright (c) 2014-2018 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __col_instantiate --
 *	Update a column-store page entry based on a lookaside table update list.
 */
static int
__col_instantiate(WT_SESSION_IMPL *session,
    uint64_t recno, WT_REF *ref, WT_CURSOR_BTREE *cbt, WT_UPDATE *updlist)
{
	WT_PAGE *page;
	WT_UPDATE *upd;

	page = ref->page;

	/*
	 * Discard any of the updates we don't need.
	 *
	 * Just free the memory: it hasn't been accounted for on the page yet.
	 */
	if (updlist->next != NULL &&
	    (upd = __wt_update_obsolete_check(session, page, updlist)) != NULL)
		__wt_free_update_list(session, upd);

	/* Search the page and add updates. */
	WT_RET(__wt_col_search(session, recno, ref, cbt, true));
	WT_RET(__wt_col_modify(
	    session, cbt, recno, NULL, updlist, WT_UPDATE_INVALID, false));
	return (0);
}

/*
 * __row_instantiate --
 *	Update a row-store page entry based on a lookaside table update list.
 */
static int
__row_instantiate(WT_SESSION_IMPL *session,
    WT_ITEM *key, WT_REF *ref, WT_CURSOR_BTREE *cbt, WT_UPDATE *updlist)
{
	WT_PAGE *page;
	WT_UPDATE *upd;

	page = ref->page;

	/*
	 * Discard any of the updates we don't need.
	 *
	 * Just free the memory: it hasn't been accounted for on the page yet.
	 */
	if (updlist->next != NULL &&
	    (upd = __wt_update_obsolete_check(session, page, updlist)) != NULL)
		__wt_free_update_list(session, upd);

	/* Search the page and add updates. */
	WT_RET(__wt_row_search(session, key, ref, cbt, true, true));
	WT_RET(__wt_row_modify(
	    session, cbt, key, NULL, updlist, WT_UPDATE_INVALID, false));
	return (0);
}

/*
 * __las_page_instantiate_verbose --
 *	Create a verbose message to display at most once per checkpoint when
 *	performing a lookaside table read.
 */
static void
__las_page_instantiate_verbose(WT_SESSION_IMPL *session, uint64_t las_pageid)
{
	WT_CACHE *cache;
	uint64_t ckpt_gen_current, ckpt_gen_last;

	if (!WT_VERBOSE_ISSET(session,
	    WT_VERB_LOOKASIDE | WT_VERB_LOOKASIDE_ACTIVITY))
		return;

	cache = S2C(session)->cache;
	ckpt_gen_current = __wt_gen(session, WT_GEN_CHECKPOINT);
	ckpt_gen_last = cache->las_verb_gen_read;

	/*
	 * This message is throttled to one per checkpoint. To do this we
	 * track the generation of the last checkpoint for which the message
	 * was printed and check against the current checkpoint generation.
	 */
	if (WT_VERBOSE_ISSET(session, WT_VERB_LOOKASIDE) ||
	    ckpt_gen_current > ckpt_gen_last) {
		/*
		 * Attempt to atomically replace the last checkpoint generation
		 * for which this message was printed. If the atomic swap fails
		 * we have raced and the winning thread will print the message.
		 */
		if (__wt_atomic_casv64(&cache->las_verb_gen_read,
			ckpt_gen_last, ckpt_gen_current)) {
			__wt_verbose(session,
			    WT_VERB_LOOKASIDE | WT_VERB_LOOKASIDE_ACTIVITY,
			    "Read from lookaside file triggered for "
			    "file ID %" PRIu32 ", page ID %" PRIu64,
			    S2BT(session)->id, las_pageid);
		}
	}
}

/*
 * __las_page_instantiate --
 *	Instantiate lookaside update records in a recently read page.
 */
static int
__las_page_instantiate(WT_SESSION_IMPL *session, WT_REF *ref)
{
	WT_CACHE *cache;
	WT_CURSOR *cursor;
	WT_CURSOR_BTREE cbt;
	WT_DECL_ITEM(current_key);
	WT_DECL_RET;
	WT_ITEM las_key, las_timestamp, las_value;
	WT_PAGE *page;
	WT_UPDATE *first_upd, *last_upd, *upd;
	size_t incr, total_incr;
	uint64_t current_recno, las_counter, las_pageid, las_txnid, recno;
	uint32_t las_id, session_flags;
	const uint8_t *p;
	uint8_t upd_type;
	bool locked;

	cursor = NULL;
	page = ref->page;
	first_upd = last_upd = upd = NULL;
	locked = false;
	total_incr = 0;
	current_recno = recno = WT_RECNO_OOB;
	las_pageid = ref->page_las->las_pageid;
	session_flags = 0;		/* [-Werror=maybe-uninitialized] */
	WT_CLEAR(las_key);

	cache = S2C(session)->cache;
	__las_page_instantiate_verbose(session, las_pageid);
	WT_STAT_CONN_INCR(session, cache_read_lookaside);
	WT_STAT_DATA_INCR(session, cache_read_lookaside);

	__wt_btcur_init(session, &cbt);
	__wt_btcur_open(&cbt);

	WT_ERR(__wt_scr_alloc(session, 0, &current_key));

	/* Open a lookaside table cursor. */
	__wt_las_cursor(session, &cursor, &session_flags);

	/*
	 * The lookaside records are in key and update order, that is, there
	 * will be a set of in-order updates for a key, then another set of
	 * in-order updates for a subsequent key. We process all of the updates
	 * for a key and then insert those updates into the page, then all the
	 * updates for the next key, and so on.
	 */
	WT_PUBLISH(cache->las_reader, true);
	__wt_readlock(session, &cache->las_sweepwalk_lock);
	WT_PUBLISH(cache->las_reader, false);
	locked = true;
	for (ret = __wt_las_cursor_position(cursor, las_pageid);
	    ret == 0;
	    ret = cursor->next(cursor)) {
		WT_ERR(cursor->get_key(cursor,
		    &las_pageid, &las_id, &las_counter, &las_key));

		/*
		 * Confirm the search using the unique prefix; if not a match,
		 * we're done searching for records for this page.
		 */
		if (las_pageid != ref->page_las->las_pageid)
			break;

		/* Allocate the WT_UPDATE structure. */
		WT_ERR(cursor->get_value(cursor,
		    &las_txnid, &las_timestamp, &upd_type, &las_value));
		WT_ERR(__wt_update_alloc(
		    session, &las_value, &upd, &incr, upd_type));
		total_incr += incr;
		upd->txnid = las_txnid;
#ifdef HAVE_TIMESTAMPS
		WT_ASSERT(session, las_timestamp.size == WT_TIMESTAMP_SIZE);
		memcpy(&upd->timestamp, las_timestamp.data, las_timestamp.size);
#endif

		switch (page->type) {
		case WT_PAGE_COL_FIX:
		case WT_PAGE_COL_VAR:
			p = las_key.data;
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
			if (current_key->size == las_key.size &&
			    memcmp(current_key->data,
			    las_key.data, las_key.size) == 0)
				break;

			if (first_upd != NULL) {
				WT_ERR(__row_instantiate(session,
				    current_key, ref, &cbt, first_upd));
				first_upd = NULL;
			}
			WT_ERR(__wt_buf_set(session,
			    current_key, las_key.data, las_key.size));
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
	__wt_readunlock(session, &cache->las_sweepwalk_lock);
	locked = false;
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
		 * If the updates in lookaside are newer than the versions on
		 * the page, it must be included in the next checkpoint.
		 *
		 * Otherwise, the page image contained the newest versions of
		 * data so the updates are all older and we could consider
		 * marking it clean (i.e., the next checkpoint can use the
		 * version already on disk).
		 *
		 * This needs care because (a) it creates pages with history
		 * that can't be evicted until they are marked dirty again, and
		 * (b) checkpoints may need to visit these pages to resolve
		 * changes evicted while a checkpoint is running.
		 */
		page->modify->first_dirty_txn = WT_TXN_FIRST;

		if (ref->page_las->las_skew_newest &&
		    !S2C(session)->txn_global.has_stable_timestamp &&
		    __wt_txn_visible_all(session, ref->page_las->las_max_txn,
		    WT_TIMESTAMP_NULL(&ref->page_las->onpage_timestamp))) {
			page->modify->rec_max_txn = ref->page_las->las_max_txn;
			__wt_timestamp_set(&page->modify->rec_max_timestamp,
			    &ref->page_las->onpage_timestamp);
			__wt_page_modify_clear(session, page);
		}
	}

err:	if (locked)
		__wt_readunlock(session, &cache->las_sweepwalk_lock);
	WT_TRET(__wt_las_cursor_close(session, &cursor, session_flags));
	WT_TRET(__wt_btcur_close(&cbt, true));

	/*
	 * On error, upd points to a single unlinked WT_UPDATE structure,
	 * first_upd points to a list.
	 */
	__wt_free(session, upd);
	__wt_free_update_list(session, first_upd);

	__wt_scr_free(session, &current_key);

	return (ret);
}

/*
 * __evict_force_check --
 *	Check if a page matches the criteria for forced eviction.
 */
static bool
__evict_force_check(WT_SESSION_IMPL *session, WT_REF *ref)
{
	WT_BTREE *btree;
	WT_PAGE *page;

	btree = S2BT(session);
	page = ref->page;

	/* Leaf pages only. */
	if (WT_PAGE_IS_INTERNAL(page))
		return (false);

	/*
	 * It's hard to imagine a page with a huge memory footprint that has
	 * never been modified, but check to be sure.
	 */
	if (__wt_page_evict_clean(page))
		return (false);

	/* Pages are usually small enough, check that first. */
	if (page->memory_footprint < btree->splitmempage)
		return (false);

	/*
	 * If this session has more than one hazard pointer, eviction will fail
	 * and there is no point trying.
	 */
	if (__wt_hazard_count(session, ref) > 1)
		return (false);

	/* If we can do an in-memory split, do it. */
	if (__wt_leaf_page_can_split(session, page))
		return (true);
	if (page->memory_footprint < btree->maxmempage)
		return (false);

	/* Bump the oldest ID, we're about to do some visibility checks. */
	WT_IGNORE_RET(__wt_txn_update_oldest(session, 0));

	/*
	 * Allow some leeway if the transaction ID isn't moving forward since
	 * it is unlikely eviction will be able to evict the page. Don't keep
	 * skipping the page indefinitely or large records can lead to
	 * extremely large memory footprints.
	 */
	if (!__wt_page_evict_retry(session, page))
		return (false);

	/* Trigger eviction on the next page release. */
	__wt_page_evict_soon(session, ref);

	/* If eviction cannot succeed, don't try. */
	return (__wt_page_can_evict(session, ref, NULL));
}

/*
 * __page_read --
 *	Read a page from the file.
 */
static int
__page_read(WT_SESSION_IMPL *session, WT_REF *ref, uint32_t flags)
{
	WT_DECL_RET;
	WT_ITEM tmp;
	WT_PAGE *notused;
	size_t addr_size;
	uint64_t time_start, time_stop;
	uint32_t page_flags, final_state, new_state, previous_state;
	const uint8_t *addr;
	bool timer;

	time_start = time_stop = 0;

	/*
	 * Don't pass an allocated buffer to the underlying block read function,
	 * force allocation of new memory of the appropriate size.
	 */
	WT_CLEAR(tmp);

	/*
	 * Attempt to set the state to WT_REF_READING for normal reads, or
	 * WT_REF_LOCKED, for deleted pages or pages with lookaside entries.
	 * The difference is that checkpoints can skip over clean pages that
	 * are being read into cache, but need to wait for deletes or lookaside
	 * updates to be resolved (in order for checkpoint to write the correct
	 * version of the page).
	 *
	 * If successful, we've won the race, read the page.
	 */
	switch (previous_state = ref->state) {
	case WT_REF_DISK:
		new_state = WT_REF_READING;
		break;
	case WT_REF_DELETED:
	case WT_REF_LIMBO:
	case WT_REF_LOOKASIDE:
		new_state = WT_REF_LOCKED;
		break;
	default:
		return (0);
	}
	if (!__wt_atomic_casv32(&ref->state, previous_state, new_state))
		return (0);

	final_state = WT_REF_MEM;

	/* If we already have the page image, just instantiate the history. */
	if (previous_state == WT_REF_LIMBO)
		goto skip_read;

	/*
	 * Get the address: if there is no address, the page was deleted or had
	 * only lookaside entries, and a subsequent search or insert is forcing
	 * re-creation of the name space.
	 */
	__wt_ref_info(ref, &addr, &addr_size, NULL);
	if (addr == NULL) {
		WT_ASSERT(session, previous_state != WT_REF_DISK);

		WT_ERR(__wt_btree_new_leaf_page(session, &ref->page));
		goto skip_read;
	}

	/*
	 * There's an address, read or map the backing disk page and build an
	 * in-memory version of the page.
	 */
	timer = !F_ISSET(session, WT_SESSION_INTERNAL);
	if (timer)
		time_start = __wt_clock(session);
	WT_ERR(__wt_bt_read(session, &tmp, addr, addr_size));
	if (timer) {
		time_stop = __wt_clock(session);
		WT_STAT_CONN_INCR(session, cache_read_app_count);
		WT_STAT_CONN_INCRV(session, cache_read_app_time,
		    WT_CLOCKDIFF_US(time_stop, time_start));
	}

	/*
	 * Build the in-memory version of the page. Clear our local reference to
	 * the allocated copy of the disk image on return, the in-memory object
	 * steals it.
	 *
	 * If a page is read with eviction disabled, we don't count evicting it
	 * as progress. Since disabling eviction allows pages to be read even
	 * when the cache is full, we want to avoid workloads repeatedly reading
	 * a page with eviction disabled (e.g., a metadata page), then evicting
	 * that page and deciding that is a sign that eviction is unstuck.
	 */
	page_flags =
	    WT_DATA_IN_ITEM(&tmp) ? WT_PAGE_DISK_ALLOC : WT_PAGE_DISK_MAPPED;
	if (LF_ISSET(WT_READ_IGNORE_CACHE_SIZE))
		FLD_SET(page_flags, WT_PAGE_EVICT_NO_PROGRESS);
	WT_ERR(__wt_page_inmem(session, ref, tmp.data, page_flags, &notused));
	tmp.mem = NULL;

	/*
	 * The WT_REF lookaside state should match the page-header state of
	 * any page we read.
	 */
	WT_ASSERT(session,
	    (previous_state != WT_REF_LIMBO &&
	    previous_state != WT_REF_LOOKASIDE) ||
	    ref->page->dsk == NULL ||
	    F_ISSET(ref->page->dsk, WT_PAGE_LAS_UPDATE));

skip_read:
	switch (previous_state) {
	case WT_REF_DELETED:
		/*
		 * A truncated page may also have lookaside information. The
		 * delete happened after page eviction (writing the lookaside
		 * information), first update based on the lookaside table and
		 * then apply the delete.
		 */
		if (ref->page_las != NULL) {
			WT_ERR(__las_page_instantiate(session, ref));
			ref->page_las->eviction_to_lookaside = false;
		}

		/* Move all records to a deleted state. */
		WT_ERR(__wt_delete_page_instantiate(session, ref));
		break;
	case WT_REF_LOOKASIDE:
		if (__wt_las_page_skip_locked(session, ref)) {
			WT_STAT_CONN_INCR(
			    session, cache_read_lookaside_skipped);
			ref->page_las->eviction_to_lookaside = true;
			final_state = WT_REF_LIMBO;
			break;
		}
		/* FALLTHROUGH */
	case WT_REF_LIMBO:
		/* Instantiate updates from the database's lookaside table. */
		if (previous_state == WT_REF_LIMBO)
			WT_STAT_CONN_INCR(session, cache_read_lookaside_delay);

		WT_ERR(__las_page_instantiate(session, ref));
		ref->page_las->eviction_to_lookaside = false;
		break;
	}

	/*
	 * We no longer need lookaside entries once the page is instantiated.
	 * There's no reason for the lookaside remove to fail, but ignore it
	 * if for some reason it fails, we've got a valid page.
	 *
	 * Don't free WT_REF.page_las, there may be concurrent readers.
	 */
	if (final_state == WT_REF_MEM && ref->page_las != NULL)
		WT_IGNORE_RET(__wt_las_remove_block(
		    session, ref->page_las->las_pageid, false));

	WT_PUBLISH(ref->state, final_state);
	return (ret);

err:	/*
	 * If the function building an in-memory version of the page failed,
	 * it discarded the page, but not the disk image.  Discard the page
	 * and separately discard the disk image in all cases.
	 */
	if (ref->page != NULL && previous_state != WT_REF_LIMBO)
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
	uint64_t sleep_usecs, yield_cnt;
	uint32_t current_state;
	int force_attempts;
	bool busy, cache_work, did_read, stalled, wont_need;

	btree = S2BT(session);

	if (F_ISSET(session, WT_SESSION_IGNORE_CACHE_SIZE))
		LF_SET(WT_READ_IGNORE_CACHE_SIZE);

	/* Sanity check flag combinations. */
	WT_ASSERT(session, !LF_ISSET(
	    WT_READ_DELETED_SKIP | WT_READ_NO_WAIT | WT_READ_LOOKASIDE) ||
	    LF_ISSET(WT_READ_CACHE));
	WT_ASSERT(session, !LF_ISSET(WT_READ_DELETED_CHECK) ||
	    !LF_ISSET(WT_READ_DELETED_SKIP));

	/*
	 * Ignore reads of pages already known to be in cache, otherwise the
	 * eviction server can dominate these statistics.
	 */
	if (!LF_ISSET(WT_READ_CACHE)) {
		WT_STAT_CONN_INCR(session, cache_pages_requested);
		WT_STAT_DATA_INCR(session, cache_pages_requested);
	}

	for (did_read = wont_need = stalled = false,
	    force_attempts = 0, sleep_usecs = yield_cnt = 0;;) {
		switch (current_state = ref->state) {
		case WT_REF_DELETED:
			if (LF_ISSET(WT_READ_DELETED_SKIP | WT_READ_NO_WAIT))
				return (WT_NOTFOUND);
			if (LF_ISSET(WT_READ_DELETED_CHECK) &&
			    __wt_delete_page_skip(session, ref, false))
				return (WT_NOTFOUND);
			goto read;
		case WT_REF_LOOKASIDE:
			if (LF_ISSET(WT_READ_CACHE)) {
				if (!LF_ISSET(WT_READ_LOOKASIDE))
					return (WT_NOTFOUND);
				/*
				 * If we skip a lookaside page, the tree
				 * cannot be left clean: lookaside entries
				 * must be resolved before the tree can be
				 * discarded.
				 */
				if (__wt_las_page_skip(session, ref)) {
					__wt_tree_modify_set(session);
					return (WT_NOTFOUND);
				}
			}
			goto read;
		case WT_REF_DISK:
			if (LF_ISSET(WT_READ_CACHE))
				return (WT_NOTFOUND);

read:			/*
			 * The page isn't in memory, read it. If this thread
			 * respects the cache size, check for space in the
			 * cache.
			 */
			if (!LF_ISSET(WT_READ_IGNORE_CACHE_SIZE))
				WT_RET(__wt_cache_eviction_check(
				    session, true,
				    !F_ISSET(&session->txn, WT_TXN_HAS_ID),
				    NULL));
			WT_RET(__page_read(session, ref, flags));

			/*
			 * We just read a page, don't evict it before we have a
			 * chance to use it.
			 */
			did_read = true;

			/*
			 * If configured to not trash the cache, leave the page
			 * generation unset, we'll set it before returning to
			 * the oldest read generation, so the page is forcibly
			 * evicted as soon as possible. We don't do that set
			 * here because we don't want to evict the page before
			 * we "acquire" it.
			 */
			wont_need = LF_ISSET(WT_READ_WONT_NEED) ||
			    F_ISSET(session, WT_SESSION_READ_WONT_NEED);
			continue;
		case WT_REF_READING:
			if (LF_ISSET(WT_READ_CACHE))
				return (WT_NOTFOUND);
			if (LF_ISSET(WT_READ_NO_WAIT))
				return (WT_NOTFOUND);

			/* Waiting on another thread's read, stall. */
			WT_STAT_CONN_INCR(session, page_read_blocked);
			stalled = true;
			break;
		case WT_REF_LOCKED:
			if (LF_ISSET(WT_READ_NO_WAIT))
				return (WT_NOTFOUND);

			/* Waiting on eviction, stall. */
			WT_STAT_CONN_INCR(session, page_locked_blocked);
			stalled = true;
			break;
		case WT_REF_SPLIT:
			return (WT_RESTART);
		case WT_REF_LIMBO:
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
				WT_STAT_CONN_INCR(session, page_busy_blocked);
				break;
			}
			/*
			 * If we are a limbo page check whether we need to
			 * instantiate the history. By having a hazard pointer
			 * we can use the locked version.
			 */
			if (current_state == WT_REF_LIMBO &&
			    ((!LF_ISSET(WT_READ_CACHE) ||
			    LF_ISSET(WT_READ_LOOKASIDE)) &&
			    !__wt_las_page_skip_locked(session, ref))) {
				WT_RET(__wt_hazard_clear(session, ref));
				goto read;
			}
			if (current_state == WT_REF_LIMBO &&
			    LF_ISSET(WT_READ_CACHE) &&
			    LF_ISSET(WT_READ_LOOKASIDE))
				__wt_tree_modify_set(session);

			/*
			 * Check if the page requires forced eviction.
			 */
			if (did_read || LF_ISSET(WT_READ_NO_SPLIT) ||
			    btree->evict_disabled > 0 || btree->lsm_primary)
				goto skip_evict;

			/*
			 * If reconciliation is disabled (e.g., when inserting
			 * into the lookaside table), skip forced eviction if
			 * the page can't split.
			 */
			if (F_ISSET(session, WT_SESSION_NO_RECONCILE) &&
			    !__wt_leaf_page_can_split(session, ref->page))
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
					WT_NOT_READ(ret, 0);
					WT_STAT_CONN_INCR(session,
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

skip_evict:		/*
			 * If we read the page and are configured to not trash
			 * the cache, and no other thread has already used the
			 * page, set the read generation so the page is evicted
			 * soon.
			 *
			 * Otherwise, if we read the page, or, if configured to
			 * update the page's read generation and the page isn't
			 * already flagged for forced eviction, update the page
			 * read generation.
			 */
			page = ref->page;
			if (page->read_gen == WT_READGEN_NOTSET) {
				if (wont_need)
					page->read_gen = WT_READGEN_WONT_NEED;
				else
					__wt_cache_read_gen_new(session, page);
			} else if (!LF_ISSET(WT_READ_NO_GEN))
				__wt_cache_read_gen_bump(session, page);

			/*
			 * Check if we need an autocommit transaction.
			 * Starting a transaction can trigger eviction, so skip
			 * it if eviction isn't permitted.
			 *
			 * The logic here is a little weird: some code paths do
			 * a blanket ban on checking the cache size in
			 * sessions, but still require a transaction (e.g.,
			 * when updating metadata or lookaside).  If
			 * WT_READ_IGNORE_CACHE_SIZE was passed in explicitly,
			 * we're done. If we set WT_READ_IGNORE_CACHE_SIZE
			 * because it was set in the session then make sure we
			 * start a transaction.
			 */
			return (LF_ISSET(WT_READ_IGNORE_CACHE_SIZE) &&
			    !F_ISSET(session, WT_SESSION_IGNORE_CACHE_SIZE) ?
			    0 : __wt_txn_autocommit_check(session));
		WT_ILLEGAL_VALUE(session);
		}

		/*
		 * We failed to get the page -- yield before retrying, and if
		 * we've yielded enough times, start sleeping so we don't burn
		 * CPU to no purpose.
		 */
		if (yield_cnt < WT_THOUSAND) {
			if (!stalled) {
				++yield_cnt;
				__wt_yield();
				continue;
			}
			yield_cnt = WT_THOUSAND;
		}

		/*
		 * If stalling and this thread is allowed to do eviction work,
		 * check if the cache needs help. If we do work for the cache,
		 * substitute that for a sleep.
		 */
		if (!LF_ISSET(WT_READ_IGNORE_CACHE_SIZE)) {
			WT_RET(__wt_cache_eviction_check(
			    session, true,
			    !F_ISSET(&session->txn, WT_TXN_HAS_ID),
			    &cache_work));
			if (cache_work)
				continue;
		}
		__wt_spin_backoff(&yield_cnt, &sleep_usecs);
		WT_STAT_CONN_INCRV(session, page_sleep, sleep_usecs);
	}
}
