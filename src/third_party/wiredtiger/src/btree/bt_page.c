/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __inmem_col_int(WT_SESSION_IMPL *, WT_PAGE *, uint64_t);
static int __inmem_col_var(WT_SESSION_IMPL *, WT_PAGE *, uint64_t, bool *, size_t *);
static int __inmem_row_int(WT_SESSION_IMPL *, WT_PAGE *, size_t *);
static int __inmem_row_leaf(WT_SESSION_IMPL *, WT_PAGE *, bool *);
static int __inmem_row_leaf_entries(WT_SESSION_IMPL *, const WT_PAGE_HEADER *, uint32_t *);

/*
 * __page_find_min_delta_int --
 *     Identify the stream with the smallest key across all active internal delta streams, unpacking
 *     the head entry of each stream on demand.
 *
 * Iterates backward from the latest delta stream (highest index) to the earliest to enforce "latest
 *     update wins": when duplicate keys exist, the higher-indexed (newer) delta is preferred and
 *     older duplicates are discarded.
 */
static int
__page_find_min_delta_int(
  WT_SESSION_IMPL *session, WTI_DELTA_INT_MERGE_STATE s[], int32_t *min_d, int32_t delta_count)
{
    WT_ITEM cur_key, min_key;
    int32_t j;
    int cmp;

    j = *min_d;

    /*
     * Iterate backward from the latest delta stream (highest index) to the earliest (lowest index).
     * This ensures that when we encounter a duplicate key, the one we already have is the LATEST.
     */
    for (int32_t i = delta_count - 1; i >= 0; --i) {
        if (s[i].entries == 0)
            continue;

        /*
         * Unpack on demand: the first time a stream entry is visited, decode both the key and value
         * and mark it as unpacked. On subsequent visits to the same entry before it is consumed,
         * skip decoding and compare against the already-decoded key.
         */
        if (!s[i].unpacked)
            WT_CELL_DELTA_INT_UNPACK(session, &s[i]);

        if (j == -1) {
            j = i;
            continue;
        }

        cur_key.data = s[i].unpack.key.data;
        cur_key.size = s[i].unpack.key.size;
        min_key.data = s[j].unpack.key.data;
        min_key.size = s[j].unpack.key.size;

        WT_RET(__wt_compare(session, S2BT(session)->collator, &cur_key, &min_key, &cmp));

        if (cmp < 0)
            /* Found a smaller key --> update minimum. */
            j = i;
        else if (cmp == 0) {
            /*
             * Keys are equal. Because we iterate from latest --> earliest, j (higher-indexed) is
             * the latest. Discard this older duplicate: clear `unpacked` so the next call to this
             * function re-enters WT_CELL_DELTA_INT_UNPACK for stream i, which (because `cell` was
             * already advanced during the unpack above) will decode the entry after the duplicate.
             */
            WT_ASSERT(session, s[i].entries >= 2);
            s[i].unpacked = false;
            s[i].entries -= 2;
        }
        /* If cmp > 0, keep the current minimum j. */
    }

    *min_d = j;
    return (0);
}

/*
 * __page_find_min_delta_leaf --
 *     Unpack and find the next min key leaf delta.
 */
static int
__page_find_min_delta_leaf(WT_SESSION_IMPL *session, WT_ITEM *deltas,
  WTI_DELTA_LEAF_MERGE_STATE s[], int32_t *jp, int32_t delta_size)
{
    int cmp;
    int32_t j = *jp;

    /*
     * Iterate backward from the latest delta stream (highest index) to the earliest (lowest index).
     * This ensures that when we encounter a duplicate key, the one we already have is the LATEST.
     */
    for (int32_t i = delta_size - 1; i >= 0; --i) {
        if (s[i].entries == 0)
            continue;
        /*
         * Unpack first if it is not unpacked yet, otherwise the entry is unpacked and the key has
         * been prefix decompressed and stored in last keys.
         */
        if (!s[i].unpacked) {
            WT_CELL_DELTA_LEAF_UNPACK(
              session, (WT_PAGE_HEADER *)deltas[i].data, s[i].unpack, s[i].cell);

            WT_ASSERT(
              session, s[i].unpack->delta_key.data != NULL || s[i].unpack->delta_key.size == 0);

            WT_RET(__wt_cell_decompress_prefix_key(session, s[i].current_key,
              s[i].unpack->delta_key.data, s[i].unpack->delta_key.size,
              s[i].unpack->delta_key.prefix));
            s[i].unpacked = true;
        }

        if (j == -1)
            j = i;
        else {
            /* Compare the current key against the current minimum. */
            WT_RET(__wt_compare(
              session, S2BT(session)->collator, s[i].current_key, s[j].current_key, &cmp));
            if (cmp < 0)
                j = i;
            else if (cmp == 0) {
                /*
                 * Keys are equal. Because we iterate from latest --> earliest, the current minimum
                 * (from a higher-indexed delta) is the latest. Skip this older duplicate.
                 */
                s[i].unpacked = false;
                s[i].entries -= 2;
            }
        }
    }

    *jp = j;
    return (0);
}

/*
 * __page_unpack_leaf_kv --
 *     Unpack a key-value pair at given cell offset for a disk image.
 */
