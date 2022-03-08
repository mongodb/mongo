/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * WT_BLOCK_RET --
 *	Handle extension list errors that would normally panic the system but
 * which should fail gracefully when verifying.
 */
#define WT_BLOCK_RET(session, block, v, ...)                                        \
    do {                                                                            \
        int __ret = (v);                                                            \
        __wt_err(session, __ret, __VA_ARGS__);                                      \
        return ((block)->verify ?                                                   \
            __ret :                                                                 \
            __wt_panic(session, WT_PANIC, "block manager extension list failure")); \
    } while (0)

static int __block_append(WT_SESSION_IMPL *, WT_BLOCK *, WT_EXTLIST *, wt_off_t, wt_off_t);
static int __block_ext_overlap(
  WT_SESSION_IMPL *, WT_BLOCK *, WT_EXTLIST *, WT_EXT **, WT_EXTLIST *, WT_EXT **);
static int __block_extlist_dump(WT_SESSION_IMPL *, WT_BLOCK *, WT_EXTLIST *, const char *);
static int __block_merge(WT_SESSION_IMPL *, WT_BLOCK *, WT_EXTLIST *, wt_off_t, wt_off_t);

/*
 * __block_off_srch_last --
 *     Return the last element in the list, along with a stack for appending.
 */
static inline WT_EXT *
__block_off_srch_last(WT_EXT **head, WT_EXT ***stack)
{
    WT_EXT **extp, *last;
    int i;

    last = NULL; /* The list may be empty */

    /*
     * Start at the highest skip level, then go as far as possible at each level before stepping
     * down to the next.
     */
    for (i = WT_SKIP_MAXDEPTH - 1, extp = &head[i]; i >= 0;)
        if (*extp != NULL) {
            last = *extp;
            extp = &(*extp)->next[i];
        } else
            stack[i--] = extp--;
    return (last);
}

/*
 * __block_off_srch --
 *     Search a by-offset skiplist (either the primary by-offset list, or the by-offset list
 *     referenced by a size entry), for the specified offset.
 */
static inline void
__block_off_srch(WT_EXT **head, wt_off_t off, WT_EXT ***stack, bool skip_off)
{
    WT_EXT **extp;
    int i;

    /*
     * Start at the highest skip level, then go as far as possible at each level before stepping
     * down to the next.
     *
     * Return a stack for an exact match or the next-largest item.
     *
     * The WT_EXT structure contains two skiplists, the primary one and the per-size bucket one: if
     * the skip_off flag is set, offset the skiplist array by the depth specified in this particular
     * structure.
     */
    for (i = WT_SKIP_MAXDEPTH - 1, extp = &head[i]; i >= 0;)
        if (*extp != NULL && (*extp)->off < off)
            extp = &(*extp)->next[i + (skip_off ? (*extp)->depth : 0)];
        else
            stack[i--] = extp--;
}

/*
 * __block_first_srch --
 *     Search the skiplist for the first available slot.
 */
static inline bool
__block_first_srch(WT_EXT **head, wt_off_t size, WT_EXT ***stack)
{
    WT_EXT *ext;

    /*
     * Linear walk of the available chunks in offset order; take the first one that's large enough.
     */
    WT_EXT_FOREACH (ext, head)
        if (ext->size >= size)
            break;
    if (ext == NULL)
        return (false);

    /* Build a stack for the offset we want. */
    __block_off_srch(head, ext->off, stack, false);
    return (true);
}

/*
 * __block_size_srch --
 *     Search the by-size skiplist for the specified size.
 */
static inline void
__block_size_srch(WT_SIZE **head, wt_off_t size, WT_SIZE ***stack)
{
    WT_SIZE **szp;
    int i;

    /*
     * Start at the highest skip level, then go as far as possible at each level before stepping
     * down to the next.
     *
     * Return a stack for an exact match or the next-largest item.
     */
    for (i = WT_SKIP_MAXDEPTH - 1, szp = &head[i]; i >= 0;)
        if (*szp != NULL && (*szp)->size < size)
            szp = &(*szp)->next[i];
        else
            stack[i--] = szp--;
}

/*
 * __block_off_srch_pair --
 *     Search a by-offset skiplist for before/after records of the specified offset.
 */
static inline void
__block_off_srch_pair(WT_EXTLIST *el, wt_off_t off, WT_EXT **beforep, WT_EXT **afterp)
{
    WT_EXT **head, **extp;
    int i;

    *beforep = *afterp = NULL;

    head = el->off;

    /*
     * Start at the highest skip level, then go as far as possible at each level before stepping
     * down to the next.
     */
    for (i = WT_SKIP_MAXDEPTH - 1, extp = &head[i]; i >= 0;) {
        if (*extp == NULL) {
            --i;
            --extp;
            continue;
        }

        if ((*extp)->off < off) { /* Keep going at this level */
            *beforep = *extp;
            extp = &(*extp)->next[i];
        } else { /* Drop down a level */
            *afterp = *extp;
            --i;
            --extp;
        }
    }
}

/*
 * __block_ext_insert --
 *     Insert an extent into an extent list.
 */
static int
__block_ext_insert(WT_SESSION_IMPL *session, WT_EXTLIST *el, WT_EXT *ext)
{
    WT_EXT **astack[WT_SKIP_MAXDEPTH];
    WT_SIZE *szp, **sstack[WT_SKIP_MAXDEPTH];
    u_int i;

    /*
     * If we are inserting a new size onto the size skiplist, we'll need a new WT_SIZE structure for
     * that skiplist.
     */
    if (el->track_size) {
        __block_size_srch(el->sz, ext->size, sstack);
        szp = *sstack[0];
        if (szp == NULL || szp->size != ext->size) {
            WT_RET(__wt_block_size_alloc(session, &szp));
            szp->size = ext->size;
            szp->depth = ext->depth;
            for (i = 0; i < ext->depth; ++i) {
                szp->next[i] = *sstack[i];
                *sstack[i] = szp;
            }
        }

        /*
         * Insert the new WT_EXT structure into the size element's offset skiplist.
         */
        __block_off_srch(szp->off, ext->off, astack, true);
        for (i = 0; i < ext->depth; ++i) {
            ext->next[i + ext->depth] = *astack[i];
            *astack[i] = ext;
        }
    }
#ifdef HAVE_DIAGNOSTIC
    if (!el->track_size)
        for (i = 0; i < ext->depth; ++i)
            ext->next[i + ext->depth] = NULL;
#endif

    /* Insert the new WT_EXT structure into the offset skiplist. */
    __block_off_srch(el->off, ext->off, astack, false);
    for (i = 0; i < ext->depth; ++i) {
        ext->next[i] = *astack[i];
        *astack[i] = ext;
    }

    ++el->entries;
    el->bytes += (uint64_t)ext->size;

    /* Update the cached end-of-list. */
    if (ext->next[0] == NULL)
        el->last = ext;

    return (0);
}

