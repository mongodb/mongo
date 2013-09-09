/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __ovfl_track_init --
 *	Initialize the overflow tracking structure.
 */
static int
__ovfl_track_init(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	return (__wt_calloc_def(session, 1, &page->modify->ovfl_track));
}

/*
 * __ovfl_onpage_verbose --
 *	Dump information about a onpage overflow record.
 */
static int
__ovfl_onpage_verbose(WT_SESSION_IMPL *session,
    WT_PAGE *page, WT_OVFL_ONPAGE *onpage, const char *tag)
{
	WT_DECL_ITEM(tmp);
	WT_DECL_RET;

	WT_RET(__wt_scr_alloc(session, 64, &tmp));

	WT_VERBOSE_ERR(session, overflow,
	    "onpage: %s%s%p %s (%s)",
	    tag == NULL ? "" : tag,
	    tag == NULL ? "" : ": ",
	    page,
	    __wt_addr_string(
		session, tmp, WT_OVFL_ONPAGE_ADDR(onpage), onpage->addr_size),
	    F_ISSET(onpage, WT_OVFL_ONPAGE_JUST_ADDED) ? "just-added" : "");

err:	__wt_scr_free(&tmp);
	return (ret);
}

#if 0
/*
 * __ovfl_onpage_dump --
 *	Debugging information.
 */
static void
__ovfl_onpage_dump(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_OVFL_ONPAGE **head, *onpage;

	head = page->modify->ovfl_track->ovfl_onpage;

	for (onpage = head[0]; onpage != NULL; onpage = onpage->next[0])
		(void)__ovfl_onpage_verbose(session, page, onpage, "dump");
}
#endif

/*
 * __ovfl_onpage_skip_search --
 *	Return the first matching address in the overflow onpage list.
 */
static WT_OVFL_ONPAGE *
__ovfl_onpage_skip_search(
    WT_OVFL_ONPAGE **head, const void *addr, uint32_t addr_size)
{
	WT_OVFL_ONPAGE **e;
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
		len = WT_MIN((*e)->addr_size, addr_size);
		cmp = memcmp(WT_OVFL_ONPAGE_ADDR(*e), addr, len);
		if (cmp == 0 && (*e)->addr_size == addr_size)
			return (*e);

		/*
		 * If the skiplist address is larger than the search address, or
		 * they compare equally and the skiplist address is longer than
		 * the search address, drop down a level, otherwise continue on
		 * this level.
		 */
		if (cmp > 0 || (cmp == 0 && (*e)->addr_size > addr_size)) {
			--i;			/* Drop down a level */
			--e;
		} else				/* Keep going at this level */
			e = &(*e)->next[i];
	}
	return (NULL);
}

/*
 * __ovfl_onpage_skip_search_stack --
 *	 Search an overflow onpage skiplist, returning an insert/remove stack.
 */
static void
__ovfl_onpage_skip_search_stack(WT_OVFL_ONPAGE **head,
    WT_OVFL_ONPAGE ***stack, const void *addr, uint32_t addr_size)
{
	WT_OVFL_ONPAGE **e;
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
		 * If the skiplist addr is larger than the search addr, or
		 * they compare equally and the skiplist addr is longer than
		 * the search addr, drop down a level, otherwise continue on
		 * this level.
		 */
		len = WT_MIN((*e)->addr_size, addr_size);
		cmp = memcmp(WT_OVFL_ONPAGE_ADDR(*e), addr, len);
		if (cmp > 0 || (cmp == 0 && (*e)->addr_size > addr_size))
			stack[i--] = e--;	/* Drop down a level */
		else
			e = &(*e)->next[i];	/* Keep going at this level */
	}
}

/*
 * __ovfl_onpage_wrapup --
 *	Resolve the page's overflow onpage list after a page is written.
 */
