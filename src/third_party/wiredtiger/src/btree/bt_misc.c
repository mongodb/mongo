/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_addr_string --
 *     Load a buffer with a printable, nul-terminated representation of an address.
 */
const char *
__wt_addr_string(WT_SESSION_IMPL *session, const uint8_t *addr, size_t addr_size, WT_ITEM *buf)
{
    WT_BM *bm;
    WT_BTREE *btree;

    btree = S2BT_SAFE(session);

    WT_ASSERT(session, buf != NULL);

    if (addr == NULL || addr_size == 0) {
        buf->data = WT_NO_ADDR_STRING;
        buf->size = strlen(WT_NO_ADDR_STRING);
    } else if (btree == NULL || (bm = btree->bm) == NULL ||
      bm->addr_string(bm, session, buf, addr, addr_size) != 0) {
        buf->data = WT_ERR_STRING;
        buf->size = strlen(WT_ERR_STRING);
    }
    return (buf->data);
}

/*
 * __wt_cell_type_string --
 *     Return a string representing the cell type.
 */
const char *
__wt_cell_type_string(uint8_t type)
{
    switch (type) {
    case WT_CELL_ADDR_DEL:
        return ("addr/del");
    case WT_CELL_ADDR_INT:
        return ("addr/int");
    case WT_CELL_ADDR_LEAF:
        return ("addr/leaf");
    case WT_CELL_ADDR_LEAF_NO:
        return ("addr/leaf-no");
    case WT_CELL_DEL:
        return ("deleted");
    case WT_CELL_KEY:
        return ("key");
    case WT_CELL_KEY_PFX:
        return ("key/pfx");
    case WT_CELL_KEY_OVFL:
        return ("key/ovfl");
    case WT_CELL_KEY_SHORT:
        return ("key/short");
    case WT_CELL_KEY_SHORT_PFX:
        return ("key/short,pfx");
    case WT_CELL_KEY_OVFL_RM:
        return ("key/ovfl,rm");
    case WT_CELL_VALUE:
        return ("value");
    case WT_CELL_VALUE_COPY:
        return ("value/copy");
    case WT_CELL_VALUE_OVFL:
        return ("value/ovfl");
    case WT_CELL_VALUE_OVFL_RM:
        return ("value/ovfl,rm");
    case WT_CELL_VALUE_SHORT:
        return ("value/short");
    default:
        return ("unknown");
    }
    /* NOTREACHED */
}

/*
 * __wt_key_string --
 *     Load a buffer with a printable, nul-terminated representation of a key.
 */
const char *
__wt_key_string(
  WT_SESSION_IMPL *session, const void *data_arg, size_t size, const char *key_format, WT_ITEM *buf)
{
    WT_ITEM tmp;

#ifdef HAVE_DIAGNOSTIC
    if (session->dump_raw)
        return (__wt_buf_set_printable(session, data_arg, size, false, buf));
#endif

    /*
     * If the format is 'S', it's a string and our version of it may not yet be nul-terminated.
     */
    if (WT_STREQ(key_format, "S") && ((char *)data_arg)[size - 1] != '\0') {
        WT_CLEAR(tmp);
        if (__wt_buf_fmt(session, &tmp, "%.*s", (int)size, (char *)data_arg) == 0) {
            data_arg = tmp.data;
            size = tmp.size + 1;
        } else {
            data_arg = WT_ERR_STRING;
            size = sizeof(WT_ERR_STRING);
        }
    }
    return (__wt_buf_set_printable_format(session, data_arg, size, key_format, false, buf));
}

/*
 * __wt_page_type_string --
 *     Return a string representing the page type.
 */
const char *
__wt_page_type_string(u_int type) WT_GCC_FUNC_ATTRIBUTE((visibility("default")))
{
    switch (type) {
    case WT_PAGE_INVALID:
        return ("invalid");
    case WT_PAGE_BLOCK_MANAGER:
        return ("block manager");
    case WT_PAGE_COL_FIX:
        return ("column-store fixed-length leaf");
    case WT_PAGE_COL_INT:
        return ("column-store internal");
    case WT_PAGE_COL_VAR:
        return ("column-store variable-length leaf");
    case WT_PAGE_OVFL:
        return ("overflow");
    case WT_PAGE_ROW_INT:
        return ("row-store internal");
    case WT_PAGE_ROW_LEAF:
        return ("row-store leaf");
    default:
        return ("unknown");
    }
    /* NOTREACHED */
}
