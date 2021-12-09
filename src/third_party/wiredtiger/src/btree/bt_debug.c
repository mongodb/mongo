/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

#ifdef HAVE_DIAGNOSTIC
/*
 * We pass around a session handle and output information, group it together.
 */
typedef struct __wt_dbg WT_DBG;
struct __wt_dbg {
    WT_CURSOR *hs_cursor;
    WT_SESSION_IMPL *session; /* Enclosing session */

    WT_ITEM *key;

    WT_ITEM *hs_key; /* History store lookups */
    WT_ITEM *hs_value;

    /*
     * When using the standard event handlers, the debugging output has to do its own message
     * handling because its output isn't line-oriented.
     */
    FILE *fp;     /* Optional file handle */
    WT_ITEM *msg; /* Buffered message */

    int (*f)(WT_DBG *, const char *, ...) /* Function to write */
      WT_GCC_FUNC_DECL_ATTRIBUTE((format(printf, 2, 3)));

    const char *key_format;
    const char *value_format;

    WT_ITEM *t1, *t2; /* Temporary space */
};

static const /* Output separator */
  char *const sep = "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=\n";

static int __debug_col_skip(WT_DBG *, WT_INSERT_HEAD *, const char *, bool, WT_CURSOR *);
static int __debug_config(WT_SESSION_IMPL *, WT_DBG *, const char *);
static int __debug_modify(WT_DBG *, const uint8_t *);
static int __debug_page(WT_DBG *, WT_REF *, uint32_t);
static int __debug_page_col_fix(WT_DBG *, WT_REF *);
static int __debug_page_col_int(WT_DBG *, WT_PAGE *, uint32_t);
static int __debug_page_col_var(WT_DBG *, WT_REF *);
static int __debug_page_metadata(WT_DBG *, WT_REF *);
static int __debug_page_row_int(WT_DBG *, WT_PAGE *, uint32_t);
static int __debug_page_row_leaf(WT_DBG *, WT_PAGE *);
static int __debug_ref(WT_DBG *, WT_REF *);
static int __debug_row_skip(WT_DBG *, WT_INSERT_HEAD *);
static int __debug_tree(WT_SESSION_IMPL *, WT_REF *, const char *, uint32_t);
static int __debug_update(WT_DBG *, WT_UPDATE *, bool);
static int __debug_wrapup(WT_DBG *);

/*
 * __wt_debug_set_verbose --
 *     Set verbose flags from the debugger.
 */
int
__wt_debug_set_verbose(WT_SESSION_IMPL *session, const char *v)
{
    char buf[256];
    const char *cfg[2] = {NULL, NULL};

    WT_RET(__wt_snprintf(buf, sizeof(buf), "verbose=[%s]", v));
    cfg[0] = buf;
    return (__wt_verbose_config(session, cfg));
}

/*
 * __debug_hex_byte --
 *     Output a single byte in hex.
 */
static inline int
__debug_hex_byte(WT_DBG *ds, uint8_t v)
{
    return (ds->f(ds, "#%c%c", __wt_hex((v & 0xf0) >> 4), __wt_hex(v & 0x0f)));
}

/*
 * __debug_bytes --
 *     Dump a single set of bytes.
 */
static int
__debug_bytes(WT_DBG *ds, const void *data_arg, size_t size)
{
    size_t i;
    const uint8_t *data;
    u_char ch;

    for (data = data_arg, i = 0; i < size; ++i, ++data) {
        ch = data[0];
        if (__wt_isprint(ch))
            WT_RET(ds->f(ds, "%c", (int)ch));
        else
            WT_RET(__debug_hex_byte(ds, data[0]));
    }
    return (0);
}

/*
 * __debug_item --
 *     Dump a single data/size item, with an optional tag.
 */
static int
__debug_item(WT_DBG *ds, const char *tag, const void *data_arg, size_t size)
{
    WT_RET(ds->f(ds, "\t%s%s{", tag == NULL ? "" : tag, tag == NULL ? "" : " "));
    WT_RET(__debug_bytes(ds, data_arg, size));
    WT_RET(ds->f(ds, "}\n"));
    return (0);
}

/*
 * __debug_item_key --
 *     Dump a single data/size key item, with an optional tag.
 */
static int
__debug_item_key(WT_DBG *ds, const char *tag, const void *data_arg, size_t size)
{
    WT_SESSION_IMPL *session;

    session = ds->session;

    return (ds->f(ds, "\t%s%s{%s}\n", tag == NULL ? "" : tag, tag == NULL ? "" : " ",
      __wt_key_string(session, data_arg, size, ds->key_format, ds->t1)));
}

/*
 * __debug_item_value --
 *     Dump a single data/size value item, with an optional tag.
 */
static int
__debug_item_value(WT_DBG *ds, const char *tag, const void *data_arg, size_t size)
{
    WT_SESSION_IMPL *session;

    session = ds->session;

    if (size == 0)
        return (ds->f(ds, "\t%s%s{}\n", tag == NULL ? "" : tag, tag == NULL ? "" : " "));

    if (session->dump_raw)
        return (ds->f(ds, "\t%s%s{%s}\n", tag == NULL ? "" : tag, tag == NULL ? "" : " ",
          __wt_buf_set_printable(session, data_arg, size, false, ds->t1)));

    /*
     * If the format is 'S', it's a string and our version of it may not yet be nul-terminated.
     */
    if (WT_STREQ(ds->value_format, "S") && ((char *)data_arg)[size - 1] != '\0') {
        WT_RET(__wt_buf_fmt(session, ds->t2, "%.*s", (int)size, (char *)data_arg));
        data_arg = ds->t2->data;
        size = ds->t2->size + 1;
    }
    return (ds->f(ds, "\t%s%s{%s}\n", tag == NULL ? "" : tag, tag == NULL ? "" : " ",
      __wt_buf_set_printable_format(session, data_arg, size, ds->value_format, false, ds->t1)));
}

/*
 * __dmsg_event --
 *     Send a debug message to the event handler.
 */
static int
__dmsg_event(WT_DBG *ds, const char *fmt, ...)
{
    WT_DECL_RET;
    WT_ITEM *msg;
    WT_SESSION_IMPL *session;
    size_t len, space;
    char *p;
    va_list ap;

    session = ds->session;

    /*
     * Debug output chunks are not necessarily terminated with a newline character. It's easy if
     * we're dumping to a stream, but if we're dumping to an event handler, which is line-oriented,
     * we must buffer the output chunk, and pass it to the event handler once we see a terminating
     * newline.
     */
    msg = ds->msg;
    for (;;) {
        p = (char *)msg->mem + msg->size;
        space = msg->memsize - msg->size;
        va_start(ap, fmt);
        ret = __wt_vsnprintf_len_set(p, space, &len, fmt, ap);
        va_end(ap);
        WT_RET(ret);

        /* Check if there was enough space. */
        if (len < space) {
            msg->size += len;
            break;
        }

        /*
         * There's not much to do on error without checking for an error return on every single
         * printf. Anyway, it's pretty unlikely and this is debugging output, I'm not going to worry
         * about it.
         */
        WT_RET(__wt_buf_grow(session, msg, msg->memsize + len + 128));
    }
    if (((uint8_t *)msg->mem)[msg->size - 1] == '\n') {
        ((uint8_t *)msg->mem)[msg->size - 1] = '\0';
        WT_RET(__wt_msg(session, "%s", (char *)msg->mem));
        msg->size = 0;
    }

    return (0);
}

