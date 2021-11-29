/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_blkcache_map --
 *     Map a segment of the file in, if possible.
 */
int
__wt_blkcache_map(WT_SESSION_IMPL *session, WT_BLOCK *block, void *mapped_regionp, size_t *lengthp,
  void *mapped_cookiep)
{
    WT_DECL_RET;
    WT_FILE_HANDLE *handle;

    *(void **)mapped_regionp = NULL;
    *lengthp = 0;
    *(void **)mapped_cookiep = NULL;

    /* Map support is configurable. */
    if (!S2C(session)->mmap)
        return (0);

    /*
     * Turn off mapping when verifying the file, because we can't perform checksum validation of
     * mapped segments, and verify has to checksum pages.
     */
    if (block->verify)
        return (0);

    /*
     * Turn off mapping if the application configured a cache size maximum, we can't control how
     * much of the cache size we use in that case.
     */
    if (block->os_cache_max != 0)
        return (0);

    /*
     * There may be no underlying functionality.
     */
    handle = block->fh->handle;
    if (handle->fh_map == NULL)
        return (0);

    /*
     * Map the file into memory. Ignore not-supported errors, we'll read the file through the cache
     * if map fails.
     */
    ret = handle->fh_map(handle, (WT_SESSION *)session, mapped_regionp, lengthp, mapped_cookiep);
    if (ret == EBUSY || ret == ENOTSUP) {
        *(void **)mapped_regionp = NULL;
        ret = 0;
    }

    return (ret);
}

/*
 * __wt_blkcache_unmap --
 *     Unmap any mapped-in segment of the file.
 */
int
__wt_blkcache_unmap(WT_SESSION_IMPL *session, WT_BLOCK *block, void *mapped_region, size_t length,
  void *mapped_cookie)
{
    WT_FILE_HANDLE *handle;

    /* Unmap the file from memory. */
    handle = block->fh->handle;
    return (handle->fh_unmap(handle, (WT_SESSION *)session, mapped_region, length, mapped_cookie));
}

/*
 * __wt_blkcache_map_read --
 *     Map address cookie referenced block into a buffer.
 */
int
__wt_blkcache_map_read(WT_BM *bm, WT_SESSION_IMPL *session, WT_ITEM *buf, const uint8_t *addr,
  size_t addr_size, bool *foundp)
{
    WT_BLOCK *block;
    WT_FH *fh;
    WT_FILE_HANDLE *handle;
    wt_off_t offset;
    uint32_t checksum, objectid, size;

    *foundp = false;

    /*
     * FIXME WT-7872: The WT_BLOCK.map test is wrong; tiered storage assumes object IDs translate to
     * WT_FH structures, not WT_BLOCK structures. When we check if the WT_BLOCK handle references a
     * mapped object, that's not going to work as we might be about to switch to a different WT_FH
     * handle which may or may not reference a mapped object.
     */
    if (!bm->map)
        return (0);

    block = bm->block;

    /* Crack the cookie. */
    WT_RET(__wt_block_addr_unpack(
      session, block, addr, addr_size, &objectid, &offset, &size, &checksum));

    /* Map the block if it's possible. */
    WT_RET(__wt_block_fh(session, block, objectid, &fh));
    handle = fh->handle;
    if (handle->fh_map_preload != NULL && offset + size <= (wt_off_t)bm->maplen &&
      handle->fh_map_preload(
        handle, (WT_SESSION *)session, (uint8_t *)bm->map + offset, size, bm->mapped_cookie) == 0) {
        if (buf != NULL) {
            buf->data = (uint8_t *)bm->map + offset;
            buf->size = size;
        }

        *foundp = true;
        WT_STAT_CONN_INCR(session, block_map_read);
        WT_STAT_CONN_INCRV(session, block_byte_map_read, size);
    }

    return (0);
}
