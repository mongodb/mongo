/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static void __wt_db_hexprint(u_int8_t *, u_int32_t, FILE *);
static void __wt_db_print_nl(u_int8_t *, u_int32_t, FILE *);
static  int __wt_dup_ahead(DB *, u_int8_t *, int *);
static  int __wt_ovfl_key_copy(DB *, u_int8_t *, size_t, DBT *);

/* Check if the next page entry is part of a duplicate data set. */
#define	WT_DUP_AHEAD(p, len, yesno) {					\
	WT_ITEM *__item = (WT_ITEM *)((p) + WT_ITEM_SPACE_REQ(len));	\
	(yesno) = __item->type == WT_ITEM_DUP ||			\
	    __item->type == WT_ITEM_DUP_OVFL ||				\
	    __item->type == WT_ITEM_OFFPAGE ? 1 : 0;			\
}

/*
 * __wt_db_dump --
 *	Dump the database.
 */
int
__wt_db_dump(DB *db, FILE *stream, u_int32_t flags)
{
	DBT last_key_ovfl, last_key_std, *last_key;
	IENV *ienv;
	WT_BTREE *bt;
	WT_ITEM *item, *next_item;
	WT_ITEM_OFFP *offp;
	WT_ITEM_OVFL *ovfl;
	WT_PAGE_HDR *hdr, *ovfl_hdr;
	u_int32_t addr, frags, i;
	u_int8_t *p, *next;
	int dup_ahead, ret;
	void (*func)(u_int8_t *, u_int32_t, FILE *);

	DB_FLAG_CHK(db, "Db.dump", flags, WT_APIMASK_DB_DUMP);

	ienv = db->ienv;
	bt = db->idb->btree;
	ret = 0;
	func = flags == WT_PRINTABLES ? __wt_db_print_nl : __wt_db_hexprint;

	WT_CLEAR(last_key_std);
	WT_CLEAR(last_key_ovfl);

	/* TRAVERSE TO FIRST LEAF PAGE */

	for (addr = WT_BTREE_ROOT;;) {
		if ((ret =
		    __wt_db_fread(bt, addr, WT_FRAGS_PER_PAGE(db), &hdr)) != 0)
			return (ret);

		for (p = WT_PAGE_BYTE(hdr), i = hdr->u.entries; i > 0; --i) {
			item = (WT_ITEM *)p;
			switch (item->type) {
			case WT_ITEM_KEY:
				last_key_std.data = WT_ITEM_BYTE(item);
				last_key_std.size = item->len;
				last_key = &last_key_std;
				/*
				 * If we're about to dump an off-page duplicate
				 * set, don't write the key here, we'll write
				 * it in the off-page dump routine.
				 */
				WT_DUP_AHEAD(p, item->len, dup_ahead);
				if (!dup_ahead)
					func(WT_ITEM_BYTE(item),
					    item->len, stream);
				break;
			case WT_ITEM_DATA:
				func(p + sizeof(WT_ITEM), item->len, stream);
				break;
			case WT_ITEM_DUP:
				func(last_key->data, last_key->size, stream);
				func(p + sizeof(WT_ITEM), item->len, stream);
				break;
			case WT_ITEM_KEY_OVFL:
				/*
				 * If the overflow key has duplicate records,
				 * we'll need a copy of the key for display on
				 * each of those records.  Look ahead and see
				 * if it's a set of duplicates.
				 */
				WT_DUP_AHEAD(p,
				    sizeof(WT_ITEM_OVFL), dup_ahead);
				/* FALLTHROUGH */
			case WT_ITEM_DATA_OVFL:
			case WT_ITEM_DUP_OVFL:
				ovfl = (WT_ITEM_OVFL *)WT_ITEM_BYTE(item);
				WT_OVERFLOW_BYTES_TO_FRAGS(
				    db, ovfl->len, frags);
				if ((ret = __wt_db_fread(bt,
				    ovfl->addr, frags, &ovfl_hdr)) != 0)
					goto err;

				/*
				 * If we're already in a duplicate set, dump
				 * the key.
				 */
				if (item->type == WT_ITEM_DUP_OVFL)
					func(last_key->data,
					    last_key->size, stream);

				/*
				 * If we're starting a new duplicate set with
				 * an overflow key, save a copy of the key for
				 * later display.  Otherwise, dump this item.
				 */
				if (dup_ahead) {
					if ((ret = __wt_ovfl_key_copy(db,
					    WT_PAGE_BYTE(ovfl_hdr), ovfl->len,
					    &last_key_ovfl)) != 0)
						goto err;
					last_key = &last_key_ovfl;
					dup_ahead = 0;
				} else
					func(WT_PAGE_BYTE(ovfl_hdr),
					    ovfl->len, stream);

				if ((ret = __wt_db_fdiscard(
				    bt, ovfl->addr, ovfl_hdr)) != 0)
					goto err;
				break;
			case WT_ITEM_OFFPAGE:
				offp = (WT_ITEM_OFFP *)WT_ITEM_BYTE(item);
				if ((ret = __wt_db_dump_offpage(db,
				    last_key, offp->addr, stream, func)) != 0)
					goto err;
				break;
			default:
				return (__wt_database_format(db));
			}

			p += WT_ITEM_SPACE_REQ(item->len);
		}

		addr = hdr->nextaddr;
		if ((ret = __wt_db_fdiscard(bt, addr, hdr)) != 0)
			return (ret);
		if (addr == WT_ADDR_INVALID)
			break;
	}

	/* Discard any space allocated to hold an overflow key. */
	if (last_key_ovfl.data != NULL)
		__wt_free(ienv, last_key_ovfl.data);


	if (0) {
err:		ret = WT_ERROR;
	}
	return (ret);
}

