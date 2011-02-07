/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"
#include "bt_inline.c"

typedef struct {
	void (*p)				/* Print function */
	    (uint8_t *, uint32_t, FILE *);
	FILE *stream;				/* Dump stream */

	void (*f)(const char *, uint64_t);	/* Progress callback */
	uint64_t fcnt;				/* Progress counter */
} WT_DSTUFF;

static int  __wt_dump_page(WT_TOC *, WT_PAGE *, void *);
static void __wt_dump_page_col_fix(WT_TOC *, WT_PAGE *, WT_DSTUFF *);
static int  __wt_dump_page_col_rle(WT_TOC *, WT_PAGE *, WT_DSTUFF *);
static int  __wt_dump_page_col_var(WT_TOC *, WT_PAGE *, WT_DSTUFF *);
static int  __wt_dump_page_row_leaf(WT_TOC *, WT_PAGE *, WT_DSTUFF *);
static void __wt_print_byte_string_hex(uint8_t *, uint32_t, FILE *);
static void __wt_print_byte_string_nl(uint8_t *, uint32_t, FILE *);

/*
 * __wt_db_dump --
 *	Db.dump method.
 */
int
__wt_db_dump(WT_TOC *toc,
    FILE *stream, void (*f)(const char *, uint64_t), uint32_t flags)
{
	WT_DSTUFF dstuff;
	int ret;

	if (LF_ISSET(WT_DEBUG)) {
		/*
		 * We use the verification code to do debugging dumps because
		 * if we're dumping in debugging mode, we want to confirm the
		 * page is OK before blindly reading it.
		 */
		return (__wt_verify(toc, f, stream));
	}

	dstuff.p = flags == WT_PRINTABLES ?
	    __wt_print_byte_string_nl : __wt_print_byte_string_hex;
	dstuff.stream = stream;
	dstuff.f = f;
	dstuff.fcnt = 0;

	/*
	 * Note we do not have a hazard reference for the root page, and that's
	 * safe -- root pages are pinned into memory when a file is opened, and
	 * never re-written until the file is closed.
	 */
	fprintf(stream, "VERSION=1\n");
	fprintf(stream, "HEADER=END\n");
	ret = __wt_tree_walk(toc, NULL, 0, __wt_dump_page, &dstuff);
	fprintf(stream, "DATA=END\n");

	/* Wrap up reporting. */
	if (f != NULL)
		f(toc->name, dstuff.fcnt);

	return (ret);
}

/*
 * __wt_dump_page --
 *	Depth-first recursive walk of a btree.
 */
static int
__wt_dump_page(WT_TOC *toc, WT_PAGE *page, void *arg)
{
	DB *db;
	WT_DSTUFF *dp;

	db = toc->db;
	dp = arg;

	switch (page->dsk->type) {
	case WT_PAGE_COL_INT:
	case WT_PAGE_ROW_INT:
		break;
	case WT_PAGE_COL_FIX:
		__wt_dump_page_col_fix(toc, page, dp);
		break;
	case WT_PAGE_COL_RLE:
		WT_RET(__wt_dump_page_col_rle(toc, page, dp));
		break;
	case WT_PAGE_COL_VAR:
		WT_RET(__wt_dump_page_col_var(toc, page, dp));
		break;
	case WT_PAGE_ROW_LEAF:
		WT_RET(__wt_dump_page_row_leaf(toc, page, dp));
		break;
	WT_ILLEGAL_FORMAT(db);
	}

	/* Report progress every 10 pages. */
	if (dp->f != NULL && ++dp->fcnt % 10 == 0)
		dp->f(toc->name, dp->fcnt);

	return (0);
}

/*
 * __wt_dump_page_col_fix --
 *	Dump a WT_PAGE_COL_FIX page.
 */
