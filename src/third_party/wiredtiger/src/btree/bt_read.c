/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __evict_force_check --
 *     Check if a page matches the criteria for forced eviction.
 */
static bool
__evict_force_check(WT_SESSION_IMPL *session, WT_REF *ref)
{
    WT_BTREE *btree;
    WT_PAGE *page;
    size_t footprint;

    btree = S2BT(session);
    page = ref->page;

    /* Leaf pages only. */
    if (F_ISSET(ref, WT_REF_FLAG_INTERNAL))
        return (false);

    /*
     * It's hard to imagine a page with a huge memory footprint that has never been modified, but
     * check to be sure.
     */
    if (__wt_page_evict_clean(page))
        return (false);

    /*
     * Exclude the disk image size from the footprint checks.  Usually the
     * disk image size is small compared with the in-memory limit (e.g.
     * 16KB vs 5MB), so this doesn't make a big difference.  Where it is
     * important is for pages with a small number of large values, where
     * the disk image size takes into account large values that have
     * already been written and should not trigger forced eviction.
     */
    footprint = page->memory_footprint;
    if (page->dsk != NULL)
        footprint -= page->dsk->mem_size;

    /* Pages are usually small enough, check that first. */
    if (footprint < btree->splitmempage)
        return (false);

    /*
     * If this session has more than one hazard pointer, eviction will fail and there is no point
     * trying.
     */
    if (__wt_hazard_count(session, ref) > 1)
        return (false);

    /* If we can do an in-memory split, do it. */
    if (__wt_leaf_page_can_split(session, page))
        return (true);
    if (footprint < btree->maxmempage)
        return (false);

    /* Bump the oldest ID, we're about to do some visibility checks. */
    WT_IGNORE_RET(__wt_txn_update_oldest(session, 0));

    /*
     * Allow some leeway if the transaction ID isn't moving forward since it is unlikely eviction
     * will be able to evict the page. Don't keep skipping the page indefinitely or large records
     * can lead to extremely large memory footprints.
     */
    if (!__wt_page_evict_retry(session, page))
        return (false);

    /* Trigger eviction on the next page release. */
    __wt_page_evict_soon(session, ref);

    /* If eviction cannot succeed, don't try. */
    return (__wt_page_can_evict(session, ref, NULL));
}

/*
 * __page_read --
 *     Read a page from the file.
 */
static int
__page_read(WT_SESSION_IMPL *session, WT_REF *ref, uint32_t flags)
{
    WT_ADDR_COPY addr;
    WT_DECL_RET;
    WT_ITEM tmp;
    WT_PAGE *notused;
    uint32_t page_flags;
    uint8_t previous_state;
    bool prepare;

    /*
     * Don't pass an allocated buffer to the underlying block read function, force allocation of new
     * memory of the appropriate size.
     */
    WT_CLEAR(tmp);

    /* Lock the WT_REF. */
    switch (previous_state = ref->state) {
    case WT_REF_DISK:
    case WT_REF_DELETED:
        if (WT_REF_CAS_STATE(session, ref, previous_state, WT_REF_LOCKED))
            break;
        return (0);
    default:
        return (0);
    }

    /*
     * Set the WT_REF_FLAG_READING flag for normal reads. Checkpoints can skip over clean pages
     * being read into cache, but need to wait for deletes to be resolved (in order for checkpoint
     * to write the correct version of the page).
     */
    if (previous_state == WT_REF_DISK)
        F_SET(ref, WT_REF_FLAG_READING);

    /*
     * Get the address: if there is no address, the page was deleted and a subsequent search or
     * insert is forcing re-creation of the name space.
     */
    if (!__wt_ref_addr_copy(session, ref, &addr)) {
        WT_ASSERT(session, previous_state == WT_REF_DELETED);

        WT_ERR(__wt_btree_new_leaf_page(session, ref));
        goto skip_read;
    }

    /* There's an address, read the backing disk page and build an in-memory version of the page. */
    WT_ERR(__wt_blkcache_read(session, &tmp, addr.addr, addr.size));

    /*
     * Build the in-memory version of the page. Clear our local reference to the allocated copy of
     * the disk image on return, the in-memory object steals it.
     *
     * If a page is read with eviction disabled, we don't count evicting it as progress. Since
     * disabling eviction allows pages to be read even when the cache is full, we want to avoid
     * workloads repeatedly reading a page with eviction disabled (e.g., a metadata page), then
     * evicting that page and deciding that is a sign that eviction is unstuck.
     */
    page_flags = WT_DATA_IN_ITEM(&tmp) ? WT_PAGE_DISK_ALLOC : WT_PAGE_DISK_MAPPED;
    if (LF_ISSET(WT_READ_IGNORE_CACHE_SIZE))
        FLD_SET(page_flags, WT_PAGE_EVICT_NO_PROGRESS);
    WT_ERR(__wt_page_inmem(session, ref, tmp.data, page_flags, &notused, &prepare));
    tmp.mem = NULL;
    if (prepare)
        WT_ERR(__wt_page_inmem_prepare(session, ref));

skip_read:
    switch (previous_state) {
    case WT_REF_DELETED:
        /* Move all records to a deleted state. */
        WT_ERR(__wt_delete_page_instantiate(session, ref));
        break;
    }

    F_CLR(ref, WT_REF_FLAG_READING);
    WT_REF_SET_STATE(ref, WT_REF_MEM);

    WT_ASSERT(session, ret == 0);
    return (0);

err:
    /*
     * If the function building an in-memory version of the page failed, it discarded the page, but
     * not the disk image. Discard the page and separately discard the disk image in all cases.
     */
    if (ref->page != NULL)
        __wt_ref_out(session, ref);

    F_CLR(ref, WT_REF_FLAG_READING);
    WT_REF_SET_STATE(ref, previous_state);

    __wt_buf_free(session, &tmp);

    return (ret);
}

