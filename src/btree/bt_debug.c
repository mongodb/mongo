/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

#ifdef HAVE_DIAGNOSTIC
static void __wt_debug_dsk_col_fix(DB *, WT_PAGE_DISK *, FILE *);
static void __wt_debug_dsk_col_int(WT_PAGE_DISK *, FILE *);
static void __wt_debug_dsk_col_rle(DB *, WT_PAGE_DISK *, FILE *);
static int  __wt_debug_dsk_item(WT_TOC *, WT_PAGE_DISK *, FILE *);
static void __wt_debug_page_col_fix(WT_TOC *, WT_PAGE *, FILE *);
static void __wt_debug_page_col_int(WT_PAGE *, FILE *);
static void __wt_debug_page_col_rle(WT_TOC *, WT_PAGE *, FILE *);
static int  __wt_debug_page_col_var(WT_TOC *, WT_PAGE *, FILE *);
static void __wt_debug_page_row_int(WT_PAGE *, FILE *);
static int  __wt_debug_page_row_leaf(WT_TOC *, WT_PAGE *, FILE *);
static int  __wt_debug_item(WT_TOC *, WT_ITEM *, FILE *);
static int  __wt_debug_item_data(WT_TOC *, WT_ITEM *, FILE *fp);
static void __wt_debug_off(WT_OFF *, const char *, FILE *);
static void __wt_debug_pair(const char *, void *, uint32_t, FILE *);
static void __wt_debug_repl(WT_REPL *, FILE *);
static void __wt_debug_rleexp(WT_RLE_EXPAND *, FILE *);
static int  __wt_debug_set_fp(const char *, FILE **, int *);

static int
__wt_debug_set_fp(const char *ofile, FILE **fpp, int *close_varp)
{
	FILE *fp;

	*close_varp = 0;

	/* If we were giving a stream, use it. */
	if ((fp = *fpp) != NULL)
		return (0);

	/* If we were given a file, use it. */
	if (ofile != NULL) {
		if ((fp = fopen(ofile, "w")) == NULL)
			return (WT_ERROR);
		*fpp = fp;
		*close_varp = 1;
		return (0);
	}

	/* Default to stdout. */
	*fpp = stdout;
	return (0);
}

/*
 * __wt_debug_dump --
 *	Dump a database in debugging mode.
 */
int
__wt_debug_dump(WT_TOC *toc, char *ofile, FILE *fp)
{
	int do_close, ret;

	WT_RET(__wt_debug_set_fp(ofile, &fp, &do_close));

	/*
	 * We use the verification code to do debugging dumps because if we're
	 * dumping in debugging mode, we want to confirm the page is OK before
	 * walking it.
	 */
	ret = __wt_verify(toc, NULL, fp);

	if (do_close)
		(void)fclose(fp);

	return (ret);
}

/*
 * __wt_debug_disk --
 *	Dump a disk page in debugging mode.
 */
int
__wt_debug_disk(WT_TOC *toc, WT_PAGE_DISK *dsk, char *ofile, FILE *fp)
{
	DB *db;
	int do_close, ret;

	db = toc->db;
	ret = 0;

	WT_RET(__wt_debug_set_fp(ofile, &fp, &do_close));

	switch (dsk->type) {
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_INT:
	case WT_PAGE_COL_RLE:
	case WT_PAGE_COL_VAR:
		fprintf(fp,
		    "%s page: starting recno %llu, level %lu, entries %lu, "
		    "lsn %lu/%lu\n",
		    __wt_page_type_string(dsk),
		    (unsigned long long)dsk->start_recno,
		    (u_long)dsk->level, (u_long)dsk->u.entries,
		    (u_long)dsk->lsn_file, (u_long)dsk->lsn_off);
		break;
	case WT_PAGE_DUP_INT:
	case WT_PAGE_DUP_LEAF:
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		fprintf(fp,
		    "%s page: level %lu, entries %lu, lsn %lu/%lu\n",
		    __wt_page_type_string(dsk),
		    (u_long)dsk->level, (u_long)dsk->u.entries,
		    (u_long)dsk->lsn_file, (u_long)dsk->lsn_off);
		break;
	case WT_PAGE_OVFL:
		fprintf(fp, "%s page: data size %lu, lsn %lu/%lu\n",
		    __wt_page_type_string(dsk), (u_long)dsk->u.datalen,
		    (u_long)dsk->lsn_file, (u_long)dsk->lsn_off);
		break;
	WT_ILLEGAL_FORMAT(db);
	}

	switch (dsk->type) {
	case WT_PAGE_COL_VAR:
	case WT_PAGE_DUP_INT:
	case WT_PAGE_DUP_LEAF:
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		ret = __wt_debug_dsk_item(toc, dsk, fp);
		break;
	case WT_PAGE_COL_FIX:
		__wt_debug_dsk_col_fix(db, dsk, fp);
		break;
	case WT_PAGE_COL_RLE:
		__wt_debug_dsk_col_rle(db, dsk, fp);
		break;
	case WT_PAGE_COL_INT:
		__wt_debug_dsk_col_int(dsk, fp);
		break;
	default:
		break;
	}

	if (do_close)
		(void)fclose(fp);

	return (ret);
}

