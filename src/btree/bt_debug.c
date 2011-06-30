/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"
#include "btree.i"
#include "cell.i"

/*
 * We pass around a session handle and output information, group it together.
 */
typedef struct {
	WT_SESSION_IMPL *session;		/* Enclosing session */

	/*
	 * When using the standard event handlers, the debugging output has to
	 * do its own message handling because its output isn't line-oriented.
	 */
	FILE		*fp;			/* Output file stream */
	WT_BUF		*msg;			/* Buffered message */
} WT_DBG;

#ifdef HAVE_DIAGNOSTIC
static void __wt_debug_byte_string(WT_DBG *, const uint8_t *, uint32_t);
static int  __wt_debug_cell(WT_DBG *, WT_CELL *);
static int  __wt_debug_cell_data(WT_DBG *, const char *, WT_CELL *);
static void __wt_debug_col_insert(WT_DBG *, WT_INSERT *);
static int  __wt_debug_config(WT_SESSION_IMPL *, WT_DBG *, const char *);
static int  __wt_debug_dsk_cell(WT_DBG *, WT_PAGE_DISK *);
static void __wt_debug_dsk_col_fix(WT_DBG *, WT_PAGE_DISK *);
static void __wt_debug_dsk_col_int(WT_DBG *, WT_PAGE_DISK *);
static void __wt_debug_dsk_col_rle(WT_DBG *, WT_PAGE_DISK *);
static void __wt_debug_ikey(WT_DBG *, WT_IKEY *);
static void __wt_debug_item(WT_DBG *, const char *, const void *, uint32_t);
static void __wt_debug_page_col_fix(WT_DBG *, WT_PAGE *);
static int  __wt_debug_page_col_int(WT_DBG *, WT_PAGE *, uint32_t);
static void __wt_debug_page_col_rle(WT_DBG *, WT_PAGE *);
static int  __wt_debug_page_col_var(WT_DBG *, WT_PAGE *);
static void __wt_debug_page_flags(WT_DBG *, WT_PAGE *);
static int  __wt_debug_page_row_int(WT_DBG *, WT_PAGE *, uint32_t);
static int  __wt_debug_page_row_leaf(WT_DBG *, WT_PAGE *);
static int  __wt_debug_page_work(WT_DBG *, WT_PAGE *, uint32_t);
static void __wt_debug_ref(WT_DBG *, WT_REF *);
static void __wt_debug_row_insert(WT_DBG *, WT_INSERT *);
static void __wt_debug_update(WT_DBG *, WT_UPDATE *);
static void __wt_dmsg(WT_DBG *, const char *, ...);
static void __wt_dmsg_wrapup(WT_DBG *);

/*
 * __wt_debug_config --
 *	Configure debugging output.
 */
static int
__wt_debug_config(WT_SESSION_IMPL *session, WT_DBG *ds, const char *ofile)
{
	memset(ds, 0, sizeof(WT_DBG));

	ds->session = session;

	/*
	 * If we weren't given a file, we use the default event handler, and
	 * we'll have to buffer messages.
	 */
	if (ofile == NULL)
		return (__wt_scr_alloc(session, 512, &ds->msg));

	return ((ds->fp = fopen(ofile, "w")) == NULL ? WT_ERROR : 0);
}

/*
 * __wt_dmsg_wrapup --
 *	Flush any remaining output, release resources.
 */
static void
__wt_dmsg_wrapup(WT_DBG *ds)
{
	WT_SESSION_IMPL *session;
	WT_BUF *msg;

	session = ds->session;
	msg = ds->msg;

	/*
	 * Discard the buffer -- it shouldn't have anything in it, but might
	 * as well be cautious.
	 */
	if (msg != NULL) {
		if (msg->size != 0)
			__wt_msg(session, "%s", (char *)msg->mem);
		__wt_scr_release(&ds->msg);
	}

	/* Close any file we opened. */
	if (ds->fp != NULL)
		(void)fclose(ds->fp);
}

/*
 * __wt_dmsg --
 *	Debug message.
 */
static void
__wt_dmsg(WT_DBG *ds, const char *fmt, ...)
    WT_GCC_ATTRIBUTE ((format (printf, 2, 3)))