/*
 * __block_off_insert --
 *     Insert a file range into an extent list.
 */
static int
__block_off_insert(WT_SESSION_IMPL *session, WT_EXTLIST *el, wt_off_t off, wt_off_t size)
{
    WT_EXT *ext;

    WT_RET(__wt_block_ext_alloc(session, &ext));
    ext->off = off;
    ext->size = size;

    return (__block_ext_insert(session, el, ext));
}

#ifdef HAVE_DIAGNOSTIC
/*
 * __block_off_match --
 *     Return if any part of a specified range appears on a specified extent list.
 */
static bool
__block_off_match(WT_EXTLIST *el, wt_off_t off, wt_off_t size)
{
    WT_EXT *before, *after;

    /* Search for before and after entries for the offset. */
    __block_off_srch_pair(el, off, &before, &after);

    /* If "before" or "after" overlaps, we have a winner. */
    if (before != NULL && before->off + before->size > off)
        return (true);
    if (after != NULL && off + size > after->off)
        return (true);
    return (false);
}

/*
 * __wt_block_misplaced --
 *     Complain if a block appears on the available or discard lists.
 */
int
__wt_block_misplaced(WT_SESSION_IMPL *session, WT_BLOCK *block, const char *list, wt_off_t offset,
  uint32_t size, bool live, const char *func, int line)
{
    const char *name;

    name = NULL;

    /*
     * Don't check during the salvage read phase, we might be reading an already freed overflow
     * page.
     */
    if (F_ISSET(session, WT_SESSION_QUIET_CORRUPT_FILE))
        return (0);

    /*
     * Verify a block the btree engine thinks it "owns" doesn't appear on the available or discard
     * lists (it might reasonably be on the alloc list, if it was allocated since the last
     * checkpoint). The engine "owns" a block if it's trying to read or free the block, and those
     * functions make this check.
     *
     * Any block being read or freed should not be "available".
     *
     * Any block being read or freed in the live system should not be on the discard list. (A
     * checkpoint handle might be reading a block which is on the live system's discard list; any
     * attempt to free a block from a checkpoint handle has already failed.)
     */
    __wt_spin_lock(session, &block->live_lock);
    if (__block_off_match(&block->live.avail, offset, size))
        name = "available";
    else if (live && __block_off_match(&block->live.discard, offset, size))
        name = "discard";
    __wt_spin_unlock(session, &block->live_lock);
    if (name != NULL)
        return (__wt_panic(session, WT_PANIC,
          "%s failed: %" PRIuMAX "/%" PRIu32 " is on the %s list (%s, %d)", list, (uintmax_t)offset,
          size, name, func, line));
    return (0);
}
#endif

/*
 * __block_off_remove --
 *     Remove a record from an extent list.
 */
static int
__block_off_remove(
  WT_SESSION_IMPL *session, WT_BLOCK *block, WT_EXTLIST *el, wt_off_t off, WT_EXT **extp)
{
    WT_EXT *ext, **astack[WT_SKIP_MAXDEPTH];
    WT_SIZE *szp, **sstack[WT_SKIP_MAXDEPTH];
    u_int i;

    /* Find and remove the record from the by-offset skiplist. */
    __block_off_srch(el->off, off, astack, false);
    ext = *astack[0];
    if (ext == NULL || ext->off != off)
        goto corrupt;
    for (i = 0; i < ext->depth; ++i)
        *astack[i] = ext->next[i];

    /*
     * Find and remove the record from the size's offset skiplist; if that empties the by-size
     * skiplist entry, remove it as well.
     */
    if (el->track_size) {
        __block_size_srch(el->sz, ext->size, sstack);
        szp = *sstack[0];
        if (szp == NULL || szp->size != ext->size)
            WT_RET_PANIC(session, EINVAL, "extent not found in by-size list during remove");
        __block_off_srch(szp->off, off, astack, true);
        ext = *astack[0];
        if (ext == NULL || ext->off != off)
            goto corrupt;
        for (i = 0; i < ext->depth; ++i)
            *astack[i] = ext->next[i + ext->depth];
        if (szp->off[0] == NULL) {
            for (i = 0; i < szp->depth; ++i)
                *sstack[i] = szp->next[i];
            __wt_block_size_free(session, szp);
        }
    }
#ifdef HAVE_DIAGNOSTIC
    if (!el->track_size) {
        bool not_null;
        for (i = 0, not_null = false; i < ext->depth; ++i)
            if (ext->next[i + ext->depth] != NULL)
                not_null = true;
        WT_ASSERT(session, not_null == false);
    }
#endif

    --el->entries;
    el->bytes -= (uint64_t)ext->size;

    /* Return the record if our caller wants it, otherwise free it. */
    if (extp == NULL)
        __wt_block_ext_free(session, ext);
    else
        *extp = ext;

    /* Update the cached end-of-list. */
    if (el->last == ext)
        el->last = NULL;

    return (0);

corrupt:
    WT_BLOCK_RET(
      session, block, EINVAL, "attempt to remove non-existent offset from an extent list");
}

/*
 * __wt_block_off_remove_overlap --
 *     Remove a range from an extent list, where the range may be part of an overlapping entry.
 */
