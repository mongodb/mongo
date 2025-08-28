/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __inmem_col_fix(WT_SESSION_IMPL *, WT_PAGE *, bool *, size_t *);
static int __inmem_col_int(WT_SESSION_IMPL *, WT_PAGE *, uint64_t);
static int __inmem_col_var(WT_SESSION_IMPL *, WT_PAGE *, uint64_t, bool *, size_t *);
static int __inmem_row_int(WT_SESSION_IMPL *, WT_PAGE *, size_t *);
static int __inmem_row_leaf(WT_SESSION_IMPL *, WT_PAGE *, bool *);
static int __inmem_row_leaf_entries(WT_SESSION_IMPL *, const WT_PAGE_HEADER *, uint32_t *);

/*
 * Define functions that increment histogram statistics for reconstruction of pages with deltas.
 */
WT_STAT_USECS_HIST_INCR_FUNC(internal_reconstruct, perf_hist_internal_reconstruct_latency)
WT_STAT_USECS_HIST_INCR_FUNC(leaf_reconstruct, perf_hist_leaf_reconstruct_latency)

/*
 * __page_build_ref --
 *     Create a ref from a base image or a delta.
 */
static int
__page_build_ref(WT_SESSION_IMPL *session, WT_REF *parent_ref, WT_CELL_UNPACK_ADDR *base_key,
  WT_CELL_UNPACK_ADDR *base_val, WT_CELL_UNPACK_DELTA_INT *delta, bool base, WT_REF **refp,
  size_t *incrp)
{
    WT_ADDR *addr;
    WT_DECL_RET;
    WT_REF *ref;
    uint8_t key_type, value_type;

    WT_ASSERT(session, incrp != NULL);
    addr = NULL;

    WT_RET(__wt_calloc_one(session, refp));
    *incrp += sizeof(WT_REF);

    ref = *refp;
    ref->home = parent_ref->page;
    key_type = base ? base_key->type : delta->key.type;
    value_type = base ? base_val->type : delta->value.type;

    switch (key_type) {
    case WT_CELL_KEY:
        if (base)
            __wt_ref_key_onpage_set(parent_ref->page, ref, base_key);
        else {
            WT_RET(__wti_row_ikey(session, 0, delta->key.data, delta->key.size, ref));
            *incrp += sizeof(WT_IKEY) + delta->key.size;
        }
        break;
    case WT_CELL_KEY_OVFL:
    /* Overflow keys are not supported. */
    default:
        WT_RET(__wt_illegal_value(session, key_type));
    }

    switch (value_type) {
    case WT_CELL_ADDR_INT:
        F_SET(ref, WT_REF_FLAG_INTERNAL);
        break;
    case WT_CELL_ADDR_LEAF:
    case WT_CELL_ADDR_LEAF_NO:
        F_SET(ref, WT_REF_FLAG_LEAF);
        break;
    case WT_CELL_ADDR_DEL:
    /* Fast truncated pages are not supported. */
    default:
        WT_RET(__wt_illegal_value(session, value_type));
    }

    switch (value_type) {
    case WT_CELL_ADDR_INT:
    case WT_CELL_ADDR_LEAF:
    case WT_CELL_ADDR_LEAF_NO:
        if (base)
            ref->addr = base_val->cell;
        else {
            WT_RET(__wt_calloc_one(session, &addr));
            ref->addr = addr;
            WT_TIME_AGGREGATE_COPY(&addr->ta, &delta->value.ta);
            WT_ERR(__wt_memdup(session, delta->value.data, delta->value.size, &addr->block_cookie));
            addr->block_cookie_size = (uint8_t)delta->value.size;
            switch (delta->value.raw) {
            case WT_CELL_ADDR_INT:
                addr->type = WT_ADDR_INT;
                break;
            case WT_CELL_ADDR_LEAF:
                addr->type = WT_ADDR_LEAF;
                break;
            case WT_CELL_ADDR_LEAF_NO:
                addr->type = WT_ADDR_LEAF_NO;
                break;
            default:
                WT_ERR(__wt_illegal_value(session, delta->value.raw));
            }
        }
        break;
    case WT_CELL_ADDR_DEL:
    /* Fast truncated pages are not supported. */
    default:
        WT_ERR(__wt_illegal_value(session, value_type));
    }

    if (0) {
err:
        if (addr != NULL) {
            __wt_free(session, addr->block_cookie);
            __wt_free(session, addr);
        }
    }

    return (ret);
}

/*
 * __page_merge_internal_delta_with_base_image --
 *     Merge the consolidated delta array with the base image.
 */
static int
__page_merge_internal_delta_with_base_image(WT_SESSION_IMPL *session, WT_REF *ref,
  WT_CELL_UNPACK_DELTA_INT **delta, size_t delta_entries, WT_REF ***refsp, size_t *ref_entriesp,
  size_t *incr)
{
    WT_CELL_UNPACK_ADDR *base, *base_key, *base_val;
    WT_DECL_RET;
    WT_ITEM base_key_buf, delta_key_buf;
    WT_PAGE *page;
    WT_REF **refs;
    size_t base_entries, estimated_entries, final_entries, i, j, k;
    int cmp;

    final_entries = i = j = k = 0;
    page = ref->page;
    base_entries = (size_t)page->dsk->u.entries;

    WT_ASSERT(session, base_entries != 0 && delta_entries != 0);

    /* Unpack all entries from the base image into an array. */
    WT_ERR(__wt_calloc_def(session, base_entries, &base));
    WT_CELL_FOREACH_ADDR (session, page->dsk, base[k]) {
        k++;
    }
    WT_CELL_FOREACH_END;

    /*
     * Creates a new refs array containing the finalized refs. The maximum number of entries is the
     * sum of half the base entries (since entries in the base image is the total of both keys and
     * values) and the delta entries.
     */
    estimated_entries = (base_entries / 2) + delta_entries + 1;
    WT_ERR(__wt_calloc_def(session, estimated_entries, refsp));
    refs = *refsp;

    /* Perform a merge sort between the base array and the delta array. */
    while (i < base_entries && j < delta_entries) {
        /* Compare the keys of the base entry and the delta entry. */
        base_key_buf.data = base[i].data;
        base_key_buf.size = base[i].size;
        delta_key_buf.data = delta[j]->key.data;
        delta_key_buf.size = delta[j]->key.size;
        WT_ERR(__wt_compare(session, S2BT(session)->collator, &base_key_buf, &delta_key_buf, &cmp));

        if (cmp < 0) {
            base_key = &base[i++];
            base_val = &base[i++];
            WT_ERR(__page_build_ref(
              session, ref, base_key, base_val, NULL, true, &refs[final_entries++], incr));
        } else if (cmp >= 0) {
            if (!F_ISSET(delta[j], WT_DELTA_INT_IS_DELETE))
                WT_ERR(__page_build_ref(
                  session, ref, NULL, NULL, delta[j], false, &refs[final_entries++], incr));
            if (cmp == 0)
                i += 2; /* Skip the current key and value. */
            j++;
        }
    }
    /* Copy the remaining entries from the base array or the delta array. */
    for (; i < base_entries;) {
        base_key = &base[i++];
        base_val = &base[i++];
        WT_ERR(__page_build_ref(
          session, ref, base_key, base_val, NULL, true, &refs[final_entries++], incr));
    }
    for (; j < delta_entries; j++)
        if (!F_ISSET(delta[j], WT_DELTA_INT_IS_DELETE))
            WT_ERR(__page_build_ref(
              session, ref, NULL, NULL, delta[j], false, &refs[final_entries++], incr));

    WT_ASSERT(session, i == base_entries && j == delta_entries);
    WT_ASSERT(session, final_entries != 0);
    WT_ASSERT(session, final_entries < estimated_entries && refs[final_entries] == NULL);

err:
    *ref_entriesp = final_entries;
    __wt_free(session, base);
    return (ret);
}

