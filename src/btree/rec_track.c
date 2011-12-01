/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static void __wt_rec_track_verbose(
		WT_SESSION_IMPL *, WT_PAGE *, WT_PAGE_TRACK *);

/*
 * A page in memory has a list of associated objects that are no longer in use.
 * For example, when an overflow item is modified, the original overflow blocks
 * must be freed at some point (I'm being deliberately vague as to when that
 * might happen, I'm sure it's going to change over the life of this software).
 * Or, when a page is split, then written again, the first split must be freed.
 * The routines in this file track those objects: they are generally called from
 * the routines in rec_write.c, which update the objects each time they write
 * the contents of a page.
 */

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
__wt_rec_track_block(
    WT_SESSION_IMPL *session, WT_PAGE *page, uint32_t addr, uint32_t size)
{
	WT_PAGE_MODIFY *mod;
	WT_PAGE_TRACK *track;
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
	for (track = mod->track, i = 0; i < mod->track_next; ++track, ++i)
		if (track->type == WT_PT_BLOCK &&
		    track->addr == addr && track->size == size)
			return (0);

	/* Reallocate space as necessary and track the new item. */
	if (mod->track_next == mod->track_entries)
		WT_RET(__rec_track_extend(session, page));

	track = &mod->track[mod->track_next++];
	track->type = WT_PT_BLOCK;
	track->data = NULL;
	track->len = 0;
	track->addr = addr;
	track->size = size;

#ifdef HAVE_VERBOSE
	__wt_rec_track_verbose(session, page, track);
#endif
	return (0);
}

/*
 * __wt_rec_track_ovfl --
 *	Add an overflow object to the page's list of tracked objects.
 */
int
__wt_rec_track_ovfl(WT_SESSION_IMPL *session, WT_PAGE *page,
    const void *data, uint32_t len, uint32_t addr, uint32_t size)
{
	WT_PAGE_MODIFY *mod;
	WT_PAGE_TRACK *track;
	uint8_t *p;

	mod = page->modify;

	/* Reallocate space as necessary and track the new item. */
	if (mod->track_next == mod->track_entries)
		WT_RET(__rec_track_extend(session, page));

	WT_RET(__wt_calloc_def(session, len, &p));
	memcpy(p, data, len);

	track = &mod->track[mod->track_next++];
	track->type = WT_PT_OVFL;
	track->data = p;
	track->len = len;
	track->addr = addr;
	track->size = size;

#ifdef HAVE_VERBOSE
	__wt_rec_track_verbose(session, page, track);
#endif
	return (0);
}

/*
 * __wt_rec_track_ovfl_reuse --
 *	Search for an overflow record and reactivate it.
 */
int
__wt_rec_track_ovfl_reuse(WT_SESSION_IMPL *session, WT_PAGE *page,
    const void *data, uint32_t len, uint32_t *addrp, uint32_t *sizep)
{
	WT_PAGE_TRACK *track;
	uint32_t i;

	WT_PAGE_MODIFY *mod;

	mod = page->modify;
	for (track = mod->track, i = 0; i < mod->track_next; ++track, ++i) {
		/* Check for a match. */
		if (track->type != WT_PT_OVFL_DISCARD ||
		    len != track->len || memcmp(data, track->data, len) != 0)
			continue;

		/* Found a match, return the record to use. */
		track->type = WT_PT_OVFL;

		/* Return the block addr/size pair to our caller. */
		*addrp = track->addr;
		*sizep = track->size;

		WT_VERBOSE(session, reconcile,
		    "page %p reactivate overflow %" PRIu32 "/%" PRIu32,
		    page, track->addr, track->size);
		return (1);
	}
	return (0);
}

/*
 * __wt_rec_track_ovfl_reset --
 *	Cleanup the tracking information each time we write a page.
 */
void
__wt_rec_track_ovfl_reset(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_PAGE_MODIFY *mod;
	WT_PAGE_TRACK *track;
	uint32_t i;

	mod = page->modify;

	/*
	 * Mark all overflow references "discarded" at the start of a page
	 * reconciliation: we'll reactivate ones we are using again as we
	 * process the page.
	 */
	for (track = mod->track, i = 0; i < mod->track_next; ++track, ++i)
		if (track->type == WT_PT_OVFL) {
			track->type = WT_PT_OVFL_DISCARD;
			WT_VERBOSE(session, reconcile,
			    "page %p set overflow OFF %" PRIu32 "/%" PRIu32,
			    page, track->addr, track->size);
		}
}

/*
 * __wt_rec_track_discard --
 *	Resolve the page's list of tracked objects.
 */
int
__wt_rec_track_discard(WT_SESSION_IMPL *session, WT_PAGE *page, int final)
{
	WT_PAGE_TRACK *track;
	uint32_t i;

	/*
	 * After a sync of a page, some of the objects we're tracking are no
	 * longer useful: discarded overflow items and tracked blocks can be
	 * discarded.
	 *
	 * When final is set (the page itself is being evicted), we no longer
	 * need to track the overflow items we're holding, but the underlying
	 * blocks are still inuse.
	 */
	for (track = page->modify->track,
	    i = 0; i < page->modify->track_next; ++track, ++i) {
		switch (track->type) {
		case WT_PT_EMPTY:
			continue;
		case WT_PT_BLOCK:
			WT_VERBOSE(session, reconcile,
			    "page %p discard block %" PRIu32 "/%" PRIu32,
			    page, track->addr, track->size);
			WT_RET(
			    __wt_block_free(session, track->addr, track->size));
			break;
		case WT_PT_OVFL:
			if (final)
				__wt_free(session, track->data);
			continue;
		case WT_PT_OVFL_DISCARD:
			__wt_free(session, track->data);
			WT_RET(
			    __wt_block_free(session, track->addr, track->size));
			WT_VERBOSE(session, reconcile,
			    "page %p discard overflow %" PRIu32 "/%" PRIu32,
			    page, track->addr, track->size);
			break;
		}

		track->type = WT_PT_EMPTY;
		track->data = NULL;
		track->len = 0;
		track->addr = WT_ADDR_INVALID;
		track->size = 0;
	}
	return (0);
}

#ifdef HAVE_VERBOSE
/*
 * __wt_rec_track_verbose --
 *	Display an entry being tracked.
 */
static void
__wt_rec_track_verbose(
    WT_SESSION_IMPL *session, WT_PAGE *page, WT_PAGE_TRACK *track)
{
	const char *onoff;

	switch (track->type) {
	case WT_PT_BLOCK:
		WT_VERBOSE(session, reconcile,
		    "page %p tracking block (%" PRIu32 "/%" PRIu32 ")",
		    page, track->addr, track->size);
		return;
	case WT_PT_OVFL:
		onoff = "ON";
		break;
	case WT_PT_OVFL_DISCARD:
		onoff = "OFF";
		break;
	case WT_PT_EMPTY:
	default:				/* Not possible. */
		return;
	}
	WT_VERBOSE(session, reconcile,
	    "page %p tracking overflow %s (%p, %" PRIu32 "/%" PRIu32 ")",
	    page, onoff, track->data, track->addr, track->size);
}
#endif
