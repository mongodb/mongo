/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * __cell_check_value_validity --
 *     Check the value's validity window for sanity.
 */
static inline int
__cell_check_value_validity(WT_SESSION_IMPL *session, WT_TIME_WINDOW *tw, bool expected_error)
{
#ifdef HAVE_DIAGNOSTIC
    WT_DECL_RET;

    if ((ret = __wt_time_value_validate(session, tw, NULL, false)) != 0)
        return (expected_error ?
            WT_ERROR :
            __wt_panic(session, ret, "value timestamp window failed validation"));
#else
    WT_UNUSED(session);
    WT_UNUSED(tw);
    WT_UNUSED(expected_error);
#endif
    return (0);
}

/*
 * __cell_pack_value_validity --
 *     Pack the validity window for a value.
 */
static inline void
__cell_pack_value_validity(WT_SESSION_IMPL *session, uint8_t **pp, WT_TIME_WINDOW *tw)
{
    uint8_t flags, *flagsp;

    /* Globally visible values have no associated validity window. */
    if (WT_TIME_WINDOW_IS_EMPTY(tw)) {
        ++*pp;
        return;
    }

    WT_IGNORE_RET(__cell_check_value_validity(session, tw, false));

    **pp |= WT_CELL_SECOND_DESC;
    ++*pp;
    flagsp = *pp;
    ++*pp;

    flags = 0;
    if (tw->start_ts != WT_TS_NONE) {
        WT_IGNORE_RET(__wt_vpack_uint(pp, 0, tw->start_ts));
        LF_SET(WT_CELL_TS_START);
    }
    if (tw->start_txn != WT_TXN_NONE) {
        WT_IGNORE_RET(__wt_vpack_uint(pp, 0, tw->start_txn));
        LF_SET(WT_CELL_TXN_START);
    }
    if (tw->durable_start_ts != WT_TS_NONE) {
        WT_ASSERT(session, tw->start_ts <= tw->durable_start_ts);
        /* Store differences if any, not absolutes. */
        if (tw->durable_start_ts - tw->start_ts > 0) {
            WT_IGNORE_RET(__wt_vpack_uint(pp, 0, tw->durable_start_ts - tw->start_ts));
            LF_SET(WT_CELL_TS_DURABLE_START);
        }
    }
    if (tw->stop_ts != WT_TS_MAX) {
        /* Store differences, not absolutes. */
        WT_IGNORE_RET(__wt_vpack_uint(pp, 0, tw->stop_ts - tw->start_ts));
        LF_SET(WT_CELL_TS_STOP);
    }
    if (tw->stop_txn != WT_TXN_MAX) {
        /* Store differences, not absolutes. */
        WT_IGNORE_RET(__wt_vpack_uint(pp, 0, tw->stop_txn - tw->start_txn));
        LF_SET(WT_CELL_TXN_STOP);
    }
    if (tw->durable_stop_ts != WT_TS_NONE) {
        WT_ASSERT(session, tw->stop_ts <= tw->durable_stop_ts);
        /* Store differences if any, not absolutes. */
        if (tw->durable_stop_ts - tw->stop_ts > 0) {
            WT_IGNORE_RET(__wt_vpack_uint(pp, 0, tw->durable_stop_ts - tw->stop_ts));
            LF_SET(WT_CELL_TS_DURABLE_STOP);
        }
    }
    if (tw->prepare)
        LF_SET(WT_CELL_PREPARE);
    *flagsp = flags;
}

/*
 * __wt_check_addr_validity --
 *     Check the address' validity window for sanity.
 */
static inline int
__wt_check_addr_validity(WT_SESSION_IMPL *session, WT_TIME_AGGREGATE *ta, bool expected_error)
{
#ifdef HAVE_DIAGNOSTIC
    WT_DECL_RET;

    if ((ret = __wt_time_aggregate_validate(session, ta, NULL, false)) != 0)
        return (expected_error ?
            WT_ERROR :
            __wt_panic(session, ret, "address timestamp window failed validation"));
#else
    WT_UNUSED(session);
    WT_UNUSED(ta);
    WT_UNUSED(expected_error);
#endif
    return (0);
}

/*
 * __cell_pack_addr_validity --
 *     Pack the validity window for an address.
 */
static inline void
__cell_pack_addr_validity(WT_SESSION_IMPL *session, uint8_t **pp, WT_TIME_AGGREGATE *ta)
{
    uint8_t flags, *flagsp;

    /* Globally visible values have no associated validity window. */
    if (WT_TIME_AGGREGATE_IS_EMPTY(ta)) {
        ++*pp;
        return;
    }

    WT_IGNORE_RET(__wt_check_addr_validity(session, ta, false));

    **pp |= WT_CELL_SECOND_DESC;
    ++*pp;
    flagsp = *pp;
    ++*pp;

    flags = 0;
    if (ta->oldest_start_ts != WT_TS_NONE) {
        WT_IGNORE_RET(__wt_vpack_uint(pp, 0, ta->oldest_start_ts));
        LF_SET(WT_CELL_TS_START);
    }
    if (ta->newest_txn != WT_TXN_NONE) {
        WT_IGNORE_RET(__wt_vpack_uint(pp, 0, ta->newest_txn));
        LF_SET(WT_CELL_TXN_START);
    }
    if (ta->newest_start_durable_ts != WT_TS_NONE) {
        /* Store differences, not absolutes. */
        WT_ASSERT(session, ta->oldest_start_ts <= ta->newest_start_durable_ts);

        /*
         * Unlike value cell, we store the durable start timestamp even the difference is zero
         * compared to oldest commit timestamp. The difference can only be zero when the page
         * contains all the key/value pairs with the same timestamp. But this scenario is rare and
         * having that check to find out whether it is zero or not will unnecessarily add overhead
         * than benefit.
         */
        WT_IGNORE_RET(__wt_vpack_uint(pp, 0, ta->newest_start_durable_ts - ta->oldest_start_ts));
        LF_SET(WT_CELL_TS_DURABLE_START);
    }
    if (ta->newest_stop_ts != WT_TS_MAX) {
        /* Store differences, not absolutes. */
        WT_IGNORE_RET(__wt_vpack_uint(pp, 0, ta->newest_stop_ts - ta->oldest_start_ts));
        LF_SET(WT_CELL_TS_STOP);
    }
    if (ta->newest_stop_txn != WT_TXN_MAX) {
        /* Store differences, not absolutes. */
        WT_IGNORE_RET(__wt_vpack_uint(pp, 0, ta->newest_stop_txn - ta->newest_txn));
        LF_SET(WT_CELL_TXN_STOP);
    }
    if (ta->newest_stop_durable_ts != WT_TS_NONE) {
        WT_ASSERT(session,
          ta->newest_stop_ts == WT_TS_MAX || ta->newest_stop_ts <= ta->newest_stop_durable_ts);

        /*
         * Store differences, not absolutes.
         *
         * Unlike value cell, we store the durable stop timestamp even the difference is zero
         * compared to newest commit timestamp. The difference can only be zero when the page
         * contains all the key/value pairs with the same timestamp. But this scenario is rare and
         * having that check to find out whether it is zero or not will unnecessarily add overhead
         * than benefit.
         */
        WT_IGNORE_RET(__wt_vpack_uint(pp, 0, ta->newest_stop_durable_ts - ta->newest_stop_ts));
        LF_SET(WT_CELL_TS_DURABLE_STOP);
    }
    if (ta->prepare)
        LF_SET(WT_CELL_PREPARE);

    *flagsp = flags;
}