int
__wt_block_off_remove_overlap(
  WT_SESSION_IMPL *session, WT_BLOCK *block, WT_EXTLIST *el, wt_off_t off, wt_off_t size)
{
    WT_EXT *before, *after, *ext;
    wt_off_t a_off, a_size, b_off, b_size;

    WT_ASSERT(session, off != WT_BLOCK_INVALID_OFFSET);

    /* Search for before and after entries for the offset. */
    __block_off_srch_pair(el, off, &before, &after);

    /* If "before" or "after" overlaps, retrieve the overlapping entry. */
    if (before != NULL && before->off + before->size > off) {
        WT_RET(__block_off_remove(session, block, el, before->off, &ext));

        /* Calculate overlapping extents. */
        a_off = ext->off;
        a_size = off - ext->off;
        b_off = off + size;
        b_size = ext->size - (a_size + size);
    } else if (after != NULL && off + size > after->off) {
        WT_RET(__block_off_remove(session, block, el, after->off, &ext));

        /*
         * Calculate overlapping extents. There's no initial overlap since the after extent
         * presumably cannot begin before "off".
         */
        a_off = WT_BLOCK_INVALID_OFFSET;
        a_size = 0;
        b_off = off + size;
        b_size = ext->size - (b_off - ext->off);
    } else
        return (WT_NOTFOUND);

    /*
     * If there are overlaps, insert the item; re-use the extent structure and save the allocation
     * (we know there's no need to merge).
     */
    if (a_size != 0) {
        ext->off = a_off;
        ext->size = a_size;
        WT_RET(__block_ext_insert(session, el, ext));
        ext = NULL;
    }
    if (b_size != 0) {
        if (ext == NULL)
            WT_RET(__block_off_insert(session, el, b_off, b_size));
        else {
            ext->off = b_off;
            ext->size = b_size;
            WT_RET(__block_ext_insert(session, el, ext));
            ext = NULL;
        }
    }
    if (ext != NULL)
        __wt_block_ext_free(session, ext);
    return (0);
}

/*
 * __block_extend --
 *     Extend the file to allocate space.
 */
static inline int
__block_extend(WT_SESSION_IMPL *session, WT_BLOCK *block, wt_off_t *offp, wt_off_t size)
{
    /*
     * Callers of this function are expected to have already acquired any locks required to extend
     * the file.
     *
     * We should never be allocating from an empty file.
     */
    if (block->size < block->allocsize)
        WT_RET_MSG(session, EINVAL, "file has no description information");

    /*
     * Make sure we don't allocate past the maximum file size.  There's no
     * easy way to know the maximum wt_off_t on a system, limit growth to
     * 8B bits (we currently check an wt_off_t is 8B in verify_build.h). I
     * don't think we're likely to see anything bigger for awhile.
     */
    if (block->size > (wt_off_t)INT64_MAX - size)
        WT_RET_MSG(session, WT_ERROR, "block allocation failed, file cannot grow further");

    *offp = block->size;
    block->size += size;

    WT_STAT_DATA_INCR(session, block_extension);
    __wt_verbose(session, WT_VERB_BLOCK, "file extend %" PRIdMAX "-%" PRIdMAX, (intmax_t)*offp,
      (intmax_t)(*offp + size));

    return (0);
}

/*
 * __wt_block_alloc --
 *     Alloc a chunk of space from the underlying file.
 */
int
__wt_block_alloc(WT_SESSION_IMPL *session, WT_BLOCK *block, wt_off_t *offp, wt_off_t size)
{
    WT_EXT *ext, **estack[WT_SKIP_MAXDEPTH];
    WT_SIZE *szp, **sstack[WT_SKIP_MAXDEPTH];

    /* If a sync is running, no other sessions can allocate blocks. */
    WT_ASSERT(session, WT_SESSION_BTREE_SYNC_SAFE(session, S2BT(session)));

    /* Assert we're maintaining the by-size skiplist. */
    WT_ASSERT(session, block->live.avail.track_size != 0);

    WT_STAT_DATA_INCR(session, block_alloc);
    if (size % block->allocsize != 0)
        WT_RET_MSG(session, EINVAL,
          "cannot allocate a block size %" PRIdMAX
          " that is not a multiple of the allocation size %" PRIu32,
          (intmax_t)size, block->allocsize);

    /*
     * Allocation is either first-fit (lowest offset), or best-fit (best size). If it's first-fit,
     * walk the offset list linearly until we find an entry that will work.
     *
     * If it's best-fit by size, search the by-size skiplist for the size and take the first entry
     * on the by-size offset list. This means we prefer best-fit over lower offset, but within a
     * size we'll prefer an offset appearing earlier in the file.
     *
     * If we don't have anything big enough, extend the file.
     */
    if (block->live.avail.bytes < (uint64_t)size)
        goto append;
    if (block->allocfirst) {
        if (!__block_first_srch(block->live.avail.off, size, estack))
            goto append;
        ext = *estack[0];
    } else {
        __block_size_srch(block->live.avail.sz, size, sstack);
        if ((szp = *sstack[0]) == NULL) {
append:
            WT_RET(__block_extend(session, block, offp, size));
            WT_RET(__block_append(session, block, &block->live.alloc, *offp, (wt_off_t)size));
            return (0);
        }

        /* Take the first record. */
        ext = szp->off[0];
    }

    /* Remove the record, and set the returned offset. */
    WT_RET(__block_off_remove(session, block, &block->live.avail, ext->off, &ext));
    *offp = ext->off;

    /* If doing a partial allocation, adjust the record and put it back. */
    if (ext->size > size) {
        __wt_verbose(session, WT_VERB_BLOCK,
          "allocate %" PRIdMAX " from range %" PRIdMAX "-%" PRIdMAX ", range shrinks to %" PRIdMAX
          "-%" PRIdMAX,
          (intmax_t)size, (intmax_t)ext->off, (intmax_t)(ext->off + ext->size),
          (intmax_t)(ext->off + size), (intmax_t)(ext->off + size + ext->size - size));

        ext->off += size;
        ext->size -= size;
        WT_RET(__block_ext_insert(session, &block->live.avail, ext));
    } else {
        __wt_verbose(session, WT_VERB_BLOCK, "allocate range %" PRIdMAX "-%" PRIdMAX,
          (intmax_t)ext->off, (intmax_t)(ext->off + ext->size));

        __wt_block_ext_free(session, ext);
    }

    /* Add the newly allocated extent to the list of allocations. */
    WT_RET(__block_merge(session, block, &block->live.alloc, *offp, (wt_off_t)size));
    return (0);
}