/*
 * __dmsg_file --
 *     Send a debug message to a file.
 */
static int
__dmsg_file(WT_DBG *ds, const char *fmt, ...)
{
    WT_DECL_RET;
    va_list ap;

    va_start(ap, fmt);
    ret = vfprintf(ds->fp, fmt, ap) < 0 ? EIO : 0;
    va_end(ap);

    return (ret);
}

/*
 * __debug_config --
 *     Configure debugging output.
 */
static int
__debug_config(WT_SESSION_IMPL *session, WT_DBG *ds, const char *ofile)
{
    WT_BTREE *btree;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;

    memset(ds, 0, sizeof(WT_DBG));

    ds->session = session;
    ds->hs_cursor = NULL;
    conn = S2C(session);

    WT_ERR(__wt_scr_alloc(session, 512, &ds->t1));
    WT_ERR(__wt_scr_alloc(session, 512, &ds->t2));

    /*
     * Set up history store support, opening a history store cursor on demand. Return error if that
     * doesn't work, except while running in-memory configuration.
     */
    if (!F_ISSET(conn, WT_CONN_IN_MEMORY) && !WT_IS_HS(session->dhandle))
        WT_ERR(__wt_curhs_open(session, NULL, &ds->hs_cursor));

    if (ds->hs_cursor != NULL) {
        F_SET(ds->hs_cursor, WT_CURSTD_HS_READ_COMMITTED);
        WT_ERR(__wt_scr_alloc(session, 0, &ds->hs_key));
        WT_ERR(__wt_scr_alloc(session, 0, &ds->hs_value));
    }
    /*
     * If we weren't given a file, we use the default event handler, and we'll have to buffer
     * messages.
     */
    if (ofile == NULL) {
        WT_ERR(__wt_scr_alloc(session, 512, &ds->msg));
        ds->f = __dmsg_event;
    } else {
        if ((ds->fp = fopen(ofile, "w")) == NULL)
            WT_ERR(__wt_set_return(session, EIO));
        __wt_stream_set_line_buffer(ds->fp);
        ds->f = __dmsg_file;
    }

    btree = S2BT(session);
    ds->key_format = btree->key_format;
    ds->value_format = btree->value_format;
    return (0);

err:
    WT_TRET(__debug_wrapup(ds));
    return (ret);
}

/*
 * __debug_wrapup --
 *     Flush any remaining output, release resources.
 */
static int
__debug_wrapup(WT_DBG *ds)
{
    WT_DECL_RET;
    WT_ITEM *msg;
    WT_SESSION_IMPL *session;

    session = ds->session;
    msg = ds->msg;

    __wt_scr_free(session, &ds->key);
    __wt_scr_free(session, &ds->hs_key);
    __wt_scr_free(session, &ds->hs_value);
    __wt_scr_free(session, &ds->t1);
    __wt_scr_free(session, &ds->t2);

    if (ds->hs_cursor != NULL)
        WT_TRET(ds->hs_cursor->close(ds->hs_cursor));

    /*
     * Discard the buffer -- it shouldn't have anything in it, but might as well be cautious.
     */
    if (msg != NULL) {
        if (msg->size != 0)
            ret = __wt_msg(session, "%s", (char *)msg->mem);
        __wt_scr_free(session, &ds->msg);
    }

    /* Close any file we opened. */
    if (ds->fp != NULL)
        (void)fclose(ds->fp);

    return (ret);
}

/*
 * __wt_debug_addr_print --
 *     Print out an address.
 */
int
__wt_debug_addr_print(WT_SESSION_IMPL *session, const uint8_t *addr, size_t addr_size)
{
    WT_DECL_ITEM(buf);
    WT_DECL_RET;

    WT_RET(__wt_scr_alloc(session, 128, &buf));
    ret = __wt_fprintf(
      session, WT_STDERR(session), "%s\n", __wt_addr_string(session, addr, addr_size, buf));
    __wt_scr_free(session, &buf);

    return (ret);
}

/*
 * __wt_debug_addr --
 *     Read and dump a disk page in debugging mode, using an addr/size pair.
 */
int
__wt_debug_addr(WT_SESSION_IMPL *session, const uint8_t *addr, size_t addr_size, const char *ofile)
{
    WT_BM *bm;
    WT_DECL_ITEM(buf);
    WT_DECL_RET;

    bm = S2BT(session)->bm;

    WT_RET(__wt_scr_alloc(session, 1024, &buf));
    WT_ERR(bm->read(bm, session, buf, addr, addr_size));
    ret = __wt_debug_disk(session, buf->mem, ofile);

err:
    __wt_scr_free(session, &buf);
    return (ret);
}

/*
 * __wt_debug_offset_blind --
 *     Read and dump a disk page in debugging mode, using a file offset.
 */
int
__wt_debug_offset_blind(WT_SESSION_IMPL *session, wt_off_t offset, const char *ofile)
{
    uint32_t checksum, size;

    WT_ASSERT(session, S2BT_SAFE(session) != NULL);

    /*
     * This routine depends on the default block manager's view of files, where an address consists
     * of a file offset, length, and checksum. This is for debugging only.
     */
    WT_RET(__wt_block_read_off_blind(session, S2BT(session)->bm->block, offset, &size, &checksum));
    return (__wt_debug_offset(session, offset, size, checksum, ofile));
}

/*
 * __wt_debug_offset --
 *     Read and dump a disk page in debugging mode, using a file offset/size/checksum triplet.
 */
int
__wt_debug_offset(
  WT_SESSION_IMPL *session, wt_off_t offset, uint32_t size, uint32_t checksum, const char *ofile)
{
    WT_BLOCK *block;
    WT_DECL_ITEM(buf);
    WT_DECL_RET;
    uint8_t addr[WT_BTREE_MAX_ADDR_COOKIE], *endp;

    WT_ASSERT(session, S2BT_SAFE(session) != NULL);

    /*
     * This routine depends on the default block manager's view of files, where an address consists
     * of a file ID, file offset, length, and checksum. This is only for debugging, other block
     * managers might not describe underlying objects the same way, that's why there's no block
     * manager method.
     *
     * Convert the triplet into an address structure.
     */
    block = S2BT(session)->bm->block;
    endp = addr;
    WT_RET(__wt_block_addr_pack(block, &endp, block->objectid, offset, size, checksum));

    /*
     * Read the address through the btree I/O functions (so the block is decompressed and/or
     * unencrypted as necessary).
     */
    WT_RET(__wt_scr_alloc(session, 0, &buf));
    WT_ERR(__wt_bt_read(session, buf, addr, WT_PTRDIFF(endp, addr)));
    ret = __wt_debug_disk(session, buf->mem, ofile);

err:
    __wt_scr_free(session, &buf);
    return (ret);
}

/*
 * __debug_hs_cursor --
 *     Dump information pointed to by a single history store cursor.
 */
