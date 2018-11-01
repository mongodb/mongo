/*-
 * Copyright (c) 2014-2018 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_row_random_leaf --
 *	Return a random key from a row-store leaf page.
 */
int
__wt_row_random_leaf(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt)
{
	WT_INSERT *ins, **start, **stop;
	WT_INSERT_HEAD *ins_head;
	WT_PAGE *page;
	uint64_t samples;
	uint32_t choice, entries, i;
	int level;

	page = cbt->ref->page;
	start = stop = NULL;		/* [-Wconditional-uninitialized] */
	entries = 0;			/* [-Wconditional-uninitialized] */

	__cursor_pos_clear(cbt);

	/* If the page has disk-based entries, select from them. */
	if (page->entries != 0) {
		cbt->compare = 0;
		cbt->slot = __wt_random(&session->rnd) % page->entries;

		/*
		 * The real row-store search function builds the key, so we
		 * have to as well.
		 */
		return (__wt_row_leaf_key(session,
		    page, page->pg_row + cbt->slot, cbt->tmp, false));
	}

	/*
	 * If the tree is new (and not empty), it might have a large insert
	 * list.
	 *
	 * Walk down the list until we find a level with at least 50 entries,
	 * that's where we'll start rolling random numbers. The value 50 is
	 * used to ignore levels with only a few entries, that is, levels which
	 * are potentially badly skewed.
	 */
	F_SET(cbt, WT_CBT_SEARCH_SMALLEST);
	if ((ins_head = WT_ROW_INSERT_SMALLEST(page)) == NULL)
		return (WT_NOTFOUND);
	for (level = WT_SKIP_MAXDEPTH - 1; level >= 0; --level) {
		start = &ins_head->head[level];
		for (entries = 0, stop = start;
		    *stop != NULL; stop = &(*stop)->next[level])
			++entries;

		if (entries > 50)
			break;
	}

	/*
	 * If it's a tiny list and we went all the way to level 0, correct the
	 * level; entries is correctly set.
	 */
	if (level < 0)
		level = 0;

	/*
	 * Step down the skip list levels, selecting a random chunk of the name
	 * space at each level.
	 */
	for (samples = entries; level > 0; samples += entries) {
		/*
		 * There are (entries) or (entries + 1) chunks of the name space
		 * considered at each level. They are: between start and the 1st
		 * element, between the 1st and 2nd elements, and so on to the
		 * last chunk which is the name space after the stop element on
		 * the current level. This last chunk of name space may or may
		 * not be there: as we descend the levels of the skip list, this
		 * chunk may appear, depending if the next level down has
		 * entries logically after the stop point in the current level.
		 * We can't ignore those entries: because of the algorithm used
		 * to determine the depth of a skiplist, there may be a large
		 * number of entries "revealed" by descending a level.
		 *
		 * If the next level down has more items after the current stop
		 * point, there are (entries + 1) chunks to consider, else there
		 * are (entries) chunks.
		 */
		if (*(stop - 1) == NULL)
			choice = __wt_random(&session->rnd) % entries;
		else
			choice = __wt_random(&session->rnd) % (entries + 1);

		if (choice == entries) {
			/*
			 * We selected the name space after the stop element on
			 * this level. Set the start point to the current stop
			 * point, descend a level and move the stop element to
			 * the end of the list, that is, the end of the newly
			 * discovered name space, counting entries as we go.
			 */
			start = stop;
			--start;
			--level;
			for (entries = 0, stop = start;
			    *stop != NULL; stop = &(*stop)->next[level])
				++entries;
		} else {
			/*
			 * We selected another name space on the level. Move the
			 * start pointer the selected number of entries forward
			 * to the start of the selected chunk (if the selected
			 * number is 0, start won't move). Set the stop pointer
			 * to the next element in the list and drop both start
			 * and stop down a level.
			 */
			for (i = 0; i < choice; ++i)
				start = &(*start)->next[level];
			stop = &(*start)->next[level];

			--start;
			--stop;
			--level;

			/* Count the entries in the selected name space. */
			for (entries = 0,
			    ins = *start; ins != *stop; ins = ins->next[level])
				++entries;
		}
	}

	/*
	 * When we reach the bottom level, entries will already be set. Select
	 * a random entry from the name space and return it.
	 *
	 * It should be impossible for the entries count to be 0 at this point,
	 * but check for it out of paranoia and to quiet static testing tools.
	 */
	if (entries > 0)
		entries = __wt_random(&session->rnd) % entries;
	for (ins = *start; entries > 0; --entries)
		ins = ins->next[0];

	cbt->ins = ins;
	cbt->ins_head = ins_head;
	cbt->compare = 0;

	/*
	 * Random lookups in newly created collections can be slow if a page
	 * consists of a large skiplist. Schedule the page for eviction if we
	 * encounter a large skiplist. This worthwhile because applications
	 * that take a sample often take many samples, so the overhead of
	 * traversing the skip list each time accumulates to real time.
	 */
	if (samples > 5000)
		__wt_page_evict_soon(session, cbt->ref);

	return (0);
}

