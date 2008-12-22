/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int __wt_db_item_walk(DB *, WT_PAGE *, bitstr_t *);
static int __wt_db_ovfl_item_copy(DB *, WT_ITEM_OVFL *, DBT *);
static int __wt_db_ovfl_verify(DB *, WT_ITEM_OVFL *, bitstr_t *);

/*
 * __wt_db_verify --
 *	Verify a Btree.
 */
int
__wt_db_verify(DB *db, u_int32_t flags)
{
	IENV *ienv;
	WT_BTREE *bt;
	WT_PAGE *page;
	WT_PAGE_DESC desc;
	bitstr_t *fragbits;
	u_int32_t addr, ffc;
	int ret;

	ienv = db->ienv;
	bt = db->idb->btree;
	ret = 0;

	DB_FLAG_CHK(db, "Db.verify", flags, WT_APIMASK_DB_VERIFY);

	/*
	 * Allocate a bit array to represent the fragments in the file --
	 * that's how we keep track of the fragments we've visited and the
	 * ones yet to visit.   Storing this on the heap seems reasonable:
	 * a 16GB file of 512B frags, where we track 8 frags per allocated
	 * byte, means we allocate 4MB for the bit array.   If we have to
	 * verify larger files than we can track this way, we'd have to write
	 * parts of the bit array into a file somewhere.
	 */
	if ((ret = bit_alloc(ienv, bt->frags, &fragbits)) != 0)
		return (ret);

	/*
	 * Walk the leaf pages first, verifying each one and storing the
	 * information we'll later use to verify relationships between
	 * different parts of the tree.
	 */
	for (addr = WT_ADDR_FIRST_PAGE;;) {
		if ((ret = __wt_db_fread(
		    db, addr, WT_FRAGS_PER_PAGE(db), &page, 0)) != 0)
			goto err;
		if ((ret = __wt_db_page_verify(db, page, fragbits)) != 0)
			goto err;
		addr = page->hdr->nextaddr;
		if ((ret = __wt_db_fdiscard(db, page)) != 0)
			goto err;
		if (addr == WT_ADDR_INVALID)
			break;
	}

	/*
	 * Walk the internal pages next, verify each one.
	 *
	 * Start by finding the root page.
	 */
	if ((ret = __wt_db_desc_read(db, &desc)) != 0)
		goto err;

	/* Check for pages fragments we haven't looked at. */
	bit_ffc(fragbits, bt->frags, &ffc);
	if (ffc != 0)
		printf("clear: %lu\n", (u_long)ffc);

err:	__wt_free(ienv, fragbits);
	return (ret);
}

/*
 * __wt_db_page_verify --
 *	Verify a single Btree page.
 */
int
__wt_db_page_verify(DB *db, WT_PAGE *page, bitstr_t *fragbits)
{
	WT_PAGE_HDR *hdr;
	u_int32_t addr;
	int ret;

	hdr = page->hdr;
	addr = page->addr;

	/*
	 * FUTURE:
	 * Check the LSN against the existing log files.
	 */

	switch (hdr->type) {
	case WT_PAGE_OVFL:
		if (hdr->u.entries == 0) {
			__wt_db_errx(db,
			    "page at addr %lu has no entries", (u_long)addr);
			ret = WT_ERROR;
		}
		/* FALLTHROUGH */
	case WT_PAGE_INT:
	case WT_PAGE_LEAF:
	case WT_PAGE_DUP_INT:
	case WT_PAGE_DUP_LEAF:
		break;
	default:
		__wt_db_errx(db,
		    "page at addr %lu has an invalid type of %lu",
		    (u_long)addr, (u_long)hdr->type);
		return (WT_ERROR);
	}

	if (hdr->unused[0] != '\0' ||
	    hdr->unused[1] != '\0' || hdr->unused[2] != '\0') {
		__wt_db_errx(db,
		    "page at addr %lu has non-zero unused header fields",
		    (u_long)addr);
		return (WT_ERROR);
	}

	/* The checksum was verified when we first read the page. */

	/* If verifying the entire tree, mark the fragments this page covers. */
	if (fragbits != NULL)
		bit_nset(fragbits, addr, addr + page->frags);

	/* Verify the items on the page. */
	if (hdr->type != WT_PAGE_OVFL &&
	    (ret = __wt_db_item_walk(db, page, fragbits)) != 0)
		return (ret);

	return (0);
}

/*
 * __wt_db_item_walk --
 *	Walk the items on a page and verify them.
 */