/*
 * __wt_cell_pack_addr --
 *     Pack an address cell.
 */
static inline size_t
__wt_cell_pack_addr(WT_SESSION_IMPL *session, WT_CELL *cell, u_int cell_type, uint64_t recno,
  WT_TIME_AGGREGATE *ta, size_t size)
{
    uint8_t *p;

    /* Start building a cell: the descriptor byte starts zero. */
    p = cell->__chunk;
    *p = '\0';

    __cell_pack_addr_validity(session, &p, ta);

    if (recno == WT_RECNO_OOB)
        cell->__chunk[0] |= (uint8_t)cell_type; /* Type */
    else {
        cell->__chunk[0] |= (uint8_t)(cell_type | WT_CELL_64V);
        /* Record number */
        WT_IGNORE_RET(__wt_vpack_uint(&p, 0, recno));
    }
    /* Length */
    WT_IGNORE_RET(__wt_vpack_uint(&p, 0, (uint64_t)size));
    return (WT_PTRDIFF(p, cell));
}

/*
 * __wt_cell_pack_value --
 *     Set a value item's WT_CELL contents.
 */
static inline size_t
__wt_cell_pack_value(
  WT_SESSION_IMPL *session, WT_CELL *cell, WT_TIME_WINDOW *tw, uint64_t rle, size_t size)
{
    uint8_t byte, *p;
    bool validity;

    /* Start building a cell: the descriptor byte starts zero. */
    p = cell->__chunk;
    *p = '\0';

    __cell_pack_value_validity(session, &p, tw);

    /*
     * Short data cells without a validity window or run-length encoding have 6 bits of data length
     * in the descriptor byte.
     */
    validity = (cell->__chunk[0] & WT_CELL_SECOND_DESC) != 0;
    if (!validity && rle < 2 && size <= WT_CELL_SHORT_MAX) {
        byte = (uint8_t)size; /* Type + length */
        cell->__chunk[0] = (uint8_t)((byte << WT_CELL_SHORT_SHIFT) | WT_CELL_VALUE_SHORT);
    } else {
        /*
         * If the size was what prevented us from using a short cell, it's larger than the
         * adjustment size. Decrement/increment it when packing/unpacking so it takes up less room.
         */
        if (!validity && rle < 2) {
            size -= WT_CELL_SIZE_ADJUST;
            cell->__chunk[0] |= WT_CELL_VALUE; /* Type */
        } else {
            cell->__chunk[0] |= WT_CELL_VALUE | WT_CELL_64V;
            /* RLE */
            WT_IGNORE_RET(__wt_vpack_uint(&p, 0, rle));
        }
        /* Length */
        WT_IGNORE_RET(__wt_vpack_uint(&p, 0, (uint64_t)size));
    }
    return (WT_PTRDIFF(p, cell));
}

/*
 * __wt_cell_pack_value_match --
 *     Return if two value items would have identical WT_CELLs (except for their validity window and
 *     any RLE).
 */
