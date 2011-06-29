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
 * The debugging output has to use its own event handlers because its output
 * isn't line-oriented.
 */
typedef struct __debug_event {
	/*
	 * The standard structure must appear first in the structure -- this
	 * structure is passed as if it's a WT_EVENT_HANDLER structure.
	 */
	WT_EVENT_HANDLER  event_handler;	/* Standard structure */
	WT_EVENT_HANDLER *save_event_handler;	/* Saved event handlers */

	WT_SESSION_IMPL *session;		/* Enclosing session */

	WT_BUF	*msg;				/* Buffered message */
	FILE	*fp;				/* Output file stream */
} WT_DEBUG_EVENT;

#ifdef HAVE_DIAGNOSTIC
static int  __wt_debug_cell(WT_SESSION_IMPL *, WT_CELL *);
static int  __wt_debug_cell_data(WT_SESSION_IMPL *, const char *, WT_CELL *);
static void __wt_debug_col_insert(WT_SESSION_IMPL *, WT_INSERT *);
static int  __wt_debug_dsk_cell(WT_SESSION_IMPL *, WT_PAGE_DISK *);
static void __wt_debug_dsk_col_fix(
		WT_SESSION_IMPL *, WT_BTREE *, WT_PAGE_DISK *);
static void __wt_debug_dsk_col_int(WT_SESSION_IMPL *, WT_PAGE_DISK *);
static void __wt_debug_dsk_col_rle(
		WT_SESSION_IMPL *, WT_BTREE *, WT_PAGE_DISK *);
static int  __wt_debug_event_config(
		WT_SESSION_IMPL *, WT_DEBUG_EVENT *, const char *);
static void __wt_debug_event_reset(WT_SESSION_IMPL *, WT_DEBUG_EVENT *);
static int  __wt_debug_handle_message(WT_EVENT_HANDLER *, const char *);
static int  __wt_debug_handle_message_flush(WT_DEBUG_EVENT *);
static void __wt_debug_ikey(WT_SESSION_IMPL *, WT_IKEY *);
static void __wt_debug_item(
		WT_SESSION_IMPL *, const char *, const void *, uint32_t);
static void __wt_debug_page_col_fix(WT_SESSION_IMPL *, WT_PAGE *);
static int  __wt_debug_page_col_int(WT_SESSION_IMPL *, WT_PAGE *, uint32_t);
static void __wt_debug_page_col_rle(WT_SESSION_IMPL *, WT_PAGE *);
static int  __wt_debug_page_col_var(WT_SESSION_IMPL *, WT_PAGE *);
static void __wt_debug_page_flags(WT_SESSION_IMPL *, WT_PAGE *);
static int  __wt_debug_page_row_int(WT_SESSION_IMPL *, WT_PAGE *, uint32_t);
static int  __wt_debug_page_row_leaf(WT_SESSION_IMPL *, WT_PAGE *);
static int  __wt_debug_page_work(WT_SESSION_IMPL *, WT_PAGE *, uint32_t);
static void __wt_debug_ref(WT_SESSION_IMPL *, WT_REF *);
static void __wt_debug_row_insert(WT_SESSION_IMPL *, WT_INSERT *);
static void __wt_debug_update(WT_SESSION_IMPL *, WT_UPDATE *);

/*
 * __wt_debug_event_config --
 *	Configure the event handler for debugging information.
 */
static int
__wt_debug_event_config(
    WT_SESSION_IMPL *session, WT_DEBUG_EVENT *dbe, const char *ofile)
{
	memset(dbe, 0, sizeof(WT_DEBUG_EVENT));

	dbe->session = session;

	/* Save a reference to the original event handlers. */
	dbe->save_event_handler = session->event_handler;

	/*
	 * Copy the original event handlers, then replace the message handler
	 * with our own.
	 */
	dbe->event_handler = *session->event_handler;
	dbe->event_handler.handle_message = __wt_debug_handle_message;

	/*
	 * If we weren't given a file, we use the default event handler, and
	 * we'll have to buffer messages.
	 */
	if (ofile == NULL)
		WT_RET(__wt_scr_alloc(session, 512, &dbe->msg));
	else
		if ((dbe->fp = fopen(ofile, "w")) == NULL)
			return (WT_ERROR);

	/* Push our event handler in front of the standard ones. */
	session->event_handler = (WT_EVENT_HANDLER *)dbe;

	return (0);
}

