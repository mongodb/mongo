/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __inmem_col_fix(WT_SESSION_IMPL *, WT_PAGE *, bool *, size_t *);
static void __inmem_col_int(WT_SESSION_IMPL *, WT_PAGE *);
static int __inmem_col_var(WT_SESSION_IMPL *, WT_PAGE *, uint64_t, bool *, size_t *);
static int __inmem_row_int(WT_SESSION_IMPL *, WT_PAGE *, size_t *);
static int __inmem_row_leaf(WT_SESSION_IMPL *, WT_PAGE *, bool *);
static int __inmem_row_leaf_entries(WT_SESSION_IMPL *, const WT_PAGE_HEADER *, uint32_t *);

/*
 * __wt_page_alloc --
 *     Create or read a page into the cache.
 */
int
__wt_page_alloc(
  WT_SESSION_IMPL *session, uint8_t type, uint32_t alloc_entries, bool alloc_refs, WT_PAGE **pagep)
{
    WT_CACHE *cache;
    WT_DECL_RET;
    WT_PAGE *page;
    WT_PAGE_INDEX *pindex;
    size_t size;
    uint32_t i;
    void *p;

    *pagep = NULL;

    cache = S2C(session)->cache;
    page = NULL;

    size = sizeof(WT_PAGE);
    switch (type) {
    case WT_PAGE_COL_FIX:
    case WT_PAGE_COL_INT:
    case WT_PAGE_ROW_INT:
        break;
    case WT_PAGE_COL_VAR:
        /*
         * Variable-length column-store leaf page: allocate memory to describe the page's contents
         * with the initial allocation.
         */
        size += alloc_entries * sizeof(WT_COL);
        break;
    case WT_PAGE_ROW_LEAF:
        /*
         * Row-store leaf page: allocate memory to describe the page's contents with the initial
         * allocation.
         */
        size += alloc_entries * sizeof(WT_ROW);
        break;
    default:
        return (__wt_illegal_value(session, type));
    }

    WT_RET(__wt_calloc(session, 1, size, &page));

    page->type = type;
    page->read_gen = WT_READGEN_NOTSET;

    switch (type) {
    case WT_PAGE_COL_FIX:
        page->entries = alloc_entries;
        break;
    case WT_PAGE_COL_INT:
    case WT_PAGE_ROW_INT:
        WT_ASSERT(session, alloc_entries != 0);
        /*
         * Internal pages have an array of references to objects so they can split. Allocate the
         * array of references and optionally, the objects to which they point.
         */
        WT_ERR(
          __wt_calloc(session, 1, sizeof(WT_PAGE_INDEX) + alloc_entries * sizeof(WT_REF *), &p));
        size += sizeof(WT_PAGE_INDEX) + alloc_entries * sizeof(WT_REF *);
        pindex = p;
        pindex->index = (WT_REF **)((WT_PAGE_INDEX *)p + 1);
        pindex->entries = alloc_entries;
        WT_INTL_INDEX_SET(page, pindex);
        if (alloc_refs)
            for (i = 0; i < pindex->entries; ++i) {
                WT_ERR(__wt_calloc_one(session, &pindex->index[i]));
                size += sizeof(WT_REF);
            }
        if (0) {
err:
            if ((pindex = WT_INTL_INDEX_GET_SAFE(page)) != NULL) {
                for (i = 0; i < pindex->entries; ++i)
                    __wt_free(session, pindex->index[i]);
                __wt_free(session, pindex);
            }
            __wt_free(session, page);
            return (ret);
        }
        break;
    case WT_PAGE_COL_VAR:
        page->pg_var = alloc_entries == 0 ? NULL : (WT_COL *)((uint8_t *)page + sizeof(WT_PAGE));
        page->entries = alloc_entries;
        break;
    case WT_PAGE_ROW_LEAF:
        page->pg_row = alloc_entries == 0 ? NULL : (WT_ROW *)((uint8_t *)page + sizeof(WT_PAGE));
        page->entries = alloc_entries;
        break;
    default:
        return (__wt_illegal_value(session, type));
    }

    /* Increment the cache statistics. */
    __wt_cache_page_inmem_incr(session, page, size);
    (void)__wt_atomic_add64(&cache->pages_inmem, 1);
    page->cache_create_gen = cache->evict_pass_gen;

    *pagep = page;
    return (0);
}

/*
 * __page_inmem_prepare_update --
 *     Create the actual update for a prepared value.
 */
