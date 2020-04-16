/*-
 * Copyright (c) 2014-2020 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * __cell_check_value_validity --
 *     Check the value's validity window for sanity.
 */
static inline void
__cell_check_value_validity(WT_SESSION_IMPL *session, wt_timestamp_t durable_start_ts,
  wt_timestamp_t start_ts, uint64_t start_txn, wt_timestamp_t durable_stop_ts,
  wt_timestamp_t stop_ts, uint64_t stop_txn)
{
#ifdef HAVE_DIAGNOSTIC
    char ts_string[2][WT_TS_INT_STRING_SIZE];

    if (start_ts > durable_start_ts)
        WT_ERR_ASSERT(session, start_ts <= durable_start_ts, WT_PANIC,
          "a start timestamp %s newer than its durable start timestamp %s",
          __wt_timestamp_to_string(start_ts, ts_string[0]),
          __wt_timestamp_to_string(durable_start_ts, ts_string[1]));

    if (start_ts != WT_TS_NONE && stop_ts == WT_TS_NONE)
        WT_ERR_ASSERT(session, stop_ts != WT_TS_NONE, WT_PANIC, "stop timestamp of 0");

    if (start_ts > stop_ts)
        WT_ERR_ASSERT(session, start_ts <= stop_ts, WT_PANIC,
          "a start timestamp %s newer than its stop timestamp %s",
          __wt_timestamp_to_string(start_ts, ts_string[0]),
          __wt_timestamp_to_string(stop_ts, ts_string[1]));

    if (start_txn > stop_txn)
        WT_ERR_ASSERT(session, start_txn <= stop_txn, WT_PANIC,
          "a start transaction ID %" PRIu64 " newer than its stop transaction ID %" PRIu64,
          start_txn, stop_txn);

    if (stop_ts != WT_TS_MAX && stop_ts > durable_stop_ts)
        WT_ERR_ASSERT(session, stop_ts <= durable_stop_ts, WT_PANIC,
          "a stop timestamp %s newer than its durable stop timestamp %s",
          __wt_timestamp_to_string(stop_ts, ts_string[0]),
          __wt_timestamp_to_string(durable_stop_ts, ts_string[1]));

#else
    WT_UNUSED(session);
    WT_UNUSED(durable_start_ts);
    WT_UNUSED(durable_stop_ts);
    WT_UNUSED(start_ts);
    WT_UNUSED(start_txn);
    WT_UNUSED(stop_ts);
    WT_UNUSED(stop_txn);
#endif
}

/*
 * __cell_pack_value_validity --
 *     Pack the validity window for a value.
 */
static inline void
__cell_pack_value_validity(WT_SESSION_IMPL *session, uint8_t **pp, wt_timestamp_t durable_start_ts,
  wt_timestamp_t start_ts, uint64_t start_txn, wt_timestamp_t durable_stop_ts,
  wt_timestamp_t stop_ts, uint64_t stop_txn, bool prepare)
{
    uint8_t flags, *flagsp;

    /* Globally visible values have no associated validity window. */
    if (durable_start_ts == WT_TS_NONE && start_ts == WT_TS_NONE && start_txn == WT_TXN_NONE &&
      durable_stop_ts == WT_TS_NONE && stop_ts == WT_TS_MAX && stop_txn == WT_TXN_MAX) {
        ++*pp;
        return;
    }

    __cell_check_value_validity(
      session, durable_start_ts, start_ts, start_txn, durable_stop_ts, stop_ts, stop_txn);

    **pp |= WT_CELL_SECOND_DESC;
    ++*pp;
    flagsp = *pp;
    ++*pp;

    flags = 0;
    if (start_ts != WT_TS_NONE) {
        WT_IGNORE_RET(__wt_vpack_uint(pp, 0, start_ts));
        LF_SET(WT_CELL_TS_START);
    }
    if (start_txn != WT_TXN_NONE) {
        WT_IGNORE_RET(__wt_vpack_uint(pp, 0, start_txn));
        LF_SET(WT_CELL_TXN_START);
    }
    if (durable_start_ts != WT_TS_NONE) {
        WT_ASSERT(session, start_ts != WT_TS_NONE && start_ts <= durable_start_ts);
        /* Store differences if any, not absolutes. */
        if (durable_start_ts - start_ts > 0) {
            WT_IGNORE_RET(__wt_vpack_uint(pp, 0, durable_start_ts - start_ts));
            LF_SET(WT_CELL_TS_DURABLE_START);
        }
    }
    if (stop_ts != WT_TS_MAX) {
        /* Store differences, not absolutes. */
        WT_IGNORE_RET(__wt_vpack_uint(pp, 0, stop_ts - start_ts));
        LF_SET(WT_CELL_TS_STOP);
    }
    if (stop_txn != WT_TXN_MAX) {
        /* Store differences, not absolutes. */
        WT_IGNORE_RET(__wt_vpack_uint(pp, 0, stop_txn - start_txn));
        LF_SET(WT_CELL_TXN_STOP);
    }
    if (durable_stop_ts != WT_TS_NONE) {
        WT_ASSERT(session, stop_ts != WT_TS_MAX && stop_ts <= durable_stop_ts);
        /* Store differences if any, not absolutes. */
        if (durable_stop_ts - stop_ts > 0) {
            WT_IGNORE_RET(__wt_vpack_uint(pp, 0, durable_stop_ts - stop_ts));
            LF_SET(WT_CELL_TS_DURABLE_STOP);
        }
    }
    /*
     * Currently, no uncommitted prepared updates are written to the data store, so this flag must
     * be false until we allow writing them in WT-5984. In that ticket this assert must be removed.
     */
    WT_ASSERT(session, prepare == false);
    if (prepare)
        LF_SET(WT_CELL_PREPARE);
    *flagsp = flags;
}