/*
 * __wt_debug_event_reset --
 *	Unconfigure the event handler for debugging information.
 */
static void
__wt_debug_event_reset(WT_SESSION_IMPL *session, WT_DEBUG_EVENT *dbe)
{
	/* Close any file we opened. */
	if (dbe->fp != NULL)
		(void)fclose(dbe->fp);

	/*
	 * Discard the buffer -- it shouldn't have anything in it, but might
	 * as well be cautious.
	 */
	if (dbe->msg != NULL) {
		(void)__wt_debug_handle_message_flush(dbe);
		__wt_scr_release(&dbe->msg);
	}

	/* Reset the event handlers. */
	session->event_handler = dbe->save_event_handler;
}

/*
 * __wt_debug_handle_message --
 *	Debug event handler when output is re-directed to a file.
 */
static int
__wt_debug_handle_message(WT_EVENT_HANDLER *handler, const char *message)
{
	WT_BUF *msg;
	WT_DEBUG_EVENT *dbe;
	size_t len;

	dbe = (WT_DEBUG_EVENT *)handler;
	msg = dbe->msg;

	/*
	 * Debug output isn't line-oriented, that is, message handler calls are
	 * not necessarily terminated with a newline character.  If dumping to
	 * a file, it's easy, we'll eventually see a newline.  If dumping to the
	 * standard event handlers, save away the message, and output it once
	 * we see a newline.
	 */
	if (dbe->fp != NULL) {
		(void)fprintf(dbe->fp, "%s", message);
		return (0);
	}

	len = strlen(message);
	if (msg->size + len > msg->mem_size)
		WT_RET(__wt_buf_grow(
		    dbe->session, msg, msg->mem_size + len + 128));
	memcpy((uint8_t *)msg->data + msg->size, message, len);
	msg->size += len;

	if (((uint8_t *)msg->data)[msg->size - 1] == '\n')
		return (__wt_debug_handle_message_flush(dbe));

	return (0);
}

/*
 * __wt_debug_handle_message_flush --
 *	Flush the current message.
 */
static int
__wt_debug_handle_message_flush(WT_DEBUG_EVENT *dbe)
{
	WT_BUF *msg;
	int ret;

	msg = dbe->msg;

	if (msg->size == 0)
		return (0);

	((uint8_t *)msg->data)[msg->size - 1] = '\0';
	ret = dbe->
	    save_event_handler->handle_message(&dbe->event_handler, msg->data);
	msg->data = msg->mem;
	msg->size = 0;

	return (ret);
}

/*
 * __wt_debug_addr --
 *	Read and dump a disk page in debugging mode.
 */
int
__wt_debug_addr(
    WT_SESSION_IMPL *session, uint32_t addr, uint32_t size, const char *ofile)
{
	WT_DEBUG_EVENT dbe;
	int ret;
	char *bp;

	ret = 0;

	WT_RET(__wt_debug_event_config(session, &dbe, ofile));

	WT_RET(__wt_calloc_def(session, (size_t)size, &bp));
	WT_ERR(__wt_disk_read(session, (WT_PAGE_DISK *)bp, addr, size));
	ret = __wt_debug_disk(session, (WT_PAGE_DISK *)bp, NULL);
err:	__wt_free(session, bp);

	__wt_debug_event_reset(session, &dbe);

	return (ret);
}

/*
 * __wt_debug_disk --
 *	Dump a disk page in debugging mode.
 */
