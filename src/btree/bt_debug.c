/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

#ifdef HAVE_DIAGNOSTIC
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
	WT_ITEM		*msg;			/* Buffered message */

	WT_ITEM		*tmp;			/* Temporary space */
} WT_DBG;

/* Diagnostic output separator. */
static const char *sep = "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=\n";

static int  __debug_cell(WT_DBG *, WT_PAGE_HEADER *, WT_CELL_UNPACK *);
static int  __debug_cell_data(WT_DBG *, const char *, WT_CELL_UNPACK *);
static void __debug_col_skip(WT_DBG *, WT_INSERT_HEAD *, const char *, int);
static int  __debug_config(WT_SESSION_IMPL *, WT_DBG *, const char *);
static int  __debug_dsk_cell(WT_DBG *, WT_PAGE_HEADER *);
static void __debug_dsk_col_fix(WT_DBG *, WT_PAGE_HEADER *);
static void __debug_ikey(WT_DBG *, WT_IKEY *);
static void __debug_item(WT_DBG *, const char *, const void *, size_t);
static int  __debug_page(WT_DBG *, WT_PAGE *, uint32_t);
static void __debug_page_col_fix(WT_DBG *, WT_PAGE *);
static int  __debug_page_col_int(WT_DBG *, WT_PAGE *, uint32_t);
static int  __debug_page_col_var(WT_DBG *, WT_PAGE *);
static int  __debug_page_hdr(WT_DBG *, WT_PAGE *);
static int  __debug_page_modify(WT_DBG *, WT_PAGE *);
static int  __debug_page_row_int(WT_DBG *, WT_PAGE *, uint32_t);
static int  __debug_page_row_leaf(WT_DBG *, WT_PAGE *);
static int  __debug_ref(WT_DBG *, WT_REF *, WT_PAGE *);
static void __debug_row_skip(WT_DBG *, WT_INSERT_HEAD *);
static int  __debug_tree(WT_SESSION_IMPL *, WT_PAGE *, const char *, uint32_t);
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

	WT_RET(__wt_scr_alloc(session, 512, &ds->tmp));

	/*
	 * If we weren't given a file, we use the default event handler, and
	 * we'll have to buffer messages.
	 */
	if (ofile == NULL)
		return (__wt_scr_alloc(session, 512, &ds->msg));

	/* If we're using a file, flush on each line. */
	if ((ds->fp = fopen(ofile, "w")) == NULL)
		WT_RET_MSG(session, __wt_errno(), "%s", ofile);

	(void)setvbuf(ds->fp, NULL, _IOLBF, 0);
	return (0);
}

/*
 * __dmsg_wrapup --
 *	Flush any remaining output, release resources.
 */
