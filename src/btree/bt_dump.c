/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"
#include "btree.i"

typedef struct {
	void (*p)				/* Print function */
	    (const uint8_t *, uint32_t, FILE *);
	FILE *stream;				/* Dump stream */

	void (*f)(const char *, uint64_t);	/* Progress callback */
	uint64_t fcnt;				/* Progress counter */
} WT_DSTUFF;

static int  __wt_dump_page(SESSION *, WT_PAGE *, void *);
static void __wt_dump_page_col_fix(SESSION *, WT_PAGE *, WT_DSTUFF *);
static int  __wt_dump_page_col_rle(SESSION *, WT_PAGE *, WT_DSTUFF *);
static int  __wt_dump_page_col_var(SESSION *, WT_PAGE *, WT_DSTUFF *);
static int  __wt_dump_page_row_leaf(SESSION *, WT_PAGE *, WT_DSTUFF *);
static void __wt_print_byte_string_hex(const uint8_t *, uint32_t, FILE *);
static void __wt_print_byte_string_nl(const uint8_t *, uint32_t, FILE *);

/*
 * __wt_btree_dump --
 *	Db.dump method.
 */
int
__wt_btree_dump(SESSION *session,
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
		return (__wt_verify(session, f, stream));
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
	ret = __wt_tree_walk(session, NULL, 0, __wt_dump_page, &dstuff);
	fprintf(stream, "DATA=END\n");

	/* Wrap up reporting. */
	if (f != NULL)
		f(session->name, dstuff.fcnt);

	return (ret);
}

/*
 * __wt_dump_page --
 *	Depth-first recursive walk of a btree.
 */
static int
__wt_dump_page(SESSION *session, WT_PAGE *page, void *arg)
{
	BTREE *btree;
	WT_DSTUFF *dp;

	btree = session->btree;
	dp = arg;

	switch (page->type) {
	case WT_PAGE_COL_INT:
	case WT_PAGE_ROW_INT:
		break;
	case WT_PAGE_COL_FIX:
		__wt_dump_page_col_fix(session, page, dp);
		break;
	case WT_PAGE_COL_RLE:
		WT_RET(__wt_dump_page_col_rle(session, page, dp));
		break;
	case WT_PAGE_COL_VAR:
		WT_RET(__wt_dump_page_col_var(session, page, dp));
		break;
	case WT_PAGE_ROW_LEAF:
		WT_RET(__wt_dump_page_row_leaf(session, page, dp));
		break;
	WT_ILLEGAL_FORMAT(btree);
	}

	/* Report progress every 10 pages. */
	if (dp->f != NULL && ++dp->fcnt % 10 == 0)
		dp->f(session->name, dp->fcnt);

	return (0);
}

/*
 * __wt_dump_page_col_fix --
 *	Dump a WT_PAGE_COL_FIX page.
 */
static void
__wt_dump_page_col_fix(SESSION *session, WT_PAGE *page, WT_DSTUFF *dp)
{
	BTREE *btree;
	WT_COL *cip;
	WT_UPDATE *upd;
	uint32_t i;
	void *cipdata;

	btree = session->btree;

	/* Walk the page, dumping data items. */
	WT_COL_INDX_FOREACH(page, cip, i) {
		cipdata = WT_COL_PTR(page, cip);
		if ((upd = WT_COL_UPDATE(page, cip)) == NULL) {
			if (!WT_FIX_DELETE_ISSET(cipdata))
				dp->p(cipdata, btree->fixed_len, dp->stream);
		} else
			if (!WT_UPDATE_DELETED_ISSET(upd))
				dp->p(WT_UPDATE_DATA(upd),
				    btree->fixed_len, dp->stream);
	}
}

/*
 * __wt_dump_page_col_rle --
 *	Dump a WT_PAGE_COL_RLE page.
 */
static int
__wt_dump_page_col_rle(SESSION *session, WT_PAGE *page, WT_DSTUFF *dp)
{
	BTREE *btree;
	FILE *fp;
	WT_BUF *tmp;
	WT_COL *cip;
	WT_RLE_EXPAND *exp, **expsort, **expp;
	WT_UPDATE *upd;
	uint64_t recno;
	uint32_t i;
	uint16_t n_repeat;
	void *cipdata;
	int ret;

	btree = session->btree;
	fp = dp->stream;
	tmp = NULL;
	ret = 0;

	recno = page->u.col_leaf.recno;
	WT_COL_INDX_FOREACH(page, cip, i) {
		cipdata = WT_COL_PTR(page, cip);
		/*
		 * Get a sorted list of any expansion entries we've created for
		 * this set of records.  The sort function returns a NULL-
		 * terminated array of references to WT_RLE_EXPAND structures,
		 * sorted by record number.
		 */
		WT_ERR(
		    __wt_rle_expand_sort(session, page, cip, &expsort, &tmp));

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
					    cipdata), btree->fixed_len, fp);
	}

	/* Free the sort array. */
