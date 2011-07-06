/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"
#include "btree.i"
#include "cell.i"

typedef struct {
	void (*p)(const uint8_t *, uint32_t, FILE *);	/* Print function */
	FILE *stream;					/* Dump stream */
	uint64_t fcnt;					/* Progress counter */
} WT_DSTUFF;

static int  __wt_dump_page(WT_SESSION_IMPL *, WT_PAGE *, void *);
static void __wt_dump_page_col_fix(WT_SESSION_IMPL *, WT_PAGE *, WT_DSTUFF *);
static int  __wt_dump_page_col_rle(WT_SESSION_IMPL *, WT_PAGE *, WT_DSTUFF *);
static int  __wt_dump_page_col_var(WT_SESSION_IMPL *, WT_PAGE *, WT_DSTUFF *);
static int  __wt_dump_page_row_leaf(WT_SESSION_IMPL *, WT_PAGE *, WT_DSTUFF *);
static void __wt_stream_byte_string_hex(const uint8_t *, uint32_t, FILE *);
static void __wt_stream_byte_string_nl(const uint8_t *, uint32_t, FILE *);

/*
 * __wt_btree_dump --
 *	Db.dump method.
 */
int
__wt_btree_dump(WT_SESSION_IMPL *session, FILE *stream, uint32_t flags)
{
	WT_DSTUFF dstuff;
	int ret;

	dstuff.p = flags == WT_DUMP_PRINT ?
	    __wt_stream_byte_string_nl : __wt_stream_byte_string_hex;
	dstuff.stream = stream;
	dstuff.fcnt = 0;

	/*
	 * Note we do not have a hazard reference for the root page, and that's
	 * safe -- root pages are pinned into memory when a file is opened, and
	 * never re-written until the file is closed.
	 */
	fprintf(stream, "VERSION=1\n");
	fprintf(stream, "HEADER=END\n");
	ret = __wt_tree_walk(session, NULL, __wt_dump_page, &dstuff);
	fprintf(stream, "DATA=END\n");

	/* Wrap up reporting. */
	__wt_progress(session, NULL, dstuff.fcnt);

	return (ret);
}

/*
 * __wt_dump_page --
 *	Depth-first recursive walk of a btree.
 */
static int
__wt_dump_page(WT_SESSION_IMPL *session, WT_PAGE *page, void *arg)
{
	WT_DSTUFF *dp;

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
	WT_ILLEGAL_FORMAT(session);
	}

	/* Report progress every 10 pages. */
	if (++dp->fcnt % 10 == 0)
		__wt_progress(session, NULL, dp->fcnt);

	return (0);
}

/*
 * __wt_dump_page_col_fix --
 *	Dump a WT_PAGE_COL_FIX page.
 */