static int
__page_inmem_prepare_update(WT_SESSION_IMPL *session, WT_ITEM *value, WT_CELL_UNPACK_KV *unpack,
  WT_UPDATE **updp, size_t *sizep)
{
    WT_DECL_RET;
    WT_UPDATE *upd, *tombstone;
    size_t size, total_size;

    *sizep = 0;

    tombstone = upd = NULL;
    total_size = 0;

    WT_RET(__wt_upd_alloc(session, value, WT_UPDATE_STANDARD, &upd, &size));
    total_size += size;
    upd->durable_ts = unpack->tw.durable_start_ts;
    upd->start_ts = unpack->tw.start_ts;
    upd->txnid = unpack->tw.start_txn;
    F_SET(upd, WT_UPDATE_PREPARE_RESTORED_FROM_DS);

    /*
     * Instantiate both update and tombstone if the prepared update is a tombstone. This is required
     * to ensure that written prepared delete operation must be removed from the data store, when
     * the prepared transaction gets rollback.
     */
    if (WT_TIME_WINDOW_HAS_STOP(&unpack->tw)) {
        WT_ERR(__wt_upd_alloc_tombstone(session, &tombstone, &size));
        total_size += size;
        tombstone->durable_ts = WT_TS_NONE;
        tombstone->start_ts = unpack->tw.stop_ts;
        tombstone->txnid = unpack->tw.stop_txn;
        tombstone->prepare_state = WT_PREPARE_INPROGRESS;
        F_SET(tombstone, WT_UPDATE_PREPARE_RESTORED_FROM_DS);

        /*
         * Mark the update also as in-progress if the update and tombstone are from same transaction
         * by comparing both the transaction and timestamps as the transaction information gets lost
         * after restart.
         */
        if (unpack->tw.start_ts == unpack->tw.stop_ts &&
          unpack->tw.durable_start_ts == unpack->tw.durable_stop_ts &&
          unpack->tw.start_txn == unpack->tw.stop_txn) {
            upd->durable_ts = WT_TS_NONE;
            upd->prepare_state = WT_PREPARE_INPROGRESS;
        }

        tombstone->next = upd;
        *updp = tombstone;
    } else {
        upd->durable_ts = WT_TS_NONE;
        upd->prepare_state = WT_PREPARE_INPROGRESS;
        *updp = upd;
    }

    *sizep = total_size;
    return (0);

err:
    __wt_free(session, upd);
    __wt_free(session, tombstone);

    return (ret);
}

/*
 * __page_inmem_prepare_update_col --
 *     Shared code for calling __page_inmem_prepare_update on columns.
 */
static int
__page_inmem_prepare_update_col(WT_SESSION_IMPL *session, WT_REF *ref, WT_CURSOR_BTREE *cbt,
  uint64_t recno, WT_ITEM *value, WT_CELL_UNPACK_KV *unpack, WT_UPDATE **updp, size_t *sizep)
{
    WT_RET(__page_inmem_prepare_update(session, value, unpack, updp, sizep));

    /* Search the page and apply the modification. */
    WT_RET(__wt_col_search(cbt, recno, ref, true, NULL));
#ifdef HAVE_DIAGNOSTIC
    WT_RET(__wt_col_modify(cbt, recno, NULL, *updp, WT_UPDATE_INVALID, true, true));
#else
    WT_RET(__wt_col_modify(cbt, recno, NULL, *updp, WT_UPDATE_INVALID, true));
#endif
    return (0);
}

/*
 * __wt_page_inmem_prepare --
 *     Instantiate prepared updates.
 */
