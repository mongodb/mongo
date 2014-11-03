/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * Tuning; global variables to allow the binary to be patched, we don't yet have
 * any real understanding of what might be useful to surface to applications.
 */
static u_int __split_deepen_max_internal_image = 100;
static u_int __split_deepen_min_child = 10;
static u_int __split_deepen_per_child = 100;
static u_int __split_deepen_split_child = 100;

/*
 * Track allocation increments, matching the cache calculations, which add an
 * estimate of allocation overhead to every object.
 */
#define	WT_MEMSIZE_ADD(total, len)	do {				\
	total += (len) + WT_ALLOC_OVERHEAD;				\
} while (0)
#define	WT_MEMSIZE_TRANSFER(from_decr, to_incr, len) do {		\
	WT_MEMSIZE_ADD(from_decr, len);					\
	WT_MEMSIZE_ADD(to_incr, len);					\
} while (0)

/*
 * __split_oldest_gen --
 *	Calculate the oldest active split generation.
 */
static uint64_t
__split_oldest_gen(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_SESSION_IMPL *s;
	uint64_t gen, oldest;
	u_int i, session_cnt;

	conn = S2C(session);
	WT_ORDERED_READ(session_cnt, conn->session_cnt);
	for (i = 0, s = conn->sessions, oldest = conn->split_gen + 1;
	    i < session_cnt;
	    i++, s++)
		if (((gen = s->split_gen) != 0) && gen < oldest)
			oldest = gen;

	return (oldest);
}

/*
 * __split_stash_add --
 *	Add a new entry into the session's split stash list.
 */
static int
__split_stash_add(WT_SESSION_IMPL *session, void *p, size_t len)
{
	WT_SPLIT_STASH *stash;

	WT_ASSERT(session, p != NULL);

	/* Grow the list as necessary. */
	WT_RET(__wt_realloc_def(session, &session->split_stash_alloc,
	    session->split_stash_cnt + 1, &session->split_stash));

	stash = session->split_stash + session->split_stash_cnt++;
	stash->split_gen = WT_ATOMIC_ADD8(S2C(session)->split_gen, 1);
	stash->p = p;
	stash->len = len;

	WT_STAT_FAST_CONN_ATOMIC_INCRV(session, rec_split_stashed_bytes, len);
	WT_STAT_FAST_CONN_ATOMIC_INCR(session, rec_split_stashed_objects);

	/* See if we can free any previous entries. */
	if (session->split_stash_cnt > 1)
		__wt_split_stash_discard(session);

	return (0);
}

/*
 * __wt_split_stash_discard --
 *	Discard any memory from a session's split stash that we can.
 */
void
__wt_split_stash_discard(WT_SESSION_IMPL *session)
{
	WT_SPLIT_STASH *stash;
	uint64_t oldest;
	size_t i;

	/* Get the oldest split generation. */
	oldest = __split_oldest_gen(session);

	for (i = 0, stash = session->split_stash;
	    i < session->split_stash_cnt;
	    ++i, ++stash) {
		if (stash->p == NULL)
			continue;
		else if (stash->split_gen >= oldest)
			break;
		/*
		 * It's a bad thing if another thread is in this memory after
		 * we free it, make sure nothing good happens to that thread.
		 */
		WT_STAT_FAST_CONN_ATOMIC_DECRV(
		    session, rec_split_stashed_bytes, stash->len);
		WT_STAT_FAST_CONN_ATOMIC_DECR(
		    session, rec_split_stashed_objects);
		__wt_overwrite_and_free_len(session, stash->p, stash->len);
	}

	/*
	 * If there are enough free slots at the beginning of the list, shuffle
	 * everything down.
	 */
	if (i > 100 || i == session->split_stash_cnt)
		if ((session->split_stash_cnt -= i) > 0)
			memmove(session->split_stash, stash,
			    session->split_stash_cnt * sizeof(*stash));
}

/*
 * __wt_split_stash_discard_all --
 *	Discard all memory from a session's split stash.
 */
void
__wt_split_stash_discard_all(
    WT_SESSION_IMPL *session_safe, WT_SESSION_IMPL *session)
{
	WT_SPLIT_STASH *stash;
	size_t i;

	/*
	 * This function is called during WT_CONNECTION.close to discard any
	 * memory that remains.  For that reason, we take two WT_SESSION_IMPL
	 * arguments: session_safe is still linked to the WT_CONNECTION and
	 * can be safely used for calls to other WiredTiger functions, while
	 * session is the WT_SESSION_IMPL we're cleaning up.
	 */
	for (i = 0, stash = session->split_stash;
	    i < session->split_stash_cnt;
	    ++i, ++stash)
		if (stash->p != NULL)
			__wt_free(session_safe, stash->p);

	__wt_free(session_safe, session->split_stash);
	session->split_stash_cnt = session->split_stash_alloc = 0;
}

/*
 * __split_safe_free --
 *	Free a buffer if we can be sure no thread is accessing it, or schedule
 *	it to be freed otherwise.
 */
