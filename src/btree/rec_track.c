/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __ovfl_reuse_verbose --
 *	Dump information about a reuse overflow record.
 */
static int
__ovfl_reuse_verbose(WT_SESSION_IMPL *session,
    WT_PAGE *page, WT_OVFL_REUSE *reuse, const char *tag)
{
	WT_DECL_ITEM(tmp);
	WT_DECL_RET;

	WT_RET(__wt_scr_alloc(session, 64, &tmp));

	WT_VERBOSE_ERR(session, overflow,
	    "%s%s%p %s %s%s%s%s%s {%.*s}",
	    tag == NULL ? "" : tag,
	    tag == NULL ? "" : ": ",
	    page,
	    __wt_addr_string(
		session, tmp, WT_OVFL_REUSE_ADDR(reuse), reuse->addr_size),
	    F_ISSET(reuse, WT_OVFL_REUSE_INUSE) &&
	    F_ISSET(reuse, WT_OVFL_REUSE_JUST_ADDED) ? "(" : "",
	    F_ISSET(reuse, WT_OVFL_REUSE_INUSE) ? "inuse" : "",
	    F_ISSET(reuse, WT_OVFL_REUSE_INUSE) &&
	    F_ISSET(reuse, WT_OVFL_REUSE_JUST_ADDED) ? ", " : "",
	    F_ISSET(reuse, WT_OVFL_REUSE_JUST_ADDED) ? "added" : "",
	    F_ISSET(reuse, WT_OVFL_REUSE_INUSE) &&
	    F_ISSET(reuse, WT_OVFL_REUSE_JUST_ADDED) ? ")" : "",
	    WT_MIN(reuse->value_size, 40), (char *)WT_OVFL_REUSE_VALUE(reuse));

err:	__wt_scr_free(&tmp);
	return (ret);
}

#if 0
/*
 * __ovfl_reuse_dump --
 *	Debugging information.
 */
static void
__ovfl_reuse_dump(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_OVFL_REUSE *reuse;

	for (reuse =
	    page->modify->ovfl_reuse[0]; reuse != NULL; reuse = reuse->next[0])
		(void)__ovfl_reuse_verbose(session, page, reuse, "dump");
}
#endif

/*
 * __ovfl_reuse_skip_search --
 *	Return the first matching value in the overflow reuse list.
 */
static WT_OVFL_REUSE *
__ovfl_reuse_skip_search(
    WT_OVFL_REUSE **head, const void *value, uint32_t value_size)
{
	WT_OVFL_REUSE **e;
	size_t len;
	int cmp, i;

	/*
	 * Start at the highest skip level, then go as far as possible at each
	 * level before stepping down to the next.
	 */
	for (i = WT_SKIP_MAXDEPTH - 1, e = &head[i]; i >= 0;) {
		if (*e == NULL) {		/* Empty levels */
			--i;
			--e;
			continue;
		}

		/*
		 * Return any exact matches: we don't care in what search level
		 * we found a match.
		 */
		len = WT_MIN((*e)->value_size, value_size);
		cmp = memcmp(WT_OVFL_REUSE_VALUE(*e), value, len);
		if (cmp == 0 && (*e)->value_size == value_size)
			return (*e);

		/*
		 * If the skiplist value is larger than the search value, or
		 * they compare equally and the skiplist value is longer than
		 * the search value, drop down a level, otherwise continue on
		 * this level.
		 */
		if (cmp > 0 || (cmp == 0 && (*e)->value_size > value_size)) {
			--i;			/* Drop down a level */
			--e;
		} else				/* Keep going at this level */
			e = &(*e)->next[i];
	}
	return (NULL);
}

/*
 * __ovfl_reuse_skip_search_stack --
 *	 Search an overflow reuse skiplist, returning an insert/remove stack.
 */