static int
__page_unpack_leaf_kv(WT_SESSION_IMPL *session, WTI_BASE_LEAF_MERGE_STATE *s, WT_PAGE_HEADER *dsk)
{
    /* Unpack the key if we did not find the key in the previous run. */
    if (!s->empty_value_cell) {
        __wt_cell_unpack_kv(session, dsk, (WT_CELL *)s->cell, s->unpack_key);
        s->cell += s->unpack_key->__len;
        WT_ASSERT(session, s->unpack_value->type != WT_CELL_KEY_OVFL);
    }

    /* Decompress prefix compressed key. */
    WT_RET(__wt_cell_decompress_prefix_key(
      session, s->current_key, s->unpack_key->data, s->unpack_key->size, s->unpack_key->prefix));

    /*
     * Unpack the value if there are entries left. If the entry has an empty value cell, we have
     * already unpacked the next key. In that case, we set empty_value_cell and we know unpack_value
     * is actually pointing to the next key.
     */
    if (s->entries > 1) {
        __wt_cell_unpack_kv(session, dsk, (WT_CELL *)s->cell, s->unpack_value);
        s->cell += s->unpack_value->__len;
        WT_ASSERT(session,
          s->unpack_value->type != WT_CELL_KEY_OVFL && s->unpack_value->type != WT_CELL_VALUE_OVFL);
        s->empty_value_cell = s->unpack_value->type == WT_CELL_KEY;
    } else
        /*
         * If there's only one entry left then it must be the last entry with a key cell and an
         * empty value cell.
         */
        s->empty_value_cell = true;

    /* We've just unpacked a k/v pair. */
    s->unpacked = true;
    return (0);
}

/*
 * __page_init_base_leaf_merge_state --
 *     Initialize base leaf merge state.
 */
static int
__page_init_base_leaf_merge_state(
  WT_SESSION_IMPL *session, WT_BTREE *btree, WT_PAGE_HEADER *base_dsk, WTI_BASE_LEAF_MERGE_STATE *s)
{
    s->entries = base_dsk->u.entries;
    s->cell = WT_PAGE_HEADER_BYTE(btree, base_dsk);
    s->unpacked = false;
    s->empty_value_cell = false;

    WT_RET(__wt_calloc_one(session, &s->unpack_key));
    WT_RET(__wt_calloc_one(session, &s->unpack_value));
    WT_RET(__wt_scr_alloc(session, 0, &s->current_key));

    return (0);
}

/*
 * __page_free_base_leaf_merge_state --
 *     Free base leaf merge state.
 */
static void
__page_free_base_leaf_merge_state(WT_SESSION_IMPL *session, WTI_BASE_LEAF_MERGE_STATE *s)
{
    __wt_free(session, s->unpack_key);
    __wt_free(session, s->unpack_value);
    __wt_scr_free(session, &s->current_key);
}

/*
 * __page_init_delta_leaf_merge_state --
 *     Initialize delta leaf merge state.
 */
static int
__page_init_delta_leaf_merge_state(WT_SESSION_IMPL *session, WT_BTREE *btree, WT_ITEM *deltas,
  size_t delta_size, WTI_DELTA_LEAF_MERGE_STATE **sp)
{
    WTI_DELTA_LEAF_MERGE_STATE *s = NULL;
    WT_RET(__wt_calloc_def(session, delta_size, &s));

    for (size_t i = 0; i < delta_size; i++) {
        WT_PAGE_HEADER *tmp = (WT_PAGE_HEADER *)deltas[i].data;
        s[i].cell = WT_PAGE_HEADER_BYTE(btree, tmp);
        s[i].entries = tmp->u.entries;
        s[i].unpacked = false;
        WT_RET(__wt_scr_alloc(session, 0, &s[i].current_key));
        WT_RET(__wt_calloc_one(session, &s[i].unpack));
    }
    *sp = s;

    return (0);
}

/*
 * __page_free_delta_leaf_merge_state --
 *     Free delta leaf merge state.
 */
static void
__page_free_delta_leaf_merge_state(
  WT_SESSION_IMPL *session, size_t delta_size, WTI_DELTA_LEAF_MERGE_STATE **sp)
{
    WTI_DELTA_LEAF_MERGE_STATE *s = *sp;
    for (size_t i = 0; i < delta_size; i++) {
        __wt_scr_free(session, &s[i].current_key);
        __wt_free(session, s[i].unpack);
    }
    __wt_free(session, s);
}

/*
 * __time_window_clear_obsolete --
 *     Where possible modify time window values to avoid writing obsolete values to the cell.
 */
static WT_INLINE void
__time_window_clear_obsolete(WT_SESSION_IMPL *session, WT_TIME_WINDOW *tw)
{
    /* Return if the time window is empty. */
    if (WT_TIME_WINDOW_IS_EMPTY(tw))
        return;

    /*
     * Check if the start of the time window is globally visible, and if so remove unnecessary
     * values.
     */
    if (__wt_txn_tw_start_visible_all(session, tw)) {
        /* The durable timestamp should never be less than the start timestamp. */
        WT_ASSERT(session, tw->start_ts <= tw->durable_start_ts);

        tw->start_ts = tw->durable_start_ts = WT_TS_NONE;
        tw->start_txn = WT_TXN_NONE;
    }

    /*
     * Check if the stop of the time window is globally visible, and if so remove unnecessary
     * values.
     */
    if (__wt_txn_tw_stop_visible_all(session, tw)) {
        /* The durable timestamp should never be less than the stop timestamp. */
        WT_ASSERT(session, tw->stop_ts <= tw->durable_stop_ts);

        tw->stop_ts = tw->durable_stop_ts = WT_TS_NONE;
        tw->stop_txn = WT_TXN_NONE;
    }
}

