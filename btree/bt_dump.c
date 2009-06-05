/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int  __wt_bt_dump_offpage(DB *, DBT *,
    WT_ITEM *, FILE *, void (*)(u_int8_t *, u_int32_t, FILE *));
static void __wt_bt_hexprint(u_int8_t *, u_int32_t, FILE *);
static void __wt_bt_print_nl(u_int8_t *, u_int32_t, FILE *);

/* Check if the next page entry is part of a duplicate data set. */
#define	WT_DUP_AHEAD(item, yesno) {					\
	u_int32_t __type = WT_ITEM_TYPE(WT_ITEM_NEXT(item));		\
	(yesno) = __type == WT_ITEM_DUP ||				\
	    __type == WT_ITEM_DUP_OVFL ||				\
	    __type == WT_ITEM_OFFP_INTL ||				\
	    __type == WT_ITEM_OFFP_LEAF ? 1 : 0;			\
}

/*
 * __wt_db_dump --
 *	Db.dump method.
 */
int
__wt_db_dump(WT_TOC *toc)
{
	wt_args_db_dump_unpack;
	DBT last_key_ovfl, last_key_std, *last_key;
	ENV *env;
	WT_ITEM *item;
	WT_ITEM_OVFL *ovfl;
	WT_PAGE *page, *ovfl_page;
	u_int32_t addr, i, item_len;
	int dup_ahead, ret;
	void (*func)(u_int8_t *, u_int32_t, FILE *);

	env = toc->env;

	WT_DB_FCHK(db, "Db.dump", flags, WT_APIMASK_DB_DUMP);

	if (LF_ISSET(WT_DEBUG)) {
#ifdef HAVE_DIAGNOSTIC
		return (__wt_bt_dump_debug(db, NULL, stream));
#else
		__wt_db_errx(db, "library not built for debugging");
		return (WT_ERROR);
#endif
	}

	dup_ahead = ret = 0;
	func = flags == WT_PRINTABLES ? __wt_bt_print_nl : __wt_bt_hexprint;

	/*lint -esym(644,last_key)
	 * LINT complains last_key may be used before being set -- that's not
	 * true, on any well-formed page, last_key will be set by encountering
	 * an item of type WT_ITEM_KEY/KEY_OVFL before anything else.
	 */
	WT_CLEAR(last_key_std);
	WT_CLEAR(last_key_ovfl);

	for (addr = WT_ADDR_FIRST_PAGE;;) {
		WT_RET((__wt_bt_page_in(db, addr, 1, 0, &page)));

		WT_ITEM_FOREACH(page, item, i) {
			item_len = WT_ITEM_LEN(item);
			switch (WT_ITEM_TYPE(item)) {
			case WT_ITEM_KEY:
				last_key_std.data = WT_ITEM_BYTE(item);
				last_key_std.size = item_len;
				last_key = &last_key_std;
				/*
				 * If we're about to dump an off-page duplicate
				 * set, don't write the key here, we'll write
				 * it in the off-page dump routine.
				 */
				WT_DUP_AHEAD(item, dup_ahead);
				if (!dup_ahead)
					func(WT_ITEM_BYTE(item),
					    item_len, stream);
				break;
			case WT_ITEM_DATA:
				func(WT_ITEM_BYTE(item), item_len, stream);
				break;
			case WT_ITEM_DUP:
				func(last_key->data, last_key->size, stream);
				func(WT_ITEM_BYTE(item), item_len, stream);
				break;
			case WT_ITEM_KEY_OVFL:
				/*
				 * If the overflow key has duplicate records,
				 * we'll need a copy of the key for display on
				 * each of those records.  Look ahead and see
				 * if it's a set of duplicates.
				 */
				WT_DUP_AHEAD(item, dup_ahead);
				/* FALLTHROUGH */
			case WT_ITEM_DATA_OVFL:
			case WT_ITEM_DUP_OVFL:
				ovfl = (WT_ITEM_OVFL *)WT_ITEM_BYTE(item);
				WT_RET((__wt_bt_ovfl_in(db,
				    ovfl->addr, ovfl->len, &ovfl_page)));

				/*
				 * If we're already in a duplicate set, dump
				 * the key.
				 */
				if (WT_ITEM_TYPE(item) == WT_ITEM_DUP_OVFL)
					func(last_key->data,
					    last_key->size, stream);

				/*
				 * If we're starting a new duplicate set with
				 * an overflow key, save a copy of the key for
				 * later display.  Otherwise, dump this item.
				 */
				if (dup_ahead) {
					WT_RET((__wt_bt_data_copy_to_dbt(db,
					    WT_PAGE_BYTE(ovfl_page), ovfl->len,
					    &last_key_ovfl)));
					last_key = &last_key_ovfl;
					dup_ahead = 0;
				} else
					func(WT_PAGE_BYTE(ovfl_page),
					    ovfl->len, stream);

				WT_RET((__wt_bt_page_out(db, ovfl_page, 0)));
				break;
			case WT_ITEM_OFFP_INTL:
			case WT_ITEM_OFFP_LEAF:
				WT_RET((__wt_bt_dump_offpage(
				    db, last_key, item, stream, func)));
				break;
			WT_DEFAULT_FORMAT(db);
			}
		}

		addr = page->hdr->nextaddr;
		WT_RET((__wt_bt_page_out(db, page, 0)));
		if (addr == WT_ADDR_INVALID)
			break;
	}

	/* Discard any space allocated to hold an overflow key. */
	WT_FREE_AND_CLEAR(env, last_key_ovfl.data);

	return (ret);
}

