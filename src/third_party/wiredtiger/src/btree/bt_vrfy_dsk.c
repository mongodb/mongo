/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __err_cell_corrupt(WT_SESSION_IMPL *, int, uint32_t, const char *);
static int __err_cell_corrupt_or_eof(WT_SESSION_IMPL *, int, uint32_t, const char *);
static int __err_cell_type(WT_SESSION_IMPL *, uint32_t, const char *, uint8_t, uint8_t);
static int __verify_dsk_chunk(WT_SESSION_IMPL *, const char *, const WT_PAGE_HEADER *, uint32_t);
static int __verify_dsk_col_fix(WT_SESSION_IMPL *, const char *, const WT_PAGE_HEADER *, WT_ADDR *);
static int __verify_dsk_col_int(WT_SESSION_IMPL *, const char *, const WT_PAGE_HEADER *, WT_ADDR *);
static int __verify_dsk_col_var(WT_SESSION_IMPL *, const char *, const WT_PAGE_HEADER *, WT_ADDR *);
static int __verify_dsk_memsize(WT_SESSION_IMPL *, const char *, const WT_PAGE_HEADER *, WT_CELL *);
static int __verify_dsk_row_int(WT_SESSION_IMPL *, const char *, const WT_PAGE_HEADER *, WT_ADDR *);
static int __verify_dsk_row_leaf(
  WT_SESSION_IMPL *, const char *, const WT_PAGE_HEADER *, WT_ADDR *);

#define WT_ERR_VRFY(session, ...)                                          \
    do {                                                                   \
        if (!(F_ISSET(session, WT_SESSION_QUIET_CORRUPT_FILE))) {          \
            __wt_errx(session, __VA_ARGS__);                               \
            /* Easy way to set a breakpoint when tracking corruption */    \
            WT_IGNORE_RET(__wt_session_breakpoint((WT_SESSION *)session)); \
        }                                                                  \
        goto err;                                                          \
    } while (0)

#define WT_RET_VRFY_RETVAL(session, ret, ...)                              \
    do {                                                                   \
        if (!(F_ISSET(session, WT_SESSION_QUIET_CORRUPT_FILE))) {          \
            if ((ret) == 0)                                                \
                __wt_errx(session, __VA_ARGS__);                           \
            else                                                           \
                __wt_err(session, ret, __VA_ARGS__);                       \
            /* Easy way to set a breakpoint when tracking corruption */    \
            WT_IGNORE_RET(__wt_session_breakpoint((WT_SESSION *)session)); \
        }                                                                  \
        return ((ret) == 0 ? WT_ERROR : ret);                              \
    } while (0)

#define WT_RET_VRFY(session, ...) WT_RET_VRFY_RETVAL(session, 0, __VA_ARGS__)

/*
 * WT_CELL_FOREACH_VRFY --
 *	Iterate through each cell on a page. Verify-specific version of the
 * WT_CELL_FOREACH macro, created because the loop can't simply unpack cells,
 * verify has to do additional work to ensure that unpack is safe.
 */
#define WT_CELL_FOREACH_VRFY(session, dsk, cell, unpack, i)                                 \
    for ((cell) = WT_PAGE_HEADER_BYTE(S2BT(session), dsk), (i) = (dsk)->u.entries; (i) > 0; \
         (cell) = (WT_CELL *)((uint8_t *)(cell) + (unpack)->__len), --(i))

#define WT_CELL_FOREACH_FIX_TIMESTAMPS_VRFY(session, dsk, aux, cell, unpack, i)                \
    for ((cell) = (WT_CELL *)((uint8_t *)(dsk) + (aux)->dataoffset), (i) = (aux)->entries * 2; \
         (i) > 0; (cell) = (WT_CELL *)((uint8_t *)(cell) + (unpack)->__len), --(i))

/*
 * __wt_verify_dsk_image --
 *     Verify a single block as read from disk.
 */
