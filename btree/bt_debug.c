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
__wt_bt_dump_pgno(DB *db, u_int32_t pgno, char *ofile)
{
	WT_BTREE *bt;
	WT_PAGE_HDR *hdr;
	int ret, tret;

	bt = db->idb->btree;

	if ((ret = __wt_bt_fread(bt,
	    WT_PGNO_TO_BLOCKS(db, pgno),
	    WT_BYTES_TO_BLOCKS(db->pagesize), &hdr)) != 0)
		return (ret);

	ret = __wt_bt_dump_page(db, hdr, ofile);

	if ((tret = __wt_bt_fdiscard(bt,
	    WT_PGNO_TO_BLOCKS(db, pgno), hdr)) != 0 && ret == 0)
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
	fprintf(fp,
	    "hdr {\n\tlsn %lu/%lu, type %s, flags %s,\n\tchecksum %lx,"
	    " entries %lu, prevpg %lu, nextpg %lu\n}\n",
	    hdr->lsn.fileno, hdr->lsn.offset, __wt_bt_hdr_type(hdr->type),
	    __wt_bt_hdr_flags(hdr->flags), (u_long)hdr->checksum,
	    (u_long)hdr->entries, (u_long)hdr->prevpg, (u_long)hdr->nextpg);

	for (p = (u_int8_t *)hdr + WT_HDR_SIZE, i = 1; i <= hdr->entries; ++i) {
		item = (WT_ITEM *)p;

		fprintf(fp, "%8lu: %lu { len %lu, type %s }\n",
		    (u_long)i, (u_long)(p - (u_int8_t *)hdr),
		    (u_long)item->len, __wt_bt_item_type(item->type));

		switch (item->type) {
		case WT_ITEM_STANDARD:
			fprintf(fp, "\t{");
			__wt_bt_print(
			    p + sizeof(WT_ITEM), item->len, fp);
			fprintf(fp, "}\n");
			p += WT_ITEM_SPACE_REQ(item->len);

			break;
		default:
			return (__wt_database_format(db));
		}
	}

err:	if (ofile != NULL)
		(void)fclose(fp);

	return (ret);
}

const char *
__wt_bt_hdr_type(u_int32_t type)
{
	switch (type) {
	case WT_PAGE_INVALID:
		return ("invalid");
	case WT_PAGE_BTREE_ROOT:
		return ("btree-root");
	case WT_PAGE_BTREE_INTERNAL:
		return ("btree-internal");
	case WT_PAGE_BTREE_LEAF:
		return ("btree-leaf");
	default:
		break;
	}
	return ("unknown");
}

const char *
__wt_bt_hdr_flags(u_int32_t flags)
{
	switch (flags) {
	case 0:
		return ("none");
	default:
		break;
	}
	return ("unknown");
}

const char *
__wt_bt_item_type(u_int32_t type)
{
	switch (type) {
	case WT_ITEM_INTERNAL:
		return ("internal");
	case WT_ITEM_OVERFLOW:
		return ("overflow");
	case WT_ITEM_STANDARD:
		return ("standard");
	default:
		break;
	}
	return ("unknown");
}
#endif
