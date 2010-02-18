/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

#ifdef HAVE_DIAGNOSTIC
static void __wt_bt_debug_dbt(const char *, void *, FILE *);
static void __wt_bt_debug_desc(WT_PAGE *, FILE *);
static int  __wt_bt_debug_item_data(WT_TOC *, WT_ITEM *, FILE *fp);
static void __wt_bt_debug_page_col_fix(DB *, WT_PAGE *, FILE *);
static int  __wt_bt_debug_page_col_inmemory(WT_PAGE *, FILE *);
static void __wt_bt_debug_page_col_int(WT_PAGE *, FILE *);
static int  __wt_bt_debug_page_item(WT_TOC *, WT_PAGE *, FILE *);
static int  __wt_bt_debug_page_row_inmemory(WT_PAGE *, FILE *);
static int  __wt_bt_debug_set_fp(const char *, FILE **, int *);

static int
__wt_bt_debug_set_fp(const char *ofile, FILE **fpp, int *close_varp)
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
 * __wt_bt_debug_dump --
 *	Dump a database in debugging mode.
 */
int
__wt_bt_debug_dump(WT_TOC *toc, char *ofile, FILE *fp)
{
	int do_close, ret;

	WT_RET(__wt_bt_debug_set_fp(ofile, &fp, &do_close));

	/*
	 * We use the verification code to do debugging dumps because if we're
	 * dumping in debugging mode, we want to confirm the page is OK before
	 * walking it.
	 */
	ret = __wt_bt_verify_int(toc, NULL, NULL, fp);

	if (do_close)
		(void)fclose(fp);

	return (ret);
}

/*
 * __wt_bt_debug_addr --
 *	Dump a single page in debugging mode.
 */
int
__wt_bt_debug_addr(WT_TOC *toc, u_int32_t addr, char *ofile, FILE *fp)
{
	DB *db;
	WT_PAGE *page;
	u_int32_t bytes;
	int do_close, ret;

	db = toc->db;

	WT_RET(__wt_bt_debug_set_fp(ofile, &fp, &do_close));

	/*
	 * Read in a single fragment.   If we get the page from the cache,
	 * it will be correct, and we can use it without further concern.
	 * If we don't get the page from the cache, figure out the type of
	 * the page and get it for real.
	 *
	 * We don't have any way to test if a page was found in the cache,
	 * so we check the in-memory page information -- pages in the cache
	 * should have in-memory page information.
	 */
	WT_RET(__wt_cache_in(
	    toc, addr, (u_int32_t)WT_FRAGMENT, WT_UNFORMATTED, &page));
	if (page->indx_count == 0) {
		switch (page->hdr->type) {
		case WT_PAGE_OVFL:
			bytes = WT_OVFL_BYTES(db, page->hdr->u.datalen);
			break;
		case WT_PAGE_COL_INT:
		case WT_PAGE_DUP_INT:
		case WT_PAGE_ROW_INT:
			bytes = db->intlsize;
			break;
		case WT_PAGE_COL_FIX:
		case WT_PAGE_COL_VAR:
		case WT_PAGE_DUP_LEAF:
		case WT_PAGE_ROW_LEAF:
			bytes = db->leafsize;
			break;
		WT_ILLEGAL_FORMAT(db);
		}
		WT_RET(__wt_cache_out(toc, page, 0));
		WT_RET(__wt_cache_in(toc, addr, bytes, 0, &page));
	}

	ret = __wt_bt_debug_page(toc, page, ofile, fp, 0);

	WT_TRET(__wt_cache_out(toc, page, 0));

	if (do_close)
		(void)fclose(fp);

	return (ret);
}

/*
 * __wt_bt_debug_page --
 *	Dump a single in-memory page in debugging mode.
 */