static int
__debug_hs_cursor(WT_DBG *ds, WT_CURSOR *hs_cursor)
{
    WT_CURSOR_BTREE *cbt;
    WT_SESSION_IMPL *session;
    WT_TIME_WINDOW tw;
    uint64_t hs_counter, hs_upd_type;
    uint32_t hs_btree_id;
    char time_string[WT_TIME_STRING_SIZE];

    cbt = __wt_curhs_get_cbt(hs_cursor);
    session = ds->session;

    WT_TIME_WINDOW_INIT(&tw);

    WT_RET(hs_cursor->get_key(hs_cursor, &hs_btree_id, ds->hs_key, &tw.start_ts, &hs_counter));
    WT_RET(hs_cursor->get_value(
      hs_cursor, &tw.stop_ts, &tw.durable_start_ts, &hs_upd_type, ds->hs_value));

    switch (hs_upd_type) {
    case WT_UPDATE_MODIFY:
        WT_RET(ds->f(ds,
          "\t"
          "hs-modify: %s\n",
          __wt_time_window_to_string(&cbt->upd_value->tw, time_string)));
        WT_RET(ds->f(ds, "\tV "));
        WT_RET(__debug_modify(ds, ds->hs_value->data));
        WT_RET(ds->f(ds, "\n"));
        break;
    case WT_UPDATE_STANDARD:
        WT_RET(ds->f(ds,
          "\t"
          "hs-update: %s\n",
          __wt_time_window_to_string(&cbt->upd_value->tw, time_string)));
        WT_RET(__debug_item_value(ds, "V", ds->hs_value->data, ds->hs_value->size));
        break;
    default:
        /*
         * Currently, we expect only modifies or full values to be exposed by hs_cursors. This means
         * we can ignore other types for now.
         */
        WT_ASSERT(session, hs_upd_type == WT_UPDATE_MODIFY || hs_upd_type == WT_UPDATE_STANDARD);
        break;
    }
    return (0);
}

/*
 * __debug_hs_key --
 *     Dump any HS records associated with the key.
 */
static int
__debug_hs_key(WT_DBG *ds)
{
    WT_BTREE *btree;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    uint32_t hs_btree_id;

    session = ds->session;
    btree = S2BT(session);
    hs_btree_id = btree->id;

    /*
     * Open a history store cursor positioned at the end of the data store key (the newest record)
     * and iterate backwards until we reach a different key or btree.
     */
    ds->hs_cursor->set_key(ds->hs_cursor, 4, hs_btree_id, ds->key, WT_TS_MAX, WT_TXN_MAX);
    ret = __wt_curhs_search_near_before(session, ds->hs_cursor);

    for (; ret == 0; ret = ds->hs_cursor->prev(ds->hs_cursor))
        WT_RET(__debug_hs_cursor(ds, ds->hs_cursor));

    return (ret == WT_NOTFOUND ? 0 : ret);
}

/*
 * __debug_cell_int_data --
 *     Dump a single WT_COL_INT or WT_ROW_INT disk image cell's data in debugging mode.
 */
static int
__debug_cell_int_data(WT_DBG *ds, WT_CELL_UNPACK_ADDR *unpack)
{
    const char *p;

    switch (unpack->raw) {
    case WT_CELL_ADDR_DEL:
    case WT_CELL_ADDR_INT:
    case WT_CELL_ADDR_LEAF:
    case WT_CELL_ADDR_LEAF_NO:
        p = __wt_cell_type_string(unpack->raw);
        return (__debug_item(ds, NULL, p, strlen(p)));
    }
    return (0);
}

/*
 * __debug_cell_int --
 *     Dump a single unpacked WT_COL_INT or WT_ROW_INT disk image WT_CELL.
 */
static int
__debug_cell_int(WT_DBG *ds, const WT_PAGE_HEADER *dsk, WT_CELL_UNPACK_ADDR *unpack)
{
    WT_DECL_ITEM(buf);
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    char time_string[WT_TIME_STRING_SIZE];

    session = ds->session;

    WT_RET(ds->f(ds, "\t%s: len %" PRIu32, __wt_cell_type_string(unpack->raw), unpack->size));

    /* Dump the cell's per-disk page type information. */
    switch (dsk->type) {
    case WT_PAGE_COL_INT:
        WT_RET(ds->f(ds, ", recno: %" PRIu64, unpack->v));
        break;
    }

    /* Dump timestamps and addresses. */
    switch (unpack->raw) {
    case WT_CELL_ADDR_DEL:
    case WT_CELL_ADDR_INT:
    case WT_CELL_ADDR_LEAF:
    case WT_CELL_ADDR_LEAF_NO:
        if (!WT_TIME_AGGREGATE_IS_EMPTY(&unpack->ta))
            WT_RET(ds->f(ds, ", %s", __wt_time_aggregate_to_string(&unpack->ta, time_string)));

        WT_RET(__wt_scr_alloc(session, 128, &buf));
        ret = ds->f(ds, ", %s", __wt_addr_string(session, unpack->data, unpack->size, buf));
        __wt_scr_free(session, &buf);
        WT_RET(ret);
        break;
    }
    WT_RET(ds->f(ds, "\n"));

    return (__debug_cell_int_data(ds, unpack));
}

/*
 * __debug_dsk_int --
 *     Dump a WT_COL_INT or WT_ROW_INT disk image.
 */
static int
__debug_dsk_int(WT_DBG *ds, const WT_PAGE_HEADER *dsk)
{
    WT_CELL_UNPACK_ADDR unpack;

    WT_CELL_FOREACH_ADDR (ds->session, dsk, unpack) {
        WT_RET(__debug_cell_int(ds, dsk, &unpack));
    }
    WT_CELL_FOREACH_END;
    return (0);
}

/*
 * __debug_cell_kv --
 *     Dump a single unpacked WT_COL_VAR or WT_ROW_LEAF disk image WT_CELL.
 */
