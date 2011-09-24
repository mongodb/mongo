/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

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
static void __debug_byte_string(WT_DBG *, const uint8_t *, size_t);
static int  __debug_cell(WT_DBG *, WT_CELL_UNPACK *);
static int  __debug_cell_data(WT_DBG *, const char *, WT_CELL_UNPACK *);
static void __debug_col_list(WT_DBG *, WT_INSERT_HEAD *, const char *, int);
static int  __debug_config(WT_SESSION_IMPL *, WT_DBG *, const char *);
static int  __debug_dsk_cell(WT_DBG *, WT_PAGE_DISK *);
static void __debug_dsk_col_fix(WT_DBG *, WT_PAGE_DISK *);
static void __debug_dsk_col_int(WT_DBG *, WT_PAGE_DISK *);
static void __debug_ikey(WT_DBG *, WT_IKEY *);
static void __debug_item(WT_DBG *, const char *, const void *, size_t);
static void __debug_page_col_fix(WT_DBG *, WT_PAGE *);
static int  __debug_page_col_int(WT_DBG *, WT_PAGE *, uint32_t);
static int  __debug_page_col_var(WT_DBG *, WT_PAGE *);
static int  __debug_page_row_int(WT_DBG *, WT_PAGE *, uint32_t);
static int  __debug_page_row_leaf(WT_DBG *, WT_PAGE *);
static int  __debug_page_work(WT_DBG *, WT_PAGE *, uint32_t);
static void __debug_ref(WT_DBG *, WT_REF *);
static void __debug_row_list(WT_DBG *, WT_INSERT_HEAD *);
static void __debug_update(WT_DBG *, WT_UPDATE *, int);
static void __dmsg(WT_DBG *, const char *, ...)
	WT_GCC_ATTRIBUTE((format (printf, 2, 3)));
static void __dmsg_wrapup(WT_DBG *);

/*
 * __debug_hex_byte --
 *	Output a single byte in hex.
 */
static inline void
__debug_hex_byte(WT_DBG *ds, uint8_t v)
{
	static const char hex[] = "0123456789abcdef";

	__dmsg(ds, "#%c%c", hex[(v & 0xf0) >> 4], hex[v & 0x0f]);
}

/*
 * __debug_config --
 *	Configure debugging output.
 */
static int
__debug_config(WT_SESSION_IMPL *session, WT_DBG *ds, const char *ofile)
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
 * __dmsg_wrapup --
 *	Flush any remaining output, release resources.
 */
static void
__dmsg_wrapup(WT_DBG *ds)
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
		__wt_scr_free(&ds->msg);
	}

	/* Close any file we opened. */
	if (ds->fp != NULL)
		(void)fclose(ds->fp);
}

/*
 * __dmsg --
 *	Debug message.
 */
static void
__dmsg(WT_DBG *ds, const char *fmt, ...)
{
	va_list ap;
	WT_BUF *msg;
	WT_SESSION_IMPL *session;
	size_t len, space;
	char *p;

	session = ds->session;

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
			p = (char *)msg->mem + msg->size;
			space = msg->memsize - msg->size;
			va_start(ap, fmt);
			len = (size_t)vsnprintf(p, space, fmt, ap);
			va_end(ap);

			/* Check if there was enough space. */
			if (len < space) {
				msg->size += (uint32_t)len;
				break;
			}

			/*
			 * There's not much to do on error without checking for
			 * an error return on every single printf.  Anyway, it's
			 * pretty unlikely and this is debugging output, I'm not
			 * going to worry about it.
			 */
			if (__wt_buf_grow(
			    session, msg, msg->memsize + len + 128) != 0)
				return;
		}
		if (((uint8_t *)msg->mem)[msg->size - 1] == '\n') {
			((uint8_t *)msg->mem)[msg->size - 1] = '\0';
			__wt_msg(session, "%s", (char *)msg->mem);
			msg->size = 0;
		}
	} else {
		va_start(ap, fmt);
		(void)vfprintf(ds->fp, fmt, ap);
		va_end(ap);
	}
}

