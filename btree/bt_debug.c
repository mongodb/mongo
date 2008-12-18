/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static void __wt_db_dump_item_data (DB *, WT_ITEM *, FILE *);

#ifdef HAVE_DIAGNOSTIC
/*
 * __wt_db_force_load --
 *	For the code to be loaded, and a simple place to put a breakpoint.
 */
int
__wt_db_force_load()
{
	return (0);
}

/*
 * __wt_db_dump_debug --
 *	Dump a database in debugging mode.
 */
int
__wt_db_dump_debug(DB *db, FILE *stream)
{
	WT_BTREE *bt;
	WT_PAGE_HDR *hdr;
	u_int32_t addr;
	int ret;

	bt = db->idb->btree;

	for (addr = WT_ADDR_FIRST_PAGE;;) {
		if ((ret =
		    __wt_db_fread(bt, addr, WT_FRAGS_PER_PAGE(db), &hdr)) != 0)
			break;

		fprintf(stream, "====== %lu\n",  (u_long)addr);

		if ((ret = __wt_db_dump_page(db, hdr, NULL, stream)) != 0)
			break;

		addr = hdr->nextaddr;
		 if ((ret = __wt_db_fdiscard(bt, addr, hdr)) != 0)
			 break;
		if (addr == WT_ADDR_INVALID)
			break;
	}

	return (ret);
}

/*
 * __wt_db_dump_addr --
 *	Dump a single page in debugging mode.
 */
int
__wt_db_dump_addr(DB *db, u_int32_t addr, char *ofile, FILE *fp)
{
	WT_BTREE *bt;
	WT_PAGE_HDR *hdr;
	int ret, tret;

	bt = db->idb->btree;

	if ((ret = __wt_db_fread(bt, addr, WT_FRAGS_PER_PAGE(db), &hdr)) != 0)
		return (ret);

	ret = __wt_db_dump_page(db, hdr, ofile, fp);

	if ((tret = __wt_db_fdiscard(bt, addr, hdr)) != 0 && ret == 0)
		ret = tret;

	return (ret);
}

/*
 * __wt_db_dump_page --
 *	Dump a single page in debugging mode.
 */
int
__wt_db_dump_page(DB *db, WT_PAGE_HDR *hdr, char *ofile, FILE *fp)
{
	WT_ITEM *item;
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

	fprintf(fp, "{\n%s: ", __wt_db_hdr_type(hdr->type));
	if (hdr->type == WT_PAGE_OVFL)
		fprintf(fp, "%lu bytes", (u_long)hdr->u.datalen);
	else
		fprintf(fp, "%lu entries", (u_long)hdr->u.entries);
	fprintf(fp, "\nlsn %lu/%lu, checksum %lx, "
	    "prntaddr %lu, prevaddr %lu, nextaddr %lu\n}\n",
	    (u_long)hdr->lsn.fileno, (u_long)hdr->lsn.offset,
	    (u_long)hdr->checksum, (u_long)hdr->prntaddr,
	    (u_long)hdr->prevaddr, (u_long)hdr->nextaddr);

	if (hdr->type == WT_PAGE_OVFL)
		return (0);

	for (p = WT_PAGE_BYTE(hdr), i = 1; i <= hdr->u.entries; ++i) {
		item = (WT_ITEM *)p;
		fprintf(fp, "%6lu: {type %s; len %lu; off %lu}\n\t{",
		    (u_long)i, __wt_db_item_type(item->type),
		    (u_long)item->len, (u_long)(p - (u_int8_t *)hdr));
		__wt_db_dump_item_data(db, item, fp);
		fprintf(fp, "}\n");
		p += WT_ITEM_SPACE_REQ(item->len);
	}

	if (do_close)
		(void)fclose(fp);

	return (0);
}

/*
 * __wt_db_dump_item --
 *	Dump a single item.
 */
void
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
	WT_BTREE *bt;
	WT_ITEM_OVFL *item_ovfl;
	WT_ITEM_OFFP *item_offp;
	WT_PAGE_HDR *hdr;
	u_int32_t addr, frags;

	bt = db->idb->btree;

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
		WT_OVERFLOW_BYTES_TO_FRAGS(db, item_ovfl->len, frags);
		if (__wt_db_fread(bt, item_ovfl->addr, frags, &hdr) == 0) {
			__wt_db_print(WT_PAGE_BYTE(hdr), item_ovfl->len, fp);
			(void)__wt_db_fdiscard(bt, addr, hdr);
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
void
__wt_db_dump_dbt(DBT *dbt, FILE *fp)
{
	if (fp == NULL)
		fp = stdout;
	fprintf(fp, "{");
	__wt_db_print(dbt->data, dbt->size, fp);
	fprintf(fp, "}\n");
}
#endif