static void
__wt_dump_page_col_fix(WT_TOC *toc, WT_PAGE *page, WT_DSTUFF *dp)
{
	DB *db;
	WT_COL *cip;
	WT_PAGE_DISK *dsk;
	WT_UPDATE *upd;
	uint32_t i;
	void *cipdata;

	db = toc->db;
	dsk = page->dsk;

	/* Walk the page, dumping data items. */
	WT_COL_INDX_FOREACH(page, cip, i) {
		cipdata = WT_COL_PTR(dsk, cip);
		if ((upd = WT_COL_UPDATE(page, cip)) == NULL) {
			if (!WT_FIX_DELETE_ISSET(cipdata))
				dp->p(cipdata, db->fixed_len, dp->stream);
		} else
			if (!WT_UPDATE_DELETED_ISSET(upd))
				dp->p(WT_UPDATE_DATA(upd),
				    db->fixed_len, dp->stream);
	}
}

/*
 * __wt_dump_page_col_rle --
 *	Dump a WT_PAGE_COL_RLE page.
 */
static int
__wt_dump_page_col_rle(WT_TOC *toc, WT_PAGE *page, WT_DSTUFF *dp)
{
	DB *db;
	ENV *env;
	FILE *fp;
	WT_COL *cip;
	WT_PAGE_DISK *dsk;
	WT_RLE_EXPAND *exp, **expsort, **expp;
	WT_UPDATE *upd;
	uint64_t recno;
	uint32_t i, n_expsort;
	uint16_t n_repeat;
	void *cipdata;

	db = toc->db;
	env = toc->env;
	dsk = page->dsk;
	fp = dp->stream;
	expsort = NULL;
	n_expsort = 0;

	recno = page->dsk->recno;
	WT_COL_INDX_FOREACH(page, cip, i) {
		cipdata = WT_COL_PTR(dsk, cip);
		/*
		 * Get a sorted list of any expansion entries we've created for
		 * this set of records.  The sort function returns a NULL-
		 * terminated array of references to WT_RLE_EXPAND structures,
		 * sorted by record number.
		 */
		WT_RET(__wt_rle_expand_sort(
		    toc, page, cip, &expsort, &n_expsort));

		/*
		 * Dump the records.   We use the WT_UPDATE entry for records in
		 * in the WT_RLE_EXPAND array, and original data otherwise.
		 */
		for (expp = expsort,
		    n_repeat = WT_RLE_REPEAT_COUNT(cipdata);
		    n_repeat > 0; --n_repeat, ++recno)
			if ((exp = *expp) != NULL && exp->recno == recno) {
				++expp;
				upd = exp->upd;
				if (!WT_UPDATE_DELETED_ISSET(upd))
					dp->p(
					    WT_UPDATE_DATA(upd), upd->size, fp);
			} else
				if (!WT_FIX_DELETE_ISSET(
				    WT_RLE_REPEAT_DATA(cipdata)))
					dp->p(WT_RLE_REPEAT_DATA(
					    cipdata), db->fixed_len, fp);
	}
	/* Free the sort array. */
	if (expsort != NULL)
		__wt_free(env, expsort, n_expsort * sizeof(WT_RLE_EXPAND *));

	return (0);
}

/*
 * __wt_dump_page_col_var --
 *	Dump a WT_PAGE_COL_VAR page.
 */
static int
__wt_dump_page_col_var(WT_TOC *toc, WT_PAGE *page, WT_DSTUFF *dp)
{
	DB *db;
	DBT *tmp;
	WT_COL *cip;
	WT_ITEM *item;
	WT_PAGE_DISK *dsk;
	WT_UPDATE *upd;
	int ret;
	uint32_t i;
	void *huffman;

	db = toc->db;
	dsk = page->dsk;
	huffman = db->btree->huffman_data;
	ret = 0;

	WT_RET(__wt_scr_alloc(toc, 0, &tmp));
	WT_COL_INDX_FOREACH(page, cip, i) {
		/* Check for update. */
		if ((upd = WT_COL_UPDATE(page, cip)) != NULL) {
			if (!WT_UPDATE_DELETED_ISSET(upd))
				dp->p(
				    WT_UPDATE_DATA(upd), upd->size, dp->stream);
			continue;
		}

		/* Process the original data. */
		item = WT_COL_PTR(dsk, cip);
		switch (WT_ITEM_TYPE(item)) {
		case WT_ITEM_DATA:
			if (huffman == NULL) {
				dp->p(WT_ITEM_BYTE(item),
				    WT_ITEM_LEN(item), dp->stream);
				break;
			}
			/* FALLTHROUGH */
		case WT_ITEM_DATA_OVFL:
			WT_ERR(__wt_item_process(toc, item, tmp));
			dp->p(tmp->data, tmp->size, dp->stream);
			break;
		case WT_ITEM_DEL:
			break;
		WT_ILLEGAL_FORMAT_ERR(db, ret);
		}
	}

err:	__wt_scr_release(&tmp);
	return (ret);
}

