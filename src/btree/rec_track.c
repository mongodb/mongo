/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * A page in memory has a list of associated blocks and overflow items.  For
 * example, when an overflow item is modified, the original overflow blocks
 * must be freed at some point.  Or, when a page is split, then written again,
 * the first split must be freed.  This code tracks those objects: they are
 * generally called from the routines in rec_write.c, which update the objects
 * each time a page is reconciled.
 */

#ifdef HAVE_VERBOSE
static void __track_msg(WT_SESSION_IMPL *, WT_PAGE *, const char *, WT_ADDR *);
static void __track_print(WT_SESSION_IMPL *, WT_PAGE *, WT_PAGE_TRACK *);
#endif

/*
 * __rec_track_clear --
 *	Clear a track entry.
 */
static inline void
__rec_track_clear(WT_PAGE_TRACK *track)
{
	track->type = WT_PT_EMPTY;
	track->data = NULL;
	track->size = 0;
	track->addr.addr = NULL;
	track->addr.size = 0;
}

/*
 * __rec_track_extend --
 *	Extend the list of objects we're tracking
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
 * __wt_rec_track_block --
 *	Add an addr/size pair to the page's list of tracked objects.
 */
int
__wt_rec_track_block(WT_SESSION_IMPL *session,
    __wt_pt_type_t type, WT_PAGE *page, const uint8_t *addr, uint32_t size)
{
	WT_PAGE_MODIFY *mod;
	WT_PAGE_TRACK *next, *track;
	uint32_t i;

	mod = page->modify;

	/*
	 * There may be multiple requests to track a single block. For example,
	 * an internal page with an overflow key that references a page that's
	 * split: every time the page is written, we'll figure out the key's
	 * overflow pages are no longer useful because the underlying page has
	 * split, but we have no way to know that we've figured that same thing
	 * out several times already.   Check for duplicates.
	 */
	next = NULL;
	for (track = mod->track, i = 0; i < mod->track_entries; ++track, ++i) {
		if (track->type == WT_PT_EMPTY) {
			next = track;
			continue;
		}
		if (track->type == type &&
		    track->addr.size == size &&
		    memcmp(addr, track->addr.addr, size) == 0)
			return (0);
	}

	/* Reallocate space as necessary. */
	if (next == NULL) {
		WT_RET(__rec_track_extend(session, page));
		next = &mod->track[mod->track_entries - 1];
	}

	track = next;
	track->type = type;
	track->data = NULL;
	track->size = 0;
	WT_RET(__wt_strndup(session, (char *)addr, size, &track->addr.addr));
	track->addr.size = size;

#ifdef HAVE_VERBOSE
	__track_print(session, page, track);
#endif
	return (0);
}

/*
 * __wt_rec_track_ovfl --
 *	Add an overflow object to the page's list of tracked objects.
 */
int
__wt_rec_track_ovfl(WT_SESSION_IMPL *session, WT_PAGE *page,
    uint8_t *addr, uint32_t addr_size, const void *data, uint32_t data_size)
{
	WT_PAGE_MODIFY *mod;
	WT_PAGE_TRACK *next, *track;
	uint8_t *p;
	uint32_t i;

	WT_ASSERT(session, addr != NULL);

	mod = page->modify;

	next = NULL;
	for (track = mod->track, i = 0; i < mod->track_entries; ++track, ++i)
		if (track->type == WT_PT_EMPTY) {
			next = track;
			break;
		}

	/* Reallocate space as necessary. */
	if (next == NULL) {
		WT_RET(__rec_track_extend(session, page));
		next = &mod->track[mod->track_entries - 1];
	}

	/*
	 * Minor optimization: allocate a single chunk of space instead of two
	 * separate ones: be careful when it's freed.
	 */
	WT_RET(__wt_calloc_def(session, addr_size + data_size, &p));

	track = next;
	track->type = WT_PT_OVFL;
	track->addr.addr = p;
	track->addr.size = addr_size;
	memcpy(track->addr.addr, addr, addr_size);

	p += addr_size;
	track->data = p;
	track->size = data_size;
	memcpy(track->data, data, data_size);

#ifdef HAVE_VERBOSE
	__track_print(session, page, track);
#endif
	return (0);
}

/*
 * __wt_rec_track_ovfl_reuse --
 *	Search for an overflow record and reactivate it.
 */
int
__wt_rec_track_ovfl_reuse(WT_SESSION_IMPL *session, WT_PAGE *page,
    const void *data, uint32_t size, uint8_t **addrp, uint32_t *sizep)
{
	WT_PAGE_TRACK *track;
	uint32_t i;

	WT_PAGE_MODIFY *mod;

	mod = page->modify;
	for (track = mod->track, i = 0; i < mod->track_entries; ++track, ++i) {
		/* Check for a match. */
		if (track->type != WT_PT_OVFL_DISCARD ||
		    size != track->size || memcmp(data, track->data, size) != 0)
			continue;

		/* Found a match, return the record to use. */
		track->type = WT_PT_OVFL;

		/* Return the block addr/size pair to our caller. */
		*addrp = track->addr.addr;
		*sizep = track->addr.size;

		__track_msg(session, page, "reactivate overflow", &track->addr);
		return (1);
	}
	return (0);
}

/*
 * __wt_rec_track_init --
 *	Initialize/Reset the tracking information when writing a page.
 */
