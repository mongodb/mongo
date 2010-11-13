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
	    (uint8_t *, uint32_t, FILE *);
	FILE *stream;				/* Dump stream */

	void (*f)(const char *, uint64_t);	/* Progress callback */
	uint64_t fcnt;				/* Progress counter */

	DBT *dupkey;				/* Offpage duplicate tree key */
} WT_DSTUFF;

static int  __wt_bt_dump_page(WT_TOC *, WT_PAGE *, void *);
static void __wt_bt_dump_page_col_fix(WT_TOC *, WT_PAGE *, WT_DSTUFF *);
static int  __wt_bt_dump_page_col_rcc(WT_TOC *, WT_PAGE *, WT_DSTUFF *);
static int  __wt_bt_dump_page_col_var(WT_TOC *, WT_PAGE *, WT_DSTUFF *);
static int  __wt_bt_dump_page_dup_leaf(WT_TOC *, WT_PAGE *, WT_DSTUFF *);
static int  __wt_bt_dump_page_row_leaf(WT_TOC *, WT_PAGE *, WT_DSTUFF *);
static void __wt_bt_hexprint(uint8_t *, uint32_t, FILE *);
static void __wt_bt_print_nl(uint8_t *, uint32_t, FILE *);

/*
 * __wt_db_dump --
 *	Db.dump method.
 */
int
__wt_db_dump(WT_TOC *toc,
    FILE *stream, void (*f)(const char *, uint64_t), uint32_t flags)
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
		__wt_bt_dump_page_col_fix(toc, page, dp);
		break;
	case WT_PAGE_COL_RCC:
		WT_RET(__wt_bt_dump_page_col_rcc(toc, page, dp));
		break;
	case WT_PAGE_COL_VAR:
		WT_RET(__wt_bt_dump_page_col_var(toc, page, dp));
		break;
	case WT_PAGE_DUP_LEAF:
		WT_RET(__wt_bt_dump_page_dup_leaf(toc, page, dp));
		break;
	case WT_PAGE_ROW_LEAF:
		WT_RET(__wt_bt_dump_page_row_leaf(toc, page, dp));
		break;
	WT_ILLEGAL_FORMAT(db);
	}

	/* Report progress every 10 pages. */
	if (dp->f != NULL && ++dp->fcnt % 10 == 0)
		dp->f(toc->name, dp->fcnt);

	return (0);
}

/*
 * __wt_bt_dump_page_col_fix --
 *	Dump a WT_PAGE_COL_FIX page.
 */
static void
__wt_bt_dump_page_col_fix(WT_TOC *toc, WT_PAGE *page, WT_DSTUFF *dp)
{
	DB *db;
	WT_COL *cip;
	WT_REPL *repl;
	uint32_t i;

	db = toc->db;

	/* Walk the page, dumping data items. */
	WT_INDX_FOREACH(page, cip, i) {
		if ((repl = WT_COL_REPL(page, cip)) == NULL) {
			if (!WT_FIX_DELETE_ISSET(cip->data))
				dp->p(cip->data, db->fixed_len, dp->stream);
		} else
			if (!WT_REPL_DELETED_ISSET(repl))
				dp->p(WT_REPL_DATA(repl),
				    db->fixed_len, dp->stream);
	}
}

/*
 * __wt_bt_dump_page_col_rcc --
 *	Dump a WT_PAGE_COL_RCC page.
 */
static int
__wt_bt_dump_page_col_rcc(WT_TOC *toc, WT_PAGE *page, WT_DSTUFF *dp)
{
	DB *db;
	ENV *env;
	WT_COL *cip;
	WT_COL_EXPAND *exp, **expsort, **expp;
	WT_REPL *repl;
	uint64_t recno;
	uint32_t i, n_expsort;
	uint16_t n_repeat;

	db = toc->db;
	env = toc->env;
	expsort = NULL;
	n_expsort = 0;

	recno = page->hdr->start_recno;
	WT_INDX_FOREACH(page, cip, i) {
		/*
		 * Get a sorted list of any expansion entries we've created for
		 * this set of records.  The sort function returns a NULL-
		 * terminated array of references to WT_COL_EXPAND structures,
		 * sorted by record number.
		 */
		WT_RET(__wt_bt_rcc_expand_sort(
		    env, page, cip, &expsort, &n_expsort));

		/*
		 * Dump the records.   We use the WT_REPL entry for records in
		 * in the WT_COL_EXPAND array, and original data otherwise.
		 */
		for (expp = expsort,
		    n_repeat = WT_RCC_REPEAT_COUNT(cip->data);
		    n_repeat > 0; --n_repeat, ++recno)
			if ((exp = *expp) != NULL && exp->recno == recno) {
				++expp;
				repl = exp->repl;
				if (WT_REPL_DELETED_ISSET(repl))
					continue;
				dp->p(
				    WT_REPL_DATA(repl), repl->size, dp->stream);
			} else
				dp->p(WT_RCC_REPEAT_DATA(cip->data),
				    db->fixed_len, dp->stream);
	}
	/* Free the sort array. */
	if (expsort != NULL)
		__wt_free(env, expsort, n_expsort * sizeof(WT_COL_EXPAND *));

	return (0);
}