/*
 * __wt_check_addr_validity --
 *     Check the address' validity window for sanity.
 */
static inline void
__wt_check_addr_validity(WT_SESSION_IMPL *session, wt_timestamp_t start_durable_ts,
  wt_timestamp_t oldest_start_ts, uint64_t oldest_start_txn, wt_timestamp_t stop_durable_ts,
  wt_timestamp_t newest_stop_ts, uint64_t newest_stop_txn)
{
#ifdef HAVE_DIAGNOSTIC
    char ts_string[2][WT_TS_INT_STRING_SIZE];

    if (oldest_start_ts != WT_TS_NONE && newest_stop_ts == WT_TS_NONE)
        WT_ERR_ASSERT(
          session, newest_stop_ts != WT_TS_NONE, WT_PANIC, "newest stop timestamp of 0");

    if (oldest_start_ts > newest_stop_ts)
        WT_ERR_ASSERT(session, oldest_start_ts <= newest_stop_ts, WT_PANIC,
          "an oldest start timestamp %s newer than its newest stop timestamp %s",
          __wt_timestamp_to_string(oldest_start_ts, ts_string[0]),
          __wt_timestamp_to_string(newest_stop_ts, ts_string[1]));

    if (oldest_start_txn > newest_stop_txn)
        WT_ERR_ASSERT(session, oldest_start_txn <= newest_stop_txn, WT_PANIC,
          "an oldest start transaction %" PRIu64 " newer than its newest stop transaction %" PRIu64,
          oldest_start_txn, newest_stop_txn);

    if (oldest_start_ts > start_durable_ts)
        WT_ERR_ASSERT(session, oldest_start_ts <= start_durable_ts, WT_PANIC,
          "an oldest start timestamp %s newer than its durable start timestamp %s",
          __wt_timestamp_to_string(oldest_start_ts, ts_string[0]),
          __wt_timestamp_to_string(start_durable_ts, ts_string[1]));

    if (newest_stop_ts != WT_TS_MAX && newest_stop_ts > stop_durable_ts)
        WT_ERR_ASSERT(session, newest_stop_ts <= stop_durable_ts, WT_PANIC,
          "a newest stop timestamp %s newer than its durable stop timestamp %s",
          __wt_timestamp_to_string(newest_stop_ts, ts_string[0]),
          __wt_timestamp_to_string(stop_durable_ts, ts_string[1]));
#else
    WT_UNUSED(session);
    WT_UNUSED(start_durable_ts);
    WT_UNUSED(oldest_start_ts);
    WT_UNUSED(oldest_start_txn);
    WT_UNUSED(stop_durable_ts);
    WT_UNUSED(newest_stop_ts);
    WT_UNUSED(newest_stop_txn);
#endif
}

