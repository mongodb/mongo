/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * Address cookie version numbers.
 *
 * Before incrementing the version number, first check if the format change could be better handled
 * by introducing a new flag. For example, a flag would be useful if adding an optional field, while
 * a new version number would be more appropriate when introducing a new mandatory field, changing
 * the meaning of an existing field, or removing a field or a flag.
 */
#define WT_BLOCK_DISAGG_ADDR_VERSION 0
#define WT_BLOCK_DISAGG_ADDR_VERSION_MIN 0 /* The oldest version that can read this format. */

/*
 * __block_disagg_addr_pack_version --
 *     Pack the address cookie version into the buffer.
 */
static inline int
__block_disagg_addr_pack_version(uint8_t **pp, size_t maxlen)
{
    return (__wt_4b_pack_posint2(pp, maxlen ? *pp + maxlen : NULL, WT_BLOCK_DISAGG_ADDR_VERSION,
      WT_BLOCK_DISAGG_ADDR_VERSION_MIN));
}

/*
 * __block_disagg_addr_unpack_version --
 *     Unpack the address cookie version from the buffer.
 */
static inline int
__block_disagg_addr_unpack_version(
  const uint8_t **pp, size_t maxlen, uint8_t *version, uint8_t *version_min)
{
    uint64_t version_ = 0,
             version_min_ =
               0; /* Just to suppress gcc "may be used uninitialized in this function" */
    WT_RET(__wt_4b_unpack_posint2(pp, maxlen ? *pp + maxlen : NULL, &version_, &version_min_));
    *version = (uint8_t)version_;
    *version_min = (uint8_t)version_min_;
    return (0);
}

/*
 * __wti_block_disagg_addr_pack --
 *     Convert the filesystem components into its address cookie.
 */
int
__wti_block_disagg_addr_pack(
  WT_SESSION_IMPL *session, uint8_t **pp, const WT_BLOCK_DISAGG_ADDRESS_COOKIE *cookie)
{
    uint64_t base_lsn_delta;

    WT_ASSERT(session, cookie->page_id != WT_BLOCK_INVALID_PAGE_ID);
    WT_ASSERT(session, cookie->size > 0);

    /* We will store the base LSN as a delta relative to the LSN to save space. */
    WT_ASSERT_ALWAYS(session, cookie->lsn > cookie->base_lsn,
      "LSN %" PRIu64 " must be larger than base LSN %" PRIu64, cookie->lsn, cookie->base_lsn);
    base_lsn_delta = cookie->lsn - cookie->base_lsn;

    /* Write the address version. */
    WT_RET(__block_disagg_addr_pack_version(pp, 0));

    /* Pack the address cookie. */
    WT_RET(__wt_vpack_uint(pp, 0, cookie->page_id));
    WT_RET(__wt_vpack_uint(pp, 0, cookie->flags));
    WT_RET(__wt_vpack_uint(pp, 0, cookie->lsn));
    WT_RET(__wt_vpack_uint(pp, 0, base_lsn_delta));
    WT_RET(__wt_vpack_uint(pp, 0, cookie->size));

    /* Pack the checksum as a fixed-length 32-bit integer. */
    WT_RET(__wt_pack_fixed_uint32(pp, 0, cookie->checksum));

    return (0);
}

/*
 * __wti_block_disagg_addr_unpack --
 *     Convert a disaggregated address cookie into its components UPDATING the caller's buffer
 *     reference.
 */