int
__wt_verify_dsk_image(WT_SESSION_IMPL *session, const char *tag, const WT_PAGE_HEADER *dsk,
  size_t size, WT_ADDR *addr, bool empty_page_ok)
{
    uint8_t flags;
    const uint8_t *p, *end;

    /* Check the page type. */
    switch (dsk->type) {
    case WT_PAGE_BLOCK_MANAGER:
    case WT_PAGE_COL_FIX:
    case WT_PAGE_COL_INT:
    case WT_PAGE_COL_VAR:
    case WT_PAGE_OVFL:
    case WT_PAGE_ROW_INT:
    case WT_PAGE_ROW_LEAF:
        break;
    case WT_PAGE_INVALID:
    default:
        WT_RET_VRFY(session, "page at %s has an invalid type of %" PRIu32, tag, dsk->type);
    }

    /* Check the page record number. */
    switch (dsk->type) {
    case WT_PAGE_COL_FIX:
    case WT_PAGE_COL_INT:
    case WT_PAGE_COL_VAR:
        if (dsk->recno != WT_RECNO_OOB)
            break;
        WT_RET_VRFY(session, "%s page at %s has an invalid record number of %d",
          __wt_page_type_string(dsk->type), tag, WT_RECNO_OOB);
    case WT_PAGE_BLOCK_MANAGER:
    case WT_PAGE_OVFL:
    case WT_PAGE_ROW_INT:
    case WT_PAGE_ROW_LEAF:
        if (dsk->recno == WT_RECNO_OOB)
            break;
        WT_RET_VRFY(session,
          "%s page at %s has a record number, which is illegal for this page type",
          __wt_page_type_string(dsk->type), tag);
    }

    /* Check the page flags. */
    flags = dsk->flags;
    if (LF_ISSET(WT_PAGE_COMPRESSED))
        LF_CLR(WT_PAGE_COMPRESSED);
    if (dsk->type == WT_PAGE_ROW_LEAF) {
        if (LF_ISSET(WT_PAGE_EMPTY_V_ALL) && LF_ISSET(WT_PAGE_EMPTY_V_NONE))
            WT_RET_VRFY(
              session, "page at %s has invalid flags combination: 0x%" PRIx8, tag, dsk->flags);
        if (LF_ISSET(WT_PAGE_EMPTY_V_ALL))
            LF_CLR(WT_PAGE_EMPTY_V_ALL);
        if (LF_ISSET(WT_PAGE_EMPTY_V_NONE))
            LF_CLR(WT_PAGE_EMPTY_V_NONE);
    }
    if (LF_ISSET(WT_PAGE_ENCRYPTED))
        LF_CLR(WT_PAGE_ENCRYPTED);
    if (LF_ISSET(WT_PAGE_UNUSED))
        LF_CLR(WT_PAGE_UNUSED);
    if (LF_ISSET(WT_PAGE_FT_UPDATE))
        LF_CLR(WT_PAGE_FT_UPDATE);
    if (flags != 0)
        WT_RET_VRFY(session, "page at %s has invalid flags set: 0x%" PRIx8, tag, flags);

    /* Check the unused byte. */
    if (dsk->unused != 0)
        WT_RET_VRFY(session, "page at %s has non-zero unused page header bytes", tag);

    /* Check the page version. */
    switch (dsk->version) {
    case WT_PAGE_VERSION_ORIG:
    case WT_PAGE_VERSION_TS:
        break;
    default:
        WT_RET_VRFY(session, "page at %s has an invalid version of %" PRIu8, tag, dsk->version);
    }

    /*
     * Any bytes after the data chunk should be nul bytes; ignore if the size is 0, that allows easy
     * checking of disk images where we don't have the size.
     */
    if (size != 0) {
        p = (uint8_t *)dsk + dsk->mem_size;
        end = (uint8_t *)dsk + size;
        for (; p < end; ++p)
            if (*p != '\0')
                WT_RET_VRFY(session, "%s page at %s has non-zero trailing bytes",
                  __wt_page_type_string(dsk->type), tag);
    }

    /* Check for empty pages, then verify the items on the page. */
    switch (dsk->type) {
    case WT_PAGE_COL_INT:
    case WT_PAGE_COL_FIX:
    case WT_PAGE_COL_VAR:
    case WT_PAGE_ROW_INT:
    case WT_PAGE_ROW_LEAF:
        if (!empty_page_ok && dsk->u.entries == 0)
            WT_RET_VRFY(
              session, "%s page at %s has no entries", __wt_page_type_string(dsk->type), tag);
        break;
    case WT_PAGE_BLOCK_MANAGER:
    case WT_PAGE_OVFL:
        if (dsk->u.datalen == 0)
            WT_RET_VRFY(
              session, "%s page at %s has no data", __wt_page_type_string(dsk->type), tag);
        break;
    }
    switch (dsk->type) {
    case WT_PAGE_COL_INT:
        return (__verify_dsk_col_int(session, tag, dsk, addr));
    case WT_PAGE_COL_FIX:
        return (__verify_dsk_col_fix(session, tag, dsk, addr));
    case WT_PAGE_COL_VAR:
        return (__verify_dsk_col_var(session, tag, dsk, addr));
    case WT_PAGE_ROW_INT:
        return (__verify_dsk_row_int(session, tag, dsk, addr));
    case WT_PAGE_ROW_LEAF:
        return (__verify_dsk_row_leaf(session, tag, dsk, addr));
    case WT_PAGE_BLOCK_MANAGER:
    case WT_PAGE_OVFL:
        return (__verify_dsk_chunk(session, tag, dsk, dsk->u.datalen));
    default:
        return (__wt_illegal_value(session, dsk->type));
    }
    /* NOTREACHED */
}

/*
 * __wt_verify_dsk --
 *     Verify a single Btree page as read from disk.
 */
int
__wt_verify_dsk(WT_SESSION_IMPL *session, const char *tag, WT_ITEM *buf)
{
    return (__wt_verify_dsk_image(session, tag, buf->data, buf->size, NULL, false));
}

/*
 * __verify_dsk_addr_validity --
 *     Verify an address cell's validity window.
 */
static int
__verify_dsk_addr_validity(WT_SESSION_IMPL *session, WT_CELL_UNPACK_ADDR *unpack, uint32_t cell_num,
  WT_ADDR *addr, const char *tag)
{
    WT_DECL_RET;

    if ((ret = __wt_time_aggregate_validate(session, &unpack->ta, addr != NULL ? &addr->ta : NULL,
           F_ISSET(session, WT_SESSION_QUIET_CORRUPT_FILE))) == 0)
        return (0);

    WT_RET_VRFY_RETVAL(session, ret, "cell %" PRIu32 " on page at %s failed timestamp validation",
      cell_num - 1, tag);
}

/*
 * __verify_dsk_value_validity --
 *     Verify a value cell's validity window.
 */
static int
__verify_dsk_value_validity(WT_SESSION_IMPL *session, WT_CELL_UNPACK_KV *unpack, uint32_t cell_num,
  WT_ADDR *addr, const char *tag)
{
    WT_DECL_RET;

    if ((ret = __wt_time_value_validate(session, &unpack->tw, addr != NULL ? &addr->ta : NULL,
           F_ISSET(session, WT_SESSION_QUIET_CORRUPT_FILE))) == 0)
        return (0);

    WT_RET_VRFY_RETVAL(session, ret, "cell %" PRIu32 " on page at %s failed timestamp validation",
      cell_num - 1, tag);
}