/*
 * __page_init_dsk_leaf_merge_state --
 *     Initialize new disk leaf merge state.
 */
static int
__page_init_dsk_leaf_merge_state(
  WT_SESSION_IMPL *session, WT_BTREE *btree, WT_ITEM *new_image, WTI_DISK_LEAF_MERGE_STATE *s)
{
    s->cell_ptr = WT_PAGE_HEADER_BYTE(btree, new_image->mem);
    s->all_empty_value = true;
    s->any_empty_value = false;
    s->entries = 0;
    s->key_pfx_last = 0;

    WT_RET(__wt_scr_alloc(session, 0, &s->last_key));
    return (0);
}

/*
 * __wti_page_merge_deltas_with_base_image_leaf --
 *     Merge leaf deltas with base image into disk image in a single pass. While emitting k/v cells,
 *     incrementally aggregate time windows into ta (if non-NULL, diagnostic builds only).
 */
int
__wti_page_merge_deltas_with_base_image_leaf(WT_SESSION_IMPL *session, WT_ITEM *deltas,
  size_t delta_size, WT_ITEM *new_image, WT_PAGE_HEADER *base_dsk
#ifdef HAVE_DIAGNOSTIC
  ,
  WT_TIME_AGGREGATE *ta
#endif
)
{
    WT_BTREE *btree;
    WT_DECL_RET;
    WT_PAGE_HEADER *dsk;
    int cmp;
    WTI_DELTA_LEAF_MERGE_STATE *delta_state = NULL;
    WTI_BASE_LEAF_MERGE_STATE base_state;
    WTI_DISK_LEAF_MERGE_STATE disk_s;
    /* Min delta index. */
    int32_t j = -1;

    WT_CLEAR(base_state);
    WT_CLEAR(disk_s);
    btree = S2BT(session);
    dsk = NULL;

#ifdef HAVE_DIAGNOSTIC
    WT_TIME_AGGREGATE_INIT_MERGE(ta);
#endif

    WT_ASSERT(session, new_image != NULL);
    WT_ERR(__page_init_delta_leaf_merge_state(session, btree, deltas, delta_size, &delta_state));
    WT_ERR(__page_init_base_leaf_merge_state(session, btree, base_dsk, &base_state));
    WT_ERR(__page_init_dsk_leaf_merge_state(session, btree, new_image, &disk_s));
    new_image->size = WT_PTRDIFF(disk_s.cell_ptr, new_image->mem);

    /* We never prefix compress the first key. */
    disk_s.key_pfx_compress = false;
    for (;;) {
        /* Only find next delta when needed. */
        if (j == -1)
            WT_ERR(
              __page_find_min_delta_leaf(session, deltas, delta_state, &j, (int32_t)delta_size));

        /* Only find next base when we have entries left and not unpacked yet. */
        if (base_state.entries > 0 && !base_state.unpacked)
            WT_ERR(__page_unpack_leaf_kv(session, &base_state, base_dsk));

        /* Check if both base and all deltas are exhausted. */
        if (base_state.entries == 0 && j == -1)
            break;

        if (j == -1)
            cmp = -1;
        else if (base_state.entries == 0)
            cmp = 1;
        else
            WT_ERR(__wt_compare(
              session, btree->collator, base_state.current_key, delta_state[j].current_key, &cmp));

        /* Build disk image */
        if (cmp < 0) {
            __time_window_clear_obsolete(session, &base_state.unpack_value->tw);
            /* Pack row-leaf base key/value. */
            WT_ERR(__wt_cell_pack_leaf_kv(session, base_state.empty_value_cell,
              base_state.current_key->data, base_state.current_key->size,
              base_state.unpack_value->data, base_state.unpack_value->size,
              &base_state.unpack_value->tw, new_image, &disk_s));

#ifdef HAVE_DIAGNOSTIC
            WT_TIME_AGGREGATE_UPDATE(session, ta, &base_state.unpack_value->tw);
#endif
        } else {
            /* Pack row-leaf delta entry. */
            if (!F_ISSET(delta_state[j].unpack, WT_DELTA_LEAF_IS_DELETE)) {
                __time_window_clear_obsolete(session, &delta_state[j].unpack->delta_value.tw);
                WT_ERR(__wt_cell_pack_leaf_kv(session,
                  delta_state[j].unpack->delta_value_data.size == 0 &&
                    WT_TIME_WINDOW_IS_EMPTY(&delta_state[j].unpack->delta_value.tw),
                  delta_state[j].current_key->data, delta_state[j].current_key->size,
                  delta_state[j].unpack->delta_value_data.data,
                  delta_state[j].unpack->delta_value_data.size,
                  &delta_state[j].unpack->delta_value.tw, new_image, &disk_s));

#ifdef HAVE_DIAGNOSTIC
                WT_TIME_AGGREGATE_UPDATE(session, ta, &delta_state[j].unpack->delta_value.tw);
#endif
            }

            /* We've packed a delta entry, reset the unpack status and clear the min delta index. */
            delta_state[j].unpacked = false;
            delta_state[j].entries -= 2;
            j = -1;
        }
        /*
         * There are two possible scenarios:
         * - If cmp < 0, we have packed the base entry to the disk image in this run.
         * - If cmp == 0, the base entry has a duplicate key as the delta entry.
         * In either case, we need to skip the entry by resetting the status.
         */
        if (cmp <= 0) {
            base_state.unpacked = false;
            /* Skip the key cell. */
            --base_state.entries;
            /*
             * If the current entry has an empty value cell, then we have unpacked the next key cell
             * and it is pointed by the unpack_value, swap unpack_key and unpack_value.
             */
            if (base_state.empty_value_cell) {
                WT_CELL_UNPACK_KV *tmp = base_state.unpack_key;
                base_state.unpack_key = base_state.unpack_value;
                base_state.unpack_value = tmp;
            } else
                /* Skip the value cell if the k/v has a non-empty value. */
                --base_state.entries;
        }
        /* After the first iteration, we prefix compress keys if this is configured.*/
        disk_s.key_pfx_compress = btree->prefix_compression;
    }

    /* Finalize header once after all appends. */
    dsk = (WT_PAGE_HEADER *)new_image->mem;
    memset(dsk, 0, sizeof(WT_PAGE_HEADER));
    dsk->u.entries = disk_s.entries;
    dsk->type = WT_PAGE_ROW_LEAF;
    dsk->flags = 0;

    if (disk_s.all_empty_value)
        F_SET(dsk, WT_PAGE_EMPTY_V_ALL);
    if (!disk_s.any_empty_value)
        F_SET(dsk, WT_PAGE_EMPTY_V_NONE);

    /* Compute final on-disk image size using pointer difference. */
    new_image->size = WT_PTRDIFF(disk_s.cell_ptr, new_image->mem);
    WT_ASSERT(session, new_image->size <= new_image->memsize);
    dsk->mem_size = WT_STORE_SIZE(new_image->size);

    dsk->write_gen = ((WT_PAGE_HEADER *)deltas[delta_size - 1].data)->write_gen;
    dsk->reserved = 0;
    dsk->version = WT_PAGE_VERSION_TS;

    /* Clear the memory owned by the block manager. */
    memset(WT_BLOCK_HEADER_REF(dsk), 0, btree->block_header);

err:
    __wt_scr_free(session, &disk_s.last_key);
    __page_free_delta_leaf_merge_state(session, delta_size, &delta_state);
    __page_free_base_leaf_merge_state(session, &base_state);
    return (ret);
}