/*
 * __wt_block_free --
 *     Free a cookie-referenced chunk of space to the underlying file.
 */
int
__wt_block_free(WT_SESSION_IMPL *session, WT_BLOCK *block, const uint8_t *addr, size_t addr_size)
{
    WT_DECL_RET;
    wt_off_t offset;
    uint32_t checksum, objectid, size;

    WT_STAT_DATA_INCR(session, block_free);

    /* Crack the cookie. */
    WT_RET(__wt_block_addr_unpack(
      session, block, addr, addr_size, &objectid, &offset, &size, &checksum));

    /* We can't reuse free space in an object. */
    if (objectid != block->objectid)
        return (0);

    __wt_verbose(session, WT_VERB_BLOCK, "free %" PRIu32 ": %" PRIdMAX "/%" PRIdMAX, objectid,
      (intmax_t)offset, (intmax_t)size);

#ifdef HAVE_DIAGNOSTIC
    WT_RET(__wt_block_misplaced(
      session, block, "free", offset, size, true, __PRETTY_FUNCTION__, __LINE__));
#endif

    WT_RET(__wt_block_ext_prealloc(session, 5));
    __wt_spin_lock(session, &block->live_lock);
    ret = __wt_block_off_free(session, block, objectid, offset, (wt_off_t)size);
    __wt_spin_unlock(session, &block->live_lock);

    return (ret);
}

/*
 * __wt_block_off_free --
 *     Free a file range to the underlying file.
 */
int
__wt_block_off_free(
  WT_SESSION_IMPL *session, WT_BLOCK *block, uint32_t objectid, wt_off_t offset, wt_off_t size)
{
    WT_DECL_RET;

    /* If a sync is running, no other sessions can free blocks. */
    WT_ASSERT(session, WT_SESSION_BTREE_SYNC_SAFE(session, S2BT(session)));

    /* We can't reuse free space in an object. */
    if (objectid != block->objectid)
        return (0);

    /*
     * Callers of this function are expected to have already acquired any locks required to
     * manipulate the extent lists.
     *
     * We can reuse this extent immediately if it was allocated during this checkpoint, merge it
     * into the avail list (which slows file growth in workloads including repeated overflow record
     * modification). If this extent is referenced in a previous checkpoint, merge into the discard
     * list.
     */
    if ((ret = __wt_block_off_remove_overlap(session, block, &block->live.alloc, offset, size)) ==
      0)
        ret = __block_merge(session, block, &block->live.avail, offset, size);
    else if (ret == WT_NOTFOUND)
        ret = __block_merge(session, block, &block->live.discard, offset, size);
    return (ret);
}

#ifdef HAVE_DIAGNOSTIC
/*
 * __wt_block_extlist_check --
 *     Return if the extent lists overlap.
 */
int
__wt_block_extlist_check(WT_SESSION_IMPL *session, WT_EXTLIST *al, WT_EXTLIST *bl)
{
    WT_EXT *a, *b;

    a = al->off[0];
    b = bl->off[0];

    /* Walk the lists in parallel, looking for overlaps. */
    while (a != NULL && b != NULL) {
        /*
         * If there's no overlap, move the lower-offset entry to the next entry in its list.
         */
        if (a->off + a->size <= b->off) {
            a = a->next[0];
            continue;
        }
        if (b->off + b->size <= a->off) {
            b = b->next[0];
            continue;
        }
        WT_RET_PANIC(session, EINVAL, "checkpoint merge check: %s list overlaps the %s list",
          al->name, bl->name);
    }
    return (0);
}
#endif

/*
 * __wt_block_extlist_overlap --
 *     Review a checkpoint's alloc/discard extent lists, move overlaps into the live system's
 *     checkpoint-avail list.
 */
int
__wt_block_extlist_overlap(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_BLOCK_CKPT *ci)
{
    WT_EXT *alloc, *discard;

    alloc = ci->alloc.off[0];
    discard = ci->discard.off[0];

    /* Walk the lists in parallel, looking for overlaps. */
    while (alloc != NULL && discard != NULL) {
        /*
         * If there's no overlap, move the lower-offset entry to the next entry in its list.
         */
        if (alloc->off + alloc->size <= discard->off) {
            alloc = alloc->next[0];
            continue;
        }
        if (discard->off + discard->size <= alloc->off) {
            discard = discard->next[0];
            continue;
        }

        /* Reconcile the overlap. */
        WT_RET(__block_ext_overlap(session, block, &ci->alloc, &alloc, &ci->discard, &discard));
    }
    return (0);
}

/*
 * __block_ext_overlap --
 *     Reconcile two overlapping ranges.
 */