static inline int
__wt_cell_pack_value_match(
  WT_CELL *page_cell, WT_CELL *val_cell, const uint8_t *val_data, bool *matchp)
{
    uint64_t alen, blen, v;
    uint8_t flags;
    const uint8_t *a, *b;
    bool rle, validity;

    *matchp = false; /* Default to no-match */

    /*
     * This is a special-purpose function used by reconciliation to support dictionary lookups.
     * We're passed an on-page cell and a created cell plus a chunk of data we're about to write on
     * the page, and we return if they would match on the page. Ignore the validity window and the
     * column-store RLE because the copied cell will have its own.
     */
    a = (uint8_t *)page_cell;
    b = (uint8_t *)val_cell;

    if (WT_CELL_SHORT_TYPE(a[0]) == WT_CELL_VALUE_SHORT) {
        alen = a[0] >> WT_CELL_SHORT_SHIFT;
        ++a;
    } else if (WT_CELL_TYPE(a[0]) == WT_CELL_VALUE) {
        rle = (a[0] & WT_CELL_64V) != 0;
        validity = (a[0] & WT_CELL_SECOND_DESC) != 0;
        ++a;
        if (validity) { /* Skip validity window */
            flags = *a;
            ++a;
            if (LF_ISSET(WT_CELL_TS_DURABLE_START))
                WT_RET(__wt_vunpack_uint(&a, 0, &v));
            if (LF_ISSET(WT_CELL_TS_DURABLE_STOP))
                WT_RET(__wt_vunpack_uint(&a, 0, &v));
            if (LF_ISSET(WT_CELL_TS_START))
                WT_RET(__wt_vunpack_uint(&a, 0, &v));
            if (LF_ISSET(WT_CELL_TS_STOP))
                WT_RET(__wt_vunpack_uint(&a, 0, &v));
            if (LF_ISSET(WT_CELL_TXN_START))
                WT_RET(__wt_vunpack_uint(&a, 0, &v));
            if (LF_ISSET(WT_CELL_TXN_STOP))
                WT_RET(__wt_vunpack_uint(&a, 0, &v));
        }
        if (rle) /* Skip RLE */
            WT_RET(__wt_vunpack_uint(&a, 0, &v));
        WT_RET(__wt_vunpack_uint(&a, 0, &alen)); /* Length */
    } else
        return (0);

    if (WT_CELL_SHORT_TYPE(b[0]) == WT_CELL_VALUE_SHORT) {
        blen = b[0] >> WT_CELL_SHORT_SHIFT;
        ++b;
    } else if (WT_CELL_TYPE(b[0]) == WT_CELL_VALUE) {
        rle = (b[0] & WT_CELL_64V) != 0;
        validity = (b[0] & WT_CELL_SECOND_DESC) != 0;
        ++b;
        if (validity) { /* Skip validity window */
            flags = *b;
            ++b;
            if (LF_ISSET(WT_CELL_TS_DURABLE_START))
                WT_RET(__wt_vunpack_uint(&b, 0, &v));
            if (LF_ISSET(WT_CELL_TS_DURABLE_STOP))
                WT_RET(__wt_vunpack_uint(&b, 0, &v));
            if (LF_ISSET(WT_CELL_TS_START))
                WT_RET(__wt_vunpack_uint(&b, 0, &v));
            if (LF_ISSET(WT_CELL_TS_STOP))
                WT_RET(__wt_vunpack_uint(&b, 0, &v));
            if (LF_ISSET(WT_CELL_TXN_START))
                WT_RET(__wt_vunpack_uint(&b, 0, &v));
            if (LF_ISSET(WT_CELL_TXN_STOP))
                WT_RET(__wt_vunpack_uint(&b, 0, &v));
        }
        if (rle) /* Skip RLE */
            WT_RET(__wt_vunpack_uint(&b, 0, &v));
        WT_RET(__wt_vunpack_uint(&b, 0, &blen)); /* Length */
    } else
        return (0);

    if (alen == blen)
        *matchp = memcmp(a, val_data, alen) == 0;
    return (0);
}

/*
 * __wt_cell_pack_copy --
 *     Write a copy value cell.
 */
static inline size_t
__wt_cell_pack_copy(
  WT_SESSION_IMPL *session, WT_CELL *cell, WT_TIME_WINDOW *tw, uint64_t rle, uint64_t v)
{
    uint8_t *p;

    /* Start building a cell: the descriptor byte starts zero. */
    p = cell->__chunk;
    *p = '\0';

    __cell_pack_value_validity(session, &p, tw);

    if (rle < 2)
        cell->__chunk[0] |= WT_CELL_VALUE_COPY; /* Type */
    else {
        cell->__chunk[0] |= /* Type */
          WT_CELL_VALUE_COPY | WT_CELL_64V;
        /* RLE */
        WT_IGNORE_RET(__wt_vpack_uint(&p, 0, rle));
    }
    /* Copy offset */
    WT_IGNORE_RET(__wt_vpack_uint(&p, 0, v));
    return (WT_PTRDIFF(p, cell));
}

/*
 * __wt_cell_pack_del --
 *     Write a deleted value cell.
 */
static inline size_t
__wt_cell_pack_del(WT_SESSION_IMPL *session, WT_CELL *cell, WT_TIME_WINDOW *tw, uint64_t rle)
{
    uint8_t *p;

    /* Start building a cell: the descriptor byte starts zero. */
    p = cell->__chunk;
    *p = '\0';

    __cell_pack_value_validity(session, &p, tw);

    if (rle < 2)
        cell->__chunk[0] |= WT_CELL_DEL; /* Type */
    else {
        /* Type */
        cell->__chunk[0] |= WT_CELL_DEL | WT_CELL_64V;
        /* RLE */
        WT_IGNORE_RET(__wt_vpack_uint(&p, 0, rle));
    }
    return (WT_PTRDIFF(p, cell));
}

/*
 * __wt_cell_pack_int_key --
 *     Set a row-store internal page key's WT_CELL contents.
 */
static inline size_t
__wt_cell_pack_int_key(WT_CELL *cell, size_t size)
{
    uint8_t byte, *p;

    /* Short keys have 6 bits of data length in the descriptor byte. */
    if (size <= WT_CELL_SHORT_MAX) {
        byte = (uint8_t)size;
        cell->__chunk[0] = (uint8_t)((byte << WT_CELL_SHORT_SHIFT) | WT_CELL_KEY_SHORT);
        return (1);
    }

    cell->__chunk[0] = WT_CELL_KEY; /* Type */
    p = cell->__chunk + 1;

    /*
     * If the size prevented us from using a short cell, it's larger than the adjustment size.
     * Decrement/increment it when packing/unpacking so it takes up less room.
     */
    size -= WT_CELL_SIZE_ADJUST; /* Length */
    WT_IGNORE_RET(__wt_vpack_uint(&p, 0, (uint64_t)size));
    return (WT_PTRDIFF(p, cell));
}

/*
 * __wt_cell_pack_leaf_key --
 *     Set a row-store leaf page key's WT_CELL contents.
 */
static inline size_t
__wt_cell_pack_leaf_key(WT_CELL *cell, uint8_t prefix, size_t size)
{
    uint8_t byte, *p;

    /* Short keys have 6 bits of data length in the descriptor byte. */
    if (size <= WT_CELL_SHORT_MAX) {
        if (prefix == 0) {
            byte = (uint8_t)size; /* Type + length */
            cell->__chunk[0] = (uint8_t)((byte << WT_CELL_SHORT_SHIFT) | WT_CELL_KEY_SHORT);
            return (1);
        }
        byte = (uint8_t)size; /* Type + length */
        cell->__chunk[0] = (uint8_t)((byte << WT_CELL_SHORT_SHIFT) | WT_CELL_KEY_SHORT_PFX);
        cell->__chunk[1] = prefix; /* Prefix */
        return (2);
    }

    if (prefix == 0) {
        cell->__chunk[0] = WT_CELL_KEY; /* Type */
        p = cell->__chunk + 1;
    } else {
        cell->__chunk[0] = WT_CELL_KEY_PFX; /* Type */
        cell->__chunk[1] = prefix;          /* Prefix */
        p = cell->__chunk + 2;
    }

    /*
     * If the size prevented us from using a short cell, it's larger than the adjustment size.
     * Decrement/increment it when packing/unpacking so it takes up less room.
     */
    size -= WT_CELL_SIZE_ADJUST; /* Length */
    WT_IGNORE_RET(__wt_vpack_uint(&p, 0, (uint64_t)size));
    return (WT_PTRDIFF(p, cell));
}