static void
__ovfl_reuse_skip_search_stack(WT_OVFL_REUSE **head,
    WT_OVFL_REUSE ***stack, const void *value, uint32_t value_size)
{
	WT_OVFL_REUSE **e;
	size_t len;
	int cmp, i;

	/*
	 * Start at the highest skip level, then go as far as possible at each
	 * level before stepping down to the next.
	 */
	for (i = WT_SKIP_MAXDEPTH - 1, e = &head[i]; i >= 0;) {
		if (*e == NULL) {		/* Empty levels */
			stack[i--] = e--;
			continue;
		}

		/*
		 * If the skiplist value is larger than the search value, or
		 * they compare equally and the skiplist value is longer than
		 * the search value, drop down a level, otherwise continue on
		 * this level.
		 */
		len = WT_MIN((*e)->value_size, value_size);
		cmp = memcmp(WT_OVFL_REUSE_VALUE(*e), value, len);
		if (cmp > 0 || (cmp == 0 && (*e)->value_size > value_size))
			stack[i--] = e--;	/* Drop down a level */
		else
			e = &(*e)->next[i];	/* Keep going at this level */
	}
}

/*
 * __ovfl_reuse_wrapup --
 *	Resolve the page's overflow reuse list after a page is written.
 */
static int
__ovfl_reuse_wrapup(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_BM *bm;
	WT_OVFL_REUSE **e, **head, *reuse;
	size_t incr, decr;
	int i;

	bm = S2BT(session)->bm;
	head = page->modify->ovfl_reuse;

	/*
	 * Discard any overflow records that aren't in-use, freeing underlying
	 * blocks.
	 *
	 * First, walk the overflow reuse lists (except for the lowest one),
	 * fixing up skiplist links.
	 */
	for (i = WT_SKIP_MAXDEPTH - 1; i > 0; --i)
		for (e = &head[i]; *e != NULL;) {
			if (F_ISSET(*e, WT_OVFL_REUSE_INUSE)) {
				e = &(*e)->next[i];
				continue;
			}
			*e = (*e)->next[i];
		}

	/*
	 * Second, discard any overflow record without an in-use flag, clear
	 * the flags for the next run.
	 *
	 * As part of the pass through the lowest level, figure out how much
	 * space we added/subtracted from the page, and update its footprint.
	 * We don't get it exactly correct because we don't know the depth of
	 * the skiplist here, but it's close enough, and figuring out the
	 * memory footprint change in the reconciliation wrapup code means
	 * fewer atomic updates and less code overall.
	 */
	incr = decr = 0;
	for (e = &head[0]; *e != NULL;) {
		if (F_ISSET(*e, WT_OVFL_REUSE_INUSE)) {
			if (F_ISSET(*e, WT_OVFL_REUSE_JUST_ADDED))
				incr += sizeof(WT_OVFL_REUSE) +
				    2 * sizeof(WT_OVFL_REUSE *) +
				    (*e)->addr_size + (*e)->value_size;

			F_CLR(*e,
			    WT_OVFL_REUSE_INUSE | WT_OVFL_REUSE_JUST_ADDED);
			e = &(*e)->next[0];
			continue;
		}

		WT_ASSERT(session, !F_ISSET(*e, WT_OVFL_REUSE_JUST_ADDED));
		decr += sizeof(WT_OVFL_REUSE) +
		    2 * sizeof(WT_OVFL_REUSE *) +
		    (*e)->addr_size + (*e)->value_size;

		reuse = *e;
		*e = (*e)->next[0];

		if (WT_VERBOSE_ISSET(session, overflow))
			WT_RET(__ovfl_reuse_verbose(
			    session, page, reuse, "discard"));
		WT_RET(bm->free(
		    bm, session, WT_OVFL_REUSE_ADDR(reuse), reuse->addr_size));
		__wt_free(session, reuse);
	}

	if (incr > decr)
		__wt_cache_page_inmem_incr(session, page, incr - decr);
	if (decr > incr)
		__wt_cache_page_inmem_decr(session, page, decr - incr);
	return (0);
}

/*
 * __ovfl_reuse_wrapup_err --
 *	Resolve the page's overflow reuse list after an error occurs.
 */
