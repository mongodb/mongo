/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

/*
 * __wt_log_cmp --
 *     Compare 2 LSNs, return -1 if lsn1 < lsn2, 0if lsn1 == lsn2 and 1 if lsn1 > lsn2.
 */
static WT_INLINE int
__wt_log_cmp(WT_LSN *lsn1, WT_LSN *lsn2)
{
    uint64_t l1, l2;

    /*
     * Read LSNs into local variables so that we only read each field once and all comparisons are
     * on the same values.
     */
    WT_READ_ONCE(l1, lsn1->file_offset);
    WT_READ_ONCE(l2, lsn2->file_offset);

    return (l1 < l2 ? -1 : (l1 > l2 ? 1 : 0));
}

/*
 * __wt_lsn_string --
 *     Return a printable string representation of an lsn.
 */
static WT_INLINE int
__wt_lsn_string(WT_SESSION_IMPL *session, WT_LSN *lsn, WT_ITEM *buf)
{
    return (
      __wt_buf_fmt(session, buf, "%" PRIu32 ",%" PRIu32, __wt_lsn_file(lsn), __wt_lsn_offset(lsn)));
}

/*
 * __wt_lsn_file --
 *     Return a log sequence number's file.
 */
static WT_INLINE uint32_t
__wt_lsn_file(WT_LSN *lsn)
{
    return (__wt_atomic_load32(&lsn->l.file));
}

/*
 * __wt_lsn_offset --
 *     Return a log sequence number's offset.
 */
static WT_INLINE uint32_t
__wt_lsn_offset(WT_LSN *lsn)
{
    return (__wt_atomic_load32(&lsn->l.offset));
}