/*
 * __wt_cell_pack_ovfl --
 *     Pack an overflow cell.
 */
static inline size_t
__wt_cell_pack_ovfl(WT_SESSION_IMPL *session, WT_CELL *cell, uint8_t type, WT_TIME_WINDOW *tw,
  uint64_t rle, size_t size)
{
    uint8_t *p;

    /* Start building a cell: the descriptor byte starts zero. */
    p = cell->__chunk;
    *p = '\0';

    switch (type) {
    case WT_CELL_KEY_OVFL:
    case WT_CELL_KEY_OVFL_RM:
        WT_ASSERT(session, tw == NULL);
        ++p;
        break;
    case WT_CELL_VALUE_OVFL:
    case WT_CELL_VALUE_OVFL_RM:
        __cell_pack_value_validity(session, &p, tw);
        break;
    }

    if (rle < 2)
        cell->__chunk[0] |= type; /* Type */
    else {
        cell->__chunk[0] |= type | WT_CELL_64V; /* Type */
                                                /* RLE */
        WT_IGNORE_RET(__wt_vpack_uint(&p, 0, rle));
    }
    /* Length */
    WT_IGNORE_RET(__wt_vpack_uint(&p, 0, (uint64_t)size));
    return (WT_PTRDIFF(p, cell));
}

/*
 * __wt_cell_rle --
 *     Return the cell's RLE value.
 */
static inline uint64_t
__wt_cell_rle(WT_CELL_UNPACK_KV *unpack)
{
    /*
     * Any item with only 1 occurrence is stored with an RLE of 0, that is, without any RLE at all.
     * This code is a single place to handle that correction, for simplicity.
     */
    return (unpack->v < 2 ? 1 : unpack->v);
}

/*
 * __wt_cell_total_len --
 *     Return the cell's total length, including data.
 */
static inline size_t
__wt_cell_total_len(void *unpack_arg)
{
    WT_CELL_UNPACK_COMMON *unpack;

    unpack = (WT_CELL_UNPACK_COMMON *)unpack_arg;

    /*
     * The length field is specially named because it's dangerous to use it: it represents the
     * length of the current cell (normally used for the loop that walks through cells on the page),
     * but occasionally we want to copy a cell directly from the page, and what we need is the
     * cell's total length. The problem is dictionary-copy cells, because in that case, the __len
     * field is the length of the current cell, not the cell for which we're returning data. To use
     * the __len field, you must be sure you're not looking at a copy cell.
     */
    return (unpack->__len);
}

/*
 * __wt_cell_type --
 *     Return the cell's type (collapsing special types).
 */
static inline u_int
__wt_cell_type(WT_CELL *cell)
{
    u_int type;

    switch (WT_CELL_SHORT_TYPE(cell->__chunk[0])) {
    case WT_CELL_KEY_SHORT:
    case WT_CELL_KEY_SHORT_PFX:
        return (WT_CELL_KEY);
    case WT_CELL_VALUE_SHORT:
        return (WT_CELL_VALUE);
    }

    switch (type = WT_CELL_TYPE(cell->__chunk[0])) {
    case WT_CELL_KEY_PFX:
        return (WT_CELL_KEY);
    case WT_CELL_KEY_OVFL_RM:
        return (WT_CELL_KEY_OVFL);
    case WT_CELL_VALUE_OVFL_RM:
        return (WT_CELL_VALUE_OVFL);
    }
    return (type);
}

/*
 * __wt_cell_type_raw --
 *     Return the cell's type.
 */
static inline u_int
__wt_cell_type_raw(WT_CELL *cell)
{
    return (WT_CELL_SHORT_TYPE(cell->__chunk[0]) == 0 ? WT_CELL_TYPE(cell->__chunk[0]) :
                                                        WT_CELL_SHORT_TYPE(cell->__chunk[0]));
}

/*
 * __wt_cell_type_reset --
 *     Reset the cell's type.
 */
static inline void
__wt_cell_type_reset(WT_SESSION_IMPL *session, WT_CELL *cell, u_int old_type, u_int new_type)
{
    /*
     * For all current callers of this function, this should happen once and only once, assert we're
     * setting what we think we're setting.
     */
    WT_ASSERT(session, old_type == 0 || old_type == __wt_cell_type(cell));
    WT_UNUSED(old_type);

    cell->__chunk[0] = (cell->__chunk[0] & ~WT_CELL_TYPE_MASK) | WT_CELL_TYPE(new_type);
}

/*
 * __wt_cell_leaf_value_parse --
 *     Return the cell if it's a row-store leaf page value, otherwise return NULL.
 */
static inline WT_CELL *
__wt_cell_leaf_value_parse(WT_PAGE *page, WT_CELL *cell)
{
    /*
     * This function exists so there's a place for this comment.
     *
     * Row-store leaf pages may have a single data cell between each key, or
     * keys may be adjacent (when the data cell is empty).
     *
     * One special case: if the last key on a page is a key without a value,
     * don't walk off the end of the page: the size of the underlying disk
     * image is exact, which means the end of the last cell on the page plus
     * the length of the cell should be the byte immediately after the page
     * disk image.
     *
     * !!!
     * This line of code is really a call to __wt_off_page, but we know the
     * cell we're given will either be on the page or past the end of page,
     * so it's a simpler check.  (I wouldn't bother, but the real problem is
     * we can't call __wt_off_page directly, it's in btree_inline.h which requires
     * this file be included first.)
     */
    if (cell >= (WT_CELL *)((uint8_t *)page->dsk + page->dsk->mem_size))
        return (NULL);

    switch (__wt_cell_type_raw(cell)) {
    case WT_CELL_KEY:
    case WT_CELL_KEY_OVFL:
    case WT_CELL_KEY_OVFL_RM:
    case WT_CELL_KEY_PFX:
    case WT_CELL_KEY_SHORT:
    case WT_CELL_KEY_SHORT_PFX:
        return (NULL);
    default:
        return (cell);
    }
}