static int
__wt_db_item_walk(DB *db, WT_PAGE *page, bitstr_t *fragbits)
{
	struct {
		u_int32_t indx;			/* Item number */

		DBT *item;			/* Item to compare */
		DBT item_ovfl;			/* Overflow holder */
		DBT item_std;			/* On-page reference */
	} *current, *last_data, *last_key, *swap_tmp, _a, _b, _c;
	IENV *ienv;
	WT_ITEM *item;
	WT_PAGE_HDR *hdr;
	u_int8_t *end;
	u_int32_t addr, i;
	int (*func)(DB *, const DBT *, const DBT *), ret;

	ienv = db->ienv;
	hdr = page->hdr;
	addr = page->addr;
	ret = 0;

	/*
	 * We have 3 key/data items we track -- the last key, the last data
	 * item, and the current item.   They're stored in the _a, _b, and
	 * _c structures (it doesn't matter which) -- what matters is which
	 * item is referenced by current, last_data or last_key.
	 */
	_a.item = _b.item = _c.item = NULL;
	WT_CLEAR(_a.item_ovfl);
	WT_CLEAR(_a.item_std);
	WT_CLEAR(_b.item_ovfl);
	WT_CLEAR(_b.item_std);
	WT_CLEAR(_c.item_ovfl);
	WT_CLEAR(_c.item_std);
	current = &_a;
	last_data = &_b;
	last_key = &_c;

	/* Set the comparison function. */
	if (hdr->type == WT_PAGE_DUP_INT ||
	    hdr->type == WT_PAGE_DUP_LEAF)
		func = db->dup_compare;
	else
		func = db->btree_compare;

	end = (u_int8_t *)hdr + db->pagesize;
	for (item = (WT_ITEM *)page->first_data, i = 1;
	    i <= hdr->u.entries;
	    item = (WT_ITEM *)
	    ((u_int8_t *)item + WT_ITEM_SPACE_REQ(item->len)), ++i) {
		/* Check if this item is entirely on the page. */
		if ((u_int8_t *)item + sizeof(WT_ITEM) > end)
			goto eop;

		/* Check the item's type. */
		switch (item->type) {
		case WT_ITEM_KEY:
		case WT_ITEM_KEY_OVFL:
			if (hdr->type != WT_PAGE_LEAF &&
			    hdr->type != WT_PAGE_INT)
				goto item_vs_page;
			break;
		case WT_ITEM_DATA:
		case WT_ITEM_DATA_OVFL:
		case WT_ITEM_OFFPAGE:
			if (hdr->type != WT_PAGE_LEAF &&
			    hdr->type != WT_PAGE_INT)
				goto item_vs_page;
			break;
		case WT_ITEM_DUP:
		case WT_ITEM_DUP_OVFL:
			if (hdr->type != WT_PAGE_LEAF &&
			    hdr->type != WT_PAGE_DUP_INT &&
			    hdr->type != WT_PAGE_DUP_LEAF) {
item_vs_page:			__wt_db_errx(db,
				    "item %lu on page at addr %lu is a %s "
				    "type on a %s page",
				    (u_long)i, (u_long)addr,
				    __wt_db_item_type(item->type),
				    __wt_db_hdr_type(hdr->type));
				goto err;
			}
			break;
		default:
			goto item_type;
		}

		/* Check the item's length. */
		switch (item->type) {
		case WT_ITEM_KEY:
		case WT_ITEM_DATA:
		case WT_ITEM_DUP:
			/* The length is variable, so we can't check it. */
			break;
		case WT_ITEM_KEY_OVFL:
		case WT_ITEM_DATA_OVFL:
		case WT_ITEM_DUP_OVFL:
			if (item->len != sizeof(WT_ITEM_OVFL))
				goto item_len;
			break;
		case WT_ITEM_OFFPAGE:
			if (item->len != sizeof(WT_ITEM_OFFP)) {
item_len:			__wt_db_errx(db,
				    "item %lu on page at addr %lu has an "
				    "incorrect length",
				    (u_long)i, (u_long)addr);
				goto err;
			}
			break;
		default:
			goto item_type;
		}

		/* Check the unused fields. */
		if (item->unused[0] != 0 || item->unused[1] != 0) {
			__wt_db_errx(db,
			    "item %lu on page at addr %lu has non-zero "
			    "unused item fields", 
			    (u_long)i, (u_long)addr);
			goto err;
		}

		/* Check if the item's data is entirely on the page. */
		if ((u_int8_t *)item + WT_ITEM_SPACE_REQ(item->len) > end) {
eop:			__wt_db_errx(db,
			    "item %lu on page at addr %lu extends past the end "
			    " of the page",
			    (u_long)i, (u_long)addr);
			goto err;
		}

		/* When walking the whole file, verify overflow page refs. */
		if (fragbits != NULL &&
		    (item->type == WT_ITEM_KEY_OVFL ||
		    item->type == WT_ITEM_DATA_OVFL ||
		    item->type == WT_ITEM_DUP_OVFL) &&
		    (ret = __wt_db_ovfl_verify(db,
		    (WT_ITEM_OVFL *)WT_ITEM_BYTE(item), fragbits)) != 0)
			goto err;

		/* Some items aren't sorted on the page, so we're done. */
		if (item->type == WT_ITEM_DATA ||
		    item->type == WT_ITEM_DATA_OVFL ||
		    item->type == WT_ITEM_OFFPAGE)
			continue;

		/* Get a DBT that represents this item. */
		switch (item->type) {
		case WT_ITEM_KEY:
		case WT_ITEM_DUP:
			current->indx = i;
			current->item = &current->item_std;
			current->item_std.data = WT_ITEM_BYTE(item);
			current->item_std.size = item->len;
			break;
		case WT_ITEM_KEY_OVFL:
		case WT_ITEM_DUP_OVFL:
			current->indx = i;
			current->item = &current->item_ovfl;
			if ((ret = __wt_db_ovfl_item_copy(db, (WT_ITEM_OVFL *)
			    WT_ITEM_BYTE(item), current->item)) != 0)
				goto err;
			break;
		default:
			goto item_type;
		}

		/* Check the sort order. */
		switch (item->type) {
		case WT_ITEM_KEY:
		case WT_ITEM_KEY_OVFL:
			if (last_key->item != NULL &&
			    func(db, last_key->item, current->item) >= 0) {
				__wt_db_errx(db,
				    "item %lu and item %lu on page at addr %lu "
				    "are incorrectly sorted", 
				    last_key->indx, current->indx,
				    (u_long)addr);
				goto err;
			}
			swap_tmp = last_key;
			last_key = current;
			current = swap_tmp;
			break;
		case WT_ITEM_DUP:
		case WT_ITEM_DUP_OVFL:
			if (last_data->item != NULL &&
			    func(db, last_data->item, current->item) >= 0) {
				__wt_db_errx(db,
				    "item %lu and item %lu on page at addr %lu "
				    "are incorrectly sorted", 
				    last_data->indx, current->indx,
				    (u_long)addr);
				goto err;
			}
			swap_tmp = last_data;
			last_data = current;
			current = swap_tmp;
			break;
		default:
			goto item_type;
		}
	}

	if (0) {
item_type:	__wt_db_errx(db,
		    "item %lu on page at addr %lu has an illegal type of %lu", 
		    (u_long)i, (u_long)addr, (u_long)item->type);
err:		ret = WT_ERROR;
	}

	if (_a.item_ovfl.data != NULL)
		__wt_free(ienv, _a.item_ovfl.data);
	if (_b.item_ovfl.data != NULL)
		__wt_free(ienv, _b.item_ovfl.data);
	if (_c.item_ovfl.data != NULL)
		__wt_free(ienv, _c.item_ovfl.data);

	return (ret);
}