/*
 * __page_unpacked_delta_key_cmp --
 *     Compare two unpacked deltas
 */
static int
__page_unpacked_delta_key_cmp(
  WT_SESSION_IMPL *session, const WT_CELL_UNPACK_DELTA_INT *a, const WT_CELL_UNPACK_DELTA_INT *b)
{
    WT_DECL_RET;
    WT_ITEM key_a, key_b;
    int cmp;

    key_a.data = a->key.data;
    key_a.size = a->key.size;
    key_b.data = b->key.data;
    key_b.size = b->key.size;

    if ((ret = __wt_compare(session, S2BT(session)->collator, &key_a, &key_b, &cmp)) != 0)
        WT_IGNORE_RET(__wt_panic(session, ret, "failed to compare keys"));

    return (cmp);
}

/*
 * __page_merge_internal_deltas --
 *     Perform a k-way merge sort on the delta arrays to create a single consolidated delta array.
 */
static int
__page_merge_internal_deltas(WT_SESSION_IMPL *session, WT_CELL_UNPACK_DELTA_INT **unpacked_deltas,
  uint32_t start, uint32_t end, size_t *sizes, WT_CELL_UNPACK_DELTA_INT ***deltas_merged,
  size_t *size)
{
    WT_CELL_UNPACK_DELTA_INT **left, **merged, **right;
    size_t left_size, right_size;
    uint32_t i, mid;

    WT_ASSERT(session, start <= end);
    left = right = merged = NULL;

    if (start == end) {
        *size = sizes[start];
        WT_RET(__wt_calloc_def(session, *size, &merged));
        for (i = 0; i < (uint32_t)sizes[start]; ++i)
            merged[i] = &unpacked_deltas[start][i];
        *deltas_merged = merged;
        return (0);
    }

    mid = (start + end) / 2;

    WT_RET(
      __page_merge_internal_deltas(session, unpacked_deltas, start, mid, sizes, &left, &left_size));
    WT_RET(__page_merge_internal_deltas(
      session, unpacked_deltas, mid + 1, end, sizes, &right, &right_size));

    WT_RET(__wt_calloc_def(session, left_size + right_size, &merged));

    WT_MERGE_SORT(session, left, left_size, right, right_size, __page_unpacked_delta_key_cmp, true,
      merged, *size);
    *deltas_merged = merged;

    __wt_free(session, left);
    __wt_free(session, right);
    return (0);
}

/*
 * __page_reconstruct_internal_deltas --
 *     Reconstructs the internal page using `delta_size` delta images.
 */
static int
__page_reconstruct_internal_deltas(
  WT_SESSION_IMPL *session, WT_REF *ref, WT_ITEM *deltas, size_t delta_size)
{
    WT_CELL_UNPACK_DELTA_INT **unpacked_deltas, **unpacked_deltas_merged;
    WT_DECL_RET;
    WT_PAGE_HEADER *header;
    WT_PAGE_INDEX *pindex;
    WT_REF **refs;
    size_t *delta_size_each, incr, pindex_size, refs_entries, unpacked_deltas_merged_size;
    uint32_t i, j;

    unpacked_deltas = unpacked_deltas_merged = NULL;
    refs = NULL;
    pindex = NULL;
    unpacked_deltas_merged_size = refs_entries = incr = 0;

    /*
     * !!!
     * Unpack all delta images into a 2D array where entry is WT_CELL_UNPACK_DELTA_INT:
     *     unpacked_deltas[0] -> [ entry_0_0, entry_0_1, entry_0_2, ... ]
     *     unpacked_deltas[1] -> [ entry_1_0, entry_1_1, entry_1_2, ... ]
     *     unpacked_deltas[2] -> [ entry_2_0, entry_2_1, entry_2_2, ... ]
     *                           ...
     *     unpacked_deltas[N] -> [ entry_N_0, entry_N_1, entry_N_2, ... ]
     */
    WT_RET(__wt_calloc_def(session, delta_size, &delta_size_each));
    WT_ERR(__wt_calloc_def(session, delta_size, &unpacked_deltas));
    for (i = 0, j = 0; i < (uint32_t)delta_size; ++i, j = 0) {
        header = (WT_PAGE_HEADER *)deltas[i].data;
        WT_ASSERT(session, header->u.entries != 0);
        delta_size_each[i] = (size_t)header->u.entries;

        WT_ERR(__wt_calloc_def(session, header->u.entries, &unpacked_deltas[i]));
        WT_CELL_FOREACH_DELTA_INT(session, ref->page->dsk, header, unpacked_deltas[i][j])
        {
            j++;
        }
        WT_CELL_FOREACH_END;
    }

    WT_ERR(__page_merge_internal_deltas(session, unpacked_deltas, 0, (uint32_t)delta_size - 1,
      delta_size_each, &unpacked_deltas_merged, &unpacked_deltas_merged_size));

    WT_ERR(__page_merge_internal_delta_with_base_image(session, ref, unpacked_deltas_merged,
      unpacked_deltas_merged_size, &refs, &refs_entries, &incr));

    /*
     * Constructs a new `p-index` using the merged `refs` list and allocate refs to the new
     * `p-index`.
     */
    pindex_size = sizeof(WT_PAGE_INDEX) + refs_entries * sizeof(WT_REF *);
    WT_ERR(__wt_calloc(session, 1, pindex_size, &pindex));
    incr += pindex_size;
    pindex->index = (WT_REF **)(pindex + 1);
    pindex->entries = (uint32_t)refs_entries;
    for (i = 0; i < pindex->entries; ++i) {
        refs[i]->pindex_hint = i;
        pindex->index[i] = refs[i];
    }

    /* Initialize the reconstructed `p-index` into the internal page */
    WT_INTL_INDEX_SET(ref->page, pindex);
    __wt_cache_page_inmem_incr(session, ref->page, incr, false);

    if (0) {
err:
        if (refs != NULL)
            for (i = 0; i < (uint32_t)refs_entries; ++i)
                __wt_free(session, refs[i]);
    }
    __wt_free(session, refs);
    __wt_free(session, unpacked_deltas_merged);
    if (unpacked_deltas != NULL) {
        for (i = 0; i < (uint32_t)delta_size; ++i)
            __wt_free(session, unpacked_deltas[i]);
        __wt_free(session, unpacked_deltas);
    }
    __wt_free(session, delta_size_each);

    return (ret);
}