/*
 * __verify_row_key_order_check --
 *     Check key ordering for row-store pages.
 */
static int
__verify_row_key_order_check(WT_SESSION_IMPL *session, WT_ITEM *last, uint32_t last_cell_num,
  WT_ITEM *current, uint32_t cell_num, const char *tag)
{
    WT_DECL_ITEM(tmp1);
    WT_DECL_ITEM(tmp2);
    WT_DECL_RET;
    int cmp;

    WT_RET(__wt_compare(session, S2BT(session)->collator, last, current, &cmp));
    if (cmp < 0)
        return (0);

    WT_ERR(__wt_scr_alloc(session, 0, &tmp1));
    WT_ERR(__wt_scr_alloc(session, 0, &tmp2));

    ret = WT_ERROR;
    WT_ERR_VRFY(session,
      "the %" PRIu32 " and %" PRIu32 " keys on page at %s are incorrectly sorted: %s, %s",
      last_cell_num, cell_num, tag,
      __wt_buf_set_printable(session, last->data, last->size, false, tmp1),
      __wt_buf_set_printable(session, current->data, current->size, false, tmp2));

err:
    __wt_scr_free(session, &tmp1);
    __wt_scr_free(session, &tmp2);
    return (ret);
}

/*
 * __verify_dsk_row_int --
 *     Walk a WT_PAGE_ROW_INT disk page and verify it.
 */
static int
__verify_dsk_row_int(
  WT_SESSION_IMPL *session, const char *tag, const WT_PAGE_HEADER *dsk, WT_ADDR *addr)
{
    WT_BM *bm;
    WT_BTREE *btree;
    WT_CELL *cell;
    WT_CELL_UNPACK_ADDR *unpack, _unpack;
    WT_DECL_ITEM(current);
    WT_DECL_ITEM(last);
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;
    enum { FIRST, WAS_KEY, WAS_VALUE } last_cell_type;
    uint32_t cell_num, cell_type, i, key_cnt;
    uint8_t *end;

    btree = S2BT(session);
    bm = btree->bm;
    unpack = &_unpack;

    WT_ERR(__wt_scr_alloc(session, 0, &current));
    WT_ERR(__wt_scr_alloc(session, 0, &last));

    end = (uint8_t *)dsk + dsk->mem_size;

    last_cell_type = FIRST;
    cell_num = 0;
    key_cnt = 0;
    WT_CELL_FOREACH_VRFY (session, dsk, cell, unpack, i) {
        ++cell_num;

        /* Carefully unpack the cell. */
        ret = __wt_cell_unpack_safe(session, dsk, cell, unpack, NULL, end);
        if (ret != 0) {
            (void)__err_cell_corrupt(session, ret, cell_num, tag);
            goto err;
        }

        /* Check the raw and collapsed cell types. */
        WT_ERR(__err_cell_type(session, cell_num, tag, unpack->raw, dsk->type));
        WT_ERR(__err_cell_type(session, cell_num, tag, unpack->type, dsk->type));
        cell_type = unpack->type;

        /* Internal row-store cells should not have prefix compression or recno/rle fields. */
        if (unpack->prefix != 0)
            WT_ERR_VRFY(
              session, "the %" PRIu32 " cell on page at %s has a non-zero prefix", cell_num, tag);
        if (unpack->v != 0)
            WT_ERR_VRFY(session,
              "the %" PRIu32 " cell on page at %s has a non-zero rle/recno field", cell_num, tag);

        /*
         * Check ordering relationships between the WT_CELL entries. For row-store internal pages,
         * check for:
         *	- two values in a row,
         *	- two keys in a row,
         *	- a value as the first cell on a page.
         */
        switch (cell_type) {
        case WT_CELL_KEY:
        case WT_CELL_KEY_OVFL:
            ++key_cnt;
            switch (last_cell_type) {
            case FIRST:
            case WAS_VALUE:
                break;
            case WAS_KEY:
                WT_ERR_VRFY(session,
                  "cell %" PRIu32 " on page at %s is the first of two adjacent keys", cell_num - 1,
                  tag);
            }
            last_cell_type = WAS_KEY;
            break;
        case WT_CELL_ADDR_DEL:
        case WT_CELL_ADDR_INT:
        case WT_CELL_ADDR_LEAF:
        case WT_CELL_ADDR_LEAF_NO:
            switch (last_cell_type) {
            case FIRST:
                WT_ERR_VRFY(session, "page at %s begins with a value", tag);
            case WAS_KEY:
                break;
            case WAS_VALUE:
                WT_ERR_VRFY(session,
                  "cell %" PRIu32 " on page at %s is the first of two adjacent values",
                  cell_num - 1, tag);
            }
            last_cell_type = WAS_VALUE;
            break;
        }

        /* Check the validity window. */
        switch (cell_type) {
        case WT_CELL_ADDR_DEL:
        case WT_CELL_ADDR_INT:
        case WT_CELL_ADDR_LEAF:
        case WT_CELL_ADDR_LEAF_NO:
            WT_ERR(__verify_dsk_addr_validity(session, unpack, cell_num, addr, tag));
            break;
        }

        /* Check if any referenced item has an invalid address. */
        switch (cell_type) {
        case WT_CELL_ADDR_DEL:
        case WT_CELL_ADDR_INT:
        case WT_CELL_ADDR_LEAF:
        case WT_CELL_ADDR_LEAF_NO:
        case WT_CELL_KEY_OVFL:
            if ((ret = bm->addr_invalid(bm, session, unpack->data, unpack->size)) == EINVAL)
                (void)__err_cell_corrupt_or_eof(session, ret, cell_num, tag);
            WT_ERR(ret);
            break;
        }

        /*
         * Remaining checks are for key order. If this cell isn't a key, we're done, move to the
         * next cell. If this cell is an overflow item, instantiate the key and compare it with the
         * last key.
         */
        switch (cell_type) {
        case WT_CELL_KEY:
            /* Get the cell's data/length and make sure we have enough buffer space. */
            WT_ERR(__wt_buf_init(session, current, unpack->size));

            /* Copy the data into place. */
            memcpy((uint8_t *)current->mem, unpack->data, unpack->size);
            current->size = unpack->size;
            break;
        case WT_CELL_KEY_OVFL:
            WT_ERR(__wt_dsk_cell_data_ref(session, dsk->type, unpack, current));
            break;
        default:
            /* Not a key -- continue with the next cell. */
            continue;
        }

        /*
         * Compare the current key against the last key.
         *
         * Be careful about the 0th key on internal pages: we only store the first byte and custom
         * collators may not be able to handle truncated keys.
         */
        if (cell_num > 3)
            WT_ERR(
              __verify_row_key_order_check(session, last, cell_num - 2, current, cell_num, tag));

        /* Swap the buffers. */
        tmp = last;
        last = current;
        current = tmp;
    }
    WT_ERR(__verify_dsk_memsize(session, tag, dsk, cell));

    /*
     * On row-store internal pages, the key count should be equal to half the number of physical
     * entries.
     */
    if (key_cnt * 2 != dsk->u.entries)
        WT_ERR_VRFY(session,
          "%s page at %s has a key count of %" PRIu32 " and a physical entry count of %" PRIu32,
          __wt_page_type_string(dsk->type), tag, key_cnt, dsk->u.entries);

    if (0) {
err:
        if (ret == 0)
            ret = WT_ERROR;
    }
    __wt_scr_free(session, &current);
    __wt_scr_free(session, &last);
    return (ret);
}