/*
 * __wt_bt_dump_offpage --
 *	Dump a set of off-page duplicates.
 */
static int
__wt_bt_dump_offpage(DB *db, DBT *key, WT_ITEM *item,
    FILE *stream, void (*func)(u_int8_t *, u_int32_t, FILE *))
{
	WT_ITEM_OVFL *ovfl;
	WT_PAGE *page, *ovfl_page;
	u_int32_t addr, i;
	int isleaf, ret;

	page = NULL;

	/*
	 * Callers pass us a reference to an on-page WT_ITEM_OFFP_INTL/LEAF.
	 *
	 * We need to know what kind of page we're getting, use the item type.
	 */
	addr = ((WT_ITEM_OFFP *)WT_ITEM_BYTE(item))->addr;
	isleaf = WT_ITEM_TYPE(item) == WT_ITEM_OFFP_LEAF ? 1 : 0;

	/* Walk down the duplicates tree to the first leaf page. */
	for (;;) {
		WT_RET((__wt_bt_page_in(db, addr, isleaf, 0, &page)));
		if (isleaf)
			break;

		/* Get the page's first WT_ITEM_OFFP. */
		__wt_bt_first_offp(page, &addr, &isleaf);

		if ((ret = __wt_bt_page_out(db, page, 0)) != 0) {
			page = NULL;
			goto err;
		}
	}

	for (;;) {
		WT_ITEM_FOREACH(page, item, i) {
			func(key->data, key->size, stream);
			switch (WT_ITEM_TYPE(item)) {
			case WT_ITEM_DUP:
				func(WT_ITEM_BYTE(item),
				    WT_ITEM_LEN(item), stream);
				break;
			case WT_ITEM_DUP_OVFL:
				ovfl = (WT_ITEM_OVFL *)WT_ITEM_BYTE(item);
				WT_ERR((__wt_bt_ovfl_in(db,
				    ovfl->addr, ovfl->len, &ovfl_page)));
				func(
				    WT_PAGE_BYTE(ovfl_page), ovfl->len, stream);
				WT_ERR((__wt_bt_page_out(db, ovfl_page, 0)));
				break;
			WT_DEFAULT_FORMAT(db);
			}
		}

		addr = page->hdr->nextaddr;
		if ((ret = __wt_bt_page_out(db, page, 0)) != 0) {
			page = NULL;
			goto err;
		}
		if (addr == WT_ADDR_INVALID)
			break;

		WT_ERR((__wt_bt_page_in(db, addr, 1, 0, &page)));
	}

	if (0) {
err:		if (page != NULL)
			(void)__wt_bt_page_out(db, page, 0);
	}
	return (ret);
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