int
__wti_block_disagg_addr_unpack(WT_SESSION_IMPL *session, const uint8_t **buf, size_t buf_size,
  WT_BLOCK_DISAGG_ADDRESS_COOKIE *cookie)
{
    uint64_t base_lsn, base_lsn_delta, flags, lsn, page_id, size, unsupported_flags;
    uint32_t checksum;
    uint8_t version = 0,
            version_min = 0; /* Just to suppress gcc "may be used uninitialized in this function" */
    const uint8_t *begin;

    begin = *buf;

    /* Avoid compiler warnings. */
    base_lsn_delta = flags = lsn = page_id = size = 0;
    checksum = 0;

    /* Unpack the address version. */
    WT_RET(__block_disagg_addr_unpack_version(buf, 0, &version, &version_min));
    if (version_min > WT_BLOCK_DISAGG_ADDR_VERSION)
        WT_RET_MSG(session, ENOTSUP,
          "Unsupported disaggregated address cookie version %" PRIu8 ", min %" PRIu8, version,
          version_min);

    /* Unpack the address cookie. */
    WT_RET(__wt_vunpack_uint(buf, 0, &page_id));
    WT_RET(__wt_vunpack_uint(buf, 0, &flags));
    WT_RET(__wt_vunpack_uint(buf, 0, &lsn));
    WT_RET(__wt_vunpack_uint(buf, 0, &base_lsn_delta));
    WT_RET(__wt_vunpack_uint(buf, 0, &size));

    /* Unpack the checksum as a fixed-length 32-bit integer. */
    WT_RET(__wt_unpack_fixed_uint32(buf, 0, &checksum));

    /* Get the base LSN from the delta. */
    if (lsn < base_lsn_delta)
        WT_RET_MSG(session, EINVAL,
          "Disaggregated address cookie LSN %" PRIu64 " is smaller than base LSN delta %" PRIu64,
          lsn, base_lsn_delta);
    base_lsn = lsn - base_lsn_delta;

    /* Check the page ID and size. */
    if (page_id == WT_BLOCK_INVALID_PAGE_ID)
        WT_RET_MSG(
          session, EINVAL, "Disaggregated address cookie page ID %" PRIu64 " is invalid", page_id);
    if (size == 0)
        WT_RET_MSG(session, EINVAL, "Disaggregated address cookie size %" PRIu32 " is invalid",
          (uint32_t)size);

    WT_CLEAR(*cookie);
    cookie->page_id = page_id;
    cookie->flags = flags;
    cookie->lsn = lsn;
    cookie->base_lsn = base_lsn;
    cookie->size = (uint32_t)size;
    cookie->checksum = checksum;

    /*
     * Check the address cookie size, but only (1) if we are reading a supported version of the
     * address cookie, and (2) if there are no unsupported flags. If we are reading a new version,
     * we can't check the size, as more fields could have been added.
     */
    unsupported_flags = flags;
    FLD_CLR(unsupported_flags, WT_BLOCK_DISAGG_ADDR_ALL_FLAGS);
    if (version <= WT_BLOCK_DISAGG_ADDR_VERSION && unsupported_flags == 0 &&
      (size_t)(*buf - begin) != buf_size)
        WT_RET_MSG(session, EINVAL,
          "Disaggregated address cookie size mismatch: expected %" PRIuMAX ", got %" PRIuMAX,
          (uintmax_t)buf_size, (uintmax_t)(*buf - begin));

    return (0);
}

/*
 * __wti_block_disagg_addr_invalid --
 *     Return an error code if an address cookie is invalid.
 */
int
__wti_block_disagg_addr_invalid(WT_SESSION_IMPL *session, const uint8_t *addr, size_t addr_size)
{
    WT_BLOCK_DISAGG_ADDRESS_COOKIE cookie;

    /* Crack the cookie - there aren't further checks for object blocks. */
    WT_RET(__wti_block_disagg_addr_unpack(session, &addr, addr_size, &cookie));

    return (0);
}

/*
 * __wti_block_disagg_addr_string --
 *     Return a printable string representation of an address cookie.
 */
int
__wti_block_disagg_addr_string(
  WT_BM *bm, WT_SESSION_IMPL *session, WT_ITEM *buf, const uint8_t *addr, size_t addr_size)
{
    WT_BLOCK_DISAGG_ADDRESS_COOKIE cookie;

    WT_UNUSED(bm);

    /* Crack the cookie. */
    WT_RET(__wti_block_disagg_addr_unpack(session, &addr, addr_size, &cookie));

    /* Printable representation. */
    WT_RET(__wt_buf_fmt(session, buf,
      "[%" PRIuMAX ", %" PRIxMAX ", %" PRIuMAX ", %" PRIuMAX ", %" PRIu32 ", %" PRIu32 "]",
      (uintmax_t)cookie.page_id, (uintmax_t)cookie.flags, (uintmax_t)cookie.lsn,
      (uintmax_t)cookie.base_lsn, cookie.size, cookie.checksum));

    return (0);
}

/*
 * __wti_block_disagg_ckpt_pack --
 *     Pack the raw content of a checkpoint record for this disagg manager. It will be encoded in
 *     the metadata for the table and used to find the checkpoint again in the future.
 */
int
__wti_block_disagg_ckpt_pack(WT_SESSION_IMPL *session, WT_BLOCK_DISAGG *block_disagg, uint8_t **buf,
  const WT_BLOCK_DISAGG_ADDRESS_COOKIE *root_cookie)
{
    WT_UNUSED(block_disagg);

    WT_RET(__wti_block_disagg_addr_pack(session, buf, root_cookie));

    return (0);
}

/*
 * __wti_block_disagg_ckpt_unpack --
 *     Pack the raw content of a checkpoint record for this disagg manager. It will be encoded in
 *     the metadata for the table and used to find the checkpoint again in the future.
 */
int
__wti_block_disagg_ckpt_unpack(WT_SESSION_IMPL *session, WT_BLOCK_DISAGG *block_disagg,
  const uint8_t *buf, size_t buf_size, WT_BLOCK_DISAGG_ADDRESS_COOKIE *root_cookie)
{
    WT_UNUSED(block_disagg);

    /* Retrieve the root page information */
    WT_RET(__wti_block_disagg_addr_unpack(session, &buf, buf_size, root_cookie));

    return (0);
}