/*
 * __verify_dsk_row_leaf --
 *     Walk a WT_PAGE_ROW_LEAF disk page and verify it.
 */
static int
__verify_dsk_row_leaf(
  WT_SESSION_IMPL *session, const char *tag, const WT_PAGE_HEADER *dsk, WT_ADDR *addr)
{
    WT_BM *bm;
    WT_BTREE *btree;
    WT_CELL *cell;
    WT_CELL_UNPACK_KV *unpack, _unpack;
    WT_DECL_ITEM(current);
    WT_DECL_ITEM(last_ovfl);
    WT_DECL_ITEM(last_pfx);
    WT_DECL_RET;
    WT_ITEM *last;
    enum { FIRST, WAS_KEY, WAS_VALUE } last_cell_type;
    size_t prefix;
    uint32_t cell_num, cell_type, i, key_cnt, last_cell_num;
    uint8_t *end;

    btree = S2BT(session);
    bm = btree->bm;
    unpack = &_unpack;

    WT_ERR(__wt_scr_alloc(session, 0, &current));
    WT_ERR(__wt_scr_alloc(session, 0, &last_pfx));
    WT_ERR(__wt_scr_alloc(session, 0, &last_ovfl));
    last = last_ovfl;

    end = (uint8_t *)dsk + dsk->mem_size;

    last_cell_type = FIRST;
    cell_num = last_cell_num = 0;
    key_cnt = 0;
    WT_CELL_FOREACH_VRFY (session, dsk, cell, unpack, i) {
        ++cell_num;

        /* Carefully unpack the cell. */
        ret = __wt_cell_unpack_safe(session, dsk, cell, NULL, unpack, end);
        if (ret != 0) {
            (void)__err_cell_corrupt(session, ret, cell_num, tag);
            goto err;
        }

        /* Check the raw and collapsed cell types. */
        WT_ERR(__err_cell_type(session, cell_num, tag, unpack->raw, dsk->type));
        WT_ERR(__err_cell_type(session, cell_num, tag, unpack->type, dsk->type));
        cell_type = unpack->type;

        /* Leaf row-store cells should not have recno/rle fields. */
        if (unpack->v != 0)
            WT_ERR_VRFY(session,
              "the %" PRIu32 " cell on page at %s has a non-zero rle/recno field", cell_num, tag);

        /*
         * Check ordering relationships between the WT_CELL entries. For row-store leaf pages, check
         * for:
         *	- two values in a row,
         *	- a value as the first cell on a page.
         */
        switch (cell_type) {
        case WT_CELL_KEY:
        case WT_CELL_KEY_OVFL:
            ++key_cnt;
            last_cell_type = WAS_KEY;
            break;
        case WT_CELL_VALUE:
        case WT_CELL_VALUE_OVFL:
            switch (last_cell_type) {
            case FIRST:
                WT_ERR_VRFY(session, "page at %s begins with a value", tag);
            case WAS_KEY:
                break;
            case WAS_VALUE:
                WT_ERR_VRFY(session,
                  "cell %" PRIu32 " on page at %s is the first of two adjacent values",
                  cell_num - 1, tag);
            }
            last_cell_type = WAS_VALUE;
            break;
        }

        /* Check the validity window. */
        switch (cell_type) {
        case WT_CELL_VALUE:
        case WT_CELL_VALUE_OVFL:
            WT_ERR(__verify_dsk_value_validity(session, unpack, cell_num, addr, tag));
            break;
        }

        /* Check if any referenced item has an invalid address. */
        switch (cell_type) {
        case WT_CELL_KEY_OVFL:
        case WT_CELL_VALUE_OVFL:
            if ((ret = bm->addr_invalid(bm, session, unpack->data, unpack->size)) == EINVAL)
                (void)__err_cell_corrupt_or_eof(session, ret, cell_num, tag);
            WT_ERR(ret);
            break;
        }

        /*
         * Remaining checks are for key order and prefix compression. If this cell isn't a key,
         * we're done, move to the next cell. If this cell is an overflow item, instantiate the key
         * and compare it with the last key. Otherwise, we have to deal with prefix compression.
         */
        switch (cell_type) {
        case WT_CELL_KEY:
            break;
        case WT_CELL_KEY_OVFL:
            WT_ERR(__wt_dsk_cell_data_ref(session, dsk->type, unpack, current));
            goto key_compare;
        default:
            /* Not a key -- continue with the next cell. */
            continue;
        }

        /*
         * Prefix compression checks.
         *
         * Confirm the first non-overflow key on a page has a zero prefix compression count.
         */
        prefix = unpack->prefix;
        if (last_pfx->size == 0 && prefix != 0)
            WT_ERR_VRFY(session,
              "the %" PRIu32
              " key on page at %s is the first non-overflow key on the page and has a non-zero "
              "prefix compression value",
              cell_num, tag);

        /* Confirm the prefix compression count is possible. */
        if (cell_num > 1 && prefix > last->size)
            WT_ERR_VRFY(session,
              "key %" PRIu32 " on page at %s has a prefix compression count of %" WT_SIZET_FMT
              ", larger than the length of the previous key, %" WT_SIZET_FMT,
              cell_num, tag, prefix, last->size);

        /*
         * Get the cell's data/length and make sure we have enough buffer space.
         */
        WT_ERR(__wt_buf_init(session, current, prefix + unpack->size));

        /* Copy the prefix then the data into place. */
        if (prefix != 0)
            memcpy(current->mem, last->data, prefix);
        memcpy((uint8_t *)current->mem + prefix, unpack->data, unpack->size);
        current->size = prefix + unpack->size;

key_compare:
        /*
         * Compare the current key against the last key.
         */
        if (cell_num > 1)
            WT_ERR(
              __verify_row_key_order_check(session, last, last_cell_num, current, cell_num, tag));
        last_cell_num = cell_num;

        /*
         * Swap the buffers: last always references the last key entry, last_pfx and last_ovfl
         * reference the last prefix-compressed and last overflow key entries. Current gets pointed
         * to the buffer we're not using this time around, which is where the next key goes.
         */
        last = current;
        if (cell_type == WT_CELL_KEY) {
            current = last_pfx;
            last_pfx = last;
        } else {
            current = last_ovfl;
            last_ovfl = last;
        }
        WT_ASSERT(session, last != current);
    }
    WT_ERR(__verify_dsk_memsize(session, tag, dsk, cell));

    /*
     * On standard row-store leaf pages there's no check to make, there may be more keys than values
     * as zero-length values aren't physically stored on the page. On row-store leaf pages, where
     * the "no empty values" flag is set, the key count should be equal to half the number of
     * physical entries. On row-store leaf pages where the "all empty values" flag is set, the key
     * count should be equal to the number of physical entries.
     */
    if (F_ISSET(dsk, WT_PAGE_EMPTY_V_ALL) && key_cnt != dsk->u.entries)
        WT_ERR_VRFY(session,
          "%s page at %s with the 'all empty values' flag set has a key count of %" PRIu32
          " and a physical entry count of %" PRIu32,
          __wt_page_type_string(dsk->type), tag, key_cnt, dsk->u.entries);
    if (F_ISSET(dsk, WT_PAGE_EMPTY_V_NONE) && key_cnt * 2 != dsk->u.entries)
        WT_ERR_VRFY(session,
          "%s page at %s with the 'no empty values' flag set has a key count of %" PRIu32
          " and a physical entry count of %" PRIu32,
          __wt_page_type_string(dsk->type), tag, key_cnt, dsk->u.entries);

    if (0) {
err:
        if (ret == 0)
            ret = WT_ERROR;
    }
    __wt_scr_free(session, &current);
    __wt_scr_free(session, &last_pfx);
    __wt_scr_free(session, &last_ovfl);
    return (ret);
}