/*
 * __wt_debug_page --
 *	Dump the in-memory information for a page.
 */
int
__wt_debug_page(WT_TOC *toc, WT_PAGE *page, char *ofile, FILE *fp)
{
	DB *db;
	int do_close;

	db = toc->db;

	WT_RET(__wt_debug_set_fp(ofile, &fp, &do_close));

	fprintf(fp, "addr: %lu-%lu {\n\t%s: size %lu",
	    (u_long)page->addr,
	    (u_long)page->addr + (WT_OFF_TO_ADDR(db, page->size) - 1),
	    __wt_page_type_string(page->dsk), (u_long)page->size);
	switch (page->dsk->type) {
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_INT:
	case WT_PAGE_COL_RLE:
	case WT_PAGE_COL_VAR:
		fprintf(stderr,
		    ", records %llu", (unsigned long long)page->records);
		break;
	default:
		break;
	}
	fprintf(stderr, "\n");


	/* Dump the WT_{ROW,COL}_INDX array. */
	switch (page->dsk->type) {
	case WT_PAGE_COL_FIX:
		__wt_debug_page_col_fix(toc, page, fp);
		break;
	case WT_PAGE_COL_INT:
		__wt_debug_page_col_int(page, fp);
		break;
	case WT_PAGE_COL_RLE:
		__wt_debug_page_col_rle(toc, page, fp);
		break;
	case WT_PAGE_COL_VAR:
		WT_RET(__wt_debug_page_col_var(toc, page, fp));
		break;
	case WT_PAGE_DUP_LEAF:
	case WT_PAGE_ROW_LEAF:
		WT_RET(__wt_debug_page_row_leaf(toc, page, fp));
		break;
	case WT_PAGE_DUP_INT:
	case WT_PAGE_ROW_INT:
		__wt_debug_page_row_int(page, fp);
		break;
	case WT_PAGE_OVFL:
		break;
	WT_ILLEGAL_FORMAT(db);
	}

	fprintf(fp, "}\n");

	if (do_close)
		(void)fclose(fp);

	return (0);
}

/*
 * __wt_debug_page_col_fix --
 *	Dump an in-memory WT_PAGE_COL_FIX page.
 */
static void
__wt_debug_page_col_fix(WT_TOC *toc, WT_PAGE *page, FILE *fp)
{
	WT_COL *cip;
	WT_REPL *repl;
	uint32_t fixed_len, i;

	fixed_len = toc->db->fixed_len;

	if (fp == NULL)				/* Default to stderr */
		fp = stderr;

	WT_INDX_FOREACH(page, cip, i) {
		fprintf(fp, "\tdata {");
		if (WT_FIX_DELETE_ISSET(cip->data))
			fprintf(fp, "deleted");
		else
			__wt_print_byte_string(cip->data, fixed_len, fp);
		fprintf(fp, "}\n");

		if ((repl = WT_COL_REPL(page, cip)) != NULL)
			__wt_debug_repl(repl, fp);
	}
}

/*
 * __wt_debug_page_col_int --
 *	Dump an in-memory WT_PAGE_COL_INT page.
 */