/*
 * __page_reconstruct_leaf_delta --
 *     Reconstruct delta on a leaf page
 */
static int
__page_reconstruct_leaf_delta(WT_SESSION_IMPL *session, WT_REF *ref, WT_ITEM *delta)
{
    WT_CELL_UNPACK_DELTA_LEAF unpack;
    WT_CURSOR_BTREE cbt;
    WT_DECL_RET;
    WT_ITEM key, value;
    WT_PAGE *page;
    WT_PAGE_HEADER *header;
    WT_ROW *rip;
    WT_UPDATE *first_upd, *standard_value, *tombstone, *upd;
    size_t size, tmp_size, total_size;

    header = (WT_PAGE_HEADER *)delta->data;
    tmp_size = total_size = 0;
    page = ref->page;

    WT_CLEAR(unpack);

    __wt_btcur_init(session, &cbt);
    __wt_btcur_open(&cbt);

    WT_CELL_FOREACH_DELTA_LEAF(session, header, unpack)
    {
        key.data = unpack.key;
        key.size = unpack.key_size;
        upd = standard_value = tombstone = NULL;
        size = 0;

        /* Search the page and apply the modification. */
        WT_ERR(__wt_row_search(&cbt, &key, true, ref, true, NULL));
        /*
         * Deltas are applied from newest to oldest, ignore keys that have already got a delta
         * update.
         */
        if (cbt.compare == 0) {
            if (cbt.ins != NULL) {
                if (cbt.ins->upd != NULL && F_ISSET(cbt.ins->upd, WT_UPDATE_RESTORED_FROM_DELTA))
                    continue;
            } else {
                rip = &page->pg_row[cbt.slot];
                first_upd = WT_ROW_UPDATE(page, rip);
                if (first_upd != NULL && F_ISSET(first_upd, WT_UPDATE_RESTORED_FROM_DELTA))
                    continue;
            }
        }

        if (F_ISSET(&unpack, WT_DELTA_LEAF_IS_DELETE)) {
            WT_ERR(__wt_upd_alloc_tombstone(session, &tombstone, &tmp_size));
            F_SET(tombstone, WT_UPDATE_DELETE_DURABLE | WT_UPDATE_RESTORED_FROM_DELTA);
            size += tmp_size;
            upd = tombstone;
        } else {
            value.data = unpack.value;
            value.size = unpack.value_size;
            WT_ERR(__wt_upd_alloc(session, &value, WT_UPDATE_STANDARD, &standard_value, &tmp_size));
            standard_value->txnid = unpack.tw.start_txn;
            standard_value->upd_start_ts = unpack.tw.start_ts;
            standard_value->upd_durable_ts = unpack.tw.durable_start_ts;
            if (WT_TIME_WINDOW_HAS_START_PREPARE(&unpack.tw)) {
                standard_value->prepared_id = unpack.tw.start_prepared_id;
                standard_value->prepare_ts = unpack.tw.start_prepare_ts;
                standard_value->prepare_state = WT_PREPARE_INPROGRESS;

                F_SET(standard_value, WT_UPDATE_PREPARE_RESTORED_FROM_DS);
            } else
                F_SET(standard_value, WT_UPDATE_DURABLE | WT_UPDATE_RESTORED_FROM_DELTA);
            size += tmp_size;

            if (WT_TIME_WINDOW_HAS_STOP(&unpack.tw)) {
                WT_ERR(__wt_upd_alloc_tombstone(session, &tombstone, &tmp_size));
                tombstone->txnid = unpack.tw.stop_txn;
                tombstone->upd_start_ts = unpack.tw.stop_ts;
                tombstone->upd_durable_ts = unpack.tw.durable_stop_ts;

                if (WT_TIME_WINDOW_HAS_STOP_PREPARE(&unpack.tw)) {
                    tombstone->prepared_id = unpack.tw.stop_prepared_id;
                    tombstone->prepare_ts = unpack.tw.stop_prepare_ts;
                    tombstone->prepare_state = WT_PREPARE_INPROGRESS;
                    F_SET(
                      tombstone, WT_UPDATE_PREPARE_DURABLE | WT_UPDATE_PREPARE_RESTORED_FROM_DS);

                    if (WT_TIME_WINDOW_HAS_START_PREPARE(&unpack.tw)) {
                        standard_value->prepared_id = unpack.tw.start_prepared_id;
                        standard_value->prepare_ts = unpack.tw.start_prepare_ts;
                        standard_value->prepare_state = WT_PREPARE_INPROGRESS;
                        F_SET(standard_value,
                          WT_UPDATE_PREPARE_DURABLE | WT_UPDATE_PREPARE_RESTORED_FROM_DS);
                    }
                } else
                    F_SET(tombstone, WT_UPDATE_DURABLE | WT_UPDATE_RESTORED_FROM_DELTA);
                size += tmp_size;
                tombstone->next = standard_value;
                upd = tombstone;
            } else {
                if (WT_TIME_WINDOW_HAS_START_PREPARE(&unpack.tw)) {
                    WT_ASSERT(session,
                      unpack.tw.start_prepared_id != WT_PREPARED_ID_NONE &&
                        unpack.tw.start_prepare_ts != WT_TS_NONE);
                    standard_value->prepared_id = unpack.tw.start_prepared_id;
                    standard_value->prepare_ts = unpack.tw.start_prepare_ts;
                    standard_value->prepare_state = WT_PREPARE_INPROGRESS;
                    F_SET(standard_value,
                      WT_UPDATE_PREPARE_DURABLE | WT_UPDATE_PREPARE_RESTORED_FROM_DS);
                }
                upd = standard_value;
            }
        }

        WT_ERR(__wt_row_modify(&cbt, &key, NULL, &upd, WT_UPDATE_INVALID, true, true));

        total_size += size;
    }
    WT_CELL_FOREACH_END;

    __wt_cache_page_inmem_incr_delta_updates(session, page, total_size);
    WT_STAT_CONN_DSRC_INCRV(session, cache_read_delta_updates, total_size);

    if (0) {
err:
        __wt_free(session, standard_value);
        __wt_free(session, tombstone);
    }
    WT_TRET(__wt_btcur_close(&cbt, true));
    return (ret);
}