/*
 * __wt_debug_addr --
 *	Read and dump a disk page in debugging mode.
 */
int
__wt_debug_addr(
    WT_SESSION_IMPL *session, uint32_t addr, uint32_t size, const char *ofile)
{
	WT_BUF *tmp;
	int ret;

	ret = 0;

	WT_RET(__wt_scr_alloc(session, size, &tmp));
	WT_ERR(__wt_block_read(session, tmp, addr, size, 0));
	ret = __wt_debug_disk(session, tmp->mem, ofile);
err:	__wt_scr_free(&tmp);

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
	WT_RET(__debug_config(session, ds, ofile));

	switch (dsk->type) {
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_INT:
	case WT_PAGE_COL_VAR:
		__dmsg(ds, "%s page: starting recno %" PRIu64
		    ", entries %" PRIu32 ", lsn %" PRIu32 "/%" PRIu32 "\n",
		    __wt_page_type_string(dsk->type),
		    dsk->recno, dsk->u.entries,
		    WT_LSN_FILE(dsk->lsn), WT_LSN_OFFSET(dsk->lsn));
		break;
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		__dmsg(ds, "%s page: entries %" PRIu32
		    ", lsn %" PRIu32 "/%" PRIu32 "\n",
		    __wt_page_type_string(dsk->type), dsk->u.entries,
		    WT_LSN_FILE(dsk->lsn), WT_LSN_OFFSET(dsk->lsn));
		break;
	case WT_PAGE_OVFL:
		__dmsg(ds, "%s page: data size %" PRIu32
		    ", lsn %" PRIu32 "/%" PRIu32 "\n",
		    __wt_page_type_string(dsk->type), dsk->u.datalen,
		    WT_LSN_FILE(dsk->lsn), WT_LSN_OFFSET(dsk->lsn));
		break;
	WT_ILLEGAL_FORMAT(session);
	}

	switch (dsk->type) {
	case WT_PAGE_COL_FIX:
		__debug_dsk_col_fix(ds, dsk);
		break;
	case WT_PAGE_COL_INT:
		__debug_dsk_col_int(ds, dsk);
		break;
	case WT_PAGE_COL_VAR:
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		ret = __debug_dsk_cell(ds, dsk);
		break;
	default:
		break;
	}

	__dmsg_wrapup(ds);

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
	WT_RET(__debug_config(session, ds, ofile));

	/* A NULL page starts at the top of the tree -- it's a convenience. */
	if (page == NULL)
		page = session->btree->root_page.page;

	ret = __debug_page_work(
	    ds, page, WT_DEBUG_TREE_LEAF | WT_DEBUG_TREE_WALK);

	__dmsg_wrapup(ds);

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
	WT_RET(__debug_config(session, ds, ofile));

	/* A NULL page starts at the top of the tree -- it's a convenience. */
	if (page == NULL)
		page = session->btree->root_page.page;

	ret = __debug_page_work(ds, page, WT_DEBUG_TREE_WALK);

	__dmsg_wrapup(ds);

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
	WT_RET(__debug_config(session, ds, ofile));

	ret = __debug_page_work(ds, page, WT_DEBUG_TREE_LEAF);

	__dmsg_wrapup(ds);

	return (ret);
}

/*
 * __debug_page_work --
 *	Dump the in-memory information for an in-memory page.
 */