/*
 * The verification code specifies an end argument, a pointer to 1B past the end-of-page. In which
 * case, make sure all reads are inside the page image. If an error occurs, return an error code but
 * don't output messages, our caller handles that.
 */
#define WT_CELL_LEN_CHK(t, len)                                                             \
    do {                                                                                    \
        if (end != NULL &&                                                                  \
          ((uint8_t *)(t) < (uint8_t *)dsk || (((uint8_t *)(t)) + (len)) > (uint8_t *)end)) \
            return (WT_ERROR);                                                              \
    } while (0)

/*
 * __wt_cell_unpack_safe --
 *     Unpack a WT_CELL into a structure, with optional boundary checks.
 */
static inline int
__wt_cell_unpack_safe(WT_SESSION_IMPL *session, const WT_PAGE_HEADER *dsk, WT_CELL *cell,
  WT_CELL_UNPACK_ADDR *unpack_addr, WT_CELL_UNPACK_KV *unpack_value, const void *end)
{
    struct {
        uint64_t v;
        uint32_t len;
        WT_TIME_WINDOW tw;
    } copy;
    WT_CELL_UNPACK_COMMON *unpack;
    WT_TIME_AGGREGATE *ta;
    WT_TIME_WINDOW *tw;
    uint64_t v;
    const uint8_t *p;
    uint8_t flags;
    bool copy_cell;

    copy_cell = false;
    copy.len = 0; /* [-Wconditional-uninitialized] */
    copy.v = 0;   /* [-Wconditional-uninitialized] */

    if (unpack_addr == NULL) {
        unpack = (WT_CELL_UNPACK_COMMON *)unpack_value;
        tw = &unpack_value->tw;
        WT_TIME_WINDOW_INIT(tw);
        ta = NULL;
    } else {
        WT_ASSERT(session, unpack_value == NULL);

        unpack = (WT_CELL_UNPACK_COMMON *)unpack_addr;
        ta = &unpack_addr->ta;
        WT_TIME_AGGREGATE_INIT(ta);
        tw = NULL;
    }

    /*
     * NB: when unpacking a WT_CELL_VALUE_COPY cell, unpack.cell is returned as the original cell,
     * not the copied cell (in other words, data from the copied cell must be available from unpack
     * after we return, as our caller has no way to find the copied cell).
     */
    unpack->cell = cell;

copy_cell_restart:
    WT_CELL_LEN_CHK(cell, 0);

    /*
     * This path is performance critical for read-only trees, we're parsing on-page structures. For
     * that reason we don't clear the unpacked cell structure (although that would be simpler),
     * instead we make sure we initialize all structure elements either here or in the immediately
     * following switch. All validity windows default to durability.
     */
    unpack->v = 0;
    unpack->raw = (uint8_t)__wt_cell_type_raw(cell);
    unpack->type = (uint8_t)__wt_cell_type(cell);
    unpack->flags = 0;

    /*
     * Handle cells with none of RLE counts, validity window or data length: WT_CELL_KEY_SHORT_PFX,
     * WT_CELL_KEY_SHORT and WT_CELL_VALUE_SHORT. Short key/data cells have 6 bits of data length in
     * the descriptor byte and nothing else
     */
    switch (unpack->raw) {
    case WT_CELL_KEY_SHORT_PFX:
        WT_CELL_LEN_CHK(cell, 1); /* skip prefix */
        unpack->prefix = cell->__chunk[1];
        unpack->data = cell->__chunk + 2;
        unpack->size = cell->__chunk[0] >> WT_CELL_SHORT_SHIFT;
        unpack->__len = 2 + unpack->size;
        goto done; /* Handle copy cells. */
    case WT_CELL_KEY_SHORT:
    case WT_CELL_VALUE_SHORT:
        unpack->prefix = 0;
        unpack->data = cell->__chunk + 1;
        unpack->size = cell->__chunk[0] >> WT_CELL_SHORT_SHIFT;
        unpack->__len = 1 + unpack->size;
        goto done; /* Handle copy cells. */
    }

    unpack->prefix = 0;
    unpack->data = NULL;
    unpack->size = 0;
    unpack->__len = 0;

    p = (uint8_t *)cell + 1; /* skip cell */

    /*
     * Check for a prefix byte that optionally follows the cell descriptor byte in keys on row-store
     * leaf pages.
     */
    if (unpack->raw == WT_CELL_KEY_PFX) {
        unpack->prefix = *p++; /* skip prefix */
        WT_CELL_LEN_CHK(p, 0);
    }

    /* Check for a validity window. */
    switch (unpack->raw) {
    case WT_CELL_ADDR_DEL:
    case WT_CELL_ADDR_INT:
    case WT_CELL_ADDR_LEAF:
    case WT_CELL_ADDR_LEAF_NO:
        /* Return an error if we're not unpacking a cell of this type. */
        if (unpack_addr == NULL)
            return (WT_ERROR);

        if ((cell->__chunk[0] & WT_CELL_SECOND_DESC) == 0)
            break;
        flags = *p++; /* skip second descriptor byte */

        if (LF_ISSET(WT_CELL_PREPARE))
            ta->prepare = 1;
        if (LF_ISSET(WT_CELL_TS_START))
            WT_RET(
              __wt_vunpack_uint(&p, end == NULL ? 0 : WT_PTRDIFF(end, p), &ta->oldest_start_ts));
        if (LF_ISSET(WT_CELL_TXN_START))
            WT_RET(__wt_vunpack_uint(&p, end == NULL ? 0 : WT_PTRDIFF(end, p), &ta->newest_txn));
        if (LF_ISSET(WT_CELL_TS_DURABLE_START)) {
            WT_RET(__wt_vunpack_uint(
              &p, end == NULL ? 0 : WT_PTRDIFF(end, p), &ta->newest_start_durable_ts));
            ta->newest_start_durable_ts += ta->oldest_start_ts;
        }

        if (LF_ISSET(WT_CELL_TS_STOP)) {
            WT_RET(
              __wt_vunpack_uint(&p, end == NULL ? 0 : WT_PTRDIFF(end, p), &ta->newest_stop_ts));
            ta->newest_stop_ts += ta->oldest_start_ts;
        }
        if (LF_ISSET(WT_CELL_TXN_STOP)) {
            WT_RET(
              __wt_vunpack_uint(&p, end == NULL ? 0 : WT_PTRDIFF(end, p), &ta->newest_stop_txn));
            ta->newest_stop_txn += ta->newest_txn;
        }
        if (LF_ISSET(WT_CELL_TS_DURABLE_STOP)) {
            WT_RET(__wt_vunpack_uint(
              &p, end == NULL ? 0 : WT_PTRDIFF(end, p), &ta->newest_stop_durable_ts));
            ta->newest_stop_durable_ts += ta->newest_stop_ts;
        }
        WT_RET(__wt_check_addr_validity(session, ta, end != NULL));
        break;
    case WT_CELL_DEL:
    case WT_CELL_VALUE:
    case WT_CELL_VALUE_COPY:
    case WT_CELL_VALUE_OVFL:
    case WT_CELL_VALUE_OVFL_RM:
        /* Return an error if we're not unpacking a cell of this type. */
        if (unpack_value == NULL)
            return (WT_ERROR);

        if ((cell->__chunk[0] & WT_CELL_SECOND_DESC) == 0)
            break;
        flags = *p++; /* skip second descriptor byte */

        if (LF_ISSET(WT_CELL_PREPARE))
            tw->prepare = 1;
        if (LF_ISSET(WT_CELL_TS_START))
            WT_RET(__wt_vunpack_uint(&p, end == NULL ? 0 : WT_PTRDIFF(end, p), &tw->start_ts));
        if (LF_ISSET(WT_CELL_TXN_START))
            WT_RET(__wt_vunpack_uint(&p, end == NULL ? 0 : WT_PTRDIFF(end, p), &tw->start_txn));
        if (LF_ISSET(WT_CELL_TS_DURABLE_START)) {
            WT_RET(
              __wt_vunpack_uint(&p, end == NULL ? 0 : WT_PTRDIFF(end, p), &tw->durable_start_ts));
            tw->durable_start_ts += tw->start_ts;
        } else
            tw->durable_start_ts = tw->start_ts;

        if (LF_ISSET(WT_CELL_TS_STOP)) {
            WT_RET(__wt_vunpack_uint(&p, end == NULL ? 0 : WT_PTRDIFF(end, p), &tw->stop_ts));
            tw->stop_ts += tw->start_ts;
        }
        if (LF_ISSET(WT_CELL_TXN_STOP)) {
            WT_RET(__wt_vunpack_uint(&p, end == NULL ? 0 : WT_PTRDIFF(end, p), &tw->stop_txn));
            tw->stop_txn += tw->start_txn;
        }
        if (LF_ISSET(WT_CELL_TS_DURABLE_STOP)) {
            WT_RET(
              __wt_vunpack_uint(&p, end == NULL ? 0 : WT_PTRDIFF(end, p), &tw->durable_stop_ts));
            tw->durable_stop_ts += tw->stop_ts;
        } else if (tw->stop_ts != WT_TS_MAX)
            tw->durable_stop_ts = tw->stop_ts;
        else
            tw->durable_stop_ts = WT_TS_NONE;

        WT_RET(__cell_check_value_validity(session, tw, end != NULL));
        break;
    }

    /*
     * Check for an RLE count or record number that optionally follows the cell descriptor byte on
     * column-store variable-length pages.
     */
    if (cell->__chunk[0] & WT_CELL_64V) /* skip value */
        WT_RET(__wt_vunpack_uint(&p, end == NULL ? 0 : WT_PTRDIFF(end, p), &unpack->v));

    /*
     * Handle special actions for a few different cell types and set the data length (deleted cells
     * are fixed-size without length bytes, almost everything else has data length bytes).
     */
    switch (unpack->raw) {
    case WT_CELL_VALUE_COPY:
        /* Return an error if we're not unpacking a cell of this type. */
        if (unpack_value == NULL)
            return (WT_ERROR);

        copy_cell = true;

        /*
         * The cell is followed by an offset to a cell written earlier in the page. Save/restore the
         * visibility window, length and RLE of this cell, we need the length to step through the
         * set of cells on the page and the RLE and timestamp information are specific to this cell.
         */
        WT_RET(__wt_vunpack_uint(&p, end == NULL ? 0 : WT_PTRDIFF(end, p), &v));
        copy.v = unpack->v;
        copy.len = WT_PTRDIFF32(p, cell);
        tw = &copy.tw;
        WT_TIME_WINDOW_INIT(tw);
        cell = (WT_CELL *)((uint8_t *)cell - v);
        goto copy_cell_restart;

    case WT_CELL_KEY_OVFL:
    case WT_CELL_KEY_OVFL_RM:
    case WT_CELL_VALUE_OVFL:
    case WT_CELL_VALUE_OVFL_RM:
        /*
         * Set overflow flag.
         */
        F_SET(unpack, WT_CELL_UNPACK_OVERFLOW);
        /* FALLTHROUGH */

    case WT_CELL_ADDR_DEL:
    case WT_CELL_ADDR_INT:
    case WT_CELL_ADDR_LEAF:
    case WT_CELL_ADDR_LEAF_NO:
    case WT_CELL_KEY:
    case WT_CELL_KEY_PFX:
    case WT_CELL_VALUE:
        /*
         * The cell is followed by a 4B data length and a chunk of data.
         */
        WT_RET(__wt_vunpack_uint(&p, end == NULL ? 0 : WT_PTRDIFF(end, p), &v));

        /*
         * If the size was what prevented us from using a short cell, it's larger than the
         * adjustment size. Decrement/increment it when packing/unpacking so it takes up less room.
         */
        if (unpack->raw == WT_CELL_KEY || unpack->raw == WT_CELL_KEY_PFX ||
          (unpack->raw == WT_CELL_VALUE && unpack->v == 0 &&
            (cell->__chunk[0] & WT_CELL_SECOND_DESC) == 0))
            v += WT_CELL_SIZE_ADJUST;

        unpack->data = p;
        unpack->size = (uint32_t)v;
        unpack->__len = WT_PTRDIFF32(p, cell) + unpack->size;
        break;

    case WT_CELL_DEL:
        unpack->__len = WT_PTRDIFF32(p, cell);
        break;
    default:
        return (WT_ERROR); /* Unknown cell type. */
    }

done:
    /*
     * Skip if we know we're not unpacking a cell of this type. This is all inlined code, and
     * ideally checking allows the compiler to discard big chunks of it.
     */
    if (unpack_addr == NULL && copy_cell) {
        unpack->v = copy.v;
        unpack->__len = copy.len;
        unpack->raw = WT_CELL_VALUE_COPY;
    }

    /*
     * Check the original cell against the full cell length (this is a diagnostic as well, we may be
     * copying the cell from the page and we need the right length).
     */
    WT_CELL_LEN_CHK(cell, unpack->__len);
    return (0);
}

