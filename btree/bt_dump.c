/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2010 WiredTiger, Inc.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

typedef struct {
	void (*p)				/* Print function */
	    (u_int8_t *, u_int32_t, FILE *);
	FILE *stream;				/* Dump stream */

	void (*f)(const char *, u_int64_t);	/* Progress callback */
	u_int64_t fcnt;				/* Progress counter */

	DBT *dupkey;				/* Offpage duplicate tree key */
} WT_DSTUFF;

static int  __wt_bt_dump_page(WT_TOC *, WT_PAGE *, void *);
static int  __wt_bt_dump_page_fixed(WT_TOC *, WT_PAGE *, WT_DSTUFF *);
static int  __wt_bt_dump_page_item(WT_TOC *, WT_PAGE *, WT_DSTUFF *);
static void __wt_bt_hexprint(u_int8_t *, u_int32_t, FILE *);
static void __wt_bt_print_nl(u_int8_t *, u_int32_t, FILE *);

/*
 * __wt_db_dump --
 *	Db.dump method.
 */
int
__wt_db_dump(WT_TOC *toc,
    FILE *stream, void (*f)(const char *, u_int64_t), u_int32_t flags)
{
	DB *db;
	IDB *idb;
	WT_DSTUFF dstuff;
	int ret;

	db = toc->db;
	idb = db->idb;

	if (LF_ISSET(WT_DEBUG)) {
#ifdef HAVE_DIAGNOSTIC
		/*
		 * We use the verification code to do debugging dumps because
		 * if we're dumping in debugging mode, we want to confirm the
		 * page is OK before blindly reading it.
		 */
		return (__wt_bt_verify(toc, f, stream));
#else
		__wt_api_db_errx(db, "library not built for debugging");
		return (WT_ERROR);
#endif
	}

	dstuff.p = flags == WT_PRINTABLES ? __wt_bt_print_nl : __wt_bt_hexprint;
	dstuff.stream = stream;
	dstuff.f = f;
	dstuff.fcnt = 0;
	dstuff.dupkey = NULL;

	/*
	 * Note we do not have a hazard reference for the root page, and that's
	 * safe -- root pages are pinned into memory when a database is opened,
	 * and never re-written until the database is closed.
	 */
	fprintf(stream, "VERSION=1\n");
	fprintf(stream, "HEADER=END\n");
	ret = __wt_bt_tree_walk(
	    toc, idb->root_addr, idb->root_size, __wt_bt_dump_page, &dstuff);
	fprintf(stream, "DATA=END\n");

	/* Wrap up reporting. */
	if (f != NULL)
		f(toc->name, dstuff.fcnt);

	return (ret);
}

/*
 * __wt_bt_dump_page --
 *	Depth-first recursive walk of a btree.
 */
static int
__wt_bt_dump_page(WT_TOC *toc, WT_PAGE *page, void *arg)
{
	DB *db;
	WT_DSTUFF *dp;

	db = toc->db;
	dp = arg;

	switch (page->hdr->type) {
	case WT_PAGE_COL_INT:
	case WT_PAGE_DUP_INT:
	case WT_PAGE_ROW_INT:
		break;
	case WT_PAGE_COL_FIX:
		WT_RET(__wt_bt_dump_page_fixed(toc, page, dp));
		break;
	case WT_PAGE_COL_VAR:
	case WT_PAGE_DUP_LEAF:
	case WT_PAGE_ROW_LEAF:
		WT_RET(__wt_bt_dump_page_item(toc, page, dp));
		break;
	WT_ILLEGAL_FORMAT(db);
	}

	/* Report progress every 10 pages. */
	if (dp->f != NULL && ++dp->fcnt % 10 == 0)
		dp->f(toc->name, dp->fcnt);

	return (0);
}

/*
 * __wt_bt_dump_page_item --
 *	Dump a page of WT_ITEM structures.
 */
