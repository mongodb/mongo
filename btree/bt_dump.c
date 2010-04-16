/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int  __wt_bt_dump_offpage(WT_TOC *, DBT *,
    WT_ITEM *, FILE *, void (*)(u_int8_t *, u_int32_t, FILE *));
static int  __wt_bt_dump_page_fixed(WT_TOC *, WT_PAGE *, FILE *, u_int32_t);
static int  __wt_bt_dump_page_item(WT_TOC *, WT_PAGE *, FILE *, u_int32_t);
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
	WT_PAGE *page;
	WT_PAGE_HDR *hdr;
	u_int64_t fcnt;
	u_int32_t addr, size;

	db = toc->db;
	idb = db->idb;
	fcnt = 0;

	if (WT_UNOPENED_DATABASE(idb))
		return (0);

	if (LF_ISSET(WT_DEBUG)) {
#ifdef HAVE_DIAGNOSTIC
		/*
		 * We use the verification code to do debugging dumps because
		 * if we're dumping in debugging mode, we want to confirm the
		 * page is OK before walking it.
		 */
		return (__wt_bt_verify_int(toc, f, stream));
#else
		__wt_api_db_errx(db, "library not built for debugging");
		return (WT_ERROR);
#endif
	}

	/*
	 * The first physical page of the database is guaranteed to be the first
	 * leaf page in the database; walk the linked list of leaf pages.
	 */
	WT_RET(__wt_bt_leaf_first(toc, idb->root_addr, idb->root_size, &page));
	for (;;) {
		hdr = page->hdr;
		switch (hdr->type) {
		case WT_PAGE_COL_FIX:
			WT_RET(
			    __wt_bt_dump_page_fixed(toc, page, stream, flags));
			break;
		case WT_PAGE_COL_VAR:
		case WT_PAGE_DUP_INT:
		case WT_PAGE_DUP_LEAF:
		case WT_PAGE_ROW_LEAF:
			WT_RET(
			    __wt_bt_dump_page_item(toc, page, stream, flags));
			break;
		WT_ILLEGAL_FORMAT(db);
		}

		addr = hdr->nextaddr;
		size = hdr->nextsize;
		__wt_bt_page_out(toc, &page, 0);
		if (addr == WT_ADDR_INVALID)
			break;
		WT_RET(__wt_bt_page_in(toc, addr, size, 0, &page));

		/* Report progress every 10 pages. */
		if (f != NULL && ++fcnt % 10 == 0)
			f(toc->name, fcnt);
	}

	/* Wrap up reporting. */
	if (f != NULL)
		f(toc->name, fcnt);

	return (0);
}

/* Check if the next page entry is part of a duplicate data set. */
#define	WT_DUP_AHEAD(item, yesno) {					\
	u_int32_t __type = WT_ITEM_TYPE(WT_ITEM_NEXT(item));		\
	(yesno) = __type == WT_ITEM_DUP ||				\
	    __type == WT_ITEM_DUP_OVFL ||				\
	    __type == WT_ITEM_OFF ? 1 : 0;				\
}

/*
 * __wt_bt_dump_page_item --
 *	Dump a page of WT_ITEM structures.
 */