static int
__debug_cell_kv(
  WT_DBG *ds, WT_PAGE *page, int page_type, const char *tag, WT_CELL_UNPACK_KV *unpack)
{
    WT_SESSION_IMPL *session;
    char time_string[WT_TIME_STRING_SIZE];
    const char *p;

    session = ds->session;

    /* Column-store references to deleted cells return a NULL cell reference. */
    if (unpack == NULL)
        return (__debug_item(ds, tag, "deleted", strlen("deleted")));

    /* Row-store references to empty cells return a NULL on-page reference. */
    if (unpack->cell == NULL)
        return (__debug_item(ds, tag, "zero-length", strlen("zero-length")));

    WT_RET(ds->f(ds, "\t%s: len %" PRIu32, __wt_cell_type_string(unpack->raw), unpack->size));

    /* Dump per-disk page type information. */
    switch (page_type) {
    case WT_PAGE_COL_FIX:
        break;
    case WT_PAGE_COL_VAR:
        WT_RET(ds->f(ds, ", rle: %" PRIu64, __wt_cell_rle(unpack)));
        break;
    case WT_PAGE_ROW_LEAF:
        switch (unpack->raw) {
        case WT_CELL_KEY_PFX:
        case WT_CELL_KEY_SHORT_PFX:
            WT_RET(ds->f(ds, ", pfx: %" PRIu8, unpack->prefix));
            break;
        }
        break;
    }

    /* Dump time window. */
    switch (unpack->raw) {
    case WT_CELL_DEL:
    case WT_CELL_VALUE:
    case WT_CELL_VALUE_COPY:
    case WT_CELL_VALUE_OVFL:
    case WT_CELL_VALUE_OVFL_RM:
    case WT_CELL_VALUE_SHORT:
        if (!WT_TIME_WINDOW_IS_EMPTY(&unpack->tw))
            WT_RET(ds->f(ds, ", %s", __wt_time_window_to_string(&unpack->tw, time_string)));
        break;
    }

    /* Column-store deleted cells. */
    switch (unpack->raw) {
    case WT_CELL_DEL:
        p = __wt_cell_type_string(unpack->raw);
        return (__debug_item(ds, tag, p, strlen(p)));
    }

    /* Overflow addresses. */
    switch (unpack->raw) {
    case WT_CELL_KEY_OVFL:
    case WT_CELL_VALUE_OVFL:
        WT_RET(ds->f(ds, ", %s", __wt_addr_string(session, unpack->data, unpack->size, ds->t1)));
        break;
    }
    WT_RET(ds->f(ds, "\n"));

    WT_RET(page == NULL ? __wt_dsk_cell_data_ref(session, page_type, unpack, ds->t1) :
                          __wt_page_cell_data_ref(session, page, unpack, ds->t1));

    /* Standard key/value cells. */
    switch (unpack->raw) {
    case WT_CELL_KEY:
    case WT_CELL_KEY_OVFL:
    case WT_CELL_KEY_PFX:
    case WT_CELL_KEY_SHORT:
    case WT_CELL_KEY_SHORT_PFX:
        WT_RET(__debug_item_key(ds, tag, ds->t1->data, ds->t1->size));
        break;
    case WT_CELL_VALUE:
    case WT_CELL_VALUE_COPY:
    case WT_CELL_VALUE_OVFL:
    case WT_CELL_VALUE_SHORT:
        WT_RET(__debug_item_value(ds, tag, ds->t1->data, ds->t1->size));
        break;
    }

    return (0);
}

/*
 * __debug_dsk_kv --
 *     Dump a WT_COL_VAR or WT_ROW_LEAF disk image.
 */
static int
__debug_dsk_kv(WT_DBG *ds, const WT_PAGE_HEADER *dsk)
{
    WT_CELL_UNPACK_KV unpack;

    WT_CELL_FOREACH_KV (ds->session, dsk, unpack) {
        WT_RET(__debug_cell_kv(ds, NULL, dsk->type, NULL, &unpack));
    }
    WT_CELL_FOREACH_END;
    return (0);
}

/*
 * __debug_dsk_col_fix --
 *     Dump a WT_PAGE_COL_FIX disk image.
 */
static int
__debug_dsk_col_fix(WT_DBG *ds, const WT_PAGE_HEADER *dsk)
{
    WT_BTREE *btree;
    WT_CELL_UNPACK_KV unpack;
    WT_COL_FIX_AUXILIARY_HEADER auxhdr;
    uint32_t i;
    uint8_t v;

    btree = S2BT(ds->session);

    WT_RET(__wt_col_fix_read_auxheader(ds->session, dsk, &auxhdr));

    switch (auxhdr.version) {
    case WT_COL_FIX_VERSION_NIL:
        WT_RET(ds->f(ds, "page version 0, no auxiliary data\n"));
        break;
    case WT_COL_FIX_VERSION_TS:
        WT_RET(ds->f(ds, "page version 1, %" PRIu32 " time windows\n", auxhdr.entries));
        break;
    default:
        WT_RET(ds->f(ds, "unknown page version %" PRIu32 "\n", auxhdr.version));
        break;
    }

    WT_COL_FIX_FOREACH_BITS (btree, dsk, v, i) {
        WT_RET(ds->f(ds, "\t{"));
        WT_RET(__debug_hex_byte(ds, v));
        WT_RET(ds->f(ds, "}\n"));
    }

    if (auxhdr.dataoffset > dsk->mem_size)
        /* Print something useful instead of crashing or failing. */
        WT_RET(ds->f(ds, "page is corrupt: offset to time windows is past end of page"));
    else if (auxhdr.version == WT_COL_FIX_VERSION_TS) {
        WT_CELL_FOREACH_FIX_TIMESTAMPS (ds->session, dsk, &auxhdr, unpack)
            WT_RET(__debug_cell_kv(ds, NULL, dsk->type, NULL, &unpack));
        WT_CELL_FOREACH_END;
    }

    return (0);
}

/*
 * __wt_debug_disk --
 *     Dump a disk page in debugging mode.
 */
int
__wt_debug_disk(WT_SESSION_IMPL *session, const WT_PAGE_HEADER *dsk, const char *ofile)
{
    WT_DBG *ds, _ds;
    WT_DECL_RET;

    ds = &_ds;
    WT_RET(__debug_config(session, ds, ofile));

    WT_ERR(ds->f(ds, "%s page", __wt_page_type_string(dsk->type)));
    switch (dsk->type) {
    case WT_PAGE_BLOCK_MANAGER:
        break;
    case WT_PAGE_COL_FIX:
    case WT_PAGE_COL_INT:
    case WT_PAGE_COL_VAR:
        WT_ERR(ds->f(ds, ", recno %" PRIu64, dsk->recno));
    /* FALLTHROUGH */
    case WT_PAGE_ROW_INT:
    case WT_PAGE_ROW_LEAF:
        WT_ERR(ds->f(ds, ", entries %" PRIu32, dsk->u.entries));
        break;
    case WT_PAGE_OVFL:
        WT_ERR(ds->f(ds, ", datalen %" PRIu32, dsk->u.datalen));
        break;
    default:
        WT_ERR(__wt_illegal_value(session, dsk->type));
    }

    if (F_ISSET(dsk, WT_PAGE_COMPRESSED))
        WT_ERR(ds->f(ds, ", compressed"));
    if (F_ISSET(dsk, WT_PAGE_ENCRYPTED))
        WT_ERR(ds->f(ds, ", encrypted"));
    if (F_ISSET(dsk, WT_PAGE_EMPTY_V_ALL))
        WT_ERR(ds->f(ds, ", empty-all"));
    if (F_ISSET(dsk, WT_PAGE_EMPTY_V_NONE))
        WT_ERR(ds->f(ds, ", empty-none"));

    WT_ERR(ds->f(ds, ", generation %" PRIu64 "\n", dsk->write_gen));

    switch (dsk->type) {
    case WT_PAGE_BLOCK_MANAGER:
        break;
    case WT_PAGE_COL_FIX:
        WT_ERR(__debug_dsk_col_fix(ds, dsk));
        break;
    case WT_PAGE_COL_INT:
    case WT_PAGE_ROW_INT:
        WT_ERR(__debug_dsk_int(ds, dsk));
        break;
    case WT_PAGE_COL_VAR:
    case WT_PAGE_ROW_LEAF:
        WT_ERR(__debug_dsk_kv(ds, dsk));
        break;
    default:
        break;
    }

err:
    WT_TRET(__debug_wrapup(ds));
    return (ret);
}

/*
 * __debug_tree_shape_info --
 *     Pretty-print information about a page.
 */
