/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * Estimated memory cost for a structure on the overflow lists, the size of the structure plus two
 * pointers (assume the average skip list depth is 2).
 */
#define WT_OVFL_SIZE(p, s) (sizeof(s) + 2 * sizeof(void *) + (p)->addr_size + (p)->value_size)

/*
 * __wt_ovfl_track_init --
 *     Initialize the overflow tracking structure.
 */
int
__wt_ovfl_track_init(WT_SESSION_IMPL *session, WT_PAGE *page)
{
    return (__wt_calloc_one(session, &page->modify->ovfl_track));
}

/*
 * __ovfl_discard_verbose --
 *     Dump information about a discard overflow record.
 */
static int
__ovfl_discard_verbose(WT_SESSION_IMPL *session, WT_PAGE *page, WT_CELL *cell, const char *tag)
{
    WT_CELL_UNPACK_KV *unpack, _unpack;
    WT_DECL_ITEM(tmp);

    /* Because we dereference the page pointer, it can't be NULL */
    if (page == NULL)
        WT_RET(EINVAL);

    WT_RET(__wt_scr_alloc(session, 512, &tmp));

    unpack = &_unpack;
    __wt_cell_unpack_kv(session, page->dsk, cell, unpack);

    __wt_verbose(session, WT_VERB_OVERFLOW, "discard: %s%s%p %s", tag == NULL ? "" : tag,
      tag == NULL ? "" : ": ", (void *)page,
      __wt_addr_string(session, unpack->data, unpack->size, tmp));

    __wt_scr_free(session, &tmp);
    return (0);
}

#if 0
/*
 * __ovfl_discard_dump --
 *     Debugging information.
 */
static void
__ovfl_discard_dump(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_CELL **cellp;
	WT_OVFL_TRACK *track;
	size_t i;

	if (page->modify == NULL || page->modify->ovfl_track == NULL)
		return;

	track = page->modify->ovfl_track;
	for (i = 0, cellp = track->discard;
	    i < track->discard_entries; ++i, ++cellp)
		(void)__ovfl_discard_verbose(session, page, *cellp, "dump");
}
#endif

/*
 * __ovfl_discard_wrapup --
 *     Resolve the page's overflow discard list after a page is written.
 */
static int
__ovfl_discard_wrapup(WT_SESSION_IMPL *session, WT_PAGE *page)
{
    WT_CELL **cellp;
    WT_OVFL_TRACK *track;
    uint32_t i;

    track = page->modify->ovfl_track;
    for (i = 0, cellp = track->discard; i < track->discard_entries; ++i, ++cellp) {
        if (WT_VERBOSE_ISSET(session, WT_VERB_OVERFLOW))
            WT_RET(__ovfl_discard_verbose(session, page, *cellp, "free"));

        /* Discard each cell's overflow item. */
        WT_RET(__wt_ovfl_discard(session, page, *cellp));
    }

    __wt_free(session, track->discard);
    track->discard_entries = track->discard_allocated = 0;

    return (0);
}

/*
 * __ovfl_discard_wrapup_err --
 *     Resolve the page's overflow discard list after an error occurs.
 */
static void
__ovfl_discard_wrapup_err(WT_SESSION_IMPL *session, WT_PAGE *page)
{
    WT_OVFL_TRACK *track;

    track = page->modify->ovfl_track;

    __wt_free(session, track->discard);
    track->discard_entries = track->discard_allocated = 0;
}

/*
 * __wt_ovfl_discard_add --
 *     Add a new entry to the page's list of overflow records that have been discarded.
 */
int
__wt_ovfl_discard_add(WT_SESSION_IMPL *session, WT_PAGE *page, WT_CELL *cell)
{
    WT_OVFL_TRACK *track;

    if (page->modify->ovfl_track == NULL)
        WT_RET(__wt_ovfl_track_init(session, page));

    track = page->modify->ovfl_track;
    WT_RET(__wt_realloc_def(
      session, &track->discard_allocated, track->discard_entries + 1, &track->discard));
    track->discard[track->discard_entries++] = cell;

    if (WT_VERBOSE_ISSET(session, WT_VERB_OVERFLOW))
        WT_RET(__ovfl_discard_verbose(session, page, cell, "add"));

    return (0);
}