int
__wt_page_inmem_prepare(WT_SESSION_IMPL *session, WT_REF *ref)
{
    WT_BTREE *btree;
    WT_CELL *cell;
    WT_CELL_UNPACK_KV unpack;
    WT_COL *cip;
    WT_CURSOR_BTREE cbt;
    WT_DECL_ITEM(key);
    WT_DECL_ITEM(value);
    WT_DECL_RET;
    WT_PAGE *page;
    WT_ROW *rip;
    WT_UPDATE *upd;
    size_t size, total_size;
    uint64_t recno, rle;
    uint32_t i, numtws, tw;
    uint8_t v;

    btree = S2BT(session);
    page = ref->page;
    upd = NULL;
    total_size = 0;

    /* We don't handle in-memory prepare resolution here. */
    WT_ASSERT(session, !F_ISSET(S2C(session), WT_CONN_IN_MEMORY));

    __wt_btcur_init(session, &cbt);
    __wt_btcur_open(&cbt);

    WT_ERR(__wt_scr_alloc(session, 0, &value));
    if (page->type == WT_PAGE_COL_VAR) {
        recno = ref->ref_recno;
        WT_COL_FOREACH (page, cip, i) {
            /* Search for prepare records. */
            cell = WT_COL_PTR(page, cip);
            __wt_cell_unpack_kv(session, page->dsk, cell, &unpack);
            rle = __wt_cell_rle(&unpack);
            if (!unpack.tw.prepare) {
                recno += rle;
                continue;
            }

            /* Get the value. */
            WT_ERR(__wt_page_cell_data_ref(session, page, &unpack, value));

            /* For each record, create an update to resolve the prepare. */
            for (; rle > 0; --rle, ++recno) {
                /* Create an update to resolve the prepare. */
                WT_ERR(__page_inmem_prepare_update_col(
                  session, ref, &cbt, recno, value, &unpack, &upd, &size));
                total_size += size;
                upd = NULL;
            }
        }
    } else if (page->type == WT_PAGE_COL_FIX) {
        WT_ASSERT(session, WT_COL_FIX_TWS_SET(page));
        /* Search for prepare records. */
        numtws = page->pg_fix_numtws;
        for (tw = 0; tw < numtws; tw++) {
            cell = WT_COL_FIX_TW_CELL(page, &page->pg_fix_tws[tw]);
            __wt_cell_unpack_kv(session, page->dsk, cell, &unpack);
            if (!unpack.tw.prepare)
                continue;
            recno = ref->ref_recno + page->pg_fix_tws[tw].recno_offset;

            /* Get the value. The update will copy it, so we don't need to allocate here. */
            v = __bit_getv_recno(ref, recno, btree->bitcnt);
            value->data = &v;
            value->size = 1;

            /* Create an update to resolve the prepare. */
            WT_ERR(__page_inmem_prepare_update_col(
              session, ref, &cbt, recno, value, &unpack, &upd, &size));
            total_size += size;
            upd = NULL;
        }
    } else {
        WT_ASSERT(session, page->type == WT_PAGE_ROW_LEAF);
        WT_ERR(__wt_scr_alloc(session, 0, &key));
        WT_ROW_FOREACH (page, rip, i) {
            /* Search for prepare records. */
            __wt_row_leaf_value_cell(session, page, rip, &unpack);
            if (!unpack.tw.prepare)
                continue;

            /* Get the key/value pair and create an update to resolve the prepare. */
            WT_ERR(__wt_row_leaf_key(session, page, rip, key, false));
            WT_ERR(__wt_page_cell_data_ref(session, page, &unpack, value));
            WT_ERR(__page_inmem_prepare_update(session, value, &unpack, &upd, &size));
            total_size += size;

            /* Search the page and apply the modification. */
            WT_ERR(__wt_row_search(&cbt, key, true, ref, true, NULL));
#ifdef HAVE_DIAGNOSTIC
            WT_ERR(__wt_row_modify(&cbt, key, NULL, upd, WT_UPDATE_INVALID, true, true));
#else
            WT_ERR(__wt_row_modify(&cbt, key, NULL, upd, WT_UPDATE_INVALID, true));
#endif
            upd = NULL;
        }
    }

    __wt_cache_page_inmem_incr(session, page, total_size);

    if (0) {
err:
        __wt_free_update_list(session, &upd);
    }
    WT_TRET(__wt_btcur_close(&cbt, true));
    __wt_scr_free(session, &key);
    __wt_scr_free(session, &value);
    return (ret);
}

/*
 * __wt_page_inmem --
 *     Build in-memory page information.
 */
int
__wt_page_inmem(WT_SESSION_IMPL *session, WT_REF *ref, const void *image, uint32_t flags,
  WT_PAGE **pagep, bool *preparedp)
{
    WT_DECL_RET;
    WT_PAGE *page;
    const WT_PAGE_HEADER *dsk;
    size_t size;
    uint32_t alloc_entries;

    *pagep = NULL;

    if (preparedp != NULL)
        *preparedp = false;

    dsk = image;
    alloc_entries = 0;

    /*
     * Figure out how many underlying objects the page references so we can allocate them along with
     * the page.
     */
    switch (dsk->type) {
    case WT_PAGE_COL_FIX:
    case WT_PAGE_COL_INT:
    case WT_PAGE_COL_VAR:
        /*
         * Column-store leaf page entries map one-to-one to the number of physical entries on the
         * page (each physical entry is a value item). Note this value isn't necessarily correct, we
         * may skip values when reading the disk image.
         *
         * Column-store internal page entries map one-to-one to the number of physical entries on
         * the page (each entry is a location cookie).
         */
        alloc_entries = dsk->u.entries;
        break;
    case WT_PAGE_ROW_INT:
        /*
         * Row-store internal page entries map one-to-two to the number of physical entries on the
         * page (each entry is a key and location cookie pair).
         */
        alloc_entries = dsk->u.entries / 2;
        break;
    case WT_PAGE_ROW_LEAF:
        /*
         * If the "no empty values" flag is set, row-store leaf page entries map one-to-one to the
         * number of physical entries on the page (each physical entry is a key or value item). If
         * that flag is not set, there are more keys than values, we have to walk the page to figure
         * it out. Note this value isn't necessarily correct, we may skip values when reading the
         * disk image.
         */
        if (F_ISSET(dsk, WT_PAGE_EMPTY_V_ALL))
            alloc_entries = dsk->u.entries;
        else if (F_ISSET(dsk, WT_PAGE_EMPTY_V_NONE))
            alloc_entries = dsk->u.entries / 2;
        else
            WT_RET(__inmem_row_leaf_entries(session, dsk, &alloc_entries));
        break;
    default:
        return (__wt_illegal_value(session, dsk->type));
    }

    /* Allocate and initialize a new WT_PAGE. */
    WT_RET(__wt_page_alloc(session, dsk->type, alloc_entries, true, &page));
    page->dsk = dsk;
    F_SET_ATOMIC_16(page, flags);

    /*
     * Track the memory allocated to build this page so we can update the cache statistics in a
     * single call. If the disk image is in allocated memory, start with that.
     *
     * Accounting is based on the page-header's in-memory disk size instead of the buffer memory
     * used to instantiate the page image even though the values might not match exactly, because
     * that's the only value we have when discarding the page image and accounting needs to match.
     */
    size = LF_ISSET(WT_PAGE_DISK_ALLOC) ? dsk->mem_size : 0;

    switch (page->type) {
    case WT_PAGE_COL_FIX:
        WT_ERR(__inmem_col_fix(session, page, preparedp, &size));
        break;
    case WT_PAGE_COL_INT:
        __inmem_col_int(session, page);
        break;
    case WT_PAGE_COL_VAR:
        WT_ERR(__inmem_col_var(session, page, dsk->recno, preparedp, &size));
        break;
    case WT_PAGE_ROW_INT:
        WT_ERR(__inmem_row_int(session, page, &size));
        break;
    case WT_PAGE_ROW_LEAF:
        WT_ERR(__inmem_row_leaf(session, page, preparedp));
        break;
    default:
        WT_ERR(__wt_illegal_value(session, page->type));
    }

    /* Update the page's cache statistics. */
    __wt_cache_page_inmem_incr(session, page, size);

    if (LF_ISSET(WT_PAGE_DISK_ALLOC))
        __wt_cache_page_image_incr(session, page);

    /* Link the new internal page to the parent. */
    if (ref != NULL) {
        switch (page->type) {
        case WT_PAGE_COL_INT:
        case WT_PAGE_ROW_INT:
            page->pg_intl_parent_ref = ref;
            break;
        }
        ref->page = page;
    }

    *pagep = page;
    return (0);

err:
    __wt_page_out(session, &page);
    return (ret);
}

