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
 * the first split must be freed.  Additionally, created overflow objects (by
 * created, I mean built based on an update to a page, as opposed to taken
 * from the original disk image) may be re-created on each reconciliation of a
 * page, we need a way to find the previous write of the created overflow value
 * so we simply re-use those blocks and don't re-write the overflow value on
 * every reconciliation.  This code tracks those objects, these functions are
 * called from the routines in rec_write.c, which updates tracking information
 * each time a page is reconciled.
 *
 * An object has one of 4 types, plus there's a "slot not in use" type.  Each
 * type can also be associated with a "permanent" flag, which means the entry
 * can't be discarded when it's resolved, it has to remain in the table for as
 * long as the page is in cache.
 *
 * WT_TRK_EMPTY:
 *	Empty slot.
 *
 * WT_TRK_DISCARD:
 * WT_TRK_DISCARD_COMPLETE:
 *	A block being discarded has its type initially set to WT_TRK_DISCARD.
 * After the page is written, WT_TRK_DISCARD blocks are freed to the underlying
 * block manager.  If the entry is not permanent, the slot is then cleared,
 * otherwise the type is set to WT_TRK_DISCARD_COMPLETE.  That allows us to
 * find the block on the page's list again, but only physically free it once.
 *	There are a few cases where a discarded block has to be "permanent"
 * and remembered for the life of the page.  For example, an internal page
 * with an overflow key referencing a page that's empty or split: each time
 * the page is written, we'll figure out the key's overflow blocks are no
 * longer useful, but we have no way to know we've figured that same thing out
 * several times already.
 *	Additionally, in the case of discarded overflow keys we may have to
 * find them again in order to determine if an overflow key must be written.
 * If an overflow key references a deleted value during the reconciliation of
 * a row-store page, the overflow key would be discarded and underlying blocks
 * freed when the reconciliation completed.  If the value were to be replaced,
 * a subsequent reconciliation would write the key, referencing those original
 * overflow blocks, causing corruption.  To avoid this, when writing overflow
 * keys, we have to check if a previous reconciliation has discarded the key's
 * blocks, in which case we have to write the full key from scratch.  This is
 * painful, but not common.
 *	Additionally, in the case of overflow values on column-store pages, we
 * need a copy of the original value during each reconciliation for comparison
 * against other values.  If the original record is deleted, we still need to
 * know that fact, otherwise we'd attempt to retrieve it from the filesystem.
 *
 * WT_TRK_OVFL:
 * WT_TRK_OVFL_ACTIVE:
 *	The key facts about a created overflow record are first that it may be
 * re-used during subsequent reconciliations, and second the blocks must be
 * physically discarded if a reconciliation of the page does not re-use the
 * previously created overflow records.  (Note this is different from overflow
 * records that cannot be re-used, for example, row-store leaf overflow values:
 * they can only be deleted, and so the WT_TRK_DISCARD type is used instead of
 * WT_TRK_OVFL.  Column-store leaf overflow values are re-used in some cases,
 * and might use either the WT_TRK_DISCARD or WT_TRK_OVFL types.)
 *	An example of re-use is an inserted key/value pair where the value is
 * an overflow item.  The overflow record will be re-created as part of each
 * reconciliation.  We don't want to physically write the overflow record every
 * time, instead we track overflow records written on behalf of the page across
 * reconciliations.
 *	However, if a created overflow record is not re-used in reconciliation,
 * the physical blocks must be discarded to the block manager since they are no
 * longer in use.
 *	The type is first set to WT_TRK_OVFL_ACTIVE; after page reconciliation
 * completes, any records with a type of WT_TRK_OVFL are discarded, and records
 * with a type of WT_TRK_OVFL_ACTIVE are reset to WT_TRK_OVFL.
 *
 * In summary: if it's an overflow item we can re-use, it's WT_TRK_OVFL, and
 * as long it's re-used, the type will cycle between WT_TRK_OVFL_ACTIVE and
 * WT_TRK_OVFL.   If it's not reused, then it will be discarded and depending
 * on the "permanent" flag, cleared or retained as WT_TRK_DISCARD_COMPLETE.
 * If it's a block, it will start out as WT_TRK_DISCARD, and depending on the
 * "permanent" flag, cleared or retained as WT_TRK_DISCARD_COMPLETE.
 */

#ifdef HAVE_VERBOSE
static int __track_dump(WT_SESSION_IMPL *, WT_PAGE *, const char *);
static int __track_msg(WT_SESSION_IMPL *, WT_PAGE *, const char *, WT_ADDR *);
static int __track_print(WT_SESSION_IMPL *, WT_PAGE *, WT_PAGE_TRACK *);
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
 * __wt_rec_track_block --
 *	Add an addr/size pair to the page's list of tracked blocks.
 */