/*
 * __wt_ovfl_discard_free --
 *     Free the page's list of discarded overflow record addresses.
 */
void
__wt_ovfl_discard_free(WT_SESSION_IMPL *session, WT_PAGE *page)
{
    WT_OVFL_TRACK *track;

    if (page->modify == NULL || page->modify->ovfl_track == NULL)
        return;

    track = page->modify->ovfl_track;

    __wt_free(session, track->discard);
    track->discard_entries = track->discard_allocated = 0;
}

/*
 * __ovfl_reuse_verbose --
 *     Dump information about a reuse overflow record.
 */
static int
__ovfl_reuse_verbose(WT_SESSION_IMPL *session, WT_PAGE *page, WT_OVFL_REUSE *reuse, const char *tag)
{
    WT_DECL_ITEM(tmp);

    WT_RET(__wt_scr_alloc(session, 64, &tmp));

    __wt_verbose(session, WT_VERB_OVERFLOW, "reuse: %s%s%p %s (%s%s%s) {%.*s}",
      tag == NULL ? "" : tag, tag == NULL ? "" : ": ", (void *)page,
      __wt_addr_string(session, WT_OVFL_REUSE_ADDR(reuse), reuse->addr_size, tmp),
      F_ISSET(reuse, WT_OVFL_REUSE_INUSE) ? "inuse" : "",
      F_ISSET(reuse, WT_OVFL_REUSE_INUSE) && F_ISSET(reuse, WT_OVFL_REUSE_JUST_ADDED) ? ", " : "",
      F_ISSET(reuse, WT_OVFL_REUSE_JUST_ADDED) ? "just-added" : "",
      (int)WT_MIN(reuse->value_size, 40), (char *)WT_OVFL_REUSE_VALUE(reuse));

    __wt_scr_free(session, &tmp);
    return (0);
}

#if 0
/*
 * __ovfl_reuse_dump --
 *     Debugging information.
 */
static void
__ovfl_reuse_dump(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_OVFL_REUSE **head, *reuse;

	if (page->modify == NULL || page->modify->ovfl_track == NULL)
		return;
	head = page->modify->ovfl_track->ovfl_reuse;

	for (reuse = head[0]; reuse != NULL; reuse = reuse->next[0])
		(void)__ovfl_reuse_verbose(session, page, reuse, "dump");
}
#endif

/*
 * __ovfl_reuse_skip_search --
 *     Return the first, not in-use, matching value in the overflow reuse list.
 */
static WT_OVFL_REUSE *
__ovfl_reuse_skip_search(WT_OVFL_REUSE **head, const void *value, size_t value_size)
{
    WT_OVFL_REUSE **e, *next;
    size_t len;
    int cmp, i;

    /*
     * Start at the highest skip level, then go as far as possible at each level before stepping
     * down to the next.
     */
    for (i = WT_SKIP_MAXDEPTH - 1, e = &head[i]; i >= 0;) {
        if (*e == NULL) { /* Empty levels */
            --i;
            --e;
            continue;
        }

        /*
         * Values are not unique, and it's possible to have long lists of identical overflow items.
         * (We've seen it in benchmarks.) Move through a list of identical items at the current
         * level as long as the next one is in-use, otherwise, drop down a level. When at the bottom
         * level, return items if reusable, else NULL.
         */
        len = WT_MIN((*e)->value_size, value_size);
        cmp = memcmp(WT_OVFL_REUSE_VALUE(*e), value, len);
        if (cmp == 0 && (*e)->value_size == value_size) {
            if (i == 0)
                return (F_ISSET(*e, WT_OVFL_REUSE_INUSE) ? NULL : *e);
            if ((next = (*e)->next[i]) == NULL || !F_ISSET(next, WT_OVFL_REUSE_INUSE) ||
              next->value_size != len || memcmp(WT_OVFL_REUSE_VALUE(next), value, len) != 0) {
                --i; /* Drop down a level */
                --e;
            } else /* Keep going at this level */
                e = &(*e)->next[i];
            continue;
        }

        /*
         * If the skiplist value is larger than the search value, or they compare equally and the
         * skiplist value is longer than the search value, drop down a level, otherwise continue on
         * this level.
         */
        if (cmp > 0 || (cmp == 0 && (*e)->value_size > value_size)) {
            --i; /* Drop down a level */
            --e;
        } else /* Keep going at this level */
            e = &(*e)->next[i];
    }
    return (NULL);
}