static void
__dmsg_wrapup(WT_DBG *ds)
{
	WT_SESSION_IMPL *session;
	WT_ITEM *msg;

	session = ds->session;
	msg = ds->msg;

	__wt_scr_free(&ds->tmp);

	/*
	 * Discard the buffer -- it shouldn't have anything in it, but might
	 * as well be cautious.
	 */
	if (msg != NULL) {
		if (msg->size != 0)
			(void)__wt_msg(session, "%s", (char *)msg->mem);
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
	WT_ITEM *msg;
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
			(void)__wt_msg(session, "%s", (char *)msg->mem);
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
 *	Read and dump a disk page in debugging mode, using an addr/size pair.
 */
int
__wt_debug_addr(WT_SESSION_IMPL *session,
    const uint8_t *addr, uint32_t addr_size, const char *ofile)
{
	WT_BM *bm;
	WT_DECL_ITEM(buf);
	WT_DECL_RET;

	bm = S2BT(session)->bm;

	WT_RET(__wt_scr_alloc(session, 1024, &buf));
	WT_ERR(bm->read(bm, session, buf, addr, addr_size));
	ret = __wt_debug_disk(session, buf->mem, ofile);

err:	__wt_scr_free(&buf);
	return (ret);
}

/*
 * __wt_debug_offset --
 *	Read and dump a disk page in debugging mode, using a file
 * offset/size/checksum triplet.
 */
int
__wt_debug_offset(WT_SESSION_IMPL *session,
     off_t offset, uint32_t size, uint32_t cksum, const char *ofile)
{
	WT_DECL_ITEM(buf);
	WT_DECL_RET;

	/*
	 * This routine depends on the default block manager's view of files,
	 * where an address consists of a file offset, length, and checksum.
	 * This is for debugging only.  Other block managers might not see a
	 * file or address the same way, that's why there's no block manager
	 * method.
	 */
	WT_RET(__wt_scr_alloc(session, 1024, &buf));
	WT_ERR(__wt_block_read_off(
	    session, S2BT(session)->bm->block, buf, offset, size, cksum));
	ret = __wt_debug_disk(session, buf->mem, ofile);

err:	__wt_scr_free(&buf);
	return (ret);
}

/*
 * __wt_debug_disk --
 *	Dump a disk page in debugging mode.
 */
int
__wt_debug_disk(
    WT_SESSION_IMPL *session, WT_PAGE_HEADER *dsk, const char *ofile)
{
	WT_DBG *ds, _ds;
	WT_DECL_RET;

	ds = &_ds;
	WT_RET(__debug_config(session, ds, ofile));

	__dmsg(ds, "%s page", __wt_page_type_string(dsk->type));
	switch (dsk->type) {
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_INT:
	case WT_PAGE_COL_VAR:
		__dmsg(ds, ", recno %" PRIu64, dsk->recno);
		/* FALLTHROUGH */
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		__dmsg(ds, ", entries %" PRIu32 "\n", dsk->u.entries);
		break;
	case WT_PAGE_OVFL:
		__dmsg(ds, ", datalen %" PRIu32 "\n", dsk->u.datalen);
		break;
	WT_ILLEGAL_VALUE(session);
	}

	switch (dsk->type) {
	case WT_PAGE_COL_FIX:
		__debug_dsk_col_fix(ds, dsk);
		break;
	case WT_PAGE_COL_INT:
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

/*
 * __debug_dsk_col_fix --
 *	Dump a WT_PAGE_COL_FIX page.
 */
static void
__debug_dsk_col_fix(WT_DBG *ds, WT_PAGE_HEADER *dsk)
{
	WT_BTREE *btree;
	uint32_t i;
	uint8_t v;

	btree = S2BT(ds->session);

	WT_FIX_FOREACH(btree, dsk, v, i) {
		__dmsg(ds, "\t{");
		__debug_hex_byte(ds, v);
		__dmsg(ds, "}\n");
	}
}

/*
 * __debug_dsk_cell --
 *	Dump a page of WT_CELL's.
 */
static int
__debug_dsk_cell(WT_DBG *ds, WT_PAGE_HEADER *dsk)
{
	WT_BTREE *btree;
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	uint32_t i;

	btree = S2BT(ds->session);
	unpack = &_unpack;

	WT_CELL_FOREACH(btree, dsk, cell, unpack, i) {
		__wt_cell_unpack(cell, dsk->type, unpack);
		WT_RET(__debug_cell(ds, dsk, unpack));
	}
	return (0);
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
	return (__debug_tree(
	    session, page, ofile, WT_DEBUG_TREE_LEAF | WT_DEBUG_TREE_WALK));
}

/*
 * __wt_debug_tree --
 *	Dump the in-memory information for a tree, not including leaf pages.
 */
int
__wt_debug_tree(WT_SESSION_IMPL *session, WT_PAGE *page, const char *ofile)
{
	return (__debug_tree(session, page, ofile, WT_DEBUG_TREE_WALK));
}

/*
 * __wt_debug_page --
 *	Dump the in-memory information for a page.
 */
int
__wt_debug_page(WT_SESSION_IMPL *session, WT_PAGE *page, const char *ofile)
{
	WT_DBG *ds, _ds;
	WT_DECL_RET;

	ds = &_ds;
	WT_RET(__debug_config(session, ds, ofile));

	ret = __debug_page(ds, page, WT_DEBUG_TREE_LEAF);

	__dmsg_wrapup(ds);

	return (ret);
}

/*
 * __debug_tree --
 *	Dump the in-memory information for a tree.
 */
static int
__debug_tree(
    WT_SESSION_IMPL *session, WT_PAGE *page, const char *ofile, uint32_t flags)
{
	WT_DBG *ds, _ds;
	WT_DECL_RET;

	ds = &_ds;
	WT_RET(__debug_config(session, ds, ofile));

	/* A NULL page starts at the top of the tree -- it's a convenience. */
	if (page == NULL)
		page = S2BT(session)->root_page;

	ret = __debug_page(ds, page, flags);

	__dmsg_wrapup(ds);

	return (ret);
}

/*
 * __debug_page --
 *	Dump the in-memory information for an in-memory page.
 */
static int
__debug_page(WT_DBG *ds, WT_PAGE *page, uint32_t flags)
{
	WT_SESSION_IMPL *session;

	session = ds->session;

	/* Dump the page header. */
	WT_RET(__debug_page_hdr(ds, page));

	/* Dump the page modification structure. */
	WT_RET(__debug_page_modify(ds, page));

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
	WT_ILLEGAL_VALUE(session);
	}

	return (0);
}

/*
 * __debug_page_hdr --
 *	Dump an in-memory page's metadata.
 */
static int
__debug_page_hdr(WT_DBG *ds, WT_PAGE *page)
{
	WT_SESSION_IMPL *session;

	session = ds->session;

	__dmsg(ds, "%p %s",
	    page, __wt_page_addr_string(session, ds->tmp, page));

	switch (page->type) {
	case WT_PAGE_COL_INT:
		__dmsg(ds, " recno %" PRIu64, page->u.intl.recno);
		break;
	case WT_PAGE_COL_FIX:
		__dmsg(ds, " recno %" PRIu64, page->u.col_fix.recno);
		break;
	case WT_PAGE_COL_VAR:
		__dmsg(ds, " recno %" PRIu64, page->u.col_var.recno);
		break;
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		break;
	WT_ILLEGAL_VALUE(session);
	}

	__dmsg(ds, ": %s", __wt_page_type_string(page->type));

	__dmsg(ds, " (%s", __wt_page_is_modified(page) ? "dirty" : "clean");
	if (F_ISSET_ATOMIC(page, WT_PAGE_BUILD_KEYS))
		__dmsg(ds, ", keys-built");
	if (page->modify != NULL) {
		if (F_ISSET(page->modify, WT_PM_REC_EMPTY))
			__dmsg(ds, ", empty");
		if (F_ISSET(page->modify, WT_PM_REC_REPLACE))
			__dmsg(ds, ", replaced");
		if (F_ISSET(page->modify, WT_PM_REC_SPLIT))
			__dmsg(ds, ", split");
		if (F_ISSET(page->modify, WT_PM_REC_SPLIT_MERGE))
			__dmsg(ds, ", split-merge");
	}
	__dmsg(ds, ")\n");

	if (WT_PAGE_IS_ROOT(page))
		__dmsg(ds, "\troot");
	else
		__dmsg(ds, "\tparent %p", page->parent);
	__dmsg(ds,
	    ", disk %p, entries %" PRIu32 "\n", page->dsk, page->entries);

	return (0);
}

/*
 * __debug_page_modify --
 *	Dump an in-memory page's modification structure.
 */
static int
__debug_page_modify(WT_DBG *ds, WT_PAGE *page)
{
	WT_PAGE_MODIFY *mod;
	WT_PAGE_TRACK *track;
	WT_SESSION_IMPL *session;
	uint32_t i;
	char buf[64];

	session = ds->session;

	if ((mod = page->modify) == NULL)
		return (0);

	__dmsg(ds,
	    "\t" "write/disk generations: %" PRIu32 "/%" PRIu32 "\n",
	    mod->write_gen, mod->disk_gen);

	switch (page->modify == NULL ?
	    0 : F_ISSET(page->modify, WT_PM_REC_MASK)) {
	case 0:
		break;
	case WT_PM_REC_EMPTY:
		__dmsg(ds, "\t" "empty page\n");
		break;
	case WT_PM_REC_REPLACE:
		__dmsg(ds, "\t" "replacement %s\n",
		    __wt_addr_string(session, ds->tmp,
		    mod->u.replace.addr, mod->u.replace.size));
		break;
	case WT_PM_REC_SPLIT:
		__dmsg(ds, "\t" "split page %p\n", mod->u.split);
		break;
	case WT_PM_REC_SPLIT_MERGE:
		__dmsg(ds, "\t" "split-merge page %p\n", mod->u.split);
		break;
	WT_ILLEGAL_VALUE(session);
	}

	if (mod->track_entries != 0)
		__dmsg(ds, "\t" "tracking list:\n");
	for (track = mod->track, i = 0; i < mod->track_entries; ++track, ++i)
		if (F_ISSET(track, WT_TRK_OBJECT)) {
			__dmsg(ds, "\t\t%s %s\n",
			    __wt_track_string(track, buf, sizeof(buf)),
			    __wt_addr_string(session,
			    ds->tmp, track->addr.addr, track->addr.size));
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
	WT_PAGE_HEADER *dsk;
	WT_SESSION_IMPL *session;
	uint64_t recno;
	uint32_t i;
	uint8_t v;

	session = ds->session;
	btree = S2BT(session);
	dsk = page->dsk;
	recno = page->u.col_fix.recno;

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
		__dmsg(ds, "%s", sep);
		__debug_col_skip(ds, WT_COL_UPDATE_SINGLE(page), "update", 1);
	}
	if (WT_COL_APPEND(page) != NULL) {
		__dmsg(ds, "%s", sep);
		__debug_col_skip(ds, WT_COL_APPEND(page), "append", 1);
	}
}

/*
 * __debug_page_col_int --
 *	Dump an in-memory WT_PAGE_COL_INT page.
 */
static int
__debug_page_col_int(WT_DBG *ds, WT_PAGE *page, uint32_t flags)
{
	WT_REF *ref;
	uint32_t i;

	WT_REF_FOREACH(page, ref, i) {
		__dmsg(ds, "\trecno %" PRIu64 "\n", ref->key.recno);
		WT_RET(__debug_ref(ds, ref, page));
	}

	if (LF_ISSET(WT_DEBUG_TREE_WALK))
		WT_REF_FOREACH(page, ref, i)
			if (ref->state == WT_REF_MEM) {
				__dmsg(ds, "\n");
				WT_RET(__debug_page(ds, ref->page, flags));
			}

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
	WT_INSERT_HEAD *update;
	uint64_t recno, rle;
	uint32_t i;
	char tag[64];

	unpack = &_unpack;
	recno = page->u.col_var.recno;

	WT_COL_FOREACH(page, cip, i) {
		if ((cell = WT_COL_PTR(page, cip)) == NULL) {
			unpack = NULL;
			rle = 1;
		} else {
			__wt_cell_unpack(cell, WT_PAGE_COL_VAR, unpack);
			rle = __wt_cell_rle(unpack);
		}
		snprintf(tag, sizeof(tag), "%" PRIu64 " %" PRIu64, recno, rle);
		WT_RET(__debug_cell_data(ds, tag, unpack));

		if ((update = WT_COL_UPDATE(page, cip)) != NULL)
			__debug_col_skip(ds, update, "update", 0);
		recno += rle;
	}

	if (WT_COL_APPEND(page) != NULL) {
		__dmsg(ds, "%s", sep);
		__debug_col_skip(ds, WT_COL_APPEND(page), "append", 0);
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
	WT_REF *ref;
	uint8_t *p;
	uint32_t i, len;

	WT_REF_FOREACH(page, ref, i) {
		__wt_ref_key(page, ref, &p, &len);
		__debug_item(ds, "K", p, len);
		WT_RET(__debug_ref(ds, ref, page));
	}

	if (LF_ISSET(WT_DEBUG_TREE_WALK))
		WT_REF_FOREACH(page, ref, i)
			if (ref->state == WT_REF_MEM) {
				__dmsg(ds, "\n");
				WT_RET(__debug_page(ds, ref->page, flags));
			}
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
	WT_INSERT_HEAD *insert;
	WT_ROW *rip;
	WT_UPDATE *upd;
	uint32_t i;
	void *ripkey;

	unpack = &_unpack;

	/*
	 * Dump any K/V pairs inserted into the page before the first from-disk
	 * key on the page.
	 */
	if ((insert = WT_ROW_INSERT_SMALLEST(page)) != NULL)
		__debug_row_skip(ds, insert);

	/* Dump the page's K/V pairs. */
	WT_ROW_FOREACH(page, rip, i) {
		ripkey = WT_ROW_KEY_COPY(rip);
		if (__wt_off_page(page, ripkey))
			__debug_ikey(ds, ripkey);
		else {
			__wt_cell_unpack(ripkey, WT_PAGE_ROW_LEAF, unpack);
			WT_RET(__debug_cell_data(ds, "K", unpack));
		}

		if ((cell = __wt_row_value(page, rip)) == NULL)
			__dmsg(ds, "\tV {}\n");
		else {
			__wt_cell_unpack(cell, WT_PAGE_ROW_LEAF, unpack);
			WT_RET(__debug_cell_data(ds, "V", unpack));
		}

		if ((upd = WT_ROW_UPDATE(page, rip)) != NULL)
			__debug_update(ds, upd, 0);

		if ((insert = WT_ROW_INSERT(page, rip)) != NULL)
			__debug_row_skip(ds, insert);
	}

	return (0);
}

/*
 * __debug_col_skip --
 *	Dump a column-store skiplist.
 */
static void
__debug_col_skip(WT_DBG *ds, WT_INSERT_HEAD *head, const char *tag, int hexbyte)
{
	WT_INSERT *ins;

	WT_SKIP_FOREACH(ins, head) {
		__dmsg(ds,
		    "\t%s %" PRIu64 "\n", tag, WT_INSERT_RECNO(ins));
		__debug_update(ds, ins->upd, hexbyte);
	}
}

/*
 * __debug_row_skip --
 *	Dump an insert list.
 */
static void
__debug_row_skip(WT_DBG *ds, WT_INSERT_HEAD *head)
{
	WT_INSERT *ins;

	WT_SKIP_FOREACH(ins, head) {
		__debug_item(ds,
		    "insert", WT_INSERT_KEY(ins), WT_INSERT_KEY_SIZE(ins));
		__debug_update(ds, ins->upd, 0);
	}
}

/*
 * __debug_update --
 *	Dump an update list.
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
 * __debug_ref --
 *	Dump a WT_REF structure.
 */
static int
__debug_ref(WT_DBG *ds, WT_REF *ref, WT_PAGE *page)
{
	WT_SESSION_IMPL *session;
	uint32_t size;
	const uint8_t *addr;

	session = ds->session;

	__dmsg(ds, "\t");
	switch (ref->state) {
	case WT_REF_DISK:
		__dmsg(ds, "disk");
		break;
	case WT_REF_DELETED:
		__dmsg(ds, "deleted");
		break;
	case WT_REF_EVICT_WALK:
		__dmsg(ds, "evict-walk %p", ref->page);
		break;
	case WT_REF_LOCKED:
		__dmsg(ds, "locked %p", ref->page);
		break;
	case WT_REF_MEM:
		__dmsg(ds, "memory %p", ref->page);
		break;
	case WT_REF_READING:
		__dmsg(ds, "reading");
		break;
	WT_ILLEGAL_VALUE(session);
	}

	if (ref->addr == NULL)
		__dmsg(ds, " %s\n", "[NoAddr]");
	else {
		__wt_get_addr(page, ref, &addr, &size);
		__dmsg(ds,
		    " %s\n", __wt_addr_string(session, ds->tmp, addr, size));
	}
	return (0);
}

/*
 * __debug_cell --
 *	Dump a single unpacked WT_CELL.
 */
static int
__debug_cell(WT_DBG *ds, WT_PAGE_HEADER *dsk, WT_CELL_UNPACK *unpack)
{
	WT_DECL_ITEM(buf);
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	const char *type;

	session = ds->session;

	__dmsg(ds, "\t%s: len %" PRIu32,
	    __wt_cell_type_string(unpack->raw), unpack->size);

	/* Dump cell's per-disk page type information. */
	switch (dsk->type) {
	case WT_PAGE_COL_INT:
		switch (unpack->type) {
		case WT_CELL_VALUE:
			__dmsg(ds, ", recno: %" PRIu64, unpack->v);
			break;
		}
		break;
	case WT_PAGE_COL_VAR:
		switch (unpack->type) {
		case WT_CELL_DEL:
		case WT_CELL_VALUE:
		case WT_CELL_VALUE_OVFL:
		case WT_CELL_VALUE_OVFL_RM:
			__dmsg(ds, ", rle: %" PRIu64, __wt_cell_rle(unpack));
			break;
		}
		break;
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		switch (unpack->type) {
		case WT_CELL_KEY:
			__dmsg(ds, ", pfx: %" PRIu8, unpack->prefix);
			break;
		}
		break;
	}

	/* Dump addresses. */
	switch (unpack->raw) {
	case WT_CELL_ADDR:
		type = "addr";
		goto addr;
	case WT_CELL_ADDR_DEL:
		type = "addr/del";
		goto addr;
	case WT_CELL_ADDR_LNO:
		type = "addr/lno";
		goto addr;
	case WT_CELL_KEY_OVFL:
	case WT_CELL_VALUE_OVFL:
	case WT_CELL_VALUE_OVFL_RM:
		type = "ovfl";
addr:		WT_RET(__wt_scr_alloc(session, 128, &buf));
		__dmsg(ds, ", %s %s", type,
		    __wt_addr_string(session, buf, unpack->data, unpack->size));
		__wt_scr_free(&buf);
		WT_RET(ret);
		break;
	}
	__dmsg(ds, "\n");

	return (__debug_cell_data(ds, NULL, unpack));
}

/*
 * __debug_cell_data --
 *	Dump a single cell's data in debugging mode.
 */
static int
__debug_cell_data(WT_DBG *ds, const char *tag, WT_CELL_UNPACK *unpack)
{
	WT_DECL_ITEM(buf);
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	session = ds->session;

	/*
	 * Column-store references to deleted cells return a NULL cell
	 * reference.
	 */
	if (unpack == NULL)
		goto deleted;

	switch (unpack->raw) {
	case WT_CELL_ADDR:
		__debug_item(ds, tag, "addr", strlen("addr"));
		break;
	case WT_CELL_ADDR_DEL:
		__debug_item(ds, tag, "addr/del", strlen("addr/del"));
		break;
	case WT_CELL_ADDR_LNO:
		__debug_item(ds, tag, "addr/lno", strlen("addr/lno"));
		break;
	case WT_CELL_DEL:
deleted:	__debug_item(ds, tag, "deleted", strlen("deleted"));
		break;
	case WT_CELL_KEY:
	case WT_CELL_KEY_OVFL:
	case WT_CELL_KEY_SHORT:
	case WT_CELL_VALUE:
	case WT_CELL_VALUE_COPY:
	case WT_CELL_VALUE_OVFL:
	case WT_CELL_VALUE_OVFL_RM:
	case WT_CELL_VALUE_SHORT:
		WT_RET(__wt_scr_alloc(session, 256, &buf));
		if ((ret = __wt_cell_unpack_ref(session, unpack, buf)) == 0)
			__debug_item(ds, tag, buf->data, buf->size);
		__wt_scr_free(&buf);
		break;
	WT_ILLEGAL_VALUE(session);
	}

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
__debug_item(WT_DBG *ds, const char *tag, const void *data_arg, size_t size)
{
	const uint8_t *data;
	int ch;

	__dmsg(ds, "\t%s%s{", tag == NULL ? "" : tag, tag == NULL ? "" : " ");
	for (data = data_arg; size > 0; --size, ++data) {
		ch = data[0];
		if (isprint(ch))
			__dmsg(ds, "%c", ch);
		else
			__debug_hex_byte(ds, data[0]);
	}
	__dmsg(ds, "}\n");
}
#endif