{
	WT_BUF *msg;
	WT_SESSION_IMPL *session;
	size_t len, remain;
	va_list ap;

	session = ds->session;

	va_start(ap, fmt);

	/*
	 * Debug output chunks are not necessarily terminated with a newline
	 * character.  It's easy if we're dumping to a stream, but if we're
	 * dumping to an event handler, which is line-oriented, we must buffer
	 * the output chunk, and pass it to the event handler once we see a
	 * terminating newline.
	 */
	if (ds->fp == NULL) {
		msg = ds->msg;
		for (;;) {
			remain = msg->mem_size - msg->size;
			len = (size_t)vsnprintf(
			    (char *)msg->mem + msg->size, remain, fmt, ap);
			if (len < remain) {
				msg->size += len;
				break;
			}

			/*
			 * There's not much to do on error without checking for
			 * an error return on every single printf.  Anyway, it's
			 * pretty unlikely and this is debugging output, I'm not
			 * going to worry about it.
			 */
			if (__wt_buf_grow(
			    session, msg, msg->mem_size + len + 128) != 0)
				return;
		}
		if (((uint8_t *)msg->mem)[msg->size - 1] == '\n') {
			((uint8_t *)msg->mem)[msg->size - 1] = '\0';
			__wt_msg(session, "%s", (char *)msg->mem);
			msg->size = 0;
		}
	} else
		(void)vfprintf(ds->fp, fmt, ap);

	va_end(ap);
}

/*
 * __wt_debug_addr --
 *	Read and dump a disk page in debugging mode.
 */
int
__wt_debug_addr(
    WT_SESSION_IMPL *session, uint32_t addr, uint32_t size, const char *ofile)
{
	WT_DBG *ds, _ds;
	int ret;
	char *bp;

	ret = 0;

	ds = &_ds;
	WT_RET(__wt_debug_config(session, ds, ofile));

	WT_RET(__wt_calloc_def(session, (size_t)size, &bp));
	WT_ERR(__wt_disk_read(session, (WT_PAGE_DISK *)bp, addr, size));
	ret = __wt_debug_disk(session, (WT_PAGE_DISK *)bp, NULL);
err:	__wt_free(session, bp);

	__wt_dmsg_wrapup(ds);

	return (ret);
}

/*
 * __wt_debug_disk --
 *	Dump a disk page in debugging mode.
 */
int
__wt_debug_disk(WT_SESSION_IMPL *session, WT_PAGE_DISK *dsk, const char *ofile)
{
	WT_DBG *ds, _ds;
	int ret;

	ret = 0;

	ds = &_ds;
	WT_RET(__wt_debug_config(session, ds, ofile));

	switch (dsk->type) {
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_INT:
	case WT_PAGE_COL_RLE:
	case WT_PAGE_COL_VAR:
		__wt_dmsg(ds, "%s page: starting recno %" PRIu64
		    ", entries %" PRIu32 ", lsn %" PRIu32 "/%" PRIu32 "\n",
		    __wt_page_type_string(dsk->type),
		    dsk->recno, dsk->u.entries,
		    WT_LSN_FILE(dsk->lsn), WT_LSN_OFFSET(dsk->lsn));
		break;
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		__wt_dmsg(ds, "%s page: entries %" PRIu32
		    ", lsn %" PRIu32 "/%" PRIu32 "\n",
		    __wt_page_type_string(dsk->type), dsk->u.entries,
		    WT_LSN_FILE(dsk->lsn), WT_LSN_OFFSET(dsk->lsn));
		break;
	case WT_PAGE_OVFL:
		__wt_dmsg(ds, "%s page: data size %" PRIu32
		    ", lsn %" PRIu32 "/%" PRIu32 "\n",
		    __wt_page_type_string(dsk->type), dsk->u.datalen,
		    WT_LSN_FILE(dsk->lsn), WT_LSN_OFFSET(dsk->lsn));
		break;
	WT_ILLEGAL_FORMAT(session);
	}

	switch (dsk->type) {
	case WT_PAGE_COL_FIX:
		__wt_debug_dsk_col_fix(ds, dsk);
		break;
	case WT_PAGE_COL_INT:
		__wt_debug_dsk_col_int(ds, dsk);
		break;
	case WT_PAGE_COL_RLE:
		__wt_debug_dsk_col_rle(ds, dsk);
		break;
	case WT_PAGE_COL_VAR:
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		ret = __wt_debug_dsk_cell(ds, dsk);
		break;
	default:
		break;
	}

	__wt_dmsg_wrapup(ds);

	return (ret);
}