static int
__block_ext_overlap(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_EXTLIST *ael, WT_EXT **ap,
  WT_EXTLIST *bel, WT_EXT **bp)
{
    WT_EXT *a, *b, **ext;
    WT_EXTLIST *avail, *el;
    wt_off_t off, size;

    avail = &block->live.ckpt_avail;

    /*
     * The ranges overlap, choose the range we're going to take from each.
     *
     * We can think of the overlap possibilities as 11 different cases:
     *
     *		AAAAAAAAAAAAAAAAAA
     * #1		BBBBBBBBBBBBBBBBBB		ranges are the same
     * #2	BBBBBBBBBBBBB				overlaps the beginning
     * #3			BBBBBBBBBBBBBBBB	overlaps the end
     * #4		BBBBB				B is a prefix of A
     * #5			BBBBBB			B is middle of A
     * #6			BBBBBBBBBB		B is a suffix of A
     *
     * and:
     *
     *		BBBBBBBBBBBBBBBBBB
     * #7	AAAAAAAAAAAAA				same as #3
     * #8			AAAAAAAAAAAAAAAA	same as #2
     * #9		AAAAA				A is a prefix of B
     * #10			AAAAAA			A is middle of B
     * #11			AAAAAAAAAA		A is a suffix of B
     *
     *
     * By swapping the arguments so "A" is always the lower range, we can
     * eliminate cases #2, #8, #10 and #11, and only handle 7 cases:
     *
     *		AAAAAAAAAAAAAAAAAA
     * #1		BBBBBBBBBBBBBBBBBB		ranges are the same
     * #3			BBBBBBBBBBBBBBBB	overlaps the end
     * #4		BBBBB				B is a prefix of A
     * #5			BBBBBB			B is middle of A
     * #6			BBBBBBBBBB		B is a suffix of A
     *
     * and:
     *
     *		BBBBBBBBBBBBBBBBBB
     * #7	AAAAAAAAAAAAA				same as #3
     * #9		AAAAA				A is a prefix of B
     */
    a = *ap;
    b = *bp;
    if (a->off > b->off) { /* Swap */
        b = *ap;
        a = *bp;
        ext = ap;
        ap = bp;
        bp = ext;
        el = ael;
        ael = bel;
        bel = el;
    }

    if (a->off == b->off) {       /* Case #1, #4, #9 */
        if (a->size == b->size) { /* Case #1 */
                                  /*
                                   * Move caller's A and B to the next element Add that A and B
                                   * range to the avail list Delete A and B
                                   */
            *ap = (*ap)->next[0];
            *bp = (*bp)->next[0];
            WT_RET(__block_merge(session, block, avail, b->off, b->size));
            WT_RET(__block_off_remove(session, block, ael, a->off, NULL));
            WT_RET(__block_off_remove(session, block, bel, b->off, NULL));
        } else if (a->size > b->size) { /* Case #4 */
                                        /*
                                         * Remove A from its list Increment/Decrement A's
                                         * offset/size by the size of B Insert A on its list
                                         */
            WT_RET(__block_off_remove(session, block, ael, a->off, &a));
            a->off += b->size;
            a->size -= b->size;
            WT_RET(__block_ext_insert(session, ael, a));

            /*
             * Move caller's B to the next element Add B's range to the avail list Delete B
             */
            *bp = (*bp)->next[0];
            WT_RET(__block_merge(session, block, avail, b->off, b->size));
            WT_RET(__block_off_remove(session, block, bel, b->off, NULL));
        } else { /* Case #9 */
                 /*
                  * Remove B from its list Increment/Decrement B's offset/size by the size of A
                  * Insert B on its list
                  */
            WT_RET(__block_off_remove(session, block, bel, b->off, &b));
            b->off += a->size;
            b->size -= a->size;
            WT_RET(__block_ext_insert(session, bel, b));

            /*
             * Move caller's A to the next element Add A's range to the avail list Delete A
             */
            *ap = (*ap)->next[0];
            WT_RET(__block_merge(session, block, avail, a->off, a->size));
            WT_RET(__block_off_remove(session, block, ael, a->off, NULL));
        } /* Case #6 */
    } else if (a->off + a->size == b->off + b->size) {
        /*
         * Remove A from its list Decrement A's size by the size of B Insert A on its list
         */
        WT_RET(__block_off_remove(session, block, ael, a->off, &a));
        a->size -= b->size;
        WT_RET(__block_ext_insert(session, ael, a));

        /*
         * Move caller's B to the next element Add B's range to the avail list Delete B
         */
        *bp = (*bp)->next[0];
        WT_RET(__block_merge(session, block, avail, b->off, b->size));
        WT_RET(__block_off_remove(session, block, bel, b->off, NULL));
    } else if /* Case #3, #7 */
      (a->off + a->size < b->off + b->size) {
        /*
         * Add overlap to the avail list
         */
        off = b->off;
        size = (a->off + a->size) - b->off;
        WT_RET(__block_merge(session, block, avail, off, size));

        /*
         * Remove A from its list Decrement A's size by the overlap Insert A on its list
         */
        WT_RET(__block_off_remove(session, block, ael, a->off, &a));
        a->size -= size;
        WT_RET(__block_ext_insert(session, ael, a));

        /*
         * Remove B from its list Increment/Decrement B's offset/size by the overlap Insert B on its
         * list
         */
        WT_RET(__block_off_remove(session, block, bel, b->off, &b));
        b->off += size;
        b->size -= size;
        WT_RET(__block_ext_insert(session, bel, b));
    } else { /* Case #5 */
        /* Calculate the offset/size of the trailing part of A. */
        off = b->off + b->size;
        size = (a->off + a->size) - off;

        /*
         * Remove A from its list Decrement A's size by trailing part of A plus B's size Insert A on
         * its list
         */
        WT_RET(__block_off_remove(session, block, ael, a->off, &a));
        a->size = b->off - a->off;
        WT_RET(__block_ext_insert(session, ael, a));

        /* Add trailing part of A to A's list as a new element. */
        WT_RET(__block_merge(session, block, ael, off, size));

        /*
         * Move caller's B to the next element Add B's range to the avail list Delete B
         */
        *bp = (*bp)->next[0];
        WT_RET(__block_merge(session, block, avail, b->off, b->size));
        WT_RET(__block_off_remove(session, block, bel, b->off, NULL));
    }

    return (0);
}

/*
 * __wt_block_extlist_merge --
 *     Merge one extent list into another.
 */
int
__wt_block_extlist_merge(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_EXTLIST *a, WT_EXTLIST *b)
{
    WT_EXT *ext;
    WT_EXTLIST tmp;
    u_int i;

    __wt_verbose(session, WT_VERB_BLOCK, "merging %s into %s", a->name, b->name);

    /*
     * Sometimes the list we are merging is much bigger than the other: if so, swap the lists around
     * to reduce the amount of work we need to do during the merge. The size lists have to match as
     * well, so this is only possible if both lists are tracking sizes, or neither are.
     */
    if (a->track_size == b->track_size && a->entries > b->entries) {
        tmp = *a;
        a->bytes = b->bytes;
        b->bytes = tmp.bytes;
        a->entries = b->entries;
        b->entries = tmp.entries;
        for (i = 0; i < WT_SKIP_MAXDEPTH; i++) {
            a->off[i] = b->off[i];
            b->off[i] = tmp.off[i];
            a->sz[i] = b->sz[i];
            b->sz[i] = tmp.sz[i];
        }
    }

    WT_EXT_FOREACH (ext, a->off)
        WT_RET(__block_merge(session, block, b, ext->off, ext->size));

    return (0);
}

