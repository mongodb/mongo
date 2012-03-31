/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * A page in memory has a list of associated blocks and overflow items.  For
 * example, when an overflow item is modified, the original overflow blocks
 * must be freed at some point.  Or, when a page is split, then written again,
 * the first split must be freed.  This code tracks those objects: they are
 * called from the routines in rec_write.c, which update the objects each time
 * a page is reconciled.
 *
 * An object has one of 4 types, plus there's a "slot not in use" type.
 *
 * WT_PT_EMPTY:
 *	Empty slot.
 *
 * WT_PT_DISCARD:
 * WT_PT_DISCARD_COMPLETE:
 *	The key fact about a discarded block or overflow record is it may be
 * discarded multiple times.  For example, an internal page with an overflow
 * key referencing a page that's empty or split: each time a page is written,
 * we'll figure out the key's overflow blocks are no longer useful, but we have
 * no way to know we've figured that same thing out several times already.
 *	The type is initially set to WT_PT_DISCARD.  After the page is written,
 * WT_PT_DISCARD blocks are freed to the underlying block manager and the type
 * is set to WT_PT_DISCARD_COMPLETE.  That allows us to find the block on the
 * page's list again, but only physically free it once.
 *	Additionally, in the case of discarded overflow records we may have to
 * find them again in order to determine if an overflow key is needed again.
 * If an overflow key references a deleted value during the reconciliation of
 * a row-store page, the overflow key would be discarded and underlying blocks
 * freed when the reconciliation completed.  If the value were to be replaced,
 * a subsequent reconciliation would write the key, referencing those original
 * overflow blocks, causing corruption.  To avoid this, when writing overflow
 * keys, we have to check if a previous reconciliation has discarded the key's
 * blocks, in which case we have to write the full key from scratch.  This is
 * painful, but not common.
 *
 * WT_PT_OVFL:
 * WT_PT_OVFL_ACTIVE:
 *	The key facts about a created overflow record are first that it may be
 * re-used during subsequent reconciliations, and second the blocks must be
 * physically discarded if a reconciliation of the page does not re-use the
 * previously created overflow records.  (Note this is different from overflow
 * records that appeared on the on-disk version of the page: they can only be
 * deleted, not re-used, and so they are handled by the WT_PT_DISCARD type.)
 *	An example of re-use is an inserted key/value pair where the value is
 * an overflow item.  The overflow record will be re-created as part of each
 * reconciliation.  We don't want to physically write the overflow record every
 * time, instead we track overflow records written on behalf of the page across
 * reconciliations.
 *	However, if a created overflow record is not re-used in reconciliation,
 * the physical blocks must be discarded to the block manager since they are no
 * longer in use.
 *	The type is first set to WT_PT_OVFL_ACTIVE; after page reconciliation
 * completes, any records with a type of WT_PT_OVFL are discarded, and records
 * with a type of WT_PT_OVFL_ACTIVE are reset to WT_PT_OVFL.
 */

#ifdef HAVE_VERBOSE
static void __track_dump(WT_SESSION_IMPL *, WT_PAGE *, const char *);
static void __track_msg(WT_SESSION_IMPL *, WT_PAGE *, const char *, WT_ADDR *);
static void __track_print(WT_SESSION_IMPL *, WT_PAGE *, WT_PAGE_TRACK *);
#endif

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
 * __wt_rec_track_addr --
 *	Add an addr/size pair to the page's list of tracked objects.
 */
int
__wt_rec_track_addr(
    WT_SESSION_IMPL *session, WT_PAGE *page, const uint8_t *addr, uint32_t size)
{
	WT_PAGE_MODIFY *mod;
	WT_PAGE_TRACK *empty, *track;
	uint32_t i;

	mod = page->modify;

	empty = NULL;
	for (track = mod->track, i = 0; i < mod->track_entries; ++track, ++i)
		switch (track->type) {
		case WT_PT_EMPTY:
			empty = track;
			break;
		case WT_PT_DISCARD:
		case WT_PT_DISCARD_COMPLETE:
			/* We've discarded this block already, ignore. */
			if (track->addr.size == size &&
			    memcmp(addr, track->addr.addr, size) == 0)
				return (0);
			break;
		case WT_PT_OVFL:
		case WT_PT_OVFL_ACTIVE:
			break;
		}

	/* Reallocate space as necessary. */
	if (empty == NULL) {
		WT_RET(__rec_track_extend(session, page));
		empty = &mod->track[mod->track_entries - 1];
	}

	track = empty;
	track->type = WT_PT_DISCARD;
	track->data = NULL;
	track->size = 0;
	WT_RET(__wt_strndup(session, (char *)addr, size, &track->addr.addr));
	track->addr.size = size;

	WT_VERBOSE_CALL(
	    session, reconcile, __track_print(session, page, track));
	return (0);
}