/*
 * __wti_page_reconstruct_deltas --
 *     Reconstruct deltas on a page
 */
int
__wti_page_reconstruct_deltas(
  WT_SESSION_IMPL *session, WT_REF *ref, WT_ITEM *deltas, size_t delta_size)
{
    WT_DECL_RET;
    WT_MULTI multi;
    WT_PAGE_MODIFY *mod;
    uint64_t time_start, time_stop;
    int i;
    void *tmp;

    WT_ASSERT(session, delta_size != 0);

    switch (ref->page->type) {
    case WT_PAGE_ROW_LEAF:

        /*
         * We apply the deltas in reverse order because we only care about the latest change of a
         * key. The older changes are ignored.
         */
        time_start = __wt_clock(session);
        for (i = (int)delta_size - 1; i >= 0; --i)
            WT_RET(__page_reconstruct_leaf_delta(session, ref, &deltas[i]));

        /*
         * We may be in a reconciliation already. Don't rewrite in this case as reconciliation is
         * not reentrant.
         *
         * FIXME-WT-14885: this should go away when we use an algorithm to directly rewrite delta.
         */
        if (F_ISSET(&S2C(session)->page_delta, WT_FLATTEN_LEAF_PAGE_DELTA) &&
          !__wt_rec_in_progress(session)) {
            ret = __wt_reconcile(session, ref, false, WT_REC_REWRITE_DELTA);
            mod = ref->page->modify;
            /*
             * We may generate an empty page if the keys all have a globally visible tombstone. Give
             * up the rewrite in this case.
             */
            if (ret == 0 && mod->mod_disk_image != NULL) {
                WT_ASSERT(session, mod->mod_replace.block_cookie == NULL);

                /* The split code works with WT_MULTI structures, build one for the disk image. */
                memset(&multi, 0, sizeof(multi));
                multi.disk_image = mod->mod_disk_image;
                WT_RET(__wt_calloc_one(session, &multi.block_meta));
                *multi.block_meta = ref->page->disagg_info->block_meta;

                /*
                 * Store the disk image to a temporary pointer in case we fail to rewrite the page
                 * and we need to link the new disk image back to the old disk image.
                 */
                tmp = mod->mod_disk_image;
                mod->mod_disk_image = NULL;
                ret = __wt_split_rewrite(session, ref, &multi, false);
                __wt_free(session, multi.block_meta);
                if (ret != 0) {
                    mod->mod_disk_image = tmp;
                    WT_STAT_CONN_DSRC_INCR(session, cache_read_flatten_leaf_delta_fail);
                    WT_RET(ret);
                }

                WT_STAT_CONN_DSRC_INCR(session, cache_read_flatten_leaf_delta);
            } else if (ret != 0) {
                WT_STAT_CONN_DSRC_INCR(session, cache_read_flatten_leaf_delta_fail);
                WT_RET(ret);
            }
        }
        time_stop = __wt_clock(session);
        __wt_stat_usecs_hist_incr_leaf_reconstruct(session, WT_CLOCKDIFF_US(time_stop, time_start));
        WT_STAT_CONN_DSRC_INCR(session, cache_read_leaf_delta);
        break;
    case WT_PAGE_ROW_INT:
        time_start = __wt_clock(session);
        WT_RET(__page_reconstruct_internal_deltas(session, ref, deltas, delta_size));
        time_stop = __wt_clock(session);
        __wt_stat_usecs_hist_incr_internal_reconstruct(
          session, WT_CLOCKDIFF_US(time_stop, time_start));
        WT_STAT_CONN_DSRC_INCR(session, cache_read_internal_delta);
        break;
    default:
        WT_RET(__wt_illegal_value(session, ref->page->type));
    }

    /* The data is written to the disk so we can mark the page clean. */
    __wt_page_modify_clear(session, ref->page);

    return (0);
}

/*
 * __wt_page_block_meta_assign --
 *     Initialize the page's block management metadata.
 */
void
__wt_page_block_meta_assign(WT_SESSION_IMPL *session, WT_PAGE_BLOCK_META *meta)
{
    WT_BTREE *btree;
    uint64_t page_id;

    btree = S2BT(session);

    WT_CLEAR(*meta);
    /*
     * Allocate an interim page ID. If the page is actually being loaded from disk, it's ok to waste
     * some IDs for now.
     */
    page_id = __wt_atomic_fetch_add64(&btree->next_page_id, 1);
    WT_ASSERT(session, page_id >= WT_BLOCK_MIN_PAGE_ID);

    meta->page_id = page_id;
    meta->disagg_lsn = WT_DISAGG_LSN_NONE;
    meta->backlink_lsn = WT_DISAGG_LSN_NONE;
    meta->base_lsn = WT_DISAGG_LSN_NONE;

    /*
     * 0 means there is no delta written for this page yet. We always write a full page for a new
     * page.
     */
    meta->delta_count = 0;
}