#define	WT_DEBUG_TREE_LEAF	0x01			/* Debug leaf pages */
#define	WT_DEBUG_TREE_WALK	0x02			/* Descend the tree */

/*
 * __wt_debug_tree_all --
 *	Dump the in-memory information for a tree, including leaf pages.
 */
int
__wt_debug_tree_all(WT_SESSION_IMPL *session, WT_PAGE *page, const char *ofile)
{
	WT_DBG *ds, _ds;
	int ret;

	ds = &_ds;
	WT_RET(__wt_debug_config(session, ds, ofile));

	/* A NULL page starts at the top of the tree -- it's a convenience. */
	if (page == NULL)
		page = session->btree->root_page.page;

	ret = __wt_debug_page_work(
	    ds, page, WT_DEBUG_TREE_LEAF | WT_DEBUG_TREE_WALK);

	__wt_dmsg_wrapup(ds);

	return (ret);
}

/*
 * __wt_debug_tree --
 *	Dump the in-memory information for a tree, not including leaf pages.
 */
int
__wt_debug_tree(WT_SESSION_IMPL *session, WT_PAGE *page, const char *ofile)
{
	WT_DBG *ds, _ds;
	int ret;

	ds = &_ds;
	WT_RET(__wt_debug_config(session, ds, ofile));

	/* A NULL page starts at the top of the tree -- it's a convenience. */
	if (page == NULL)
		page = session->btree->root_page.page;

	ret = __wt_debug_page_work(ds, page, WT_DEBUG_TREE_WALK);

	__wt_dmsg_wrapup(ds);

	return (ret);
}

/*
 * __wt_debug_page --
 *	Dump the in-memory information for a page.
 */
int
__wt_debug_page(WT_SESSION_IMPL *session, WT_PAGE *page, const char *ofile)
{
	WT_DBG *ds, _ds;
	int ret;

	ds = &_ds;
	WT_RET(__wt_debug_config(session, ds, ofile));

	ret = __wt_debug_page_work(ds, page, WT_DEBUG_TREE_LEAF);

	__wt_dmsg_wrapup(ds);

	return (ret);
}

/*
 * __wt_debug_page_work --
 *	Dump the in-memory information for an in-memory page.
 */
static int
__wt_debug_page_work(WT_DBG *ds, WT_PAGE *page, uint32_t flags)
{
	WT_SESSION_IMPL *session;
	WT_BTREE *btree;

	session = ds->session;
	btree = session->btree;

	__wt_dmsg(ds, "%p: ", page);
	if (WT_PADDR(page) == WT_ADDR_INVALID)
		__wt_dmsg(ds, "[NoAddr]");
	else
		__wt_dmsg(ds, "[%" PRIu32 "-%" PRIu32 "]", WT_PADDR(page),
		    WT_PADDR(page) + (WT_PSIZE(page) / btree->allocsize) - 1);
	__wt_dmsg(ds, "/%" PRIu32 " %s",
	    WT_PSIZE(page), __wt_page_type_string(page->type));

	switch (page->type) {
	case WT_PAGE_COL_INT:
		__wt_dmsg(ds, " recno %" PRIu64, page->u.col_int.recno);
		break;
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_RLE:
	case WT_PAGE_COL_VAR:
		__wt_dmsg(ds, " recno %" PRIu64, page->u.col_leaf.recno);
		break;
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		break;
	WT_ILLEGAL_FORMAT(session);
	}

	__wt_debug_page_flags(ds, page);

	/* Skip bulk-loaded pages. */
	if (F_ISSET(page, WT_PAGE_BULK_LOAD)) {
		__wt_dmsg(ds, "bulk-loaded page -- skipped\n");
		return (0);
	}

	/* Dump the page. */
	switch (page->type) {
	case WT_PAGE_COL_FIX:
		if (LF_ISSET(WT_DEBUG_TREE_LEAF))
			__wt_debug_page_col_fix(ds, page);
		break;
	case WT_PAGE_COL_INT:
		WT_RET(__wt_debug_page_col_int(ds, page, flags));
		break;
	case WT_PAGE_COL_RLE:
		if (LF_ISSET(WT_DEBUG_TREE_LEAF))
			__wt_debug_page_col_rle(ds, page);
		break;
	case WT_PAGE_COL_VAR:
		if (LF_ISSET(WT_DEBUG_TREE_LEAF))
			WT_RET(__wt_debug_page_col_var(ds, page));
		break;
	case WT_PAGE_ROW_INT:
		WT_RET(__wt_debug_page_row_int(ds, page, flags));
		break;
	case WT_PAGE_ROW_LEAF:
		if (LF_ISSET(WT_DEBUG_TREE_LEAF))
			WT_RET(__wt_debug_page_row_leaf(ds, page));
		break;
	WT_ILLEGAL_FORMAT(session);
	}

	return (0);
}