static int
__ovfl_reuse_wrapup_err(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_BM *bm;
	WT_DECL_RET;
	WT_OVFL_REUSE **e, **head, *reuse;
	int i;

	bm = S2BT(session)->bm;
	head = page->modify->ovfl_reuse;

	/*
	 * Discard any overflow records that were just added, freeing underlying
	 * blocks.
	 *
	 * First, walk the overflow reuse lists (except for the lowest one),
	 * fixing up skiplist links.
	 */
	for (i = WT_SKIP_MAXDEPTH - 1; i > 0; --i)
		for (e = &head[i]; *e != NULL;) {
			if (!F_ISSET(*e, WT_OVFL_REUSE_JUST_ADDED)) {
				e = &(*e)->next[i];
				continue;
			}
			*e = (*e)->next[i];
		}

	/*
	 * Second, discard any overflow record with a just-added flag, clear the
	 * flags for the next run.
	 */
	for (e = &head[0]; *e != NULL;) {
		if (!F_ISSET(*e, WT_OVFL_REUSE_JUST_ADDED)) {
			F_CLR(*e, WT_OVFL_REUSE_INUSE);
			e = &(*e)->next[0];
			continue;
		}
		reuse = *e;
		*e = (*e)->next[0];

		if (WT_VERBOSE_ISSET(session, overflow))
			WT_RET(__ovfl_reuse_verbose(
			    session, page, reuse, "discard"));
		WT_TRET(bm->free(
		    bm, session, WT_OVFL_REUSE_ADDR(reuse), reuse->addr_size));
		__wt_free(session, reuse);
	}
	return (0);
}

/*
 * __wt_ovfl_reuse_add --
 *	Add a new entry to the page's list of overflow records tracked for
 * reuse.
 */
int
__wt_ovfl_reuse_add(WT_SESSION_IMPL *session, WT_PAGE *page,
    const uint8_t *addr, uint32_t addr_size,
    const void *value, uint32_t value_size)
{
	WT_OVFL_REUSE **head, *reuse, **stack[WT_SKIP_MAXDEPTH];
	size_t size;
	u_int i, skipdepth;
	uint8_t *p;

	head = page->modify->ovfl_reuse;

	/* Choose a skiplist depth for this insert. */
	skipdepth = __wt_skip_choose_depth();

	/*
	 * Allocate the WT_OVFL_REUSE structure, next pointers for the skip
	 * list, room for the address and value, then copy everything into
	 * place.
	 */
	size = sizeof(WT_OVFL_REUSE) +
	    skipdepth * sizeof(WT_OVFL_REUSE *) + addr_size + value_size;
	WT_RET(__wt_calloc(session, 1, size, &reuse));
	p = (uint8_t *)reuse +
	    sizeof(WT_OVFL_REUSE) + skipdepth * sizeof(WT_OVFL_REUSE *);
	reuse->addr_offset = WT_PTRDIFF32(p, reuse);
	reuse->addr_size = addr_size;
	memcpy(p, addr, addr_size);
	p += addr_size;
	reuse->value_offset = WT_PTRDIFF32(p, reuse);
	reuse->value_size = value_size;
	memcpy(p, value, value_size);
	F_SET(reuse, WT_OVFL_REUSE_INUSE | WT_OVFL_REUSE_JUST_ADDED);

	/* Insert the new entry into the skiplist. */
	__ovfl_reuse_skip_search_stack(head, stack, value, value_size);
	for (i = 0; i < skipdepth; ++i) {
		reuse->next[i] = *stack[i];
		*stack[i] = reuse;
	}

	if (WT_VERBOSE_ISSET(session, overflow))
		WT_RET(__ovfl_reuse_verbose(session, page, reuse, "add"));

	return (0);
}

/*
 * __wt_ovfl_reuse_srch --
 *	Search the page's list of overflow records tracked for a matching,
 * unused record, and return the address.
 */
int
__wt_ovfl_reuse_srch(WT_SESSION_IMPL *session, WT_PAGE *page,
    uint8_t **addrp, uint32_t *addr_sizep,
    const void *value, uint32_t value_size)
{
	WT_OVFL_REUSE **head, *reuse;

	*addrp = NULL;
	*addr_sizep = 0;

	head = page->modify->ovfl_reuse;

	/*
	 * The search function returns the first matching record in the list,
	 * which may be the first of many, overflow records may be identical.
	 * Look for one without the the in-use flag set, and put it back into
	 * service.
	 */
	if ((reuse = __ovfl_reuse_skip_search(head, value, value_size)) == NULL)
		return (0);
	do {
		if (!F_ISSET(reuse, WT_OVFL_REUSE_INUSE)) {
			*addrp = WT_OVFL_REUSE_ADDR(reuse);
			*addr_sizep = reuse->addr_size;
			F_SET(reuse, WT_OVFL_REUSE_INUSE);

			if (WT_VERBOSE_ISSET(session, overflow))
				WT_RET(__ovfl_reuse_verbose(
				    session, page, reuse, "reclaim"));
			return (1);
		}
	} while ((reuse = reuse->next[0]) != NULL &&
	    reuse->value_size == value_size &&
	    memcmp(WT_OVFL_REUSE_VALUE(reuse), value, value_size) == 0);

	return (0);
}