err:	if (tmp != NULL)
		__wt_scr_release(&tmp);

	return (ret);
}

/*
 * __wt_dump_page_col_var --
 *	Dump a WT_PAGE_COL_VAR page.
 */
static int
__wt_dump_page_col_var(SESSION *session, WT_PAGE *page, WT_DSTUFF *dp)
{
	BTREE *btree;
	WT_BUF *tmp;
	WT_COL *cip;
	WT_CELL *cell;
	WT_UPDATE *upd;
	int ret;
	uint32_t i;
	void *huffman;

	btree = session->btree;
	huffman = btree->huffman_data;
	ret = 0;

	WT_RET(__wt_scr_alloc(session, 0, &tmp));
	WT_COL_INDX_FOREACH(page, cip, i) {
		/* Check for update. */
		if ((upd = WT_COL_UPDATE(page, cip)) != NULL) {
			if (!WT_UPDATE_DELETED_ISSET(upd))
				dp->p(
				    WT_UPDATE_DATA(upd), upd->size, dp->stream);
			continue;
		}

		/* Process the original data. */
		cell = WT_COL_PTR(page, cip);
		switch (WT_CELL_TYPE(cell)) {
		case WT_CELL_DATA:
			if (huffman == NULL) {
				dp->p(WT_CELL_BYTE(cell),
				    WT_CELL_LEN(cell), dp->stream);
				break;
			}
			/* FALLTHROUGH */
		case WT_CELL_DATA_OVFL:
			WT_ERR(__wt_cell_process(session, cell, tmp));
			dp->p(tmp->item.data, tmp->item.size, dp->stream);
			break;
		case WT_CELL_DEL:
			break;
		WT_ILLEGAL_FORMAT_ERR(btree, ret);
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
__wt_dump_page_row_leaf(SESSION *session, WT_PAGE *page, WT_DSTUFF *dp)
{
	BTREE *btree;
	WT_ITEM *key, *value, key_local, value_local;
	WT_BUF *key_tmp, *value_tmp;
	WT_CELL *cell;
	WT_ROW *rip;
	WT_UPDATE *upd;
	uint32_t i;
	int ret;
	void *huffman;

	btree = session->btree;
	key = value = NULL;
	key_tmp = value_tmp = NULL;
	huffman = btree->huffman_data;
	ret = 0;

	WT_ERR(__wt_scr_alloc(session, 0, &key_tmp));
	WT_ERR(__wt_scr_alloc(session, 0, &value_tmp));
	WT_CLEAR(key_local);
	WT_CLEAR(value_local);

	WT_ROW_INDX_FOREACH(page, rip, i) {
		/* Check for deletion. */
		upd = WT_ROW_UPDATE(page, rip);
		if (upd != NULL && WT_UPDATE_DELETED_ISSET(upd))
			continue;

		/*
		 * The key and value variables reference the items we will
		 * print.  Set the key.
		 */
		if (__wt_key_process(rip)) {
			WT_ERR(__wt_cell_process(session, rip->key, key_tmp));
			key = &key_tmp->item;
		} else
			key = (WT_ITEM *)rip;

		/*
		 * If the item was ever updated, dump the data from the
		 * update entry.
		 */
		if (upd != NULL) {
			dp->p(key->data, key->size, dp->stream);
			dp->p(WT_UPDATE_DATA(upd), upd->size, dp->stream);
			continue;
		}

		/* Set value to reference the value we'll dump. */
		cell = rip->value;
		switch (WT_CELL_TYPE(cell)) {
		case WT_CELL_DATA:
			if (huffman == NULL) {
				value_local.data = WT_CELL_BYTE(cell);
				value_local.size = WT_CELL_LEN(cell);
				value = &value_local;
				break;
			}
			/* FALLTHROUGH */
		case WT_CELL_DATA_OVFL:
			WT_ERR(__wt_cell_process(session, cell, value_tmp));
			value = &value_tmp->item;
			break;
		WT_ILLEGAL_FORMAT_ERR(btree, ret);
		}

		dp->p(key->data, key->size, dp->stream);
		dp->p(value->data, value->size, dp->stream);
	}

err:	/* Discard any space allocated to hold off-page key/value items. */
	if (key_tmp != NULL)
		__wt_scr_release(&key_tmp);
	if (value_tmp != NULL)
		__wt_scr_release(&value_tmp);

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
__wt_print_byte_string_nl(const uint8_t *data, uint32_t size, FILE *stream)
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
__wt_print_byte_string(const uint8_t *data, uint32_t size, FILE *stream)
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
__wt_print_byte_string_hex(const uint8_t *data, uint32_t size, FILE *stream)
{
	for (; size > 0; --size, ++data)
		fprintf(stream, "%x%x",
		    hex[(data[0] & 0xf0) >> 4], hex[data[0] & 0x0f]);
	fprintf(stream, "\n");
}