/*
 * __wt_bt_dump_page_col_var --
 *	Dump a WT_PAGE_COL_VAR page.
 */
static int
__wt_bt_dump_page_col_var(WT_TOC *toc, WT_PAGE *page, WT_DSTUFF *dp)
{
	DB *db;
	DBT *tmp;
	WT_COL *cip;
	WT_ITEM *item;
	WT_PAGE *ovfl;
	WT_REPL *repl;
	uint32_t i;
	int ret;
	void *huffman;

	db = toc->db;
	huffman = db->idb->huffman_data;
	ret = 0;

	WT_ERR(__wt_scr_alloc(toc, &tmp));
	WT_INDX_FOREACH(page, cip, i) {
		/* Check for replace or deletion. */
		if ((repl = WT_COL_REPL(page, cip)) != NULL) {
			if (!WT_REPL_DELETED_ISSET(repl))
				dp->p(
				    WT_REPL_DATA(repl), repl->size, dp->stream);
			continue;
		}

		/* Process the original data. */
		item = cip->data;
		switch (WT_ITEM_TYPE(item)) {
		case WT_ITEM_DATA:
			if (huffman == NULL) {
				dp->p(WT_ITEM_BYTE(item),
				    WT_ITEM_LEN(item), dp->stream);
				break;
			}
			/* FALLTHROUGH */
		case WT_ITEM_DATA_OVFL:
			WT_ERR(__wt_bt_item_process(toc, item, &ovfl, tmp));
			if (ovfl == NULL)
				dp->p(tmp->data, tmp->size, dp->stream);
			else {
				dp->p(WT_PAGE_BYTE(ovfl),
				    ovfl->hdr->u.datalen, dp->stream);
				__wt_bt_page_out(toc, &ovfl, 0);
			}
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
 * __wt_bt_dump_page_dup_leaf --
 *	Dump a WT_PAGE_DUP_LEAF page.
 */
static int
__wt_bt_dump_page_dup_leaf(WT_TOC *toc, WT_PAGE *page, WT_DSTUFF *dp)
{
	DB *db;
	DBT *dupkey, *tmp;
	WT_ITEM *item;
	WT_PAGE *ovfl;
	WT_REPL *repl;
	WT_ROW *rip;
	uint32_t i;
	int ret;
	void *huffman;

	db = toc->db;
	dupkey = dp->dupkey;
	huffman = db->idb->huffman_data;
	ret = 0;

	WT_ERR(__wt_scr_alloc(toc, &tmp));
	WT_INDX_FOREACH(page, rip, i) {
		/* Check for deletion. */
		if ((repl = WT_ROW_REPL(
		    page, rip)) != NULL && WT_REPL_DELETED_ISSET(repl))
			continue;

		/* Output the key, we're going to need it. */
		dp->p(dupkey->data, dupkey->size, dp->stream);

		/* Output the replacement item. */
		if (repl != NULL) {
			dp->p(WT_REPL_DATA(repl), repl->size, dp->stream);
			continue;
		}

		/* Process the original data. */
		item = rip->data;
		switch (WT_ITEM_TYPE(item)) {
		case WT_ITEM_DATA_DUP:
			if (huffman == NULL) {
				dp->p(WT_ITEM_BYTE(item),
				    WT_ITEM_LEN(item), dp->stream);
				break;
			}
			/* FALLTHROUGH */
		case WT_ITEM_DATA_DUP_OVFL:
			WT_ERR(__wt_bt_item_process(toc, item, &ovfl, tmp));
			if (ovfl == NULL)
				dp->p(tmp->data, tmp->size, dp->stream);
			else {
				dp->p(WT_PAGE_BYTE(ovfl),
				    ovfl->hdr->u.datalen, dp->stream);
				__wt_bt_page_out(toc, &ovfl, 0);
			}
			break;
		WT_ILLEGAL_FORMAT_ERR(db, ret);
		}
	}

err:	__wt_scr_release(&tmp);
	return (ret);
}

/*
 * __wt_bt_dump_page_row_leaf --
 *	Dump a WT_PAGE_ROW_LEAF page.
 */
static int
__wt_bt_dump_page_row_leaf(WT_TOC *toc, WT_PAGE *page, WT_DSTUFF *dp)
{
	DB *db;
	DBT *key, *data, *key_tmp, *data_tmp, key_local, data_local;
	WT_PAGE *key_ovfl, *data_ovfl;
	WT_ITEM *item;
	WT_REPL *repl;
	WT_ROW *rip;
	uint32_t i;
	int ret;
	void *huffman;

	db = toc->db;
	key = data = key_tmp = data_tmp = NULL;
	key_ovfl = data_ovfl = NULL;
	huffman = db->idb->huffman_data;
	ret = 0;

	WT_ERR(__wt_scr_alloc(toc, &key_tmp));
	WT_ERR(__wt_scr_alloc(toc, &data_tmp));
	WT_CLEAR(key_local);
	WT_CLEAR(data_local);

	WT_INDX_FOREACH(page, rip, i) {
		/* Check for deletion. */
		if ((repl = WT_ROW_REPL(
		    page, rip)) != NULL && WT_REPL_DELETED_ISSET(repl))
			continue;

		/*
		 * The key and data variables reference the DBT's we'll print.
		 * Set the key.
		 */
		if (WT_KEY_PROCESS(rip)) {
			/* Discard any previously held key overflow page. */
			if (key_ovfl != NULL)
				__wt_bt_page_out(toc, &key_ovfl, 0);
			WT_ERR(__wt_bt_item_process(
			    toc, rip->key, &key_ovfl, key_tmp));
			if (key_ovfl == NULL)
				key = key_tmp;
			else {
				key_local.data = WT_PAGE_BYTE(key_ovfl);
				key_local.size = key_ovfl->hdr->u.datalen;
				key = &key_local;
			}
		} else
			key = (DBT *)rip;

		/*
		 * If the item was ever replaced, we're done: it can't be an
		 * off-page tree, and we don't care what kind of item it was
		 * originally.  Dump the data from the replacement entry.
		 */
		if (repl != NULL) {
			dp->p(key->data, key->size, dp->stream);
			dp->p(WT_REPL_DATA(repl), repl->size, dp->stream);
			continue;
		}

		/* Set data to reference the data we'll dump. */
		item = rip->data;
		switch (WT_ITEM_TYPE(item)) {
		case WT_ITEM_DATA:
		case WT_ITEM_DATA_DUP:
			if (huffman == NULL) {
				data_local.data = WT_ITEM_BYTE(item);
				data_local.size = WT_ITEM_LEN(item);
				data = &data_local;
				break;
			}
			/* FALLTHROUGH */
		case WT_ITEM_DATA_DUP_OVFL:
		case WT_ITEM_DATA_OVFL:
			/* Discard any previously held data overflow page. */
			if (data_ovfl != NULL)
				__wt_bt_page_out(toc, &data_ovfl, 0);
			WT_ERR(__wt_bt_item_process(
			    toc, item, &data_ovfl, data_tmp));
			if (data_ovfl == NULL)
				data = data_tmp;
			else {
				data_local.data = WT_PAGE_BYTE(data_ovfl);
				data_local.size = data_ovfl->hdr->u.datalen;
				data = &data_local;
			}
			break;
		case WT_ITEM_OFF:
			dp->dupkey = key;
			WT_RET_RESTART(__wt_bt_tree_walk(toc,
			    WT_ROW_OFF_ADDR(rip), WT_ROW_OFF_SIZE(rip),
			    __wt_bt_dump_page, dp));
			continue;
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

	/* Discard any overflow pages we're still holding. */
	if (key_ovfl != NULL)
		__wt_bt_page_out(toc, &key_ovfl, 0);
	if (data_ovfl != NULL)
		__wt_bt_page_out(toc, &data_ovfl, 0);

	return (ret);
}

static const char hex[] = "0123456789abcdef";

/*
 * __wt_bt_print_nl --
 *	Output a single key/data entry in printable characters, where possible.
 *	In addition, terminate with a <newline> character, unless the entry is
 *	itself terminated with a <newline> character.
 */
static void
__wt_bt_print_nl(uint8_t *data, uint32_t size, FILE *stream)
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
__wt_bt_print(uint8_t *data, uint32_t size, FILE *stream)
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
__wt_bt_hexprint(uint8_t *data, uint32_t size, FILE *stream)
{
	for (; size > 0; --size, ++data)
		fprintf(stream, "%x%x",
		    hex[(data[0] & 0xf0) >> 4], hex[data[0] & 0x0f]);
	fprintf(stream, "\n");
}