static void
__wt_debug_page_col_int(WT_PAGE *page, FILE *fp)
{
	WT_COL *cip;
	uint32_t i;

	if (fp == NULL)				/* Default to stderr */
		fp = stderr;

	WT_INDX_FOREACH(page, cip, i)
		__wt_debug_off(cip->data, "\t", fp);
}

/*
 * __wt_debug_page_col_rle --
 *	Dump an in-memory WT_PAGE_COL_RLE page.
 */
static void
__wt_debug_page_col_rle(WT_TOC *toc, WT_PAGE *page, FILE *fp)
{
	WT_COL *cip;
	WT_RLE_EXPAND *exp;
	uint32_t fixed_len, i;

	fixed_len = toc->db->fixed_len;

	if (fp == NULL)				/* Default to stderr */
		fp = stderr;

	WT_INDX_FOREACH(page, cip, i) {
		fprintf(fp,
		    "\trepeat %lu {", (u_long)WT_RLE_REPEAT_COUNT(cip->data));
		if (WT_FIX_DELETE_ISSET(WT_RLE_REPEAT_DATA(cip->data)))
			fprintf(fp, "deleted");
		else
			__wt_print_byte_string(
			    WT_RLE_REPEAT_DATA(cip->data), fixed_len, fp);
		fprintf(fp, "}\n");

		if ((exp = WT_COL_RLEEXP(page, cip)) != NULL)
			__wt_debug_rleexp(exp, fp);
	}
}

/*
 * __wt_debug_page_col_var --
 *	Dump an in-memory WT_PAGE_COL_VAR page.
 */
static int
__wt_debug_page_col_var(WT_TOC *toc, WT_PAGE *page, FILE *fp)
{
	WT_COL *cip;
	WT_REPL *repl;
	uint32_t i;

	if (fp == NULL)				/* Default to stderr */
		fp = stderr;

	WT_INDX_FOREACH(page, cip, i) {
		fprintf(fp, "\tdata {");
		WT_RET(__wt_debug_item_data(toc, cip->data, fp));
		fprintf(fp, "}\n");

		if ((repl = WT_COL_REPL(page, cip)) != NULL)
			__wt_debug_repl(repl, fp);
	}
	return (0);
}

/*
 * __wt_debug_page_row_leaf --
 *	Dump an in-memory WT_PAGE_DUP_LEAF or WT_PAGE_ROW_LEAF page.
 */
static int
__wt_debug_page_row_leaf(WT_TOC *toc, WT_PAGE *page, FILE *fp)
{
	WT_REPL *repl;
	WT_ROW *rip;
	uint32_t i;

	if (fp == NULL)				/* Default to stderr */
		fp = stderr;

	WT_INDX_FOREACH(page, rip, i) {
		if (__wt_key_process(rip))
			fprintf(fp, "\tkey: {requires processing}\n");
		else
			__wt_debug_dbt("\tkey", rip, fp);

		fprintf(fp, "\tdata: {");
		WT_RET(__wt_debug_item_data(toc, rip->data, fp));
		fprintf(fp, "}\n");

		if ((repl = WT_ROW_REPL(page, rip)) != NULL)
			__wt_debug_repl(repl, fp);
	}

	return (0);
}

/*
 * __wt_debug_page_row_int --
 *	Dump an in-memory WT_PAGE_DUP_INT or WT_PAGE_ROW_INT page.
 */
static void
__wt_debug_page_row_int(WT_PAGE *page, FILE *fp)
{
	WT_ROW *rip;
	uint32_t i;

	if (fp == NULL)				/* Default to stderr */
		fp = stderr;

	WT_INDX_FOREACH(page, rip, i) {
		if (__wt_key_process(rip))
			fprintf(fp, "\tkey: {requires processing}\n");
		else
			__wt_debug_dbt("\tkey", rip, fp);

		__wt_debug_off(rip->data, "\t", fp);
	}
}

/*
 * __wt_debug_repl --
 *	Dump a replacement array.
 */
static void
__wt_debug_repl(WT_REPL *repl, FILE *fp)
{
	if (fp == NULL)				/* Default to stderr */
		fp = stderr;

	for (; repl != NULL; repl = repl->next)
		if (WT_REPL_DELETED_ISSET(repl))
			fprintf(fp, "\trepl: {deleted}\n");
		else
			__wt_debug_pair(
			    "\trepl", WT_REPL_DATA(repl), repl->size, fp);
}