/*
 * __verify_dsk_col_int --
 *     Walk a WT_PAGE_COL_INT disk page and verify it.
 */
static int
__verify_dsk_col_int(
  WT_SESSION_IMPL *session, const char *tag, const WT_PAGE_HEADER *dsk, WT_ADDR *addr)
{
    WT_BM *bm;
    WT_BTREE *btree;
    WT_CELL *cell;
    WT_CELL_UNPACK_ADDR *unpack, _unpack;
    WT_DECL_RET;
    uint32_t cell_num, i;
    uint8_t *end;

    btree = S2BT(session);
    bm = btree->bm;
    unpack = &_unpack;
    end = (uint8_t *)dsk + dsk->mem_size;

    cell_num = 0;
    WT_CELL_FOREACH_VRFY (session, dsk, cell, unpack, i) {
        ++cell_num;

        /* Carefully unpack the cell. */
        ret = __wt_cell_unpack_safe(session, dsk, cell, unpack, NULL, end);
        if (ret != 0)
            return (__err_cell_corrupt(session, ret, cell_num, tag));

        /* Check the raw and collapsed cell types. */
        WT_RET(__err_cell_type(session, cell_num, tag, unpack->raw, dsk->type));
        WT_RET(__err_cell_type(session, cell_num, tag, unpack->type, dsk->type));

        /* Check the validity window. */
        WT_RET(__verify_dsk_addr_validity(session, unpack, cell_num, addr, tag));

        /* Check if any referenced item is entirely in the file. */
        ret = bm->addr_invalid(bm, session, unpack->data, unpack->size);
        WT_RET_ERROR_OK(ret, EINVAL);
        if (ret == EINVAL)
            return (__err_cell_corrupt_or_eof(session, ret, cell_num, tag));
    }
    WT_RET(__verify_dsk_memsize(session, tag, dsk, cell));

    return (0);
}