/*
 * __wt_db_dump_offpage --
 *	Dump a set of off-page duplicates.
 */
int
__wt_db_dump_offpage(DB *db, DBT *key,
    u_int32_t addr, FILE *stream, void (*func)(u_int8_t *, u_int32_t, FILE *))
{
	WT_BTREE *bt;
	WT_ITEM *item;
	WT_ITEM_OVFL *ovfl;
	WT_PAGE_HDR *hdr, *ovfl_hdr;
	u_int32_t frags, i, next_addr;
	u_int8_t *p;
	int ret;

	bt = db->idb->btree;

	hdr = NULL;
	do {
		if ((ret =
		    __wt_db_fread(bt, addr, WT_FRAGS_PER_PAGE(db), &hdr)) != 0)
			goto err;

		for (p = WT_PAGE_BYTE(hdr), i = hdr->u.entries; i > 0; --i) {
			func(key->data, key->size, stream);
			item = (WT_ITEM *)p;
			switch (item->type) {
			case WT_ITEM_DUP:
				func(p + sizeof(WT_ITEM), item->len, stream);
				break;
			case WT_ITEM_DUP_OVFL:
				ovfl = (WT_ITEM_OVFL *)WT_ITEM_BYTE(item);
				WT_OVERFLOW_BYTES_TO_FRAGS(
				    db, ovfl->len, frags);
				if ((ret = __wt_db_fread(bt,
				    ovfl->addr, frags, &ovfl_hdr)) != 0)
					goto err;
				func(WT_PAGE_BYTE(ovfl_hdr), ovfl->len, stream);
				if ((ret = __wt_db_fdiscard(
				    bt, ovfl->addr, ovfl_hdr)) != 0)
					goto err;
				break;
			default:
				return (__wt_database_format(db));
			}
			p += WT_ITEM_SPACE_REQ(item->len);
		}

		next_addr = hdr->nextaddr;
		if ((ret = __wt_db_fdiscard(bt, addr, hdr)) != 0) {
			hdr = NULL;
			goto err;
		}
		addr = next_addr;
	} while (addr != WT_ADDR_INVALID);

	if (0) {
err:		ret = WT_ERROR;
		if (hdr != NULL)
			(void)__wt_db_fdiscard(bt, addr, hdr);
	}
	return (ret);
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
 * __wt_db_print_nl --
 *	Output a single key/data entry in printable characters, where possible.
 *	In addition, terminate with a <newline> character, unless the entry is
 *	itself terminated with a <newline> character.
 */
static void
__wt_db_print_nl(u_int8_t *data, u_int32_t len, FILE *stream)
{
	if (data[len - 1] == '\n')
		--len;
	__wt_db_print(data, len, stream);
	fprintf(stream, "\n");
}

/*
 * __wt_db_print --
 *	Output a single key/data entry in printable characters, where possible.
 */
void
__wt_db_print(u_int8_t *data, u_int32_t len, FILE *stream)
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
 * __wt_db_hexprint --
 *	Output a single key/data entry in hex.
 */
static void
__wt_db_hexprint(u_int8_t *data, u_int32_t len, FILE *stream)
{
	for (; len > 0; --len, ++data)
		fprintf(stream, "%x%x",
		    hex[(data[0] & 0xf0) >> 4], hex[data[0] & 0x0f]);
	fprintf(stream, "\n");
}