static int
__ovfl_onpage_wrapup(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_BM *bm;
	WT_OVFL_ONPAGE **e, **head, *onpage;
	size_t incr;

	bm = S2BT(session)->bm;
	head = page->modify->ovfl_track->ovfl_onpage;

	/*
	 * Free the underlying blocks for any newly added overflow records.
	 *
	 * As part of the pass through the lowest level, figure out how much
	 * space we added/subtracted from the page, and update its footprint.
	 * We don't get it exactly correct because we don't know the depth of
	 * the skiplist here, but it's close enough, and figuring out the
	 * memory footprint change in the reconciliation wrapup code means
	 * fewer atomic updates and less code overall.
	 */
	incr = 0;
	for (e = &head[0]; (onpage = *e) != NULL; e = &(*e)->next[0])
		if (F_ISSET(onpage, WT_OVFL_ONPAGE_JUST_ADDED)) {
			F_CLR(onpage, WT_OVFL_ONPAGE_JUST_ADDED);

			incr += sizeof(WT_OVFL_ONPAGE) +
			    2 * sizeof(WT_OVFL_ONPAGE *) + onpage->addr_size;

			if (WT_VERBOSE_ISSET(session, overflow))
				WT_RET(__ovfl_onpage_verbose(
				    session, page, onpage, "free"));
			WT_RET(bm->free(bm, session,
			    WT_OVFL_ONPAGE_ADDR(onpage), onpage->addr_size));
		}

	if (incr != 0)
		__wt_cache_page_inmem_incr(session, page, incr);
	return (0);
}

/*
 * __ovfl_onpage_wrapup_err --
 *	Resolve the page's overflow onpage list after an error occurs.
 */
static int
__ovfl_onpage_wrapup_err(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_OVFL_ONPAGE **e, **head, *onpage;
	int i;

	head = page->modify->ovfl_track->ovfl_onpage;

	/*
	 * Discard any overflow records that were just added.
	 *
	 * First, walk the overflow onpage lists (except for the lowest one),
	 * fixing up skiplist links.
	 */
	for (i = WT_SKIP_MAXDEPTH - 1; i > 0; --i)
		for (e = &head[i]; *e != NULL;) {
			if (!F_ISSET(*e, WT_OVFL_ONPAGE_JUST_ADDED)) {
				e = &(*e)->next[i];
				continue;
			}
			*e = (*e)->next[i];
		}

	/* Second, discard any overflow record with a just-added flag. */
	for (e = &head[0]; (onpage = *e) != NULL;) {
		if (!F_ISSET(onpage, WT_OVFL_ONPAGE_JUST_ADDED)) {
			e = &(*e)->next[0];
			continue;
		}
		*e = (*e)->next[0];
		__wt_free(session, onpage);
	}
	return (0);
}

/*
 * __wt_ovfl_onpage_search --
 *	Return true/false if an address appears in the page's list of tracked
 * on-page overflow records.
 */
int
__wt_ovfl_onpage_search(WT_PAGE *page, const uint8_t *addr, uint32_t addr_size)
{
	WT_OVFL_ONPAGE **head;

	if (page->modify->ovfl_track == NULL)
		return (0);

	head = page->modify->ovfl_track->ovfl_onpage;

	return (
	    __ovfl_onpage_skip_search(head, addr, addr_size) == NULL ? 0 : 1);
}

/*
 * __wt_ovfl_onpage_add --
 *	Add a new entry to the page's list of onpage overflow records that have
 * been discarded.
 */