/*
 * __verify_dsk_col_fix --
 *     Walk a WT_PAGE_COL_FIX disk page and verify it.
 */
static int
__verify_dsk_col_fix(
  WT_SESSION_IMPL *session, const char *tag, const WT_PAGE_HEADER *dsk, WT_ADDR *addr)
{
    WT_BTREE *btree;
    WT_CELL *cell;
    WT_CELL_UNPACK_KV *unpack, _unpack;
    WT_COL_FIX_AUXILIARY_HEADER auxhdr;
    WT_DECL_RET;
    uint64_t recno_offset;
    uint32_t cell_num, datalen, i;
    const uint8_t *bitstring, *p, *end;

    btree = S2BT(session);
    unpack = &_unpack;
    end = (uint8_t *)dsk + dsk->mem_size;

    /* First, check that the bitmap data isn't off the end of the page. */
    datalen = __bitstr_size(btree->bitcnt * dsk->u.entries);
    bitstring = (uint8_t *)WT_PAGE_HEADER_BYTE(btree, dsk);
    if (bitstring + datalen > end)
        WT_RET_VRFY(session, "data on page at %s extends past the end of the page", tag);

    /* Check that any leftover bits in the bitmap are zeroed. */
    if (!__bit_end_is_clear(bitstring, dsk->u.entries, btree->bitcnt))
        WT_RET_VRFY(session, "last byte of data on page at %s contains trailing garbage", tag);

    /* Unpack the auxiliary header. This function is expected to be paranoid enough to use here. */
    ret = __wt_col_fix_read_auxheader(session, dsk, &auxhdr);
    if (ret != 0)
        WT_RET_VRFY_RETVAL(session, ret, "auxiliary header on page %s invalid", tag);

    switch (auxhdr.version) {
    case WT_COL_FIX_VERSION_NIL:
        /* No time window data; nothing more to do. */
        return (0);
    case WT_COL_FIX_VERSION_TS:
        break;
    default:
        WT_RET_VRFY(session, "%s page at %s has unknown page version %" PRIu32,
          __wt_page_type_string(dsk->type), tag, dsk->version);
    }

    /* Validate the offsets in the auxiliary header. */
    if (auxhdr.emptyoffset > auxhdr.dataoffset)
        /* The empty-space offset is the also end of the auxiliary header. */
        WT_RET_VRFY_RETVAL(session, EINVAL,
          "%s page at %s auxiliary header overlaps data: header ends at offset %" PRIu32
          " and data begins at offset %" PRIu32,
          __wt_page_type_string(dsk->type), tag, auxhdr.emptyoffset, auxhdr.dataoffset);
    if (auxhdr.dataoffset > dsk->mem_size)
        WT_RET_VRFY(session, "%s page at %s has cell offset %" PRIu32 " off the end at %" PRIu32,
          __wt_page_type_string(dsk->type), tag, auxhdr.dataoffset, dsk->mem_size);
    if (auxhdr.dataoffset == dsk->mem_size && auxhdr.entries > 0)
        WT_RET_VRFY(session,
          "%s page at %s has cell offset %" PRIu32 " at the end with %" PRIu32 " auxiliary entries",
          __wt_page_type_string(dsk->type), tag, auxhdr.dataoffset, auxhdr.entries);

    /* Check the number of entries in the auxiliary header. (Note dsk->u.entries is uint32_t.) */
    if (auxhdr.entries > dsk->u.entries)
        WT_RET_VRFY(session,
          "%s page at %s has %" PRIu32
          " auxiliary (time window) entries but there are only %" PRIu32 " keys",
          __wt_page_type_string(dsk->type), tag, auxhdr.entries, dsk->u.entries);

    /* The space between the end of the auxiliary header and the auxiliary data should be zeroed. */
    for (p = (uint8_t *)dsk + auxhdr.emptyoffset; p != (uint8_t *)dsk + auxhdr.dataoffset; p++) {
        if (*p != 0)
            WT_RET_VRFY(session,
              "%s page at %s has nonzero filler byte %u at offset %u (auxiliary start %u)",
              __wt_page_type_string(dsk->type), tag, *p, WT_PTRDIFF32(p, (uint8_t *)dsk),
              auxhdr.dataoffset);
    }

    cell_num = 0;
    WT_CELL_FOREACH_FIX_TIMESTAMPS_VRFY (session, dsk, &auxhdr, cell, unpack, i) {
        ++cell_num;

        /* Carefully unpack the cell. */
        ret = __wt_cell_unpack_safe(session, dsk, cell, NULL, unpack, end);
        if (ret != 0)
            return (__err_cell_corrupt(session, ret, cell_num, tag));

        /* Check the raw cell type. */
        WT_RET(__err_cell_type(session, cell_num, tag, unpack->raw, dsk->type));

        /* The cells should alternate keys and values. */
        if ((cell_num - 1) % 2 == 0) {
            if (unpack->type != WT_CELL_KEY)
                WT_RET_VRFY(session,
                  "in %s page at %s, cell %" PRIu32 " should be a WT_CELL_KEY but is %s",
                  __wt_page_type_string(dsk->type), tag, cell_num - 1,
                  __wt_cell_type_string(unpack->type));
            /* Unpack the key and make sure it's in range. It's a recno offset. */
            p = unpack->data;
            /* Note that unpack->size does not reach past the end of the page. */
            ret = __wt_vunpack_uint(&p, unpack->size, &recno_offset);
            if (ret != 0)
                WT_RET_VRFY_RETVAL(session, ret,
                  "in %s page at %s, the key in cell %" PRIu32 " failed to unpack",
                  __wt_page_type_string(dsk->type), tag, cell_num - 1);
            if (recno_offset >= dsk->u.entries)
                WT_RET_VRFY_RETVAL(session, ret,
                  "in %s page at %s, out of range recno offset %" PRIu64 " in cell %" PRIu32,
                  __wt_page_type_string(dsk->type), tag, recno_offset, cell_num - 1);
        } else {
            if (unpack->type != WT_CELL_VALUE)
                WT_RET_VRFY(session,
                  "in %s page at %s, cell %" PRIu32 " should be a WT_CELL_VALUE but is %s",
                  __wt_page_type_string(dsk->type), tag, cell_num - 1,
                  __wt_cell_type_string(unpack->type));
            if (unpack->size != 0)
                WT_RET_VRFY(session,
                  "in %s page at %s, cell %" PRIu32 " should be empty but has size %" PRIu32,
                  __wt_page_type_string(dsk->type), tag, cell_num - 1, unpack->size);

            /*
             * Empty validity windows should not result in on-disk cells. Note that because we used
             * the safe unpack, the time window won't have been cleared even if out of date, so we
             * won't get spurious failures from that situation.
             */
            if (WT_TIME_WINDOW_IS_EMPTY(&unpack->tw))
                WT_RET_VRFY(session, "in %s page at %s, cell %" PRIu32 " has an empty time window",
                  __wt_page_type_string(dsk->type), tag, cell_num - 1);

            /* Check the validity window. */
            WT_RET(__verify_dsk_value_validity(session, unpack, cell_num, addr, tag));
        }
    }

    if (cell_num != 2 * auxhdr.entries) {
        WT_RET_VRFY(session,
          "in %s page at %s, the header said to expect %" PRIu32 " cells but only saw %" PRIu32,
          __wt_page_type_string(dsk->type), tag, 2 * auxhdr.entries, cell_num);
    }

    WT_RET(__verify_dsk_memsize(session, tag, dsk, cell));

    return (0);
}