static int
__split_safe_free(WT_SESSION_IMPL *session, int exclusive, void *p, size_t s)
{
	/*
	 * We have swapped something in a page: if we don't have exclusive
	 * access, check whether there are other threads in the same tree.
	 */
	if (!exclusive &&
	    __split_oldest_gen(session) == S2C(session)->split_gen + 1)
		exclusive = 1;

	if (exclusive) {
		__wt_free(session, p);
		return (0);
	}

	return (__split_stash_add(session, p, s));
}

/*
 * __split_should_deepen --
 *	Return if we should deepen the tree.
 */
static int
__split_should_deepen(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_PAGE_INDEX *pindex;

	/*
	 * Splits are based on either the number of child pages that will be
	 * created by the split (splitting an internal page that will be slow
	 * to search), or by the memory footprint of the parent page (avoiding
	 * an internal page that will eat up all of the cache and put eviction
	 * pressure on the system).
	 */
	pindex = WT_INTL_INDEX_COPY(page);

	/*
	 * Deepen the tree if the page's memory footprint is larger than the
	 * maximum size for a page in memory.  We need an absolute minimum
	 * number of entries in order to split the page: if there is a single
	 * huge key, splitting won't help.
	 */
	if (page->memory_footprint > S2BT(session)->maxmempage &&
	    pindex->entries >= __split_deepen_min_child)
		return (1);

	/*
	 * Deepen the tree if the page's memory footprint is at least N
	 * times the maximum internal page size chunk in the backing file and
	 * the split will result in at least N children in the newly created
	 * intermediate layer.
	 */
	if (page->memory_footprint >
	    __split_deepen_max_internal_image * S2BT(session)->maxintlpage &&
	    pindex->entries >=
	    (__split_deepen_per_child * __split_deepen_split_child))
		return (1);

	return (0);
}

/*
 * __split_ovfl_key_cleanup --
 *	Handle cleanup for on-page row-store overflow keys.
 */
static int
__split_ovfl_key_cleanup(WT_SESSION_IMPL *session, WT_PAGE *page, WT_REF *ref)
{
	WT_CELL *cell;
	WT_CELL_UNPACK kpack;
	WT_IKEY *ikey;
	uint32_t cell_offset;

	/*
	 * A key being discarded (page split) or moved to a different page (page
	 * deepening) may be an on-page overflow key.  Clear any reference to an
	 * underlying disk image, and, if the key hasn't been deleted, delete it
	 * along with any backing blocks.
	 */
	if ((ikey = __wt_ref_key_instantiated(ref)) == NULL)
		return (0);
	if ((cell_offset = ikey->cell_offset) == 0)
		return (0);

	/* Leak blocks rather than try this twice. */
	ikey->cell_offset = 0;

	cell = WT_PAGE_REF_OFFSET(page, cell_offset);
	__wt_cell_unpack(cell, &kpack);
	if (kpack.ovfl && kpack.raw != WT_CELL_KEY_OVFL_RM)
		WT_RET(__wt_ovfl_discard(session, cell));

	return (0);
}

/*
 * __split_ref_instantiate --
 *	Instantiate key/address pairs in memory in service of a split.
 */
static int
__split_ref_instantiate(WT_SESSION_IMPL *session,
    WT_PAGE *page, WT_REF *ref, size_t *parent_decrp, size_t *child_incrp)
{
	WT_ADDR *addr;
	WT_CELL_UNPACK unpack;
	WT_DECL_RET;
	WT_IKEY *ikey;
	size_t size;
	void *key;

	/*
	 * Instantiate row-store keys, and column- and row-store addresses in
	 * the WT_REF structures referenced by a page that's being split (and
	 * deepening the tree).  The WT_REF structures aren't moving, but the
	 * index references are moving from the page we're splitting to a set
	 * of child pages, and so we can no longer reference the block image
	 * that remains with the page being split.
	 *
	 * Track how much memory the parent is losing and the child gaining.
	 *
	 * No locking is required to update the WT_REF structure because we're
	 * the only thread splitting the parent page, and there's no way for
	 * readers to race with our updates of single pointers.  The changes
	 * have to be written before the page goes away, of course, our caller
	 * owns that problem.
	 *
	 * Row-store keys, first.
	 */
	if (page->type == WT_PAGE_ROW_INT) {
		if ((ikey = __wt_ref_key_instantiated(ref)) == NULL) {
			__wt_ref_key(page, ref, &key, &size);
			WT_RET(__wt_row_ikey(session, 0, key, size, &ikey));
			ref->key.ikey = ikey;
		} else {
			WT_RET(__split_ovfl_key_cleanup(session, page, ref));
			WT_MEMSIZE_ADD(*parent_decrp,
			    sizeof(WT_IKEY) + ikey->size);
		}
		WT_MEMSIZE_ADD(*child_incrp, sizeof(WT_IKEY) + ikey->size);
	}

	/*
	 * If there's no address (the page has never been written), or the
	 * address has been instantiated, there's no work to do.  Otherwise,
	 * get the address from the on-page cell.
	 */
	if ((addr = ref->addr) == NULL)
		return (0);
	if (__wt_off_page(page, addr))
		WT_MEMSIZE_TRANSFER(*parent_decrp, *child_incrp,
		    sizeof(WT_ADDR) + addr->size);
	else {
		__wt_cell_unpack((WT_CELL *)ref->addr, &unpack);
		WT_RET(__wt_calloc_def(session, 1, &addr));
		if ((ret = __wt_strndup(
		    session, unpack.data, unpack.size, &addr->addr)) != 0) {
			__wt_free(session, addr);
			return (ret);
		}
		addr->size = (uint8_t)unpack.size;
		addr->type =
		    unpack.raw == WT_CELL_ADDR_INT ? WT_ADDR_INT : WT_ADDR_LEAF;
		ref->addr = addr;
		WT_MEMSIZE_ADD(*child_incrp, sizeof(WT_ADDR) + addr->size);
	}
	return (0);
}