int
__wt_debug_disk(WT_SESSION_IMPL *session, WT_PAGE_DISK *dsk, const char *ofile)
{
	WT_BTREE *btree;
	WT_DEBUG_EVENT dbe;
	int ret;

	btree = session->btree;
	ret = 0;

	WT_RET(__wt_debug_event_config(session, &dbe, ofile));

	switch (dsk->type) {
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_INT:
	case WT_PAGE_COL_RLE:
	case WT_PAGE_COL_VAR:
		__wt_msg(session, "%s page: starting recno %" PRIu64
		    ", entries %" PRIu32 ", lsn %" PRIu32 "/%" PRIu32 "\n",
		    __wt_page_type_string(dsk->type),
		    dsk->recno, dsk->u.entries,
		    WT_LSN_FILE(dsk->lsn), WT_LSN_OFFSET(dsk->lsn));
		break;
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		__wt_msg(session, "%s page: entries %" PRIu32
		    ", lsn %" PRIu32 "/%" PRIu32 "\n",
		    __wt_page_type_string(dsk->type), dsk->u.entries,
		    WT_LSN_FILE(dsk->lsn), WT_LSN_OFFSET(dsk->lsn));
		break;
	case WT_PAGE_OVFL:
		__wt_msg(session, "%s page: data size %" PRIu32
		    ", lsn %" PRIu32 "/%" PRIu32 "\n",
		    __wt_page_type_string(dsk->type), dsk->u.datalen,
		    WT_LSN_FILE(dsk->lsn), WT_LSN_OFFSET(dsk->lsn));
		break;
	WT_ILLEGAL_FORMAT(session);
	}

	switch (dsk->type) {
	case WT_PAGE_COL_FIX:
		__wt_debug_dsk_col_fix(session, btree, dsk);
		break;
	case WT_PAGE_COL_INT:
		__wt_debug_dsk_col_int(session, dsk);
		break;
	case WT_PAGE_COL_RLE:
		__wt_debug_dsk_col_rle(session, btree, dsk);
		break;
	case WT_PAGE_COL_VAR:
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		ret = __wt_debug_dsk_cell(session, dsk);
		break;
	default:
		break;
	}

	__wt_debug_event_reset(session, &dbe);

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
	WT_DEBUG_EVENT dbe;
	int ret;

	WT_RET(__wt_debug_event_config(session, &dbe, ofile));

	/* A NULL page starts at the top of the tree -- it's a convenience. */
	if (page == NULL)
		page = session->btree->root_page.page;

	ret = __wt_debug_page_work(
	    session, page, WT_DEBUG_TREE_LEAF | WT_DEBUG_TREE_WALK);

	__wt_debug_event_reset(session, &dbe);

	return (ret);
}

/*
 * __wt_debug_tree --
 *	Dump the in-memory information for a tree, not including leaf pages.
 */
int
__wt_debug_tree(WT_SESSION_IMPL *session, WT_PAGE *page, const char *ofile)
{
	WT_DEBUG_EVENT dbe;
	int ret;

	WT_RET(__wt_debug_event_config(session, &dbe, ofile));

	/* A NULL page starts at the top of the tree -- it's a convenience. */
	if (page == NULL)
		page = session->btree->root_page.page;

	ret = __wt_debug_page_work(session, page, WT_DEBUG_TREE_WALK);

	__wt_debug_event_reset(session, &dbe);

	return (ret);
}

/*
 * __wt_debug_page --
 *	Dump the in-memory information for a page.
 */
int
__wt_debug_page(WT_SESSION_IMPL *session, WT_PAGE *page, const char *ofile)
{
	WT_DEBUG_EVENT dbe;
	int ret;

	WT_RET(__wt_debug_event_config(session, &dbe, ofile));

	ret = __wt_debug_page_work(session, page, WT_DEBUG_TREE_LEAF);

	__wt_debug_event_reset(session, &dbe);

	return (ret);
}