/*
 * __block_append --
 *     Append a new entry to the allocation list.
 */
static int
__block_append(
  WT_SESSION_IMPL *session, WT_BLOCK *block, WT_EXTLIST *el, wt_off_t off, wt_off_t size)
{
    WT_EXT *ext, **astack[WT_SKIP_MAXDEPTH];
    u_int i;

    WT_UNUSED(block);
    WT_ASSERT(session, el->track_size == 0);

    /*
     * Identical to __block_merge, when we know the file is being extended, that is, the information
     * is either going to be used to extend the last object on the list, or become a new object
     * ending the list.
     *
     * The terminating element of the list is cached, check it; otherwise, get a stack for the last
     * object in the skiplist, check for a simple extension, and otherwise append a new structure.
     */
    if ((ext = el->last) != NULL && ext->off + ext->size == off)
        ext->size += size;
    else {
        ext = __block_off_srch_last(el->off, astack);
        if (ext != NULL && ext->off + ext->size == off)
            ext->size += size;
        else {
            WT_RET(__wt_block_ext_alloc(session, &ext));
            ext->off = off;
            ext->size = size;

            for (i = 0; i < ext->depth; ++i)
                *astack[i] = ext;
            ++el->entries;
        }

        /* Update the cached end-of-list */
        el->last = ext;
    }
    el->bytes += (uint64_t)size;

    return (0);
}

/*
 * __wt_block_insert_ext --
 *     Insert an extent into an extent list, merging if possible.
 */
int
__wt_block_insert_ext(
  WT_SESSION_IMPL *session, WT_BLOCK *block, WT_EXTLIST *el, wt_off_t off, wt_off_t size)
{
    /*
     * There are currently two copies of this function (this code is a one- liner that calls the
     * internal version of the function, which means the compiler should compress out the function
     * call). It's that way because the interface is still fluid, I'm not convinced there won't be a
     * need for a functional split between the internal and external versions in the future.
     *
     * Callers of this function are expected to have already acquired any locks required to
     * manipulate the extent list.
     */
    return (__block_merge(session, block, el, off, size));
}

/*
 * __block_merge --
 *     Insert an extent into an extent list, merging if possible (internal version).
 */
static int
__block_merge(
  WT_SESSION_IMPL *session, WT_BLOCK *block, WT_EXTLIST *el, wt_off_t off, wt_off_t size)
{
    WT_EXT *ext, *after, *before;

    /*
     * Retrieve the records preceding/following the offset. If the records are contiguous with the
     * free'd offset, combine records.
     */
    __block_off_srch_pair(el, off, &before, &after);
    if (before != NULL) {
        if (before->off + before->size > off)
            WT_BLOCK_RET(session, block, EINVAL,
              "%s: existing range %" PRIdMAX "-%" PRIdMAX " overlaps with merge range %" PRIdMAX
              "-%" PRIdMAX,
              el->name, (intmax_t)before->off, (intmax_t)(before->off + before->size),
              (intmax_t)off, (intmax_t)(off + size));
        if (before->off + before->size != off)
            before = NULL;
    }
    if (after != NULL) {
        if (off + size > after->off) {
            WT_BLOCK_RET(session, block, EINVAL,
              "%s: merge range %" PRIdMAX "-%" PRIdMAX " overlaps with existing range %" PRIdMAX
              "-%" PRIdMAX,
              el->name, (intmax_t)off, (intmax_t)(off + size), (intmax_t)after->off,
              (intmax_t)(after->off + after->size));
        }
        if (off + size != after->off)
            after = NULL;
    }
    if (before == NULL && after == NULL) {
        __wt_verbose(session, WT_VERB_BLOCK, "%s: insert range %" PRIdMAX "-%" PRIdMAX, el->name,
          (intmax_t)off, (intmax_t)(off + size));

        return (__block_off_insert(session, el, off, size));
    }

    /*
     * If the "before" offset range abuts, we'll use it as our new record; if the "after" offset
     * range also abuts, include its size and remove it from the system. Else, only the "after"
     * offset range abuts, use the "after" offset range as our new record. In either case, remove
     * the record we're going to use, adjust it and re-insert it.
     */
    if (before == NULL) {
        WT_RET(__block_off_remove(session, block, el, after->off, &ext));

        __wt_verbose(session, WT_VERB_BLOCK,
          "%s: range grows from %" PRIdMAX "-%" PRIdMAX ", to %" PRIdMAX "-%" PRIdMAX, el->name,
          (intmax_t)ext->off, (intmax_t)(ext->off + ext->size), (intmax_t)off,
          (intmax_t)(off + ext->size + size));

        ext->off = off;
        ext->size += size;
    } else {
        if (after != NULL) {
            size += after->size;
            WT_RET(__block_off_remove(session, block, el, after->off, NULL));
        }
        WT_RET(__block_off_remove(session, block, el, before->off, &ext));

        __wt_verbose(session, WT_VERB_BLOCK,
          "%s: range grows from %" PRIdMAX "-%" PRIdMAX ", to %" PRIdMAX "-%" PRIdMAX, el->name,
          (intmax_t)ext->off, (intmax_t)(ext->off + ext->size), (intmax_t)ext->off,
          (intmax_t)(ext->off + ext->size + size));

        ext->size += size;
    }
    return (__block_ext_insert(session, el, ext));
}

/*
 * __wt_block_extlist_read_avail --
 *     Read an avail extent list, includes minor special handling.
 */
int
__wt_block_extlist_read_avail(
  WT_SESSION_IMPL *session, WT_BLOCK *block, WT_EXTLIST *el, wt_off_t ckpt_size)
{
    WT_DECL_RET;

    /* If there isn't a list, we're done. */
    if (el->offset == WT_BLOCK_INVALID_OFFSET)
        return (0);

#ifdef HAVE_DIAGNOSTIC
    /*
     * In diagnostic mode, reads are checked against the available and discard lists (a block being
     * read should never appear on either). Checkpoint threads may be running in the file, don't
     * race with them.
     */
    __wt_spin_lock(session, &block->live_lock);
#endif

    WT_ERR(__wt_block_extlist_read(session, block, el, ckpt_size));

    /*
     * Extent blocks are allocated from the available list: if reading the avail list, the extent
     * blocks might be included, remove them.
     */
    WT_ERR_NOTFOUND_OK(
      __wt_block_off_remove_overlap(session, block, el, el->offset, el->size), false);

err:
#ifdef HAVE_DIAGNOSTIC
    __wt_spin_unlock(session, &block->live_lock);
#endif

    return (ret);
}