int
__wt_bt_debug_page(
    WT_TOC *toc, WT_PAGE *page, char *ofile, FILE *fp, int inmemory)
{
	DB *db;
	WT_PAGE_HDR *hdr;
	int do_close, ret;

	db = toc->db;
	hdr = page->hdr;
	ret = 0;

	WT_RET(__wt_bt_debug_set_fp(ofile, &fp, &do_close));

	fprintf(fp, "addr: %lu-%lu {\n", (u_long)page->addr,
	    (u_long)page->addr + (WT_OFF_TO_ADDR(db, page->bytes) - 1));

	fprintf(fp, "\taddr %lu, bytes %lu, lsn %lu/%lu\n",
	    (u_long)page->addr, (u_long)page->bytes,
	    (u_long)hdr->lsn[0], (u_long)hdr->lsn[1]);

	fprintf(fp, "\t%s: ", __wt_bt_hdr_type(hdr));
	if (hdr->type == WT_PAGE_OVFL)
		fprintf(fp, "%lu bytes\n", (u_long)hdr->u.datalen);
	else {
		fprintf(fp, "%lu entries, %llu records\n",
		    (u_long)hdr->u.entries, page->records);
		fprintf(fp,
		    "\tfirst-free %#lx, space avail: %lu\n",
		    (u_long)page->first_free, (u_long)page->space_avail);
	}
	if (hdr->prntaddr == WT_ADDR_INVALID)
		fprintf(fp, "\tprntaddr (none), ");
	else
		fprintf(fp, "\tprntaddr %lu, ", (u_long)hdr->prntaddr);
	if (hdr->prevaddr == WT_ADDR_INVALID)
		fprintf(fp, "prevaddr (none), ");
	else
		fprintf(fp, "prevaddr %lu, ", (u_long)hdr->prevaddr);
	if (hdr->nextaddr == WT_ADDR_INVALID)
		fprintf(fp, "nextaddr (none)");
	else
		fprintf(fp, "nextaddr %lu", (u_long)hdr->nextaddr);
	fprintf(fp, "\n}\n");

	if (page->addr == 0)
		__wt_bt_debug_desc(page, fp);

	if (inmemory)
		switch (hdr->type) {
		case WT_PAGE_DUP_INT:
		case WT_PAGE_DUP_LEAF:
		case WT_PAGE_ROW_INT:
		case WT_PAGE_ROW_LEAF:
			ret = __wt_bt_debug_page_row_inmemory(page, fp);
			break;
		case WT_PAGE_COL_FIX:
		case WT_PAGE_COL_INT:
		case WT_PAGE_COL_VAR:
			ret = __wt_bt_debug_page_col_inmemory(page, fp);
			break;
		case WT_PAGE_OVFL:
			break;
		WT_ILLEGAL_FORMAT(db);
		}
	else
		switch (hdr->type) {
		case WT_PAGE_COL_VAR:
		case WT_PAGE_DUP_INT:
		case WT_PAGE_DUP_LEAF:
		case WT_PAGE_ROW_INT:
		case WT_PAGE_ROW_LEAF:
			ret = __wt_bt_debug_page_item(toc, page, fp);
			break;
		case WT_PAGE_COL_FIX:
			__wt_bt_debug_page_col_fix(db, page, fp);
			break;
		case WT_PAGE_COL_INT:
			__wt_bt_debug_page_col_int(page, fp);
			break;
		case WT_PAGE_OVFL:
			break;
		WT_ILLEGAL_FORMAT(db);
		}

	if (do_close)
		(void)fclose(fp);

	return (ret);
}

/*
 * __wt_bt_debug_desc --
 *	Dump the database description on page 0.
 */
static void
__wt_bt_debug_desc(WT_PAGE *page, FILE *fp)
{
	WT_PAGE_DESC desc;

	if (fp == NULL)				/* Callable from a debugger. */
		fp = stdout;

	memcpy(
	    &desc, (u_int8_t *)page->hdr + WT_PAGE_HDR_SIZE, WT_PAGE_DESC_SIZE);

	fprintf(fp, "description record: {\n"),
	fprintf(fp, "\tmagic: %#lx, major: %lu, minor: %lu\n",
	    (u_long)desc.magic, (u_long)desc.majorv, (u_long)desc.minorv);
	fprintf(fp, "\tintlsize: %lu, leafsize: %lu, base record: %llu\n",
	    (u_long)desc.intlsize, (u_long)desc.leafsize, desc.base_recno);
	fprintf(fp, "\tfixed_len: %lu\n", (u_long)desc.fixed_len);
	if (desc.root_addr == WT_ADDR_INVALID)
		fprintf(fp, "\troot addr (none), ");
	else
		fprintf(fp, "\troot addr %lu, ", (u_long)desc.root_addr);
	if (desc.free_addr == WT_ADDR_INVALID)
		fprintf(fp, "free addr (none)");
	else
		fprintf(fp, "free addr %lu", (u_long)desc.free_addr);
	fprintf(fp, "\n}\n");
}