static int
__debug_page_work(WT_DBG *ds, WT_PAGE *page, uint32_t flags)
{
	WT_SESSION_IMPL *session;
	WT_BTREE *btree;

	session = ds->session;
	btree = session->btree;

	__dmsg(ds, "%p: ", page);
	if (WT_PADDR(page) == WT_ADDR_INVALID)
		__dmsg(ds, "[NoAddr]");
	else
		__dmsg(ds, "[%" PRIu32 "-%" PRIu32 "]", WT_PADDR(page),
		    WT_PADDR(page) + (WT_PSIZE(page) / btree->allocsize) - 1);
	__dmsg(ds, "/%" PRIu32 " %s",
	    WT_PSIZE(page), __wt_page_type_string(page->type));

	switch (page->type) {
	case WT_PAGE_COL_INT:
		__dmsg(ds, " recno %" PRIu64, page->u.col_int.recno);
		break;
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_VAR:
		__dmsg(ds, " recno %" PRIu64, page->u.col_leaf.recno);
		break;
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		break;
	WT_ILLEGAL_FORMAT(session);
	}

	if (WT_PAGE_IS_ROOT(page))
		__dmsg(ds, ", root");
	else
		__dmsg(ds, ", parent %p", page->parent);

	__dmsg(ds,
	    " (%s", WT_PAGE_IS_MODIFIED(page) ? "dirty" : "clean");
	if (F_ISSET(page, WT_PAGE_BULK_LOAD))
		__dmsg(ds, ", bulk-loaded");
	if (F_ISSET(page, WT_PAGE_DELETED))
		__dmsg(ds, ", deleted");
	if (F_ISSET(page, WT_PAGE_INITIAL_EMPTY))
		__dmsg(ds, ", initial-empty");
	if (F_ISSET(page, WT_PAGE_MERGE))
		__dmsg(ds, ", merge");
	if (F_ISSET(page, WT_PAGE_PINNED))
		__dmsg(ds, ", pinned");
	__dmsg(ds, ")\n");

	/* Skip bulk-loaded pages. */
	if (F_ISSET(page, WT_PAGE_BULK_LOAD)) {
		__dmsg(ds, "bulk-loaded page -- skipped\n");
		return (0);
	}

	/* Dump the page. */
	switch (page->type) {
	case WT_PAGE_COL_FIX:
		if (LF_ISSET(WT_DEBUG_TREE_LEAF))
			__debug_page_col_fix(ds, page);
		break;
	case WT_PAGE_COL_INT:
		WT_RET(__debug_page_col_int(ds, page, flags));
		break;
	case WT_PAGE_COL_VAR:
		if (LF_ISSET(WT_DEBUG_TREE_LEAF))
			WT_RET(__debug_page_col_var(ds, page));
		break;
	case WT_PAGE_ROW_INT:
		WT_RET(__debug_page_row_int(ds, page, flags));
		break;
	case WT_PAGE_ROW_LEAF:
		if (LF_ISSET(WT_DEBUG_TREE_LEAF))
			WT_RET(__debug_page_row_leaf(ds, page));
		break;
	WT_ILLEGAL_FORMAT(session);
	}

	return (0);
}

/*
 * __debug_page_col_fix --
 *	Dump an in-memory WT_PAGE_COL_FIX page.
 */
static void
__debug_page_col_fix(WT_DBG *ds, WT_PAGE *page)
{
	WT_BTREE *btree;
	WT_INSERT *ins;
	WT_PAGE_DISK *dsk;
	WT_SESSION_IMPL *session;
	uint64_t recno;
	uint32_t i;
	uint8_t v;

	session = ds->session;
	btree = session->btree;
	dsk = page->dsk;
	recno = page->u.col_leaf.recno;

	if (dsk != NULL) {
		ins = WT_SKIP_FIRST(WT_COL_UPDATE_SINGLE(page));
		WT_FIX_FOREACH(btree, dsk, v, i) {
			__dmsg(ds, "\t%" PRIu64 "\t{", recno);
			__debug_hex_byte(ds, v);
			__dmsg(ds, "}\n");

			/* Check for a match on the update list. */
			if (ins != NULL && WT_INSERT_RECNO(ins) == recno) {
				__dmsg(ds,
				    "\tupdate %" PRIu64 "\n",
				    WT_INSERT_RECNO(ins));
				__debug_update(ds, ins->upd, 1);
				ins = WT_SKIP_NEXT(ins);
			}
			++recno;
		}
	}

	if (WT_COL_UPDATE_SINGLE(page) != NULL) {
		__dmsg(ds, "%s\n", S2C(session)->sep);
		__debug_col_list(ds, WT_COL_UPDATE_SINGLE(page), "update", 1);
	}
	if (WT_COL_APPEND(page) != NULL) {
		__dmsg(ds, "%s\n", S2C(session)->sep);
		__debug_col_list(ds, WT_COL_APPEND(page), "append", 1);
	}
}

