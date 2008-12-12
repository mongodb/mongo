/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static const char *__wt_bt_hdr_type(u_int32_t);
static const char *__wt_bt_item_type(u_int32_t);

#ifdef HAVE_DIAGNOSTIC
/*
 * __wt_bt_force_load --
 *	For the code to be loaded, and a simple place to put a breakpoint.
 */
int
__wt_bt_force_load()
{
	return (0);
}

/*
 * __wt_bt_dump_pgno --
 *	Dump a single page in debugging mode.
 */
int
__wt_bt_dump_pgno(DB *db, u_int32_t addr, char *ofile)
{
	WT_BTREE *bt;
	WT_PAGE_HDR *hdr;
	int ret, tret;

	bt = db->idb->btree;

	if ((ret = __wt_bt_fread(bt, addr, WT_FRAGS_PER_PAGE(db), &hdr)) != 0)
		return (ret);

	ret = __wt_bt_dump_page(db, hdr, ofile);

	if ((tret = __wt_bt_fdiscard(bt, addr, hdr)) != 0 && ret == 0)
		ret = tret;

	return (ret);
}

/*
 * __wt_bt_dump_page --
 *	Dump a single page in debugging mode.
 */
int
__wt_bt_dump_page(DB *db, WT_PAGE_HDR *hdr, char *ofile)
{
	FILE *fp;
	WT_ITEM *item;
	u_int32_t i;
	u_int8_t *p;
	int ret;

	ret = 0;

	/* If there's a file to use, use it, otherwise use stderr. */
	if (ofile == NULL)
		fp = stderr;
	else
		if ((fp = fopen(ofile, "w")) == NULL)
			return (WT_ERROR);

	fprintf(fp, "======\n");
	fprintf(fp, "{\n\t%s: ", __wt_bt_hdr_type(hdr->type));
	if (hdr->type == WT_PAGE_OVFL)
		fprintf(fp, "%lu bytes", (u_long)hdr->u.datalen);
	else
		fprintf(fp, "%lu entries", (u_long)hdr->u.entries);
	fprintf(fp,
	    "\n\tlsn %lu/%lu, checksum %lx, "
	    "paraddr %lu, prevaddr %lu, nextaddr %lu\n}\n",
	    (u_long)hdr->lsn.fileno, (u_long)hdr->lsn.offset,
	    (u_long)hdr->checksum,
	    (u_long)hdr->paraddr, (u_long)hdr->prevaddr, (u_long)hdr->nextaddr);

	if (hdr->type == WT_PAGE_OVFL)
		return (0);

	for (p = WT_PAGE_DATA(hdr), i = 1; i <= hdr->u.entries; ++i) {
		item = (WT_ITEM *)p;
		fprintf(fp, "%8lu: {type %s; len %lu; off %lu}\n",
		    (u_long)i, __wt_bt_item_type(item->type),
		    (u_long)item->len, (u_long)(p - (u_int8_t *)hdr));
		__wt_bt_dump_item(item, fp);
		p += WT_ITEM_SPACE_REQ(item->len);
	}

err:	if (ofile != NULL)
		(void)fclose(fp);

	return (ret);
}

/*
 * __wt_bt_dump_item --
 *	Dump a single item.
 */
void
__wt_bt_dump_item(WT_ITEM *item, FILE *fp)
{
	if (fp == NULL)
		fp = stdout;

	switch (item->type) {
	case WT_ITEM_KEY:
	case WT_ITEM_DATA:
	case WT_ITEM_DUP:
		fprintf(fp, "\t{");
		__wt_bt_print(WT_ITEM_BYTE(item), item->len, fp);
		fprintf(fp, "}\n");
		break;
	default:
		fprintf(fp, "\t{unsupported type}\n");
		break;
	}
}

static const char *
__wt_bt_hdr_type(u_int32_t type)
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
__wt_bt_item_type(u_int32_t type)
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
	default:
		break;
	}
	return ("unknown");
}
#endif