/*
 * __wt_rec_track_addr_discarded --
 *	Search for a discarded block record; if the address references discarded
 * blocks it cannot be re-used.
 */
int
__wt_rec_track_addr_discarded(WT_PAGE *page, const uint8_t *addr, uint32_t size)
{
	WT_PAGE_MODIFY *mod;
	WT_PAGE_TRACK *track;
	uint32_t i;

	mod = page->modify;
	for (track = mod->track, i = 0; i < mod->track_entries; ++track, ++i)
		switch (track->type) {
		case WT_PT_DISCARD:
		case WT_PT_EMPTY:
		case WT_PT_OVFL:
		case WT_PT_OVFL_ACTIVE:
			break;
		case WT_PT_DISCARD_COMPLETE:
			/*
			 * Only check WT_PT_DISCARD_COMPLETE entries.
			 * Checking other entry types would only be useful for
			 * diagnostics, if we find the address associated with
			 * another entry type, something has gone badly wrong.
			 */
			if (track->addr.size == size &&
			    memcmp(addr, track->addr.addr, size) == 0)
				return (1);
			break;
		}
	return (0);
}

/*
 * __wt_rec_track_ovfl --
 *	Add an overflow object to the page's list of tracked objects.
 */
int
__wt_rec_track_ovfl(WT_SESSION_IMPL *session, WT_PAGE *page,
    const uint8_t *addr, uint32_t addr_size,
    const void *data, uint32_t data_size)
{
	WT_PAGE_MODIFY *mod;
	WT_PAGE_TRACK *empty, *track;
	uint8_t *p;
	uint32_t i;

	WT_ASSERT(session, addr != NULL);

	mod = page->modify;

	empty = NULL;
	for (track = mod->track, i = 0; i < mod->track_entries; ++track, ++i)
		if (track->type == WT_PT_EMPTY) {
			empty = track;
			break;
		}

	/* Reallocate space as necessary. */
	if (empty == NULL) {
		WT_RET(__rec_track_extend(session, page));
		empty = &mod->track[mod->track_entries - 1];
	}

	/*
	 * Minor optimization: allocate a single chunk of space instead of two
	 * separate ones: be careful when it's freed.
	 */
	WT_RET(__wt_calloc_def(session, addr_size + data_size, &p));

	track = empty;
	track->type = WT_PT_OVFL_ACTIVE;
	track->addr.addr = p;
	track->addr.size = addr_size;
	memcpy(track->addr.addr, addr, addr_size);

	p += addr_size;
	track->data = p;
	track->size = data_size;
	memcpy(track->data, data, data_size);

	WT_VERBOSE_CALL(
	    session, reconcile, __track_print(session, page, track));
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
		if (track->type != WT_PT_OVFL ||
		    size != track->size || memcmp(data, track->data, size) != 0)
			continue;

		/* Found a match, return the record to use. */
		track->type = WT_PT_OVFL_ACTIVE;

		/* Return the block addr/size pair to our caller. */
		*addrp = track->addr.addr;
		*sizep = track->addr.size;

		WT_VERBOSE_CALL(session, reconcile, __track_msg(
		    session, page, "reactivate overflow", &track->addr));
		return (1);
	}
	return (0);
}

/*
 * __wt_rec_track_ovfl_srch --
 *	Search for an overflow record and return a copy if it's entered.
 */