/*
 * __wt_block_extlist_read --
 *     Read an extent list.
 */
int
__wt_block_extlist_read(
  WT_SESSION_IMPL *session, WT_BLOCK *block, WT_EXTLIST *el, wt_off_t ckpt_size)
{
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;
    wt_off_t off, size;
    const uint8_t *p;
    int (*func)(WT_SESSION_IMPL *, WT_BLOCK *, WT_EXTLIST *, wt_off_t, wt_off_t);

    /* If there isn't a list, we're done. */
    if (el->offset == WT_BLOCK_INVALID_OFFSET)
        return (0);

    WT_RET(__wt_scr_alloc(session, el->size, &tmp));
    WT_ERR(
      __wt_block_read_off(session, block, tmp, el->objectid, el->offset, el->size, el->checksum));

    p = WT_BLOCK_HEADER_BYTE(tmp->mem);
    WT_ERR(__wt_extlist_read_pair(&p, &off, &size));
    if (off != WT_BLOCK_EXTLIST_MAGIC || size != 0)
        goto corrupted;

    /*
     * If we're not creating both offset and size skiplists, use the simpler append API, otherwise
     * do a full merge. There are two reasons for the test: first, checkpoint "available" lists are
     * NOT sorted (checkpoints write two separate lists, both of which are sorted but they're not
     * merged). Second, the "available" list is sorted by size as well as by offset, and the
     * fast-path append code doesn't support that, it's limited to offset. The test of "track size"
     * is short-hand for "are we reading the available-blocks list".
     */
    func = el->track_size == 0 ? __block_append : __block_merge;
    for (;;) {
        WT_ERR(__wt_extlist_read_pair(&p, &off, &size));
        if (off == WT_BLOCK_INVALID_OFFSET)
            break;

        /*
         * We check the offset/size pairs represent valid file ranges, then insert them into the
         * list. We don't necessarily have to check for offsets past the end of the checkpoint, but
         * it's a cheap test to do here and we'd have to do the check as part of file verification,
         * regardless.
         */
        if (off < block->allocsize || off % block->allocsize != 0 || size % block->allocsize != 0 ||
          off + size > ckpt_size) {
corrupted:
            __wt_scr_free(session, &tmp);
            WT_BLOCK_RET(session, block, WT_ERROR,
              "file contains a corrupted %s extent list, range %" PRIdMAX "-%" PRIdMAX
              " past end-of-file",
              el->name, (intmax_t)off, (intmax_t)(off + size));
        }

        WT_ERR(func(session, block, el, off, size));
    }

    WT_ERR(__block_extlist_dump(session, block, el, "read"));

err:
    __wt_scr_free(session, &tmp);
    return (ret);
}

/*
 * __wt_block_extlist_write --
 *     Write an extent list at the tail of the file.
 */
int
__wt_block_extlist_write(
  WT_SESSION_IMPL *session, WT_BLOCK *block, WT_EXTLIST *el, WT_EXTLIST *additional)
{
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;
    WT_EXT *ext;
    WT_PAGE_HEADER *dsk;
    size_t size;
    uint32_t entries;
    uint8_t *p;

    WT_RET(__block_extlist_dump(session, block, el, "write"));

    /*
     * Figure out how many entries we're writing -- if there aren't any entries, there's nothing to
     * write, unless we still have to write the extent list to include the checkpoint recovery
     * information.
     */
    entries = el->entries + (additional == NULL ? 0 : additional->entries);
    if (entries == 0 && block->final_ckpt == NULL) {
        el->offset = WT_BLOCK_INVALID_OFFSET;
        el->checksum = el->size = 0;
        return (0);
    }

    /*
     * Get a scratch buffer, clear the page's header and data, initialize the header.
     *
     * Allocate memory for the extent list entries plus two additional entries: the initial
     * WT_BLOCK_EXTLIST_MAGIC/0 pair and the list- terminating WT_BLOCK_INVALID_OFFSET/0 pair.
     */
    size = ((size_t)entries + 2) * 2 * WT_INTPACK64_MAXSIZE;
    WT_RET(__wt_block_write_size(session, block, &size));
    WT_RET(__wt_scr_alloc(session, size, &tmp));
    dsk = tmp->mem;
    memset(dsk, 0, WT_BLOCK_HEADER_BYTE_SIZE);
    dsk->type = WT_PAGE_BLOCK_MANAGER;
    dsk->version = WT_PAGE_VERSION_TS;

    /* Fill the page's data. */
    p = WT_BLOCK_HEADER_BYTE(dsk);
    /* Extent list starts */
    WT_ERR(__wt_extlist_write_pair(&p, WT_BLOCK_EXTLIST_MAGIC, 0));
    WT_EXT_FOREACH (ext, el->off) /* Free ranges */
        WT_ERR(__wt_extlist_write_pair(&p, ext->off, ext->size));
    if (additional != NULL)
        WT_EXT_FOREACH (ext, additional->off) /* Free ranges */
            WT_ERR(__wt_extlist_write_pair(&p, ext->off, ext->size));
    /* Extent list stops */
    WT_ERR(__wt_extlist_write_pair(
      &p, WT_BLOCK_INVALID_OFFSET, block->final_ckpt == NULL ? 0 : WT_BLOCK_EXTLIST_VERSION_CKPT));

    dsk->u.datalen = WT_PTRDIFF32(p, WT_BLOCK_HEADER_BYTE(dsk));
    tmp->size = dsk->mem_size = WT_PTRDIFF32(p, dsk);

#ifdef HAVE_DIAGNOSTIC
    /*
     * The extent list is written as a valid btree page because the salvage functionality might move
     * into the btree layer some day, besides, we don't need another format and this way the page
     * format can be easily verified.
     */
    WT_ERR(__wt_verify_dsk(session, "[extent list check]", tmp));
#endif

    /* Write the extent list to disk. */
    WT_ERR(__wt_block_write_off(
      session, block, tmp, &el->objectid, &el->offset, &el->size, &el->checksum, true, true, true));

    /*
     * Remove the allocated blocks from the system's allocation list, extent blocks never appear on
     * any allocation list.
     */
    WT_TRET(
      __wt_block_off_remove_overlap(session, block, &block->live.alloc, el->offset, el->size));

    __wt_verbose(session, WT_VERB_BLOCK, "%s written %" PRIdMAX "/%" PRIu32, el->name,
      (intmax_t)el->offset, el->size);

err:
    __wt_scr_free(session, &tmp);
    return (ret);
}