/*
 * __wt_debug_page_flags --
 *	Print out the page flags.
 */
static void
__wt_debug_page_flags(WT_DBG *ds, WT_PAGE *page)
{
	__wt_dmsg(ds,
	    " (%s", WT_PAGE_IS_MODIFIED(page) ? "dirty" : "clean");
	if (WT_PAGE_IS_ROOT(page))
		__wt_dmsg(ds, ", root");
	if (F_ISSET(page, WT_PAGE_BULK_LOAD))
		__wt_dmsg(ds, ", bulk-loaded");
	if (F_ISSET(page, WT_PAGE_CACHE_COUNTED))
		__wt_dmsg(ds, ", cache-counted");
	if (F_ISSET(page, WT_PAGE_DELETED))
		__wt_dmsg(ds, ", deleted");
	if (F_ISSET(page, WT_PAGE_INITIAL_EMPTY))
		__wt_dmsg(ds, ", initial-empty");
	if (F_ISSET(page, WT_PAGE_PINNED))
		__wt_dmsg(ds, ", pinned");
	if (F_ISSET(page, WT_PAGE_SPLIT))
		__wt_dmsg(ds, ", split");
	__wt_dmsg(ds, ")\n");
}

/*
 * __wt_debug_page_col_fix --
 *	Dump an in-memory WT_PAGE_COL_FIX page.
 */
static void
__wt_debug_page_col_fix(WT_DBG *ds, WT_PAGE *page)
{
	WT_COL *cip;
	WT_UPDATE *upd;
	uint32_t fixed_len, i;
	void *cipvalue;

	fixed_len = ds->session->btree->fixed_len;

	WT_COL_FOREACH(page, cip, i) {
		cipvalue = WT_COL_PTR(page, cip);
		__wt_dmsg(ds, "\tV {");
		if (WT_FIX_DELETE_ISSET(cipvalue))
			__wt_dmsg(ds, "deleted");
		else
			__wt_debug_byte_string(ds, cipvalue, fixed_len);
		__wt_dmsg(ds, "}\n");

		if ((upd = WT_COL_UPDATE(page, cip)) != NULL)
			__wt_debug_update(ds, upd);
	}
}

/*
 * __wt_debug_page_col_int --
 *	Dump an in-memory WT_PAGE_COL_INT page.
 */
static int
__wt_debug_page_col_int(WT_DBG *ds, WT_PAGE *page, uint32_t flags)
{
	WT_COL_REF *cref;
	uint32_t i;

	WT_COL_REF_FOREACH(page, cref, i) {
		__wt_dmsg(ds, "\trecno %" PRIu64 ", ", cref->recno);
		__wt_debug_ref(ds, &cref->ref);
	}

	if (!LF_ISSET(WT_DEBUG_TREE_WALK))
		return (0);

	WT_COL_REF_FOREACH(page, cref, i)
		if (WT_COL_REF_STATE(cref) == WT_REF_MEM)
			WT_RET(__wt_debug_page_work(
			    ds, WT_COL_REF_PAGE(cref), flags));
	return (0);
}

/*
 * __wt_debug_page_col_rle --
 *	Dump an in-memory WT_PAGE_COL_RLE page.
 */