/*
 * __debug_page_col_int --
 *	Dump an in-memory WT_PAGE_COL_INT page.
 */
static int
__debug_page_col_int(WT_DBG *ds, WT_PAGE *page, uint32_t flags)
{
	WT_COL_REF *cref;
	uint32_t i;

	WT_COL_REF_FOREACH(page, cref, i) {
		__dmsg(ds, "\trecno %" PRIu64 ", ", cref->recno);
		__debug_ref(ds, &cref->ref);
	}

	if (!LF_ISSET(WT_DEBUG_TREE_WALK))
		return (0);

	WT_COL_REF_FOREACH(page, cref, i)
		if (WT_COL_REF_STATE(cref) == WT_REF_MEM)
			WT_RET(__debug_page_work(
			    ds, WT_COL_REF_PAGE(cref), flags));
	return (0);
}

/*
 * __debug_page_col_var --
 *	Dump an in-memory WT_PAGE_COL_VAR page.
 */
static int
__debug_page_col_var(WT_DBG *ds, WT_PAGE *page)
{
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	WT_COL *cip;
	WT_INSERT_HEAD *inshead;
	WT_SESSION_IMPL *session;
	uint64_t recno, rle;
	uint32_t i;
	char tag[64];

	session = ds->session;
	unpack = &_unpack;
	recno = page->u.col_leaf.recno;

	WT_COL_FOREACH(page, cip, i) {
		if ((cell = WT_COL_PTR(page, cip)) == NULL) {
			unpack = NULL;
			rle = 1;
		} else {
			__wt_cell_unpack(cell, unpack);
			rle = unpack->rle;
		}
		snprintf(tag, sizeof(tag), "%" PRIu64 " %" PRIu64, recno, rle);
		WT_RET(__debug_cell_data(ds, tag, unpack));

		if ((inshead = WT_COL_UPDATE(page, cip)) != NULL)
			__debug_col_list(ds, inshead, "update", 0);
		recno += rle;
	}

	if (WT_COL_APPEND(page) != NULL) {
		__dmsg(ds, "%s\n", S2C(session)->sep);
		__debug_col_list(ds, WT_COL_APPEND(page), "append", 0);
	}

	return (0);
}

/*
 * __debug_page_row_int --
 *	Dump an in-memory WT_PAGE_ROW_INT page.
 */
static int
__debug_page_row_int(WT_DBG *ds, WT_PAGE *page, uint32_t flags)
{
	WT_ROW_REF *rref;
	uint32_t i;

	WT_ROW_REF_FOREACH(page, rref, i) {
		__debug_ikey(ds, rref->key);
		__dmsg(ds, "\t");
		__debug_ref(ds, &rref->ref);
	}

	if (!LF_ISSET(WT_DEBUG_TREE_WALK))
		return (0);

	WT_ROW_REF_FOREACH(page, rref, i)
		if (WT_ROW_REF_STATE(rref) == WT_REF_MEM)
			WT_RET(__debug_page_work(
			    ds, WT_ROW_REF_PAGE(rref), flags));
	return (0);
}

/*
 * __debug_page_row_leaf --
 *	Dump an in-memory WT_PAGE_ROW_LEAF page.
 */
static int
__debug_page_row_leaf(WT_DBG *ds, WT_PAGE *page)
{
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	WT_INSERT_HEAD *inshead;
	WT_ROW *rip;
	WT_UPDATE *upd;
	uint32_t i;

	unpack = &_unpack;

	/*
	 * Dump any K/V pairs inserted into the page before the first from-disk
	 * key on the page.
	 */
	if ((inshead = WT_ROW_INSERT_SMALLEST(page)) != NULL)
		__debug_row_list(ds, inshead);

	/* Dump the page's K/V pairs. */
	WT_ROW_FOREACH(page, rip, i) {
		if (__wt_off_page(page, rip->key))
			__debug_ikey(ds, rip->key);
		else {
			__wt_cell_unpack(rip->key, unpack);
			WT_RET(__debug_cell_data(ds, "K", unpack));
		}

		if ((cell = __wt_row_value(page, rip)) == NULL)
			__dmsg(ds, "\tV {}\n");
		else {
			__wt_cell_unpack(cell, unpack);
			WT_RET(__debug_cell_data(ds, "V", unpack));
		}

		if ((upd = WT_ROW_UPDATE(page, rip)) != NULL)
			__debug_update(ds, upd, 0);

		if ((inshead = WT_ROW_INSERT(page, rip)) != NULL)
			__debug_row_list(ds, inshead);
	}

	return (0);
}