/*
 * __wt_page_alloc --
 *     Create or read a page into the cache.
 */
int
__wt_page_alloc(WT_SESSION_IMPL *session, uint8_t type, uint32_t alloc_entries, bool alloc_refs,
  WT_PAGE **pagep, uint32_t flags)
{
    WT_DECL_RET;
    WT_PAGE *page;
    WT_PAGE_INDEX *pindex;
    size_t size;
    uint32_t i;
    void *p;

    WT_UNUSED(flags);

    *pagep = NULL;
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

    /* Allocate the structure that holds the disaggregated information for the page. */
    if (F_ISSET(S2BT(session), WT_BTREE_DISAGGREGATED)) {
        size += sizeof(WT_PAGE_DISAGG_INFO);
        WT_RET(__wt_calloc(session, 1, size, &page));
        page->disagg_info =
          (WT_PAGE_DISAGG_INFO *)((uint8_t *)page + size - sizeof(WT_PAGE_DISAGG_INFO));
    } else
        WT_RET(__wt_calloc(session, 1, size, &page));

    page->type = type;
    __wt_evict_page_init(page);

    switch (type) {
    case WT_PAGE_COL_FIX:
        page->entries = alloc_entries;
        break;
    case WT_PAGE_COL_INT:
    case WT_PAGE_ROW_INT:
        if (!LF_ISSET(WT_PAGE_WITH_DELTAS)) {
            WT_ASSERT(session, alloc_entries != 0);
            /*
             * Internal pages have an array of references to objects so they can split. Allocate the
             * array of references and optionally, the objects to which they point.
             */
            WT_ERR(__wt_calloc(
              session, 1, sizeof(WT_PAGE_INDEX) + alloc_entries * sizeof(WT_REF *), &p));
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
                __wt_page_out(session, &page);
                return (ret);
            }
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
    __wt_cache_page_inmem_incr(session, page, size, false);
    (void)__wt_atomic_add64(&S2C(session)->cache->pages_inmem, 1);
    page->cache_create_gen = __wt_atomic_load64(&S2C(session)->evict->evict_pass_gen);

    *pagep = page;
    return (0);
}

/*
 * __page_inmem_tombstone --
 *     Create the actual update for a tombstone.
 */
static int
__page_inmem_tombstone(
  WT_SESSION_IMPL *session, WT_CELL_UNPACK_KV *unpack, WT_UPDATE **updp, size_t *sizep)
{
    WT_UPDATE *tombstone;
    size_t size, total_size;

    size = 0;
    *sizep = 0;
    *updp = NULL;

    tombstone = NULL;
    total_size = 0;

    WT_ASSERT(session, WT_TIME_WINDOW_HAS_STOP(&unpack->tw));

    WT_RET(__wt_upd_alloc_tombstone(session, &tombstone, &size));
    total_size += size;
    tombstone->upd_durable_ts = unpack->tw.durable_stop_ts;
    tombstone->upd_start_ts = unpack->tw.stop_ts;
    tombstone->txnid = unpack->tw.stop_txn;
    F_SET(tombstone, WT_UPDATE_RESTORED_FROM_DS);
    if (WT_DELTA_LEAF_ENABLED(session))
        F_SET(tombstone, WT_UPDATE_DURABLE);
    *updp = tombstone;
    *sizep = total_size;

    WT_STAT_CONN_DSRC_INCRV(session, cache_read_restored_tombstone_bytes, total_size);

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
    bool delta_enabled;

    size = 0;
    *sizep = 0;

    tombstone = upd = NULL;
    total_size = 0;
    delta_enabled = WT_DELTA_LEAF_ENABLED(session);

    WT_RET(__wt_upd_alloc(session, value, WT_UPDATE_STANDARD, &upd, &size));
    total_size += size;

    /*
     * Instantiate both update and tombstone if the prepared update is a tombstone. This is required
     * to ensure that written prepared delete operation must be removed from the data store, when
     * the prepared transaction gets rollback.
     */
    upd->txnid = unpack->tw.start_txn;
    if (WT_TIME_WINDOW_HAS_START_PREPARE(&(unpack->tw))) {
        upd->prepared_id = unpack->tw.start_prepared_id;
        upd->prepare_ts = unpack->tw.start_prepare_ts;
        upd->upd_durable_ts = WT_TS_NONE;
        upd->upd_start_ts = unpack->tw.start_prepare_ts;
        upd->prepare_state = WT_PREPARE_INPROGRESS;
        F_SET(upd, WT_UPDATE_PREPARE_RESTORED_FROM_DS);
        if (delta_enabled)
            F_SET(upd, WT_UPDATE_PREPARE_DURABLE);
    } else {
        upd->upd_durable_ts = unpack->tw.durable_start_ts;
        upd->upd_start_ts = unpack->tw.start_ts;
        F_SET(upd, WT_UPDATE_RESTORED_FROM_DS);
        if (delta_enabled)
            F_SET(upd, WT_UPDATE_DURABLE);
    }
    if (WT_TIME_WINDOW_HAS_STOP_PREPARE(&(unpack->tw))) {
        WT_ERR(__wt_upd_alloc_tombstone(session, &tombstone, &size));
        total_size += size;
        tombstone->upd_durable_ts = WT_TS_NONE;
        tombstone->txnid = unpack->tw.stop_txn;
        tombstone->prepare_state = WT_PREPARE_INPROGRESS;
        tombstone->upd_start_ts = unpack->tw.stop_prepare_ts;
        tombstone->prepare_ts = unpack->tw.stop_prepare_ts;
        tombstone->prepared_id = unpack->tw.stop_prepared_id;
        tombstone->prepare_state = WT_PREPARE_INPROGRESS;
        F_SET(tombstone, WT_UPDATE_PREPARE_RESTORED_FROM_DS);
        if (delta_enabled)
            F_SET(tombstone, WT_UPDATE_PREPARE_DURABLE);
        tombstone->next = upd;
        *updp = tombstone;
    } else
        *updp = upd;

    *sizep = total_size;
    return (0);

err:
    __wt_free(session, upd);
    __wt_free(session, tombstone);

    return (ret);
}

/*
 * __page_inmem_update --
 *     Create the actual update.
 */
static int
__page_inmem_update(WT_SESSION_IMPL *session, WT_ITEM *value, WT_CELL_UNPACK_KV *unpack,
  WT_UPDATE **updp, size_t *sizep)
{
    if (WT_TIME_WINDOW_HAS_PREPARE(&unpack->tw))
        return (__page_inmem_prepare_update(session, value, unpack, updp, sizep));

    WT_ASSERT(session, WT_TIME_WINDOW_HAS_STOP(&unpack->tw));
    return (__page_inmem_tombstone(session, unpack, updp, sizep));
}

/*
 * __page_inmem_update_col --
 *     Shared code for calling __page_inmem_update on columns.
 */
static int
__page_inmem_update_col(WT_SESSION_IMPL *session, WT_REF *ref, WT_CURSOR_BTREE *cbt, uint64_t recno,
  WT_ITEM *value, WT_CELL_UNPACK_KV *unpack, WT_UPDATE **updp, size_t *sizep)
{
    WT_RET(__page_inmem_update(session, value, unpack, updp, sizep));

    /* Search the page and apply the modification. */
    WT_RET(__wt_col_search(cbt, recno, ref, true, NULL));
    WT_RET(__wt_col_modify(cbt, recno, NULL, updp, WT_UPDATE_INVALID, true, true));
    return (0);
}

/*
 * __wti_page_inmem_updates --
 *     Instantiate updates.
 */
int
__wti_page_inmem_updates(WT_SESSION_IMPL *session, WT_REF *ref)
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
    WT_UPDATE *first_upd, *upd;
    size_t size, total_size;
    uint64_t recno, rle;
    uint32_t i, numtws, tw;
    uint8_t v;

    btree = S2BT(session);
    page = ref->page;
    upd = NULL;
    total_size = 0;

    /* We don't handle in-memory prepare resolution here. */
    WT_ASSERT(
      session, !F_ISSET(S2C(session), WT_CONN_IN_MEMORY) && !F_ISSET(btree, WT_BTREE_IN_MEMORY));

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
            if (!WT_TIME_WINDOW_HAS_PREPARE(&unpack.tw)) {
                recno += rle;
                continue;
            }

            /* Get the value. */
            WT_ERR(__wt_page_cell_data_ref_kv(session, page, &unpack, value));
            WT_ASSERT_ALWAYS(session, __wt_cell_type_raw(unpack.cell) != WT_CELL_VALUE_OVFL_RM,
              "Should never read an overflow removed value for a prepared update");

            /* For each record, create an update to resolve the prepare. */
            for (; rle > 0; --rle, ++recno) {
                /* Create an update to resolve the prepare. */
                WT_ERR(
                  __page_inmem_update_col(session, ref, &cbt, recno, value, &unpack, &upd, &size));
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
            if (!WT_TIME_WINDOW_HAS_PREPARE(&unpack.tw))
                continue;
            recno = ref->ref_recno + page->pg_fix_tws[tw].recno_offset;

            /* Get the value. The update will copy it, so we don't need to allocate here. */
            v = __bit_getv_recno(ref, recno, btree->bitcnt);
            value->data = &v;
            value->size = 1;

            /* Create an update to resolve the prepare. */
            WT_ERR(__page_inmem_update_col(session, ref, &cbt, recno, value, &unpack, &upd, &size));
            total_size += size;
            upd = NULL;
        }
    } else {
        WT_ASSERT(session, page->type == WT_PAGE_ROW_LEAF);
        WT_ERR(__wt_scr_alloc(session, 0, &key));
        bool delta_enabled = WT_DELTA_LEAF_ENABLED(session);
        WT_ROW_FOREACH (page, rip, i) {
            /*
             * Search for prepare records and records with a stop time point if we want to build
             * delta.
             */
            __wt_row_leaf_value_cell(session, page, rip, &unpack);
            if (!WT_TIME_WINDOW_HAS_PREPARE(&unpack.tw) &&
              (!delta_enabled || !WT_TIME_WINDOW_HAS_STOP(&unpack.tw)))
                continue;

            /* Get the key/value pair and instantiate the update. */
            WT_ERR(__wt_row_leaf_key(session, page, rip, key, false));
            WT_ERR(__wt_page_cell_data_ref_kv(session, page, &unpack, value));
            WT_ASSERT_ALWAYS(session, __wt_cell_type_raw(unpack.cell) != WT_CELL_VALUE_OVFL_RM,
              "Should never read an overflow removed value for a prepared update");
            first_upd = WT_ROW_UPDATE(page, rip);
            /*
             * FIXME-WT-14885: This key must have been overwritten by a delta. Don't instantiate it.
             */
            if (first_upd == NULL) {
                WT_ERR(__page_inmem_update(session, value, &unpack, &upd, &size));
                total_size += size;

                /* Search the page and apply the modification. */
                WT_ERR(__wt_row_search(&cbt, key, true, ref, true, NULL));
                WT_ERR(__wt_row_modify(&cbt, key, NULL, &upd, WT_UPDATE_INVALID, true, true));
                upd = NULL;
            } else
                WT_ASSERT(session, F_ISSET(first_upd, WT_UPDATE_RESTORED_FROM_DELTA));
        }
    }

    /*
     * The data is written to the disk so we can mark the page clean after re-instantiating prepared
     * updates to avoid reconciling the page every time.
     */
    __wt_page_modify_clear(session, page);
    __wt_cache_page_inmem_incr(session, page, total_size, false);

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
 * __wti_page_inmem --
 *     Build in-memory page information.
 */