/*
 * An in-memory page has a list of tracked overflow records, where each record
 * has status information:
 *
 * WT_TRK_DISCARD	The object's backing blocks have been discarded.
 * WT_TRK_INUSE		The object is in-use.
 * WT_TRK_JUST_ADDED	The object was added in this reconciliation (and should
 *			be cleaned up, if reconciliation fails).
 * WT_TRK_OBJECT	Tracking slot is empty/not-empty.
 * WT_TRK_ONPAGE	The object is named on the original page, and we might
 *			encounter it every time we reconcile the page.
 *
 * We use this list for two things.
 * Task #1:
 *	Free overflow records when we're finished with them.  The complexity is
 * because we want to re-use overflow records whenever possible.  For example,
 * if an overflow record is inserted, we allocate space and write it to the
 * backing file; we don't want to do that work again every time the page is
 * reconciled, we want to re-use the overflow record each time we reconcile the
 * page.  For this we use the in-use flag: when reconciliation starts, all of
 * the tracked overflow records have their "in-use" flag cleared.  As
 * reconciliation proceeds, every time we are about to write an overflow item,
 * we check our list of tracked objects for a match.  If we find one, we set the
 * in-use flag and re-use the existing record.  When reconciliation finishes,
 * overflow records not marked in-use are discarded.   The discard flag affects
 * this, once an overflow record's backing blocks are discarded it can't ever
 * be re-used.
 *
 * Task #2:
 *	Cache deleted overflow values.  Sometimes we delete an overflow key or
 * record, and an older reader in the system still needs a copy.  If it's a
 * key, we can instantiate the key in the cache; if it's a row-store value, we
 * can add it to the end of the slot's WT_UPDATE list.  If it's a column-store
 * value, we need a place to stash it, and so we stash it in this list.  (Yes,
 * this is a nasty little bag-on-the-side, it was just too convenient to look
 * away.)  For this purpose, we've added an additional flag:
 *
 * WT_TRK_CACHE_DEL	The object is a cached, deleted overflow value and is
 *			ignored for general reconciliation purposes.
 */

static int __track_dump(WT_SESSION_IMPL *, WT_PAGE *, const char *);
static int __track_msg(
	WT_SESSION_IMPL *, WT_PAGE *, const char *, WT_PAGE_TRACK *);

/*
 * __rec_track_extend --
 *	Extend the page's list of tracked overflow records.
 */
static int
__rec_track_extend(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_PAGE_MODIFY *mod;
	size_t bytes_allocated;

	mod = page->modify;

	/*
	 * The __wt_realloc() function uses the "bytes allocated" value
	 * to figure out how much of the memory it needs to clear (see
	 * the function for an explanation of why the memory is cleared,
	 * it's a security thing).  We can calculate the bytes allocated
	 * so far, which saves a size_t in the WT_PAGE_MODIFY structure.
	 * That's worth a little dance, we have one of them per modified
	 * page.
	 */
	bytes_allocated = mod->track_entries * sizeof(*mod->track);
	WT_RET(__wt_realloc(session, &bytes_allocated,
	    (mod->track_entries + 20) * sizeof(*mod->track), &mod->track));
	mod->track_entries += 20;
	return (0);
}

/*
 * __wt_rec_track --
 *	Add an overflow record to the page's list of tracked overflow records.
 */