static int
__wt_bt_dump_page_item(WT_TOC *toc, WT_PAGE *page, WT_DSTUFF *dp)
{
	DB *db;
	IDB *idb;
	DBT *key, *data, *key_process, *data_process, key_onpage, data_onpage;
	WT_ITEM *item;
	WT_OFF *off;
	WT_PAGE *key_ovfl, *data_ovfl;
	u_int32_t i;
	int ret;

	db = toc->db;
	idb = db->idb;
	key = NULL;
	key_process = data_process = NULL;
	key_ovfl = data_ovfl = NULL;
	ret = 0;

	/*
	 * There are two pairs of DBTs, one pair for key items and one pair for
	 * data items.  One of the pair is used for on-page items, the other is
	 * used for compressed or overflow items, that require processing.
	 */
	WT_CLEAR(key_onpage);
	WT_CLEAR(data_onpage);
	WT_ERR(__wt_scr_alloc(toc, &key_process));
	WT_ERR(__wt_scr_alloc(toc, &data_process));

	/*
	 * If we're passed a key by our caller, we're dumping an off-page
	 * duplicate tree and we'll write that key for each off-page item.
	 */
	if (dp->dupkey != NULL)
		key = dp->dupkey;

	WT_ITEM_FOREACH(page, item, i) {
		switch (WT_ITEM_TYPE(item)) {
		case WT_ITEM_KEY:
			if (idb->huffman_key == NULL) {
				key_onpage.data = WT_ITEM_BYTE(item);
				key_onpage.size = WT_ITEM_LEN(item);
				key = &key_onpage;
				continue;
			}
			/* FALLTHROUGH */
		case WT_ITEM_KEY_OVFL:
			/* Discard any previous overflow key page. */
			if (key_ovfl != NULL)
				__wt_bt_page_out(toc, &key_ovfl, 0);

			/* Get a copy of the key or an overflow key page. */
			WT_ERR(__wt_bt_item_process(
			    toc, item, &key_ovfl, key_process));
			if (key_ovfl == NULL)
				key = key_process;
			else {
				key_onpage.data = WT_PAGE_BYTE(key_ovfl);
				key_onpage.size = key_ovfl->hdr->u.datalen;
				key = &key_onpage;
			}
			continue;
		case WT_ITEM_DATA:
			if (idb->huffman_data == NULL) {
				data_onpage.data = WT_ITEM_BYTE(item);
				data_onpage.size = WT_ITEM_LEN(item);
				data = &data_onpage;
				break;
			}
			/* FALLTHROUGH */
		case WT_ITEM_DATA_OVFL:
		case WT_ITEM_DATA_DUP:
		case WT_ITEM_DATA_DUP_OVFL:
			/* Discard any previous overflow data page. */
			if (data_ovfl != NULL)
				__wt_bt_page_out(toc, &data_ovfl, 0);

			/* Get a copy of the data or an overflow data page. */
			WT_ERR(__wt_bt_item_process(
			    toc, item, &data_ovfl, data_process));
			if (data_ovfl == NULL)
				data = data_process;
			else {
				data_onpage.data = WT_PAGE_BYTE(data_ovfl);
				data_onpage.size = data_ovfl->hdr->u.datalen;
				data = &data_onpage;
			}
			break;
		case WT_ITEM_DEL:
			continue;
		case WT_ITEM_OFF:
			/*
			 * Off-page duplicate tree.   Set the key for display,
			 * and dump the entire tree.
			 */
			dp->dupkey = key;
			off = WT_ITEM_BYTE_OFF(item);
			WT_RET_RESTART(__wt_bt_tree_walk(toc,
			    off->addr, off->size, __wt_bt_dump_page, dp));
			dp->dupkey = NULL;
			continue;
		WT_ILLEGAL_FORMAT(db);
		}

		if (key != NULL)
			dp->p(key->data, key->size, dp->stream);
		dp->p(data->data, data->size, dp->stream);
	}

err:	/* Discard any space allocated to hold off-page key/data items. */
	if (key_process != NULL)
		__wt_scr_release(&key_process);
	if (data_process != NULL)
		__wt_scr_release(&data_process);

	/* Discard any overflow pages we're still holding. */
	if (key_ovfl != NULL)
		__wt_bt_page_out(toc, &key_ovfl, 0);
	if (data_ovfl != NULL)
		__wt_bt_page_out(toc, &data_ovfl, 0);

	return (ret);
}

/*
 * __wt_bt_dump_page_fixed --
 *	Dump a page of fixed-length objects.
 */
static int
__wt_bt_dump_page_fixed(WT_TOC *toc, WT_PAGE *page, WT_DSTUFF *dp)
{
	DB *db;
	IDB *idb;
	u_int32_t i, j;
	u_int8_t *p;

	db = toc->db;
	idb = db->idb;

	if (F_ISSET(idb, WT_REPEAT_COMP))
		WT_FIX_REPEAT_ITERATE(db, page, p, i, j)
			dp->p(WT_FIX_REPEAT_DATA(p), db->fixed_len, dp->stream);
	else
		WT_FIX_FOREACH(db, page, p, i)
			dp->p(p, db->fixed_len, dp->stream);
	return (0);
}

static const char hex[] = "0123456789abcdef";

/*
 * __wt_bt_print_nl --
 *	Output a single key/data entry in printable characters, where possible.
 *	In addition, terminate with a <newline> character, unless the entry is
 *	itself terminated with a <newline> character.
 */
static void
__wt_bt_print_nl(u_int8_t *data, u_int32_t size, FILE *stream)
{
	if (data[size - 1] == '\n')
		--size;
	__wt_bt_print(data, size, stream);
	fprintf(stream, "\n");
}

/*
 * __wt_bt_print --
 *	Output a single key/data entry in printable characters, where possible.
 */
void
__wt_bt_print(u_int8_t *data, u_int32_t size, FILE *stream)
{
	int ch;

	for (; size > 0; --size, ++data) {
		ch = data[0];
		if (isprint(ch))
			fprintf(stream, "%c", ch);
		else
			fprintf(stream, "%x%x",
			    hex[(data[0] & 0xf0) >> 4], hex[data[0] & 0x0f]);
	}
}

/*
 * __wt_bt_hexprint --
 *	Output a single key/data entry in hex.
 */
static void
__wt_bt_hexprint(u_int8_t *data, u_int32_t size, FILE *stream)
{
	for (; size > 0; --size, ++data)
		fprintf(stream, "%x%x",
		    hex[(data[0] & 0xf0) >> 4], hex[data[0] & 0x0f]);
	fprintf(stream, "\n");
}