/*
 * __wt_debug_rleexp --
 *	Dump a column store expansion array.
 */
static void
__wt_debug_rleexp(WT_RLE_EXPAND *exp, FILE *fp)
{
	WT_REPL *repl;

	if (fp == NULL)				/* Default to stderr */
		fp = stderr;

	for (; exp != NULL; exp = exp->next) {
		repl = exp->repl;
		if (WT_REPL_DELETED_ISSET(repl))
			fprintf(fp, "\trepl: {deleted}\n");
		else
			__wt_debug_pair(
			    "\trepl", WT_REPL_DATA(repl), repl->size, fp);
	}
}

/*
 * __wt_debug_dsk_item --
 *	Dump a page of WT_ITEM's.
 */
static int
__wt_debug_dsk_item(WT_TOC *toc, WT_PAGE_DISK *dsk, FILE *fp)
{
	WT_ITEM *item;
	uint32_t i;

	if (fp == NULL)				/* Default to stderr */
		fp = stderr;

	WT_ITEM_FOREACH(dsk, item, i)
		WT_RET(__wt_debug_item(toc, item, fp));
	return (0);
}

/*
 * __wt_debug_item --
 *	Dump a single WT_ITEM.
 */
static int
__wt_debug_item(WT_TOC *toc, WT_ITEM *item, FILE *fp)
{
	DB *db;
	WT_OVFL *ovfl;

	if (fp == NULL)				/* Default to stderr */
		fp = stderr;

	db = toc->db;

	fprintf(fp, "\t%s: len %lu",
	    __wt_item_type_string(item), (u_long)WT_ITEM_LEN(item));

	switch (WT_ITEM_TYPE(item)) {
	case WT_ITEM_KEY:
	case WT_ITEM_KEY_DUP:
	case WT_ITEM_DATA:
	case WT_ITEM_DATA_DUP:
		break;
	case WT_ITEM_KEY_OVFL:
	case WT_ITEM_KEY_DUP_OVFL:
	case WT_ITEM_DATA_OVFL:
	case WT_ITEM_DATA_DUP_OVFL:
		ovfl = WT_ITEM_BYTE_OVFL(item);
		fprintf(fp, ", addr %lu, size %lu",
		    (u_long)ovfl->addr, (u_long)ovfl->size);
		break;
	case WT_ITEM_DEL:
		fprintf(fp, "\n");
		return (0);
	case WT_ITEM_OFF:
		__wt_debug_off(WT_ITEM_BYTE_OFF(item), ", ", fp);
		return (0);
	WT_ILLEGAL_FORMAT(db);
	}

	fprintf(fp, "\n\t{");
	WT_RET(__wt_debug_item_data(toc, item, fp));
	fprintf(fp, "}\n");
	return (0);
}

/*
 * __wt_debug_dsk_col_int --
 *	Dump a WT_PAGE_COL_INT page.
 */
static void
__wt_debug_dsk_col_int(WT_PAGE_DISK *dsk, FILE *fp)
{
	WT_OFF *off;
	uint32_t i;

	if (fp == NULL)				/* Default to stderr */
		fp = stderr;

	WT_OFF_FOREACH(dsk, off, i)
		__wt_debug_off(off, "\t", fp);
}

/*
 * __wt_debug_dsk_col_fix --
 *	Dump a WT_PAGE_COL_FIX page.
 */
static void
__wt_debug_dsk_col_fix(DB *db, WT_PAGE_DISK *dsk, FILE *fp)
{
	uint32_t i;
	uint8_t *p;

	if (fp == NULL)				/* Default to stderr */
		fp = stderr;

	WT_FIX_FOREACH(db, dsk, p, i) {
		fprintf(fp, "\t{");
		if (WT_FIX_DELETE_ISSET(p))
			fprintf(fp, "deleted");
		else
			__wt_print_byte_string(p, db->fixed_len, fp);
		fprintf(fp, "}\n");
	}
}

/*
 * __wt_debug_dsk_col_rle --
 *	Dump a WT_PAGE_COL_RLE page.
 */