int
__wt_rec_track(WT_SESSION_IMPL *session, WT_PAGE *page,
    const uint8_t *addr, uint32_t addr_size,
    const void *data, uint32_t data_size, uint32_t flags)
{
	WT_PAGE_MODIFY *mod;
	WT_PAGE_TRACK *empty, *track;
	uint8_t *p;
	uint32_t i;

	mod = page->modify;

	/* Find an empty slot. */
	empty = NULL;
	for (track = mod->track, i = 0; i < mod->track_entries; ++track, ++i)
		if (!F_ISSET(track, WT_TRK_OBJECT)) {
			empty = track;
			break;
		}

	/* Reallocate space as necessary. */
	if (empty == NULL) {
		WT_RET(__rec_track_extend(session, page));
		empty = &mod->track[mod->track_entries - 1];
	}
	track = empty;

	/*
	 * Minor optimization: allocate a single chunk of space instead of two
	 * separate ones: be careful when it's freed.
	 */
	WT_RET(__wt_calloc_def(session, addr_size + data_size, &p));

	/*
	 * Set the just-added flag so we clean up should reconciliation fail,
	 * except for cached overflow values, which don't get discarded, even
	 * if reconciliation fails.
	 */
	track->flags = (uint8_t)flags | WT_TRK_OBJECT;
	if (!F_ISSET(track, WT_TRK_CACHE_DEL))
		F_SET(track, WT_TRK_JUST_ADDED);
	track->addr.addr = p;
	track->addr.size = addr_size;
	memcpy(track->addr.addr, addr, addr_size);
	if (data_size) {
		p += addr_size;
		track->data = p;
		track->size = data_size;
		memcpy(track->data, data, data_size);
	}

	/*
	 * Overflow items are potentially large and on-page items remain in the
	 * tracking list until the page is evicted.  If we're tracking a lot of
	 * them, their memory might matter: increment the page and cache memory
	 * totals.   This is unlikely to matter, but it's inexpensive (unless
	 * there are lots of them, in which case I guess the memory matters).
	 *
	 * If this reconciliation were to fail, we would reasonably perform the
	 * inverse operation in __wt_rec_track_wrapup_err.  I'm not bothering
	 * with that because we'd have to crack the structure itself to figure
	 * out how much to decrement and I don't think it's worth the effort.
	 * The potential problem is repeatedly failing reconciliation of a page
	 * with a large number of overflow items, which causes the page's memory
	 * memory footprint to become incorrectly high, causing us to push the
	 * page out of cache unnecessarily.  Like I said, not worth the effort.
	 *
	 * Ditto cache-deleted items, they're permanently in the cache until the
	 * page is discarded.
	 */
	if (LF_ISSET(WT_TRK_CACHE_DEL | WT_TRK_ONPAGE))
		__wt_cache_page_inmem_incr(
		    session, page, addr_size + data_size);

	if (WT_VERBOSE_ISSET(session, reconcile))
		WT_RET(__track_msg(session, page, "add", track));
	return (0);
}

/*
 * __wt_rec_track_cache_del_srch --
 *	Search the page's list of tracked overflow records for a specific,
 * cache-deleted value.
 */
int
__wt_rec_track_cache_del_srch(
    WT_PAGE *page, const uint8_t *addr, uint32_t addr_size, WT_ITEM *data)
{
	WT_PAGE_MODIFY *mod;
	WT_PAGE_TRACK *track;
	uint32_t i;

	mod = page->modify;
	for (track = mod->track, i = 0; i < mod->track_entries; ++track, ++i)
		if (F_ISSET(track, WT_TRK_CACHE_DEL) &&
		    track->addr.size == addr_size &&
		    memcmp(addr, track->addr.addr, addr_size) == 0) {
			data->data = track->data;
			data->size = track->size;
			return (1);
		}
	return (0);
}

/*
 * __wt_rec_track_srch --
 *	Search the page's list of tracked overflow records for a specific,
 * "on-page" value.
 */
int
__wt_rec_track_srch(WT_PAGE *page, const uint8_t *addr, uint32_t addr_size)
{
	WT_PAGE_MODIFY *mod;
	WT_PAGE_TRACK *track;
	uint32_t i;

	mod = page->modify;
	for (track = mod->track, i = 0; i < mod->track_entries; ++track, ++i) {
		/*
		 * Searching is always for objects referenced from the original
		 * page, and is only checking to see if the object's address
		 * matches the address we saved.
		 *
		 * It is possible for the address to appear multiple times in
		 * the list of tracked objects: if we discard an overflow item,
		 * for example, it can be re-allocated for use by the same page
		 * during a subsequent reconciliation, and would appear on the
		 * list of objects based on both the original slot allocated
		 * from an on-page review, and subsequently as entered during a
		 * block or overflow object allocation.  This can repeat, too,
		 * the only entry that can't be discarded is the original one
		 * from the page.
		 *
		 * We don't care if the object is currently in-use or not, just
		 * if it's there.
		 *
		 * Ignore cached overflow values and objects not loaded from a
		 * page, then check for an address match.
		 */
		if (F_ISSET(track, WT_TRK_ONPAGE) &&
		    track->addr.size == addr_size &&
		    memcmp(addr, track->addr.addr, addr_size) == 0)
			return (1);
	}
	return (0);
}