/*
 * __wt_col_fix_read_auxheader --
 *     Read the auxiliary header following the bitmap data, if any. This code is used by verify and
 *     needs to be accordingly careful. It is also used by mainline reads so it must also not crash
 *     or print on behalf of verify, and it should not waste time on checks that inmem doesn't need.
 *     Currently this means it does do bounds checks on the header itself (they are embedded in the
 *     integer unpacking) but not on the returned offset, and we don't check the version number.
 *     Careful callers (verify, perhaps debug) should check this. Fast callers (inmem) probably
 *     needn't bother. Salvage is protected by verify and doesn't need to check any of it.
 */
int
__wt_col_fix_read_auxheader(
  WT_SESSION_IMPL *session, const WT_PAGE_HEADER *dsk, WT_COL_FIX_AUXILIARY_HEADER *auxhdr)
{
    WT_BTREE *btree;
    uint64_t dataoffset, entries;
    uint32_t auxheaderoffset, bitmapsize;
    const uint8_t *end, *raw;

    btree = S2BT(session);

    /*
     * Figure where the auxiliary header is. It is always immediately after the bitmap data,
     * regardless of whether the page is full.
     */
    bitmapsize = __bitstr_size(dsk->u.entries * btree->bitcnt);
    auxheaderoffset = WT_PAGE_HEADER_BYTE_SIZE(btree) + bitmapsize;

    /*
     * If the auxiliary header is past the in-memory page size, there's no auxiliary data. If
     * there's at least one byte past the bitmap data, check whether it's zero. If that's zero,
     * there's no auxiliary data. (We are guaranteed that any allocation slop that we might be
     * looking at is all zeros.) Set everything to zero and return.
     */
    if (auxheaderoffset >= dsk->mem_size || *(raw = (uint8_t *)dsk + auxheaderoffset) == 0) {
        auxhdr->version = WT_COL_FIX_VERSION_NIL;
        auxhdr->entries = 0;
        auxhdr->emptyoffset = 0;
        auxhdr->dataoffset = 0;
        return (0);
    }

    /* Remember the end of the page for easy computation of maximum lengths. */
    end = (uint8_t *)dsk + dsk->mem_size;

    /*
     * The on-disk header is a 1-byte version, a packed integer with the number of entries, and a
     * second packed integer that gives the offset from the header start to the data.
     */

    auxhdr->version = *(raw++);
    WT_RET(__wt_vunpack_uint(&raw, WT_PTRDIFF32(end, raw), &entries));
    WT_RET(__wt_vunpack_uint(&raw, WT_PTRDIFF32(end, raw), &dataoffset));

    /* The returned offsets are from the start of the page. */
    auxhdr->entries = (uint32_t)entries;
    auxhdr->emptyoffset = WT_PTRDIFF32(raw, (uint8_t *)dsk);
    auxhdr->dataoffset = auxheaderoffset + (uint32_t)dataoffset;

    return (0);
}

/*
 * __inmem_col_fix --
 *     Build in-memory index for fixed-length column-store leaf pages.
 */
