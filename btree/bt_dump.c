/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
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

	DBT *dupkey;				/* Key for offpage duplicates */
} WT_DSTUFF;

static int __wt_bt_dump(WT_TOC *, u_int32_t, u_int32_t, WT_DSTUFF *);
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
		 * page is OK before walking it.
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
	ret = __wt_bt_dump(toc, idb->root_addr, idb->root_size, &dstuff);

	/* Wrap up reporting. */
	if (f != NULL)
		f(toc->name, dstuff.fcnt);

	return (ret);
}

/*
 * __wt_bt_dump --
 *	Depth-first recursive walk of a btree.
 */
static int
__wt_bt_dump(WT_TOC *toc, u_int32_t addr, u_int32_t size, WT_DSTUFF *dp)
{
	DB *db;
	WT_COL_INDX *page_cip;
	WT_OFF *off;
	WT_PAGE *page;
	WT_ROW_INDX *page_rip;
	u_int32_t i;
	int ret;

	db = toc->db;
	ret = 0;

	WT_RET(__wt_bt_page_in(toc, addr, size, 1, &page));

	switch (page->hdr->type) {
	case WT_PAGE_COL_INT:
		WT_INDX_FOREACH(page, page_cip, i) {
			off = (WT_OFF *)page_cip->page_data;
			WT_ERR(__wt_bt_dump(toc, off->addr, off->size, dp));
		}
		break;
	case WT_PAGE_DUP_INT:
	case WT_PAGE_ROW_INT:
		WT_INDX_FOREACH(page, page_rip, i) {
			off = WT_ITEM_BYTE_OFF(page_rip->page_data);
			WT_ERR(__wt_bt_dump(toc, off->addr, off->size, dp));
		}
		break;
	case WT_PAGE_COL_FIX:
		WT_ERR(__wt_bt_dump_page_fixed(toc, page, dp));
		break;
	case WT_PAGE_COL_VAR:
	case WT_PAGE_DUP_LEAF:
	case WT_PAGE_ROW_LEAF:
		WT_ERR(__wt_bt_dump_page_item(toc, page, dp));
		break;
	WT_ILLEGAL_FORMAT_ERR(db, ret);
	}

	__wt_bt_page_out(toc, &page, 0);

	/* Report progress every 10 pages. */
	if (dp->f != NULL && ++dp->fcnt % 10 == 0)
		dp->f(toc->name, dp->fcnt);

err:	if (page != NULL)
		__wt_bt_page_out(toc, &page, 0);

	return (ret);
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
__wt_bt_dump_page_item(WT_TOC *toc, WT_PAGE *page, WT_DSTUFF *dp)
{
	DB *db;
	DBT last_key_ovfl, last_key_std, *last_key;
	ENV *env;
	WT_ITEM *item;
	WT_OFF *off;
	WT_OVFL *ovfl;
	WT_PAGE *ovfl_page;
	u_int32_t i, item_len;
	int dup_ahead, ret;

	db = toc->db;
	env = toc->env;
	WT_CLEAR(last_key_std);
	WT_CLEAR(last_key_ovfl);
	ret = 0;

	/*
	 * If we're passed a key, then we're dumping an off-page duplicate tree,
	 * and we'll write the key for each off-page item.
	 */
	if (dp->dupkey != NULL)
		last_key = dp->dupkey;

	WT_ITEM_FOREACH(page, item, i) {
		item_len = WT_ITEM_LEN(item);
		switch (WT_ITEM_TYPE(item)) {
		case WT_ITEM_KEY:
			last_key_std.data = WT_ITEM_BYTE(item);
			last_key_std.size = item_len;
			last_key = &last_key_std;
			/*
			 * If about to dump an off-page duplicate tree, don't
			 * write the key here, we'll write it when writing the
			 * off-page duplicates.
			 */
			WT_DUP_AHEAD(item, dup_ahead);
			if (!dup_ahead)
				dp->p(WT_ITEM_BYTE(item), item_len, dp->stream);
			break;
		case WT_ITEM_DATA:
			dp->p(WT_ITEM_BYTE(item), item_len, dp->stream);
			break;
		case WT_ITEM_DUP:
			dp->p(last_key->data, last_key->size, dp->stream);
			dp->p(WT_ITEM_BYTE(item), item_len, dp->stream);
			break;
		case WT_ITEM_KEY_OVFL:
			/*
			 * If the overflow key has duplicate records, we'll need
			 * a copy of the key to write for each of those records.
			 * Look ahead and see if it's a set of duplicates.
			 */
			WT_DUP_AHEAD(item, dup_ahead);
			/* FALLTHROUGH */
		case WT_ITEM_DATA_OVFL:
		case WT_ITEM_DUP_OVFL:
			ovfl = WT_ITEM_BYTE_OVFL(item);
			WT_ERR(__wt_bt_ovfl_in(
			    toc, ovfl->addr, ovfl->size, &ovfl_page));

			/* If we're already in a duplicate set, dump the key. */
			if (WT_ITEM_TYPE(item) == WT_ITEM_DUP_OVFL)
				dp->p(
				    last_key->data, last_key->size, dp->stream);

			/*
			 * If starting a new duplicate set with an overflow key,
			 * save a copy of the key for later writing.  Otherwise,
			 * dump this item.
			 */
			if (dup_ahead) {
				WT_ERR(__wt_bt_data_copy_to_dbt(db,
				    WT_PAGE_BYTE(ovfl_page), ovfl->size,
				    &last_key_ovfl));
				last_key = &last_key_ovfl;
				dup_ahead = 0;
			} else
				dp->p(WT_PAGE_BYTE(
				   ovfl_page), ovfl->size, dp->stream);

			__wt_bt_page_out(toc, &ovfl_page, 0);
			break;
		case WT_ITEM_OFF:
			/*
			 * Off-page duplicate tree.   Set the key for display,
			 * and dump the entire tree.
			 */
			dp->dupkey = last_key;
			off = WT_ITEM_BYTE_OFF(item);
			WT_RET(__wt_bt_dump(toc, off->addr, off->size, dp));
			dp->dupkey = NULL;
			break;
		WT_ILLEGAL_FORMAT(db);
		}
	}

err:	/* Discard any space allocated to hold an overflow key. */
	__wt_free(env, last_key_ovfl.data, last_key_ovfl.mem_size);

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
			dp->p(p + sizeof(u_int16_t), db->fixed_len, dp->stream);
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
