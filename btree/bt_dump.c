/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static void __wt_bt_hexprint(u_int8_t *, u_int32_t, FILE *);
static void __wt_bt_print_nl(u_int8_t *, u_int32_t, FILE *);
static  int __wt_dup_ahead(DB *, u_int8_t *, int *);
static  int __wt_ovfl_key_copy(DB *, u_int8_t *, size_t, DBT *);

/*
 * __wt_db_dump --
 *	Dump the database.
 */
int
__wt_db_dump(DB *db, FILE *stream, u_int32_t flags)
{
	DBT last_ovfl_key;
	WT_BTREE *bt;
	WT_ITEM *item, *next_item, *last_key_item;
	WT_ITEM_OVFL *ovfl;
	WT_PAGE_HDR *hdr, *ovfl_hdr;
	u_int32_t addr, frags, i;
	u_int8_t *p, *next;
	int need_key_copy, ret;
	void (*func)(u_int8_t *, u_int32_t, FILE *);

	bt = db->idb->btree;

	DB_FLAG_CHK(db, "Db.dump", flags, WT_APIMASK_DB_DUMP);

	WT_CLEAR(last_ovfl_key);
	ret = 0;
	func = flags == WT_PRINTABLES ? __wt_bt_print_nl : __wt_bt_hexprint;

	/* TRAVERSE TO FIRST LEAF PAGE */

	for (addr = WT_BTREE_ROOT;;) {
		if ((ret =
		    __wt_bt_fread(bt, addr, WT_FRAGS_PER_PAGE(db), &hdr)) != 0)
			return (ret);

		for (p = WT_PAGE_DATA(hdr), i = hdr->u.entries; i > 0; --i) {
			item = (WT_ITEM *)p;
			switch (item->type) {
			case WT_ITEM_KEY:
				last_key_item = item;
				/* FALLTHROUGH */
			case WT_ITEM_DATA:
				func(p + sizeof(WT_ITEM), item->len, stream);
				p += WT_ITEM_SPACE_REQ(item->len);
				break;
			case WT_ITEM_KEY_OVFL:
				/*
				 * If the overflow key has duplicate records,
				 * we'll need a copy of the key for display on
				 * each of those records.  Look ahead and see
				 * if duplicate data items follow the first
				 * data item.
				 */
				last_key_item = NULL;
				need_key_copy = 0;
				if (i > 2 && (ret =
				    __wt_dup_ahead(db, p, &need_key_copy)) != 0)
					goto err;

				/* FALLTHROUGH */
			case WT_ITEM_DATA_OVFL:
			case WT_ITEM_DUP_OVFL:
				ovfl = (WT_ITEM_OVFL *)
				    ((u_int8_t *)item + sizeof(WT_ITEM));
				WT_OVERFLOW_BYTES_TO_FRAGS(
				    db, ovfl->len, frags);
				if ((ret = __wt_bt_fread(bt,
				    ovfl->addr, frags, &ovfl_hdr)) != 0)
					goto err;

				/*
				 * Save a copy of this overflow key if it's
				 * needed for later duplicate data items.
				 */
				if (need_key_copy) {
					if ((ret = __wt_ovfl_key_copy(db,
					    WT_PAGE_DATA(ovfl_hdr), ovfl->len,
					    &last_ovfl_key)) != 0)
						goto err;
					need_key_copy = 0;
				}

				func(WT_PAGE_DATA(ovfl_hdr), ovfl->len, stream);

				if ((ret = __wt_bt_fdiscard(
				    bt, ovfl->addr, ovfl_hdr)) != 0)
					goto err;
				p += WT_ITEM_SPACE_REQ(sizeof(WT_ITEM_OVFL));
				break;
			case WT_ITEM_DUP:
				/* Display the previous key. */
				if (last_key_item == NULL)
					func(last_ovfl_key.data,
					    last_ovfl_key.size, stream);
				else
					func((u_int8_t *)last_key_item +
					    sizeof(WT_ITEM),
					    last_key_item->len, stream);
					
				/* Display the data item. */
				func(p + sizeof(WT_ITEM), item->len, stream);
				p += WT_ITEM_SPACE_REQ(item->len);
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


	if (0) {
err:		ret = WT_ERROR;
	}
	return (ret);
}

/*
 * __wt_dup_ahead --
 *	Check to see if there's a duplicate data item coming up.
 */
static int
__wt_dup_ahead(DB *db, u_int8_t *p, int *yesnop)
{
	WT_ITEM *item;
	
	p += WT_ITEM_SPACE_REQ(sizeof(WT_ITEM_OVFL));
	item = (WT_ITEM *)p;
	switch (item->type) {
	case WT_ITEM_DATA:
		p += WT_ITEM_SPACE_REQ(item->len);
		break;
	case WT_ITEM_DATA_OVFL:
		p += WT_ITEM_SPACE_REQ(sizeof(WT_ITEM_OVFL));
		break;
	default:
		return (__wt_database_format(db));
	}
	item = (WT_ITEM *)p;
	*yesnop =
	    item->type == WT_ITEM_DUP || item->type == WT_ITEM_DUP_OVFL ? 1 : 0;
	return (0);
}

/*
 * __wt_ovfl_key_copy --
 *	Copy an overflow key into a DBT.
 */
static int
__wt_ovfl_key_copy(DB *db, u_int8_t *data, size_t len, DBT *copy)
{
	int ret;

	if (copy->data == NULL || copy->alloc_size < len) {
		if ((ret = __wt_realloc(db->ienv, len, &copy->data)) != 0)
			return (ret);
		copy->alloc_size = len;
	}
	memcpy(copy->data, data, copy->size = len);

	return (0);
}

static const char hex[] = "0123456789abcdef";

/*
 * __wt_bt_print_nl --
 *	Output a single key/data entry in printable characters, where possible.
 *	In addition, terminate with a <newline> character, unless the entry is
 *	itself terminated with a <newline> character.
 */
static void
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
static void
__wt_bt_hexprint(u_int8_t *data, u_int32_t len, FILE *stream)
{
	for (; len > 0; --len, ++data)
		fprintf(stream, "%x%x",
		    hex[(data[0] & 0xf0) >> 4], hex[data[0] & 0x0f]);
	fprintf(stream, "\n");
}
