/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once
#include "log_private.h"

/*
 * __wti_log_desc_byteswap --
 *     Handle big- and little-endian transformation of the log file description block.
 */
static WT_INLINE void
__wti_log_desc_byteswap(WTI_LOG_DESC *desc)
{
#ifdef WORDS_BIGENDIAN
    desc->log_magic = __wt_bswap32(desc->log_magic);
    desc->version = __wt_bswap16(desc->version);
    desc->unused = __wt_bswap16(desc->unused);
    desc->log_size = __wt_bswap64(desc->log_size);
#else
    WT_UNUSED(desc);
#endif
}

/*
 * __wti_log_record_byteswap --
 *     Handle big- and little-endian transformation of the log record header block.
 */
static WT_INLINE void
__wti_log_record_byteswap(WT_LOG_RECORD *record)
{
#ifdef WORDS_BIGENDIAN
    record->len = __wt_bswap32(record->len);
    record->checksum = __wt_bswap32(record->checksum);
    record->flags = __wt_bswap16(record->flags);
    record->mem_len = __wt_bswap32(record->mem_len);
#else
    WT_UNUSED(record);
#endif
}

/*
 * __wt_log_cmp --
 *     Compare 2 LSNs, return -1 if lsn1 < lsn2, 0 if lsn1 == lsn2 and 1 if lsn1 > lsn2.
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
 *     Return a printable string representation of an lsn into a fixed array.
 */
static WT_INLINE int
__wt_lsn_string(WT_LSN *lsn, size_t len, char *buf)
{
    WT_ASSERT(NULL, len >= WT_MAX_LSN_STRING);
    return (
      __wt_snprintf(buf, len, "%" PRIu32 ",%" PRIu32, __wt_lsn_file(lsn), __wt_lsn_offset(lsn)));
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

/*
 * __wti_log_is_prealloc_enabled --
 *     Check if pre-allocation is configured using log_mgr->prealloc_init_count. Use the
 *     log_mgr->prealloc_init_count variable because the log_mgr->prealloc variable performs
 *     concurrent writes/reads and may induce race conditions.
 */
static WT_INLINE bool
__wti_log_is_prealloc_enabled(WT_SESSION_IMPL *session)
{
    return (S2C(session)->log_mgr.prealloc_init_count > 0);
}