static void
__wt_debug_dsk_col_rle(DB *db, WT_PAGE_DISK *dsk, FILE *fp)
{
	uint32_t i;
	uint8_t *p;

	if (fp == NULL)				/* Default to stderr */
		fp = stderr;

	WT_RLE_REPEAT_FOREACH(db, dsk, p, i) {
		fprintf(fp, "\trepeat %lu {",
		    (u_long)WT_RLE_REPEAT_COUNT(p));
		if (WT_FIX_DELETE_ISSET(WT_RLE_REPEAT_DATA(p)))
			fprintf(fp, "deleted");
		else
			__wt_print_byte_string(
			    WT_RLE_REPEAT_DATA(p), db->fixed_len, fp);
		fprintf(fp, "}\n");
	}
}

/*
 * __wt_debug_item_data --
 *	Dump a single item's data in debugging mode.
 */
static int
__wt_debug_item_data(WT_TOC *toc, WT_ITEM *item, FILE *fp)
{
	DB *db;
	DBT *tmp;
	IDB *idb;
	uint32_t size;
	uint8_t *p;
	int ret;

	if (fp == NULL)				/* Default to stderr */
		fp = stderr;

	db = toc->db;
	tmp = NULL;
	idb = db->idb;
	ret = 0;

	switch (WT_ITEM_TYPE(item)) {
	case WT_ITEM_KEY:
		if (idb->huffman_key != NULL)
			goto process;
		goto onpage;
	case WT_ITEM_KEY_DUP:
	case WT_ITEM_DATA:
	case WT_ITEM_DATA_DUP:
		if (idb->huffman_data != NULL)
			goto process;
onpage:		p = WT_ITEM_BYTE(item);
		size = WT_ITEM_LEN(item);
		break;
	case WT_ITEM_KEY_OVFL:
	case WT_ITEM_KEY_DUP_OVFL:
	case WT_ITEM_DATA_OVFL:
	case WT_ITEM_DATA_DUP_OVFL:
process:	WT_ERR(__wt_scr_alloc(toc, 0, &tmp));
		WT_ERR(__wt_item_process(toc, item, tmp));
		p = tmp->data;
		size = tmp->size;
		break;
	case WT_ITEM_DEL:
		p = (uint8_t *)"deleted";
		size = 7;
		break;
	case WT_ITEM_OFF:
		p = (uint8_t *)"offpage";
		size = 7;
		break;
	WT_ILLEGAL_FORMAT_ERR(db, ret);
	}

	__wt_print_byte_string(p, size, fp);

err:	if (tmp != NULL)
		__wt_scr_release(&tmp);
	return (ret);
}

/*
 * __wt_debug_off --
 *	Dump a WT_OFF structure.
 */
static void
__wt_debug_off(WT_OFF *off, const char *prefix, FILE *fp)
{
	if (fp == NULL)				/* Default to stderr */
		fp = stderr;

	fprintf(fp, "%soffpage: addr %lu, size %lu, records %llu\n",
	    prefix, (u_long)off->addr, (u_long)off->size,
	    (unsigned long long)WT_RECORDS(off));
}

/*
 * __wt_debug_dbt --
 *	Dump a single DBT in debugging mode, with an optional tag.
 */
void
__wt_debug_dbt(const char *tag, void *arg_dbt, FILE *fp)
{
	DBT *dbt;

	if (fp == NULL)				/* Default to stderr */
		fp = stderr;

	/*
	 * The argument isn't necessarily a DBT structure, but the first two
	 * fields of the argument are always a void *data/uint32_t size pair.
	 */
	dbt = arg_dbt;
	__wt_debug_pair(tag, dbt->data, dbt->size, fp);
}

/*
 * __wt_debug_pair --
 *	Dump a single data/size pair, with an optional tag.
 */
static void
__wt_debug_pair(const char *tag, void *data, uint32_t size, FILE *fp)
{
	if (fp == NULL)				/* Default to stderr */
		fp = stderr;

	if (tag != NULL)
		fprintf(fp, "%s: ", tag);
	fprintf(fp, "%lu {",  (u_long)size);
	__wt_print_byte_string(data, size, fp);
	fprintf(fp, "}\n");
}
#endif