/*
 * __wt_random_descent --
 *	Find a random page in a tree for either sampling or eviction.
 */
int
__wt_random_descent(WT_SESSION_IMPL *session, WT_REF **refp, uint32_t flags)
{
	WT_BTREE *btree;
	WT_DECL_RET;
	WT_PAGE *page;
	WT_PAGE_INDEX *pindex;
	WT_REF *current, *descent;
	uint32_t i, entries, retry;
	bool eviction;

	*refp = NULL;

	btree = S2BT(session);
	current = NULL;
	retry = 100;
	/*
	 * This function is called by eviction to find a random page in the
	 * cache. That case is indicated by the WT_READ_CACHE flag. Ordinary
	 * lookups in a tree will read pages into cache as needed.
	 */
	eviction = LF_ISSET(WT_READ_CACHE);

	if (0) {
restart:	/*
		 * Discard the currently held page and restart the search from
		 * the root.
		 */
		WT_RET(__wt_page_release(session, current, flags));
	}

	/* Search the internal pages of the tree. */
	current = &btree->root;
	for (;;) {
		page = current->page;
		if (!WT_PAGE_IS_INTERNAL(page))
			break;

		WT_INTL_INDEX_GET(session, page, pindex);
		entries = pindex->entries;

		/* Eviction just wants any random child. */
		if (eviction) {
			descent = pindex->index[
			    __wt_random(&session->rnd) % entries];
			goto descend;
		}

		/*
		 * There may be empty pages in the tree, and they're useless to
		 * us. If we don't find a non-empty page in "entries" random
		 * guesses, take the first non-empty page in the tree. If the
		 * search page contains nothing other than empty pages, restart
		 * from the root some number of times before giving up.
		 *
		 * Random sampling is looking for a key/value pair on a random
		 * leaf page, and so will accept any page that contains a valid
		 * key/value pair, so on-disk is fine, but deleted is not.
		 */
		descent = NULL;
		for (i = 0; i < entries; ++i) {
			descent =
			    pindex->index[__wt_random(&session->rnd) % entries];
			if (descent->state == WT_REF_DISK ||
			    descent->state == WT_REF_LIMBO ||
			    descent->state == WT_REF_LOOKASIDE ||
			    descent->state == WT_REF_MEM)
				break;
		}
		if (i == entries)
			for (i = 0; i < entries; ++i) {
				descent = pindex->index[i];
				if (descent->state == WT_REF_DISK ||
				    descent->state == WT_REF_LIMBO ||
				    descent->state == WT_REF_LOOKASIDE ||
				    descent->state == WT_REF_MEM)
					break;
			}
		if (i == entries || descent == NULL) {
			if (--retry > 0)
				goto restart;

			WT_RET(__wt_page_release(session, current, flags));
			return (WT_NOTFOUND);
		}

		/*
		 * Swap the current page for the child page. If the page splits
		 * while we're retrieving it, restart the search at the root.
		 *
		 * On other error, simply return, the swap call ensures we're
		 * holding nothing on failure.
		 */
descend:	if ((ret = __wt_page_swap(
		    session, current, descent, flags)) == 0) {
			current = descent;
			continue;
		}
		if (eviction && (ret == WT_NOTFOUND || ret == WT_RESTART))
			break;
		if (ret == WT_RESTART)
			goto restart;
		return (ret);
	}

	/*
	 * There is no point starting with the root page: the walk will exit
	 * immediately.  In that case we aren't holding a hazard pointer so
	 * there is nothing to release.
	 */
	if (!eviction || !__wt_ref_is_root(current))
		*refp = current;
	return (0);
}

/*
 * __wt_btcur_next_random --
 *	Move to a random record in the tree. There are two algorithms, one
 *	where we select a record at random from the whole tree on each
 *	retrieval and one where we first select a record at random from the
 *	whole tree, and then subsequently sample forward from that location.
 *	The sampling approach allows us to select reasonably uniform random
 *	points from unbalanced trees.
 */
int
__wt_btcur_next_random(WT_CURSOR_BTREE *cbt)
{
	WT_BTREE *btree;
	WT_CURSOR *cursor;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	WT_UPDATE *upd;
	wt_off_t size;
	uint64_t n, skip;
	uint32_t read_flags;
	bool valid;

	btree = cbt->btree;
	cursor = &cbt->iface;
	session = (WT_SESSION_IMPL *)cbt->iface.session;
	read_flags = WT_READ_RESTART_OK;
	if (F_ISSET(cbt, WT_CBT_READ_ONCE))
		FLD_SET(read_flags, WT_READ_WONT_NEED);

	/*
	 * Only supports row-store: applications can trivially select a random
	 * value from a column-store, if there were any reason to do so.
	 */
	if (btree->type != BTREE_ROW)
		WT_RET_MSG(session, ENOTSUP,
		    "WT_CURSOR.next_random only supported by row-store tables");

	WT_STAT_CONN_INCR(session, cursor_next);
	WT_STAT_DATA_INCR(session, cursor_next);

	F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);