/*
 * __debug_col_list --
 *	Dump a column-store skiplist.
 */
static void
__debug_col_list(
    WT_DBG *ds, WT_INSERT_HEAD *inshead, const char *tag, int hexbyte)
{
	WT_INSERT *ins;

	WT_SKIP_FOREACH(ins, inshead) {
		__dmsg(ds,
		    "\t%s %" PRIu64 "\n", tag, WT_INSERT_RECNO(ins));
		__debug_update(ds, ins->upd, hexbyte);
	}
}

/*
 * __debug_row_list --
 *	Dump an insert array.
 */
static void
__debug_row_list(WT_DBG *ds, WT_INSERT_HEAD *inshead)
{
	WT_INSERT *ins;

	WT_SKIP_FOREACH(ins, inshead) {
		__debug_item(ds,
		    "insert", WT_INSERT_KEY(ins), WT_INSERT_KEY_SIZE(ins));
		__debug_update(ds, ins->upd, 0);
	}
}

/*
 * __debug_update --
 *	Dump an update array.
 */
static void
__debug_update(WT_DBG *ds, WT_UPDATE *upd, int hexbyte)
{
	for (; upd != NULL; upd = upd->next)
		if (WT_UPDATE_DELETED_ISSET(upd))
			__dmsg(ds, "\tvalue {deleted}\n");
		else if (hexbyte) {
			__dmsg(ds, "\t{");
			__debug_hex_byte(ds,
			    ((uint8_t *)WT_UPDATE_DATA(upd))[0]);
			__dmsg(ds, "}\n");
		} else
			__debug_item(ds,
			    "value", WT_UPDATE_DATA(upd), upd->size);
}

/*
 * __debug_dsk_cell --
 *	Dump a page of WT_CELL's.
 */
static int
__debug_dsk_cell(WT_DBG *ds, WT_PAGE_DISK *dsk)
{
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	uint32_t i;

	unpack = &_unpack;

	WT_CELL_FOREACH(dsk, cell, unpack, i) {
		__wt_cell_unpack(cell, unpack);
		WT_RET(__debug_cell(ds, unpack));
	}
	return (0);
}

/*
 * __debug_cell --
 *	Dump a single WT_CELL.
 */
static int
__debug_cell(WT_DBG *ds, WT_CELL_UNPACK *unpack)
{
	WT_SESSION_IMPL *session;

	session = ds->session;

	__dmsg(ds, "\t%s: len %" PRIu32,
	    __wt_cell_type_string(unpack->raw), unpack->size);

	switch (unpack->type) {
	case WT_CELL_DEL:
	case WT_CELL_VALUE:
		if (unpack->rle != 0)
			__dmsg(ds, ", rle: %" PRIu64, unpack->rle);
		break;
	case WT_CELL_KEY:
		__dmsg(ds, ", pfx: %" PRIu8, unpack->prefix);
		break;
	case WT_CELL_KEY_OVFL:
	case WT_CELL_OFF:
	case WT_CELL_VALUE_OVFL:
		__dmsg(ds, ", offpage: addr %" PRIu32 ", size %" PRIu32,
		    unpack->off.addr, unpack->off.size);
		break;
	WT_ILLEGAL_FORMAT(session);
	}
	__dmsg(ds, "\n");

	return (__debug_cell_data(ds, NULL, unpack));
}

/*
 * __debug_dsk_col_int --
 *	Dump a WT_PAGE_COL_INT page.
 */