#ifdef HAVE_DIAGNOSTIC
/*
 * __split_verify_intl_key_order --
 *	Verify the key order on an internal page after a split, diagnostic only.
 */
static void
__split_verify_intl_key_order(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_BTREE *btree;
	WT_ITEM *next, _next, *last, _last, *tmp;
	WT_REF *ref;
	uint64_t recno;
	int cmp, first;

	btree = S2BT(session);

	switch (page->type) {
	case WT_PAGE_COL_INT:
		recno = 0;
		WT_INTL_FOREACH_BEGIN(session, page, ref) {
			WT_ASSERT(session, ref->key.recno > recno);
			recno = ref->key.recno;
		} WT_INTL_FOREACH_END;
		break;
	case WT_PAGE_ROW_INT:
		next = &_next;
		WT_CLEAR(_next);
		last = &_last;
		WT_CLEAR(_last);

		first = 1;
		WT_INTL_FOREACH_BEGIN(session, page, ref) {
			__wt_ref_key(page, ref, &next->data, &next->size);
			if (last->size == 0) {
				if (first)
					first = 0;
				else {
					WT_ASSERT(session, __wt_compare(
					    session, btree->collator, last,
					    next, &cmp) == 0);
					WT_ASSERT(session, cmp < 0);
				}
			}
			tmp = last;
			last = next;
			next = tmp;
		} WT_INTL_FOREACH_END;
		break;
	}
}
#endif

/*
 * __split_deepen --
 *	Split an internal page in-memory, deepening the tree.
 */