static int
__inmem_col_fix(WT_SESSION_IMPL *session, WT_PAGE *page, bool *preparedp, size_t *sizep)
{
    WT_BTREE *btree;
    WT_CELL_UNPACK_KV unpack;
    WT_COL_FIX_AUXILIARY_HEADER auxhdr;
    const WT_PAGE_HEADER *dsk;
    size_t size;
    uint64_t tmp;
    uint32_t entry_num, recno_offset, skipped;
    const uint8_t *p8;
    bool prepare;
    void *pv;

    btree = S2BT(session);
    dsk = page->dsk;
    prepare = false;

    page->pg_fix_bitf = WT_PAGE_HEADER_BYTE(btree, dsk);

    WT_RET(__wt_col_fix_read_auxheader(session, dsk, &auxhdr));
    WT_ASSERT(session, auxhdr.dataoffset <= dsk->mem_size);

    switch (auxhdr.version) {
    case WT_COL_FIX_VERSION_NIL:
        /* There is no time window data. */
        page->u.col_fix.fix_tw = NULL;
        break;
    case WT_COL_FIX_VERSION_TS:
        /* The page should be VERSION_NIL if there are no timestamp entries. */
        WT_ASSERT(session, auxhdr.entries > 0);

        recno_offset = 0;
        skipped = 0;

        /* Walk the entries to build the index. */
        entry_num = 0;
        WT_CELL_FOREACH_FIX_TIMESTAMPS (session, dsk, &auxhdr, unpack) {
            if (unpack.type == WT_CELL_KEY) {
                p8 = unpack.data;
                /* The array is attached to the page, so we don't need to free it on error here. */
                WT_RET(__wt_vunpack_uint(&p8, unpack.size, &tmp));
                /* For now at least, check that the entries are in ascending order. */
                WT_ASSERT(session, tmp < UINT32_MAX);
                WT_ASSERT(session, (recno_offset == 0 && tmp == 0) || tmp > recno_offset);
                recno_offset = (uint32_t)tmp;
            } else if (!WT_TIME_WINDOW_IS_EMPTY(&unpack.tw)) {
                /* Only index entries that are not already obsolete. */

                if (entry_num == 0) {
                    size = sizeof(WT_COL_FIX_TW) +
                      (auxhdr.entries - skipped) * sizeof(WT_COL_FIX_TW_ENTRY);
                    WT_RET(__wt_calloc(session, 1, size, &pv));
                    *sizep += size;
                    page->u.col_fix.fix_tw = pv;
                }
                page->pg_fix_tws[entry_num].recno_offset = recno_offset;
                page->pg_fix_tws[entry_num].cell_offset = WT_PAGE_DISK_OFFSET(page, unpack.cell);
                if (unpack.tw.prepare)
                    prepare = true;
                entry_num++;
            } else
                skipped++;
        }
        WT_CELL_FOREACH_END;

        /*
         * Set the number of time windows. If there weren't any, the variable doesn't exist. Also,
         * while we could now reallocate the array to the exact count, assume it's not worthwhile.
         */
        if (entry_num > 0)
            page->pg_fix_numtws = entry_num;

        /*
         * If we skipped "quite a few" entries (threshold is arbitrary), mark the page dirty so it
         * gets rewritten without them.
         */
        if (!F_ISSET(btree, WT_BTREE_READONLY) && skipped >= auxhdr.entries / 4 &&
          skipped >= dsk->u.entries / 100 && skipped > 4) {
            WT_RET(__wt_page_modify_init(session, page));
            __wt_page_modify_set(session, page);
        }

        break;
    }

    /* Report back whether we found a prepared value. */
    if (preparedp != NULL && prepare)
        *preparedp = true;

    return (0);
}

/*
 * __inmem_col_int --
 *     Build in-memory index for column-store internal pages.
 */
static void
__inmem_col_int(WT_SESSION_IMPL *session, WT_PAGE *page)
{
    WT_CELL_UNPACK_ADDR unpack;
    WT_PAGE_INDEX *pindex;
    WT_REF **refp, *ref;
    uint32_t hint;

    /*
     * Walk the page, building references: the page contains value items. The value items are
     * on-page items (WT_CELL_VALUE).
     */
    pindex = WT_INTL_INDEX_GET_SAFE(page);
    refp = pindex->index;
    hint = 0;
    WT_CELL_FOREACH_ADDR (session, page->dsk, unpack) {
        ref = *refp++;
        ref->home = page;
        ref->pindex_hint = hint++;
        ref->addr = unpack.cell;
        ref->ref_recno = unpack.v;

        F_SET(ref, unpack.type == WT_CELL_ADDR_INT ? WT_REF_FLAG_INTERNAL : WT_REF_FLAG_LEAF);
    }
    WT_CELL_FOREACH_END;
}

/*
 * __inmem_col_var_repeats --
 *     Count the number of repeat entries on the page.
 */