/*
 * __wt_bt_debug_page_col_inmemory --
 *	Dump the WT_PAGE information for a column-store.
 */
static int
__wt_bt_debug_page_col_inmemory(WT_PAGE *page, FILE *fp)
{
	WT_COL_INDX *ip;
	WT_SDBT *sdbt;
	u_int32_t i, icnt, repl_cnt;

	if (fp == NULL)				/* Callable from a debugger. */
		fp = stdout;

	icnt = 0;
	WT_INDX_FOREACH(page, ip, i) {
		fprintf(fp, "%6lu:\n", (u_long)++icnt);
		if (ip->page_data != NULL)
			fprintf(fp, "\tpage_data: %#lx",
			    WT_ADDR_TO_ULONG(ip->page_data));
		if (ip->repl != NULL) {
			for (sdbt = ip->repl->data, repl_cnt = 0;
			    repl_cnt < ip->repl->repl_next; ++sdbt, ++repl_cnt)
				if (sdbt->data == WT_DATA_DELETED)
					fprintf(fp, "\trepl deleted");
				else
					__wt_bt_debug_dbt(
					    "\trepl dbt", sdbt, fp);
		}
		fprintf(fp, "\n");
	}
	return (0);
}

/*
 * __wt_bt_debug_page_row_inmemory --
 *	Dump the WT_PAGE information for a row-store.
 */
static int
__wt_bt_debug_page_row_inmemory(WT_PAGE *page, FILE *fp)
{
	WT_ROW_INDX *ip;
	WT_SDBT *sdbt;
	u_int32_t i, icnt, repl_cnt;

	if (fp == NULL)				/* Callable from a debugger. */
		fp = stdout;

	icnt = 0;
	WT_INDX_FOREACH(page, ip, i) {
		fprintf(fp,
		    "%6lu: {flags %#lx}\n", (u_long)++icnt, (u_long)ip->flags);
		if (ip->data != NULL)
			__wt_bt_debug_dbt("\tdata dbt", ip, fp);
		if (ip->repl != NULL) {
			for (sdbt = ip->repl->data, repl_cnt = 0;
			    repl_cnt < ip->repl->repl_next; ++sdbt, ++repl_cnt)
				if (sdbt->data == WT_DATA_DELETED)
					fprintf(fp, "\trepl deleted");
				else
					__wt_bt_debug_dbt(
					    "\trepl dbt", sdbt, fp);
		}
		if (ip->page_data != NULL)
			fprintf(fp, "\tpage_data: %#lx",
			    WT_ADDR_TO_ULONG(ip->page_data));
		fprintf(fp, "\n");
	}
	return (0);
}

/*
 * __wt_bt_debug_page_item --
 *	Dump a page of WT_ITEM's.
 */
static int
__wt_bt_debug_page_item(WT_TOC *toc, WT_PAGE *page, FILE *fp)
{
	WT_ITEM *item;
	u_int32_t i, cnt;

	if (fp == NULL)				/* Callable from a debugger. */
		fp = stdout;

	cnt = 1;
	WT_ITEM_FOREACH(page, item, i) {
		fprintf(fp, "%6lu: {type %s; len %lu; off %lu}\n\t{",
		    (u_long)cnt++, __wt_bt_item_type(item),
		    (u_long)WT_ITEM_LEN(item),
		    (u_long)((u_int8_t *)item - (u_int8_t *)(page->hdr)));
		WT_RET(__wt_bt_debug_item_data(toc, item, fp));
		fprintf(fp, "}\n");
	}
	return (0);
}

/*
 * __wt_bt_debug_page_col_int --
 *	Dump a WT_PAGE_COL_INT page.
 */
static void
__wt_bt_debug_page_col_int(WT_PAGE *page, FILE *fp)
{
	WT_OFF *offp;
	u_int32_t i;
	char *ref;

	if (fp == NULL)				/* Callable from a debugger. */
		fp = stdout;

	ref = F_ISSET(page->hdr, WT_OFFPAGE_REF_LEAF) ? "leaf" : "tree";
	WT_OFF_FOREACH(page, offp, i)
		fprintf(fp, "\toffpage %s { addr: %lu, records %llu }\n",
		    ref, (u_long)offp->addr, (u_quad)WT_RECORDS(offp));
}