static int
__split_deepen(WT_SESSION_IMPL *session, WT_PAGE *parent)
{
	WT_DECL_RET;
	WT_PAGE *child;
	WT_PAGE_INDEX *alloc_index, *child_pindex, *pindex;
	WT_REF **alloc_refp;
	WT_REF *child_ref, **child_refp, *parent_ref, **parent_refp, *ref;
	size_t child_incr, parent_decr, parent_incr, size;
	uint32_t children, chunk, i, j, remain, slots;
	int panic;
	void *p;

	alloc_index = NULL;
	parent_incr = parent_decr = 0;
	panic = 0;

	pindex = WT_INTL_INDEX_COPY(parent);

	/*
	 * Create N children, unless we are dealing with a large page without
	 * many entries, in which case split into the minimum number of pages.
	 */
	children = WT_MAX(pindex->entries / __split_deepen_per_child,
	    __split_deepen_min_child);

	WT_STAT_FAST_CONN_INCR(session, cache_eviction_deepen);
	WT_ERR(__wt_verbose(session, WT_VERB_SPLIT,
	    "%p: %" PRIu32 " elements, splitting into %" PRIu32 " children",
	    parent, pindex->entries, children));

	/*
	 * If the workload is prepending/appending to the tree, we could deepen
	 * without bound.  Don't let that happen, keep the first/last pages of
	 * the tree at their current level.
	 *
	 * XXX
	 * To improve this, we could track which pages were last merged into
	 * this page by eviction, and leave those pages alone, to prevent any
	 * sustained insert into the tree from deepening a single location.
	 */
#undef	SPLIT_CORRECT_1
#define	SPLIT_CORRECT_1	1		/* First page correction */
#undef	SPLIT_CORRECT_2
#define	SPLIT_CORRECT_2	2		/* First/last page correction */

	/*
	 * Allocate a new WT_PAGE_INDEX and set of WT_REF objects.  Initialize
	 * the first/last slots of the allocated WT_PAGE_INDEX to point to the
	 * first/last pages we're keeping at the current level, and the rest of
	 * the slots to point to new WT_REF objects.
	 */
	size = sizeof(WT_PAGE_INDEX) +
	    (children + SPLIT_CORRECT_2) * sizeof(WT_REF *);
	WT_ERR(__wt_calloc(session, 1, size, &alloc_index));
	WT_MEMSIZE_ADD(parent_incr, size);
	alloc_index->index = (WT_REF **)(alloc_index + 1);
	alloc_index->entries = children + SPLIT_CORRECT_2;
	alloc_index->index[0] = pindex->index[0];
	alloc_index->index[alloc_index->entries - 1] =
	    pindex->index[pindex->entries - 1];
	for (alloc_refp = alloc_index->index + SPLIT_CORRECT_1,
	    i = 0; i < children; ++alloc_refp, ++i) {
		WT_ERR(__wt_calloc_def(session, 1, alloc_refp));
		WT_MEMSIZE_ADD(parent_incr, sizeof(WT_REF));
	}

	/* Allocate child pages, and connect them into the new page index. */
	chunk = (pindex->entries - SPLIT_CORRECT_2) / children;
	remain = (pindex->entries - SPLIT_CORRECT_2) - chunk * (children - 1);
	for (parent_refp = pindex->index + SPLIT_CORRECT_1,
	    alloc_refp = alloc_index->index + SPLIT_CORRECT_1,
	    i = 0; i < children; ++i) {
		slots = i == children - 1 ? remain : chunk;
		WT_ERR(__wt_page_alloc(
		    session, parent->type, 0, slots, 0, &child));

		/*
		 * Initialize the parent page's child reference; we need a copy
		 * of the page's key.
		 */
		ref = *alloc_refp++;
		ref->home = parent;
		ref->page = child;
		ref->addr = NULL;
		if (parent->type == WT_PAGE_ROW_INT) {
			__wt_ref_key(parent, *parent_refp, &p, &size);
			WT_ERR(
			    __wt_row_ikey(session, 0, p, size, &ref->key.ikey));
			WT_MEMSIZE_ADD(parent_incr, sizeof(WT_IKEY) + size);
		} else
			ref->key.recno = (*parent_refp)->key.recno;
		ref->state = WT_REF_MEM;

		/* Initialize the child page. */
		if (parent->type == WT_PAGE_COL_INT)
			child->pg_intl_recno = (*parent_refp)->key.recno;
		child->pg_intl_parent_ref = ref;

		/* Mark it dirty. */
		WT_ERR(__wt_page_modify_init(session, child));
		__wt_page_only_modify_set(session, child);

		/*
		 * Once the split goes live, the newly created internal pages
		 * might be evicted and their WT_REF structures freed.  If those
		 * pages are evicted before threads exit the previous page index
		 * array, a thread might see a freed WT_REF.  Set the eviction
		 * transaction requirement for the newly created internal pages.
		 */
		child->modify->mod_split_txn = __wt_txn_new_id(session);

		/*
		 * The newly allocated child's page index references the same
		 * structures as the parent.  (We cannot move WT_REF structures,
		 * threads may be underneath us right now changing the structure
		 * state.)  However, if the WT_REF structures reference on-page
		 * information, we have to fix that, because the disk image for
		 * the page that has an page index entry for the WT_REF is about
		 * to change.
		 */
		child_incr = 0;
		child_pindex = WT_INTL_INDEX_COPY(child);
		for (child_refp = child_pindex->index, j = 0; j < slots; ++j) {
			WT_ERR(__split_ref_instantiate(session,
			    parent, *parent_refp, &parent_decr, &child_incr));
			*child_refp++ = *parent_refp++;

			WT_MEMSIZE_TRANSFER(
			    parent_decr, child_incr, sizeof(WT_REF));
		}
		__wt_cache_page_inmem_incr(session, child, child_incr);
	}
	WT_ASSERT(session, alloc_refp -
	    alloc_index->index == alloc_index->entries - SPLIT_CORRECT_1);
	WT_ASSERT(session,
	    parent_refp - pindex->index == pindex->entries - SPLIT_CORRECT_1);

	/*
	 * Update the parent's index; this is the update which splits the page,
	 * making the change visible to threads descending the tree.  From now
	 * on, we're committed to the split.  If any subsequent work fails, we
	 * have to panic because we potentially have threads of control using
	 * the new page index we just swapped in.
	 *
	 * A note on error handling: until this point, there's no problem with
	 * unwinding on error.  We allocated a new page index, a new set of
	 * WT_REFs and a new set of child pages -- if an error occurred, the
	 * parent remained unchanged, although it may have an incorrect memory
	 * footprint.  From now on we've modified the parent page, attention
	 * needs to be paid.
	 */
	WT_INTL_INDEX_SET(parent, alloc_index);
	panic = 1;

#ifdef HAVE_DIAGNOSTIC
	__split_verify_intl_key_order(session, parent);
#endif

	/*
	 * The moved reference structures now reference the wrong parent page,
	 * and we have to fix that up.  The problem is revealed when a thread
	 * of control searches for a page's reference structure slot, and fails
	 * to find it because the page it's searching no longer references it.
	 * When that failure happens, the thread waits for the reference's home
	 * page to be updated, which we do here: walk the children and fix them
	 * up.
	 *
	 * We're not acquiring hazard pointers on these pages, they cannot be
	 * evicted because of the eviction transaction value set above.
	 */
	for (parent_refp = alloc_index->index,
	    i = alloc_index->entries; i > 0; ++parent_refp, --i) {
		parent_ref = *parent_refp;
		WT_ASSERT(session, parent_ref->home == parent);
		if (parent_ref->state != WT_REF_MEM)
			continue;

		/*
		 * We left the first/last children of the parent at the current
		 * level to avoid bad split patterns, they might be leaf pages;
		 * check the page type before we continue.
		 */
		child = parent_ref->page;
		if (!WT_PAGE_IS_INTERNAL(child))
			continue;
#ifdef HAVE_DIAGNOSTIC
		__split_verify_intl_key_order(session, child);
#endif
		WT_INTL_FOREACH_BEGIN(session, child, child_ref) {
			/*
			 * The page's parent reference may not be wrong, as we
			 * opened up access from the top of the tree already,
			 * pages may have been read in since then.  Check and
			 * only update pages that reference the original page,
			 * they must be wrong.
			 */
			if (child_ref->home == parent) {
				child_ref->home = child;
				child_ref->ref_hint = 0;
			}
		} WT_INTL_FOREACH_END;
	}

	/*
	 * Push out the changes: not required for correctness, but don't let
	 * threads spin on incorrect page references longer than necessary.
	 */
	WT_FULL_BARRIER();
	alloc_index = NULL;

	/*
	 * We can't free the previous parent's index, there may be threads using
	 * it.  Add to the session's discard list, to be freed once we know no
	 * threads can still be using it.
	 *
	 * This change requires care with error handling: we have already
	 * updated the page with a new index.  Even if stashing the old value
	 * fails, we don't roll back that change, because threads may already
	 * be using the new index.
	 */
	size = sizeof(WT_PAGE_INDEX) + pindex->entries * sizeof(WT_REF *);
	WT_MEMSIZE_ADD(parent_decr, size);
	WT_ERR(__split_safe_free(session, 0, pindex, size));

	/*
	 * Adjust the parent's memory footprint.  This may look odd, but we
	 * have already taken the allocation overhead into account, and an
	 * increment followed by a decrement will cancel out the normal
	 * adjustment.
	 */
	__wt_cache_page_inmem_incr(session, parent, parent_incr);
	__wt_cache_page_inmem_decr(session, parent, parent_decr);

	if (0) {
err:		__wt_free_ref_index(session, parent, alloc_index, 1);

		/*
		 * If panic is set, we saw an error after opening up the tree
		 * to descent through the parent page's new index.  There is
		 * nothing we can do, the tree is inconsistent and there are
		 * threads potentially active in both versions of the tree.
		 */
		if (panic)
			ret = __wt_panic(session);
	}
	return (ret);
}