/*
 * __ovfl_reuse_skip_search_stack --
 *     Search an overflow reuse skiplist, returning an insert/remove stack.
 */
static void
__ovfl_reuse_skip_search_stack(
  WT_OVFL_REUSE **head, WT_OVFL_REUSE ***stack, const void *value, size_t value_size)
{
    WT_OVFL_REUSE **e;
    size_t len;
    int cmp, i;

    /*
     * Start at the highest skip level, then go as far as possible at each level before stepping
     * down to the next.
     */
    for (i = WT_SKIP_MAXDEPTH - 1, e = &head[i]; i >= 0;) {
        if (*e == NULL) { /* Empty levels */
            stack[i--] = e--;
            continue;
        }

        /*
         * If the skiplist value is larger than the search value, or they compare equally and the
         * skiplist value is longer than the search value, drop down a level, otherwise continue on
         * this level.
         */
        len = WT_MIN((*e)->value_size, value_size);
        cmp = memcmp(WT_OVFL_REUSE_VALUE(*e), value, len);
        if (cmp > 0 || (cmp == 0 && (*e)->value_size > value_size))
            stack[i--] = e--; /* Drop down a level */
        else
            e = &(*e)->next[i]; /* Keep going at this level */
    }
}

/*
 * __ovfl_reuse_wrapup --
 *     Resolve the page's overflow reuse list after a page is written.
 */
static int
__ovfl_reuse_wrapup(WT_SESSION_IMPL *session, WT_PAGE *page)
{
    WT_BM *bm;
    WT_OVFL_REUSE **e, **head, *reuse;
    size_t decr;
    int i;

    bm = S2BT(session)->bm;
    head = page->modify->ovfl_track->ovfl_reuse;

    /*
     * Discard any overflow records that aren't in-use, freeing underlying blocks.
     *
     * First, walk the overflow reuse lists (except for the lowest one), fixing up skiplist links.
     */
    for (i = WT_SKIP_MAXDEPTH - 1; i > 0; --i)
        for (e = &head[i]; (reuse = *e) != NULL;) {
            if (F_ISSET(reuse, WT_OVFL_REUSE_INUSE)) {
                e = &reuse->next[i];
                continue;
            }
            *e = reuse->next[i];
        }

    /*
     * Second, discard any overflow record without an in-use flag, clear the flags for the next run.
     *
     * As part of the pass through the lowest level, figure out how much space we added/subtracted
     * from the page, and update its footprint. We don't get it exactly correct because we don't
     * know the depth of the skiplist here, but it's close enough, and figuring out the memory
     * footprint change in the reconciliation wrapup code means fewer atomic updates and less code
     * overall.
     */
    decr = 0;
    for (e = &head[0]; (reuse = *e) != NULL;) {
        if (F_ISSET(reuse, WT_OVFL_REUSE_INUSE)) {
            F_CLR(reuse, WT_OVFL_REUSE_INUSE | WT_OVFL_REUSE_JUST_ADDED);
            e = &reuse->next[0];
            continue;
        }
        *e = reuse->next[0];

        WT_ASSERT_ALWAYS(session, !F_ISSET(reuse, WT_OVFL_REUSE_JUST_ADDED),
          "Attempting to reuse dirty overflow record");

        if (WT_VERBOSE_ISSET(session, WT_VERB_OVERFLOW))
            WT_RET(__ovfl_reuse_verbose(session, page, reuse, "free"));

        WT_RET(bm->free(bm, session, WT_OVFL_REUSE_ADDR(reuse), reuse->addr_size));
        decr += WT_OVFL_SIZE(reuse, WT_OVFL_REUSE);
        __wt_free(session, reuse);
    }

    if (decr != 0)
        __wt_cache_page_inmem_decr(session, page, decr);
    return (0);
}