/*
 * __wti_page_merge_deltas_with_base_image_int --
 *     Merge internal deltas with the base image into a new disk image in a single pass, unpacking
 *     both the base page and each delta stream progressively during the merge. While emitting child
 *     address cells, the merge helper will aggregate child time aggregates into ta (if non-NULL,
 *     diagnostic builds only).
 */
int
__wti_page_merge_deltas_with_base_image_int(WT_SESSION_IMPL *session, WT_ITEM *deltas,
  size_t delta_size, WT_ITEM *new_image, const void *base_image_addr
#ifdef HAVE_DIAGNOSTIC
  ,
  WT_TIME_AGGREGATE *ta
#endif
)
{
    WT_BTREE *btree;
    WT_DECL_RET;
    WT_ITEM base_key_buf, delta_key_buf;
    WT_PAGE_HEADER *base_image_header, *hdr;
    WTI_BASE_INT_MERGE_STATE base_state;
    WTI_DELTA_INT_MERGE_STATE *delta_state;
    uint64_t latest_write_gen;
    uint32_t entry_count;
    uint8_t *cell_ptr;
    int32_t j; /* index of the delta stream with the current minimum key, -1 when not yet found */
    int cmp;

    btree = S2BT(session);
    base_image_header = (WT_PAGE_HEADER *)base_image_addr;
    delta_state = NULL;
    WT_CLEAR(base_state);
    entry_count = 0;
    j = -1;

#ifdef HAVE_DIAGNOSTIC
    WT_TIME_AGGREGATE_INIT_MERGE(ta);
#endif

    /* Retrieve the latest write generation from the last delta. */
    latest_write_gen = ((WT_PAGE_HEADER *)deltas[delta_size - 1].data)->write_gen;

    /*
     * Initialize base page state for progressive unpacking.
     *
     * State invariant: `cell` advances during unpack, not during consume. After a k/v pair is
     * packed into the new image, we clear `unpacked` and decrement `entries` by 2 (one for the key
     * cell, one for the value cell). The next loop iteration reads from the already-advanced `cell`
     * pointer, delivering the next pair.
     */
    base_state.dsk = base_image_header;
    base_state.cell = WT_PAGE_HEADER_BYTE(btree, base_image_header);
    base_state.entries = base_image_header->u.entries;
    base_state.unpacked = false;

    /*
     * Initialize one state struct per delta stream for progressive unpacking.
     *
     * Each stream follows the same invariant as the base: each unpack call advances the read
     * position past both the key and value cells and marks the entry as decoded. Consuming or
     * discarding an entry resets the decoded flag and decrements the remaining count by 2, so the
     * next iteration reads the following entry. Duplicate resolution reuses this mechanism: when an
     * older duplicate is dropped, its state is reset so the next iteration reads past the duplicate
     * to the entry that follows.
     */
    WT_RET(__wt_calloc_def(session, delta_size, &delta_state));
    for (size_t i = 0; i < delta_size; ++i) {
        WT_PAGE_HEADER *dhdr = (WT_PAGE_HEADER *)deltas[i].data;
        delta_state[i].base_dsk = base_image_header;
        delta_state[i].delta_dsk = dhdr;
        delta_state[i].cell = WT_PAGE_HEADER_BYTE(btree, dhdr);
        delta_state[i].entries = dhdr->u.entries;
        delta_state[i].unpacked = false;
    }

    WT_ASSERT(session, new_image != NULL && new_image->mem != NULL);
    WT_ASSERT(session, base_state.entries != 0);

    /*
     * Encode the first key always from the base image. The btrees using customized collators cannot
     * handle a truncated first key.
     */
    WT_CELL_BASE_INT_UNPACK(session, &base_state);

    cell_ptr = WT_PAGE_HEADER_BYTE(btree, new_image->data);
    /*
     * Initialize the size here since the cell packing function uses it to calculate where to begin
     * writing the first packed key and value data.
     */
    new_image->size = WT_PTRDIFF(cell_ptr, new_image->data);

    WT_ERR(__wt_cell_pack_internal_key_addr(
      session, new_image, &base_state.unpack_key, &base_state.unpack_val, NULL, false, &cell_ptr));
    /* key + value cells */
    entry_count += 2;
#ifdef HAVE_DIAGNOSTIC
    WT_TIME_AGGREGATE_MERGE(session, ta, &base_state.unpack_val.ta);
#endif
    base_state.unpacked = false;
    base_state.entries -= 2;

    /*
     * !!!
     * Example: Demonstration of how the merge logic works with base and multiple delta streams.
     *
     * Suppose we have a base and three delta streams (D1 = oldest, D3 = latest):
     *
     *   Base:  [1, 3, 5, 7]
     *   D1:    [2, 3, 6]
     *   D2:    [3, 4, 6, 8]
     *   D3:    [3, 5, 9]
     *
     * Processing steps:
     *   1. Scan from latest to oldest delta stream, unpacking the head entry of each on demand to
     *      find the smallest key. When duplicates are found, newer deltas (higher index) take
     *      precedence.
     *
     *   2. Initially:
     *        - Base points to 3 (1 was already written as the first key)
     *        - D3 points to 3, D2 -> 3, D1 -> 2
     *      Minimum delta key = 2 (from D1) -> emit D1(2); base(3) waits
     *
     *   3. Keys 3 appear in D3, D2, D1, and Base. D3 wins; D2 and D1 duplicates are discarded.
     *      Base entry for key 3 is also skipped (cmp == 0).
     *
     *   4. Continue merging in ascending order:
     *        Emit D3(3), D2(4), D3(5) [base 5 overridden], D1(6), base(7), D2(8), D3(9)
     *
     * Final merged output:
     *   [1(base), 2(D1), 3(D3), 4(D2), 5(D3), 6(D2), 7(base), 8(D2), 9(D3)]
     */
    for (;;) {
        /* Find the minimum delta entry only when needed. */
        if (j == -1)
            WT_ERR(__page_find_min_delta_int(session, delta_state, &j, (int32_t)delta_size));

        /* Check if both base and all deltas are exhausted. */
        if (base_state.entries == 0 && j == -1)
            break;

        /* Unpack the next base entry when needed. */
        if (base_state.entries > 0 && !base_state.unpacked)
            WT_CELL_BASE_INT_UNPACK(session, &base_state);

        /* Diagnostics: log early exhaustion of base or deltas. */
        if (base_state.entries == 0 && j != -1)
            __wt_verbose_debug2(session, WT_VERB_PAGE_DELTA,
              "__wti_page_merge_deltas_with_base_image_int: ran out of base keys before deltas "
              "(delta stream=%" PRId32 "/%" PRIu64 ")",
              j, (uint64_t)delta_size);

        if (base_state.entries > 0 && j == -1)
            __wt_verbose_debug2(session, WT_VERB_PAGE_DELTA,
              "__wti_page_merge_deltas_with_base_image_int: ran out of deltas before base keys "
              "(base_entries_remaining=%" PRIu32 ")",
              base_state.entries);

        /* Determine which stream's entry wins. */
        if (base_state.entries == 0)
            cmp = 1;
        else if (j == -1)
            cmp = -1;
        else {
            base_key_buf.data = base_state.unpack_key.data;
            base_key_buf.size = base_state.unpack_key.size;
            delta_key_buf.data = delta_state[j].unpack.key.data;
            delta_key_buf.size = delta_state[j].unpack.key.size;
            WT_ERR(__wt_compare(session, btree->collator, &base_key_buf, &delta_key_buf, &cmp));
        }

        if (cmp < 0) {
            /* Base entry wins: pack it and advance the base stream. */
            WT_ERR(__wt_cell_pack_internal_key_addr(session, new_image, &base_state.unpack_key,
              &base_state.unpack_val, NULL, false, &cell_ptr));
            /* key + value cells */
            entry_count += 2;
#ifdef HAVE_DIAGNOSTIC
            WT_TIME_AGGREGATE_MERGE(session, ta, &base_state.unpack_val.ta);
#endif
            base_state.unpacked = false;
            base_state.entries -= 2;
        } else {
            /* Delta entry wins (or equal): pack it if visible, then advance the delta stream. */
            if (!__wt_delta_cell_type_visible_all(&delta_state[j].unpack)) {
                WT_ERR(__wt_cell_pack_internal_key_addr(
                  session, new_image, NULL, NULL, &delta_state[j].unpack, true, &cell_ptr));
                /* key + value cells */
                entry_count += 2;
#ifdef HAVE_DIAGNOSTIC
                WT_TIME_AGGREGATE_MERGE(session, ta, &delta_state[j].unpack.value.ta);
#endif
            }
            /* When keys are equal, also consume the base entry. */
            if (cmp == 0) {
                base_state.unpacked = false;
                base_state.entries -= 2;
            }
            delta_state[j].unpacked = false;
            delta_state[j].entries -= 2;
            j = -1;
        }
    }

    /* Finalize the page header once after all entries are packed. */
    hdr = (WT_PAGE_HEADER *)new_image->data;
    memset(hdr, 0, sizeof(WT_PAGE_HEADER));
    hdr->u.entries = entry_count;
    F_SET(hdr, WT_PAGE_FT_UPDATE);

    /* Compute final on-disk image size using pointer arithmetic. */
    new_image->size = WT_PTRDIFF(cell_ptr, new_image->mem);
    WT_ASSERT(session, new_image->size <= new_image->memsize);
    hdr->mem_size = (uint32_t)new_image->size;
    hdr->write_gen = latest_write_gen;
    hdr->type = WT_PAGE_ROW_INT;
    hdr->reserved = 0;
    hdr->version = WT_PAGE_VERSION_TS;

err:
    __wt_free(session, delta_state);
    return (ret);
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
    page_id = __wt_atomic_fetch_add_uint64(&btree->next_page_id, 1);
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
    WT_BTREE *btree;
    WT_CACHE *cache;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_PAGE *page;
    WT_PAGE_INDEX *pindex;
    size_t size;
    uint32_t i;
    void *p;

    WT_UNUSED(flags);

    btree = S2BT(session);
    conn = S2C(session);
    cache = conn->cache;
    *pagep = NULL;
    page = NULL;

    size = sizeof(WT_PAGE);
    switch (type) {
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
    if (F_ISSET(btree, WT_BTREE_DISAGGREGATED)) {
        size += sizeof(WT_PAGE_DISAGG_INFO);
        WT_RET(__wt_calloc(session, 1, size, &page));
        page->disagg_info =
          (WT_PAGE_DISAGG_INFO *)((uint8_t *)page + size - sizeof(WT_PAGE_DISAGG_INFO));
    } else
        WT_RET(__wt_calloc(session, 1, size, &page));

    page->type = type;
    __wt_evict_page_init(page);

    switch (type) {
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
            __wt_page_out(session, &page);
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
    __wt_cache_page_inmem_incr(session, page, size, false);
    (void)__wt_atomic_add_uint64_relaxed(&cache->pages_inmem, 1);
    if (!WT_PAGE_IS_INTERNAL(page))
        (void)__wt_atomic_add_uint64_relaxed(&cache->pages_inmem_leaf, 1);
    if (__wt_conn_is_disagg(session)) {
        if (F_ISSET(btree, WT_BTREE_GARBAGE_COLLECT))
            (void)__wt_atomic_add_uint64_relaxed(&cache->pages_inmem_ingest, 1);
        else if (F_ISSET(btree, WT_BTREE_DISAGGREGATED))
            (void)__wt_atomic_add_uint64_relaxed(&cache->pages_inmem_stable, 1);
    }
    page->cache_create_gen = __wt_atomic_load_uint64_relaxed(&conn->evict->evict_pass_gen);

    *pagep = page;
    return (0);
}

/*
 * __page_inmem_prepare_update --
 *     Create the actual update for a prepared value.
 */
static int
__page_inmem_prepare_update(
  WT_SESSION_IMPL *session, WT_ITEM *value, WT_CELL_UNPACK_KV *unpack, WT_UPDATE **updp)
{
    WT_DECL_RET;
    WT_UPDATE *upd, *tombstone;
    bool is_disagg;

    tombstone = upd = NULL;
    is_disagg = F_ISSET(S2BT(session), WT_BTREE_DISAGGREGATED);

    WT_RET(__wt_upd_alloc(session, value, WT_UPDATE_STANDARD, &upd, NULL));

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
        if (is_disagg)
            F_SET(upd, WT_UPDATE_PREPARE_DURABLE);
    } else {
        upd->upd_durable_ts = unpack->tw.durable_start_ts;
        upd->upd_start_ts = unpack->tw.start_ts;
        F_SET(upd, WT_UPDATE_RESTORED_FROM_DS);
        /*
         * We reach this branch only when the cell carries a prepared stop (the caller only routes
         * here when the time window has a prepare and the if-branch above handled the start-prepare
         * case). Do not mark the restored value as durable: if the prepared tombstone is rolled
         * back, the next reconciliation must write this value again to clear the prepared cell from
         * the disk image, and the durable flag would suppress that write.
         */
        WT_ASSERT(session, WT_TIME_WINDOW_HAS_STOP_PREPARE(&(unpack->tw)));
    }
    if (WT_TIME_WINDOW_HAS_STOP_PREPARE(&(unpack->tw))) {
        WT_ERR(__wt_upd_alloc_tombstone(session, &tombstone, NULL));
        tombstone->upd_durable_ts = WT_TS_NONE;
        tombstone->txnid = unpack->tw.stop_txn;
        tombstone->prepare_state = WT_PREPARE_INPROGRESS;
        tombstone->upd_start_ts = unpack->tw.stop_prepare_ts;
        tombstone->prepare_ts = unpack->tw.stop_prepare_ts;
        tombstone->prepared_id = unpack->tw.stop_prepared_id;
        tombstone->prepare_state = WT_PREPARE_INPROGRESS;
        F_SET(tombstone, WT_UPDATE_PREPARE_RESTORED_FROM_DS);
        if (is_disagg)
            F_SET(tombstone, WT_UPDATE_PREPARE_DURABLE);
        tombstone->next = upd;
        *updp = tombstone;
    } else
        *updp = upd;

    return (0);

err:
    __wt_free(session, upd);
    __wt_free(session, tombstone);

    return (ret);
}

/*
 * __page_inmem_update_col --
 *     Shared code for calling __page_inmem_prepare_update on columns.
 */
static int
__page_inmem_update_col(WT_SESSION_IMPL *session, WT_REF *ref, WT_CURSOR_BTREE *cbt, uint64_t recno,
  WT_ITEM *value, WT_CELL_UNPACK_KV *unpack, WT_UPDATE **updp)
{
    WT_RET(__page_inmem_prepare_update(session, value, unpack, updp));

    /* Search the page and apply the modification. */
    WT_RET(__wt_col_search(cbt, recno, ref, true, NULL));
    return (__wt_col_modify(cbt, recno, NULL, updp, WT_UPDATE_INVALID, true, true));
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
    WT_DECL_ITEM(value);
    WT_DECL_RET;
    WT_PAGE *page;
    WT_ROW *rip;
    WT_UPDATE *upd;
    uint64_t recno, rle;
    uint32_t i;

    btree = S2BT(session);
    page = ref->page;
    upd = NULL;

    /*
     * This variable is only used in assertions so in non-diagnostic builds it throws an unused
     * error.
     */
    WT_UNUSED(btree);
    WT_ASSERT(session, !F_ISSET(btree, WT_BTREE_READONLY));

    /* We don't handle in-memory prepare resolution here. */
    WT_ASSERT(session, !__wt_btree_stays_in_memory(btree));

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
                WT_ERR(__page_inmem_update_col(session, ref, &cbt, recno, value, &unpack, &upd));
                upd = NULL;
            }
        }
    } else {
        WT_ASSERT(session, page->type == WT_PAGE_ROW_LEAF);
        /*
         * We already know each row's slot from WT_ROW_FOREACH, so position the cursor directly
         * instead of calling __wt_row_search (which would binary search for a slot we already
         * have).
         *
         * When compare=0 and ins=NULL, __wt_row_modify writes to mod_row_update[slot] and never
         * reaches the insert path where the key parameter is required.
         */
        __cursor_pos_clear(&cbt);
        cbt.ref = ref;
        cbt.compare = 0;
        WT_ROW_FOREACH (page, rip, i) {
            /* Search for prepare records. */
            __wt_row_leaf_value_cell(session, page, rip, &unpack);
            if (!WT_TIME_WINDOW_HAS_PREPARE(&unpack.tw))
                continue;

            /* Get the value and instantiate the update. */
            WT_ERR(__wt_page_cell_data_ref_kv(session, page, &unpack, value));
            WT_ASSERT_ALWAYS(session, __wt_cell_type_raw(unpack.cell) != WT_CELL_VALUE_OVFL_RM,
              "Should never read an overflow removed value for a prepared update");

            WT_ERR(__page_inmem_prepare_update(session, value, &unpack, &upd));

            cbt.slot = WT_ROW_SLOT(page, rip);
            cbt.ref = ref;
            WT_ERR(__wt_row_modify(&cbt, NULL, NULL, &upd, WT_UPDATE_INVALID, true, true));
            upd = NULL;
        }
    }

    /*
     * The data is written to the disk so we can mark the page clean after re-instantiating prepared
     * updates to avoid reconciling the page every time.
     */
    __wt_page_modify_clear(session, page);

    if (0) {
err:
        __wt_free_update_list(session, &upd);
    }
    WT_TRET(__wt_btcur_close(&cbt, true));
    __wt_scr_free(session, &value);
    return (ret);
}