static char *
__debug_tree_shape_info(WT_REF *ref, char *buf, size_t len)
{
    WT_PAGE *page;
    uint64_t v;
    const char *unit;

    page = ref->page;
    v = page->memory_footprint;

    if (v > WT_GIGABYTE) {
        v /= WT_GIGABYTE;
        unit = "G";
    } else if (v > WT_MEGABYTE) {
        v /= WT_MEGABYTE;
        unit = "M";
    } else if (v > WT_KILOBYTE) {
        v /= WT_KILOBYTE;
        unit = "K";
    } else {
        unit = "B";
    }

    WT_IGNORE_RET(
      __wt_snprintf(buf, len, "(%p, %" PRIu64 "%s, evict gen %" PRIu64 ", create gen %" PRIu64 ")",
        (void *)ref, v, unit, page->evict_pass_gen, page->cache_create_gen));
    return (buf);
}

/*
 * __debug_tree_shape_worker --
 *     Dump information about the current page and descend.
 */
static int
__debug_tree_shape_worker(WT_DBG *ds, WT_REF *ref, int level)
{
    WT_REF *walk;
    WT_SESSION_IMPL *session;
    char buf[128];

    session = ds->session;

    if (F_ISSET(ref, WT_REF_FLAG_INTERNAL)) {
        WT_RET(ds->f(ds,
          "%*s"
          "I"
          "%d %s\n",
          level * 3, " ", level, __debug_tree_shape_info(ref, buf, sizeof(buf))));
        WT_INTL_FOREACH_BEGIN (session, ref->page, walk) {
            if (walk->state == WT_REF_MEM)
                WT_RET(__debug_tree_shape_worker(ds, walk, level + 1));
        }
        WT_INTL_FOREACH_END;
    } else
        WT_RET(ds->f(ds,
          "%*s"
          "L"
          " %s\n",
          level * 3, " ", __debug_tree_shape_info(ref, buf, sizeof(buf))));
    return (0);
}

/*
 * __wt_debug_tree_shape --
 *     Dump the shape of the in-memory tree.
 */
int
__wt_debug_tree_shape(WT_SESSION_IMPL *session, WT_REF *ref, const char *ofile)
{
    WT_DBG *ds, _ds;
    WT_DECL_RET;

    WT_ASSERT(session, S2BT_SAFE(session) != NULL);

    ds = &_ds;
    WT_RET(__debug_config(session, ds, ofile));

    /* A NULL WT_REF starts at the top of the tree -- it's a convenience. */
    if (ref == NULL)
        ref = &S2BT(session)->root;

    WT_WITH_PAGE_INDEX(session, ret = __debug_tree_shape_worker(ds, ref, 1));

    WT_TRET(__debug_wrapup(ds));
    return (ret);
}

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_DEBUG_TREE_LEAF 0x1u /* Debug leaf pages */
#define WT_DEBUG_TREE_WALK 0x2u /* Descend the tree */
/* AUTOMATIC FLAG VALUE GENERATION STOP 32 */

/*
 * __wt_debug_tree_all --
 *     Dump the in-memory information for a tree, including leaf pages.
 */
int
__wt_debug_tree_all(void *session_arg, WT_BTREE *btree, WT_REF *ref, const char *ofile)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    /*
     * Allow an explicit btree as an argument, as one may not yet be set on the session.
     */
    session = (WT_SESSION_IMPL *)session_arg;
    if (btree == NULL)
        btree = S2BT(session);

    WT_WITH_BTREE(session, btree,
      ret = __debug_tree(session, ref, ofile, WT_DEBUG_TREE_LEAF | WT_DEBUG_TREE_WALK));
    return (ret);
}

/*
 * __wt_debug_tree --
 *     Dump the in-memory information for a tree, not including leaf pages.
 */
int
__wt_debug_tree(void *session_arg, WT_BTREE *btree, WT_REF *ref, const char *ofile)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    /*
     * Allow an explicit btree as an argument, as one may not yet be set on the session.
     */
    session = (WT_SESSION_IMPL *)session_arg;
    if (btree == NULL)
        btree = S2BT(session);

    WT_WITH_BTREE(session, btree, ret = __debug_tree(session, ref, ofile, WT_DEBUG_TREE_WALK));
    return (ret);
}

/*
 * __wt_debug_page --
 *     Dump the in-memory information for a page.
 */
int
__wt_debug_page(void *session_arg, WT_BTREE *btree, WT_REF *ref, const char *ofile)
{
    WT_DBG *ds, _ds;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    /*
     * Allow an explicit btree as an argument, as one may not yet be set on the session.
     */
    session = (WT_SESSION_IMPL *)session_arg;
    if (btree == NULL)
        btree = S2BT(session);

    ds = &_ds;
    WT_WITH_BTREE(session, btree, ret = __debug_config(session, ds, ofile));
    WT_ERR(ret);

    WT_WITH_BTREE(session, btree, ret = __debug_page(ds, ref, WT_DEBUG_TREE_LEAF));

err:
    WT_TRET(__debug_wrapup(ds));
    return (ret);
}

/*
 * __wt_debug_cursor_page --
 *     Dump the in-memory information for a cursor-referenced page.
 */
int
__wt_debug_cursor_page(void *cursor_arg, const char *ofile)
  WT_GCC_FUNC_ATTRIBUTE((visibility("default")))
{
    WT_CURSOR_BTREE *cbt;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    cbt = cursor_arg;
    session = CUR2S(cursor_arg);

    WT_WITH_BTREE(session, CUR2BT(cbt), ret = __wt_debug_page(session, NULL, cbt->ref, ofile));
    return (ret);
}

/*
 * __wt_debug_cursor_tree_hs --
 *     Dump the history store tree given a user cursor.
 */
int
__wt_debug_cursor_tree_hs(void *session_arg, const char *ofile)
  WT_GCC_FUNC_ATTRIBUTE((visibility("default")))
{
    WT_BTREE *hs_btree;
    WT_CURSOR *hs_cursor;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    session = (WT_SESSION_IMPL *)session_arg;
    WT_RET(__wt_curhs_open(session, NULL, &hs_cursor));
    hs_btree = __wt_curhs_get_btree(hs_cursor);
    WT_WITH_BTREE(session, hs_btree, ret = __wt_debug_tree_all(session, NULL, NULL, ofile));
    WT_TRET(hs_cursor->close(hs_cursor));

    return (ret);
}

/*
 * __debug_tree --
 *     Dump the in-memory information for a tree.
 */
static int
__debug_tree(WT_SESSION_IMPL *session, WT_REF *ref, const char *ofile, uint32_t flags)
{
    WT_DBG *ds, _ds;
    WT_DECL_RET;

    ds = &_ds;
    WT_ERR(__debug_config(session, ds, ofile));

    /* A NULL page starts at the top of the tree -- it's a convenience. */
    if (ref == NULL)
        ref = &S2BT(session)->root;

    ret = __debug_page(ds, ref, flags);

err:
    WT_TRET(__debug_wrapup(ds));
    return (ret);
}

/*
 * __debug_page --
 *     Dump the in-memory information for an in-memory page.
 */