/*
 * __ovfl_reuse_wrapup_err --
 *     Resolve the page's overflow reuse list after an error occurs.
 */
static int
__ovfl_reuse_wrapup_err(WT_SESSION_IMPL *session, WT_PAGE *page)
{
    WT_BM *bm;
    WT_DECL_RET;
    WT_OVFL_REUSE **e, **head, *reuse;
    size_t decr;
    int i;

    bm = S2BT(session)->bm;
    head = page->modify->ovfl_track->ovfl_reuse;

    /*
     * Discard any overflow records that were just added, freeing underlying blocks.
     *
     * First, walk the overflow reuse lists (except for the lowest one), fixing up skiplist links.
     */
    for (i = WT_SKIP_MAXDEPTH - 1; i > 0; --i)
        for (e = &head[i]; (reuse = *e) != NULL;) {
            if (!F_ISSET(reuse, WT_OVFL_REUSE_JUST_ADDED)) {
                e = &reuse->next[i];
                continue;
            }
            *e = reuse->next[i];
        }

    /*
     * Second, discard any overflow record with a just-added flag, clear the flags for the next run.
     */
    decr = 0;
    for (e = &head[0]; (reuse = *e) != NULL;) {
        if (!F_ISSET(reuse, WT_OVFL_REUSE_JUST_ADDED)) {
            F_CLR(reuse, WT_OVFL_REUSE_INUSE);
            e = &reuse->next[0];
            continue;
        }
        *e = reuse->next[0];

        if (WT_VERBOSE_ISSET(session, WT_VERB_OVERFLOW))
            WT_RET(__ovfl_reuse_verbose(session, page, reuse, "free"));

        WT_TRET(bm->free(bm, session, WT_OVFL_REUSE_ADDR(reuse), reuse->addr_size));
        decr += WT_OVFL_SIZE(reuse, WT_OVFL_REUSE);
        __wt_free(session, reuse);
    }

    if (decr != 0)
        __wt_cache_page_inmem_decr(session, page, decr);
    return (0);
}

/*
 * __wt_ovfl_reuse_search --
 *     Search the page's list of overflow records for a match.
 */
int
__wt_ovfl_reuse_search(WT_SESSION_IMPL *session, WT_PAGE *page, uint8_t **addrp, size_t *addr_sizep,
  const void *value, size_t value_size)
{
    WT_OVFL_REUSE **head, *reuse;

    *addrp = NULL;
    *addr_sizep = 0;

    if (page->modify->ovfl_track == NULL)
        return (0);

    head = page->modify->ovfl_track->ovfl_reuse;

    /*
     * The search function returns the first matching record in the list which does not have the
     * in-use flag set, or NULL.
     */
    if ((reuse = __ovfl_reuse_skip_search(head, value, value_size)) == NULL)
        return (0);

    *addrp = WT_OVFL_REUSE_ADDR(reuse);
    *addr_sizep = reuse->addr_size;
    F_SET(reuse, WT_OVFL_REUSE_INUSE);

    if (WT_VERBOSE_ISSET(session, WT_VERB_OVERFLOW))
        WT_RET(__ovfl_reuse_verbose(session, page, reuse, "reclaim"));
    return (0);
}

/*
 * __wt_ovfl_reuse_add --
 *     Add a new entry to the page's list of overflow records tracked for reuse.
 */