/*
 * __inmem_shared_dsk_account --
 *     Account a shared disk image's bytes in the page footprint and the owning btree's in-memory
 *     totals.
 */
static void
__inmem_shared_dsk_account(WT_SESSION_IMPL *session, WT_PAGE *page, size_t size)
{
    WT_BTREE *btree = S2BT(session);

    (void)__wt_atomic_add_size_relaxed(&page->memory_footprint, size);
    (void)__wt_atomic_add_uint64_relaxed(&btree->bytes_inmem, size);
    if (WT_PAGE_IS_INTERNAL(page))
        (void)__wt_atomic_add_uint64_relaxed(&btree->bytes_internal, size);
}

/*
 * __wti_page_inmem --
 *     Build in-memory page information.
 */
int
__wti_page_inmem(WT_SESSION_IMPL *session, WT_REF *ref, const void *image, uint32_t flags,
  WT_SHARED_DSK_ITEM *shared_dsk_item, WT_PAGE **pagep, bool *instantiate_updp)
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
        __wt_log_data_dump(session, dsk, dsk->mem_size,
          "page corrupt dump: page type %" PRIu8 ", page size %" PRIu32
          ", write generation %" PRIu64 ", entries %" PRIu32,
          dsk->type, dsk->mem_size, dsk->write_gen, dsk->u.entries);
        return (__wt_illegal_value(session, dsk->type));
    }

    /*
     * Mark the page as tied to the shared disk cache layer if a shared disk item was supplied. Set
     * the local flag before any failure point so if we succeed on page alloc and fail later, page
     * out accounting can identify shared disk pages on the error path.
     */
    if (shared_dsk_item != NULL)
        LF_SET(WT_PAGE_DISK_SHARED);

    /* Allocate and initialize a new WT_PAGE. */
    WT_RET(__wt_page_alloc(session, dsk->type, alloc_entries, true, &page, flags));
    __wt_tsan_suppress_store_wt_page_header_ptr(&page->dsk, dsk);
    F_SET_ATOMIC_16(page, flags);

    /* Update image stats early so the increment is balanced by the page out decrement on error. */
    if (LF_ISSET(WT_PAGE_DISK_ALLOC) && !LF_ISSET(WT_PAGE_DISK_SHARED))
        __wt_cache_page_image_incr(session, page);

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
    case WT_PAGE_COL_INT:
        WT_ERR(__inmem_col_int(session, page, dsk->recno));
        break;
    case WT_PAGE_COL_VAR:
        WT_ERR(__inmem_col_var(session, page, dsk->recno, instantiate_updp, &size));
        break;
    case WT_PAGE_ROW_INT:
        WT_ERR(__inmem_row_int(session, page, &size));
        break;
    case WT_PAGE_ROW_LEAF:
        WT_ERR(__inmem_row_leaf(session, page, instantiate_updp));
        break;
    default:
        __wt_log_data_dump(session, dsk, dsk->mem_size,
          "page corrupt dump: page type %" PRIu8 ", page size %" PRIu32
          ", write generation %" PRIu64 ", entries %" PRIu32,
          dsk->type, dsk->mem_size, dsk->write_gen, dsk->u.entries);
        WT_ERR(__wt_illegal_value(session, page->type));
    }

    /*
     * Update the page's cache statistics. For shared disk pages, cache totals exclude the disk size
     * as it is owned by the shared disk cache layer.
     */
    if (!LF_ISSET(WT_PAGE_DISK_SHARED))
        __wt_cache_page_inmem_incr(session, page, size, false);
    else {
        WT_ASSERT(session, size >= dsk->mem_size);
        __wt_cache_page_inmem_incr(session, page, size - dsk->mem_size, false);
    }

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

    WT_ASSERT(session, shared_dsk_item == NULL || page->disagg_info != NULL);
    if (page->disagg_info != NULL) {
        page->disagg_info->shared_dsk_item = shared_dsk_item;
        /* Count the disk image in the page footprint and this btree's in-memory total. */
        if (LF_ISSET(WT_PAGE_DISK_SHARED))
            __inmem_shared_dsk_account(session, page, dsk->mem_size);
    }

    *pagep = page;
    return (0);

