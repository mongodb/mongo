/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
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
typedef struct {
	WT_SESSION_IMPL *session;		/* Enclosing session */

	/*
	 * When using the standard event handlers, the debugging output has to
	 * do its own message handling because its output isn't line-oriented.
	 */
	FILE		*fp;
	WT_ITEM		*msg;			/* Buffered message */

	WT_ITEM		*tmp;			/* Temporary space */
} WT_DBG;

static const					/* Output separator */
    char * const sep = "=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=\n";

static int  __debug_cell(WT_DBG *, const WT_PAGE_HEADER *, WT_CELL_UNPACK *);
static int  __debug_cell_data(
	WT_DBG *, WT_PAGE *, int type, const char *, WT_CELL_UNPACK *);
static void __debug_col_skip(WT_DBG *, WT_INSERT_HEAD *, const char *, bool);
static int  __debug_config(WT_SESSION_IMPL *, WT_DBG *, const char *);
static int  __debug_dsk_cell(WT_DBG *, const WT_PAGE_HEADER *);
static void __debug_dsk_col_fix(WT_DBG *, const WT_PAGE_HEADER *);
static void __debug_item(WT_DBG *, const char *, const void *, size_t);
static int  __debug_page(WT_DBG *, WT_REF *, uint32_t);
static void __debug_page_col_fix(WT_DBG *, WT_REF *);
static int  __debug_page_col_int(WT_DBG *, WT_PAGE *, uint32_t);
static int  __debug_page_col_var(WT_DBG *, WT_REF *);
static int  __debug_page_metadata(WT_DBG *, WT_REF *);
static int  __debug_page_row_int(WT_DBG *, WT_PAGE *, uint32_t);
static int  __debug_page_row_leaf(WT_DBG *, WT_PAGE *);
static void __debug_ref(WT_DBG *, WT_REF *);
static void __debug_row_skip(WT_DBG *, WT_INSERT_HEAD *);
static int  __debug_tree(
	WT_SESSION_IMPL *, WT_BTREE *, WT_REF *, const char *, uint32_t);
static void __debug_update(WT_DBG *, WT_UPDATE *, bool);
static void __dmsg(WT_DBG *, const char *, ...)
	WT_GCC_FUNC_DECL_ATTRIBUTE((format (printf, 2, 3)));
static void __dmsg_wrapup(WT_DBG *);

/*
 * __wt_debug_set_verbose --
 *	Set verbose flags from the debugger.
 */
int
__wt_debug_set_verbose(WT_SESSION_IMPL *session, const char *v)
{
	const char *cfg[2] = { NULL, NULL };
	char buf[256];

	snprintf(buf, sizeof(buf), "verbose=[%s]", v);
	cfg[0] = buf;
	return (__wt_verbose_config(session, cfg));
}

/*
 * __debug_hex_byte --
 *	Output a single byte in hex.
 */