/*
 * __wt_db_ovfl_verify --
 *	Verify an overflow item.
 */
static int
__wt_db_ovfl_verify(DB *db, WT_ITEM_OVFL *ovfl, bitstr_t *fragbits)
{
	WT_PAGE *ovfl_page;
	u_int32_t frags;
	int ret, tret;

	WT_OVERFLOW_BYTES_TO_FRAGS(db, ovfl->len, frags);
	if ((ret = __wt_db_fread(db, ovfl->addr, frags, &ovfl_page, 0)) != 0)
		return (ret);

	ret = __wt_db_page_verify(db, ovfl_page, fragbits);

	if ((tret = __wt_db_fdiscard(db, ovfl_page)) != 0 && ret == 0)
		ret = tret;

	return (ret);
}

/*
 * __wt_db_ovfl_item_copy --
 *	Copy an overflow item into a DBT.
 */
static int
__wt_db_ovfl_item_copy(DB *db, WT_ITEM_OVFL *ovfl, DBT *copy)
{
	WT_PAGE *ovfl_page;
	u_int32_t frags, len;
	int ret, tret;

	ret = 0;

	len = ovfl->len;
	WT_OVERFLOW_BYTES_TO_FRAGS(db, len, frags);
	if ((ret = __wt_db_fread(db, ovfl->addr, frags, &ovfl_page, 0)) != 0)
		return (ret);

	if (copy->data == NULL || copy->alloc_size < len) {
		if ((ret = __wt_realloc(db->ienv, len, &copy->data)) != 0)
			goto err;
		copy->alloc_size = len;
	}
	memcpy(copy->data, WT_PAGE_BYTE(ovfl_page), copy->size = len);

err:	if ((tret = __wt_db_fdiscard(db, ovfl_page)) != 0 && ret == 0)
		ret = tret;

	return (ret);
}