static int
__debug_page(WT_DBG *ds, WT_REF *ref, uint32_t flags)
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    session = ds->session;
    WT_RET(__wt_scr_alloc(session, 100, &ds->key));

    /* Dump the page metadata. */
    WT_WITH_PAGE_INDEX(session, ret = __debug_page_metadata(ds, ref));
    WT_ERR(ret);

    /* Dump the page. */
    switch (ref->page->type) {
    case WT_PAGE_COL_FIX:
        if (LF_ISSET(WT_DEBUG_TREE_LEAF))
            WT_ERR(__debug_page_col_fix(ds, ref));
        break;
    case WT_PAGE_COL_INT:
        WT_WITH_PAGE_INDEX(session, ret = __debug_page_col_int(ds, ref->page, flags));
        WT_ERR(ret);
        break;
    case WT_PAGE_COL_VAR:
        if (LF_ISSET(WT_DEBUG_TREE_LEAF))
            WT_ERR(__debug_page_col_var(ds, ref));
        break;
    case WT_PAGE_ROW_INT:
        WT_WITH_PAGE_INDEX(session, ret = __debug_page_row_int(ds, ref->page, flags));
        WT_ERR(ret);
        break;
    case WT_PAGE_ROW_LEAF:
        if (LF_ISSET(WT_DEBUG_TREE_LEAF))
            WT_ERR(__debug_page_row_leaf(ds, ref->page));
        break;
    default:
        WT_ERR(__wt_illegal_value(session, ref->page->type));
    }

err:
    return (ret);
}

/*
 * __debug_page_metadata --
 *     Dump an in-memory page's metadata.
 */
static int
__debug_page_metadata(WT_DBG *ds, WT_REF *ref)
{
    WT_PAGE *page;
    WT_PAGE_INDEX *pindex;
    WT_PAGE_MODIFY *mod;
    WT_SESSION_IMPL *session;
    uint64_t split_gen;
    uint32_t entries;

    page = ref->page;
    session = ds->session;
    mod = page->modify;
    split_gen = 0;

    WT_RET(ds->f(ds, "%p", (void *)ref));

    switch (page->type) {
    case WT_PAGE_COL_INT:
        WT_RET(ds->f(ds, " recno %" PRIu64, ref->ref_recno));
        WT_INTL_INDEX_GET(session, page, pindex);
        entries = pindex->entries;
        split_gen = page->pg_intl_split_gen;
        break;
    case WT_PAGE_COL_FIX:
        WT_RET(ds->f(ds, " recno %" PRIu64, ref->ref_recno));
        entries = page->entries;
        break;
    case WT_PAGE_COL_VAR:
        WT_RET(ds->f(ds, " recno %" PRIu64, ref->ref_recno));
        entries = page->entries;
        break;
    case WT_PAGE_ROW_INT:
        WT_INTL_INDEX_GET(session, page, pindex);
        entries = pindex->entries;
        split_gen = page->pg_intl_split_gen;
        break;
    case WT_PAGE_ROW_LEAF:
        entries = page->entries;
        break;
    default:
        return (__wt_illegal_value(session, page->type));
    }

    WT_RET(ds->f(ds, ": %s\n", __wt_page_type_string(page->type)));
    WT_RET(__debug_ref(ds, ref));

    WT_RET(ds->f(ds,
      "\t"
      "disk %p",
      (void *)page->dsk));
    if (page->dsk != NULL)
        WT_RET(ds->f(ds, ", dsk_mem_size %" PRIu32 ", write_gen: %" PRIu64, page->dsk->mem_size,
          page->dsk->write_gen));
    WT_RET(ds->f(ds, ", entries %" PRIu32, entries));
    WT_RET(ds->f(ds, ", %s", __wt_page_is_modified(page) ? "dirty" : "clean"));

    if (F_ISSET_ATOMIC_16(page, WT_PAGE_BUILD_KEYS))
        WT_RET(ds->f(ds, ", keys-built"));
    if (F_ISSET_ATOMIC_16(page, WT_PAGE_DISK_ALLOC))
        WT_RET(ds->f(ds, ", disk-alloc"));
    if (F_ISSET_ATOMIC_16(page, WT_PAGE_DISK_MAPPED))
        WT_RET(ds->f(ds, ", disk-mapped"));
    if (F_ISSET_ATOMIC_16(page, WT_PAGE_EVICT_LRU))
        WT_RET(ds->f(ds, ", evict-lru"));
    if (F_ISSET_ATOMIC_16(page, WT_PAGE_INTL_OVERFLOW_KEYS))
        WT_RET(ds->f(ds, ", overflow-keys"));
    if (F_ISSET_ATOMIC_16(page, WT_PAGE_SPLIT_INSERT))
        WT_RET(ds->f(ds, ", split-insert"));
    if (F_ISSET_ATOMIC_16(page, WT_PAGE_UPDATE_IGNORE))
        WT_RET(ds->f(ds, ", update-ignore"));

    if (mod != NULL)
        switch (mod->rec_result) {
        case WT_PM_REC_EMPTY:
            WT_RET(ds->f(ds, ", empty"));
            break;
        case WT_PM_REC_MULTIBLOCK:
            WT_RET(ds->f(ds, ", multiblock"));
            break;
        case WT_PM_REC_REPLACE:
            WT_RET(ds->f(ds, ", replaced"));
            break;
        case 0:
            break;
        default:
            return (__wt_illegal_value(session, mod->rec_result));
        }
    if (split_gen != 0)
        WT_RET(ds->f(ds, ", split-gen=%" PRIu64, split_gen));
    if (mod != NULL)
        WT_RET(ds->f(ds, ", page-state=%" PRIu32, mod->page_state));
    WT_RET(ds->f(ds, ", memory-size %" WT_SIZET_FMT, page->memory_footprint));
    return (ds->f(ds, "\n"));
}

/*
 * __debug_page_col_fix --
 *     Dump an in-memory WT_PAGE_COL_FIX page.
 */
