/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * WiredTiger's block manager interface.
 */

/*
 * __wt_extlist_write_pair --
 *     Write an extent list pair.
 */
static inline int
__wt_extlist_write_pair(uint8_t **p, wt_off_t off, wt_off_t size)
{
    WT_RET(__wt_vpack_uint(p, 0, (uint64_t)(off)));
    WT_RET(__wt_vpack_uint(p, 0, (uint64_t)(size)));
    return (0);
}

/*
 * __wt_extlist_read_pair --
 *     Read an extent list pair.
 */
static inline int
__wt_extlist_read_pair(const uint8_t **p, wt_off_t *offp, wt_off_t *sizep)
{
    uint64_t v;

    WT_RET(__wt_vunpack_uint(p, 0, &v));
    *offp = (wt_off_t)v;
    WT_RET(__wt_vunpack_uint(p, 0, &v));
    *sizep = (wt_off_t)v;
    return (0);
}