/*
 * __cell_pack_addr_validity --
 *     Pack the validity window for an address.
 */
static inline void
__cell_pack_addr_validity(WT_SESSION_IMPL *session, uint8_t **pp, wt_timestamp_t start_durable_ts,
  wt_timestamp_t oldest_start_ts, uint64_t oldest_start_txn, wt_timestamp_t stop_durable_ts,
  wt_timestamp_t newest_stop_ts, uint64_t newest_stop_txn, bool prepare)
{
    uint8_t flags, *flagsp;

    /* Globally visible values have no associated validity window. */
    if (start_durable_ts == WT_TS_NONE && stop_durable_ts == WT_TS_NONE &&
      oldest_start_ts == WT_TS_NONE && oldest_start_txn == WT_TXN_NONE &&
      newest_stop_ts == WT_TS_MAX && newest_stop_txn == WT_TXN_MAX) {
        ++*pp;
        return;
    }

    __wt_check_addr_validity(session, start_durable_ts, oldest_start_ts, oldest_start_txn,
      stop_durable_ts, newest_stop_ts, newest_stop_txn);

    **pp |= WT_CELL_SECOND_DESC;
    ++*pp;
    flagsp = *pp;
    ++*pp;

    flags = 0;
    if (oldest_start_ts != WT_TS_NONE) {
        WT_IGNORE_RET(__wt_vpack_uint(pp, 0, oldest_start_ts));
        LF_SET(WT_CELL_TS_START);
    }
    if (oldest_start_txn != WT_TXN_NONE) {
        WT_IGNORE_RET(__wt_vpack_uint(pp, 0, oldest_start_txn));
        LF_SET(WT_CELL_TXN_START);
    }
    if (start_durable_ts != WT_TS_NONE) {
        /* Store differences, not absolutes. */
        /*
         * FIXME-prepare-support:
         * WT_ASSERT(
         *  session, oldest_start_ts != WT_TS_NONE && oldest_start_ts <= start_durable_ts);
         */
        /*
         * Unlike value cell, we store the durable start timestamp even the difference is zero
         * compared to oldest commit timestamp. The difference can only be zero when the page
         * contains all the key/value pairs with the same timestamp. But this scenario is rare and
         * having that check to find out whether it is zero or not will unnecessarily add overhead
         * than benefit.
         */
        WT_IGNORE_RET(__wt_vpack_uint(pp, 0, start_durable_ts - oldest_start_ts));
        LF_SET(WT_CELL_TS_DURABLE_START);
    }
    if (newest_stop_ts != WT_TS_MAX) {
        /* Store differences, not absolutes. */
        WT_IGNORE_RET(__wt_vpack_uint(pp, 0, newest_stop_ts - oldest_start_ts));
        LF_SET(WT_CELL_TS_STOP);
    }
    if (newest_stop_txn != WT_TXN_MAX) {
        /* Store differences, not absolutes. */
        WT_IGNORE_RET(__wt_vpack_uint(pp, 0, newest_stop_txn - oldest_start_txn));
        LF_SET(WT_CELL_TXN_STOP);
    }
    if (stop_durable_ts != WT_TS_NONE) {
        /* Store differences, not absolutes. */
        /*
         * FIXME-prepare-support:
         * WT_ASSERT(session,
         *   newest_stop_ts != WT_TS_MAX && newest_stop_ts <= stop_durable__ts);
         */
        /*
         * Unlike value cell, we store the durable stop timestamp even the difference is zero
         * compared to newest commit timestamp. The difference can only be zero when the page
         * contains all the key/value pairs with the same timestamp. But this scenario is rare and
         * having that check to find out whether it is zero or not will unnecessarily add overhead
         * than benefit.
         */
        WT_IGNORE_RET(__wt_vpack_uint(pp, 0, stop_durable_ts - newest_stop_ts));
        LF_SET(WT_CELL_TS_DURABLE_STOP);
    }
    /*
     * Currently, no uncommitted prepared updates are written to the data store, so this flag must
     * be false until we allow writing them in WT-5984. In that ticket this assert must be removed.
     */
    WT_ASSERT(session, prepare == false);
    if (prepare)
        LF_SET(WT_CELL_PREPARE);
    *flagsp = flags;
}