static void
__inmem_col_var_repeats(WT_SESSION_IMPL *session, WT_PAGE *page, uint32_t *np)
{
    WT_CELL_UNPACK_KV unpack;

    *np = 0;

    /* Walk the page, counting entries for the repeats array. */
    WT_CELL_FOREACH_KV (session, page->dsk, unpack) {
        if (__wt_cell_rle(&unpack) > 1)
            ++*np;
    }
    WT_CELL_FOREACH_END;
}

/*
 * __inmem_col_var --
 *     Build in-memory index for variable-length, data-only leaf pages in column-store trees.
 */
static int
__inmem_col_var(
  WT_SESSION_IMPL *session, WT_PAGE *page, uint64_t recno, bool *preparedp, size_t *sizep)
{
    WT_CELL_UNPACK_KV unpack;
    WT_COL *cip;
    WT_COL_RLE *repeats;
    size_t size;
    uint64_t rle;
    uint32_t indx, n, repeat_off;
    bool prepare;
    void *p;

    repeats = NULL;
    repeat_off = 0;
    prepare = false;

    /*
     * Walk the page, building references: the page contains unsorted value items. The value items
     * are on-page (WT_CELL_VALUE), overflow items (WT_CELL_VALUE_OVFL) or deleted items
     * (WT_CELL_DEL).
     */
    indx = 0;
    cip = page->pg_var;
    WT_CELL_FOREACH_KV (session, page->dsk, unpack) {
        WT_COL_PTR_SET(cip, WT_PAGE_DISK_OFFSET(page, unpack.cell));
        cip++;

        /*
         * Add records with repeat counts greater than 1 to an array we use for fast lookups. The
         * first entry we find needing the repeats array triggers a re-walk from the start of the
         * page to determine the size of the array.
         */
        rle = __wt_cell_rle(&unpack);
        if (rle > 1) {
            if (repeats == NULL) {
                __inmem_col_var_repeats(session, page, &n);
                size = sizeof(WT_COL_VAR_REPEAT) + (n + 1) * sizeof(WT_COL_RLE);
                WT_RET(__wt_calloc(session, 1, size, &p));
                *sizep += size;

                page->u.col_var.repeats = p;
                page->pg_var_nrepeats = n;
                repeats = page->pg_var_repeats;
            }
            repeats[repeat_off].indx = indx;
            repeats[repeat_off].recno = recno;
            repeats[repeat_off++].rle = rle;
        }

        /* If we find a prepare, we'll have to instantiate it in the update chain later. */
        if (unpack.tw.prepare)
            prepare = true;

        indx++;
        recno += rle;
    }
    WT_CELL_FOREACH_END;

    if (preparedp != NULL && prepare)
        *preparedp = true;

    return (0);
}

/*
 * __inmem_row_int --
 *     Build in-memory index for row-store internal pages.
 */
static int
__inmem_row_int(WT_SESSION_IMPL *session, WT_PAGE *page, size_t *sizep)
{
    WT_BTREE *btree;
    WT_CELL_UNPACK_ADDR unpack;
    WT_DECL_ITEM(current);
    WT_DECL_RET;
    WT_PAGE_INDEX *pindex;
    WT_REF *ref, **refp;
    uint32_t hint;
    bool overflow_keys;

    btree = S2BT(session);

    WT_RET(__wt_scr_alloc(session, 0, &current));

    /*
     * Walk the page, instantiating keys: the page contains sorted key and location cookie pairs.
     * Keys are on-page/overflow items and location cookies are WT_CELL_ADDR_XXX items.
     */
    pindex = WT_INTL_INDEX_GET_SAFE(page);
    refp = pindex->index;
    overflow_keys = false;
    hint = 0;
    WT_CELL_FOREACH_ADDR (session, page->dsk, unpack) {
        ref = *refp;
        ref->home = page;
        ref->pindex_hint = hint++;

        switch (unpack.type) {
        case WT_CELL_ADDR_INT:
            F_SET(ref, WT_REF_FLAG_INTERNAL);
            break;
        case WT_CELL_ADDR_DEL:
        case WT_CELL_ADDR_LEAF:
        case WT_CELL_ADDR_LEAF_NO:
            F_SET(ref, WT_REF_FLAG_LEAF);
            break;
        }

        switch (unpack.type) {
        case WT_CELL_KEY:
            /*
             * Note: we don't Huffman encode internal page keys, there's no decoding work to do.
             */
            __wt_ref_key_onpage_set(page, ref, &unpack);
            break;
        case WT_CELL_KEY_OVFL:
            /*
             * Instantiate any overflow keys; WiredTiger depends on this, assuming any overflow key
             * is instantiated, and any keys that aren't instantiated cannot be overflow items.
             */
            WT_ERR(__wt_dsk_cell_data_ref(session, page->type, &unpack, current));

            WT_ERR(__wt_row_ikey_incr(session, page, WT_PAGE_DISK_OFFSET(page, unpack.cell),
              current->data, current->size, ref));

            *sizep += sizeof(WT_IKEY) + current->size;
            overflow_keys = true;
            break;
        case WT_CELL_ADDR_DEL:
            /*
             * A cell may reference a deleted leaf page: if a leaf page was deleted without being
             * read (fast truncate), and the deletion committed, but older transactions in the
             * system required the previous version of the page to remain available, a special
             * deleted-address type cell is written. We'll see that cell on a page if we read from a
             * checkpoint including a deleted cell or if we crash/recover and start off from such a
             * checkpoint (absent running recovery, a version of the page without the deleted cell
             * would eventually have been written). If we crash and recover to a page with a
             * deleted-address cell, we want to discard the page from the backing store (it was
             * never discarded), and, of course, by definition no earlier transaction will ever need
             * it.
             *
             * Re-create the state of a deleted page.
             */
            ref->addr = unpack.cell;
            WT_REF_SET_STATE(ref, WT_REF_DELETED);
            ++refp;

            /*
             * If the tree is already dirty and so will be written, mark the page dirty. (We want to
             * free the deleted pages, but if the handle is read-only or if the application never
             * modifies the tree, we're not able to do so.)
             */
            if (btree->modified) {
                WT_ERR(__wt_page_modify_init(session, page));
                __wt_page_modify_set(session, page);
            }
            break;
        case WT_CELL_ADDR_INT:
        case WT_CELL_ADDR_LEAF:
        case WT_CELL_ADDR_LEAF_NO:
            ref->addr = unpack.cell;
            ++refp;
            break;
        default:
            WT_ERR(__wt_illegal_value(session, unpack.type));
        }
    }
    WT_CELL_FOREACH_END;

    /*
     * We track if an internal page has backing overflow keys, as overflow keys limit the eviction
     * we can do during a checkpoint. (This is only for historical tables, reconciliation no longer
     * writes overflow cookies on internal pages, no matter the size of the key.)
     */
    if (overflow_keys)
        F_SET_ATOMIC_16(page, WT_PAGE_INTL_OVERFLOW_KEYS);

err:
    __wt_scr_free(session, &current);
    return (ret);
}