static void
__wt_debug_page_col_rle(WT_DBG *ds, WT_PAGE *page)
{
	WT_COL *cip;
	WT_INSERT *ins;
	uint32_t fixed_len, i;
	void *cipvalue;

	fixed_len = ds->session->btree->fixed_len;

	WT_COL_FOREACH(page, cip, i) {
		cipvalue = WT_COL_PTR(page, cip);
		__wt_dmsg(ds,
		    "\trepeat %" PRIu32 " {", WT_RLE_REPEAT_COUNT(cipvalue));
		if (WT_FIX_DELETE_ISSET(WT_RLE_REPEAT_DATA(cipvalue)))
			__wt_dmsg(ds, "deleted");
		else
			__wt_debug_byte_string(
			    ds, WT_RLE_REPEAT_DATA(cipvalue), fixed_len);
		__wt_dmsg(ds, "}\n");

		if ((ins = WT_COL_INSERT(page, cip)) != NULL)
			__wt_debug_col_insert(ds, ins);
	}
}

/*
 * __wt_debug_page_col_var --
 *	Dump an in-memory WT_PAGE_COL_VAR page.
 */
static int
__wt_debug_page_col_var(WT_DBG *ds, WT_PAGE *page)
{
	WT_COL *cip;
	WT_UPDATE *upd;
	uint32_t i;

	WT_COL_FOREACH(page, cip, i) {
		WT_RET(
		    __wt_debug_cell_data(ds, "V", WT_COL_PTR(page, cip)));

		if ((upd = WT_COL_UPDATE(page, cip)) != NULL)
			__wt_debug_update(ds, upd);
	}
	return (0);
}

/*
 * __wt_debug_page_row_int --
 *	Dump an in-memory WT_PAGE_ROW_INT page.
 */
static int
__wt_debug_page_row_int(WT_DBG *ds, WT_PAGE *page, uint32_t flags)
{
	WT_ROW_REF *rref;
	uint32_t i;

	WT_ROW_REF_FOREACH(page, rref, i) {
		__wt_debug_ikey(ds, rref->key);
		__wt_dmsg(ds, "\t");
		__wt_debug_ref(ds, &rref->ref);
	}

	if (!LF_ISSET(WT_DEBUG_TREE_WALK))
		return (0);

	WT_ROW_REF_FOREACH(page, rref, i)
		if (WT_ROW_REF_STATE(rref) == WT_REF_MEM)
			WT_RET(__wt_debug_page_work(
			    ds, WT_ROW_REF_PAGE(rref), flags));
	return (0);
}

/*
 * __wt_debug_page_row_leaf --
 *	Dump an in-memory WT_PAGE_ROW_LEAF page.
 */
static int
__wt_debug_page_row_leaf(WT_DBG *ds, WT_PAGE *page)
{
	WT_CELL *cell;
	WT_INSERT *ins;
	WT_ROW *rip;
	WT_UPDATE *upd;
	uint32_t i;

	/*
	 * Dump any K/V pairs inserted into the page before the first from-disk
	 * key on the page.
	 */
	if ((ins = WT_ROW_INSERT_SMALLEST(page)) != NULL)
		__wt_debug_row_insert(ds, ins);

	/* Dump the page's K/V pairs. */
	WT_ROW_FOREACH(page, rip, i) {
		if (__wt_off_page(page, rip->key))
			__wt_debug_ikey(ds, rip->key);
		else
			WT_RET(__wt_debug_cell_data(ds, "K", rip->key));

		if ((cell = __wt_row_value(page, rip)) == NULL)
			__wt_dmsg(ds, "\tV {}\n");
		else
			WT_RET(__wt_debug_cell_data(ds, "V", cell));

		if ((upd = WT_ROW_UPDATE(page, rip)) != NULL)
			__wt_debug_update(ds, upd);

		if ((ins = WT_ROW_INSERT(page, rip)) != NULL)
			__wt_debug_row_insert(ds, ins);
	}

	return (0);
}

/*
 * __wt_debug_col_insert --
 *	Dump an RLE column-store insert array.
 */
static void
__wt_debug_col_insert(WT_DBG *ds, WT_INSERT *ins)
{
	for (; ins != NULL; ins = ins->next) {
		__wt_dmsg(ds,
		    "\tinsert %" PRIu64 "\n", WT_INSERT_RECNO(ins));
		__wt_debug_update(ds, ins->upd);
	}
}