/*
 * __wt_cell_pack_addr --
 *     Pack an address cell.
 */
static inline size_t
__wt_cell_pack_addr(WT_SESSION_IMPL *session, WT_CELL *cell, u_int cell_type, uint64_t recno,
  wt_timestamp_t start_durable_ts, wt_timestamp_t oldest_start_ts, uint64_t oldest_start_txn,
  wt_timestamp_t stop_durable_ts, wt_timestamp_t newest_stop_ts, uint64_t newest_stop_txn,
  bool prepare, size_t size)
{
    uint8_t *p;

    /* Start building a cell: the descriptor byte starts zero. */
    p = cell->__chunk;
    *p = '\0';

    __cell_pack_addr_validity(session, &p, start_durable_ts, oldest_start_ts, oldest_start_txn,
      stop_durable_ts, newest_stop_ts, newest_stop_txn, prepare);

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
__wt_cell_pack_value(WT_SESSION_IMPL *session, WT_CELL *cell, wt_timestamp_t durable_start_ts,
  wt_timestamp_t start_ts, uint64_t start_txn, wt_timestamp_t durable_stop_ts,
  wt_timestamp_t stop_ts, uint64_t stop_txn, bool prepare, uint64_t rle, size_t size)
{
    uint8_t byte, *p;
    bool validity;

    /* Start building a cell: the descriptor byte starts zero. */
    p = cell->__chunk;
    *p = '\0';

    __cell_pack_value_validity(session, &p, durable_start_ts, start_ts, start_txn, durable_stop_ts,
      stop_ts, stop_txn, prepare);

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
__wt_cell_pack_copy(WT_SESSION_IMPL *session, WT_CELL *cell, wt_timestamp_t start_durable_ts,
  wt_timestamp_t start_ts, uint64_t start_txn, wt_timestamp_t stop_durable_ts,
  wt_timestamp_t stop_ts, uint64_t stop_txn, bool prepare, uint64_t rle, uint64_t v)
{
    uint8_t *p;

    /* Start building a cell: the descriptor byte starts zero. */
    p = cell->__chunk;
    *p = '\0';

    __cell_pack_value_validity(session, &p, start_durable_ts, start_ts, start_txn, stop_durable_ts,
      stop_ts, stop_txn, prepare);

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
__wt_cell_pack_del(WT_SESSION_IMPL *session, WT_CELL *cell, wt_timestamp_t start_durable_ts,
  wt_timestamp_t start_ts, uint64_t start_txn, wt_timestamp_t stop_durable_ts,
  wt_timestamp_t stop_ts, uint64_t stop_txn, uint64_t rle)
{
    uint8_t *p;

    /* Start building a cell: the descriptor byte starts zero. */
    p = cell->__chunk;
    *p = '\0';

    /* FIXME-prepare-support: we should pass prepare value. */
    __cell_pack_value_validity(session, &p, start_durable_ts, start_ts, start_txn, stop_durable_ts,
      stop_ts, stop_txn, false);

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
__wt_cell_pack_ovfl(WT_SESSION_IMPL *session, WT_CELL *cell, uint8_t type,
  wt_timestamp_t durable_start_ts, wt_timestamp_t start_ts, uint64_t start_txn,
  wt_timestamp_t durable_stop_ts, wt_timestamp_t stop_ts, uint64_t stop_txn, bool prepare,
  uint64_t rle, size_t size)
{
    uint8_t *p;

    /* Start building a cell: the descriptor byte starts zero. */
    p = cell->__chunk;
    *p = '\0';

    switch (type) {
    case WT_CELL_KEY_OVFL:
    case WT_CELL_KEY_OVFL_RM:
        ++p;
        break;
    case WT_CELL_VALUE_OVFL:
    case WT_CELL_VALUE_OVFL_RM:
        __cell_pack_value_validity(session, &p, durable_start_ts, start_ts, start_txn,
          durable_stop_ts, stop_ts, stop_txn, prepare);
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
__wt_cell_rle(WT_CELL_UNPACK *unpack)
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
__wt_cell_total_len(WT_CELL_UNPACK *unpack)
{
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
     * we can't call __wt_off_page directly, it's in btree.i which requires
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
 * __wt_cell_unpack_safe --
 *     Unpack a WT_CELL into a structure, with optional boundary checks.
 */
static inline int
__wt_cell_unpack_safe(WT_SESSION_IMPL *session, const WT_PAGE_HEADER *dsk, WT_CELL *cell,
  WT_CELL_UNPACK *unpack, const void *end)
{
    struct {
        uint64_t v;
        wt_timestamp_t start_ts;
        wt_timestamp_t durable_start_ts;
        uint64_t start_txn;
        wt_timestamp_t stop_ts;
        wt_timestamp_t durable_stop_ts;
        uint64_t stop_txn;
        uint32_t len;
    } copy;
    uint64_t v;
    const uint8_t *p;
    uint8_t flags;

    copy.v = 0; /* -Werror=maybe-uninitialized */
    copy.start_ts = WT_TS_NONE;
    copy.durable_start_ts = WT_TS_NONE;
    copy.start_txn = WT_TXN_NONE;
    copy.stop_ts = WT_TS_MAX;
    copy.durable_stop_ts = WT_TS_NONE;
    copy.stop_txn = WT_TXN_MAX;
    copy.len = 0;

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
     * NB: when unpacking a WT_CELL_VALUE_COPY cell, unpack.cell is returned as the original cell,
     * not the copied cell (in other words, data from the copied cell must be available from unpack
     * after we return, as our caller has no way to find the copied cell).
     */
    unpack->cell = cell;

restart:
    WT_CELL_LEN_CHK(cell, 0);

    /*
     * This path is performance critical for read-only trees, we're parsing on-page structures. For
     * that reason we don't clear the unpacked cell structure (although that would be simpler),
     * instead we make sure we initialize all structure elements either here or in the immediately
     * following switch. All validity windows default to durability.
     */
    unpack->v = 0;
    unpack->durable_start_ts = WT_TS_NONE;
    unpack->durable_stop_ts = WT_TS_NONE;
    unpack->start_ts = WT_TS_NONE;
    unpack->start_txn = WT_TXN_NONE;
    unpack->stop_ts = WT_TS_MAX;
    unpack->stop_txn = WT_TXN_MAX;
    unpack->newest_start_durable_ts = WT_TS_NONE;
    unpack->newest_stop_durable_ts = WT_TS_NONE;
    unpack->oldest_start_ts = WT_TS_NONE;
    unpack->oldest_start_txn = WT_TXN_NONE;
    unpack->newest_stop_ts = WT_TS_MAX;
    unpack->newest_stop_txn = WT_TXN_MAX;
    unpack->raw = (uint8_t)__wt_cell_type_raw(cell);
    unpack->type = (uint8_t)__wt_cell_type(cell);
    unpack->flags = 0;

    /*
     * Handle cells with none of RLE counts, validity window or data length: short key/data cells
     * have 6 bits of data length in the descriptor byte and nothing else.
     */
    switch (unpack->raw) {
    case WT_CELL_KEY_SHORT_PFX:
        WT_CELL_LEN_CHK(cell, 1); /* skip prefix */
        unpack->prefix = cell->__chunk[1];
        unpack->data = cell->__chunk + 2;
        unpack->size = cell->__chunk[0] >> WT_CELL_SHORT_SHIFT;
        unpack->__len = 2 + unpack->size;
        goto done;
    case WT_CELL_KEY_SHORT:
    case WT_CELL_VALUE_SHORT:
        unpack->prefix = 0;
        unpack->data = cell->__chunk + 1;
        unpack->size = cell->__chunk[0] >> WT_CELL_SHORT_SHIFT;
        unpack->__len = 1 + unpack->size;
        goto done;
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
        if ((cell->__chunk[0] & WT_CELL_SECOND_DESC) == 0)
            break;
        flags = *p++; /* skip second descriptor byte */

        if (LF_ISSET(WT_CELL_PREPARE))
            F_SET(unpack, WT_CELL_UNPACK_PREPARE);
        if (LF_ISSET(WT_CELL_TS_START))
            WT_RET(__wt_vunpack_uint(
              &p, end == NULL ? 0 : WT_PTRDIFF(end, p), &unpack->oldest_start_ts));
        if (LF_ISSET(WT_CELL_TXN_START))
            WT_RET(__wt_vunpack_uint(
              &p, end == NULL ? 0 : WT_PTRDIFF(end, p), &unpack->oldest_start_txn));
        if (LF_ISSET(WT_CELL_TS_DURABLE_START)) {
            WT_RET(__wt_vunpack_uint(
              &p, end == NULL ? 0 : WT_PTRDIFF(end, p), &unpack->newest_start_durable_ts));
            unpack->newest_start_durable_ts += unpack->oldest_start_ts;
        }

        if (LF_ISSET(WT_CELL_TS_STOP)) {
            WT_RET(
              __wt_vunpack_uint(&p, end == NULL ? 0 : WT_PTRDIFF(end, p), &unpack->newest_stop_ts));
            unpack->newest_stop_ts += unpack->oldest_start_ts;
        }
        if (LF_ISSET(WT_CELL_TXN_STOP)) {
            WT_RET(__wt_vunpack_uint(
              &p, end == NULL ? 0 : WT_PTRDIFF(end, p), &unpack->newest_stop_txn));
            unpack->newest_stop_txn += unpack->oldest_start_txn;
        }
        if (LF_ISSET(WT_CELL_TS_DURABLE_STOP)) {
            WT_RET(__wt_vunpack_uint(
              &p, end == NULL ? 0 : WT_PTRDIFF(end, p), &unpack->newest_stop_durable_ts));
            unpack->newest_stop_durable_ts += unpack->newest_stop_ts;
        }

        __wt_check_addr_validity(session, unpack->newest_start_durable_ts, unpack->oldest_start_ts,
          unpack->oldest_start_txn, unpack->newest_stop_durable_ts, unpack->newest_stop_ts,
          unpack->newest_stop_txn);
        break;
    case WT_CELL_DEL:
    case WT_CELL_VALUE:
    case WT_CELL_VALUE_COPY:
    case WT_CELL_VALUE_OVFL:
    case WT_CELL_VALUE_OVFL_RM:
        if ((cell->__chunk[0] & WT_CELL_SECOND_DESC) == 0)
            break;
        flags = *p++; /* skip second descriptor byte */

        if (LF_ISSET(WT_CELL_PREPARE))
            F_SET(unpack, WT_CELL_UNPACK_PREPARE);
        if (LF_ISSET(WT_CELL_TS_START))
            WT_RET(__wt_vunpack_uint(&p, end == NULL ? 0 : WT_PTRDIFF(end, p), &unpack->start_ts));
        if (LF_ISSET(WT_CELL_TXN_START))
            WT_RET(__wt_vunpack_uint(&p, end == NULL ? 0 : WT_PTRDIFF(end, p), &unpack->start_txn));
        if (LF_ISSET(WT_CELL_TS_DURABLE_START)) {
            WT_RET(__wt_vunpack_uint(
              &p, end == NULL ? 0 : WT_PTRDIFF(end, p), &unpack->durable_start_ts));
            unpack->durable_start_ts += unpack->start_ts;
        } else
            unpack->durable_start_ts = unpack->start_ts;

        if (LF_ISSET(WT_CELL_TS_STOP)) {
            WT_RET(__wt_vunpack_uint(&p, end == NULL ? 0 : WT_PTRDIFF(end, p), &unpack->stop_ts));
            unpack->stop_ts += unpack->start_ts;
        }
        if (LF_ISSET(WT_CELL_TXN_STOP)) {
            WT_RET(__wt_vunpack_uint(&p, end == NULL ? 0 : WT_PTRDIFF(end, p), &unpack->stop_txn));
            unpack->stop_txn += unpack->start_txn;
        }
        if (LF_ISSET(WT_CELL_TS_DURABLE_STOP)) {
            WT_RET(__wt_vunpack_uint(
              &p, end == NULL ? 0 : WT_PTRDIFF(end, p), &unpack->durable_stop_ts));
            unpack->durable_stop_ts += unpack->stop_ts;
        } else if (unpack->stop_ts != WT_TS_MAX)
            unpack->durable_stop_ts = unpack->stop_ts;
        else
            unpack->durable_stop_ts = WT_TS_NONE;

        __cell_check_value_validity(session, unpack->durable_start_ts, unpack->start_ts,
          unpack->start_txn, unpack->durable_stop_ts, unpack->stop_ts, unpack->stop_txn);
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
        /*
         * The cell is followed by an offset to a cell written earlier in the page. Save/restore the
         * length and RLE of this cell, we need the length to step through the set of cells on the
         * page and this RLE is probably different from the RLE of the earlier cell.
         */
        WT_RET(__wt_vunpack_uint(&p, end == NULL ? 0 : WT_PTRDIFF(end, p), &v));
        copy.v = unpack->v;
        copy.start_ts = unpack->start_ts;
        copy.durable_start_ts = unpack->durable_start_ts;
        copy.start_txn = unpack->start_txn;
        copy.stop_ts = unpack->stop_ts;
        copy.durable_stop_ts = unpack->durable_stop_ts;
        copy.stop_txn = unpack->stop_txn;
        copy.len = WT_PTRDIFF32(p, cell);
        cell = (WT_CELL *)((uint8_t *)cell - v);
        goto restart;

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

/*
 * Check the original cell against the full cell length (this is a diagnostic as well, we may be
 * copying the cell from the page and we need the right length).
 */
done:
    WT_CELL_LEN_CHK(cell, unpack->__len);
    if (copy.len != 0) {
        unpack->raw = WT_CELL_VALUE_COPY;
        unpack->v = copy.v;
        unpack->start_ts = copy.start_ts;
        unpack->durable_start_ts = copy.durable_start_ts;
        unpack->start_txn = copy.start_txn;
        unpack->stop_ts = copy.stop_ts;
        unpack->durable_stop_ts = copy.durable_stop_ts;
        unpack->stop_txn = copy.stop_txn;
        unpack->__len = copy.len;
    }

    return (0);
}

/*
 * __wt_cell_unpack_dsk --
 *     Unpack a WT_CELL into a structure.
 */
static inline void
__wt_cell_unpack_dsk(
  WT_SESSION_IMPL *session, const WT_PAGE_HEADER *dsk, WT_CELL *cell, WT_CELL_UNPACK *unpack)
{
    /*
     * Row-store doesn't store zero-length values on pages, but this allows us to pretend.
     */
    if (cell == NULL) {
        unpack->cell = NULL;
        unpack->v = 0;
        /*
         * If there isn't any value validity window (which is what it will take to get to a
         * zero-length item), the value must be stable.
         */
        unpack->durable_start_ts = WT_TS_NONE;
        unpack->durable_stop_ts = WT_TS_NONE;
        unpack->start_ts = WT_TS_NONE;
        unpack->start_txn = WT_TXN_NONE;
        unpack->stop_ts = WT_TS_MAX;
        unpack->stop_txn = WT_TXN_MAX;
        unpack->newest_start_durable_ts = WT_TS_NONE;
        unpack->newest_stop_durable_ts = WT_TS_NONE;
        unpack->oldest_start_ts = WT_TS_NONE;
        unpack->oldest_start_txn = WT_TXN_NONE;
        unpack->newest_stop_ts = WT_TS_MAX;
        unpack->newest_stop_txn = WT_TXN_MAX;
        unpack->data = "";
        unpack->size = 0;
        unpack->__len = 0;
        unpack->prefix = 0;
        unpack->raw = unpack->type = WT_CELL_VALUE;
        unpack->flags = 0;
        return;
    }

    WT_IGNORE_RET(__wt_cell_unpack_safe(session, dsk, cell, unpack, NULL));

    /*
     * If the page came from a previous run, reset the transaction ids to "none" and timestamps to 0
     * as appropriate. Transaction ids shouldn't persist between runs so these are always set to
     * "none". Timestamps should persist between runs however, the absence of a timestamp (in the
     * case of a non-timestamped write) should default to WT_TS_NONE rather than "max" as usual.
     *
     * Note that it is still necessary to unpack each value above even if we end up overwriting them
     * since values in a cell need to be unpacked sequentially.
     *
     * This is how the stop time pair should be interpreted for each type of delete:
     * -
     *                  Timestamp delete  Non-timestamp delete  No delete
     * Current startup  txnid=x, ts=y       txnid=x, ts=WT_TS_NONE           txnid=MAX, ts=MAX
     * Previous startup txnid=0, ts=y       txnid=0, ts=WT_TS_NONE           txnid=MAX, ts=MAX
     */
    if (dsk->write_gen > 0 && dsk->write_gen <= S2C(session)->base_write_gen) {
        /* FIXME-prepare-support: deal with durable timestamps. */
        /* Tell reconciliation we cleared the transaction ids and the cell needs to be rebuilt. */
        if (unpack->start_txn != WT_TXN_NONE) {
            unpack->start_txn = WT_TXN_NONE;
            F_SET(unpack, WT_CELL_UNPACK_TIME_PAIRS_CLEARED);
        }
        if (unpack->stop_txn != WT_TXN_MAX) {
            unpack->stop_txn = WT_TXN_NONE;
            F_SET(unpack, WT_CELL_UNPACK_TIME_PAIRS_CLEARED);
            if (unpack->stop_ts == WT_TS_MAX)
                unpack->stop_ts = WT_TS_NONE;
        } else
            WT_ASSERT(session, unpack->stop_ts == WT_TS_MAX);
        if (unpack->oldest_start_txn != WT_TXN_NONE) {
            unpack->oldest_start_txn = WT_TXN_NONE;
            F_SET(unpack, WT_CELL_UNPACK_TIME_PAIRS_CLEARED);
        }
        if (unpack->newest_stop_txn != WT_TXN_MAX) {
            unpack->newest_stop_txn = WT_TXN_NONE;
            F_SET(unpack, WT_CELL_UNPACK_TIME_PAIRS_CLEARED);
            if (unpack->newest_stop_ts == WT_TS_MAX)
                unpack->newest_stop_ts = WT_TS_NONE;
        } else
            WT_ASSERT(session, unpack->newest_stop_ts == WT_TS_MAX);
    }
}

/*
 * __wt_cell_unpack --
 *     Unpack a WT_CELL into a structure.
 */
static inline void
__wt_cell_unpack(WT_SESSION_IMPL *session, WT_PAGE *page, WT_CELL *cell, WT_CELL_UNPACK *unpack)
{
    __wt_cell_unpack_dsk(session, page->dsk, cell, unpack);
}

/*
 * __cell_data_ref --
 *     Set a buffer to reference the data from an unpacked cell.
 */
static inline int
__cell_data_ref(
  WT_SESSION_IMPL *session, WT_PAGE *page, int page_type, WT_CELL_UNPACK *unpack, WT_ITEM *store)
{
    WT_BTREE *btree;
    bool decoded;
    void *huffman;

    btree = S2BT(session);

    /* Reference the cell's data, optionally decode it. */
    switch (unpack->type) {
    case WT_CELL_KEY:
        store->data = unpack->data;
        store->size = unpack->size;
        if (page_type == WT_PAGE_ROW_INT)
            return (0);

        huffman = btree->huffman_key;
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

        huffman = btree->huffman_key;
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

    return (huffman == NULL || store->size == 0 ? 0 : __wt_huffman_decode(session, huffman,
                                                        store->data, store->size, store));
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
__wt_dsk_cell_data_ref(
  WT_SESSION_IMPL *session, int page_type, WT_CELL_UNPACK *unpack, WT_ITEM *store)
{
    WT_ASSERT(session, __wt_cell_type_raw(unpack->cell) != WT_CELL_VALUE_OVFL_RM);
    return (__cell_data_ref(session, NULL, page_type, unpack, store));
}

/*
 * __wt_page_cell_data_ref --
 *     Set a buffer to reference the data from an unpacked cell.
 */
static inline int
__wt_page_cell_data_ref(
  WT_SESSION_IMPL *session, WT_PAGE *page, WT_CELL_UNPACK *unpack, WT_ITEM *store)
{
    return (__cell_data_ref(session, page, page->type, unpack, store));
}

/*
 * WT_CELL_FOREACH --
 *	Walk the cells on a page.
 */
#define WT_CELL_FOREACH_BEGIN(session, btree, dsk, unpack)                              \
    do {                                                                                \
        uint32_t __i;                                                                   \
        uint8_t *__cell;                                                                \
        for (__cell = WT_PAGE_HEADER_BYTE(btree, dsk), __i = (dsk)->u.entries; __i > 0; \
             __cell += (unpack).__len, --__i) {                                         \
            __wt_cell_unpack_dsk(session, dsk, (WT_CELL *)__cell, &(unpack));

#define WT_CELL_FOREACH_END \
    }                       \
    }                       \
    while (0)
