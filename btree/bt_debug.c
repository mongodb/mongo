/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static const char *__wt_db_hdr_type(u_int32_t);
static const char *__wt_db_item_type(u_int32_t);

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
 * __wt_db_dump_db --
 *	Dump a database in debugging mode.
 */
int
__wt_db_dump_db(DB *db, char *ofile)
{
	FILE *fp;
	WT_BTREE *bt;
	WT_PAGE_HDR *hdr;
	u_int32_t addr;
	int ret;

	bt = db->idb->btree;

	if ((fp = fopen(ofile, "w")) == NULL)
		return (WT_ERROR);

	for (addr = WT_BTREE_ROOT;;) {
		if ((ret =
		    __wt_db_fread(bt, addr, WT_FRAGS_PER_PAGE(db), &hdr)) != 0)
			break;
		if ((ret = __wt_db_dump_page(db, hdr, NULL, fp)) != 0)
			break;

		addr = hdr->nextaddr;
		 if ((ret = __wt_db_fdiscard(bt, addr, hdr)) != 0)
			 break;
		if (addr == WT_ADDR_INVALID)
			break;
	}

	(void)fclose(fp);
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

	fprintf(fp, "======\n");
	fprintf(fp, "{\n\t%s: ", __wt_db_hdr_type(hdr->type));
	if (hdr->type == WT_PAGE_OVFL)
		fprintf(fp, "%lu bytes", (u_long)hdr->u.datalen);
	else
		fprintf(fp, "%lu entries", (u_long)hdr->u.entries);
	fprintf(fp,
	    "\n\tlsn %lu/%lu, checksum %lx, "
	    "prntaddr %lu, prevaddr %lu, nextaddr %lu\n}\n",
	    (u_long)hdr->lsn.fileno, (u_long)hdr->lsn.offset,
	    (u_long)hdr->checksum, (u_long)hdr->prntaddr,
	    (u_long)hdr->prevaddr, (u_long)hdr->nextaddr);

	if (hdr->type == WT_PAGE_OVFL)
		return (0);

	for (p = WT_PAGE_BYTE(hdr), i = 1; i <= hdr->u.entries; ++i) {
		item = (WT_ITEM *)p;
		fprintf(fp, "%6lu: {type %s; len %lu; off %lu}\n",
		    (u_long)i, __wt_db_item_type(item->type),
		    (u_long)item->len, (u_long)(p - (u_int8_t *)hdr));
		__wt_db_dump_item(db, item, fp);
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
	WT_BTREE *bt;
	WT_ITEM_OVFL *item_ovfl;
	WT_ITEM_OFFP *item_offp;
	WT_PAGE_HDR *hdr;
	u_int32_t addr, frags;

	bt = db->idb->btree;

	if (fp == NULL)
		fp = stdout;

	fprintf(fp, "\t%s {", __wt_db_item_type(item->type));

	switch (item->type) {
	case WT_ITEM_KEY:
	case WT_ITEM_DATA:
	case WT_ITEM_DUP:
		__wt_db_print(WT_ITEM_BYTE(item), item->len, fp);
		break;
	case WT_ITEM_OFFPAGE:
		item_offp = (WT_ITEM_OFFP *)WT_ITEM_BYTE(item);
		fprintf(fp, "addr: %lu, records %lu",
		    (u_long)item_offp->addr, (u_long)item_offp->records);
		break;
	case WT_ITEM_KEY_OVFL:
	case WT_ITEM_DATA_OVFL:
	case WT_ITEM_DUP_OVFL:
		item_ovfl = (WT_ITEM_OVFL *)WT_ITEM_BYTE(item);
		fprintf(fp, "addr: %lu, len %lu\n",
		    (u_long)item_ovfl->addr, (u_long)item_ovfl->len);
		WT_OVERFLOW_BYTES_TO_FRAGS(db, item_ovfl->len, frags);
		if (__wt_db_fread(bt, item_ovfl->addr, frags, &hdr) == 0) {
			__wt_db_print(WT_PAGE_BYTE(hdr), item->len, fp);
			(void)__wt_db_fdiscard(bt, addr, hdr);
		}
		break;
	default:
		fprintf(fp, "unsupported type");
		break;
	}
	fprintf(fp, "}\n");
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

static const char *
__wt_db_hdr_type(u_int32_t type)
{
	switch (type) {
	case WT_PAGE_INVALID:
		return ("invalid");
	case WT_PAGE_OVFL:
		return ("overflow");
	case WT_PAGE_ROOT:
		return ("primary root");
	case WT_PAGE_INT:
		return ("primary internal");
	case WT_PAGE_LEAF:
		return ("primary leaf");
	case WT_PAGE_DUP_ROOT:
		return ("off-page duplicate root");
	case WT_PAGE_DUP_INT:
		return ("off-page duplicate internal");
	case WT_PAGE_DUP_LEAF:
		return ("off-page duplicate leaf");
	default:
		break;
	}
	return ("unknown");
}

static const char *
__wt_db_item_type(u_int32_t type)
{
	switch (type) {
	case WT_ITEM_KEY:
		return ("key");
	case WT_ITEM_DATA:
		return ("data");
	case WT_ITEM_DUP:
		return ("duplicate");
	case WT_ITEM_KEY_OVFL:
		return ("key-overflow");
	case WT_ITEM_DATA_OVFL:
		return ("data-overflow");
	case WT_ITEM_DUP_OVFL:
		return ("duplicate-overflow");
	case WT_ITEM_OFFPAGE:
		return ("offpage");
	default:
		break;
	}
	return ("unknown");
}
#endif