/*
 * __wt_rec_track_add --
 *	Search the page's list of tracked overflow records for a specific,
 * "on-page" value, and add it if it's not already there.
 */
int
__wt_rec_track_add(WT_SESSION_IMPL *session,
    WT_PAGE *page, const uint8_t *addr, uint32_t addr_size)
{
	if (__wt_rec_track_srch(page, addr, addr_size))
		return (0);

	/*
	 * Note there is no possibility of object re-use, the object is
	 * discarded when reconciliation completes.
	 */
	return (__wt_rec_track(
	    session, page, addr, addr_size, NULL, 0, WT_TRK_ONPAGE));
}

/*
 * __wt_rec_track_init --
 *	Initialize the page's list of tracked objects when reconciliation
 * starts.
 */
int
__wt_rec_track_init(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	if (WT_VERBOSE_ISSET(session, reconcile))
		WT_RET(__track_dump(session, page, "reconcile init"));
	return (0);
}

/*
 * __wt_rec_track_wrapup --
 *	Resolve the page's list of tracked objects after the page is written.
 */
int
__wt_rec_track_wrapup(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_BM *bm;
	WT_PAGE_MODIFY *mod;
	WT_PAGE_TRACK *track;
	uint32_t i;

	bm = S2BT(session)->bm;

	WT_RET(__ovfl_reuse_wrapup(session, page));

	if (WT_VERBOSE_ISSET(session, reconcile))
		WT_RET(__track_dump(session, page, "reconcile wrapup"));

	/*
	 * After the successful reconciliation of a page, some of the objects
	 * we're tracking are no longer needed, free what we can free.
	 */
	mod = page->modify;
	for (track = mod->track, i = 0; i < mod->track_entries; ++track, ++i) {
		/* Ignore empty slots */
		if (!F_ISSET(track, WT_TRK_OBJECT))
			continue;

		/* Ignore cache-deleted values. */
		if (F_ISSET(track, WT_TRK_CACHE_DEL))
			continue;

		/*
		 * Ignore discarded objects (discarded objects left on the list
		 * are never just-added, never in-use, and only include objects
		 * found on a page).
		 */
		if (F_ISSET(track, WT_TRK_DISCARD)) {
			WT_ASSERT(session,
			    !F_ISSET(track, WT_TRK_JUST_ADDED | WT_TRK_INUSE));
			WT_ASSERT(session, F_ISSET(track, WT_TRK_ONPAGE));
			continue;
		}

		/* Clear the just-added flag, reconciliation succeeded. */
		F_CLR(track, WT_TRK_JUST_ADDED);

		/*
		 * Ignore in-use objects, other than to clear the in-use flag
		 * in preparation for the next reconciliation.
		 */
		if (F_ISSET(track, WT_TRK_INUSE)) {
			F_CLR(track, WT_TRK_INUSE);
			continue;
		}

		/*
		 * The object isn't in-use and hasn't yet been discarded.  We
		 * no longer need the underlying blocks, discard them.
		 */
		if (WT_VERBOSE_ISSET(session, reconcile))
			WT_RET(__track_msg(session, page, "discard", track));
		WT_RET(
		    bm->free(bm, session, track->addr.addr, track->addr.size));

		/*
		 * There are page and overflow blocks we track anew as part of
		 * each page reconciliation, we need to know about them even if
		 * the underlying blocks are no longer in use.  If the object
		 * came from a page, keep it around.  Regardless, only discard
		 * objects once.
		 */
		if (F_ISSET(track, WT_TRK_ONPAGE)) {
			F_SET(track, WT_TRK_DISCARD);
			continue;
		}

		__wt_free(session, track->addr.addr);
		memset(track, 0, sizeof(*track));
	}
	return (0);
}

/*
 * __wt_rec_track_wrapup_err --
 *	Resolve the page's list of tracked objects after an error occurs.
 */