/*
 * __wt_dump_page_row_leaf --
 *	Dump a WT_PAGE_ROW_LEAF page.
 */
static int
__wt_dump_page_row_leaf(WT_TOC *toc, WT_PAGE *page, WT_DSTUFF *dp)
{
	DB *db;
	DBT *key, *data, *key_tmp, *data_tmp, key_local, data_local;
	WT_ITEM *item;
	WT_ROW *rip;
	WT_UPDATE *upd;
	uint32_t i;
	int ret;
	void *huffman;

	db = toc->db;
	key = data = key_tmp = data_tmp = NULL;
	huffman = db->btree->huffman_data;
	ret = 0;

	WT_ERR(__wt_scr_alloc(toc, 0, &key_tmp));
	WT_ERR(__wt_scr_alloc(toc, 0, &data_tmp));
	WT_CLEAR(key_local);
	WT_CLEAR(data_local);

	WT_ROW_INDX_FOREACH(page, rip, i) {
		/* Check for deletion. */
		upd = WT_ROW_UPDATE(page, rip);
		if (upd != NULL && WT_UPDATE_DELETED_ISSET(upd))
			continue;

		/*
		 * The key and data variables reference the DBT's we'll print.
		 * Set the key.
		 */
		if (__wt_key_process(rip)) {
			WT_ERR(__wt_item_process(toc, rip->key, key_tmp));
			key = key_tmp;
		} else
			key = (DBT *)rip;

		/*
		 * If the item was ever updated, dump the data from the
		 * update entry.
		 */
		if (upd != NULL) {
			dp->p(key->data, key->size, dp->stream);
			dp->p(WT_UPDATE_DATA(upd), upd->size, dp->stream);
			continue;
		}

		/* Set data to reference the data we'll dump. */
		item = rip->data;
		switch (WT_ITEM_TYPE(item)) {
		case WT_ITEM_DATA:
			if (huffman == NULL) {
				data_local.data = WT_ITEM_BYTE(item);
				data_local.size = WT_ITEM_LEN(item);
				data = &data_local;
				break;
			}
			/* FALLTHROUGH */
		case WT_ITEM_DATA_OVFL:
			WT_ERR(__wt_item_process(toc, item, data_tmp));
			data = data_tmp;
			break;
		WT_ILLEGAL_FORMAT_ERR(db, ret);
		}

		dp->p(key->data, key->size, dp->stream);
		dp->p(data->data, data->size, dp->stream);
	}

err:	/* Discard any space allocated to hold off-page key/data items. */
	if (key_tmp != NULL)
		__wt_scr_release(&key_tmp);
	if (data_tmp != NULL)
		__wt_scr_release(&data_tmp);

	return (ret);
}

static const char hex[] = "0123456789abcdef";

/*
 * __wt_print_byte_string_nl --
 *	Output a single byte stringin printable characters, where possible.
 *	In addition, terminate with a <newline> character, unless the entry
 *	is itself terminated with a <newline> character.
 */
static void
__wt_print_byte_string_nl(uint8_t *data, uint32_t size, FILE *stream)
{
	if (size > 0 && data[size - 1] == '\n')
		--size;
	__wt_print_byte_string(data, size, stream);
	fprintf(stream, "\n");
}

/*
 * __wt_print_byte_string --
 *	Output a single byte string in printable characters, where possible.
 */
void
__wt_print_byte_string(uint8_t *data, uint32_t size, FILE *stream)
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
 * __wt_print_byte_string_hex --
 *	Output a single byte string in hexadecimal characters.
 */
static void
__wt_print_byte_string_hex(uint8_t *data, uint32_t size, FILE *stream)
{
	for (; size > 0; --size, ++data)
		fprintf(stream, "%x%x",
		    hex[(data[0] & 0xf0) >> 4], hex[data[0] & 0x0f]);
	fprintf(stream, "\n");
}