static inline void
__debug_hex_byte(WT_DBG *ds, uint8_t v)
{
	__dmsg(ds, "#%c%c", __wt_hex[(v & 0xf0) >> 4], __wt_hex[v & 0x0f]);
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

	if ((ds->fp = fopen(ofile, "w")) == NULL)
		return (EIO);
	__wt_stream_set_line_buffer(ds->fp);

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

	__wt_scr_free(session, &ds->tmp);

	/*
	 * Discard the buffer -- it shouldn't have anything in it, but might
	 * as well be cautious.
	 */
	if (msg != NULL) {
		if (msg->size != 0)
			(void)__wt_msg(session, "%s", (char *)msg->mem);
		__wt_scr_free(session, &ds->msg);
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
 * __wt_debug_addr_print --
 *	Print out an address.
 */
int
__wt_debug_addr_print(
    WT_SESSION_IMPL *session, const uint8_t *addr, size_t addr_size)
{
	WT_DECL_ITEM(buf);
	WT_DECL_RET;

	WT_RET(__wt_scr_alloc(session, 128, &buf));
	ret = __wt_fprintf(session, WT_STDERR(session),
	    "%s\n", __wt_addr_string(session, addr, addr_size, buf));
	__wt_scr_free(session, &buf);

	return (ret);
}

/*
 * __wt_debug_addr --
 *	Read and dump a disk page in debugging mode, using an addr/size pair.
 */
int
__wt_debug_addr(WT_SESSION_IMPL *session,
    const uint8_t *addr, size_t addr_size, const char *ofile)
{
	WT_BM *bm;
	WT_DECL_ITEM(buf);
	WT_DECL_RET;

	WT_ASSERT(session, S2BT_SAFE(session) != NULL);

	bm = S2BT(session)->bm;

	WT_RET(__wt_scr_alloc(session, 1024, &buf));
	WT_ERR(bm->read(bm, session, buf, addr, addr_size));
	ret = __wt_debug_disk(session, buf->mem, ofile);

err:	__wt_scr_free(session, &buf);
	return (ret);
}

/*
 * __wt_debug_offset_blind --
 *	Read and dump a disk page in debugging mode, using a file offset.
 */
int
__wt_debug_offset_blind(
    WT_SESSION_IMPL *session, wt_off_t offset, const char *ofile)
{
	WT_DECL_ITEM(buf);
	WT_DECL_RET;

	WT_ASSERT(session, S2BT_SAFE(session) != NULL);

	/*
	 * This routine depends on the default block manager's view of files,
	 * where an address consists of a file offset, length, and checksum.
	 * This is for debugging only.  Other block managers might not see a
	 * file or address the same way, that's why there's no block manager
	 * method.
	 */
	WT_RET(__wt_scr_alloc(session, 1024, &buf));
	WT_ERR(__wt_block_read_off_blind(
	    session, S2BT(session)->bm->block, buf, offset));
	ret = __wt_debug_disk(session, buf->mem, ofile);

err:	__wt_scr_free(session, &buf);
	return (ret);
}

/*
 * __wt_debug_offset --
 *	Read and dump a disk page in debugging mode, using a file
 * offset/size/checksum triplet.
 */
int
__wt_debug_offset(WT_SESSION_IMPL *session,
     wt_off_t offset, uint32_t size, uint32_t cksum, const char *ofile)
{
	WT_DECL_ITEM(buf);
	WT_DECL_RET;
	uint8_t addr[WT_BTREE_MAX_ADDR_COOKIE], *endp;

	WT_ASSERT(session, S2BT_SAFE(session) != NULL);

	/*
	 * This routine depends on the default block manager's view of files,
	 * where an address consists of a file offset, length, and checksum.
	 * This is for debugging only: other block managers might not see a
	 * file or address the same way, that's why there's no block manager
	 * method.
	 *
	 * Convert the triplet into an address structure.
	 */
	endp = addr;
	WT_RET(__wt_block_addr_to_buffer(
	    S2BT(session)->bm->block, &endp, offset, size, cksum));

	/*
	 * Read the address through the btree I/O functions (so the block is
	 * decompressed as necessary).
	 */
	WT_RET(__wt_scr_alloc(session, 0, &buf));
	WT_ERR(__wt_bt_read(session, buf, addr, WT_PTRDIFF(endp, addr)));
	ret = __wt_debug_disk(session, buf->mem, ofile);

err:	__wt_scr_free(session, &buf);
	return (ret);
}

/*
 * __wt_debug_disk --
 *	Dump a disk page in debugging mode.
 */
int
__wt_debug_disk(
    WT_SESSION_IMPL *session, const WT_PAGE_HEADER *dsk, const char *ofile)
{
	WT_DBG *ds, _ds;
	WT_DECL_RET;

	ds = &_ds;
	WT_RET(__debug_config(session, ds, ofile));

	__dmsg(ds, "%s page", __wt_page_type_string(dsk->type));
	switch (dsk->type) {
	case WT_PAGE_BLOCK_MANAGER:
		break;
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_INT:
	case WT_PAGE_COL_VAR:
		__dmsg(ds, ", recno %" PRIu64, dsk->recno);
		/* FALLTHROUGH */
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		__dmsg(ds, ", entries %" PRIu32, dsk->u.entries);
		break;
	case WT_PAGE_OVFL:
		__dmsg(ds, ", datalen %" PRIu32, dsk->u.datalen);
		break;
	WT_ILLEGAL_VALUE(session);
	}

	if (F_ISSET(dsk, WT_PAGE_COMPRESSED))
		__dmsg(ds, ", compressed");
	if (F_ISSET(dsk, WT_PAGE_ENCRYPTED))
		__dmsg(ds, ", encrypted");
	if (F_ISSET(dsk, WT_PAGE_EMPTY_V_ALL))
		__dmsg(ds, ", empty-all");
	if (F_ISSET(dsk, WT_PAGE_EMPTY_V_NONE))
		__dmsg(ds, ", empty-none");
	if (F_ISSET(dsk, WT_PAGE_LAS_UPDATE))
		__dmsg(ds, ", LAS-update");

	__dmsg(ds, ", generation %" PRIu64 "\n", dsk->write_gen);

	switch (dsk->type) {
	case WT_PAGE_BLOCK_MANAGER:
		break;
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
__debug_dsk_col_fix(WT_DBG *ds, const WT_PAGE_HEADER *dsk)
{
	WT_BTREE *btree;
	uint32_t i;
	uint8_t v;

	WT_ASSERT(ds->session, S2BT_SAFE(ds->session) != NULL);

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
__debug_dsk_cell(WT_DBG *ds, const WT_PAGE_HEADER *dsk)
{
	WT_BTREE *btree;
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	uint32_t i;

	WT_ASSERT(ds->session, S2BT_SAFE(ds->session) != NULL);

	btree = S2BT(ds->session);
	unpack = &_unpack;

	WT_CELL_FOREACH(btree, dsk, cell, unpack, i) {
		__wt_cell_unpack(cell, unpack);
		WT_RET(__debug_cell(ds, dsk, unpack));
	}
	return (0);
}

/*
 * __debug_tree_shape_info --
 *	Pretty-print information about a page.
 */
static char *
__debug_tree_shape_info(WT_PAGE *page)
{
	uint64_t v;
	static char buf[32];

	v = page->memory_footprint;
	if (v >= WT_GIGABYTE)
		snprintf(buf, sizeof(buf),
		    "(%p %" PRIu64 "G)", (void *)page, v / WT_GIGABYTE);
	else if (v >= WT_MEGABYTE)
		snprintf(buf, sizeof(buf),
		    "(%p %" PRIu64 "M)", (void *)page, v / WT_MEGABYTE);
	else
		snprintf(buf, sizeof(buf), "(%p %" PRIu64 ")", (void *)page, v);
	return (buf);
}

/*
 * __debug_tree_shape_worker --
 *	Dump information about the current page and descend.
 */
static void
__debug_tree_shape_worker(WT_DBG *ds, WT_PAGE *page, int level)
{
	WT_REF *ref;
	WT_SESSION_IMPL *session;

	session = ds->session;

	if (WT_PAGE_IS_INTERNAL(page)) {
		__dmsg(ds, "%*s" "I" "%d %s\n",
		    level * 3, " ", level, __debug_tree_shape_info(page));
		WT_INTL_FOREACH_BEGIN(session, page, ref) {
			if (ref->state == WT_REF_MEM)
				__debug_tree_shape_worker(
				    ds, ref->page, level + 1);
		} WT_INTL_FOREACH_END;
	} else
		__dmsg(ds, "%*s" "L" " %s\n",
		    level * 3, " ", __debug_tree_shape_info(page));
}

/*
 * __wt_debug_tree_shape --
 *	Dump the shape of the in-memory tree.
 */
int
__wt_debug_tree_shape(
    WT_SESSION_IMPL *session, WT_PAGE *page, const char *ofile)
{
	WT_DBG *ds, _ds;

	WT_ASSERT(session, S2BT_SAFE(session) != NULL);

	ds = &_ds;
	WT_RET(__debug_config(session, ds, ofile));

	/* A NULL page starts at the top of the tree -- it's a convenience. */
	if (page == NULL)
		page = S2BT(session)->root.page;

	WT_WITH_PAGE_INDEX(session, __debug_tree_shape_worker(ds, page, 1));

	__dmsg_wrapup(ds);
	return (0);
}

#define	WT_DEBUG_TREE_LEAF	0x01			/* Debug leaf pages */
#define	WT_DEBUG_TREE_WALK	0x02			/* Descend the tree */

/*
 * __wt_debug_tree_all --
 *	Dump the in-memory information for a tree, including leaf pages.
 *	Takes an explicit btree as an argument, as one may not yet be set on
 *	the session. This is often the case as this function will be called
 *	from within a debugger, which makes setting a btree complicated.
 */
int
__wt_debug_tree_all(
    WT_SESSION_IMPL *session, WT_BTREE *btree, WT_REF *ref, const char *ofile)
{
	return (__debug_tree(session,
	    btree, ref, ofile, WT_DEBUG_TREE_LEAF | WT_DEBUG_TREE_WALK));
}

/*
 * __wt_debug_tree --
 *	Dump the in-memory information for a tree, not including leaf pages.
 *	Takes an explicit btree as an argument, as one may not yet be set on
 *	the session. This is often the case as this function will be called
 *	from within a debugger, which makes setting a btree complicated.
 */
int
__wt_debug_tree(
    WT_SESSION_IMPL *session, WT_BTREE *btree, WT_REF *ref, const char *ofile)
{
	return (__debug_tree(session, btree, ref, ofile, WT_DEBUG_TREE_WALK));
}

/*
 * __wt_debug_page --
 *	Dump the in-memory information for a page.
 */
int
__wt_debug_page(WT_SESSION_IMPL *session, WT_REF *ref, const char *ofile)
{
	WT_DBG *ds, _ds;
	WT_DECL_RET;

	WT_ASSERT(session, S2BT_SAFE(session) != NULL);

	ds = &_ds;
	WT_RET(__debug_config(session, ds, ofile));

	ret = __debug_page(ds, ref, WT_DEBUG_TREE_LEAF);

	__dmsg_wrapup(ds);

	return (ret);
}

/*
 * __debug_tree --
 *	Dump the in-memory information for a tree. Takes an explicit btree
 *	as an argument, as one may not be set on the session. This is often
 *	the case as this function will be called from within a debugger, which
 *	makes setting a btree complicated. We mark the session to the btree
 *	in this function
 */
static int
__debug_tree(WT_SESSION_IMPL *session,
    WT_BTREE *btree, WT_REF *ref, const char *ofile, uint32_t flags)
{
	WT_DBG *ds, _ds;
	WT_DECL_RET;

	ds = &_ds;
	WT_RET(__debug_config(session, ds, ofile));

	/* A NULL page starts at the top of the tree -- it's a convenience. */
	if (ref == NULL)
		ref = &btree->root;

	WT_WITH_BTREE(session, btree, ret = __debug_page(ds, ref, flags));

	__dmsg_wrapup(ds);

	return (ret);
}

/*
 * __debug_page --
 *	Dump the in-memory information for an in-memory page.
 */
static int
__debug_page(WT_DBG *ds, WT_REF *ref, uint32_t flags)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	session = ds->session;

	/* Dump the page metadata. */
	WT_WITH_PAGE_INDEX(session, ret = __debug_page_metadata(ds, ref));
	WT_RET(ret);

	/* Dump the page. */
	switch (ref->page->type) {
	case WT_PAGE_COL_FIX:
		if (LF_ISSET(WT_DEBUG_TREE_LEAF))
			__debug_page_col_fix(ds, ref);
		break;
	case WT_PAGE_COL_INT:
		WT_WITH_PAGE_INDEX(session,
		    ret = __debug_page_col_int(ds, ref->page, flags));
		WT_RET(ret);
		break;
	case WT_PAGE_COL_VAR:
		if (LF_ISSET(WT_DEBUG_TREE_LEAF))
			WT_RET(__debug_page_col_var(ds, ref));
		break;
	case WT_PAGE_ROW_INT:
		WT_WITH_PAGE_INDEX(session,
		    ret = __debug_page_row_int(ds, ref->page, flags));
		WT_RET(ret);
		break;
	case WT_PAGE_ROW_LEAF:
		if (LF_ISSET(WT_DEBUG_TREE_LEAF))
			WT_RET(__debug_page_row_leaf(ds, ref->page));
		break;
	WT_ILLEGAL_VALUE(session);
	}

	return (0);
}

/*
 * __debug_page_metadata --
 *	Dump an in-memory page's metadata.
 */
static int
__debug_page_metadata(WT_DBG *ds, WT_REF *ref)
{
	WT_PAGE *page;
	WT_PAGE_INDEX *pindex;
	WT_PAGE_MODIFY *mod;
	WT_SESSION_IMPL *session;
	uint32_t entries;

	session = ds->session;
	page = ref->page;
	mod = page->modify;

	__dmsg(ds, "%p", (void *)page);

	switch (page->type) {
	case WT_PAGE_COL_INT:
		__dmsg(ds, " recno %" PRIu64, ref->ref_recno);
		WT_INTL_INDEX_GET(session, page, pindex);
		entries = pindex->entries;
		break;
	case WT_PAGE_COL_FIX:
		__dmsg(ds, " recno %" PRIu64, ref->ref_recno);
		entries = page->pg_fix_entries;
		break;
	case WT_PAGE_COL_VAR:
		__dmsg(ds, " recno %" PRIu64, ref->ref_recno);
		entries = page->pg_var_entries;
		break;
	case WT_PAGE_ROW_INT:
		WT_INTL_INDEX_GET(session, page, pindex);
		entries = pindex->entries;
		break;
	case WT_PAGE_ROW_LEAF:
		entries = page->pg_row_entries;
		break;
	WT_ILLEGAL_VALUE(session);
	}

	__dmsg(ds, ": %s\n", __wt_page_type_string(page->type));
	__dmsg(ds,
	    "\t" "disk %p, entries %" PRIu32, (void *)page->dsk, entries);
	__dmsg(ds, ", %s", __wt_page_is_modified(page) ? "dirty" : "clean");
	__dmsg(ds, ", %s", __wt_fair_islocked(
	    session, &page->page_lock) ? "locked" : "unlocked");

	if (F_ISSET_ATOMIC(page, WT_PAGE_BUILD_KEYS))
		__dmsg(ds, ", keys-built");
	if (F_ISSET_ATOMIC(page, WT_PAGE_DISK_ALLOC))
		__dmsg(ds, ", disk-alloc");
	if (F_ISSET_ATOMIC(page, WT_PAGE_DISK_MAPPED))
		__dmsg(ds, ", disk-mapped");
	if (F_ISSET_ATOMIC(page, WT_PAGE_EVICT_LRU))
		__dmsg(ds, ", evict-lru");
	if (F_ISSET_ATOMIC(page, WT_PAGE_OVERFLOW_KEYS))
		__dmsg(ds, ", overflow-keys");
	if (F_ISSET_ATOMIC(page, WT_PAGE_SPLIT_BLOCK))
		__dmsg(ds, ", split-block");
	if (F_ISSET_ATOMIC(page, WT_PAGE_SPLIT_INSERT))
		__dmsg(ds, ", split-insert");
	if (F_ISSET_ATOMIC(page, WT_PAGE_UPDATE_IGNORE))
		__dmsg(ds, ", update-ignore");

	if (mod != NULL)
		switch (mod->rec_result) {
		case WT_PM_REC_EMPTY:
			__dmsg(ds, ", empty");
			break;
		case WT_PM_REC_MULTIBLOCK:
			__dmsg(ds, ", multiblock");
			break;
		case WT_PM_REC_REPLACE:
			__dmsg(ds, ", replaced");
			break;
		case 0:
			break;
		WT_ILLEGAL_VALUE(session);
		}
	if (mod != NULL)
		__dmsg(ds, ", write generation=%" PRIu32, mod->write_gen);
	__dmsg(ds, "\n");

	return (0);
}

/*
 * __debug_page_col_fix --
 *	Dump an in-memory WT_PAGE_COL_FIX page.
 */
static void
__debug_page_col_fix(WT_DBG *ds, WT_REF *ref)
{
	WT_BTREE *btree;
	WT_INSERT *ins;
	WT_PAGE *page;
	const WT_PAGE_HEADER *dsk;
	WT_SESSION_IMPL *session;
	uint64_t recno;
	uint32_t i;
	uint8_t v;

	WT_ASSERT(ds->session, S2BT_SAFE(ds->session) != NULL);

	session = ds->session;
	btree = S2BT(session);
	page = ref->page;
	dsk = page->dsk;
	recno = ref->ref_recno;

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
				__debug_update(ds, ins->upd, true);
				ins = WT_SKIP_NEXT(ins);
			}
			++recno;
		}
	}

	if (WT_COL_UPDATE_SINGLE(page) != NULL) {
		__dmsg(ds, "%s", sep);
		__debug_col_skip(
		    ds, WT_COL_UPDATE_SINGLE(page), "update", true);
	}
	if (WT_COL_APPEND(page) != NULL) {
		__dmsg(ds, "%s", sep);
		__debug_col_skip(ds, WT_COL_APPEND(page), "append", true);
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
	WT_SESSION_IMPL *session;

	session = ds->session;

	WT_INTL_FOREACH_BEGIN(session, page, ref) {
		__dmsg(ds, "\trecno %" PRIu64 "\n", ref->ref_recno);
		__debug_ref(ds, ref);
	} WT_INTL_FOREACH_END;

	if (LF_ISSET(WT_DEBUG_TREE_WALK))
		WT_INTL_FOREACH_BEGIN(session, page, ref) {
			if (ref->state == WT_REF_MEM) {
				__dmsg(ds, "\n");
				WT_RET(__debug_page(ds, ref, flags));
			}
		} WT_INTL_FOREACH_END;

	return (0);
}

/*
 * __debug_page_col_var --
 *	Dump an in-memory WT_PAGE_COL_VAR page.
 */
static int
__debug_page_col_var(WT_DBG *ds, WT_REF *ref)
{
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	WT_COL *cip;
	WT_INSERT_HEAD *update;
	WT_PAGE *page;
	uint64_t recno, rle;
	uint32_t i;
	char tag[64];

	unpack = &_unpack;
	page = ref->page;
	recno = ref->ref_recno;

	WT_COL_FOREACH(page, cip, i) {
		if ((cell = WT_COL_PTR(page, cip)) == NULL) {
			unpack = NULL;
			rle = 1;
		} else {
			__wt_cell_unpack(cell, unpack);
			rle = __wt_cell_rle(unpack);
		}
		snprintf(tag, sizeof(tag), "%" PRIu64 " %" PRIu64, recno, rle);
		WT_RET(
		    __debug_cell_data(ds, page, WT_PAGE_COL_VAR, tag, unpack));

		if ((update = WT_COL_UPDATE(page, cip)) != NULL)
			__debug_col_skip(ds, update, "update", false);
		recno += rle;
	}

	if (WT_COL_APPEND(page) != NULL) {
		__dmsg(ds, "%s", sep);
		__debug_col_skip(ds, WT_COL_APPEND(page), "append", false);
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
	WT_SESSION_IMPL *session;
	size_t len;
	void *p;

	session = ds->session;

	WT_INTL_FOREACH_BEGIN(session, page, ref) {
		__wt_ref_key(page, ref, &p, &len);
		__debug_item(ds, "K", p, len);
		__debug_ref(ds, ref);
	} WT_INTL_FOREACH_END;

	if (LF_ISSET(WT_DEBUG_TREE_WALK))
		WT_INTL_FOREACH_BEGIN(session, page, ref) {
			if (ref->state == WT_REF_MEM) {
				__dmsg(ds, "\n");
				WT_RET(__debug_page(ds, ref, flags));
			}
		} WT_INTL_FOREACH_END;
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
	WT_DECL_ITEM(key);
	WT_DECL_RET;
	WT_INSERT_HEAD *insert;
	WT_ROW *rip;
	WT_SESSION_IMPL *session;
	WT_UPDATE *upd;
	uint32_t i;

	session = ds->session;
	unpack = &_unpack;
	WT_RET(__wt_scr_alloc(session, 256, &key));

	/*
	 * Dump any K/V pairs inserted into the page before the first from-disk
	 * key on the page.
	 */
	if ((insert = WT_ROW_INSERT_SMALLEST(page)) != NULL)
		__debug_row_skip(ds, insert);

	/* Dump the page's K/V pairs. */
	WT_ROW_FOREACH(page, rip, i) {
		WT_ERR(__wt_row_leaf_key(session, page, rip, key, false));
		__debug_item(ds, "K", key->data, key->size);

		if ((cell = __wt_row_leaf_value_cell(page, rip, NULL)) == NULL)
			__dmsg(ds, "\tV {}\n");
		else {
			__wt_cell_unpack(cell, unpack);
			WT_ERR(__debug_cell_data(
			    ds, page, WT_PAGE_ROW_LEAF, "V", unpack));
		}

		if ((upd = WT_ROW_UPDATE(page, rip)) != NULL)
			__debug_update(ds, upd, false);

		if ((insert = WT_ROW_INSERT(page, rip)) != NULL)
			__debug_row_skip(ds, insert);
	}

err:	__wt_scr_free(session, &key);
	return (ret);
}

/*
 * __debug_col_skip --
 *	Dump a column-store skiplist.
 */
static void
__debug_col_skip(
    WT_DBG *ds, WT_INSERT_HEAD *head, const char *tag, bool hexbyte)
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
		__debug_update(ds, ins->upd, false);
	}
}

/*
 * __debug_update --
 *	Dump an update list.
 */
static void
__debug_update(WT_DBG *ds, WT_UPDATE *upd, bool hexbyte)
{
	for (; upd != NULL; upd = upd->next)
		if (WT_UPDATE_DELETED_ISSET(upd))
			__dmsg(ds, "\tvalue {deleted}\n");
		else if (hexbyte) {
			__dmsg(ds, "\t{");
			__debug_hex_byte(ds, *(uint8_t *)WT_UPDATE_DATA(upd));
			__dmsg(ds, "}\n");
		} else
			__debug_item(ds,
			    "value", WT_UPDATE_DATA(upd), upd->size);
}

/*
 * __debug_ref --
 *	Dump a WT_REF structure.
 */
static void
__debug_ref(WT_DBG *ds, WT_REF *ref)
{
	WT_SESSION_IMPL *session;
	size_t addr_size;
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
	case WT_REF_LOCKED:
		__dmsg(ds, "locked %p", (void *)ref->page);
		break;
	case WT_REF_MEM:
		__dmsg(ds, "memory %p", (void *)ref->page);
		break;
	case WT_REF_READING:
		__dmsg(ds, "reading");
		break;
	case WT_REF_SPLIT:
		__dmsg(ds, "split");
		break;
	default:
		__dmsg(ds, "INVALID");
		break;
	}

	__wt_ref_info(ref, &addr, &addr_size, NULL);
	__dmsg(ds, " %s\n",
	    __wt_addr_string(session, addr, addr_size, ds->tmp));
}