/*
 * __wt_block_extlist_truncate --
 *     Truncate the file based on the last available extent in the list.
 */
int
__wt_block_extlist_truncate(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_EXTLIST *el)
{
    WT_EXT *ext, **astack[WT_SKIP_MAXDEPTH];
    wt_off_t size;

    /*
     * Check if the last available extent is at the end of the file, and if so, truncate the file
     * and discard the extent.
     */
    if ((ext = __block_off_srch_last(el->off, astack)) == NULL)
        return (0);
    WT_ASSERT(session, ext->off + ext->size <= block->size);
    if (ext->off + ext->size < block->size)
        return (0);

    /*
     * Remove the extent list entry. (Save the value, we need it to reset the cached file size, and
     * that can't happen until after the extent list removal succeeds.)
     */
    size = ext->off;
    WT_RET(__block_off_remove(session, block, el, size, NULL));

    /* Truncate the file. */
    return (__wt_block_truncate(session, block, size));
}

/*
 * __wt_block_extlist_init --
 *     Initialize an extent list.
 */
int
__wt_block_extlist_init(
  WT_SESSION_IMPL *session, WT_EXTLIST *el, const char *name, const char *extname, bool track_size)
{
    size_t size;

    WT_CLEAR(*el);

    size =
      (name == NULL ? 0 : strlen(name)) + strlen(".") + (extname == NULL ? 0 : strlen(extname) + 1);
    WT_RET(__wt_calloc_def(session, size, &el->name));
    WT_RET(__wt_snprintf(
      el->name, size, "%s.%s", name == NULL ? "" : name, extname == NULL ? "" : extname));

    el->offset = WT_BLOCK_INVALID_OFFSET;
    el->track_size = track_size;
    return (0);
}

/*
 * __wt_block_extlist_free --
 *     Discard an extent list.
 */
void
__wt_block_extlist_free(WT_SESSION_IMPL *session, WT_EXTLIST *el)
{
    WT_EXT *ext, *next;
    WT_SIZE *szp, *nszp;

    __wt_free(session, el->name);

    for (ext = el->off[0]; ext != NULL; ext = next) {
        next = ext->next[0];
        __wt_free(session, ext);
    }
    for (szp = el->sz[0]; szp != NULL; szp = nszp) {
        nszp = szp->next[0];
        __wt_free(session, szp);
    }

    /* Extent lists are re-used, clear them. */
    WT_CLEAR(*el);
}

/*
 * __block_extlist_dump --
 *     Dump an extent list as verbose messages.
 */
static int
__block_extlist_dump(WT_SESSION_IMPL *session, WT_BLOCK *block, WT_EXTLIST *el, const char *tag)
{
    WT_DECL_ITEM(t1);
    WT_DECL_ITEM(t2);
    WT_DECL_RET;
    WT_EXT *ext;
    WT_VERBOSE_LEVEL level;
    uint64_t pow, sizes[64];
    u_int i;
    const char *sep;

    if (!block->verify_layout && !WT_VERBOSE_ISSET(session, WT_VERB_BLOCK))
        return (0);

    WT_ERR(__wt_scr_alloc(session, 0, &t1));
    if (block->verify_layout)
        level = WT_VERBOSE_NOTICE;
    else
        level = WT_VERBOSE_DEBUG;
    __wt_verbose_level(session, WT_VERB_BLOCK, level,
      "%s extent list %s, %" PRIu32 " entries, %s bytes", tag, el->name, el->entries,
      __wt_buf_set_size(session, el->bytes, true, t1));

    if (el->entries == 0)
        goto done;

    memset(sizes, 0, sizeof(sizes));
    WT_EXT_FOREACH (ext, el->off)
        for (i = 9, pow = 512;; ++i, pow *= 2)
            if (ext->size <= (wt_off_t)pow) {
                ++sizes[i];
                break;
            }
    sep = "extents by bucket:";
    t1->size = 0;
    WT_ERR(__wt_scr_alloc(session, 0, &t2));
    for (i = 9, pow = 512; i < WT_ELEMENTS(sizes); ++i, pow *= 2)
        if (sizes[i] != 0) {
            WT_ERR(__wt_buf_catfmt(session, t1, "%s {%s: %" PRIu64 "}", sep,
              __wt_buf_set_size(session, pow, false, t2), sizes[i]));
            sep = ",";
        }

    __wt_verbose_level(session, WT_VERB_BLOCK, level, "%s", (char *)t1->data);

done:
err:
    __wt_scr_free(session, &t1);
    __wt_scr_free(session, &t2);
    return (ret);
}

#ifdef HAVE_UNITTEST
WT_EXT *
__ut_block_off_srch_last(WT_EXT **head, WT_EXT ***stack)
{
    return (__block_off_srch_last(head, stack));
}

void
__ut_block_off_srch(WT_EXT **head, wt_off_t off, WT_EXT ***stack, bool skip_off)
{
    __block_off_srch(head, off, stack, skip_off);
}

bool
__ut_block_first_srch(WT_EXT **head, wt_off_t size, WT_EXT ***stack)
{
    return (__block_first_srch(head, size, stack));
}

void
__ut_block_size_srch(WT_SIZE **head, wt_off_t size, WT_SIZE ***stack)
{
    __block_size_srch(head, size, stack);
}
#endif