/*
 * __inmem_row_leaf_entries --
 *     Return the number of entries for row-store leaf pages.
 */
static int
__inmem_row_leaf_entries(WT_SESSION_IMPL *session, const WT_PAGE_HEADER *dsk, uint32_t *nindxp)
{
    WT_CELL_UNPACK_KV unpack;
    uint32_t nindx;

    /*
     * Leaf row-store page entries map to a maximum of one-to-one to the number of physical entries
     * on the page (each physical entry might be a key without a subsequent data item). To avoid
     * over-allocation in workloads without empty data items, first walk the page counting the
     * number of keys, then allocate the indices.
     *
     * The page contains key/data pairs. Keys are on-page (WT_CELL_KEY) or overflow
     * (WT_CELL_KEY_OVFL) items, data are either non-existent or a single on-page (WT_CELL_VALUE) or
     * overflow (WT_CELL_VALUE_OVFL) item.
     */
    nindx = 0;
    WT_CELL_FOREACH_KV (session, dsk, unpack) {
        switch (unpack.type) {
        case WT_CELL_KEY:
        case WT_CELL_KEY_OVFL:
            ++nindx;
            break;
        case WT_CELL_VALUE:
        case WT_CELL_VALUE_OVFL:
            break;
        default:
            return (__wt_illegal_value(session, unpack.type));
        }
    }
    WT_CELL_FOREACH_END;

    *nindxp = nindx;
    return (0);
}

/*
 * __inmem_row_leaf --
 *     Build in-memory index for row-store leaf pages.
 */
