/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"
/*
 * An in-memory page has a list of tracked blocks and overflow items we use for
 * three different tasks.  First, each tracked object has flag information set:
 *
 * WT_TRK_DISCARD	The object's backing blocks have been discarded.
 * WT_TRK_INUSE		The object is in-use.
 * WT_TRK_JUST_ADDED	The object was added in this reconciliation (and should
 *			be cleaned up, if reconciliation fails).
 * WT_TRK_OBJECT	Tracking slot is empty/not-empty.
 * WT_TRK_ONPAGE	The object is named on the original page, and we might
 *			encounter it every time we reconcile the page.
 * WT_TRK_OVFL_VALUE	The object is a cached, deleted overflow value and is
 *			ignored for general reconciliation purposes.
 * The tasks:
 *
 * Task #1:
 *	Free blocks when we're finished with them.  If a page reconciliation
 * results in a split, and then the page is reconciled again, the split pages
 * from the first reconciliation should be discarded.  These blocks are added
 * to the tracking list, and when reconciliation completes, they are discarded.
 * Normally, the slot is then cleared, but in a few cases, these blocks are
 * associated with the page, and we might encounter them each time the page
 * is reconciled.  In that case, the on-page flag is set, and the discard flag
 * will be set when the backing blocks are discarded, so subsequent page
 * reconciliations will realize the blocks have already been discarded.
 *
 * Task #2:
 *	Free overflow records when we're finished with them, similarly to the
 * blocks in task #1.  But, overflow records have an additional complication,
 * we want to re-use overflow records whenever possible.  For example, if an
 * overflow record is inserted, we allocate space and write it to the backing
 * file; we don't want to do that again every time the page is reconciled, we
 * want to re-use the overflow record each time we reconcile the page.  For
 * this we use the in-use flag.  When reconciliation starts, all of the tracked
 * overflow records have the "in-use" flag cleared.  As reconciliation proceeds,
 * every time we create an overflow item, we check our list of tracked objects
 * for a match.  If we find one we set the in-use flag and re-use the existing
 * record.  When reconciliation finishes, overflow records not marked in-use are
 * discarded.   The discard flags affect this, so we know an overflow record is
 * is discarded and can't be re-used in future reconciliations.
 *
 * Task #3:
 *	Cache deleted overflow values.  Sometimes we delete an overflow key or
 * record, and an older reader in the system still may need a copy.  If it's a
 * key, we instantiate the key in the cache; if it's a row-store value, we add
 * it to the end of the slot's WT_UPDATE list.  If it's a column-store value,
 * we need a place to stash it, and so we stash it in here.
 */

static int __track_dump(WT_SESSION_IMPL *, WT_PAGE *, const char *);
static int __track_msg(
	WT_SESSION_IMPL *, WT_PAGE *, const char *, WT_PAGE_TRACK *);

/*
 * __rec_track_extend --
 *	Extend the list of objects we're tracking
 */
static int
__rec_track_extend(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_PAGE_MODIFY *mod;
	size_t bytes_allocated, newlen;

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
	newlen = WT_MAX(2 * mod->track_entries, mod->track_entries + 20);
	WT_RET(__wt_realloc(session, &bytes_allocated,
	    newlen * sizeof(*mod->track), &mod->track));
	mod->track_entries = newlen;
	return (0);
}

/*
 * __wt_rec_track --
 *	Add an object to the page's list of tracked objects.
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
	if (!F_ISSET(track, WT_TRK_OVFL_VALUE))
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
	 * there are lots of them, in with case I guess the memory matters).
	 *
	 * If this reconciliation were to fail, we would reasonably perform the
	 * inverse operation in __wt_rec_track_wrapup_err.  I'm not bothering
	 * with that because we'd have to crack the structure itself to figure
	 * out how much to decrement and I don't think it's worth the effort.
	 * The potential problem is repeatedly failing reconciliation of a page
	 * with a large number of overflow items, which causes the page's memory
	 * memory footprint to become incorrectly high, causing us to push the
	 * page out of cache unnecessarily.  Like I said, not worth the effort.
	 */
	if (LF_ISSET(WT_TRK_ONPAGE))
		__wt_cache_page_inmem_incr(
		    session, page, addr_size + data_size);

	if (WT_VERBOSE_ISSET(session, reconcile))
		WT_RET(__track_msg(session, page, "add", track));
	return (0);
}

/*
 * __wt_rec_track_ovfl_srch --
 *	Search for a cached overflow object.
 */