static int
__debug_page_col_fix(WT_DBG *ds, WT_REF *ref)
{
    WT_BTREE *btree;
    WT_CELL *cell;
    WT_CELL_UNPACK_KV unpack;
    WT_INSERT *ins;
    WT_PAGE *page;
    const WT_PAGE_HEADER *dsk;
    WT_SESSION_IMPL *session;
    uint64_t recno;
    uint32_t curtw, i, numtws;
    uint8_t v;
    char time_string[WT_TIME_STRING_SIZE];

    WT_ASSERT(ds->session, S2BT_SAFE(ds->session) != NULL);

    session = ds->session;
    btree = S2BT(session);
    page = ref->page;
    dsk = page->dsk;
    recno = ref->ref_recno;

    if (dsk != NULL) {
        ins = WT_SKIP_FIRST(WT_COL_UPDATE_SINGLE(page));
        curtw = 0;
        numtws = WT_COL_FIX_TWS_SET(page) ? page->pg_fix_numtws : 0;

        WT_COL_FIX_FOREACH_BITS (btree, dsk, v, i) {
            WT_RET(ds->f(ds, "\t%" PRIu64 "\t{", recno));
            WT_RET(__debug_hex_byte(ds, v));
            WT_RET(ds->f(ds, "}"));
            if (curtw < numtws && recno - ref->ref_recno == page->pg_fix_tws[curtw].recno_offset) {
                cell = WT_COL_FIX_TW_CELL(page, &page->pg_fix_tws[curtw]);
                __wt_cell_unpack_kv(ds->session, page->dsk, cell, &unpack);
                if (!WT_TIME_WINDOW_IS_EMPTY(&unpack.tw))
                    WT_RET(ds->f(ds, ", %s", __wt_time_window_to_string(&unpack.tw, time_string)));
                curtw++;
            }
            WT_RET(ds->f(ds, "\n"));

            /* Check for a match on the update list. */
            if (ins != NULL && WT_INSERT_RECNO(ins) == recno) {
                WT_RET(ds->f(ds, "\tupdate %" PRIu64 "\n", WT_INSERT_RECNO(ins)));
                WT_RET(__debug_update(ds, ins->upd, true));
                ins = WT_SKIP_NEXT(ins);
            }
            ++recno;
        }
    }

    if (WT_COL_UPDATE_SINGLE(page) != NULL) {
        WT_RET(ds->f(ds, "%s", sep));
        WT_RET(__debug_col_skip(ds, WT_COL_UPDATE_SINGLE(page), "update", true, NULL));
    }
    if (WT_COL_APPEND(page) != NULL) {
        WT_RET(ds->f(ds, "%s", sep));
        WT_RET(__debug_col_skip(ds, WT_COL_APPEND(page), "append", true, NULL));
    }
    return (0);
}

/*
 * __debug_page_col_int --
 *     Dump an in-memory WT_PAGE_COL_INT page.
 */
static int
__debug_page_col_int(WT_DBG *ds, WT_PAGE *page, uint32_t flags)
{
    WT_REF *ref;
    WT_SESSION_IMPL *session;

    session = ds->session;

    WT_INTL_FOREACH_BEGIN (session, page, ref) {
        WT_RET(ds->f(ds, "\trecno %" PRIu64 "\n", ref->ref_recno));
        WT_RET(__debug_ref(ds, ref));
    }
    WT_INTL_FOREACH_END;

    if (LF_ISSET(WT_DEBUG_TREE_WALK)) {
        WT_INTL_FOREACH_BEGIN (session, page, ref) {
            if (ref->state == WT_REF_MEM) {
                WT_RET(ds->f(ds, "\n"));
                WT_RET(__debug_page(ds, ref, flags));
            }
        }
        WT_INTL_FOREACH_END;
    }
    return (0);
}

/*
 * __debug_page_col_var --
 *     Dump an in-memory WT_PAGE_COL_VAR page.
 */
static int
__debug_page_col_var(WT_DBG *ds, WT_REF *ref)
{
    WT_CELL *cell;
    WT_CELL_UNPACK_KV *unpack, _unpack;
    WT_COL *cip;
    WT_INSERT_HEAD *update;
    WT_PAGE *page;
    WT_SESSION_IMPL *session;
    uint64_t recno, rle;
    uint32_t i;
    uint8_t *p;
    char tag[64];

    unpack = &_unpack;
    page = ref->page;
    session = ds->session;
    recno = ref->ref_recno;

    WT_COL_FOREACH (page, cip, i) {
        cell = WT_COL_PTR(page, cip);
        __wt_cell_unpack_kv(ds->session, page->dsk, cell, unpack);
        rle = __wt_cell_rle(unpack);
        WT_RET(__wt_snprintf(tag, sizeof(tag), "%" PRIu64 " %" PRIu64, recno, rle));
        WT_RET(__debug_cell_kv(ds, page, WT_PAGE_COL_VAR, tag, unpack));

        if (!WT_IS_HS(session->dhandle) && ds->hs_cursor != NULL) {
            p = ds->key->mem;
            WT_RET(__wt_vpack_uint(&p, 0, recno));
            ds->key->size = WT_PTRDIFF(p, ds->key->mem);
            WT_RET(__debug_hs_key(ds));
        }

        if ((update = WT_COL_UPDATE(page, cip)) != NULL)
            WT_RET(__debug_col_skip(ds, update, "update", false, ds->hs_cursor));
        recno += rle;
    }

    if (WT_COL_APPEND(page) != NULL) {
        WT_RET(ds->f(ds, "%s", sep));
        WT_RET(__debug_col_skip(ds, WT_COL_APPEND(page), "append", false, ds->hs_cursor));
    }

    return (0);
}

/*
 * __debug_page_row_int --
 *     Dump an in-memory WT_PAGE_ROW_INT page.
 */
static int
__debug_page_row_int(WT_DBG *ds, WT_PAGE *page, uint32_t flags)
{
    WT_REF *ref;
    WT_SESSION_IMPL *session;
    size_t len;
    void *p;

    session = ds->session;

    WT_INTL_FOREACH_BEGIN (session, page, ref) {
        __wt_ref_key(page, ref, &p, &len);
        WT_RET(__debug_item_key(ds, "K", p, len));
        WT_RET(__debug_ref(ds, ref));
    }
    WT_INTL_FOREACH_END;

    if (LF_ISSET(WT_DEBUG_TREE_WALK)) {
        WT_INTL_FOREACH_BEGIN (session, page, ref) {
            if (ref->state == WT_REF_MEM) {
                WT_RET(ds->f(ds, "\n"));
                WT_RET(__debug_page(ds, ref, flags));
            }
        }
        WT_INTL_FOREACH_END;
    }
    return (0);
}

/*
 * __debug_page_row_leaf --
 *     Dump an in-memory WT_PAGE_ROW_LEAF page.
 */
static int
__debug_page_row_leaf(WT_DBG *ds, WT_PAGE *page)
{
    WT_CELL_UNPACK_KV *unpack, _unpack;
    WT_INSERT_HEAD *insert;
    WT_ROW *rip;
    WT_SESSION_IMPL *session;
    WT_UPDATE *upd;
    uint32_t i;

    session = ds->session;
    unpack = &_unpack;

    /*
     * Dump any K/V pairs inserted into the page before the first from-disk key on the page.
     */
    if ((insert = WT_ROW_INSERT_SMALLEST(page)) != NULL)
        WT_RET(__debug_row_skip(ds, insert));

    /* Dump the page's K/V pairs. */
    WT_ROW_FOREACH (page, rip, i) {
        WT_RET(__wt_row_leaf_key(session, page, rip, ds->key, false));
        WT_RET(__debug_item_key(ds, "K", ds->key->data, ds->key->size));

        __wt_row_leaf_value_cell(session, page, rip, unpack);
        WT_RET(__debug_cell_kv(ds, page, WT_PAGE_ROW_LEAF, "V", unpack));

        if ((upd = WT_ROW_UPDATE(page, rip)) != NULL)
            WT_RET(__debug_update(ds, upd, false));

        if (!WT_IS_HS(session->dhandle) && ds->hs_cursor != NULL)
            WT_RET(__debug_hs_key(ds));

        if ((insert = WT_ROW_INSERT(page, rip)) != NULL)
            WT_RET(__debug_row_skip(ds, insert));
    }
    return (0);
}

/*
 * __debug_col_skip --
 *     Dump a column-store skiplist.
 */