int
__wti_page_inmem(WT_SESSION_IMPL *session, WT_REF *ref, const void *image, uint32_t flags,
  WT_PAGE **pagep, bool *instantiate_updp)
{
    WT_CELL_UNPACK_ADDR unpack_addr;
    WT_DECL_RET;
    WT_PAGE *page;
    const WT_PAGE_HEADER *dsk;
    size_t size;
    uint32_t alloc_entries;

    *pagep = NULL;

    if (instantiate_updp != NULL)
        *instantiate_updp = false;

    dsk = image;
    alloc_entries = 0;

    /*
     * Figure out how many underlying objects the page references so we can allocate them along with
     * the page.
     */
    switch (dsk->type) {
    case WT_PAGE_COL_FIX:
    case WT_PAGE_COL_VAR:
        /*
         * Column-store leaf page entries map one-to-one to the number of physical entries on the
         * page (each physical entry is a value item). Note this value isn't necessarily correct, we
         * may skip values when reading the disk image.
         */
        alloc_entries = dsk->u.entries;
        break;
    case WT_PAGE_COL_INT:
        /*
         * Column-store internal page entries map one-to-one to the number of physical entries on
         * the page (each entry is a location cookie), but in some cases we need to allocate one
         * extra slot. This arises if there's a gap between the page's own start recno and the first
         * child's start recno; we need to insert a blank (deleted) page to cover that chunk of the
         * namespace. Examine the first cell on the page to decide.
         */
        alloc_entries = dsk->u.entries;
        WT_CELL_FOREACH_ADDR (session, dsk, unpack_addr) {
            if (unpack_addr.v != dsk->recno)
                alloc_entries++;
            break;
        }
        WT_CELL_FOREACH_END;
        break;
    case WT_PAGE_ROW_INT:
        /*
         * Row-store internal page entries map one-to-two to the number of physical entries on the
         * page (each entry is a key and location cookie pair).
         */
        if (!LF_ISSET(WT_PAGE_WITH_DELTAS))
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
    WT_RET(__wt_page_alloc(session, dsk->type, alloc_entries, true, &page, flags));
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
        WT_ERR(__inmem_col_fix(session, page, instantiate_updp, &size));
        break;
    case WT_PAGE_COL_INT:
        WT_ERR(__inmem_col_int(session, page, dsk->recno));
        break;
    case WT_PAGE_COL_VAR:
        WT_ERR(__inmem_col_var(session, page, dsk->recno, instantiate_updp, &size));
        break;
    case WT_PAGE_ROW_INT:
        if (!LF_ISSET(WT_PAGE_WITH_DELTAS))
            WT_ERR(__inmem_row_int(session, page, &size));
        break;
    case WT_PAGE_ROW_LEAF:
        WT_ERR(__inmem_row_leaf(session, page, instantiate_updp));
        break;
    default:
        WT_ERR(__wt_illegal_value(session, page->type));
    }

    /* Update the page's cache statistics. */
    __wt_cache_page_inmem_incr(session, page, size, false);

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
 * __wti_col_fix_read_auxheader --
 *     Read the auxiliary header following the bitmap data, if any. This code is used by verify and
 *     needs to be accordingly careful. It is also used by mainline reads so it must also not crash
 *     or print on behalf of verify, and it should not waste time on checks that inmem doesn't need.
 *     Currently this means it does do bounds checks on the header itself (they are embedded in the
 *     integer unpacking) but not on the returned offset, and we don't check the version number.
 *     Careful callers (verify, perhaps debug) should check this. Fast callers (inmem) probably
 *     needn't bother. Salvage is protected by verify and doesn't need to check any of it.
 */
int
__wti_col_fix_read_auxheader(
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
__inmem_col_fix(WT_SESSION_IMPL *session, WT_PAGE *page, bool *instantiate_updp, size_t *sizep)
{
    WT_BTREE *btree;
    WT_CELL_UNPACK_KV unpack;
    WT_COL_FIX_AUXILIARY_HEADER auxhdr;
    const WT_PAGE_HEADER *dsk;
    size_t size;
    uint64_t tmp;
    uint32_t entry_num, recno_offset, skipped;
    const uint8_t *p8;
    bool instantiate_upd;
    void *pv;

    btree = S2BT(session);
    dsk = page->dsk;
    tmp = 0;
    instantiate_upd = false;

    page->pg_fix_bitf = WT_PAGE_HEADER_BYTE(btree, dsk);

    WT_RET(__wti_col_fix_read_auxheader(session, dsk, &auxhdr));
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
                if (WT_TIME_WINDOW_HAS_PREPARE(&(unpack.tw)))
                    instantiate_upd = true;
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
         * If we skipped "quite a few" entries (threshold is arbitrary), and the tree is already
         * dirty and so will be written, mark the page dirty so it gets rewritten without them.
         */
        if (btree->modified && skipped >= auxhdr.entries / 4 && skipped >= dsk->u.entries / 100 &&
          skipped > 4) {
            WT_RET(__wt_page_modify_init(session, page));
            __wt_page_only_modify_set(session, page);
        }

        break;
    }

    /* Report back whether we found a prepared value. */
    if (instantiate_updp != NULL && instantiate_upd)
        *instantiate_updp = true;

    return (0);
}

/*
 * __inmem_col_int_init_ref --
 *     Initialize one ref in a column-store internal page.
 */
static int
__inmem_col_int_init_ref(WT_SESSION_IMPL *session, WT_REF *ref, WT_PAGE *home, uint32_t hint,
  void *addr, uint64_t recno, bool internal, bool deleted, WT_PAGE_DELETED *page_del)
{
    WT_BTREE *btree;

    btree = S2BT(session);

    ref->home = home;
    ref->pindex_hint = hint;
    ref->addr = addr;
    ref->ref_recno = recno;
    F_SET(ref, internal ? WT_REF_FLAG_INTERNAL : WT_REF_FLAG_LEAF);
    if (deleted) {
        /*
         * If a page was deleted without being read (fast truncate), and the delete committed, but
         * older transactions in the system required the previous version of the page to remain
         * available or the delete can still be rolled back by RTS, a deleted-address type cell is
         * type written. We'll see that cell on a page if we read from a checkpoint including a
         * deleted cell or if we crash/recover and start off from such a checkpoint. Recreate the
         * fast-delete state for the page.
         */
        if (page_del != NULL && F_ISSET(home->dsk, WT_PAGE_FT_UPDATE)) {
            WT_RET(__wt_calloc_one(session, &ref->page_del));
            *ref->page_del = *page_del;
        }
        WT_REF_SET_STATE(ref, WT_REF_DELETED);

        /*
         * If the tree is already dirty and so will be written, mark the page dirty. (We want to
         * free the deleted pages, but if the handle is read-only or if the application never
         * modifies the tree, we're not able to do so.)
         */
        if (btree->modified) {
            WT_RET(__wt_page_modify_init(session, home));
            __wt_page_only_modify_set(session, home);
        }
    }

    return (0);
}

/*
 * __inmem_col_int --
 *     Build in-memory index for column-store internal pages.
 */
static int
__inmem_col_int(WT_SESSION_IMPL *session, WT_PAGE *page, uint64_t page_recno)
{
    WT_CELL_UNPACK_ADDR unpack;
    WT_PAGE_INDEX *pindex;
    WT_REF **refp, *ref;
    uint32_t hint;
    bool first;

    first = true;

    /*
     * Walk the page, building references: the page contains value items. The value items are
     * on-page items (WT_CELL_VALUE).
     */
    WT_INTL_INDEX_GET_SAFE(page, pindex);
    refp = pindex->index;
    hint = 0;
    WT_CELL_FOREACH_ADDR (session, page->dsk, unpack) {
        ref = *refp++;

        if (first && unpack.v != page_recno) {
            /*
             * There's a gap in the namespace. Create a deleted leaf page (with no address) to cover
             * that gap. We allocated an extra slot in the array in __wt_page_alloc to make room for
             * this case. (Note that this doesn't result in all gaps being covered, just ones on the
             * left side of the tree where we need to be able to search to them. Other gaps end up
             * covered by the insert list of the preceding leaf page.)
             */

            /* Assert that we allocated enough space for the extra ref. */
            WT_ASSERT(session, pindex->entries == page->dsk->u.entries + 1);

            /* Fill it in. */
            WT_RET(__inmem_col_int_init_ref(
              session, ref, page, hint++, NULL, page_recno, false, true, NULL));

            /* Get the next ref. */
            ref = *refp++;
        }
        first = false;

        WT_RET(__inmem_col_int_init_ref(session, ref, page, hint++, unpack.cell, unpack.v,
          unpack.type == WT_CELL_ADDR_INT, unpack.type == WT_CELL_ADDR_DEL, &unpack.page_del));
    }
    WT_CELL_FOREACH_END;

    return (0);
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
  WT_SESSION_IMPL *session, WT_PAGE *page, uint64_t recno, bool *instantiate_updp, size_t *sizep)
{
    WT_CELL_UNPACK_KV unpack;
    WT_COL *cip;
    WT_COL_RLE *repeats;
    size_t size;
    uint64_t rle;
    uint32_t indx, n, repeat_off;
    bool instantiate_upd;
    void *p;

    repeats = NULL;
    repeat_off = 0;
    instantiate_upd = false;

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
        if (WT_TIME_WINDOW_HAS_PREPARE(&(unpack.tw)))
            instantiate_upd = true;

        indx++;
        recno += rle;
    }
    WT_CELL_FOREACH_END;

    if (instantiate_updp != NULL && instantiate_upd)
        *instantiate_updp = true;

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
    WT_INTL_INDEX_GET_SAFE(page, pindex);
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
            __wt_ref_key_onpage_set(page, ref, &unpack);
            break;
        case WT_CELL_KEY_OVFL:
            /*
             * Instantiate any overflow keys; WiredTiger depends on this, assuming any overflow key
             * is instantiated, and any keys that aren't instantiated cannot be overflow items.
             */
            WT_ERR(__wt_dsk_cell_data_ref_addr(session, &unpack, current));

            WT_ERR(__wti_row_ikey_incr(session, page, WT_PAGE_DISK_OFFSET(page, unpack.cell),
              current->data, current->size, ref));

            *sizep += sizeof(WT_IKEY) + current->size;
            overflow_keys = true;
            break;
        case WT_CELL_ADDR_DEL:
            /*
             * If a page was deleted without being read (fast truncate), and the delete committed,
             * but older transactions in the system required the previous version of the page to
             * remain available or the delete can still be rolled back by RTS, a deleted-address
             * type cell is written. We'll see that cell on a page if we read from a checkpoint
             * including a deleted cell or if we crash/recover and start off from such a checkpoint.
             * Recreate the fast-delete state for the page.
             */
            if (F_ISSET(page->dsk, WT_PAGE_FT_UPDATE)) {
                WT_ERR(__wt_calloc_one(session, &ref->page_del));
                *ref->page_del = unpack.page_del;
            }
            WT_REF_SET_STATE(ref, WT_REF_DELETED);

            /*
             * If the tree is already dirty and so will be written, mark the page dirty. (We want to
             * free the deleted pages, but if the handle is read-only or if the application never
             * modifies the tree, we're not able to do so.)
             */
            if (btree->modified) {
                WT_ERR(__wt_page_modify_init(session, page));
                __wt_page_only_modify_set(session, page);
            }

            ref->addr = unpack.cell;
            ++refp;
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
__inmem_row_leaf(WT_SESSION_IMPL *session, WT_PAGE *page, bool *instantiate_updp)
{
    WT_CELL_UNPACK_KV unpack;
    WT_DECL_RET;
    WT_ROW *rip;
    uint32_t best_prefix_count, best_prefix_start, best_prefix_stop;
    uint32_t last_slot, prefix_count, prefix_start, prefix_stop, slot;
    uint8_t smallest_prefix;
    bool instantiate_upd, delta_enabled;

    last_slot = 0;
    instantiate_upd = false;
    delta_enabled = WT_DELTA_LEAF_ENABLED(session);

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
             * Pages from checkpoint-related files that have been pushed onto the pre-fetch queue
             * will be comprised of data that is globally visible, and so the reader thread which
             * attempts to read the page into cache can skip the visible all check.
             */
            if (!(WT_READING_CHECKPOINT(session) && F_ISSET(session, WT_SESSION_PREFETCH_THREAD)) &&
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

        /*
         * If we find a prepare, we'll have to instantiate it in the update chain later. Also
         * instantiate the tombstone if leaf delta is enabled. We need the tombstone to trace
         * whether we have included the delete in the delta or not.
         */
        if (WT_TIME_WINDOW_HAS_PREPARE(&(unpack.tw)) ||
          (delta_enabled && WT_TIME_WINDOW_HAS_STOP(&unpack.tw)))
            instantiate_upd = true;
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

    if (instantiate_updp != NULL && instantiate_upd)
        *instantiate_updp = true;

err:
    return (ret);
}