/*
 * __verify_dsk_col_var --
 *     Walk a WT_PAGE_COL_VAR disk page and verify it.
 */
static int
__verify_dsk_col_var(
  WT_SESSION_IMPL *session, const char *tag, const WT_PAGE_HEADER *dsk, WT_ADDR *addr)
{
    struct {
        const void *data;
        size_t size;
        WT_TIME_WINDOW tw;
        bool deleted;
    } last;
    WT_BM *bm;
    WT_BTREE *btree;
    WT_CELL *cell;
    WT_CELL_UNPACK_KV *unpack, _unpack;
    WT_DECL_RET;
    uint32_t cell_num, cell_type, i;
    uint8_t *end;

    btree = S2BT(session);
    bm = btree->bm;
    unpack = &_unpack;
    end = (uint8_t *)dsk + dsk->mem_size;

    last.data = NULL;
    last.size = 0;
    WT_TIME_WINDOW_INIT(&last.tw);
    last.deleted = false;

    cell_num = 0;
    WT_CELL_FOREACH_VRFY (session, dsk, cell, unpack, i) {
        ++cell_num;

        /* Carefully unpack the cell. */
        ret = __wt_cell_unpack_safe(session, dsk, cell, NULL, unpack, end);
        if (ret != 0)
            return (__err_cell_corrupt(session, ret, cell_num, tag));

        /* Check the raw and collapsed cell types. */
        WT_RET(__err_cell_type(session, cell_num, tag, unpack->raw, dsk->type));
        WT_RET(__err_cell_type(session, cell_num, tag, unpack->type, dsk->type));
        cell_type = unpack->type;

        /* Check the validity window. */
        WT_RET(__verify_dsk_value_validity(session, unpack, cell_num, addr, tag));

        /* Check if any referenced item is entirely in the file. */
        if (cell_type == WT_CELL_VALUE_OVFL) {
            ret = bm->addr_invalid(bm, session, unpack->data, unpack->size);
            WT_RET_ERROR_OK(ret, EINVAL);
            if (ret == EINVAL)
                return (__err_cell_corrupt_or_eof(session, ret, cell_num, tag));
        }

        /*
         * Compare the last two items and see if reconciliation missed a chance for RLE encoding.
         * The time windows must match and we otherwise don't have to care about data encoding, a
         * byte comparison is enough.
         */
        if (!WT_TIME_WINDOWS_EQUAL(&unpack->tw, &last.tw))
            ;
        else if (last.deleted) {
            if (cell_type == WT_CELL_DEL)
                goto match_err;
        } else if (cell_type == WT_CELL_VALUE && last.data != NULL && last.size == unpack->size &&
          memcmp(last.data, unpack->data, last.size) == 0)
match_err:
            WT_RET_VRFY(session,
              "data entries %" PRIu32 " and %" PRIu32
              " on page at %s are identical and should have been run-length encoded",
              cell_num - 1, cell_num, tag);

        WT_TIME_WINDOW_COPY(&last.tw, &unpack->tw);
        switch (cell_type) {
        case WT_CELL_DEL:
            last.data = NULL;
            last.deleted = true;
            break;
        case WT_CELL_VALUE_OVFL:
            last.data = NULL;
            last.deleted = false;
            break;
        case WT_CELL_VALUE:
            last.data = unpack->data;
            last.size = unpack->size;
            last.deleted = false;
            break;
        }
    }
    WT_RET(__verify_dsk_memsize(session, tag, dsk, cell));

    return (0);
}