int
__wt_rec_track_ovfl_srch(
    WT_PAGE *page, const uint8_t *addr, uint32_t addr_size, WT_ITEM *data)
{
	WT_PAGE_MODIFY *mod;
	WT_PAGE_TRACK *track;
	uint32_t i;

	mod = page->modify;
	for (track = mod->track, i = 0; i < mod->track_entries; ++track, ++i)
		if (F_ISSET(track, WT_TRK_OVFL_VALUE) &&
		    track->addr.size == addr_size &&
		    memcmp(addr, track->addr.addr, addr_size) == 0) {
			data->data = track->data;
			data->size = track->size;
			return (1);
		}
	return (0);
}

/*
 * __wt_rec_track_onpage_srch --
 *	Search for a permanently tracked object.
 */
int
__wt_rec_track_onpage_srch(
    WT_PAGE *page, const uint8_t *addr, uint32_t addr_size)
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
		    !F_ISSET(track, WT_TRK_OVFL_VALUE) &&
		    track->addr.size == addr_size &&
		    memcmp(addr, track->addr.addr, addr_size) == 0)
			return (1);
	}
	return (0);
}

/*
 * __wt_rec_track_onpage_addr --
 *	Search for a permanently tracked object (based on an addr/size pair),
 * and add it if it isn't already tracked.
 *
 * __wt_rec_track_onpage_ref --
 *	Search for a permanently tracked object (based on a page and ref),
 * and add it if it isn't already tracked.
 *
 * These functions are short-hand for "search the on-page records, and if the
 * address is not already listed as an object, add it".  Note there is no
 * possibility of object re-use, the object is discarded when reconciliation
 * completes.
 */
int
__wt_rec_track_onpage_addr(WT_SESSION_IMPL *session,
    WT_PAGE *page, const uint8_t *addr, uint32_t addr_size)
{
	if (__wt_rec_track_onpage_srch(page, addr, addr_size))
		return (0);

	return (__wt_rec_track(
	    session, page, addr, addr_size, NULL, 0, WT_TRK_ONPAGE));
}

int
__wt_rec_track_onpage_ref(
    WT_SESSION_IMPL *session, WT_PAGE *page, WT_PAGE *refpage, WT_REF *ref)
{
	uint32_t size;
	const uint8_t *addr;

	__wt_get_addr(refpage, ref, &addr, &size);
	return (__wt_rec_track_onpage_addr(session, page, addr, size));
}

/*
 * __wt_rec_track_ovfl_reuse --
 *	Search for a matching overflow record and reactivate it.
 */
int
__wt_rec_track_ovfl_reuse(
    WT_SESSION_IMPL *session, WT_PAGE *page,
    const void *data, uint32_t data_size,
    uint8_t **addrp, uint32_t *addr_sizep, int *foundp)
{
	WT_PAGE_MODIFY *mod;
	WT_PAGE_TRACK *track;
	uint32_t i;

	*foundp = 0;

	mod = page->modify;
	for (track = mod->track, i = 0; i < mod->track_entries; ++track, ++i) {
		/* Ignore empty slots */
		if (!F_ISSET(track, WT_TRK_OBJECT))
			continue;

		/*
		 * Ignore discarded objects, objects already in-use, or cached
		 * overflow values.  We don't care about whether or not the
		 * object came from a page, we can re-use objects from the page
		 * or objects created in a previous reconciliation.
		 */
		if (F_ISSET(track,
		    WT_TRK_DISCARD | WT_TRK_INUSE | WT_TRK_OVFL_VALUE))
			continue;

		/*
		 * Ignore objects without data (must be block objects).  This is
		 * not really necessary (presumably, our caller is matching on a
		 * non-zero-length data item), but paranoia is healthy.
		 */
		if (track->data == NULL)
			continue;

		/* Check to see if the data matches. */
		if (track->size != data_size ||
		    memcmp(data, track->data, data_size) != 0)
			continue;

		/*
		 * Reactivate the record.
		 * Return the block addr/size pair to our caller.
		 */
		F_SET(track, WT_TRK_INUSE);
		*addrp = track->addr.addr;
		*addr_sizep = track->addr.size;
		*foundp = 1;
		if (WT_VERBOSE_ISSET(session, reconcile))
			WT_RET(__track_msg(
			    session, page, "reactivate overflow", track));
		return (0);
	}
	return (0);
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

	bm = session->btree->bm;

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

		/* Ignore cached overflow values. */
		if (F_ISSET(track, WT_TRK_OVFL_VALUE))
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

	bm = session->btree->bm;

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
 *	Fill in a buffer, describing a track object.
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
	WT_APPEND_FLAG(WT_TRK_DISCARD, "discard");
	WT_APPEND_FLAG(WT_TRK_INUSE, "inuse");
	WT_APPEND_FLAG(WT_TRK_JUST_ADDED, "just-added");
	WT_APPEND_FLAG(WT_TRK_ONPAGE, "onpage");
	WT_APPEND_FLAG(WT_TRK_OVFL_VALUE, "ovfl-value");

	return (buf);
}