/*
 * __wt_debug_row_insert --
 *	Dump an insert array.
 */
static void
__wt_debug_row_insert(WT_DBG *ds, WT_INSERT *ins)
{
	for (; ins != NULL; ins = ins->next) {
		__wt_debug_item(ds,
		    "insert", WT_INSERT_KEY(ins), WT_INSERT_KEY_SIZE(ins));
		__wt_debug_update(ds, ins->upd);
	}
}

/*
 * __wt_debug_update --
 *	Dump an update array.
 */
static void
__wt_debug_update(WT_DBG *ds, WT_UPDATE *upd)
{
	for (; upd != NULL; upd = upd->next)
		if (WT_UPDATE_DELETED_ISSET(upd))
			__wt_dmsg(ds, "\tupdate: {deleted}\n");
		else
			__wt_debug_item(ds,
			    "update", WT_UPDATE_DATA(upd), upd->size);
}

/*
 * __wt_debug_dsk_cell --
 *	Dump a page of WT_CELL's.
 */
static int
__wt_debug_dsk_cell(WT_DBG *ds, WT_PAGE_DISK *dsk)
{
	WT_CELL *cell;
	uint32_t i;

	WT_CELL_FOREACH(dsk, cell, i)
		WT_RET(__wt_debug_cell(ds, cell));
	return (0);
}

/*
 * __wt_debug_cell --
 *	Dump a single WT_CELL.
 */
static int
__wt_debug_cell(WT_DBG *ds, WT_CELL *cell)
{
	WT_SESSION_IMPL *session;
	WT_OFF off;

	session = ds->session;

	__wt_dmsg(ds, "\t%s: len %" PRIu32,
	    __wt_cell_type_string(cell), __wt_cell_datalen(cell));

	switch (__wt_cell_type(cell)) {
	case WT_CELL_DATA:
	case WT_CELL_DEL:
		break;
	case WT_CELL_KEY:
		__wt_dmsg(ds, ", pfx: %u", __wt_cell_prefix(cell));
		break;
	case WT_CELL_DATA_OVFL:
	case WT_CELL_KEY_OVFL:
	case WT_CELL_OFF:
		__wt_cell_off(cell, &off);
		__wt_dmsg(ds, ", offpage: addr %" PRIu32 ", size %" PRIu32,
		    off.addr, off.size);
		break;
	WT_ILLEGAL_FORMAT(session);
	}
	__wt_dmsg(ds, "\n");

	return (__wt_debug_cell_data(ds, NULL, cell));
}

/*
 * __wt_debug_dsk_col_int --
 *	Dump a WT_PAGE_COL_INT page.
 */
static void
__wt_debug_dsk_col_int(WT_DBG *ds, WT_PAGE_DISK *dsk)
{
	WT_OFF_RECORD *off_record;
	uint32_t i;

	WT_OFF_FOREACH(dsk, off_record, i)
		__wt_dmsg(ds, "\toffpage: addr %" PRIu32 ", size %" PRIu32
		    ", starting recno %" PRIu64 "\n",
		    off_record->addr, off_record->size, WT_RECNO(off_record));
}

/*
 * __wt_debug_dsk_col_fix --
 *	Dump a WT_PAGE_COL_FIX page.
 */
static void
__wt_debug_dsk_col_fix(WT_DBG *ds, WT_PAGE_DISK *dsk)
{
	WT_BTREE *btree;
	uint32_t fixed_len, i;
	uint8_t *p;

	btree = ds->session->btree;
	fixed_len = ds->session->btree->fixed_len;

	WT_FIX_FOREACH(btree, dsk, p, i) {
		__wt_dmsg(ds, "\t{");
		if (WT_FIX_DELETE_ISSET(p))
			__wt_dmsg(ds, "deleted");
		else
			__wt_debug_byte_string(ds, p, fixed_len);
		__wt_dmsg(ds, "}\n");
	}
}

/*
 * __wt_debug_dsk_col_rle --
 *	Dump a WT_PAGE_COL_RLE page.
 */