/*
 * __cell_addr_window_cleanup --
 *     Clean up addr cells loaded from a previous run.
 */
static inline void
__cell_addr_window_cleanup(WT_SESSION_IMPL *session, WT_CELL_UNPACK_ADDR *unpack_addr)
{
    WT_TIME_AGGREGATE *ta;

    /* Tell reconciliation we cleared the transaction ids and the cell needs to be rebuilt. */
    if (unpack_addr != NULL) {
        ta = &unpack_addr->ta;
        if (ta->newest_txn != WT_TXN_NONE) {
            ta->newest_txn = WT_TXN_NONE;
            F_SET(unpack_addr, WT_CELL_UNPACK_TIME_WINDOW_CLEARED);
        }
        if (ta->newest_stop_txn != WT_TXN_MAX) {
            ta->newest_stop_txn = WT_TXN_NONE;
            F_SET(unpack_addr, WT_CELL_UNPACK_TIME_WINDOW_CLEARED);

            /*
             * The combination of newest stop timestamp being WT_TS_MAX while the newest stop
             * transaction not being WT_TXN_MAX is possible only for the non-timestamped tables. In
             * this scenario there shouldn't be any timestamp value as part of durable stop
             * timestamp other than the default value WT_TS_NONE.
             */
            if (ta->newest_stop_ts == WT_TS_MAX) {
                ta->newest_stop_ts = WT_TS_NONE;
                WT_ASSERT(session, ta->newest_stop_durable_ts == WT_TS_NONE);
            }
        } else
            WT_ASSERT(session, ta->newest_stop_ts == WT_TS_MAX);
    }
}

