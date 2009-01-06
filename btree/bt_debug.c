/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int  __wt_db_dump_addr(DB *, u_int32_t, char *, FILE *);
static void __wt_db_dump_dbt(DBT *, FILE *);
static void __wt_db_dump_item(DB *, WT_ITEM *, FILE *);
static void __wt_db_dump_item_data (DB *, WT_ITEM *, FILE *);
static int  __wt_db_dump_page(DB *, WT_PAGE *, char *, FILE *);

#ifdef HAVE_DIAGNOSTIC
/*
 * __wt_db_force_load --
 *	For the code to be loaded, and a simple place to put a breakpoint.
 */
int
__wt_db_force_load(void)
{
	return (0);
}

/*
 * __wt_db_dump_debug --
 *	Dump a database in debugging mode.
 */
int
__wt_db_dump_debug(DB *db, char *ofile, FILE *fp)
{
	IDB *idb;
	WT_PAGE *page;
	u_int32_t addr, frags;
	int do_close, ret, tret;

	idb = db->idb;

	/* Optionally dump to a file, else to a stream, default to stdout. */
	do_close = 0;
	if (ofile != NULL) {
		if ((fp = fopen(ofile, "w")) == NULL)
			return (WT_ERROR);
		do_close = 1;
	} else if (fp == NULL)
		fp = stdout;

	for (addr = WT_ADDR_FIRST_PAGE;;) {
		/* Read a database page of information. */
		frags = WT_FRAGS_PER_PAGE(db);
		if ((ret = __wt_cache_db_in(
		    db, addr, frags, &page, WT_NO_CHECKSUM)) != 0)
			break;

		/*
		 * Check for overflow pages.  Reads of overflow pages from
		 * the disk may not be the right length.  (If we read the
		 * page out of the cache, it will be OK, it's only if it
		 * was read from the disk that it might be wrong.)  Check
		 * the length, and if it's incorrect, discard this page and
		 * get the right one.
		 */
		if (page->hdr->type == WT_PAGE_OVFL) {
			frags = WT_BYTES_TO_FRAGS(db, page->hdr->u.datalen);
			if (frags != page->frags && (
			    (ret = __wt_cache_db_out(
			    db, page, WT_DISCARD)) != 0 ||
			    (ret = __wt_cache_db_in(
			    db, addr, frags, &page, WT_NO_CHECKSUM)) != 0))
				break;
		}

		ret = __wt_db_dump_page(db, page, NULL, fp);

		if ((tret = __wt_cache_db_out(db, page, 0)) != 0 && ret == 0)
			ret = tret;

		addr += frags;
		if (ret != 0 || addr >= idb->frags)
			break;
	}

	if (do_close)
		(void)fclose(fp);

	return (ret);
}

/*
 * __wt_db_dump_addr --
 *	Dump a single page in debugging mode.
 */
static int
__wt_db_dump_addr(DB *db, u_int32_t addr, char *ofile, FILE *fp)
{
	WT_PAGE *page;
	int ret, tret;

	if ((ret =
	    __wt_cache_db_in(db, addr, WT_FRAGS_PER_PAGE(db), &page, 0)) != 0)
		return (ret);

	ret = __wt_db_dump_page(db, page, ofile, fp);

	if ((tret = __wt_cache_db_out(db, page, 0)) != 0 && ret == 0)
		ret = tret;

	return (ret);
}

/*
 * __wt_db_dump_page --
 *	Dump a single page in debugging mode.
 */