int
__wt_ovfl_onpage_add(WT_SESSION_IMPL *session,
    WT_PAGE *page, const uint8_t *addr, uint32_t addr_size)
{
	WT_OVFL_ONPAGE **head, *onpage, **stack[WT_SKIP_MAXDEPTH];
	size_t size;
	u_int i, skipdepth;
	uint8_t *p;

	if (page->modify->ovfl_track == NULL)
		WT_RET(__ovfl_track_init(session, page));

	head = page->modify->ovfl_track->ovfl_onpage;

	/* Check if the record already appears in the list. */
	if (__wt_ovfl_onpage_search(page, addr, addr_size))
		return (0);

	/* Choose a skiplist depth for this insert. */
	skipdepth = __wt_skip_choose_depth();

	/*
	 * Allocate the WT_OVFL_ONPAGE structure, next pointers for the skip
	 * list, room for the address, then copy everything into place.
	 *
	 * To minimize the WT_OVFL_ONPAGE structure size, the address offset
	 * and size are single bytes: that's safe because the address follows
	 * the structure (which can't be more than about 100B), and address
	 * cookies are limited to 255B.
	 */
	size = sizeof(WT_OVFL_ONPAGE) +
	    skipdepth * sizeof(WT_OVFL_ONPAGE *) + addr_size;
	WT_RET(__wt_calloc(session, 1, size, &onpage));
	p = (uint8_t *)onpage +
	    sizeof(WT_OVFL_ONPAGE) + skipdepth * sizeof(WT_OVFL_ONPAGE *);
	onpage->addr_offset = (uint8_t)WT_PTRDIFF(p, onpage);
	onpage->addr_size = (uint8_t)addr_size;
	memcpy(p, addr, addr_size);
	F_SET(onpage, WT_OVFL_ONPAGE_JUST_ADDED);

	/* Insert the new entry into the skiplist. */
	__ovfl_onpage_skip_search_stack(head, stack, addr, addr_size);
	for (i = 0; i < skipdepth; ++i) {
		onpage->next[i] = *stack[i];
		*stack[i] = onpage;
	}

	if (WT_VERBOSE_ISSET(session, overflow))
		WT_RET(__ovfl_onpage_verbose(session, page, onpage, "add"));

	return (0);
}

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
	    "reuse: %s%s%p %s (%s%s%s) {%.*s}",
	    tag == NULL ? "" : tag,
	    tag == NULL ? "" : ": ",
	    page,
	    __wt_addr_string(
		session, tmp, WT_OVFL_REUSE_ADDR(reuse), reuse->addr_size),
	    F_ISSET(reuse, WT_OVFL_REUSE_INUSE) ? "inuse" : "",
	    F_ISSET(reuse, WT_OVFL_REUSE_INUSE) &&
	    F_ISSET(reuse, WT_OVFL_REUSE_JUST_ADDED) ? ", " : "",
	    F_ISSET(reuse, WT_OVFL_REUSE_JUST_ADDED) ? "just-added" : "",
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
	WT_OVFL_REUSE **head, *reuse;

	head = page->modify->ovfl_track->ovfl_reuse;

	for (reuse = head[0]; reuse != NULL; reuse = reuse->next[0])
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
	head = page->modify->ovfl_track->ovfl_reuse;

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
	for (e = &head[0]; (reuse = *e) != NULL;) {
		if (F_ISSET(reuse, WT_OVFL_REUSE_INUSE)) {
			if (F_ISSET(reuse, WT_OVFL_REUSE_JUST_ADDED))
				incr += sizeof(WT_OVFL_REUSE) +
				    2 * sizeof(WT_OVFL_REUSE *) +
				    reuse->addr_size + reuse->value_size;

			F_CLR(reuse,
			    WT_OVFL_REUSE_INUSE | WT_OVFL_REUSE_JUST_ADDED);
			e = &(*e)->next[0];
			continue;
		}
		*e = (*e)->next[0];

		WT_ASSERT(session, !F_ISSET(reuse, WT_OVFL_REUSE_JUST_ADDED));
		decr += sizeof(WT_OVFL_REUSE) +
		    2 * sizeof(WT_OVFL_REUSE *) +
		    reuse->addr_size + reuse->value_size;

		if (WT_VERBOSE_ISSET(session, overflow))
			WT_RET(
			    __ovfl_reuse_verbose(session, page, reuse, "free"));
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
	head = page->modify->ovfl_track->ovfl_reuse;

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
	for (e = &head[0]; (reuse = *e) != NULL;) {
		if (!F_ISSET(reuse, WT_OVFL_REUSE_JUST_ADDED)) {
			F_CLR(reuse, WT_OVFL_REUSE_INUSE);
			e = &(*e)->next[0];
			continue;
		}
		*e = (*e)->next[0];

		if (WT_VERBOSE_ISSET(session, overflow))
			WT_RET(
			    __ovfl_reuse_verbose(session, page, reuse, "free"));
		WT_TRET(bm->free(
		    bm, session, WT_OVFL_REUSE_ADDR(reuse), reuse->addr_size));
		__wt_free(session, reuse);
	}
	return (0);
}

/*
 * __wt_ovfl_reuse_search --
 *	Search the page's list of overflow records for a match.
 */