static void
__wt_debug_dsk_col_rle(WT_DBG *ds, WT_PAGE_DISK *dsk)
{
	WT_BTREE *btree;
	uint32_t fixed_len, i;
	uint8_t *p;

	btree = ds->session->btree;
	fixed_len = ds->session->btree->fixed_len;

	WT_RLE_REPEAT_FOREACH(btree, dsk, p, i) {
		__wt_dmsg(ds, "\trepeat %" PRIu32 " {", WT_RLE_REPEAT_COUNT(p));
		if (WT_FIX_DELETE_ISSET(WT_RLE_REPEAT_DATA(p)))
			__wt_dmsg(ds, "deleted");
		else
			__wt_debug_byte_string(
			    ds, WT_RLE_REPEAT_DATA(p), fixed_len);
		__wt_dmsg(ds, "}\n");
	}
}

/*
 * __wt_debug_cell_data --
 *	Dump a single cell's data in debugging mode.
 */
static int
__wt_debug_cell_data(WT_DBG *ds, const char *tag, WT_CELL *cell)
{
	WT_BUF *tmp;
	WT_SESSION_IMPL *session;
	uint32_t size;
	const uint8_t *p;
	int ret;

	session = ds->session;
	tmp = NULL;
	ret = 0;

	switch (__wt_cell_type(cell)) {
	case WT_CELL_DATA:
	case WT_CELL_DATA_OVFL:
	case WT_CELL_KEY:
	case WT_CELL_KEY_OVFL:
		WT_ERR(__wt_scr_alloc(session, 0, &tmp));
		WT_ERR(__wt_cell_copy(session, cell, tmp));
		p = tmp->data;
		size = tmp->size;
		break;
	case WT_CELL_DEL:
		p = (uint8_t *)"deleted";
		size = sizeof("deleted") - 1;
		break;
	case WT_CELL_OFF:
		p = (uint8_t *)"offpage";
		size = sizeof("offpage") - 1;
		break;
	WT_ILLEGAL_FORMAT_ERR(session, ret);
	}

	__wt_debug_item(ds, tag, p, size);

err:	if (tmp != NULL)
		__wt_scr_release(&tmp);
	return (ret);
}

/*
 * __wt_debug_ikey --
 *	Dump a single WT_IKEY in debugging mode, with an optional tag.
 */
static void
__wt_debug_ikey(WT_DBG *ds, WT_IKEY *ikey)
{
	__wt_debug_item(ds, "K", WT_IKEY_DATA(ikey), ikey->size);
}

/*
 * __wt_debug_item --
 *	Dump a single data/size pair, with an optional tag.
 */
static void
__wt_debug_item(WT_DBG *ds, const char *tag, const void *data, uint32_t size)
{
	__wt_dmsg(ds,
	    "\t%s%s{", tag == NULL ? "" : tag, tag == NULL ? "" : " ");
	__wt_debug_byte_string(ds, data, size);
	__wt_dmsg(ds, "}\n");
}

/*
 * __wt_debug_ref --
 *	Print out a page's in-memory WT_REF state.
 */
static void
__wt_debug_ref(WT_DBG *ds, WT_REF *ref)
{
	const char *s;

	switch (ref->state) {
	case WT_REF_DISK:
		s = "disk";
		break;
	case WT_REF_LOCKED:
		s = "locked";
		break;
	case WT_REF_MEM:
		s = "memory";
		break;
	default:
		s = "UNKNOWN";
		break;
	}

	if (ref->addr == WT_ADDR_INVALID)
		__wt_dmsg(ds, "NoAddr");
	else
		__wt_dmsg(ds, "%" PRIu32 "/%" PRIu32, ref->addr, ref->size);

	__wt_dmsg(ds, ": %s", s);
	if (ref->state == WT_REF_MEM)
		__wt_dmsg(ds, "(%p)", ref->page);
	__wt_dmsg(ds, "\n");
}

/*
 * __wt_debug_byte_string --
 *	Output a single byte string in printable characters, where possible.
 */
static void
__wt_debug_byte_string(WT_DBG *ds, const uint8_t *data, uint32_t size)
{
	static const char hex[] = "0123456789abcdef";
	int ch;

	for (; size > 0; --size, ++data) {
		ch = data[0];
		if (isprint(ch))
			__wt_dmsg(ds, "%c", ch);
		else
			__wt_dmsg(ds, "%x%x",
			    hex[(data[0] & 0xf0) >> 4], hex[data[0] & 0x0f]);
	}
}
#endif