/*
 * __split_inmem_build --
 *	Instantiate a page in a multi-block set, when an update couldn't be
 * written.
 */
static int
__split_inmem_build(
    WT_SESSION_IMPL *session, WT_PAGE *orig, WT_REF *ref, WT_MULTI *multi)
{
	WT_CURSOR_BTREE cbt;
	WT_DECL_ITEM(key);
	WT_DECL_RET;
	WT_PAGE *page;
	WT_UPDATE *upd;
	WT_UPD_SKIPPED *skip;
	uint64_t recno;
	uint32_t i, slot;

	WT_CLEAR(cbt);
	cbt.iface.session = &session->iface;
	cbt.btree = S2BT(session);

	/*
	 * We can find unresolved updates when attempting to evict a page, which
	 * can't be written. This code re-creates the in-memory page and applies
	 * the unresolved updates to that page.
	 *
	 * Clear the disk image and link the page into the passed-in WT_REF to
	 * simplify error handling: our caller will not discard the disk image
	 * when discarding the original page, and our caller will discard the
	 * allocated page on error, when discarding the allocated WT_REF.
	 */
	WT_RET(__wt_page_inmem(
	    session, ref, multi->skip_dsk, WT_PAGE_DISK_ALLOC, &page));
	multi->skip_dsk = NULL;

	if (orig->type == WT_PAGE_ROW_LEAF)
		WT_RET(__wt_scr_alloc(session, 0, &key));

	/* Re-create each modification we couldn't write. */
	for (i = 0, skip = multi->skip; i < multi->skip_entries; ++i, ++skip)
		switch (orig->type) {
		case WT_PAGE_COL_FIX:
		case WT_PAGE_COL_VAR:
			/* Build a key. */
			upd = skip->ins->upd;
			skip->ins->upd = NULL;
			recno = WT_INSERT_RECNO(skip->ins);

			/* Search the page. */
			WT_ERR(__wt_col_search(session, recno, ref, &cbt));

			/* Apply the modification. */
			WT_ERR(__wt_col_modify(
			    session, &cbt, recno, NULL, upd, 0));
			break;
		case WT_PAGE_ROW_LEAF:
			/* Build a key. */
			if (skip->ins == NULL) {
				slot = WT_ROW_SLOT(orig, skip->rip);
				upd = orig->pg_row_upd[slot];
				orig->pg_row_upd[slot] = NULL;

				WT_ERR(__wt_row_leaf_key(
				    session, orig, skip->rip, key, 0));
			} else {
				upd = skip->ins->upd;
				skip->ins->upd = NULL;

				key->data = WT_INSERT_KEY(skip->ins);
				key->size = WT_INSERT_KEY_SIZE(skip->ins);
			}

			/* Search the page. */
			WT_ERR(__wt_row_search(session, key, ref, &cbt, 1));

			/* Apply the modification. */
			WT_ERR(
			    __wt_row_modify(session, &cbt, key, NULL, upd, 0));
			break;
		WT_ILLEGAL_VALUE_ERR(session);
		}

	/*
	 * We modified the page above, which will have set the first dirty
	 * transaction to the last transaction current running.  However, the
	 * updates we installed may be older than that.  Take the oldest active
	 * transaction ID to make sure these updates are not skipped by a
	 * checkpoint.
	 */
	page->modify->first_dirty_txn = S2C(session)->txn_global.oldest_id;

err:	__wt_scr_free(&key);
	/* Free any resources that may have been cached in the cursor. */
	WT_TRET(__wt_btcur_close(&cbt));
	return (ret);
}