int
__wt_rec_track_ovfl_srch(WT_SESSION_IMPL *session,
    WT_PAGE *page, const uint8_t *addr, uint32_t size, WT_ITEM *copy)
{
	WT_PAGE_MODIFY *mod;
	WT_PAGE_TRACK *track;
	uint32_t i;

	copy->size = 0;				/* By default, not found. */

	mod = page->modify;
	for (track = mod->track, i = 0; i < mod->track_entries; ++track, ++i)
		switch (track->type) {
		case WT_PT_DISCARD:
		case WT_PT_EMPTY:
		case WT_PT_OVFL_ACTIVE:
			break;
		case WT_PT_DISCARD_COMPLETE:
		case WT_PT_OVFL:
			/*
			 * This function is used to track overflow column-store
			 * values, there are special considerations, review the
			 * calling code for more detail.
			 *
			 * Check WT_PT_DISCARD_COMPLETE and WT_PT_OVFL entries.
			 * If it's a WT_PT_DISCARD_COMPLETE entry, we discarded
			 * the underlying overflow blocks, and the page is being
			 * reconciled after that discard.  We still copy out the
			 * original key to our caller, even though they should
			 * not need it (we get here because all of the keys on
			 * the page that originally referenced this item were
			 * updated, which means our callers reconciliation loop
			 * won't need the original value).  If it's a WT_PT_OVFL
			 * entry, the underlying blocks have not been discarded
			 * and are likely to be re-used -- our caller definitely
			 * needs a copy of the original key.
			 *
			 * Checking other entry types would only be useful for
			 * diagnostics, if we find the address associated with
			 * another entry type, something has gone badly wrong.
			 */
			if (track->addr.size == size &&
			    memcmp(addr, track->addr.addr, size) == 0) {
				WT_RET(__wt_buf_set(
				    session, copy, track->data, track->size));
				return (0);
			}
			break;
		}
	return (0);
}

/*
 * __wt_rec_track_wrapup --
 *	Resolve the page's list of tracked objects after the page is written.
 */
int
__wt_rec_track_wrapup(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_PAGE_TRACK *track;
	uint32_t i;

	WT_VERBOSE_CALL(session, reconcile,
	    __track_dump(session, page, "reconcile wrapup"));

	/*
	 * After a sync of a page, some of the objects we're tracking are no
	 * longer needed -- free what we can free.
	 */
	for (track = page->modify->track,
	    i = 0; i < page->modify->track_entries; ++track, ++i)
		switch (track->type) {
		case WT_PT_EMPTY:
		case WT_PT_DISCARD_COMPLETE:
			continue;
		case WT_PT_DISCARD:
		case WT_PT_OVFL:
			WT_VERBOSE_CALL(session, reconcile, __track_msg(
			    session, page, "discard block", &track->addr));

			/* We no longer need these blocks. */
			WT_RET(__wt_bm_free(
			    session, track->addr.addr, track->addr.size));

			/*
			 * There are page and overflow blocks we track anew as
			 * part of each reconciliation, we need to know about
			 * them so we don't free them more than once.  Set the
			 * type -- we no longer care if it's an overflow item
			 * or not, we just care that the slot exists.
			 */
			track->type = WT_PT_DISCARD_COMPLETE;
			break;
		case WT_PT_OVFL_ACTIVE:
			/* Reset the type to prepare for the next reconcile. */
			track->type = WT_PT_OVFL;
			break;
		}
	return (0);
}

/*
 * __wt_rec_track_discard --
 *	Discard the page's list of tracked objects.
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

#ifdef HAVE_VERBOSE
/*
 * __track_dump --
 *	Dump the list of tracked objects.
 */
static void
__track_dump(WT_SESSION_IMPL *session, WT_PAGE *page, const char *tag)
{
	WT_PAGE_MODIFY *mod;
	WT_PAGE_TRACK *track;
	uint32_t i;

	mod = page->modify;

	if (mod->track_entries == 0)
		return;

	WT_VERBOSE(session,
	    reconcile, "page %p tracking list at %s:", page, tag);
	for (track = mod->track, i = 0; i < mod->track_entries; ++track, ++i)
		__track_print(session, page, track);
}

/*
 * __track_print --
 *	Display a tracked entry.
 */
static void
__track_print(WT_SESSION_IMPL *session, WT_PAGE *page, WT_PAGE_TRACK *track)
{
	switch (track->type) {
	case WT_PT_DISCARD:
		__track_msg(session, page, "discard", &track->addr);
		break;
	case WT_PT_DISCARD_COMPLETE:
		__track_msg(session, page, "discard-complete", &track->addr);
		break;
	case WT_PT_OVFL:
		__track_msg(session, page, "overflow", &track->addr);
		break;
	case WT_PT_OVFL_ACTIVE:
		__track_msg(session, page, "overflow-active", &track->addr);
		break;
	case WT_PT_EMPTY:
	default:				/* Not possible. */
		break;
	}
}

/*
 * __track_msg --
 *	Output a verbose message and associated page and address pair.
 */
static void
__track_msg(
    WT_SESSION_IMPL *session, WT_PAGE *page, const char *msg, WT_ADDR *addr)
{
	WT_ITEM *buf;

	if (__wt_scr_alloc(session, 64, &buf))
		return;
	WT_VERBOSE(session, reconcile, "page %p %s %s", page, msg,
	    __wt_addr_string(session, buf, addr->addr, addr->size));
	__wt_scr_free(&buf);
}
#endif