int
__wt_rec_track_block(WT_SESSION_IMPL *session,
    WT_PAGE *page, const uint8_t *addr, uint32_t size, int permanent)
{
	WT_PAGE_MODIFY *mod;
	WT_PAGE_TRACK *empty, *track;
	uint32_t i;

	mod = page->modify;

	empty = NULL;
	for (track = mod->track, i = 0; i < mod->track_entries; ++track, ++i)
		switch (WT_TRK_TYPE(track)) {
		case WT_TRK_EMPTY:
			empty = track;
			break;
		case WT_TRK_DISCARD:
		case WT_TRK_DISCARD_COMPLETE:
			/*
			 * We've discarded this block already, ignore it; the
			 * expected type is WT_TRK_DISCARD_COMPLETE (the block
			 * was discarded in a previous reconciliation), but if
			 * we decide to discard a block twice in some code, it
			 * isn't necessarily wrong.
			 */
			if (track->addr.size == size &&
			    memcmp(addr, track->addr.addr, size) == 0)
				return (0);
			break;
		case WT_TRK_OVFL:
		case WT_TRK_OVFL_ACTIVE:
			/*
			 * Checking other entry types would only be useful for
			 * diagnostics, if we find the address associated with
			 * another entry type, something has gone badly wrong.
			 */
			break;
		}

	/* Reallocate space as necessary. */
	if (empty == NULL) {
		WT_RET(__rec_track_extend(session, page));
		empty = &mod->track[mod->track_entries - 1];
	}

	track = empty;
	track->flags = WT_TRK_DISCARD;
	if (permanent)
		F_SET(track, WT_TRK_PERM);
	track->data = NULL;
	track->size = 0;
	WT_RET(__wt_strndup(session, (char *)addr, size, &track->addr.addr));
	track->addr.size = size;

	if (WT_VERBOSE_ISSET(session, reconcile))
		WT_RET(__track_print(session, page, track));
	return (0);
}

/*
 * __wt_rec_track_ovfl --
 *	Add an overflow object to the page's list of tracked objects.
 */