/*
 * __wt_multi_to_ref --
 *	Move a multi-block list into an array of WT_REF structures.
 */
int
__wt_multi_to_ref(WT_SESSION_IMPL *session,
    WT_PAGE *page, WT_MULTI *multi, WT_REF **refp, size_t *incrp)
{
	WT_ADDR *addr;
	WT_IKEY *ikey;
	WT_REF *ref;
	size_t incr;

	addr = NULL;
	incr = 0;

	/* In some cases, the underlying WT_REF has not yet been allocated. */
	if (*refp == NULL) {
		WT_RET(__wt_calloc_def(session, 1, refp));
		WT_MEMSIZE_ADD(incr, sizeof(WT_REF));
	}
	ref = *refp;

	/*
	 * Any parent reference must be filled in by our caller; the primary
	 * use of this function is when splitting into a parent page, and we
	 * aren't holding any locks here that would allow us to know which
	 * parent we'll eventually split into, if the tree is simultaneously
	 * being deepened.
	 */
	ref->home = NULL;

	if (multi->skip == NULL) {
		/*
		 * Copy the address: we could simply take the buffer, but that
		 * would complicate error handling, freeing the reference array
		 * would have to avoid freeing the memory, and it's not worth
		 * the confusion.
		 */
		WT_RET(__wt_calloc_def(session, 1, &addr));
		WT_MEMSIZE_ADD(incr, sizeof(WT_ADDR));
		ref->addr = addr;
		addr->size = multi->addr.size;
		addr->type = multi->addr.type;
		WT_RET(__wt_strndup(session,
		    multi->addr.addr, addr->size, &addr->addr));
		/* Need a cast to avoid an implicit conversion warning. */
		WT_MEMSIZE_ADD(incr, addr->size);
	} else
		WT_RET(__split_inmem_build(session, page, ref, multi));

	switch (page->type) {
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		ikey = multi->key.ikey;
		WT_RET(__wt_row_ikey(session, 0,
		    WT_IKEY_DATA(ikey), ikey->size, &ref->key.ikey));
		WT_MEMSIZE_ADD(incr, sizeof(WT_IKEY) + ikey->size);
		break;
	default:
		ref->key.recno = multi->key.recno;
		break;
	}

	ref->state = multi->skip == NULL ? WT_REF_DISK : WT_REF_MEM;

	/*
	 * If our caller wants to track the memory allocations, we have a return
	 * reference.
	 */
	if (incrp != NULL)
		*incrp += incr;
	return (0);
}

/*
 * __split_evict_multi --
 *	Resolve a multi-page split, inserting new information into the parent.
 */
