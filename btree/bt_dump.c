/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

/*
 * __wt_bt_dump --
 *	Dump the database.
 */
int
__wt_bt_dump(DB *db, FILE *stream, u_int32_t flags)
{
	WT_BTREE *bt;
	WT_ITEM *item;
	WT_ITEM_OVFL *ovfl;
	WT_PAGE_HDR *hdr, *ovfl_hdr;
	u_int32_t addr, frags, i;
	u_int8_t *p;
	int ret;
	void (*func)(u_int8_t *, u_int32_t, FILE *);

	bt = db->idb->btree;

	DB_FLAG_CHK(db, "Db.dump", flags, WT_APIMASK_DB_DUMP);
	func = flags == WT_PRINTABLES ? __wt_bt_print_nl : __wt_bt_hexprint;

	/* TRAVERSE TO FIRST LEAF PAGE */

	for (addr = WT_BTREE_ROOT;;) {
		if ((ret =
		    __wt_bt_fread(bt, addr, db->frags_per_page, &hdr)) != 0)
			return (ret);
		for (p = (u_int8_t *)hdr + WT_HDR_SIZE,
		    i = hdr->entries; i > 0; --i) {
			item = (WT_ITEM *)p;
			switch (item->type) {
			case WT_ITEM_STANDARD:
				func(p + sizeof(WT_ITEM), item->len, stream);
				p += WT_ITEM_SPACE_REQ(item->len);
				break;
			case WT_ITEM_OVERFLOW:
				ovfl = (WT_ITEM_OVFL *)
				    ((u_int8_t *)item + sizeof(WT_ITEM));
				WT_OVERFLOW_BYTES_TO_FRAGS(
				    db, ovfl->len, frags);
				if ((ret = __wt_bt_fread(bt,
				    ovfl->addr, frags, &ovfl_hdr)) != 0)
					return (WT_ERROR);
				func(WT_PAGE_DATA(ovfl_hdr), ovfl->len, stream);
				if ((ret = __wt_bt_fdiscard(
				    bt, ovfl->addr, ovfl_hdr)) != 0)
					return (WT_ERROR);
				p += WT_ITEM_SPACE_REQ(sizeof(WT_ITEM_OVFL));
				break;
			default:
				return (__wt_database_format(db));
			}
		}
		addr = hdr->nextaddr;
		if ((ret = __wt_bt_fdiscard(bt, addr, hdr)) != 0)
			return (ret);
		if (addr == WT_ADDR_INVALID)
			break;
	}

	return (0);
}

static const char hex[] = "0123456789abcdef";

/*
 * __wt_bt_print_nl --
 *	Output a single key/data entry in printable characters, where possible.
 *	In addition, terminate with a <newline> character, unless the entry is
 *	itself terminated with a <newline> character.
 */
void
__wt_bt_print_nl(u_int8_t *data, u_int32_t len, FILE *stream)
{
	if (data[len - 1] == '\n')
		--len;
	__wt_bt_print(data, len, stream);
	fprintf(stream, "\n");
}

/*
 * __wt_bt_print --
 *	Output a single key/data entry in printable characters, where possible.
 */
void
__wt_bt_print(u_int8_t *data, u_int32_t len, FILE *stream)
{
	int ch;

	for (; len > 0; --len, ++data) {
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
void
__wt_bt_hexprint(u_int8_t *data, u_int32_t len, FILE *stream)
{
	for (; len > 0; --len, ++data)
		fprintf(stream, "%x%x",
		    hex[(data[0] & 0xf0) >> 4], hex[data[0] & 0x0f]);
	fprintf(stream, "\n");
}
