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
 * Define functions that increment histogram statistics for reconstruction of pages with deltas.
 */
WT_STAT_USECS_HIST_INCR_FUNC(leaf_reconstruct, perf_hist_leaf_reconstruct_latency)

/*
 * __page_find_min_delta --
 *     Identify the smallest key across all active delta streams and return the corresponding delta
 *     entry and its stream index (min_d).
 *
 * This function iterates backward through the delta streams (from latest to earliest) to naturally
 *     enforce the "latest update wins" rule. When duplicate keys exist in multiple deltas, the
 *     later (higher-indexed) delta is automatically preferred, and older duplicates are skipped by
 *     advancing their stream indices. This ensures only the newest visible version of a key is
 *     emitted in subsequent merge operations.
 *
 * Returns 0 on success or a WT_ERR code on failure.
 */
static inline int
__page_find_min_delta(WT_SESSION_IMPL *session, WT_CELL_UNPACK_DELTA_INT **unpacked_deltas,
  size_t *delta_size_each, size_t *delta_idx, size_t delta_count,
  WT_CELL_UNPACK_DELTA_INT **min_delta, uint32_t *min_d)
{
    WT_ITEM delta_key, min_delta_key;
    ssize_t d;
    int cmp;

    /* Initialize output pointers to NULL/UINT32_MAX. */
    *min_delta = NULL;
    *min_d = UINT32_MAX;

    /*
     * Iterate backward from the latest delta stream (highest index) to the earliest (lowest index).
     * This ensures that when we encounter a duplicate key, the one we already have is the LATEST.
     */
    for (d = (ssize_t)delta_count - 1; d >= 0; --d) {
        /* Skip exhausted delta streams. */
        if (delta_idx[d] >= delta_size_each[d])
            continue;

        WT_ASSERT(session, unpacked_deltas[d] != NULL);

        /* Get the key for the current delta entry in stream 'd'. */
        delta_key.data = unpacked_deltas[d][delta_idx[d]].key.data;
        delta_key.size = unpacked_deltas[d][delta_idx[d]].key.size;

        /* If no minimum has been established yet, set the first valid one. */
        if (*min_delta == NULL) {
            *min_delta = &unpacked_deltas[d][delta_idx[d]];
            *min_d = (uint32_t)d;
            continue;
        }

        /* Get the key for the current minimum delta entry. */
        min_delta_key.data = (*min_delta)->key.data;
        min_delta_key.size = (*min_delta)->key.size;

        /* Compare the current key against the current minimum. */
        WT_RET(__wt_compare(session, S2BT(session)->collator, &delta_key, &min_delta_key, &cmp));

        if (cmp < 0) {
            /* Found a smaller key --> update minimum. */
            *min_delta = &unpacked_deltas[d][delta_idx[d]];
            *min_d = (uint32_t)d;
        } else if (cmp == 0) {
            /*
             * Keys are equal. Because we iterate from latest --> earliest, the current minimum
             * (from a higher-indexed delta) is the latest. Skip this older duplicate.
             */
            delta_idx[d]++;
            WT_ASSERT(session, delta_idx[d] <= delta_size_each[d]);
        }
        /* If cmp > 0, keep the current minimum. */
    }

    return (0);
}

/*
 * __page_find_min_delta_leaf --
 *     Unpack and find the next min key leaf delta.
 */