/*
 * __wt_bt_debug_page_col_fix --
 *	Dump a WT_PAGE_COL_FIX page.
 */
static void
__wt_bt_debug_page_col_fix(DB *db, WT_PAGE *page, FILE *fp)
{
	IDB *idb;
	u_int32_t i;
	u_int8_t *p;

	if (fp == NULL)				/* Callable from a debugger. */
		fp = stdout;

	idb = db->idb;

	if (F_ISSET(idb, WT_REPEAT_COMP))
		WT_FIX_REPEAT_FOREACH(db, page, p, i) {
			fprintf(fp, "\trepeat %lu {", (u_long)*(u_int16_t *)p);
			__wt_bt_print(p + sizeof(u_int16_t), db->fixed_len, fp);
			fprintf(fp, "}\n");
		}
	else
		WT_FIX_FOREACH(db, page, p, i) {
			fprintf(fp, "\t{");
			__wt_bt_print(p, db->fixed_len, fp);
			fprintf(fp, "}\n");
		}
}

/*
 * __wt_bt_debug_item_data --
 *	Dump a single item's data in debugging mode.
 */
static int
__wt_bt_debug_item_data(WT_TOC *toc, WT_ITEM *item, FILE *fp)
{
	DB *db;
	IDB *idb;
	WT_OFF *offp;
	WT_OVFL *ovfl;
	WT_PAGE *page;
	u_int32_t len;
	u_int8_t *p;
	void *hp;
	int ret;

	db = toc->db;
	idb = db->idb;
	page = NULL;
	hp = NULL;
	ret = 0;

	switch (WT_ITEM_TYPE(item)) {
	case WT_ITEM_KEY:
		hp = idb->huffman_key;
		p = WT_ITEM_BYTE(item);
		len = WT_ITEM_LEN(item);
		break;
	case WT_ITEM_DATA:
	case WT_ITEM_DUP:
		hp = idb->huffman_data;
		p = WT_ITEM_BYTE(item);
		len = WT_ITEM_LEN(item);
		break;
	case WT_ITEM_KEY_OVFL:
	case WT_ITEM_DATA_OVFL:
	case WT_ITEM_DUP_OVFL:
		ovfl = WT_ITEM_BYTE_OVFL(item);
		fprintf(fp, "addr %lu; len %lu; ",
		    (u_long)ovfl->addr, (u_long)ovfl->len);

		WT_ERR(__wt_bt_ovfl_in(
		    toc, ovfl->addr, (u_int32_t)ovfl->len, &page));
		hp = idb->huffman_data;
		p = WT_PAGE_BYTE(page);
		len = ovfl->len;
		break;
	case WT_ITEM_OFF_INT:
	case WT_ITEM_OFF_LEAF:
		offp = WT_ITEM_BYTE_OFF(item);
		fprintf(fp, "addr: %lu, records %llu",
		    (u_long)offp->addr, WT_RECORDS(offp));
		return (0);
	WT_ILLEGAL_FORMAT(db);
	}

	/* Uncompress the item as necessary. */
	if (hp != NULL) {
		WT_ERR(__wt_huffman_decode(
		    hp, p, len, &toc->scratch.data,
		    &toc->scratch.data_len, &toc->scratch.size));
		p = toc->scratch.data;
		len = toc->scratch.size;
	}

	__wt_bt_print(p, len, fp);

err:	if (page != NULL)
		(void)__wt_bt_page_out(toc, page, 0);
	return (ret);
}

/*
 * __wt_bt_debug_dbt --
 *	Dump a single DBT in debugging mode, with an optional tag.
 */
static void
__wt_bt_debug_dbt(const char *tag, void *arg_dbt, FILE *fp)
{
	DBT *dbt;

	if (fp == NULL)				/* Callable from a debugger */
		fp = stdout;

	/*
	 * The argument isn't necessarily a DBT structure, but the first two
	 * fields of the argument are always a void *data/u_int32_t size pair.
	 */
	dbt = arg_dbt;

	if (tag != NULL)
		fprintf(fp, "%s: ", tag);
	fprintf(fp, "{");
	__wt_bt_print(dbt->data, dbt->size, fp);
	fprintf(fp, "}\n");
}
#endif