static int
__split_evict_multi(WT_SESSION_IMPL *session, WT_REF *ref, int exclusive)
{
	WT_DECL_RET;
	WT_IKEY *ikey;
	WT_PAGE *parent, *child;
	WT_PAGE_INDEX *alloc_index, *pindex;
	WT_PAGE_MODIFY *mod;
	WT_REF **alloc_refp, *parent_ref, ref_copy, **ref_tmp;
	size_t parent_decr, parent_incr, size;
	uint32_t i, j, parent_entries, result_entries, split_entries;
	int complete, hazard, locked;

	parent = NULL;			/* -Wconditional-uninitialized */
	alloc_index = NULL;
	parent_ref = NULL;
	ref_tmp = NULL;
	parent_decr = parent_incr = 0;
	complete = hazard = locked = 0;

	child = ref->page;
	mod = child->modify;

	/*
	 * Convert the split page's multiblock reconciliation information into
	 * an array of page reference structures.
	 */
	split_entries = mod->mod_multi_entries;
	WT_RET(__wt_calloc_def(session, split_entries, &ref_tmp));
	for (i = 0; i < split_entries; ++i)
		WT_ERR(__wt_multi_to_ref(session,
		    child, &mod->mod_multi[i], &ref_tmp[i], &parent_incr));

	/*
	 * Get a page-level lock on the parent to single-thread splits into the
	 * page because we need to single-thread sizing/growing the page index.
	 * It's OK to queue up multiple splits as the child pages split, but the
	 * actual split into the parent has to be serialized.  Note we allocate
	 * memory inside of the lock and may want to invest effort in making the
	 * locked period shorter.
	 *
	 * We could race with another thread deepening our parent.  To deal
	 * with that, read the parent pointer each time we try to lock it, and
	 * check that it's still correct after it is locked.
	 */
	for (;;) {
		parent = ref->home;
		F_CAS_ATOMIC(parent, WT_PAGE_SPLITTING, ret);
		if (ret == 0) {
			if (parent == ref->home)
				break;
			F_CLR_ATOMIC(parent, WT_PAGE_SPLITTING);
			continue;
		}
		__wt_yield();
	}
	locked = 1;

	/*
	 * We have exclusive access to split the parent, and at this point, the
	 * child prevents the parent from being evicted.  However, once we
	 * update the parent's index, it will no longer refer to the child, and
	 * could conceivably be evicted.  Get a hazard pointer on the parent
	 * now, so that we can safely access it after updating the index.
	 */
	if (!__wt_ref_is_root(parent_ref = parent->pg_intl_parent_ref)) {
		WT_ERR(__wt_page_in(session, parent_ref, WT_READ_NO_EVICT));
		hazard = 1;
	}

	pindex = WT_INTL_INDEX_COPY(parent);
	parent_entries = pindex->entries;
	result_entries = (parent_entries - 1) + split_entries;

	/*
	 * Allocate and initialize a new page index array for the parent, then
	 * copy references from the original index array, plus references from
	 * the newly created split array, into place.
	 */
	size = sizeof(WT_PAGE_INDEX) + result_entries * sizeof(WT_REF *);
	WT_ERR(__wt_calloc(session, 1, size, &alloc_index));
	WT_MEMSIZE_ADD(parent_incr, size);
	alloc_index->index = (WT_REF **)(alloc_index + 1);
	alloc_index->entries = result_entries;
	for (alloc_refp = alloc_index->index, i = 0; i < parent_entries; ++i)
		if (pindex->index[i] == ref)
			for (j = 0; j < split_entries; ++j) {
				ref_tmp[j]->home = parent;
				*alloc_refp++ = ref_tmp[j];

				/*
				 * Clear the split reference as it moves to the
				 * allocated page index, so it never appears on
				 * both after an error.
				 */
				ref_tmp[j] = NULL;
			}
		else
			*alloc_refp++ = pindex->index[i];
	__wt_free(session, ref_tmp);

	/*
	 * Update the parent page's index: this update makes the split visible
	 * to threads descending the tree.
	 */
	WT_INTL_INDEX_SET(parent, alloc_index);
	alloc_index = NULL;

#ifdef HAVE_DIAGNOSTIC
	__split_verify_intl_key_order(session, parent);
#endif

	/*
	 * Reset the page's original WT_REF field to split.  Threads cursoring
	 * through the tree were blocked because that WT_REF state was set to
	 * locked.  This update changes the locked state to split, unblocking
	 * those threads and causing them to re-calculate their position based
	 * on the updated parent page's index.
	 */
	WT_PUBLISH(ref->state, WT_REF_SPLIT);

	/*
	 * A note on error handling: failures before we swapped the new page
	 * index into the parent can be resolved by simply freeing allocated
	 * memory because the original page is unchanged, we can continue to
	 * use it and we have not yet modified the parent.  (See below for an
	 * exception, we cannot discard pages referencing unresolved changes.)
	 * Failures after we swap the new page index into the parent are also
	 * relatively benign because the split is OK and complete and the page
	 * is reset so it will be discarded by eviction.  For that reason, we
	 * mostly ignore further errors unless there's a panic.
	 */
	complete = 1;

	/*
	 * The previous parent page's key for this child page may have been an
	 * on-page overflow key.  In that case, if the key hasn't been deleted,
	 * delete it now, including its backing blocks.  We are exchanging the
	 * WT_REF that referenced it for the split page WT_REFs and their keys,
	 * and there's no longer any reference to it.  Done after completing the
	 * split (if we failed, we'd leak the underlying blocks, but the parent
	 * page would be unaffected).
	 */
	if (parent->type == WT_PAGE_ROW_INT)
		WT_TRET(__split_ovfl_key_cleanup(session, parent, ref));

	/*
	 * We can't free the previous page index, or the page's original WT_REF
	 * structure and instantiated key, there may be threads using them. Add
	 * them to the session discard list, to be freed once we know it's safe.
	 */
	size = sizeof(WT_PAGE_INDEX) + pindex->entries * sizeof(WT_REF *);
	WT_TRET(__split_safe_free(session, exclusive, pindex, size));
	WT_MEMSIZE_ADD(parent_decr, size);
	if (parent->type == WT_PAGE_ROW_INT &&
	    (ikey = __wt_ref_key_instantiated(ref)) != NULL) {
		size = sizeof(WT_IKEY) + ikey->size;
		WT_TRET(__split_safe_free(session, exclusive, ikey, size));
		WT_MEMSIZE_ADD(parent_decr, size);
	}
	/*
	 * Take a copy of the ref in case we can free it immediately: we still
	 * need to discard the page.
	 */
	ref_copy = *ref;
	WT_TRET(__split_safe_free(session, exclusive, ref, sizeof(WT_REF)));
	WT_MEMSIZE_ADD(parent_decr, sizeof(WT_REF));

	/*
	 * Adjust the parent's memory footprint.  This may look odd, but we
	 * have already taken the allocation overhead into account, and an
	 * increment followed by a decrement will cancel out the normal
	 * adjustment.
	 */
	__wt_cache_page_inmem_incr(session, parent, parent_incr);
	__wt_cache_page_inmem_decr(session, parent, parent_decr);

	WT_STAT_FAST_CONN_INCR(session, cache_eviction_split);
	WT_ERR(__wt_verbose(session, WT_VERB_SPLIT,
	    "%p: %s split into parent %p %" PRIu32 " -> %" PRIu32
	    " (%" PRIu32 ")",
	    child, __wt_page_type_string(child->type), parent, parent_entries,
	    result_entries, result_entries - parent_entries));

	/*
	 * Simple page splits trickle up the tree, that is, as leaf pages grow
	 * large enough and are evicted, they'll split into their parent.  And,
	 * as that parent grows large enough and is evicted, it will split into
	 * its parent and so on.  When the page split wave reaches the root,
	 * the tree will permanently deepen as multiple root pages are written.
	 *	However, this only helps if first, the pages are evicted (and
	 * we resist evicting internal pages for obvious reasons), and second,
	 * if the tree is closed and re-opened from a disk image, which may be
	 * a rare event.
	 *	To avoid the case of internal pages becoming too large when they
	 * aren't being evicted, check internal pages each time a leaf page is
	 * split into them.  If it's big enough, deepen the tree at that point.
	 *	Do the check here because we've just grown the parent page and
	 * are holding it locked.
	 */
	if (ret == 0 && !exclusive && __split_should_deepen(session, parent))
		ret = __split_deepen(session, parent);

err:	if (locked)
		F_CLR_ATOMIC(parent, WT_PAGE_SPLITTING);

	if (hazard)
		WT_TRET(__wt_hazard_clear(session, parent));

	/*
	 * Discard the child; test for split completion instead of errors, there
	 * might be a relatively innocuous error, and if we split the parent, we
	 * want to discard the child.
	 */
	if (complete) {
		/*
		 * Pages with unresolved changes are not marked clean during
		 * reconciliation, do it now.
		 */
		if (__wt_page_is_modified(child)) {
			 mod->write_gen = 0;
			 __wt_cache_dirty_decr(session, child);
		}
		__wt_ref_out(session, &ref_copy);
	}

	/*
	 * A note on error handling: in the case of evicting a page that has
	 * unresolved changes, we just instantiated some in-memory pages that
	 * reflect those unresolved changes.  The problem is those pages
	 * reference the same WT_UPDATE chains as the page we're splitting,
	 * that is, we simply copied references into the new pages.  If the
	 * split fails, the original page is fine, but discarding the created
	 * page would free those update chains, and that's wrong.  There isn't
	 * an easy solution, there's a lot of small memory allocations in some
	 * common code paths, and unwinding those changes will be difficult.
	 * For now, leak the memory by not discarding the instantiated pages.
	 */
	__wt_free_ref_index(session, NULL, alloc_index, 0);
	if (ref_tmp != NULL) {
		for (i = 0; i < split_entries; ++i)
			__wt_free_ref(session, child, ref_tmp[i], 0);
		__wt_free(session, ref_tmp);
	}

	/*
	 * A note on error handling: if we completed the split, return success,
	 * nothing really bad can have happened.
	 */
	return (ret == WT_PANIC || !complete ? ret : 0);
}