static int
__page_find_min_delta_leaf(WT_SESSION_IMPL *session, WT_ITEM *deltas,
  WTI_DELTA_LEAF_MERGE_STATE s[], int32_t *jp, size_t delta_size)
{
    int cmp;
    int32_t j = *jp;

    /*
     * Iterate backward from the latest delta stream (highest index) to the earliest (lowest index).
     * This ensures that when we encounter a duplicate key, the one we already have is the LATEST.
     */
    for (int32_t i = (int32_t)delta_size - 1; i >= 0; --i) {
        if (s[i].entries == 0)
            continue;
        /*
         * Unpack first if it is not unpacked yet, otherwise the entry is unpacked and the key has
         * been prefix decompressed and stored in last keys.
         */
        if (!s[i].unpacked) {
            WT_CELL_DELTA_LEAF_UNPACK(
              session, (WT_PAGE_HEADER *)deltas[i].data, s[i].unpack, s[i].cell);

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
 * __page_unpack_deltas_internal_new --
 *     Internal helper: allocate and unpack all delta images into arrays.
 */
static int
__page_unpack_deltas_internal_new(WT_SESSION_IMPL *session, WT_ITEM *deltas, size_t delta_size,
  WT_CELL_UNPACK_DELTA_INT ***unpacked_deltasp, size_t **delta_size_eachp,
  const void *base_image_addr)
{
    WT_CELL_UNPACK_DELTA_INT **unpacked_deltas;
    WT_DECL_RET;
    WT_PAGE_HEADER *base_image_page_header;
    size_t *delta_size_each;
    size_t idx, i;

    base_image_page_header = (WT_PAGE_HEADER *)base_image_addr;

    unpacked_deltas = NULL;
    delta_size_each = NULL;

    /* Allocate space to track delta sizes and unpacked deltas. */
    WT_RET(__wt_calloc_def(session, delta_size, &delta_size_each));
    WT_ERR(__wt_calloc_def(session, delta_size, &unpacked_deltas));

    /* Unpack all delta images (do not merge them yet). */
    for (i = 0; i < delta_size; ++i) {
        WT_PAGE_HEADER *header = (WT_PAGE_HEADER *)deltas[i].data;
        size_t entries = header->u.entries / 2; /* key/value pairs */
        delta_size_each[i] = entries;
        WT_ERR(__wt_calloc_def(session, entries, &unpacked_deltas[i]));

        idx = 0;
        WT_CELL_FOREACH_DELTA_INT(session, base_image_page_header, header, unpacked_deltas[i][idx])
        {
            idx++;
        }
        WT_CELL_FOREACH_END;
    }

    *unpacked_deltasp = unpacked_deltas;
    *delta_size_eachp = delta_size_each;
    return (0);

err:
    if (unpacked_deltas != NULL) {
        for (i = 0; i < delta_size; ++i)
            __wt_free(session, unpacked_deltas[i]);
        __wt_free(session, unpacked_deltas);
    }
    __wt_free(session, delta_size_each);
    return (ret);
}

/*
 * __page_unpack_deltas --
 *     Unpack all delta images into individual arrays (generic wrapper for reuse).
 */
static int
__page_unpack_deltas(WT_SESSION_IMPL *session, WT_ITEM *deltas, size_t delta_size,
  WT_CELL_UNPACK_DELTA_INT ***unpacked_deltasp, size_t **delta_size_eachp,
  const void *base_image_addr, bool row_leaf_page, bool row_internal_page)
{
    if (row_leaf_page) {
        /* Implement unpacking for row leaf pages. */
    } else if (row_internal_page)
        /* Implement unpacking for row internal pages. */
        WT_RET(__page_unpack_deltas_internal_new(
          session, deltas, delta_size, unpacked_deltasp, delta_size_eachp, base_image_addr));
    return (0);
}

/*
 * __page_merge_base_internal_deltas --
 *     Merge base and multiple internal delta arrays into a single set of WT_REFs. Always prefers
 *     the latest version (delta) when keys are equal.
 */
static int
__page_merge_base_internal_deltas(WT_SESSION_IMPL *session, WT_CELL_UNPACK_ADDR *base,
  size_t base_entries, WT_CELL_UNPACK_DELTA_INT **unpacked_deltas, size_t *delta_size_each,
  size_t *delta_idx, size_t delta_size, WT_REF *ref, WT_REF ***refsp, size_t *ref_entriesp,
  size_t *incr, WT_ITEM *new_image, bool build_disk, uint64_t latest_write_gen, bool row_leaf_page,
  bool row_internal_page)
{
    WT_CELL_UNPACK_ADDR *base_key, *base_val;
    WT_CELL_UNPACK_DELTA_INT *min_delta;
    WT_ITEM base_key_buf, delta_key_buf;
    WT_REF **refs;
    size_t i = 0, final_entries = 0; /* final_entries = number of WT_REFs emitted */
    uint32_t min_d, entry_count; /* entry_count = number of page cells (cells = keys + values) */
    int cmp;
    WT_PAGE_HEADER *hdr;
    uint8_t *p_ptr;

    WT_ASSERT(session, base != NULL);
    WT_ASSERT(session, base_entries != 0);
    WT_ASSERT(session, refsp != NULL);

    refs = *refsp;
    entry_count = 0;
    min_d = 0;
    min_delta = NULL;
    hdr = NULL;
    p_ptr = NULL;

    WT_UNUSED(new_image);

    /*
     * Encode the first key always from the base image. The btrees using customized collator cannot
     * handle the truncated first key.
     */
    base_key = &base[i++];
    base_val = &base[i++];

    if (build_disk) {
        WT_ASSERT(session, new_image != NULL);
        p_ptr = WT_PAGE_HEADER_BYTE(S2BT(session), new_image->data);
        /*
         * Initialize new_image->size here since __wt_rec_pack_internal_key_addr uses it to
         * calculate where to begin writing the first packed key and value data.
         */
        new_image->size = WT_PTRDIFF(p_ptr, new_image->data);

        WT_RET(__wt_cell_pack_internal_key_addr(
          session, new_image, base_key, base_val, NULL, false, &p_ptr));

        entry_count += 2;   /* key + value cells */
        final_entries += 1; /* one ref (child) emitted */
    } else
        WT_RET(__page_build_ref(
          session, ref, base_key, base_val, NULL, true, &refs[final_entries++], incr));

    /*
     * !!!
     * Example: Demonstration of how the merge logic works with base and multiple delta arrays.
     *
     * Suppose we have a base array and three delta arrays (D1 = oldest, D3 = latest):
     *
     *   Base:  [1, 3, 5, 7]
     *   D1:    [2, 3, 6]
     *   D2:    [3, 4, 6, 8]
     *   D3:    [3, 5, 9]
     *
     * Processing steps:
     *   1. __page_find_min_delta() scans D3  D2  D1 (latest  oldest) to find the smallest key
     *      among the current delta heads. When duplicates are found, newer deltas (higher index)
     *      take precedence.
     *
     *   2. Initially:
     *        - Base points to 1
     *        - D3 points to 3, D2  3, D1  2
     *      Minimum key = 1 (from Base)  emit Base(1)
     *
     *   3. Next smallest across deltas = 2 (from D1)  emit D1(2)
     *
     *   4. Keys 3 appear in D3, D2, and D1. Because D3 is the latest, its version of key 3 wins.
     *      Older duplicates in D2 and D1 are skipped by advancing their indices.
     *
     *   5. Continue merging in ascending order:
     *        Emit D3(3), Base(5 skipped since D3 overrides it), D2(4), D3(5), D1(6), D2(8), D3(9)
     *
     * Final merged output:
     *   [1(base), 2(D1), 3(D3), 4(D2), 5(D3), 6(D1), 8(D2), 9(D3)]
     *
     */
    for (;;) {

        /* Only find next delta when needed */
        if (min_delta == NULL)
            WT_RET(__page_find_min_delta(session, unpacked_deltas, delta_size_each, delta_idx,
              delta_size, &min_delta, &min_d));

        /* Check if both base and all deltas are exhausted. */
        if (i >= base_entries && min_delta == NULL)
            break;

        /* Diagnostics: detect early exhaustion of base keys or deltas. */
        if (i >= base_entries && min_delta != NULL)
            __wt_verbose_debug2(session, WT_VERB_PAGE_DELTA,
              "__page_merge_base_internal_deltas: ran out of base keys before deltas "
              "(base_entries=%" PRIu64 ", delta=%" PRIu64 "/%" PRIu64 ")",
              (uint64_t)base_entries, (uint64_t)min_d, (uint64_t)delta_size);

        if (i < base_entries && min_delta == NULL)
            __wt_verbose_debug2(session, WT_VERB_PAGE_DELTA,
              "__page_merge_base_internal_deltas: ran out of deltas before base keys "
              "(base_entries=%" PRIu64 ", i=%" PRIu64 ")",
              (uint64_t)base_entries, (uint64_t)i);

        if (i >= base_entries)
            cmp = 1;
        else if (min_delta == NULL)
            cmp = -1;
        else {
            base_key_buf.data = base[i].data;
            base_key_buf.size = base[i].size;
            delta_key_buf.data = min_delta->key.data;
            delta_key_buf.size = min_delta->key.size;
            WT_RET(
              __wt_compare(session, S2BT(session)->collator, &base_key_buf, &delta_key_buf, &cmp));
        }
        /* Old implementation: build WT_REFs */
        if (!build_disk) {
            if (cmp < 0) {
                /* Base key < Delta key -> emit base */
                base_key = &base[i++];
                base_val = &base[i++];
                WT_RET(__page_build_ref(
                  session, ref, base_key, base_val, NULL, true, &refs[final_entries++], incr));
            } else {
                /* Either delta < base or delta == base --> emit delta (prefer latest) */
                if (!__wt_delta_cell_type_visible_all(min_delta))
                    WT_RET(__page_build_ref(
                      session, ref, NULL, NULL, min_delta, false, &refs[final_entries++], incr));
                delta_idx[min_d]++;
                if (cmp == 0)
                    i += 2; /* skip base key/value if keys equal */
                /* We consumed this delta, so recompute next round */
                min_delta = NULL;
            }
        } else {
            /* New implementation: build_disk == true: append to new_image */
            if (cmp < 0) {
                /* Base entry wins */
                /*
                 * !!! NOTE: The following commented code is a placeholder for the actual
                 * implementation of base_key = &base[i++]; base_val = &base[i++];
                 */
                if (row_leaf_page) {
                    /* Pack row-leaf base key/value. */
                } else if (row_internal_page) {
                    /*
                     * Pack internal base key/value.
                     * For a base entry, we pass:
                     *   key_entry = &base[i]
                     *   val_entry = &base[i + 1]
                     */
                    WT_RET(__wt_cell_pack_internal_key_addr(
                      session, new_image, &base[i], &base[i + 1], NULL, false, &p_ptr));

                    entry_count += 2;   /* key + value cells */
                    final_entries += 1; /* one ref (child) emitted */
                }
                i += 2;
            } else {
                if (row_leaf_page) {
                    /* Pack row-leaf delta entry. */
                } else if (row_internal_page && !__wt_delta_cell_type_visible_all(min_delta)) {
                    /*
                     * Pack internal delta entry.
                     * For a delta entry, both key and value are within the same structure.
                     * So we pass:
                     *   key_entry = min_delta
                     *   val_entry = min_delta
                     */
                    WT_RET(__wt_cell_pack_internal_key_addr(
                      session, new_image, NULL, NULL, min_delta, true, &p_ptr));
                    entry_count += 2;   /* key + value */
                    final_entries += 1; /* one ref (child) emitted */
                }
                if (cmp == 0)
                    i += 2;
                delta_idx[min_d]++;
                /* We consumed this delta, so recompute next round */
                min_delta = NULL;
            }
        }
    }

    if (build_disk) {
        /* Finalize header once after all appends. */
        hdr = (WT_PAGE_HEADER *)new_image->data;
        memset(hdr, 0, sizeof(WT_PAGE_HEADER));
        hdr->u.entries = entry_count;
        if (row_internal_page)
            F_SET(hdr, WT_PAGE_FT_UPDATE);

        /* Compute final on-disk image size using pointer difference. */
        new_image->size = WT_PTRDIFF(p_ptr, new_image->mem);
        WT_ASSERT(session, new_image->size <= new_image->memsize);
        hdr->mem_size = (uint32_t)new_image->size;

        hdr->write_gen = latest_write_gen;
        hdr->type = WT_PAGE_ROW_INT;
        hdr->unused = 0;
        hdr->version = WT_PAGE_VERSION_TS;
    }

    *ref_entriesp = final_entries;
    *refsp = refs;

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
 * __page_init_dsk_leaf_merge_state --
 *     Initialize new disk leaf merge state.
 */
static int
__page_init_dsk_leaf_merge_state(
  WT_SESSION_IMPL *session, WT_BTREE *btree, WT_ITEM *new_image, WTI_DISK_LEAF_MERGE_STATE *s)
{
    s->p_ptr = WT_PAGE_HEADER_BYTE(btree, new_image->mem);
    s->all_empty_value = true;
    s->any_empty_value = false;
    s->entries = 0;
    s->key_pfx_last = 0;

    WT_RET(__wt_scr_alloc(session, 0, &s->last_key));
    return (0);
}

/*
 * __wti_page_merge_deltas_with_base_image_leaf --
 *     Merge leaf deltas with base image into disk image in a single pass.
 */
int
__wti_page_merge_deltas_with_base_image_leaf(WT_SESSION_IMPL *session, WT_ITEM *deltas,
  size_t delta_size, WT_ITEM *new_image, WT_PAGE_HEADER *base_dsk)
{
    WT_BTREE *btree;
    WT_DECL_RET;
    WT_PAGE_HEADER *dsk;
    int cmp;
    WTI_DELTA_LEAF_MERGE_STATE *delta_s = NULL;
    WTI_BASE_LEAF_MERGE_STATE base_s;
    WTI_DISK_LEAF_MERGE_STATE disk_s;
    /* Min delta index. */
    int32_t j = -1;

    WT_CLEAR(base_s);
    WT_CLEAR(disk_s);
    btree = S2BT(session);
    dsk = NULL;
    WT_ASSERT(session, new_image != NULL);
    WT_ERR(__page_init_delta_leaf_merge_state(session, btree, deltas, delta_size, &delta_s));
    WT_ERR(__page_init_base_leaf_merge_state(session, btree, base_dsk, &base_s));
    WT_ERR(__page_init_dsk_leaf_merge_state(session, btree, new_image, &disk_s));
    new_image->size = WT_PTRDIFF(disk_s.p_ptr, new_image->mem);

    /* We never prefix compress the first key. */
    disk_s.key_pfx_compress = false;
    for (;;) {
        /* Only find next delta when needed. */
        if (j == -1)
            WT_ERR(__page_find_min_delta_leaf(session, deltas, delta_s, &j, delta_size));

        /* Only find next base when we have entries left and not unpacked yet. */
        if (base_s.entries > 0 && !base_s.unpacked)
            WT_ERR(__page_unpack_leaf_kv(session, &base_s, base_dsk));

        /* Check if both base and all deltas are exhausted. */
        if (base_s.entries == 0 && j == -1)
            break;

        if (j == -1)
            cmp = -1;
        else if (base_s.entries == 0)
            cmp = 1;
        else
            WT_ERR(__wt_compare(
              session, btree->collator, base_s.current_key, delta_s[j].current_key, &cmp));

        /* Build disk image */
        if (cmp < 0)
            /* Pack row-leaf base key/value. */
            WT_ERR(__wt_cell_pack_leaf_kv(session, base_s.empty_value_cell,
              base_s.current_key->data, base_s.current_key->size, base_s.unpack_value->data,
              base_s.unpack_value->size, &base_s.unpack_value->tw, new_image, &disk_s));
        else {
            /* Pack row-leaf delta entry. */
            if (!F_ISSET(delta_s[j].unpack, WT_DELTA_LEAF_IS_DELETE))
                WT_ERR(__wt_cell_pack_leaf_kv(session,
                  delta_s[j].unpack->delta_value_data.size == 0 &&
                    WT_TIME_WINDOW_IS_EMPTY(&delta_s[j].unpack->delta_value.tw),
                  delta_s[j].current_key->data, delta_s[j].current_key->size,
                  delta_s[j].unpack->delta_value_data.data,
                  delta_s[j].unpack->delta_value_data.size, &delta_s[j].unpack->delta_value.tw,
                  new_image, &disk_s));

            /* We've packed a delta entry, reset the unpack status and clear the min delta index. */
            delta_s[j].unpacked = false;
            delta_s[j].entries -= 2;
            j = -1;
        }
        /*
         * There are two possible scenarios:
         * - If cmp < 0, we have packed the base entry to the disk image in this run.
         * - If cmp == 0, the base entry has a duplicate key as the delta entry.
         * In either case, we need to skip the entry by resetting the status.
         */
        if (cmp <= 0) {
            base_s.unpacked = false;
            /* Skip the key cell. */
            --base_s.entries;
            /*
             * If the current entry has an empty value cell, then we have unpacked the next key cell
             * and it is pointed by the unpack_value, swap unpack_key and unpack_value.
             */
            if (base_s.empty_value_cell) {
                WT_CELL_UNPACK_KV *tmp = base_s.unpack_key;
                base_s.unpack_key = base_s.unpack_value;
                base_s.unpack_value = tmp;
            } else
                /* Skip the value cell if the k/v has a non-empty value. */
                --base_s.entries;
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
    new_image->size = WT_PTRDIFF(disk_s.p_ptr, new_image->mem);
    WT_ASSERT(session, new_image->size <= new_image->memsize);
    dsk->mem_size = WT_STORE_SIZE(new_image->size);

    dsk->write_gen = ((WT_PAGE_HEADER *)deltas[delta_size - 1].data)->write_gen;
    dsk->unused = 0;
    dsk->version = WT_PAGE_VERSION_TS;

    /* Clear the memory owned by the block manager. */
    memset(WT_BLOCK_HEADER_REF(dsk), 0, btree->block_header);

err:
    __wt_scr_free(session, &disk_s.last_key);
    __page_free_delta_leaf_merge_state(session, delta_size, &delta_s);
    __page_free_base_leaf_merge_state(session, &base_s);
    return (ret);
}

/*
 * __wti_page_merge_deltas_with_base_image_int --
 *     Merge deltas with base image into disk image in a single pass.
 */
int
__wti_page_merge_deltas_with_base_image_int(WT_SESSION_IMPL *session, WT_REF *ref, WT_ITEM *deltas,
  size_t delta_size, WT_REF ***refsp, size_t *ref_entriesp, size_t *incr, WT_ITEM *new_image,
  const void *base_image_addr)
{
    WT_CELL_UNPACK_ADDR *base = NULL;
    WT_CELL_UNPACK_DELTA_INT **unpacked_deltas = NULL;
    WT_DECL_RET;
    WT_REF **refs = NULL;
    size_t *delta_size_each = NULL, *delta_idx = NULL;
    size_t base_entries, estimated_entries, k;
    uint32_t d;
    WT_PAGE_HEADER *base_image_header = (WT_PAGE_HEADER *)base_image_addr;
    uint64_t latest_write_gen;
    bool row_leaf_page = false, row_internal_page = false;

    if (F_ISSET(ref, WT_REF_FLAG_INTERNAL))
        row_internal_page = true;
    else if (F_ISSET(ref, WT_REF_FLAG_LEAF))
        row_leaf_page = true;
    else
        return (__wt_illegal_value(session, ref->home->type));

    WT_RET(__page_unpack_deltas(session, deltas, delta_size, &unpacked_deltas, &delta_size_each,
      base_image_addr, row_leaf_page, row_internal_page));

    /* Retrieve the latest write generation from the last delta. */
    latest_write_gen = ((WT_PAGE_HEADER *)deltas[delta_size - 1].data)->write_gen;

    k = 0;
    base_entries = base_image_header->u.entries;
    WT_ERR(__wt_calloc_def(session, base_entries, &base));
    WT_CELL_FOREACH_ADDR (session, base_image_header, base[k]) {
        k++;
    }
    WT_CELL_FOREACH_END;

    estimated_entries = (base_entries / 2) + 1;
    for (d = 0; d < delta_size; ++d)
        estimated_entries += delta_size_each[d];
    WT_ERR(__wt_calloc_def(session, estimated_entries, &refs));
    WT_ERR(__wt_calloc_def(session, delta_size, &delta_idx));

    /* Common merge logic (disk mode) */
    WT_ERR(__page_merge_base_internal_deltas(session, base, base_entries, unpacked_deltas,
      delta_size_each, delta_idx, delta_size, ref, &refs, ref_entriesp, incr, new_image, true,
      latest_write_gen, row_leaf_page, row_internal_page));

    *refsp = refs;
    /*
     * Ownership of 'refs' and its elements is transferred to the caller. Null the local pointer so
     * the local cleanup does not free it.
     */
    refs = NULL;

err:
    if (unpacked_deltas != NULL) {
        for (d = 0; d < delta_size; ++d)
            __wt_free(session, unpacked_deltas[d]);
        __wt_free(session, unpacked_deltas);
    }
    __wt_free(session, delta_size_each);
    __wt_free(session, delta_idx);
    __wt_free(session, base);
    /*
     * If an error happened before we transferred refs ownership, free them. If we successfully
     * transferred ownership we set refs = NULL above so this is a no-op on success.
     */
    if (refs != NULL) {
        size_t i;
        for (i = 0; i < *ref_entriesp; ++i)
            __wt_free(session, refs[i]);
        __wt_free(session, refs);
    }
    return (ret);
}

/*
 * __page_reconstruct_leaf_delta --
 *     Reconstruct delta on a leaf page
 */
static int
__page_reconstruct_leaf_delta(WT_SESSION_IMPL *session, WT_REF *ref, WT_ITEM *delta)
{
    WT_CELL_UNPACK_DELTA_LEAF_KV unpack;
    WT_CURSOR_BTREE cbt;
    WT_DECL_ITEM(lastkey);
    WT_DECL_RET;
    WT_ITEM key, value;
    WT_PAGE *page;
    WT_PAGE_HEADER *header;
    WT_ROW *rip;
    WT_UPDATE *first_upd, *standard_value, *tombstone, *upd;
    size_t size, tmp_size, total_size;
    uint8_t key_prefix;

    header = (WT_PAGE_HEADER *)delta->data;
    tmp_size = total_size = 0;
    page = ref->page;
    standard_value = tombstone = NULL;

    WT_CLEAR(unpack);

    WT_RET(__wt_scr_alloc(session, 0, &lastkey));

    __wt_btcur_init(session, &cbt);
    __wt_btcur_open(&cbt);

    WT_CELL_FOREACH_DELTA_LEAF(session, header, &unpack)
    {
        key.data = unpack.delta_key.data;
        key.size = unpack.delta_key.size;
        key_prefix = unpack.delta_key.prefix;
        /*
         * If the key has no prefix count, no prefix compression work is needed; else check for a
         * previously built key big enough cover this key's prefix count.
         */
        if (key_prefix == 0) {
            lastkey->data = key.data;
            lastkey->size = key.size;
        } else {
            WT_ASSERT(session, lastkey->size >= key_prefix);
            /*
             * Grow the buffer as necessary as well as ensure data has been copied into local buffer
             * space, then append the suffix to the prefix already in the buffer. Don't grow the
             * buffer unnecessarily or copy data we don't need, truncate the item's CURRENT data
             * length to the prefix bytes before growing the buffer.
             */
            lastkey->size = key_prefix;
            WT_ERR(__wt_buf_grow(session, lastkey, key_prefix + key.size));
            memcpy((uint8_t *)lastkey->mem + key_prefix, key.data, key.size);
            lastkey->size = key_prefix + key.size;
        }

        upd = standard_value = tombstone = NULL;
        size = 0;

        /* Search the page and apply the modification. */
        WT_ERR(__wt_row_search(&cbt, lastkey, true, ref, true, NULL));
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
            value.data = unpack.delta_value_data.data;
            value.size = unpack.delta_value_data.size;
            WT_ERR(__wt_upd_alloc(session, &value, WT_UPDATE_STANDARD, &standard_value, &tmp_size));
            standard_value->txnid = unpack.delta_value.tw.start_txn;
            if (WT_TIME_WINDOW_HAS_START_PREPARE(&unpack.delta_value.tw)) {
                standard_value->prepared_id = unpack.delta_value.tw.start_prepared_id;
                standard_value->prepare_ts = unpack.delta_value.tw.start_prepare_ts;
                standard_value->prepare_state = WT_PREPARE_INPROGRESS;
                standard_value->upd_start_ts = unpack.delta_value.tw.start_prepare_ts;

                F_SET(standard_value,
                  WT_UPDATE_PREPARE_DURABLE | WT_UPDATE_PREPARE_RESTORED_FROM_DS |
                    WT_UPDATE_RESTORED_FROM_DELTA);
            } else {
                standard_value->upd_start_ts = unpack.delta_value.tw.start_ts;
                standard_value->upd_durable_ts = unpack.delta_value.tw.durable_start_ts;
                F_SET(standard_value, WT_UPDATE_DURABLE | WT_UPDATE_RESTORED_FROM_DELTA);
            }
            size += tmp_size;

            if (WT_TIME_WINDOW_HAS_STOP(&unpack.delta_value.tw)) {
                WT_ERR(__wt_upd_alloc_tombstone(session, &tombstone, &tmp_size));
                tombstone->txnid = unpack.delta_value.tw.stop_txn;

                if (WT_TIME_WINDOW_HAS_STOP_PREPARE(&unpack.delta_value.tw)) {
                    tombstone->prepared_id = unpack.delta_value.tw.stop_prepared_id;
                    tombstone->prepare_ts = unpack.delta_value.tw.stop_prepare_ts;
                    tombstone->prepare_state = WT_PREPARE_INPROGRESS;
                    tombstone->upd_start_ts = unpack.delta_value.tw.stop_prepare_ts;
                    F_SET(tombstone,
                      WT_UPDATE_PREPARE_DURABLE | WT_UPDATE_PREPARE_RESTORED_FROM_DS |
                        WT_UPDATE_RESTORED_FROM_DELTA);
                } else {
                    tombstone->upd_start_ts = unpack.delta_value.tw.stop_ts;
                    tombstone->upd_durable_ts = unpack.delta_value.tw.durable_stop_ts;
                    F_SET(tombstone, WT_UPDATE_DURABLE | WT_UPDATE_RESTORED_FROM_DELTA);
                }
                size += tmp_size;
                tombstone->next = standard_value;
                upd = tombstone;
            } else
                upd = standard_value;
        }

        WT_ERR(__wt_row_modify(&cbt, lastkey, NULL, &upd, WT_UPDATE_INVALID, true, true));

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
    __wt_scr_free(session, &lastkey);
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
         * FIXME-WT-16211: this should go away when we use an algorithm to directly rewrite delta.
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
        WT_ASSERT_ALWAYS(session, false, "Internal delta reconstruction not supported");
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
 * __wt_page_inmem_update --
 *     Create the actual update.
 */
int
__wt_page_inmem_update(WT_SESSION_IMPL *session, WT_ITEM *value, WT_CELL_UNPACK_KV *unpack,
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
    WT_RET(__wt_page_inmem_update(session, value, unpack, updp, sizep));

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
    uint32_t i;

    btree = S2BT(session);
    page = ref->page;
    upd = NULL;
    total_size = 0;

    /*
     * This variable is only used in assertions so in non-diagnostic builds it throws an unused
     * error.
     */
    WT_UNUSED(btree);
    WT_ASSERT(session, !F_ISSET(btree, WT_BTREE_READONLY));

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
             * FIXME-WT-16211: This key must have been overwritten by a delta. Don't instantiate it.
             */
            if (first_upd == NULL) {
                WT_ERR(__wt_page_inmem_update(session, value, &unpack, &upd, &size));
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
        return (__wt_illegal_value(session, dsk->type));
    }

    /* Allocate and initialize a new WT_PAGE. */
    WT_RET(__wt_page_alloc(session, dsk->type, alloc_entries, true, &page, flags));
    __wt_tsan_suppress_store_wt_page_header_ptr(&page->dsk, dsk);
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
    bool instantiate_upd, delta_enabled;

    last_slot = 0;
    btree = S2BT(session);
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

        /*
         * If we find a prepare, we'll have to instantiate it in the update chain later. Also
         * instantiate the tombstone if leaf delta is enabled. We need the tombstone to trace
         * whether we have included the delete in the delta or not.
         */
        if (!F_ISSET(btree, WT_BTREE_READONLY) &&
          (WT_TIME_WINDOW_HAS_PREPARE(&unpack.tw) ||
            (delta_enabled && WT_TIME_WINDOW_HAS_STOP(&unpack.tw))))
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