int
__wt_ovfl_reuse_search(WT_SESSION_IMPL *session, WT_PAGE *page,
    uint8_t **addrp, uint32_t *addr_sizep,
    const void *value, uint32_t value_size)
{
	WT_OVFL_REUSE **head, *reuse;

	*addrp = NULL;
	*addr_sizep = 0;

	if (page->modify->ovfl_track == NULL)
		return (0);

	head = page->modify->ovfl_track->ovfl_reuse;

	/*
	 * The search function returns the first matching record in the list,
	 * which may be the first of many, overflow records may be identical.
	 * Find one without the in-use flag set and put it back into service.
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

	if (page->modify->ovfl_track == NULL)
		WT_RET(__ovfl_track_init(session, page));

	head = page->modify->ovfl_track->ovfl_reuse;

	/* Choose a skiplist depth for this insert. */
	skipdepth = __wt_skip_choose_depth();

	/*
	 * Allocate the WT_OVFL_REUSE structure, next pointers for the skip
	 * list, room for the address and value, then copy everything into
	 * place.
	 *
	 * To minimize the WT_OVFL_REUSE structure size, the address offset
	 * and size are single bytes: that's safe because the address follows
	 * the structure (which can't be more than about 100B), and address
	 * cookies are limited to 255B.
	 */
	size = sizeof(WT_OVFL_REUSE) +
	    skipdepth * sizeof(WT_OVFL_REUSE *) + addr_size + value_size;
	WT_RET(__wt_calloc(session, 1, size, &reuse));
	p = (uint8_t *)reuse +
	    sizeof(WT_OVFL_REUSE) + skipdepth * sizeof(WT_OVFL_REUSE *);
	reuse->addr_offset = (uint8_t)WT_PTRDIFF(p, reuse);
	reuse->addr_size = (uint8_t)addr_size;
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
 * __ovfl_txnc_verbose --
 *	Dump information about a transaction-cached overflow record.
 */
static int
__ovfl_txnc_verbose(WT_SESSION_IMPL *session,
    WT_PAGE *page, WT_OVFL_TXNC *txnc, const char *tag)
{
	WT_DECL_ITEM(tmp);
	WT_DECL_RET;

	WT_RET(__wt_scr_alloc(session, 64, &tmp));

	WT_VERBOSE_ERR(session, overflow,
	    "txn-cache: %s%s%p %s {%.*s}",
	    tag == NULL ? "" : tag,
	    tag == NULL ? "" : ": ",
	    page,
	    __wt_addr_string(
		session, tmp, WT_OVFL_TXNC_ADDR(txnc), txnc->addr_size),
	    WT_MIN(txnc->value_size, 40), (char *)WT_OVFL_TXNC_VALUE(txnc));

err:	__wt_scr_free(&tmp);
	return (ret);
}

#if 0
/*
 * __ovfl_txnc_dump --
 *	Debugging information.
 */
static void
__ovfl_txnc_dump(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_OVFL_TXNC **head, *txnc;

	head = page->modify->ovfl_track->ovfl_txnc;

	for (txnc = head[0]; txnc != NULL; txnc = txnc->next[0])
		(void)__ovfl_txnc_verbose(session, page, txnc, "dump");
}
#endif

/*
 * __ovfl_txnc_skip_search --
 *	Return the first matching addr in the overflow transaction-cache list.
 */
static WT_OVFL_TXNC *
__ovfl_txnc_skip_search(
    WT_OVFL_TXNC **head, const void *addr, uint32_t addr_size)
{
	WT_OVFL_TXNC **e;
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
		len = WT_MIN((*e)->addr_size, addr_size);
		cmp = memcmp(WT_OVFL_TXNC_ADDR(*e), addr, len);
		if (cmp == 0 && (*e)->addr_size == addr_size)
			return (*e);

		/*
		 * If the skiplist address is larger than the search address, or
		 * they compare equally and the skiplist address is longer than
		 * the search address, drop down a level, otherwise continue on
		 * this level.
		 */
		if (cmp > 0 || (cmp == 0 && (*e)->addr_size > addr_size)) {
			--i;			/* Drop down a level */
			--e;
		} else				/* Keep going at this level */
			e = &(*e)->next[i];
	}
	return (NULL);
}

/*
 * __ovfl_txnc_skip_search_stack --
 *	 Search an overflow transaction-cache skiplist, returning an
 * insert/remove stack.
 */
static void
__ovfl_txnc_skip_search_stack(WT_OVFL_TXNC **head,
    WT_OVFL_TXNC ***stack, const void *addr, uint32_t addr_size)
{
	WT_OVFL_TXNC **e;
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
		 * If the skiplist addr is larger than the search addr, or
		 * they compare equally and the skiplist addr is longer than
		 * the search addr, drop down a level, otherwise continue on
		 * this level.
		 */
		len = WT_MIN((*e)->addr_size, addr_size);
		cmp = memcmp(WT_OVFL_TXNC_ADDR(*e), addr, len);
		if (cmp > 0 || (cmp == 0 && (*e)->addr_size > addr_size))
			stack[i--] = e--;	/* Drop down a level */
		else
			e = &(*e)->next[i];	/* Keep going at this level */
	}
}