/*
 * __debug_cell --
 *	Dump a single unpacked WT_CELL.
 */
static int
__debug_cell(WT_DBG *ds, const WT_PAGE_HEADER *dsk, WT_CELL_UNPACK *unpack)
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
		case WT_CELL_KEY_OVFL_RM:
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
	case WT_CELL_ADDR_DEL:
		type = "addr/del";
		goto addr;
	case WT_CELL_ADDR_INT:
		type = "addr/int";
		goto addr;
	case WT_CELL_ADDR_LEAF:
		type = "addr/leaf";
		goto addr;
	case WT_CELL_ADDR_LEAF_NO:
		type = "addr/leaf-no";
		goto addr;
	case WT_CELL_KEY_OVFL:
	case WT_CELL_KEY_OVFL_RM:
	case WT_CELL_VALUE_OVFL:
	case WT_CELL_VALUE_OVFL_RM:
		type = "ovfl";
addr:		WT_RET(__wt_scr_alloc(session, 128, &buf));
		__dmsg(ds, ", %s %s", type,
		    __wt_addr_string(session, unpack->data, unpack->size, buf));
		__wt_scr_free(session, &buf);
		WT_RET(ret);
		break;
	}
	__dmsg(ds, "\n");

	return (__debug_cell_data(ds, NULL, dsk->type, NULL, unpack));
}