static void
__wt_dump_page_col_fix(WT_SESSION_IMPL *session, WT_PAGE *page, WT_DSTUFF *dp)
{
	WT_BTREE *btree;
	WT_COL *cip;
	WT_UPDATE *upd;
	uint32_t i;
	void *cipdata;

	btree = session->btree;

	/* Walk the page, dumping data items. */
	WT_COL_FOREACH(page, cip, i) {
		cipdata = WT_COL_PTR(page, cip);
		if ((upd = WT_COL_UPDATE(page, cip)) == NULL) {
			if (cipdata != NULL && !WT_FIX_DELETE_ISSET(cipdata))
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
__wt_dump_page_col_rle(WT_SESSION_IMPL *session, WT_PAGE *page, WT_DSTUFF *dp)
{
	WT_BTREE *btree;
	FILE *fp;
	WT_COL *cip;
	WT_INSERT *ins;
	WT_UPDATE *upd;
	uint64_t recno;
	uint32_t i;
	uint16_t n_repeat;
	void *cipdata;
	int ret;

	btree = session->btree;
	fp = dp->stream;
	ret = 0;

	recno = page->u.col_leaf.recno;
	WT_COL_FOREACH(page, cip, i) {
		cipdata = WT_COL_PTR(page, cip);
		/*
		 * Dump the records.   We use the WT_UPDATE entry for records in
		 * in the WT_INSERT array, and original data otherwise.
		 */
		for (ins = WT_COL_INSERT(page, cip),
		    n_repeat = WT_RLE_REPEAT_COUNT(cipdata);
		    n_repeat > 0; --n_repeat, ++recno)
			if (ins != NULL && WT_INSERT_RECNO(ins) == recno) {
				upd = ins->upd;
				if (!WT_UPDATE_DELETED_ISSET(upd))
					dp->p(
					    WT_UPDATE_DATA(upd), upd->size, fp);
				ins = ins->next;
			} else
				if (cipdata != NULL &&
				    !WT_FIX_DELETE_ISSET(
				    WT_RLE_REPEAT_DATA(cipdata)))
					dp->p(WT_RLE_REPEAT_DATA(
					    cipdata), btree->fixed_len, fp);
	}

	return (ret);
}

/*
 * __wt_dump_page_col_var --
 *	Dump a WT_PAGE_COL_VAR page.
 */
static int
__wt_dump_page_col_var(WT_SESSION_IMPL *session, WT_PAGE *page, WT_DSTUFF *dp)
{
	WT_BTREE *btree;
	WT_BUF *tmp;
	WT_COL *cip;
	WT_CELL *cell;
	WT_UPDATE *upd;
	int ret;
	uint32_t i;
	void *huffman;

	btree = session->btree;
	huffman = btree->huffman_value;
	ret = 0;

	WT_RET(__wt_scr_alloc(session, 0, &tmp));
	WT_COL_FOREACH(page, cip, i) {
		/* Check for update. */
		if ((upd = WT_COL_UPDATE(page, cip)) != NULL) {
			if (!WT_UPDATE_DELETED_ISSET(upd))
				dp->p(
				    WT_UPDATE_DATA(upd), upd->size, dp->stream);
			continue;
		}

		/* Check for deleted entry. */
		if ((cell = WT_COL_PTR(page, cip)) == NULL)
			continue;

		/* Process the original data. */
		switch (__wt_cell_type(cell)) {
		case WT_CELL_DATA:
			if (huffman == NULL) {
				dp->p(__wt_cell_data(cell),
				    __wt_cell_datalen(cell), dp->stream);
				break;
			}
			/* FALLTHROUGH */
		case WT_CELL_DATA_OVFL:
			WT_ERR(__wt_cell_copy(session, cell, tmp));
			dp->p(tmp->data, tmp->size, dp->stream);
			break;
		case WT_CELL_DEL:
			break;
		WT_ILLEGAL_FORMAT_ERR(session, ret);
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
__wt_dump_page_row_leaf(WT_SESSION_IMPL *session, WT_PAGE *page, WT_DSTUFF *dp)
{
	WT_BTREE *btree;
	WT_BUF *key_tmp, *value_tmp;
	WT_CELL *cell;
	WT_INSERT *ins;
	WT_ITEM *key, _key, *value, _value;
	WT_ROW *rip;
	WT_UPDATE *upd;
	uint32_t i;
	int ret;
	void *huffman;

	btree = session->btree;
	key = value = NULL;
	key_tmp = value_tmp = NULL;
	huffman = btree->huffman_value;
	ret = 0;

	WT_ERR(__wt_scr_alloc(session, 0, &key_tmp));
	WT_ERR(__wt_scr_alloc(session, 0, &value_tmp));
	WT_CLEAR(_value);

	/*
	 * Dump any K/V pairs inserted into the page before the first from-disk
	 * key on the page.
	 */
	for (ins = WT_ROW_INSERT_SMALLEST(page); ins != NULL; ins = ins->next) {
		upd = ins->upd;
		if (WT_UPDATE_DELETED_ISSET(upd))
			continue;
		dp->p(WT_INSERT_KEY(ins), WT_INSERT_KEY_SIZE(ins), dp->stream);
		dp->p(WT_UPDATE_DATA(upd), upd->size, dp->stream);
	}

	/* Dump the page's K/V pairs. */
	WT_ROW_FOREACH(page, rip, i) {
		/* Check for deletion. */
		upd = WT_ROW_UPDATE(page, rip);
		if (upd != NULL && WT_UPDATE_DELETED_ISSET(upd))
			goto dump_insert;

		/*
		 * The key and value variables reference the items we will
		 * print.  Set the key.
		 */
		if (__wt_off_page(page, rip->key)) {
			_key.data = WT_IKEY_DATA(rip->key);
			_key.size = ((WT_IKEY *)rip->key)->size;
			key = &_key;
		} else {
			WT_ERR(__wt_row_key(session, page, rip, key_tmp));
			key = (WT_ITEM *)key_tmp;
		}

		/*
		 * If the item was ever updated, dump the data from the
		 * update entry.
		 */
		if (upd != NULL) {
			dp->p(key->data, key->size, dp->stream);
			dp->p(WT_UPDATE_DATA(upd), upd->size, dp->stream);
			goto dump_insert;
		}

		/* Check for an empty item. */
		if ((cell = __wt_row_value(page, rip)) == NULL) {
			dp->p(key->data, key->size, dp->stream);
			dp->p(NULL, 0, dp->stream);
			goto dump_insert;
		}

		/* Set cell to reference the value we'll dump. */
		switch (__wt_cell_type(cell)) {
		case WT_CELL_DATA:
			if (huffman == NULL) {
				value = &_value;
				__wt_cell_data_and_len(
				    cell, &value->data, &value->size);
				break;
			}
			/* FALLTHROUGH */
		case WT_CELL_DATA_OVFL:
			WT_ERR(__wt_cell_copy(session, cell, value_tmp));
			value = (WT_ITEM *)value_tmp;
			break;
		WT_ILLEGAL_FORMAT_ERR(session, ret);
		}

		dp->p(key->data, key->size, dp->stream);
		dp->p(value->data, value->size, dp->stream);

dump_insert:	/* Dump inserted K/V pairs. */
		for (ins =
		    WT_ROW_INSERT(page, rip); ins != NULL; ins = ins->next) {
			upd = ins->upd;
			if (WT_UPDATE_DELETED_ISSET(upd))
				continue;
			dp->p(WT_INSERT_KEY(ins),
			    WT_INSERT_KEY_SIZE(ins), dp->stream);
			dp->p(WT_UPDATE_DATA(upd), upd->size, dp->stream);
		}
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
 * __wt_stream_byte_string_nl --
 *	Output a single byte stringin printable characters, where possible.
 *	In addition, terminate with a <newline> character, unless the entry
 *	is itself terminated with a <newline> character.
 */
static void
__wt_stream_byte_string_nl(const uint8_t *data, uint32_t size, FILE *stream)
{
	if (size > 0 && data[size - 1] == '\n')
		--size;
	__wt_stream_byte_string(data, size, stream);
	fprintf(stream, "\n");
}

/*
 * __wt_stream_byte_string --
 *	Output a single byte string in printable characters, where possible,
 * to the specified stream.
 */
void
__wt_stream_byte_string(const uint8_t *data, uint32_t size, FILE *stream)
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
 * __wt_load_byte_string --
 *	Load a single byte string into a buffer, in printable characters,
 * where possible.
 */
int
__wt_load_byte_string(
    WT_SESSION_IMPL *session, const uint8_t *data, uint32_t size, WT_BUF *buf)
{
	uint32_t avail;
	int ch, len;
	char *p;

	/*
	 * The maximum size is the byte-string length, all hex characters, plus
	 * a trailing nul byte.  Throw in a few extra bytes for fun.
	 *
	 * The underlying functions use type int, not uint32_t, check we're
	 * not in trouble, just out of sheer, raving paranoia.
	 */
	if ((uint64_t)size * 4 + 20 >= UINT32_MAX)
		return (ENOMEM);
	avail = size * 4 + 20;
	WT_RET(__wt_buf_init(session, buf, avail));

	for (p = buf->mem; size > 0; --size, ++data) {
		ch = data[0];
		if (isprint(ch))
			len = snprintf(p, avail, "%c", ch);
		else
			len = snprintf(p, avail, "%x%x",
			    hex[(data[0] & 0xf0) >> 4], hex[data[0] & 0x0f]);
		/*
		 * Be paranoid about buffer overflow: even if our calculation
		 * is off, or snprintf(3) returns large length values, don't
		 * overflow the end of the buffer.
		 */
		if ((u_int)len >= avail)
			break;
		p += len;
		buf->size += (u_int)len;
		avail -= (u_int)len;
	}
	return (0);
}

/*
 * __wt_stream_byte_string_hex --
 *	Output a single byte string in hexadecimal characters.
 */
static void
__wt_stream_byte_string_hex(const uint8_t *data, uint32_t size, FILE *stream)
{
	for (; size > 0; --size, ++data)
		fprintf(stream, "%x%x",
		    hex[(data[0] & 0xf0) >> 4], hex[data[0] & 0x0f]);
	fprintf(stream, "\n");
}