void
__wt_rec_track_init(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_PAGE_MODIFY *mod;
	WT_PAGE_TRACK *track;
	uint32_t i;

	mod = page->modify;

	/*
	 * Mark all overflow references "discarded" at the start of a page
	 * reconciliation: we'll reactivate ones we are using again as we
	 * process the page.
	 *
	 * Discard any blocks intended to be discarded on eviction, we are
	 * not evicting based on the last reconciliation, they're no longer
	 * interesting.
	 */
	for (track = mod->track, i = 0; i < mod->track_entries; ++track, ++i)
		switch (track->type) {
		case WT_PT_BLOCK:
			WT_ASSERT(session, track->type != WT_PT_BLOCK);
			break;
		case WT_PT_BLOCK_EVICT:
			/*
			 * We had a block we would have discarded, had the last
			 * reconciliation been the final one used to evict the
			 * page -- it wasn't, and we didn't.  Clear the slot.
			 */
			__rec_track_clear(track);
			break;
		case WT_PT_OVFL:
			__track_msg(
			    session, page, "set overflow OFF", &track->addr);
			track->type = WT_PT_OVFL_DISCARD;
			break;
		case WT_PT_OVFL_DISCARD:
			WT_ASSERT(session, track->type != WT_PT_BLOCK);
			break;
		case WT_PT_EMPTY:
			break;
		}
}

/*
 * __wt_rec_track_wrapup --
 *	Temporarily/Permanently resolve the page's list of tracked objects.
 */
int
__wt_rec_track_wrapup(WT_SESSION_IMPL *session, WT_PAGE *page, int final)
{
	WT_PAGE_TRACK *track;
	uint32_t i;

	/*
	 * After a sync of a page, some of the objects we're tracking are no
	 * longer needed -- free what we can free.
	 *
	 * WT_PT_EMPTY:
	 *	Empty slot.
	 * WT_PT_BLOCK:
	 *	A discarded block, free when this reconciliation completes.
	 * WT_PT_BLOCK_EVICT:
	 *	A discarded block based on this reconciliation; if the page is
	 *	evicted based on this reconciliation, discard the block.  (For
	 *	example, an overflow key that references a deleted item will be
	 *	discarded, but a subsequent reconciliation might find the key
	 *	is once more in use because the item is no longer deleted.)
	 * WT_PT_OVFL:
	 *	An overflow record that's in-use.  Ignored after any particular
	 *	reconciliation, because we need to track it for re-use in future
	 *	reconciliations.   When the page is evicted, discard its memory,
	 *	leaving the underlying blocks alone.
	 * WT_PT_OVFL_DISCARD:
	 *	An overflow record that's no longer in-use.  Discard the memory
	 *	and free the underlying blocks after reconciliation completes.
	 */
	for (track = page->modify->track,
	    i = 0; i < page->modify->track_entries; ++track, ++i) {
		switch (track->type) {
		case WT_PT_EMPTY:
			continue;
		case WT_PT_BLOCK_EVICT:
			if (!final)
				continue;
			/* FALLTHROUGH */
		case WT_PT_BLOCK:
			__track_msg(
			    session, page, "discard block", &track->addr);
			WT_RET(__wt_bm_free(
			    session, track->addr.addr, track->addr.size));
			__wt_free(session, track->addr.addr);
			break;
		case WT_PT_OVFL:
			__track_msg(
			    session, page, "retain overflow", &track->addr);
			if (!final)
				continue;

			/* Freeing WT_PAGE_TRACK->addr frees ->data, too. */
			__wt_free(session, track->addr.addr);
			break;
		case WT_PT_OVFL_DISCARD:
			__track_msg(
			    session, page, "discard overflow", &track->addr);
			WT_RET(__wt_bm_free(
			    session, track->addr.addr, track->addr.size));

			/* Freeing WT_PAGE_TRACK->addr frees ->data, too. */
			__wt_free(session, track->addr.addr);
			break;
		}

		__rec_track_clear(track);
	}
	return (0);
}

#ifdef HAVE_VERBOSE
/*
 * __track_msg --
 *	Output a verbose message and associated page and address pair.
 */
static void
__track_msg(
    WT_SESSION_IMPL *session, WT_PAGE *page, const char *msg, WT_ADDR *addr)
{
	WT_BUF *buf;

	if (!WT_VERBOSE_ISSET(session, reconcile))
		return;
	if (__wt_scr_alloc(session, 64, &buf))
		return;
	WT_VERBOSE(session, reconcile, "page %p %s %s", page, msg,
	    __wt_addr_string(session, buf, addr->addr, addr->size));
	__wt_scr_free(&buf);
}

/*
 * __track_print --
 *	Display a tracked entry.
 */
static void
__track_print(WT_SESSION_IMPL *session, WT_PAGE *page, WT_PAGE_TRACK *track)
{
	switch (track->type) {
	case WT_PT_BLOCK:
	case WT_PT_BLOCK_EVICT:
		__track_msg(session, page, "tracking block", &track->addr);
		return;
	case WT_PT_OVFL:
		__track_msg(
		    session, page, "tracking overflow ON", &track->addr);
		break;
	case WT_PT_OVFL_DISCARD:
		__track_msg(
		    session, page, "tracking overflow OFF", &track->addr);
		break;
	case WT_PT_EMPTY:
	default:				/* Not possible. */
		break;
	}
}
#endif