int
__wt_rec_track_ovfl(WT_SESSION_IMPL *session, WT_PAGE *page,
    const uint8_t *addr, uint32_t addr_size,
    const void *data, uint32_t data_size, uint32_t flags)
{
	WT_PAGE_MODIFY *mod;
	WT_PAGE_TRACK *empty, *track;
	uint8_t *p;
	uint32_t i;

	WT_ASSERT(session, addr != NULL);

	mod = page->modify;

	empty = NULL;
	for (track = mod->track, i = 0; i < mod->track_entries; ++track, ++i)
		if (track->flags == WT_TRK_EMPTY) {
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
	track->flags = (uint8_t)flags;
	track->addr.addr = p;
	track->addr.size = addr_size;
	memcpy(track->addr.addr, addr, addr_size);

	p += addr_size;
	track->data = p;
	track->size = data_size;
	memcpy(track->data, data, data_size);

	if (WT_VERBOSE_ISSET(session, reconcile))
		WT_RET(__track_print(session, page, track));
	return (0);
}

/*
 * __wt_rec_track_ovfl_reuse --
 *	Search for an overflow record and reactivate it.
 */
int
__wt_rec_track_ovfl_reuse(
    WT_SESSION_IMPL *session, WT_PAGE *page, const void *data,
    uint32_t size, uint8_t **addrp, uint32_t *sizep, int *foundp)
{
	WT_PAGE_TRACK *track;
	uint32_t i;

	WT_PAGE_MODIFY *mod;
	*foundp = 0;

	mod = page->modify;
	for (track = mod->track, i = 0; i < mod->track_entries; ++track, ++i) {
		/* Check for a match. */
		if (!F_ISSET(track, WT_TRK_OVFL) ||
		    size != track->size || memcmp(data, track->data, size) != 0)
			continue;

		/* Found a match, return the record to use. */
		F_CLR(track, WT_TRK_OVFL);
		F_SET(track, WT_TRK_OVFL_ACTIVE);

		/* Return the block addr/size pair to our caller. */
		*addrp = track->addr.addr;
		*sizep = track->addr.size;

		if (WT_VERBOSE_ISSET(session, reconcile))
			WT_RET(__track_msg(session,
			    page, "reactivate overflow", &track->addr));
		*foundp = 1;
		break;
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

	mod = page->modify;
	for (track = mod->track, i = 0; i < mod->track_entries; ++track, ++i)
		switch (WT_TRK_TYPE(track)) {
		case WT_TRK_DISCARD_COMPLETE:
		case WT_TRK_OVFL:
			/*
			 * This function is used to track overflow column-store
			 * values, review the calling code for more detail.
			 *
			 * If it's a WT_TRK_DISCARD_COMPLETE entry, we discarded
			 * the underlying overflow blocks, and the page is being
			 * reconciled after that discard.  We still copy out the
			 * original key to our caller, even though they should
			 * not need it (we get here because all of the keys on
			 * the page that originally referenced this item were
			 * updated, which means our caller's reconciliation loop
			 * won't need the original value).
			 *
			 * If it's a WT_TRK_OVFL entry, underlying blocks have
			 * not been discarded and are likely to be re-used, our
			 * caller requires a copy of the original key.
			 */
			if (track->addr.size == size &&
			    memcmp(addr, track->addr.addr, size) == 0) {
				WT_RET(__wt_buf_set(
				    session, copy, track->data, track->size));
				return (0);
			}
			break;
		case WT_TRK_DISCARD:
		case WT_TRK_EMPTY:
		case WT_TRK_OVFL_ACTIVE:
			/*
			 * Checking other entry types would only be useful for
			 * diagnostics, if we find the address associated with
			 * another entry type, something has gone badly wrong.
			 */
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

	if (WT_VERBOSE_ISSET(session, reconcile))
		WT_RET(__track_dump(session, page, "reconcile wrapup"));

	/*
	 * After a sync of a page, some of the objects we're tracking are no
	 * longer needed -- free what we can free.
	 */
	for (track = page->modify->track,
	    i = 0; i < page->modify->track_entries; ++track, ++i)
		switch (WT_TRK_TYPE(track)) {
		case WT_TRK_DISCARD_COMPLETE:
		case WT_TRK_EMPTY:
			continue;
		case WT_TRK_DISCARD:
		case WT_TRK_OVFL:
			if (WT_VERBOSE_ISSET(session, reconcile))
				WT_RET(__track_msg(session,
				    page, WT_TRK_TYPE(track) == WT_TRK_DISCARD ?
				    "discard block" : "inactive overflow",
				    &track->addr));

			/* We no longer need the underlying blocks. */
			WT_RET(__wt_bm_free(
			    session, track->addr.addr, track->addr.size));

			/*
			 * There are page and overflow blocks we track anew in
			 * each page reconciliation, we need to know about them
			 * even if the underlying blocks are no longer in use.
			 */
			if (F_ISSET(track, WT_TRK_PERM))
				track->flags = WT_TRK_DISCARD_COMPLETE;
			else {
				__wt_free(session, track->addr.addr);

				track->data = NULL;
				track->size = 0;
				track->addr.addr = NULL;
				track->addr.size = 0;
				track->flags = WT_TRK_EMPTY;
			}
			break;
		case WT_TRK_OVFL_ACTIVE:
			/* Reset to prepare for the next page reconciliation. */
			F_CLR(track, WT_TRK_OVFL_ACTIVE);
			F_SET(track, WT_TRK_OVFL);
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
static int
__track_dump(WT_SESSION_IMPL *session, WT_PAGE *page, const char *tag)
{
	WT_PAGE_MODIFY *mod;
	WT_PAGE_TRACK *track;
	uint32_t i;

	mod = page->modify;

	if (mod->track_entries == 0)
		return (0);

	WT_VERBOSE_RET(session,
	    reconcile, "page %p tracking list at %s:", page, tag);
	for (track = mod->track, i = 0; i < mod->track_entries; ++track, ++i)
		WT_RET(__track_print(session, page, track));
	return (0);
}

/*
 * __track_print --
 *	Display a tracked entry.
 */
static int
__track_print(WT_SESSION_IMPL *session, WT_PAGE *page, WT_PAGE_TRACK *track)
{
	WT_DECL_RET;

	switch (WT_TRK_TYPE(track)) {
	case WT_TRK_DISCARD:
		ret = __track_msg(session, page, "discard", &track->addr);
		break;
	case WT_TRK_DISCARD_COMPLETE:
		ret = __track_msg(
		    session, page, "discard-complete", &track->addr);
		break;
	case WT_TRK_OVFL:
		ret = __track_msg(session, page, "overflow", &track->addr);
		break;
	case WT_TRK_OVFL_ACTIVE:
		ret = __track_msg(
		    session, page, "overflow-active", &track->addr);
		break;
	case WT_TRK_EMPTY:
		ret = __track_msg(session, page, "empty", &track->addr);
		break;
	}
	return (ret);
}

/*
 * __track_msg --
 *	Output a verbose message and associated page and address pair.
 */
static int
__track_msg(
    WT_SESSION_IMPL *session, WT_PAGE *page, const char *msg, WT_ADDR *addr)
{
	WT_DECL_RET;
	WT_ITEM *buf;

	WT_RET(__wt_scr_alloc(session, 64, &buf));

	WT_VERBOSE_ERR(session, reconcile, "page %p %s %s", page, msg,
	    __wt_addr_string(session, buf, addr->addr, addr->size));

err:	__wt_scr_free(&buf);
	return (ret);
}
#endif