int
__wt_rec_track_wrapup_err(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_BM *bm;
	WT_DECL_RET;
	WT_PAGE_MODIFY *mod;
	WT_PAGE_TRACK *track;
	uint32_t i;

	bm = S2BT(session)->bm;

	ret = __ovfl_reuse_wrapup_err(session, page);

	/*
	 * After a failed reconciliation of a page, discard entries added in the
	 * current reconciliation, their information is incorrect, additionally,
	 * clear the in-use flag in preparation for the next reconciliation.
	 */
	mod = page->modify;
	for (track = mod->track, i = 0; i < mod->track_entries; ++track, ++i)
		if (F_ISSET(track, WT_TRK_JUST_ADDED)) {
			/*
			 * The in-use flag is used to avoid discarding backing
			 * blocks: if an object is both just-added and in-use,
			 * we allocated the blocks on this run, and we want to
			 * discard them on error.
			 */
			if (F_ISSET(track, WT_TRK_INUSE))
				WT_TRET(bm->free(bm, session,
				    track->addr.addr, track->addr.size));

			__wt_free(session, track->addr.addr);
			memset(track, 0, sizeof(*track));
		} else
			F_CLR(track, WT_TRK_INUSE);
	return (ret);
}

/*
 * __wt_rec_track_discard --
 *	Discard the page's list of tracked overflow records.
 */
void
__wt_rec_track_discard(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_PAGE_TRACK *track;
	uint32_t i;

	for (track = page->modify->track,
	    i = 0; i < page->modify->track_entries; ++track, ++i)
		__wt_free(session, track->addr.addr);
}

/*
 * __track_dump --
 *	Dump the page's list of tracked overflow records.
 */
static int
__track_dump(WT_SESSION_IMPL *session, WT_PAGE *page, const char *tag)
{
	WT_PAGE_MODIFY *mod;
	WT_PAGE_TRACK *track;
	uint32_t i;

	mod = page->modify;

	if (mod->track_entries == 0)
		return (0);

	WT_VERBOSE_RET(session, reconcile, "\n");
	WT_VERBOSE_RET(session,
	    reconcile, "page %p tracking list at %s:", page, tag);
	for (track = mod->track, i = 0; i < mod->track_entries; ++track, ++i)
		if (F_ISSET(track, WT_TRK_OBJECT))
			WT_RET(__track_msg(session, page, "dump", track));
	WT_VERBOSE_RET(session, reconcile, "\n");
	return (0);
}

/*
 * __track_msg --
 *	Output a verbose message and associated page and address pair.
 */
static int
__track_msg(WT_SESSION_IMPL *session,
    WT_PAGE *page, const char *msg, WT_PAGE_TRACK *track)
{
	WT_DECL_RET;
	WT_DECL_ITEM(buf);
	char f[64];

	WT_RET(__wt_scr_alloc(session, 64, &buf));

	WT_VERBOSE_ERR(
	    session, reconcile, "page %p %s (%s) %" PRIu32 "B @%s",
	    page, msg,
	    __wt_track_string(track, f, sizeof(f)),
	    track->size,
	    __wt_addr_string(session, buf, track->addr.addr, track->addr.size));

err:	__wt_scr_free(&buf);
	return (ret);
}

/*
 * __wt_track_string --
 *	Fill in a buffer, describing a tracked overflow record.
 */
char *
__wt_track_string(WT_PAGE_TRACK *track, char *buf, size_t len)
{
	size_t remain, wlen;
	char *p, *end;
	const char *sep;

	buf[0] = 0;

	p = buf;
	end = buf + len;

#define	WT_APPEND_FLAG(f, name)						\
	if (F_ISSET(track, f)) {					\
		remain = WT_PTRDIFF(end, p);				\
		wlen = (size_t)snprintf(p, remain, "%s%s", sep, name);	\
		p = wlen >= remain ? end : p + wlen;			\
		sep = ", ";						\
	}

	sep = "";
	WT_APPEND_FLAG(WT_TRK_CACHE_DEL, "cache-deleted");
	WT_APPEND_FLAG(WT_TRK_DISCARD, "discard");
	WT_APPEND_FLAG(WT_TRK_INUSE, "inuse");
	WT_APPEND_FLAG(WT_TRK_JUST_ADDED, "just-added");
	WT_APPEND_FLAG(WT_TRK_ONPAGE, "onpage");

	return (buf);
}