static int
__wt_bt_dump_page_item(
    WT_TOC *toc, WT_PAGE *page, FILE *stream, u_int32_t flags)
{
	DB *db;
	DBT last_key_ovfl, last_key_std, *last_key;
	ENV *env;
	WT_ITEM *item;
	WT_OVFL *ovfl;
	WT_PAGE *ovfl_page;
	u_int32_t i, item_len;
	int dup_ahead, ret;
	void (*func)(u_int8_t *, u_int32_t, FILE *);

	db = toc->db;
	env = toc->env;
	WT_CLEAR(last_key_std);
	WT_CLEAR(last_key_ovfl);
	ret = 0;
	func = flags == WT_PRINTABLES ? __wt_bt_print_nl : __wt_bt_hexprint;

	WT_ITEM_FOREACH(page, item, i) {
		item_len = WT_ITEM_LEN(item);
		switch (WT_ITEM_TYPE(item)) {
		case WT_ITEM_KEY:
			last_key_std.data = WT_ITEM_BYTE(item);
			last_key_std.size = item_len;
			last_key = &last_key_std;
			/*
			 * If we're about to dump an off-page duplicate
			 * set, don't write the key here, we'll write
			 * it in the off-page dump routine.
			 */
			WT_DUP_AHEAD(item, dup_ahead);
			if (!dup_ahead)
				func(WT_ITEM_BYTE(item), item_len, stream);
			break;
		case WT_ITEM_DATA:
			func(WT_ITEM_BYTE(item), item_len, stream);
			break;
		case WT_ITEM_DUP:
			func(last_key->data, last_key->size, stream);
			func(WT_ITEM_BYTE(item), item_len, stream);
			break;
		case WT_ITEM_KEY_OVFL:
			/*
			 * If the overflow key has duplicate records,
			 * we'll need a copy of the key for display on
			 * each of those records.  Look ahead and see
			 * if it's a set of duplicates.
			 */
			WT_DUP_AHEAD(item, dup_ahead);
			/* FALLTHROUGH */
		case WT_ITEM_DATA_OVFL:
		case WT_ITEM_DUP_OVFL:
			ovfl = WT_ITEM_BYTE_OVFL(item);
			WT_ERR(__wt_bt_ovfl_in(
			    toc, ovfl->addr, ovfl->size, &ovfl_page));

			/*
			 * If we're already in a duplicate set, dump
			 * the key.
			 */
			if (WT_ITEM_TYPE(item) == WT_ITEM_DUP_OVFL)
				func(last_key->data, last_key->size, stream);

			/*
			 * If we're starting a new duplicate set with
			 * an overflow key, save a copy of the key for
			 * later display.  Otherwise, dump this item.
			 */
			if (dup_ahead) {
				WT_ERR(__wt_bt_data_copy_to_dbt(db,
				    WT_PAGE_BYTE(ovfl_page), ovfl->size,
				    &last_key_ovfl));
				last_key = &last_key_ovfl;
				dup_ahead = 0;
			} else
				func(
				   WT_PAGE_BYTE(ovfl_page), ovfl->size, stream);

			__wt_bt_page_out(toc, &ovfl_page, 0);
			break;
		case WT_ITEM_OFF:
			WT_ERR(__wt_bt_dump_offpage(
			    toc, last_key, item, stream, func));
			break;
		WT_ILLEGAL_FORMAT(db);
		}
	}

err:	/* Discard any space allocated to hold an overflow key. */
	__wt_free(env, last_key_ovfl.data, last_key_ovfl.mem_size);

	return (ret);
}

/*
 * __wt_bt_dump_offpage --
 *	Dump a set of off-page duplicates.
 */
static int
__wt_bt_dump_offpage(WT_TOC *toc, DBT *key, WT_ITEM *item,
    FILE *stream, void (*func)(u_int8_t *, u_int32_t, FILE *))
{
	DB *db;
	WT_OFF *off;
	WT_OVFL *ovfl;
	WT_PAGE *page, *ovfl_page;
	u_int32_t addr, size, i;
	int ret;

	db = toc->db;
	page = NULL;
	ret = 0;

	/*
	 * Callers pass us a reference to an on-page WT_ITEM_OFF.
	 *
	 * Walk down the duplicates tree to the first leaf page.
	 */
	off = WT_ITEM_BYTE_OFF(item);
	WT_RET(__wt_bt_leaf_first(toc, off->addr, off->size, &page));
	for (;;) {
		WT_ITEM_FOREACH(page, item, i) {
			func(key->data, key->size, stream);
			switch (WT_ITEM_TYPE(item)) {
			case WT_ITEM_DUP:
				func(WT_ITEM_BYTE(item),
				    WT_ITEM_LEN(item), stream);
				break;
			case WT_ITEM_DUP_OVFL:
				ovfl = WT_ITEM_BYTE_OVFL(item);
				WT_ERR(__wt_bt_ovfl_in(toc,
				    ovfl->addr, ovfl->size, &ovfl_page));
				func(WT_PAGE_BYTE(
				    ovfl_page), ovfl->size, stream);
				__wt_bt_page_out(toc, &ovfl_page, 0);
				break;
			WT_ILLEGAL_FORMAT(db);
			}
		}

		addr = page->hdr->nextaddr;
		size = page->hdr->nextsize;
		__wt_bt_page_out(toc, &page, 0);
		if (addr == WT_ADDR_INVALID ||
		    (ret = __wt_bt_page_in(toc, addr, size, 0, &page)) != 0)
			break;
	}

err:	if (page != NULL)
		__wt_bt_page_out(toc, &page, 0);
	return (ret);
}

/*
 * __wt_bt_dump_page_fixed --
 *	Dump a page of fixed-length objects.
 */
static int
__wt_bt_dump_page_fixed(
    WT_TOC *toc, WT_PAGE *page, FILE *stream, u_int32_t flags)
{
	DB *db;
	IDB *idb;
	u_int32_t i, j;
	u_int8_t *p;
	void (*func)(u_int8_t *, u_int32_t, FILE *);

	db = toc->db;
	idb = db->idb;
	func = flags == WT_PRINTABLES ? __wt_bt_print_nl : __wt_bt_hexprint;

	if (F_ISSET(idb, WT_REPEAT_COMP))
		WT_FIX_REPEAT_ITERATE(db, page, p, i, j)
			func(p + sizeof(u_int16_t), db->fixed_len, stream);
	else
		WT_FIX_FOREACH(db, page, p, i)
			func(p, db->fixed_len, stream);
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