static int
__inmem_row_leaf(WT_SESSION_IMPL *session, WT_PAGE *page, bool *preparedp)
{
    WT_BTREE *btree;
    WT_CELL_UNPACK_KV unpack;
    WT_DECL_RET;
    WT_ROW *rip;
    uint32_t best_prefix_count, best_prefix_start, best_prefix_stop;
    uint32_t last_slot, prefix_count, prefix_start, prefix_stop, slot;
    uint8_t smallest_prefix;
    bool prepare;

    btree = S2BT(session);
    last_slot = 0;
    prepare = false;

    /* The code depends on the prefix count variables, other initialization shouldn't matter. */
    best_prefix_count = prefix_count = 0;
    smallest_prefix = 0;                      /* [-Wconditional-uninitialized] */
    prefix_start = prefix_stop = 0;           /* [-Wconditional-uninitialized] */
    best_prefix_start = best_prefix_stop = 0; /* [-Wconditional-uninitialized] */

    /* Walk the page, building indices. */
    rip = page->pg_row;
    WT_CELL_FOREACH_KV (session, page->dsk, unpack) {
        switch (unpack.type) {
        case WT_CELL_KEY:
            /*
             * Simple keys and prefix-compressed keys can be directly referenced on the page to
             * avoid repeatedly unpacking their cells.
             *
             * Review groups of prefix-compressed keys, and track the biggest group as the page's
             * prefix. What we're finding is the biggest group of prefix-compressed keys we can
             * immediately build using a previous key plus their suffix bytes, without rolling
             * forward through intermediate keys. We save that information on the page and then
             * never physically instantiate those keys, avoiding memory amplification for pages with
             * a page-wide prefix. On the first of a group of prefix-compressed keys, track the slot
             * of the fully-instantiated key from which it's derived and the current key's prefix
             * length. On subsequent keys, if the key can be built from the original key plus the
             * current key's suffix bytes, update the maximum slot to which the prefix applies and
             * the smallest prefix length.
             *
             * Groups of prefix-compressed keys end when a key is not prefix-compressed (ignoring
             * overflow keys), or the key's prefix length increases. A prefix length decreasing is
             * OK, it only means fewer bytes taken from the original key. A prefix length increasing
             * doesn't necessarily end a group of prefix-compressed keys as we might be able to
             * build a subsequent key using the original key and the key's suffix bytes, that is the
             * prefix length could increase and then decrease to the same prefix length as before
             * and those latter keys could be built without rolling forward through intermediate
             * keys.
             *
             * However, that gets tricky: once a key prefix grows, we can never include a prefix
             * smaller than the smallest prefix found so far, in the group, as a subsequent key
             * prefix larger than the smallest prefix found so far might include bytes not present
             * in the original instantiated key. Growing and shrinking is complicated to track, so
             * rather than code up that complexity, we close out a group whenever the prefix grows.
             * Plus, growing has additional issues. Any key with a larger prefix cannot be
             * instantiated without rolling forward through intermediate keys, and so while such a
             * key isn't required to close out the prefix group in all cases, it's not a useful
             * entry for finding the best group of prefix-compressed keys, either, it's only
             * possible keys after the prefix shrinks again that are potentially worth including in
             * a group.
             */
            slot = WT_ROW_SLOT(page, rip);
            if (unpack.prefix == 0) {
                /* If the last prefix group was the best, track it. */
                if (prefix_count > best_prefix_count) {
                    best_prefix_start = prefix_start;
                    best_prefix_stop = prefix_stop;
                    best_prefix_count = prefix_count;
                }
                prefix_count = 0;
                prefix_start = slot;
            } else {
                /* Check for starting or continuing a prefix group. */
                if (prefix_count == 0 ||
                  (last_slot == slot - 1 && unpack.prefix <= smallest_prefix)) {
                    smallest_prefix = unpack.prefix;
                    last_slot = prefix_stop = slot;
                    ++prefix_count;
                }
            }
            __wt_row_leaf_key_set(page, rip, &unpack);
            ++rip;
            continue;
        case WT_CELL_KEY_OVFL:
            /*
             * Prefix compression skips overflow items, ignore this slot. The last slot value is
             * only used inside a group of prefix-compressed keys, so blindly increment it, it's not
             * used unless the count of prefix-compressed keys is non-zero.
             */
            ++last_slot;

            __wt_row_leaf_key_set(page, rip, &unpack);
            ++rip;
            continue;
        case WT_CELL_VALUE:
            /*
             * Simple values without compression can be directly referenced on the page to avoid
             * repeatedly unpacking their cells.
             *
             * The visibility information is not referenced on the page so we need to ensure that
             * the value is globally visible at the point in time where we read the page into cache.
             */
            if (!btree->huffman_value &&
              (WT_TIME_WINDOW_IS_EMPTY(&unpack.tw) ||
                (!WT_TIME_WINDOW_HAS_STOP(&unpack.tw) &&
                  __wt_txn_tw_start_visible_all(session, &unpack.tw))))
                __wt_row_leaf_value_set(rip - 1, &unpack);
            break;
        case WT_CELL_VALUE_OVFL:
            break;
        default:
            WT_ERR(__wt_illegal_value(session, unpack.type));
        }

        /* If we find a prepare, we'll have to instantiate it in the update chain later. */
        if (unpack.tw.prepare)
            prepare = true;
    }
    WT_CELL_FOREACH_END;

    /* If the last prefix group was the best, track it. Save the best prefix group for the page. */
    if (prefix_count > best_prefix_count) {
        best_prefix_start = prefix_start;
        best_prefix_stop = prefix_stop;
    }
    page->prefix_start = best_prefix_start;
    page->prefix_stop = best_prefix_stop;

    /*
     * Backward cursor traversal can be too slow if we're forced to process long stretches of
     * prefix-compressed keys to create every key as we walk backwards through the page, and we
     * handle that by instantiating periodic keys when backward cursor traversal enters a new page.
     * Mark the page as not needing that work if there aren't stretches of prefix-compressed keys.
     */
    if (best_prefix_count <= 10)
        F_SET_ATOMIC_16(page, WT_PAGE_BUILD_KEYS);

    if (preparedp != NULL && prepare)
        *preparedp = true;

err:
    return (ret);
}