/*
 * __wt_debug_page_work --
 *	Dump the in-memory information for an in-memory page.
 */
static int
__wt_debug_page_work(WT_SESSION_IMPL *session, WT_PAGE *page, uint32_t flags)
{
	WT_BTREE *btree;

	btree = session->btree;

	__wt_msg(session, "%p: ", page);
	if (WT_PADDR(page) == WT_ADDR_INVALID)
		__wt_msg(session, "[NoAddr]");
	else
		__wt_msg(session, "[%" PRIu32 "-%" PRIu32 "]", WT_PADDR(page),
		    WT_PADDR(page) + (WT_PSIZE(page) / btree->allocsize) - 1);
	__wt_msg(session, "/%" PRIu32 " %s",
	    WT_PSIZE(page), __wt_page_type_string(page->type));

	switch (page->type) {
	case WT_PAGE_COL_INT:
		__wt_msg(session, " recno %" PRIu64, page->u.col_int.recno);
		break;
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_RLE:
	case WT_PAGE_COL_VAR:
		__wt_msg(session, " recno %" PRIu64, page->u.col_leaf.recno);
		break;
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		break;
	WT_ILLEGAL_FORMAT(session);
	}

	__wt_debug_page_flags(session, page);

	/* Skip bulk-loaded pages. */
	if (F_ISSET(page, WT_PAGE_BULK_LOAD)) {
		__wt_msg(session, "bulk-loaded page -- skipped\n");
		return (0);
	}

	/* Dump the page. */
	switch (page->type) {
	case WT_PAGE_COL_FIX:
		if (LF_ISSET(WT_DEBUG_TREE_LEAF))
			__wt_debug_page_col_fix(session, page);
		break;
	case WT_PAGE_COL_INT:
		WT_RET(__wt_debug_page_col_int(session, page, flags));
		break;
	case WT_PAGE_COL_RLE:
		if (LF_ISSET(WT_DEBUG_TREE_LEAF))
			__wt_debug_page_col_rle(session, page);
		break;
	case WT_PAGE_COL_VAR:
		if (LF_ISSET(WT_DEBUG_TREE_LEAF))
			WT_RET(__wt_debug_page_col_var(session, page));
		break;
	case WT_PAGE_ROW_INT:
		WT_RET(__wt_debug_page_row_int(session, page, flags));
		break;
	case WT_PAGE_ROW_LEAF:
		if (LF_ISSET(WT_DEBUG_TREE_LEAF))
			WT_RET(__wt_debug_page_row_leaf(session, page));
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
__wt_debug_page_flags(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	__wt_msg(session,
	    " (%s", WT_PAGE_IS_MODIFIED(page) ? "dirty" : "clean");
	if (WT_PAGE_IS_ROOT(page))
		__wt_msg(session, ", root");
	if (F_ISSET(page, WT_PAGE_BULK_LOAD))
		__wt_msg(session, ", bulk-loaded");
	if (F_ISSET(page, WT_PAGE_CACHE_COUNTED))
		__wt_msg(session, ", cache-counted");
	if (F_ISSET(page, WT_PAGE_DELETED))
		__wt_msg(session, ", deleted");
	if (F_ISSET(page, WT_PAGE_INITIAL_EMPTY))
		__wt_msg(session, ", initial-empty");
	if (F_ISSET(page, WT_PAGE_PINNED))
		__wt_msg(session, ", pinned");
	if (F_ISSET(page, WT_PAGE_SPLIT))
		__wt_msg(session, ", split");
	__wt_msg(session, ")\n");
}

/*
 * __wt_debug_page_col_fix --
 *	Dump an in-memory WT_PAGE_COL_FIX page.
 */
static void
__wt_debug_page_col_fix(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_COL *cip;
	WT_UPDATE *upd;
	uint32_t fixed_len, i;
	void *cipvalue;

	fixed_len = session->btree->fixed_len;

	WT_COL_FOREACH(page, cip, i) {
		cipvalue = WT_COL_PTR(page, cip);
		__wt_msg(session, "\tV {");
		if (WT_FIX_DELETE_ISSET(cipvalue))
			__wt_msg(session, "deleted");
		else
			__wt_msg_byte_string(session, cipvalue, fixed_len);
		__wt_msg(session, "}\n");

		if ((upd = WT_COL_UPDATE(page, cip)) != NULL)
			__wt_debug_update(session, upd);
	}
}

/*
 * __wt_debug_page_col_int --
 *	Dump an in-memory WT_PAGE_COL_INT page.
 */
static int
__wt_debug_page_col_int(WT_SESSION_IMPL *session, WT_PAGE *page, uint32_t flags)
{
	WT_COL_REF *cref;
	uint32_t i;

	WT_COL_REF_FOREACH(page, cref, i) {
		__wt_msg(session, "\trecno %" PRIu64 ", ", cref->recno);
		__wt_debug_ref(session, &cref->ref);
	}

	if (!LF_ISSET(WT_DEBUG_TREE_WALK))
		return (0);

	WT_COL_REF_FOREACH(page, cref, i)
		if (WT_COL_REF_STATE(cref) == WT_REF_MEM)
			WT_RET(__wt_debug_page_work(
			    session, WT_COL_REF_PAGE(cref), flags));
	return (0);
}

/*
 * __wt_debug_page_col_rle --
 *	Dump an in-memory WT_PAGE_COL_RLE page.
 */
static void
__wt_debug_page_col_rle(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_COL *cip;
	WT_INSERT *ins;
	uint32_t fixed_len, i;
	void *cipvalue;

	fixed_len = session->btree->fixed_len;

	WT_COL_FOREACH(page, cip, i) {
		cipvalue = WT_COL_PTR(page, cip);
		__wt_msg(session,
		    "\trepeat %" PRIu32 " {", WT_RLE_REPEAT_COUNT(cipvalue));
		if (WT_FIX_DELETE_ISSET(WT_RLE_REPEAT_DATA(cipvalue)))
			__wt_msg(session, "deleted");
		else
			__wt_msg_byte_string(
			    session, WT_RLE_REPEAT_DATA(cipvalue), fixed_len);
		__wt_msg(session, "}\n");

		if ((ins = WT_COL_INSERT(page, cip)) != NULL)
			__wt_debug_col_insert(session, ins);
	}
}

/*
 * __wt_debug_page_col_var --
 *	Dump an in-memory WT_PAGE_COL_VAR page.
 */
static int
__wt_debug_page_col_var(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_COL *cip;
	WT_UPDATE *upd;
	uint32_t i;

	WT_COL_FOREACH(page, cip, i) {
		WT_RET(
		    __wt_debug_cell_data(session, "V", WT_COL_PTR(page, cip)));

		if ((upd = WT_COL_UPDATE(page, cip)) != NULL)
			__wt_debug_update(session, upd);
	}
	return (0);
}

/*
 * __wt_debug_page_row_int --
 *	Dump an in-memory WT_PAGE_ROW_INT page.
 */
static int
__wt_debug_page_row_int(WT_SESSION_IMPL *session, WT_PAGE *page, uint32_t flags)
{
	WT_ROW_REF *rref;
	uint32_t i;

	WT_ROW_REF_FOREACH(page, rref, i) {
		__wt_debug_ikey(session, rref->key);
		__wt_msg(session, "\t");
		__wt_debug_ref(session, &rref->ref);
	}

	if (!LF_ISSET(WT_DEBUG_TREE_WALK))
		return (0);

	WT_ROW_REF_FOREACH(page, rref, i)
		if (WT_ROW_REF_STATE(rref) == WT_REF_MEM)
			WT_RET(__wt_debug_page_work(
			    session, WT_ROW_REF_PAGE(rref), flags));
	return (0);
}

/*
 * __wt_debug_page_row_leaf --
 *	Dump an in-memory WT_PAGE_ROW_LEAF page.
 */
static int
__wt_debug_page_row_leaf(WT_SESSION_IMPL *session, WT_PAGE *page)
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
		__wt_debug_row_insert(session, ins);

	/* Dump the page's K/V pairs. */
	WT_ROW_FOREACH(page, rip, i) {
		if (__wt_off_page(page, rip->key))
			__wt_debug_ikey(session, rip->key);
		else
			WT_RET(__wt_debug_cell_data(session, "K", rip->key));

		if ((cell = __wt_row_value(page, rip)) == NULL)
			__wt_msg(session, "\tV {}\n");
		else
			WT_RET(__wt_debug_cell_data(session, "V", cell));

		if ((upd = WT_ROW_UPDATE(page, rip)) != NULL)
			__wt_debug_update(session, upd);

		if ((ins = WT_ROW_INSERT(page, rip)) != NULL)
			__wt_debug_row_insert(session, ins);
	}

	return (0);
}

/*
 * __wt_debug_col_insert --
 *	Dump an RLE column-store insert array.
 */
static void
__wt_debug_col_insert(WT_SESSION_IMPL *session, WT_INSERT *ins)
{
	for (; ins != NULL; ins = ins->next) {
		__wt_msg(session,
		    "\tinsert %" PRIu64 "\n", WT_INSERT_RECNO(ins));
		__wt_debug_update(session, ins->upd);
	}
}

/*
 * __wt_debug_row_insert --
 *	Dump an insert array.
 */
static void
__wt_debug_row_insert(WT_SESSION_IMPL *session, WT_INSERT *ins)
{
	for (; ins != NULL; ins = ins->next) {
		__wt_debug_item(session,
		    "insert", WT_INSERT_KEY(ins), WT_INSERT_KEY_SIZE(ins));
		__wt_debug_update(session, ins->upd);
	}
}

/*
 * __wt_debug_update --
 *	Dump an update array.
 */
static void
__wt_debug_update(WT_SESSION_IMPL *session, WT_UPDATE *upd)
{
	for (; upd != NULL; upd = upd->next)
		if (WT_UPDATE_DELETED_ISSET(upd))
			__wt_msg(session, "\tupdate: {deleted}\n");
		else
			__wt_debug_item(session,
			    "update", WT_UPDATE_DATA(upd), upd->size);
}

/*
 * __wt_debug_dsk_cell --
 *	Dump a page of WT_CELL's.
 */
static int
__wt_debug_dsk_cell(WT_SESSION_IMPL *session, WT_PAGE_DISK *dsk)
{
	WT_CELL *cell;
	uint32_t i;

	WT_CELL_FOREACH(dsk, cell, i)
		WT_RET(__wt_debug_cell(session, cell));
	return (0);
}

/*
 * __wt_debug_cell --
 *	Dump a single WT_CELL.
 */
static int
__wt_debug_cell(WT_SESSION_IMPL *session, WT_CELL *cell)
{
	WT_OFF off;

	__wt_msg(session, "\t%s: len %" PRIu32,
	    __wt_cell_type_string(cell), __wt_cell_datalen(cell));

	switch (__wt_cell_type(cell)) {
	case WT_CELL_DATA:
	case WT_CELL_DEL:
		break;
	case WT_CELL_KEY:
		__wt_msg(session, ", pfx: %u", __wt_cell_prefix(cell));
		break;
	case WT_CELL_DATA_OVFL:
	case WT_CELL_KEY_OVFL:
	case WT_CELL_OFF:
		__wt_cell_off(cell, &off);
		__wt_msg(session, ", offpage: addr %" PRIu32 ", size %" PRIu32,
		    off.addr, off.size);
		break;
	WT_ILLEGAL_FORMAT(session);
	}
	__wt_msg(session, "\n");

	return (__wt_debug_cell_data(session, NULL, cell));
}

/*
 * __wt_debug_dsk_col_int --
 *	Dump a WT_PAGE_COL_INT page.
 */
static void
__wt_debug_dsk_col_int(WT_SESSION_IMPL *session, WT_PAGE_DISK *dsk)
{
	WT_OFF_RECORD *off_record;
	uint32_t i;

	WT_OFF_FOREACH(dsk, off_record, i)
		__wt_msg(session, "\toffpage: addr %" PRIu32 ", size %" PRIu32
		    ", starting recno %" PRIu64 "\n",
		    off_record->addr, off_record->size, WT_RECNO(off_record));
}

/*
 * __wt_debug_dsk_col_fix --
 *	Dump a WT_PAGE_COL_FIX page.
 */
static void
__wt_debug_dsk_col_fix(
    WT_SESSION_IMPL *session, WT_BTREE *btree, WT_PAGE_DISK *dsk)
{
	uint32_t i;
	uint8_t *p;

	WT_FIX_FOREACH(btree, dsk, p, i) {
		__wt_msg(session, "\t{");
		if (WT_FIX_DELETE_ISSET(p))
			__wt_msg(session, "deleted");
		else
			__wt_msg_byte_string(session, p, btree->fixed_len);
		__wt_msg(session, "}\n");
	}
}

/*
 * __wt_debug_dsk_col_rle --
 *	Dump a WT_PAGE_COL_RLE page.
 */
static void
__wt_debug_dsk_col_rle(
    WT_SESSION_IMPL *session, WT_BTREE *btree, WT_PAGE_DISK *dsk)
{
	uint32_t i;
	uint8_t *p;

	WT_RLE_REPEAT_FOREACH(btree, dsk, p, i) {
		__wt_msg(session,
		    "\trepeat %" PRIu32 " {", WT_RLE_REPEAT_COUNT(p));
		if (WT_FIX_DELETE_ISSET(WT_RLE_REPEAT_DATA(p)))
			__wt_msg(session, "deleted");
		else
			__wt_msg_byte_string(
			    session, WT_RLE_REPEAT_DATA(p), btree->fixed_len);
		__wt_msg(session, "}\n");
	}
}

/*
 * __wt_debug_cell_data --
 *	Dump a single cell's data in debugging mode.
 */
static int
__wt_debug_cell_data(WT_SESSION_IMPL *session, const char *tag, WT_CELL *cell)
{
	WT_BUF *tmp;
	uint32_t size;
	const uint8_t *p;
	int ret;

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

	__wt_debug_item(session, tag, p, size);

err:	if (tmp != NULL)
		__wt_scr_release(&tmp);
	return (ret);
}

/*
 * __wt_debug_ikey --
 *	Dump a single WT_IKEY in debugging mode, with an optional tag.
 */
static void
__wt_debug_ikey(WT_SESSION_IMPL *session, WT_IKEY *ikey)
{
	__wt_debug_item(session, "K", WT_IKEY_DATA(ikey), ikey->size);
}

/*
 * __wt_debug_item --
 *	Dump a single data/size pair, with an optional tag.
 */
static void
__wt_debug_item(
    WT_SESSION_IMPL *session, const char *tag, const void *data, uint32_t size)
{
	__wt_msg(session,
	    "\t%s%s{", tag == NULL ? "" : tag, tag == NULL ? "" : " ");
	__wt_msg_byte_string(session, data, size);
	__wt_msg(session, "}\n");
}

/*
 * __wt_debug_ref --
 *	Print out a page's in-memory WT_REF state.
 */
static void
__wt_debug_ref(WT_SESSION_IMPL *session, WT_REF *ref)
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
		__wt_msg(session, "NoAddr");
	else
		__wt_msg(session, "%" PRIu32 "/%" PRIu32, ref->addr, ref->size);

	__wt_msg(session, ": %s", s);
	if (ref->state == WT_REF_MEM)
		__wt_msg(session, "(%p)", ref->page);
	__wt_msg(session, "\n");
}
#endif