/*
 * __split_evict_single --
 *	Resolve a single page split, replacing a page with a new version.
 */
static int
__split_evict_single(WT_SESSION_IMPL *session, WT_REF *ref)
{
	WT_PAGE *page;
	WT_PAGE_MODIFY *mod;
	WT_REF new;

	page = ref->page;
	mod = page->modify;

	/* Build the new page. */
	memset(&new, 0, sizeof(new));
	WT_RET(__split_inmem_build(session, page, &new, &mod->mod_multi[0]));

	/*
	 * Discard the original page. Pages with unresolved changes are not
	 * marked clean during reconciliation, do it now.
	 */
	mod->write_gen = 0;
	__wt_cache_dirty_decr(session, page);
	__wt_page_out(session, &page);

	/* Swap the new page into place. */
	ref->page = new.page;
	WT_PUBLISH(ref->state, WT_REF_MEM);

	return (0);
}

/*
 * __wt_split_evict --
 *	Resolve a page split.
 */
int
__wt_split_evict(WT_SESSION_IMPL *session, WT_REF *ref, int exclusive)
{
	uint32_t split_entries;

	/*
	 * There are two cases entering this code. First, an in-memory page that
	 * got too large, we forcibly evicted it, and there wasn't anything to
	 * write. (Imagine two threads updating a small set keys on a leaf page.
	 * The page is too large so we try to evict it, but after reconciliation
	 * there's only a small amount of data (so it's a single page we can't
	 * split), and because there are two threads, there's some data we can't
	 * write (so we can't evict it). In that case, we take advantage of the
	 * fact we have exclusive access to the page and rewrite it in memory.)
	 *
	 * Second, a real split where we reconciled a page and it turned into a
	 * lot of pages.
	 */
	split_entries = ref->page->modify->mod_multi_entries;
	return (split_entries == 1 ?
	    __split_evict_single(session, ref) :
	    __split_evict_multi(session, ref, exclusive));
}