/*
 * __cell_kv_window_cleanup --
 *     Clean up kv cells loaded from a previous run.
 */
static inline void
__cell_kv_window_cleanup(WT_SESSION_IMPL *session, WT_CELL_UNPACK_KV *unpack_kv)
{
    WT_TIME_WINDOW *tw;

    if (unpack_kv != NULL) {
        tw = &unpack_kv->tw;
        if (tw->start_txn != WT_TXN_NONE) {
            tw->start_txn = WT_TXN_NONE;
            F_SET(unpack_kv, WT_CELL_UNPACK_TIME_WINDOW_CLEARED);
        }
        if (tw->stop_txn != WT_TXN_MAX) {
            tw->stop_txn = WT_TXN_NONE;
            F_SET(unpack_kv, WT_CELL_UNPACK_TIME_WINDOW_CLEARED);

            /*
             * The combination of stop timestamp being WT_TS_MAX while the stop transaction not
             * being WT_TXN_MAX is possible only for the non-timestamped tables. In this scenario
             * there shouldn't be any timestamp value as part of durable stop timestamp other than
             * the default value WT_TS_NONE.
             */
            if (tw->stop_ts == WT_TS_MAX) {
                tw->stop_ts = WT_TS_NONE;
                WT_ASSERT(session, tw->durable_stop_ts == WT_TS_NONE);
            }
        } else
            WT_ASSERT(session, tw->stop_ts == WT_TS_MAX);
    }
}

/*
 * __cell_unpack_window_cleanup --
 *     Clean up cells loaded from a previous run.
 */
static inline void
__cell_unpack_window_cleanup(WT_SESSION_IMPL *session, const WT_PAGE_HEADER *dsk,
  WT_CELL_UNPACK_ADDR *unpack_addr, WT_CELL_UNPACK_KV *unpack_kv)
{
    /*
     * If the page came from a previous run, reset the transaction ids to "none" and timestamps to 0
     * as appropriate. Transaction ids shouldn't persist between runs so these are always set to
     * "none". Timestamps should persist between runs however, the absence of a timestamp (in the
     * case of a non-timestamped write) should default to WT_TS_NONE rather than "max" as usual.
     *
     * Note that it is still necessary to unpack each value above even if we end up overwriting them
     * since values in a cell need to be unpacked sequentially.
     *
     * This is how the stop time point should be interpreted for each type of delete:
     * -
     *                        Current startup               Previous startup
     * Timestamp delete       txnid=x, ts=y,                txnid=0, ts=y,
     *                        durable_ts=z                  durable_ts=z
     * Non-timestamp delete   txnid=x, ts=NONE,             txnid=0, ts=NONE,
     *                        durable_ts=NONE               durable_ts=NONE
     * No delete              txnid=MAX, ts=MAX,            txnid=MAX, ts=MAX,
     *                        durable_ts=NONE               durable_ts=NONE
     */
    WT_ASSERT(session, dsk->write_gen != 0);
    if (dsk->write_gen > S2BT(session)->base_write_gen)
        return;

    __cell_addr_window_cleanup(session, unpack_addr);
    __cell_kv_window_cleanup(session, unpack_kv);
}

/*
 * __wt_cell_unpack_addr --
 *     Unpack an address WT_CELL into a structure.
 */
static inline void
__wt_cell_unpack_addr(WT_SESSION_IMPL *session, const WT_PAGE_HEADER *dsk, WT_CELL *cell,
  WT_CELL_UNPACK_ADDR *unpack_addr)
{
    WT_IGNORE_RET(__wt_cell_unpack_safe(session, dsk, cell, unpack_addr, NULL, NULL));
    __cell_unpack_window_cleanup(session, dsk, unpack_addr, NULL);
}