/*
 * __wt_ovfl_txnc_search --
 *	Search the page's list of transaction-cache overflow records for a
 * match.
 */
int
__wt_ovfl_txnc_search(
    WT_PAGE *page, const uint8_t *addr, uint32_t addr_size, WT_ITEM *store)
{
	WT_OVFL_TXNC **head, *txnc;

	if (page->modify->ovfl_track == NULL)
		return (WT_NOTFOUND);

	head = page->modify->ovfl_track->ovfl_txnc;

	if ((txnc = __ovfl_txnc_skip_search(head, addr, addr_size)) == NULL)
		return (WT_NOTFOUND);

	store->data = WT_OVFL_TXNC_VALUE(txnc);
	store->size = txnc->value_size;
	return (0);
}

/*
 * __wt_ovfl_txnc_add --
 *	Add a new entry to the page's list of transaction-cached overflow
 * records.
 */
int
__wt_ovfl_txnc_add(WT_SESSION_IMPL *session, WT_PAGE *page,
    const uint8_t *addr, uint32_t addr_size,
    const void *value, uint32_t value_size)
{
	WT_OVFL_TXNC **head, **stack[WT_SKIP_MAXDEPTH], *txnc;
	size_t size;
	u_int i, skipdepth;
	uint8_t *p;

	if (page->modify->ovfl_track == NULL)
		WT_RET(__ovfl_track_init(session, page));

	head = page->modify->ovfl_track->ovfl_txnc;

	/* Choose a skiplist depth for this insert. */
	skipdepth = __wt_skip_choose_depth();

	/*
	 * Allocate the WT_OVFL_TXNC structure, next pointers for the skip
	 * list, room for the address and value, then copy everything into
	 * place.
	 *
	 * To minimize the WT_OVFL_TXNC structure size, the address offset
	 * and size are single bytes: that's safe because the address follows
	 * the structure (which can't be more than about 100B), and address
	 * cookies are limited to 255B.
	 */
	size = sizeof(WT_OVFL_TXNC) +
	    skipdepth * sizeof(WT_OVFL_TXNC *) + addr_size + value_size;
	WT_RET(__wt_calloc(session, 1, size, &txnc));
	p = (uint8_t *)txnc +
	    sizeof(WT_OVFL_TXNC) + skipdepth * sizeof(WT_OVFL_TXNC *);
	txnc->addr_offset = (uint8_t)WT_PTRDIFF(p, txnc);
	txnc->addr_size = (uint8_t)addr_size;
	memcpy(p, addr, addr_size);
	p += addr_size;
	txnc->value_offset = WT_PTRDIFF32(p, txnc);
	txnc->value_size = value_size;
	memcpy(p, value, value_size);

	__wt_cache_page_inmem_incr(
	    session, page, size + addr_size + value_size);

	/* Insert the new entry into the skiplist. */
	__ovfl_txnc_skip_search_stack(head, stack, addr, addr_size);
	for (i = 0; i < skipdepth; ++i) {
		txnc->next[i] = *stack[i];
		*stack[i] = txnc;
	}

	if (WT_VERBOSE_ISSET(session, overflow))
		WT_RET(__ovfl_txnc_verbose(session, page, txnc, "add"));

	return (0);
}

/*
 * __wt_ovfl_track_wrapup --
 *	Resolve the page's overflow tracking on reconciliation success.
 */
int
__wt_ovfl_track_wrapup(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	if (page->modify->ovfl_track != NULL) {
		WT_RET(__ovfl_onpage_wrapup(session, page));
		WT_RET(__ovfl_reuse_wrapup(session, page));
	}
	return (0);
}

/*
 * __wt_ovfl_track_wrapup_err --
 *	Resolve the page's overflow tracking on reconciliation error.
 */
int
__wt_ovfl_track_wrapup_err(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	if (page->modify->ovfl_track != NULL) {
		WT_RET(__ovfl_onpage_wrapup_err(session, page));
		WT_RET(__ovfl_reuse_wrapup_err(session, page));
	}
	return (0);
}