err:
    __wt_page_out(session, &page);
    return (ret);
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

    __wt_tsan_suppress_store_wt_page_ptr_v(&ref->home, home);
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
    WT_BTREE *btree;
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
    btree = S2BT(session);
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
        if (!F_ISSET(btree, WT_BTREE_READONLY) && WT_TIME_WINDOW_HAS_PREPARE(&(unpack.tw)))
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
    WT_BTREE *btree;
    WT_CELL_UNPACK_KV unpack;
    WT_DECL_RET;
    WT_ROW *rip;
    uint32_t best_prefix_count, best_prefix_start, best_prefix_stop;
    uint32_t last_slot, prefix_count, prefix_start, prefix_stop, slot;
    uint8_t smallest_prefix;
    bool instantiate_prepare_upd;

    last_slot = 0;
    btree = S2BT(session);
    instantiate_prepare_upd = false;

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
            /* Checkpoints use a different visibility model, so avoid clearing the timestamps. */
            if (WT_READING_CHECKPOINT(session))
                break;

            /*
             * We need the original timestamps of the ingest tables for the step-up even when they
             * are globally visible.
             */
            if (F_ISSET(btree, WT_BTREE_GARBAGE_COLLECT))
                break;

            /*
             * Simple values without compression can be directly referenced on the page to avoid
             * repeatedly unpacking their cells.
             *
             * The visibility information is not referenced on the page so we need to ensure that
             * the value is globally visible at the point in time where we read the page into cache.
             */
            if (WT_TIME_WINDOW_IS_EMPTY(&unpack.tw) ||
              (!WT_TIME_WINDOW_HAS_STOP(&unpack.tw) &&
                __wt_txn_tw_start_visible_all(session, &unpack.tw)))
                __wt_row_leaf_value_set(rip - 1, &unpack);
            break;
        case WT_CELL_VALUE_OVFL:
            break;
        default:
            WT_ERR(__wt_illegal_value(session, unpack.type));
        }

        /* If we find a prepare, we'll have to instantiate it in the update chain later. */
        if (!F_ISSET(btree, WT_BTREE_READONLY) && WT_TIME_WINDOW_HAS_PREPARE(&unpack.tw))
            instantiate_prepare_upd = true;
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

    if (instantiate_updp != NULL && instantiate_prepare_upd)
        *instantiate_updp = true;

err:
    return (ret);
}