int
__wt_ovfl_reuse_add(WT_SESSION_IMPL *session, WT_PAGE *page, const uint8_t *addr, size_t addr_size,
  const void *value, size_t value_size)
{
    WT_OVFL_REUSE **head, *reuse, **stack[WT_SKIP_MAXDEPTH];
    size_t size;
    uint8_t *p;
    u_int i, skipdepth;

    if (page->modify->ovfl_track == NULL)
        WT_RET(__wt_ovfl_track_init(session, page));

    head = page->modify->ovfl_track->ovfl_reuse;

    /* Choose a skiplist depth for this insert. */
    skipdepth = __wt_skip_choose_depth(session);

    /*
     * Allocate the WT_OVFL_REUSE structure, next pointers for the skip list, room for the address
     * and value, then copy everything into place.
     *
     * To minimize the WT_OVFL_REUSE structure size, the address offset and size are single bytes:
     * that's safe because the address follows the structure (which can't be more than about 100B),
     * and address cookies are limited to 255B.
     */
    size = sizeof(WT_OVFL_REUSE) + skipdepth * sizeof(WT_OVFL_REUSE *) + addr_size + value_size;
    WT_RET(__wt_calloc(session, 1, size, &reuse));
    p = (uint8_t *)reuse + sizeof(WT_OVFL_REUSE) + skipdepth * sizeof(WT_OVFL_REUSE *);
    reuse->addr_offset = (uint8_t)WT_PTRDIFF(p, reuse);
    reuse->addr_size = (uint8_t)addr_size;
    memcpy(p, addr, addr_size);
    p += addr_size;
    reuse->value_offset = WT_PTRDIFF32(p, reuse);
    reuse->value_size = WT_STORE_SIZE(value_size);
    memcpy(p, value, value_size);
    F_SET(reuse, WT_OVFL_REUSE_INUSE | WT_OVFL_REUSE_JUST_ADDED);

    __wt_cache_page_inmem_incr(session, page, WT_OVFL_SIZE(reuse, WT_OVFL_REUSE));

    /* Insert the new entry into the skiplist. */
    __ovfl_reuse_skip_search_stack(head, stack, value, value_size);
    for (i = 0; i < skipdepth; ++i) {
        reuse->next[i] = *stack[i];
        *stack[i] = reuse;
    }

    if (WT_VERBOSE_ISSET(session, WT_VERB_OVERFLOW))
        WT_RET(__ovfl_reuse_verbose(session, page, reuse, "add"));

    return (0);
}

/*
 * __wt_ovfl_reuse_free --
 *     Free the page's list of overflow records tracked for reuse.
 */
void
__wt_ovfl_reuse_free(WT_SESSION_IMPL *session, WT_PAGE *page)
{
    WT_OVFL_REUSE *reuse;
    WT_PAGE_MODIFY *mod;
    void *next;

    mod = page->modify;
    if (mod == NULL || mod->ovfl_track == NULL)
        return;

    for (reuse = mod->ovfl_track->ovfl_reuse[0]; reuse != NULL; reuse = next) {
        next = reuse->next[0];
        __wt_free(session, reuse);
    }
}

/*
 * __wt_ovfl_track_wrapup --
 *     Resolve the page's overflow tracking on reconciliation success.
 */
int
__wt_ovfl_track_wrapup(WT_SESSION_IMPL *session, WT_PAGE *page)
{
    WT_OVFL_TRACK *track;

    if (page->modify == NULL || page->modify->ovfl_track == NULL)
        return (0);

    track = page->modify->ovfl_track;
    if (track->discard != NULL)
        WT_RET(__ovfl_discard_wrapup(session, page));

    if (track->ovfl_reuse[0] != NULL)
        WT_RET(__ovfl_reuse_wrapup(session, page));

    return (0);
}

/*
 * __wt_ovfl_track_wrapup_err --
 *     Resolve the page's overflow tracking on reconciliation error.
 */
int
__wt_ovfl_track_wrapup_err(WT_SESSION_IMPL *session, WT_PAGE *page)
{
    WT_OVFL_TRACK *track;

    if (page->modify == NULL || page->modify->ovfl_track == NULL)
        return (0);

    track = page->modify->ovfl_track;
    if (track->discard != NULL)
        __ovfl_discard_wrapup_err(session, page);

    if (track->ovfl_reuse[0] != NULL)
        WT_RET(__ovfl_reuse_wrapup_err(session, page));

    return (0);
}

#ifdef HAVE_UNITTEST
int
__ut_ovfl_discard_verbose(WT_SESSION_IMPL *session, WT_PAGE *page, WT_CELL *cell, const char *tag)
{
    return (__ovfl_discard_verbose(session, page, cell, tag));
}

int
__ut_ovfl_discard_wrapup(WT_SESSION_IMPL *session, WT_PAGE *page)
{
    return (__ovfl_discard_wrapup(session, page));
}
#endif