/*
 * __debug_cell_data --
 *	Dump a single cell's data in debugging mode.
 */
static int
__debug_cell_data(WT_DBG *ds,
    WT_PAGE *page, int page_type, const char *tag, WT_CELL_UNPACK *unpack)
{
	WT_DECL_ITEM(buf);
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	const char *p;

	session = ds->session;

	/*
	 * Column-store references to deleted cells return a NULL cell
	 * reference.
	 */
	if (unpack == NULL) {
		__debug_item(ds, tag, "deleted", strlen("deleted"));
		return (0);
	}

	switch (unpack->raw) {
	case WT_CELL_ADDR_DEL:
	case WT_CELL_ADDR_INT:
	case WT_CELL_ADDR_LEAF:
	case WT_CELL_ADDR_LEAF_NO:
	case WT_CELL_DEL:
	case WT_CELL_KEY_OVFL_RM:
	case WT_CELL_VALUE_OVFL_RM:
		p = __wt_cell_type_string(unpack->raw);
		__debug_item(ds, tag, p, strlen(p));
		break;
	case WT_CELL_KEY:
	case WT_CELL_KEY_OVFL:
	case WT_CELL_KEY_PFX:
	case WT_CELL_KEY_SHORT:
	case WT_CELL_KEY_SHORT_PFX:
	case WT_CELL_VALUE:
	case WT_CELL_VALUE_COPY:
	case WT_CELL_VALUE_OVFL:
	case WT_CELL_VALUE_SHORT:
		WT_RET(__wt_scr_alloc(session, 256, &buf));
		ret = page == NULL ?
		    __wt_dsk_cell_data_ref(session, page_type, unpack, buf) :
		    __wt_page_cell_data_ref(session, page, unpack, buf);
		if (ret == 0)
			__debug_item(ds, tag, buf->data, buf->size);
		__wt_scr_free(session, &buf);
		break;
	WT_ILLEGAL_VALUE(session);
	}

	return (ret);
}

/*
 * __debug_item --
 *	Dump a single data/size pair, with an optional tag.
 */
static void
__debug_item(WT_DBG *ds, const char *tag, const void *data_arg, size_t size)
{
	size_t i;
	u_char ch;
	const uint8_t *data;

	__dmsg(ds, "\t%s%s{", tag == NULL ? "" : tag, tag == NULL ? "" : " ");
	for (data = data_arg, i = 0; i < size; ++i, ++data) {
		ch = data[0];
		if (__wt_isprint(ch))
			__dmsg(ds, "%c", (int)ch);
		else
			__debug_hex_byte(ds, data[0]);
	}
	__dmsg(ds, "}\n");
}
#endif