static int
__debug_col_skip(
  WT_DBG *ds, WT_INSERT_HEAD *head, const char *tag, bool hexbyte, WT_CURSOR *hs_cursor)
{
    WT_INSERT *ins;
    WT_SESSION_IMPL *session;
    uint8_t *p;

    session = ds->session;

    WT_SKIP_FOREACH (ins, head) {
        WT_RET(ds->f(ds, "\t%s %" PRIu64 "\n", tag, WT_INSERT_RECNO(ins)));
        WT_RET(__debug_update(ds, ins->upd, hexbyte));

        if (!WT_IS_HS(session->dhandle) && hs_cursor != NULL) {
            p = ds->key->mem;
            WT_RET(__wt_vpack_uint(&p, 0, WT_INSERT_RECNO(ins)));
            ds->key->size = WT_PTRDIFF(p, ds->key->mem);
            WT_RET(__debug_hs_key(ds));
        }
    }
    return (0);
}

/*
 * __debug_row_skip --
 *     Dump an insert list.
 */
static int
__debug_row_skip(WT_DBG *ds, WT_INSERT_HEAD *head)
{
    WT_INSERT *ins;
    WT_SESSION_IMPL *session;

    session = ds->session;

    WT_SKIP_FOREACH (ins, head) {
        WT_RET(__debug_item_key(ds, "insert", WT_INSERT_KEY(ins), WT_INSERT_KEY_SIZE(ins)));
        WT_RET(__debug_update(ds, ins->upd, false));

        if (!WT_IS_HS(session->dhandle) && ds->hs_cursor != NULL) {
            WT_RET(__wt_buf_set(session, ds->key, WT_INSERT_KEY(ins), WT_INSERT_KEY_SIZE(ins)));
            WT_RET(__debug_hs_key(ds));
        }
    }
    return (0);
}

/*
 * __debug_modify --
 *     Dump a modify update.
 */
static int
__debug_modify(WT_DBG *ds, const uint8_t *data)
{
    size_t nentries, data_size, offset, size;
    const size_t *p;

    p = (size_t *)data;
    memcpy(&nentries, p++, sizeof(size_t));
    data += sizeof(size_t) + (nentries * 3 * sizeof(size_t));

    WT_RET(ds->f(ds, "%" WT_SIZET_FMT ": ", nentries));
    for (; nentries-- > 0; data += data_size) {
        memcpy(&data_size, p++, sizeof(size_t));
        memcpy(&offset, p++, sizeof(size_t));
        memcpy(&size, p++, sizeof(size_t));
        WT_RET(ds->f(ds, "{%" WT_SIZET_FMT ", %" WT_SIZET_FMT ", %" WT_SIZET_FMT ", ", data_size,
          offset, size));
        WT_RET(__debug_bytes(ds, data, data_size));
        WT_RET(ds->f(ds, "}%s", nentries == 0 ? "" : ", "));
    }

    return (0);
}

/*
 * __debug_update --
 *     Dump an update list.
 */
static int
__debug_update(WT_DBG *ds, WT_UPDATE *upd, bool hexbyte)
{
    char ts_string[WT_TS_INT_STRING_SIZE];
    const char *prepare_state;

    for (; upd != NULL; upd = upd->next) {
        switch (upd->type) {
        case WT_UPDATE_INVALID:
            WT_RET(ds->f(ds, "\tvalue {invalid}\n"));
            break;
        case WT_UPDATE_MODIFY:
            WT_RET(ds->f(ds, "\tvalue {modify: "));
            WT_RET(__debug_modify(ds, upd->data));
            WT_RET(ds->f(ds, "}\n"));
            break;
        case WT_UPDATE_RESERVE:
            WT_RET(ds->f(ds, "\tvalue {reserve}\n"));
            break;
        case WT_UPDATE_STANDARD:
            if (hexbyte) {
                WT_RET(ds->f(ds, "\t{"));
                WT_RET(__debug_hex_byte(ds, *upd->data));
                WT_RET(ds->f(ds, "}\n"));
            } else
                WT_RET(__debug_item_value(ds, "value", upd->data, upd->size));
            break;
        case WT_UPDATE_TOMBSTONE:
            WT_RET(ds->f(ds, "\tvalue {tombstone}\n"));
            break;
        }

        if (upd->txnid == WT_TXN_ABORTED)
            WT_RET(ds->f(ds,
              "\t"
              "txn id aborted"));
        else
            WT_RET(ds->f(ds,
              "\t"
              "txn id %" PRIu64,
              upd->txnid));

        WT_RET(ds->f(ds, ", start_ts %s", __wt_timestamp_to_string(upd->start_ts, ts_string)));
        if (upd->durable_ts != WT_TS_NONE)
            WT_RET(
              ds->f(ds, ", durable_ts %s", __wt_timestamp_to_string(upd->durable_ts, ts_string)));

        prepare_state = NULL;
        switch (upd->prepare_state) {
        case WT_PREPARE_INIT:
            break;
        case WT_PREPARE_INPROGRESS:
            prepare_state = "in-progress";
            break;
        case WT_PREPARE_LOCKED:
            prepare_state = "locked";
            break;
        case WT_PREPARE_RESOLVED:
            prepare_state = "resolved";
            break;
        }
        if (prepare_state != NULL)
            WT_RET(ds->f(ds, ", prepare %s", prepare_state));

        WT_RET(ds->f(ds, "\n"));
    }
    return (0);
}

/*
 * __debug_ref_state --
 *     Return a string representing the WT_REF state.
 */
static const char *
__debug_ref_state(u_int state)
{
    switch (state) {
    case WT_REF_DISK:
        return ("disk");
    case WT_REF_DELETED:
        return ("deleted");
    case WT_REF_LOCKED:
        return ("locked");
    case WT_REF_MEM:
        return ("memory");
    case WT_REF_SPLIT:
        return ("split");
    default:
        return ("INVALID");
    }
    /* NOTREACHED */
}

/*
 * __debug_ref --
 *     Dump a WT_REF structure.
 */
static int
__debug_ref(WT_DBG *ds, WT_REF *ref)
{
    WT_ADDR_COPY addr;
    WT_SESSION_IMPL *session;
    char time_string[WT_TIME_STRING_SIZE];

    session = ds->session;

    WT_RET(ds->f(ds, "\t%p, ", (void *)ref));
    WT_RET(ds->f(ds, "%s", __debug_ref_state(ref->state)));
    if (F_ISSET(ref, WT_REF_FLAG_INTERNAL))
        WT_RET(ds->f(ds, ", %s", "internal"));
    if (F_ISSET(ref, WT_REF_FLAG_LEAF))
        WT_RET(ds->f(ds, ", %s", "leaf"));
    if (F_ISSET(ref, WT_REF_FLAG_READING))
        WT_RET(ds->f(ds, ", %s", "reading"));

    if (__wt_ref_addr_copy(session, ref, &addr) && !WT_TIME_AGGREGATE_IS_EMPTY(&addr.ta))
        WT_RET(ds->f(ds, ", %s, %s", __wt_time_aggregate_to_string(&addr.ta, time_string),
          __wt_addr_string(session, addr.addr, addr.size, ds->t1)));
    return (ds->f(ds, "\n"));
}
#endif