/*
 * __wt_cell_unpack_kv --
 *     Unpack a value WT_CELL into a structure.
 */
static inline void
__wt_cell_unpack_kv(WT_SESSION_IMPL *session, const WT_PAGE_HEADER *dsk, WT_CELL *cell,
  WT_CELL_UNPACK_KV *unpack_value)
{
    /*
     * Row-store doesn't store zero-length values on pages, but this allows us to pretend.
     */
    if (cell == NULL) {
        unpack_value->cell = NULL;
        unpack_value->v = 0;
        unpack_value->data = "";
        unpack_value->size = 0;
        unpack_value->__len = 0;
        unpack_value->prefix = 0;
        unpack_value->raw = unpack_value->type = WT_CELL_VALUE;
        unpack_value->flags = 0;

        /*
         * If there isn't any value validity window (which is what it will take to get to a
         * zero-length item), the value must be stable.
         */
        WT_TIME_WINDOW_INIT(&unpack_value->tw);

        return;
    }

    WT_IGNORE_RET(__wt_cell_unpack_safe(session, dsk, cell, NULL, unpack_value, NULL));
    __cell_unpack_window_cleanup(session, dsk, NULL, unpack_value);
}

/*
 * __cell_data_ref --
 *     Set a buffer to reference the data from an unpacked cell.
 */
static inline int
__cell_data_ref(WT_SESSION_IMPL *session, WT_PAGE *page, int page_type,
  WT_CELL_UNPACK_COMMON *unpack, WT_ITEM *store)
{
    WT_BTREE *btree;
    bool decoded;
    void *huffman;

    btree = S2BT(session);
    huffman = NULL;

    /* Reference the cell's data, optionally decode it. */
    switch (unpack->type) {
    case WT_CELL_KEY:
        store->data = unpack->data;
        store->size = unpack->size;
        if (page_type == WT_PAGE_ROW_INT)
            return (0);
        break;
    case WT_CELL_VALUE:
        store->data = unpack->data;
        store->size = unpack->size;
        huffman = btree->huffman_value;
        break;
    case WT_CELL_KEY_OVFL:
        WT_RET(__wt_ovfl_read(session, page, unpack, store, &decoded));
        if (page_type == WT_PAGE_ROW_INT || decoded)
            return (0);
        break;
    case WT_CELL_VALUE_OVFL:
        WT_RET(__wt_ovfl_read(session, page, unpack, store, &decoded));
        if (decoded)
            return (0);
        huffman = btree->huffman_value;
        break;
    default:
        return (__wt_illegal_value(session, unpack->type));
    }

    return (huffman == NULL || store->size == 0 ?
        0 :
        __wt_huffman_decode(session, huffman, (const uint8_t *)store->data, store->size, store));
}

/*
 * __wt_dsk_cell_data_ref --
 *     Set a buffer to reference the data from an unpacked cell. There are two versions because of
 *     WT_CELL_VALUE_OVFL_RM type cells. When an overflow item is deleted, its backing blocks are
 *     removed; if there are still running transactions that might need to see the overflow item, we
 *     cache a copy of the item and reset the item's cell to WT_CELL_VALUE_OVFL_RM. If we find a
 *     WT_CELL_VALUE_OVFL_RM cell when reading an overflow item, we use the page reference to look
 *     aside into the cache. So, calling the "dsk" version of the function declares the cell cannot
 *     be of type WT_CELL_VALUE_OVFL_RM, and calling the "page" version means it might be.
 */
static inline int
__wt_dsk_cell_data_ref(WT_SESSION_IMPL *session, int page_type, void *unpack_arg, WT_ITEM *store)
{
    WT_CELL_UNPACK_COMMON *unpack;

    unpack = (WT_CELL_UNPACK_COMMON *)unpack_arg;

    WT_ASSERT(session, __wt_cell_type_raw(unpack->cell) != WT_CELL_VALUE_OVFL_RM);
    return (__cell_data_ref(session, NULL, page_type, unpack, store));
}

/*
 * __wt_page_cell_data_ref --
 *     Set a buffer to reference the data from an unpacked cell.
 */
static inline int
__wt_page_cell_data_ref(WT_SESSION_IMPL *session, WT_PAGE *page, void *unpack_arg, WT_ITEM *store)
{
    return (__cell_data_ref(session, page, page->type, (WT_CELL_UNPACK_COMMON *)unpack_arg, store));
}

/*
 * WT_CELL_FOREACH --
 *	Walk the cells on a page.
 */
#define WT_CELL_FOREACH_ADDR(session, dsk, unpack)                                              \
    do {                                                                                        \
        uint32_t __i;                                                                           \
        uint8_t *__cell;                                                                        \
        for (__cell = WT_PAGE_HEADER_BYTE(S2BT(session), dsk), __i = (dsk)->u.entries; __i > 0; \
             __cell += (unpack).__len, --__i) {                                                 \
            __wt_cell_unpack_addr(session, dsk, (WT_CELL *)__cell, &(unpack));

#define WT_CELL_FOREACH_KV(session, dsk, unpack)                                                \
    do {                                                                                        \
        uint32_t __i;                                                                           \
        uint8_t *__cell;                                                                        \
        for (__cell = WT_PAGE_HEADER_BYTE(S2BT(session), dsk), __i = (dsk)->u.entries; __i > 0; \
             __cell += (unpack).__len, --__i) {                                                 \
            __wt_cell_unpack_kv(session, dsk, (WT_CELL *)__cell, &(unpack));

#define WT_CELL_FOREACH_FIX_TIMESTAMPS(session, dsk, aux, unpack)                              \
    do {                                                                                       \
        uint32_t __i;                                                                          \
        uint8_t *__cell;                                                                       \
        for (__cell = (uint8_t *)(dsk) + (aux)->dataoffset, __i = (aux)->entries * 2; __i > 0; \
             __cell += (unpack).__len, --__i) {                                                \
            __wt_cell_unpack_kv(session, dsk, (WT_CELL *)__cell, &(unpack));

#define WT_CELL_FOREACH_END \
    }                       \
    }                       \
    while (0)