static void
__debug_dsk_col_int(WT_DBG *ds, WT_PAGE_DISK *dsk)
{
	WT_OFF_RECORD *off_record;
	uint32_t i;

	WT_OFF_FOREACH(dsk, off_record, i)
		__dmsg(ds, "\toffpage: addr %" PRIu32 ", size %" PRIu32
		    ", starting recno %" PRIu64 "\n",
		    off_record->addr, off_record->size, WT_RECNO(off_record));
}

/*
 * __debug_dsk_col_fix --
 *	Dump a WT_PAGE_COL_FIX page.
 */
static void
__debug_dsk_col_fix(WT_DBG *ds, WT_PAGE_DISK *dsk)
{
	WT_BTREE *btree;
	uint32_t i;
	uint8_t v;

	btree = ds->session->btree;

	WT_FIX_FOREACH(btree, dsk, v, i) {
		__dmsg(ds, "\t{");
		__debug_hex_byte(ds, v);
		__dmsg(ds, "}\n");
	}
}

/*
 * __debug_cell_data --
 *	Dump a single cell's data in debugging mode.
 */
static int
__debug_cell_data(WT_DBG *ds, const char *tag, WT_CELL_UNPACK *unpack)
{
	WT_BUF *tmp;
	WT_SESSION_IMPL *session;
	size_t size;
	const uint8_t *p;
	int ret;

	session = ds->session;
	tmp = NULL;
	ret = 0;

	/*
	 * Column-store references to deleted cells return a NULL cell
	 * reference.
	 */
	if (unpack == NULL)
		goto deleted;

	switch (unpack->type) {
	case WT_CELL_KEY:
	case WT_CELL_KEY_OVFL:
	case WT_CELL_VALUE:
	case WT_CELL_VALUE_OVFL:
		WT_ERR(__wt_scr_alloc(session, 0, &tmp));
		WT_ERR(__wt_cell_unpack_copy(session, unpack, tmp));
		p = tmp->data;
		size = tmp->size;
		break;
	case WT_CELL_DEL:
deleted:	p = (uint8_t *)"deleted";
		size = strlen("deleted");
		break;
	case WT_CELL_OFF:
		p = (uint8_t *)"offpage";
		size = strlen("offpage");
		break;
	WT_ILLEGAL_FORMAT_ERR(session);
	}

	__debug_item(ds, tag, p, size);

err:	__wt_scr_free(&tmp);
	return (ret);
}

/*
 * __debug_ikey --
 *	Dump a single WT_IKEY in debugging mode, with an optional tag.
 */
static void
__debug_ikey(WT_DBG *ds, WT_IKEY *ikey)
{
	__debug_item(ds, "K", WT_IKEY_DATA(ikey), ikey->size);
}

/*
 * __debug_item --
 *	Dump a single data/size pair, with an optional tag.
 */
static void
__debug_item(WT_DBG *ds, const char *tag, const void *data, size_t size)
{
	__dmsg(ds,
	    "\t%s%s{", tag == NULL ? "" : tag, tag == NULL ? "" : " ");
	__debug_byte_string(ds, data, size);
	__dmsg(ds, "}\n");
}

/*
 * __debug_ref --
 *	Print out a page's in-memory WT_REF state.
 */
static void
__debug_ref(WT_DBG *ds, WT_REF *ref)
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
		__dmsg(ds, "NoAddr");
	else
		__dmsg(ds, "%" PRIu32 "/%" PRIu32, ref->addr, ref->size);

	__dmsg(ds, ": %s", s);
	if (ref->state == WT_REF_MEM)
		__dmsg(ds, "(%p)", ref->page);
	__dmsg(ds, "\n");
}

/*
 * __debug_byte_string --
 *	Output a single byte string in printable characters, where possible.
 */
static void
__debug_byte_string(WT_DBG *ds, const uint8_t *data, size_t size)
{
	int ch;

	for (; size > 0; --size, ++data) {
		ch = data[0];
		if (isprint(ch))
			__dmsg(ds, "%c", ch);
		else
			__debug_hex_byte(ds, data[0]);
	}
}
#endif