/*
 * __verify_dsk_memsize --
 *     Verify the last cell on the page matches the page's memory size.
 */
static int
__verify_dsk_memsize(
  WT_SESSION_IMPL *session, const char *tag, const WT_PAGE_HEADER *dsk, WT_CELL *cell)
{
    size_t len;

    /*
     * We use the fact that cells exactly fill a page to detect the case of a row-store leaf page
     * where the last cell is a key (that is, there's no subsequent value cell). Check for any page
     * type containing cells.
     */
    len = WT_PTRDIFF((uint8_t *)dsk + dsk->mem_size, cell);
    if (len == 0)
        return (0);
    WT_RET_VRFY(session,
      "%s page at %s has %" WT_SIZET_FMT " unexpected bytes of data after the last cell",
      __wt_page_type_string(dsk->type), tag, len);
}

/*
 * __verify_dsk_chunk --
 *     Verify a Chunk O' Data on a Btree page.
 */
static int
__verify_dsk_chunk(
  WT_SESSION_IMPL *session, const char *tag, const WT_PAGE_HEADER *dsk, uint32_t datalen)
{
    WT_BTREE *btree;
    uint8_t *p, *end;

    btree = S2BT(session);
    end = (uint8_t *)dsk + dsk->mem_size;

    /*
     * Fixed-length column-store and overflow pages are simple chunks of data-> Verify the data
     * doesn't overflow the end of the page.
     */
    p = WT_PAGE_HEADER_BYTE(btree, dsk);
    if (p + datalen > end)
        WT_RET_VRFY(session, "data on page at %s extends past the end of the page", tag);

    /* Any bytes after the data chunk should be nul bytes. */
    for (p += datalen; p < end; ++p)
        if (*p != '\0')
            WT_RET_VRFY(session, "%s page at %s has non-zero trailing bytes",
              __wt_page_type_string(dsk->type), tag);

    return (0);
}

/*
 * __err_cell_corrupt --
 *     Generic corrupted cell, we couldn't read it.
 */
static int
__err_cell_corrupt(WT_SESSION_IMPL *session, int retval, uint32_t entry_num, const char *tag)
{
    WT_RET_VRFY_RETVAL(
      session, retval, "item %" PRIu32 " on page at %s is a corrupted cell", entry_num, tag);
}

/*
 * __err_cell_corrupt_or_eof --
 *     Generic corrupted cell or item references non-existent file pages error.
 */
static int
__err_cell_corrupt_or_eof(WT_SESSION_IMPL *session, int retval, uint32_t entry_num, const char *tag)
{
    WT_RET_VRFY_RETVAL(session, retval,
      "item %" PRIu32 " on page at %s is a corrupted cell or references non-existent file pages",
      entry_num, tag);
}

/*
 * __wt_cell_type_check --
 *     Check the cell type against the page type.
 */
bool
__wt_cell_type_check(uint8_t cell_type, uint8_t dsk_type)
{
    switch (cell_type) {
    case WT_CELL_ADDR_DEL:
    case WT_CELL_ADDR_INT:
    case WT_CELL_ADDR_LEAF:
    case WT_CELL_ADDR_LEAF_NO:
        if (dsk_type == WT_PAGE_COL_INT || dsk_type == WT_PAGE_ROW_INT)
            return (true);
        break;
    case WT_CELL_DEL:
        if (dsk_type == WT_PAGE_COL_VAR)
            return (true);
        break;
    case WT_CELL_KEY_SHORT:
        if (dsk_type == WT_PAGE_COL_FIX)
            return (true);
        /* FALLTHROUGH */
    case WT_CELL_KEY:
    case WT_CELL_KEY_OVFL:
        if (dsk_type == WT_PAGE_ROW_INT || dsk_type == WT_PAGE_ROW_LEAF)
            return (true);
        break;
    case WT_CELL_KEY_PFX:
    case WT_CELL_KEY_SHORT_PFX:
        if (dsk_type == WT_PAGE_ROW_LEAF)
            return (true);
        break;
    case WT_CELL_KEY_OVFL_RM:
    case WT_CELL_VALUE_OVFL_RM:
        /*
         * Removed overflow cells are in-memory only, it's an error to ever see one on a disk page.
         */
        break;
    case WT_CELL_VALUE:
        if (dsk_type == WT_PAGE_COL_FIX)
            return (true);
        /* FALLTHROUGH */
    case WT_CELL_VALUE_COPY:
    case WT_CELL_VALUE_OVFL:
    case WT_CELL_VALUE_SHORT:
        if (dsk_type == WT_PAGE_COL_VAR || dsk_type == WT_PAGE_ROW_LEAF)
            return (true);
        break;
    }
    return (false);
}

/*
 * __err_cell_type --
 *     Generic illegal cell type for a particular page type error.
 */
static int
__err_cell_type(WT_SESSION_IMPL *session, uint32_t entry_num, const char *tag, uint8_t cell_type,
  uint8_t dsk_type)
{
    if (!__wt_cell_type_check(cell_type, dsk_type))
        WT_RET_VRFY(session,
          "illegal cell and page type combination: cell %" PRIu32
          " on page at %s is a %s cell on a %s page",
          entry_num, tag, __wt_cell_type_string(cell_type), __wt_page_type_string(dsk_type));
    return (0);
}