#ifdef HAVE_DIAGNOSTIC
	/*
	 * Under some conditions we end up using the underlying cursor.next to
	 * walk through the object. Since there are multiple calls, we can hit
	 * the cursor-order checks, turn them off.
	 */
	__wt_cursor_key_order_reset(cbt);
#endif
	/*
	 * If we don't have a current position in the tree, or if retrieving
	 * random values without sampling, pick a roughly random leaf page in
	 * the tree and return an entry from it.
	 */
	if (cbt->ref == NULL || cbt->next_random_sample_size == 0) {
		WT_ERR(__cursor_func_init(cbt, true));
		WT_WITH_PAGE_INDEX(session,
		    ret = __wt_random_descent(session, &cbt->ref, read_flags));
		if (ret == 0)
			goto random_page_entry;

		/*
		 * Random descent may return not-found: the tree might be empty
		 * or have so many deleted items we didn't find any valid pages.
		 * We can't return WT_NOTFOUND to the application unless a tree
		 * is really empty, fallback to skipping through tree pages.
		 */
		WT_ERR_NOTFOUND_OK(ret);
	}

	/*
	 * Cursor through the tree, skipping past the sample size of the leaf
	 * pages in the tree between each random key return to compensate for
	 * unbalanced trees.
	 *
	 * If the random descent attempt failed, we don't have a configured
	 * sample size, use 100 for no particular reason.
	 */
	if (cbt->next_random_sample_size == 0)
		cbt->next_random_sample_size = 100;

	/*
	 * If the random descent attempt failed, or it's our first skip attempt,
	 * we haven't yet set the pages to skip, do it now.
	 *
	 * Use the underlying file size divided by its block allocation size as
	 * our guess of leaf pages in the file (this can be entirely wrong, as
	 * it depends on how many pages are in this particular checkpoint, how
	 * large the leaf and internal pages really are, and other factors).
	 * Then, divide that value by the configured sample size and increment
	 * the final result to make sure tiny files don't leave us with a skip
	 * value of 0.
	 *
	 * !!!
	 * Ideally, the number would be prime to avoid restart issues.
	 */
	if (cbt->next_random_leaf_skip == 0) {
		WT_ERR(btree->bm->size(btree->bm, session, &size));
		cbt->next_random_leaf_skip = (uint64_t)
		    ((size / btree->allocsize) /
		    cbt->next_random_sample_size) + 1;
	}

	/*
	 * Be paranoid about loop termination: first, if the last leaf page
	 * skipped was also the last leaf page in the tree, skip may be set to
	 * zero on return along with the NULL WT_REF end-of-walk condition.
	 * Second, if a tree has no valid pages at all (the condition after
	 * initial creation), we might make no progress at all, or finally, if
	 * a tree has only deleted pages, we'll make progress, but never get a
	 * useful WT_REF. And, of course, the tree can switch from one of these
	 * states to another without warning. Decrement skip regardless of what
	 * is happening in the search, guarantee we eventually quit.
	 *
	 * Pages read for data sampling aren't "useful"; don't update the read
	 * generation of pages already in memory, and if a page is read, set
	 * its generation to a low value so it is evicted quickly.
	 */
	for (skip = cbt->next_random_leaf_skip; cbt->ref == NULL || skip > 0;) {
		n = skip;
		WT_ERR(__wt_tree_walk_skip(session, &cbt->ref, &skip));
		if (n == skip) {
			if (skip == 0)
				break;
			--skip;
		}
	}

	/*
	 * We can't return WT_NOTFOUND to the application unless a tree is
	 * really empty, fallback to a random entry from the first page in the
	 * tree that has anything at all.
	 */
	if (cbt->ref == NULL)
		WT_ERR(__wt_btcur_next(cbt, false));

random_page_entry:
	/*
	 * Select a random entry from the leaf page. If it's not valid, move to
	 * the next entry, if that doesn't work, move to the previous entry.
	 */
	WT_ERR(__wt_row_random_leaf(session, cbt));
	WT_ERR(__wt_cursor_valid(cbt, &upd, &valid));
	if (valid) {
		WT_ERR(__wt_key_return(session, cbt));
		WT_ERR(__wt_value_return(session, cbt, upd));
	} else {
		if ((ret = __wt_btcur_next(cbt, false)) == WT_NOTFOUND)
			ret = __wt_btcur_prev(cbt, false);
		WT_ERR(ret);
	}
	return (0);

err:	WT_TRET(__cursor_reset(cbt));
	return (ret);
}