/*
 * __wt_page_in_func --
 *     Acquire a hazard pointer to a page; if the page is not in-memory, read it from the disk and
 *     build an in-memory version.
 */
int
__wt_page_in_func(WT_SESSION_IMPL *session, WT_REF *ref, uint32_t flags
#ifdef HAVE_DIAGNOSTIC
  ,
  const char *func, int line
#endif
)
{
    WT_BTREE *btree;
    WT_DECL_RET;
    WT_PAGE *page;
    uint64_t sleep_usecs, yield_cnt;
    uint8_t current_state;
    int force_attempts;
    bool busy, cache_work, evict_skip, stalled, wont_need;

    btree = S2BT(session);

    if (F_ISSET(session, WT_SESSION_IGNORE_CACHE_SIZE))
        LF_SET(WT_READ_IGNORE_CACHE_SIZE);

    /* Sanity check flag combinations. */
    WT_ASSERT(session,
      !LF_ISSET(WT_READ_DELETED_SKIP) || !LF_ISSET(WT_READ_NO_WAIT) || LF_ISSET(WT_READ_CACHE));
    WT_ASSERT(session, !LF_ISSET(WT_READ_DELETED_CHECK) || !LF_ISSET(WT_READ_DELETED_SKIP));

    /*
     * Ignore reads of pages already known to be in cache, otherwise the eviction server can
     * dominate these statistics.
     */
    if (!LF_ISSET(WT_READ_CACHE))
        WT_STAT_CONN_DATA_INCR(session, cache_pages_requested);

    for (evict_skip = stalled = wont_need = false, force_attempts = 0, sleep_usecs = yield_cnt = 0;
         ;) {
        switch (current_state = ref->state) {
        case WT_REF_DELETED:
            if (LF_ISSET(WT_READ_DELETED_SKIP | WT_READ_NO_WAIT))
                return (WT_NOTFOUND);
            if (LF_ISSET(WT_READ_DELETED_CHECK) &&
              __wt_delete_page_skip(session, ref, !F_ISSET(session->txn, WT_TXN_HAS_SNAPSHOT)))
                return (WT_NOTFOUND);
            goto read;
        case WT_REF_DISK:
            /* Optionally limit reads to cache-only. */
            if (LF_ISSET(WT_READ_CACHE))
                return (WT_NOTFOUND);
read:
            /*
             * The page isn't in memory, read it. If this thread respects the cache size, check for
             * space in the cache.
             */
            if (!LF_ISSET(WT_READ_IGNORE_CACHE_SIZE))
                WT_RET(__wt_cache_eviction_check(
                  session, true, !F_ISSET(session->txn, WT_TXN_HAS_ID), NULL));
            WT_RET(__page_read(session, ref, flags));

            /* We just read a page, don't evict it before we have a chance to use it. */
            evict_skip = true;
            F_CLR(session->dhandle, WT_DHANDLE_EVICTED);

            /*
             * If configured to not trash the cache, leave the page generation unset, we'll set it
             * before returning to the oldest read generation, so the page is forcibly evicted as
             * soon as possible. We don't do that set here because we don't want to evict the page
             * before we "acquire" it.
             */
            wont_need = LF_ISSET(WT_READ_WONT_NEED) ||
              F_ISSET(session, WT_SESSION_READ_WONT_NEED) ||
              F_ISSET(S2C(session)->cache, WT_CACHE_EVICT_NOKEEP);
            continue;
        case WT_REF_LOCKED:
            if (LF_ISSET(WT_READ_NO_WAIT))
                return (WT_NOTFOUND);

            if (F_ISSET(ref, WT_REF_FLAG_READING)) {
                if (LF_ISSET(WT_READ_CACHE))
                    return (WT_NOTFOUND);

                /* Waiting on another thread's read, stall. */
                WT_STAT_CONN_INCR(session, page_read_blocked);
            } else
                /* Waiting on eviction, stall. */
                WT_STAT_CONN_INCR(session, page_locked_blocked);

            stalled = true;
            break;
        case WT_REF_SPLIT:
            return (WT_RESTART);
        case WT_REF_MEM:
            /*
             * The page is in memory.
             *
             * Get a hazard pointer if one is required. We cannot be evicting if no hazard pointer
             * is required, we're done.
             */
            if (F_ISSET(btree, WT_BTREE_IN_MEMORY))
                goto skip_evict;

/*
 * The expected reason we can't get a hazard pointer is because the page is being evicted, yield,
 * try again.
 */
#ifdef HAVE_DIAGNOSTIC
            WT_RET(__wt_hazard_set_func(session, ref, &busy, func, line));
#else
            WT_RET(__wt_hazard_set_func(session, ref, &busy));
#endif
            if (busy) {
                WT_STAT_CONN_INCR(session, page_busy_blocked);
                break;
            }

            /*
             * If a page has grown too large, we'll try and forcibly evict it before making it
             * available to the caller. There are a variety of cases where that's not possible.
             * Don't involve a thread resolving a transaction in forced eviction, they're usually
             * making the problem better.
             */
            if (evict_skip || F_ISSET(session, WT_SESSION_RESOLVING_TXN) ||
              LF_ISSET(WT_READ_NO_SPLIT) || btree->evict_disabled > 0 || btree->lsm_primary)
                goto skip_evict;

            /*
             * If reconciliation is disabled (e.g., when inserting into the history store table),
             * skip forced eviction if the page can't split.
             */
            if (F_ISSET(session, WT_SESSION_NO_RECONCILE) &&
              !__wt_leaf_page_can_split(session, ref->page))
                goto skip_evict;

            /*
             * Forcibly evict pages that are too big.
             */
            if (force_attempts < 10 && __evict_force_check(session, ref)) {
                ++force_attempts;
                ret = __wt_page_release_evict(session, ref, 0);
                /*
                 * If forced eviction succeeded, don't retry. If it failed, stall.
                 */
                if (ret == 0)
                    evict_skip = true;
                else if (ret == EBUSY) {
                    WT_NOT_READ(ret, 0);
                    WT_STAT_CONN_INCR(session, page_forcible_evict_blocked);
                    stalled = true;
                    break;
                }
                WT_RET(ret);

                /*
                 * The result of a successful forced eviction is a page-state transition
                 * (potentially to an in-memory page we can use, or a restart return for our
                 * caller), continue the outer page-acquisition loop.
                 */
                continue;
            }

skip_evict:
            /*
             * If we read the page and are configured to not trash the cache, and no other thread
             * has already used the page, set the read generation so the page is evicted soon.
             *
             * Otherwise, if we read the page, or, if configured to update the page's read
             * generation and the page isn't already flagged for forced eviction, update the page
             * read generation.
             */
            page = ref->page;
            if (page->read_gen == WT_READGEN_NOTSET) {
                if (wont_need)
                    page->read_gen = WT_READGEN_WONT_NEED;
                else
                    __wt_cache_read_gen_new(session, page);
            } else if (!LF_ISSET(WT_READ_NO_GEN))
                __wt_cache_read_gen_bump(session, page);

            /*
             * Check if we need an autocommit transaction. Starting a transaction can trigger
             * eviction, so skip it if eviction isn't permitted.
             *
             * The logic here is a little weird: some code paths do a blanket ban on checking the
             * cache size in sessions, but still require a transaction (e.g., when updating metadata
             * or the history store). If WT_READ_IGNORE_CACHE_SIZE was passed in explicitly, we're
             * done. If we set WT_READ_IGNORE_CACHE_SIZE because it was set in the session then make
             * sure we start a transaction.
             */
            return (LF_ISSET(WT_READ_IGNORE_CACHE_SIZE) &&
                  !F_ISSET(session, WT_SESSION_IGNORE_CACHE_SIZE) ?
                0 :
                __wt_txn_autocommit_check(session));
        default:
            return (__wt_illegal_value(session, current_state));
        }

        /*
         * We failed to get the page -- yield before retrying, and if we've yielded enough times,
         * start sleeping so we don't burn CPU to no purpose.
         */
        if (yield_cnt < WT_THOUSAND) {
            if (!stalled) {
                ++yield_cnt;
                __wt_yield();
                continue;
            }
            yield_cnt = WT_THOUSAND;
        }

        /*
         * If stalling and this thread is allowed to do eviction work, check if the cache needs help
         * evicting clean pages (don't force a read to do dirty eviction). If we do work for the
         * cache, substitute that for a sleep.
         */
        if (!LF_ISSET(WT_READ_IGNORE_CACHE_SIZE)) {
            WT_RET(__wt_cache_eviction_check(session, true, true, &cache_work));
            if (cache_work)
                continue;
        }
        __wt_spin_backoff(&yield_cnt, &sleep_usecs);
        WT_STAT_CONN_INCRV(session, page_sleep, sleep_usecs);
    }
}