static int
__wt_db_dump_page(DB *db, WT_PAGE *page, char *ofile, FILE *fp)
{
	WT_ITEM *item;
	WT_PAGE_DESC desc;
	WT_PAGE_HDR *hdr;
	u_int32_t i;
	u_int8_t *p;
	int do_close;

	/* Optionally dump to a file, else to a stream, default to stdout. */
	do_close = 0;
	if (ofile != NULL) {
		if ((fp = fopen(ofile, "w")) == NULL)
			return (WT_ERROR);
		do_close = 1;
	} else if (fp == NULL)
		fp = stdout;

	fprintf(fp, "fragments: %lu-%lu {\n",
	    (u_long)page->addr, (u_long)page->addr + (page->frags - 1));

	/* Dump the description area, if it's page 0. */
	if (page->addr == 0) {
		memcpy(&desc,
		    (u_int8_t *)page->hdr + WT_HDR_SIZE, WT_DESC_SIZE);
		fprintf(fp, "magic: %#lx, major: %lu, minor: %lu\n",
		    (u_long)desc.magic,
		    (u_long)desc.majorv, (u_long)desc.minorv);
		fprintf(fp, "pagesize: %lu, base record: %lu\n",
		    (u_long)desc.pagesize, (u_long)desc.base_recno);
		if (desc.root_addr == WT_ADDR_INVALID)
			fprintf(fp, "root fragment (none), ");
		else
			fprintf(fp,
			    "root fragment %lu, ", (u_long)desc.root_addr);
		if (desc.free_addr == WT_ADDR_INVALID)
			fprintf(fp, "free fragment (none), ");
		else
			fprintf(fp,
			    "free fragment %lu, ", (u_long)desc.free_addr);
		fprintf(fp, "\n");
	}

	hdr = page->hdr;
	fprintf(fp, "%s: ", __wt_db_hdr_type(hdr->type));
	if (hdr->type == WT_PAGE_OVFL)
		fprintf(fp, "%lu bytes", (u_long)hdr->u.datalen);
	else
		fprintf(fp, "%lu entries", (u_long)hdr->u.entries);
	fprintf(fp, ", lsn %lu/%lu\n", (u_long)hdr->lsn.f, (u_long)hdr->lsn.o);
	if (hdr->prntaddr == WT_ADDR_INVALID)
		fprintf(fp, "prntaddr (none), ");
	else
		fprintf(fp, "prntaddr %lu, ", (u_long)hdr->prntaddr);
	if (hdr->prevaddr == WT_ADDR_INVALID)
		fprintf(fp, "prevaddr (none), ");
	else
		fprintf(fp, "prevaddr %lu, ", (u_long)hdr->prevaddr);
	if (hdr->nextaddr == WT_ADDR_INVALID)
		fprintf(fp, "nextaddr (none)");
	else
		fprintf(fp, "nextaddr %lu", (u_long)hdr->nextaddr);

	fprintf(fp, "\nfirst-free %#lx, space avail: %lu\n",
	    (u_long)page->first_free, (u_long)page->space_avail);

	fprintf(fp, "}\n");

	if (hdr->type == WT_PAGE_OVFL)
		return (0);

	for (p = WT_PAGE_BYTE(page), i = 1; i <= hdr->u.entries; ++i) {
		item = (WT_ITEM *)p;
		fprintf(fp, "%6lu: {type %s; len %lu; off %lu}\n\t{",
		    (u_long)i, __wt_db_item_type(item->type),
		    (u_long)item->len, (u_long)(p - (u_int8_t *)hdr));
		__wt_db_dump_item_data(db, item, fp);
		fprintf(fp, "}\n");
		p += WT_ITEM_SPACE_REQ(item->len);
	}
	fprintf(fp, "\n");

	if (do_close)
		(void)fclose(fp);

	return (0);
}

/*
 * __wt_db_dump_item --
 *	Dump a single item.
 */
static void
__wt_db_dump_item(DB *db, WT_ITEM *item, FILE *fp)
{
	if (fp == NULL)
		fp = stdout;

	fprintf(fp, "%s {", __wt_db_item_type(item->type));

	__wt_db_dump_item_data(db, item, fp);

	fprintf(fp, "}\n");
}


/*
 * __wt_db_dump_item_data --
 *	Dump an item's data.
 */
static void
__wt_db_dump_item_data (DB *db, WT_ITEM *item, FILE *fp)
{
	WT_ITEM_OVFL *item_ovfl;
	WT_ITEM_OFFP *item_offp;
	WT_PAGE *page;

	switch (item->type) {
	case WT_ITEM_KEY:
	case WT_ITEM_DATA:
	case WT_ITEM_DUP:
		__wt_db_print(WT_ITEM_BYTE(item), item->len, fp);
		break;
	case WT_ITEM_KEY_OVFL:
	case WT_ITEM_DATA_OVFL:
	case WT_ITEM_DUP_OVFL:
		item_ovfl = (WT_ITEM_OVFL *)WT_ITEM_BYTE(item);
		fprintf(fp, "addr %lu; len %lu; ",
		    (u_long)item_ovfl->addr, (u_long)item_ovfl->len);
		
		if (__wt_cache_db_in(db, item_ovfl->addr,
		    WT_OVFL_BYTES_TO_FRAGS(db, item_ovfl->len),
		    &page, 0) == 0) {
			__wt_db_print(WT_PAGE_BYTE(page), item_ovfl->len, fp);
			(void)__wt_cache_db_out(db, page, 0);
		}
		break;
	case WT_ITEM_OFFPAGE:
		item_offp = (WT_ITEM_OFFP *)WT_ITEM_BYTE(item);
		fprintf(fp, "addr: %lu, records %lu",
		    (u_long)item_offp->addr, (u_long)item_offp->records);
		break;
	default:
		fprintf(fp, "unsupported type");
		break;
	}
}

/*
 * __wt_db_dump_dbt --
 *	Dump a single DBT.
 */
static void
__wt_db_dump_dbt(DBT *dbt, FILE *fp)
{
	if (fp == NULL)
		fp = stdout;
	fprintf(fp, "{");
	__wt_db_print(dbt->data, dbt->size, fp);
	fprintf(fp, "}\n");
}
#endif
