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
static int __wt_db_verify_checkfrag(DB *, bitstr_t *);
static int __wt_db_verify_level(DB *, u_int32_t, bitstr_t *);
static int __wt_db_verify_ovfl(DB *, WT_ITEM_OVFL *, bitstr_t *);

/*
 * __wt_db_verify --
 *	Verify a Btree.
 */
int
__wt_db_verify(DB *db, u_int32_t flags)
{
	IDB *idb;
	IENV *ienv;
	WT_PAGE *page;
	WT_PAGE_DESC desc;
	bitstr_t *fragbits;
	int ret;

	ienv = db->ienv;
	idb = db->idb;
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
	 *
	 * !!!
	 * There's one portability issue -- the bitstring package uses "ints",
	 * not unsigned ints, or any fixed size.   If an "int" can't hold a
	 * big enough value, then we can lose.   There's a check here just
	 * to make sure we don't overflow.   I don't ever expect to see this
	 * error message, but better safe than sorry.
	 */
	if (idb->frags > INT_MAX) {
		__wt_db_errx(db, "file has too many fragments to verify");
		return (WT_ERROR);
	}
	if ((ret = bit_alloc(ienv, idb->frags, &fragbits)) != 0)
		return (ret);

	/* Get the root and walk the tree. */
	if ((ret = __wt_db_desc_read(db, &desc)) != 0)
		goto err;

	/* If the root address has never been set, it's a one-page database. */
	if (desc.root_addr == WT_ADDR_INVALID) {
		if ((ret = __wt_db_page_in(db,
		    WT_ADDR_FIRST_PAGE, WT_FRAGS_PER_PAGE(db), &page, 0)) != 0)
			goto err;
		if ((ret = __wt_db_verify_page(db, page, fragbits)) != 0)
			goto err;
		if ((ret = __wt_db_page_out(db, page, 0)) != 0)
			goto err;
	} else
		if ((ret =
		    __wt_db_verify_level(db, desc.root_addr, fragbits)) != 0)
			goto err;

	ret = __wt_db_verify_checkfrag(db, fragbits);

err:	__wt_free(ienv, fragbits);
	return (ret);
}

/*
 * __wt_db_verify_level --
 *	Verify one level of a tree.
 */
static int
__wt_db_verify_level(DB *db, u_int32_t addr, bitstr_t *fragbits)
{
	WT_PAGE *page;
	u_int32_t descend_addr;
	int first, ret, tret;

	descend_addr = WT_ADDR_INVALID;

	/*
	 * Verify a level of the tree -- we are passed the address of the
	 * left-most page in the level.   We do it this way because while
	 * I don't mind bouncing around the tree for the internal pages,
	 * I want to read the leaf pages in contiguous order.
	 */
	for (first = 1, page = NULL; addr != WT_ADDR_INVALID;) {
		if ((ret = __wt_db_page_in(
		    db, addr, WT_FRAGS_PER_PAGE(db), &page, 0)) != 0)
			return (ret);

		if (first) {
			/*
			 * We'll want to descend to the first offpage in this
			 * level, save the address for later.
			 */
			first = 0;
			if (page->hdr->type == WT_PAGE_INT ||
			    page->hdr->type == WT_PAGE_DUP_INT)
				__wt_first_offp_addr(page, &descend_addr);
		}

		ret = __wt_db_verify_page(db, page, fragbits);

		addr = page->hdr->nextaddr;

		if ((tret = __wt_db_page_out(db, page, 0)) != 0 && ret == 0)
			ret = tret;

		if (ret != 0)
			return (ret);
	}

	if (descend_addr != WT_ADDR_INVALID)
		return (__wt_db_verify_level(db, descend_addr, fragbits));

	return (0);
}

/*
 * __wt_db_verify_page --
 *	Verify a single Btree page.
 */
int
__wt_db_verify_page(DB *db, WT_PAGE *page, bitstr_t *fragbits)
{
	WT_PAGE_HDR *hdr;
	u_int32_t addr;
	int ret;

	hdr = page->hdr;
	addr = page->addr;

	/*
	 * If we're verifying the whole tree, complain if there's a page
	 * we've already verified.
	 */
	if (fragbits != NULL)
		if (bit_test(fragbits, addr))
			__wt_db_errx(db,
			    "page at addr %lu already verified", (u_long)addr);
		else
			bit_nset(fragbits, addr, addr + (page->frags - 1));

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

	/*
	 * Don't verify the checksum -- it verified when we first read the
	 * page.
	 */

	/* Page 0 has a descriptor record. */
	if (addr == WT_ADDR_FIRST_PAGE &&
	    (ret = __wt_db_desc_verify(db, page)) != 0)
		return (ret);

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
	u_int32_t addr, i, item_no;
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
	item_no = 0;
	WT_ITEM_FOREACH(page, item, i) {
		++item_no;

		/* Check if this item is entirely on the page. */
		if ((u_int8_t *)item + sizeof(WT_ITEM) > end)
			goto eop;

		/* Check the item's type. */
		switch (item->type) {
		case WT_ITEM_KEY:
		case WT_ITEM_KEY_OVFL:
			if (hdr->type != WT_PAGE_INT &&
			    hdr->type != WT_PAGE_LEAF &&
			    hdr->type != WT_PAGE_DUP_INT)
				goto item_vs_page;
			break;
		case WT_ITEM_DATA:
		case WT_ITEM_DATA_OVFL:
			if (hdr->type != WT_PAGE_LEAF)
				goto item_vs_page;
			break;
		case WT_ITEM_DUP:
		case WT_ITEM_DUP_OVFL:
			if (hdr->type != WT_PAGE_LEAF &&
			    hdr->type != WT_PAGE_DUP_LEAF)
				goto item_vs_page;
			break;
		case WT_ITEM_OFFPAGE:
			if (hdr->type != WT_PAGE_INT &&
			    hdr->type != WT_PAGE_LEAF &&
			    hdr->type != WT_PAGE_DUP_INT) {
item_vs_page:			__wt_db_errx(db,
				    "item %lu on page at addr %lu is a %s "
				    "type on a %s page",
				    (u_long)item_no, (u_long)addr,
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
				    (u_long)item_no, (u_long)addr);
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
			    (u_long)item_no, (u_long)addr);
			goto err;
		}

		/* Check if the item's data is entirely on the page. */
		if ((u_int8_t *)WT_ITEM_NEXT(item) > end) {
eop:			__wt_db_errx(db,
			    "item %lu on page at addr %lu extends past the end "
			    " of the page",
			    (u_long)item_no, (u_long)addr);
			goto err;
		}

		/*
		 * When walking the whole file, verify off-page duplicate sets
		 * and overflow page references.
		 */
		if (fragbits != NULL) {
			if (item->type == WT_ITEM_OFFPAGE &&
			    page->hdr->type == WT_PAGE_LEAF &&
			    (ret = __wt_db_verify_level(db,
			    ((WT_ITEM_OFFP *)WT_ITEM_BYTE(item))->addr,
			    fragbits)) != 0)
				goto err;

			if ((item->type == WT_ITEM_KEY_OVFL ||
			    item->type == WT_ITEM_DATA_OVFL ||
			    item->type == WT_ITEM_DUP_OVFL) &&
			    (ret = __wt_db_verify_ovfl(db,
			    (WT_ITEM_OVFL *)WT_ITEM_BYTE(item), fragbits)) != 0)
				goto err;
		}

		/* Some items aren't sorted on the page, so we're done. */
		if (item->type == WT_ITEM_DATA ||
		    item->type == WT_ITEM_DATA_OVFL ||
		    item->type == WT_ITEM_OFFPAGE)
			continue;

		/* Get a DBT that represents this item. */
		switch (item->type) {
		case WT_ITEM_KEY:
		case WT_ITEM_DUP:
			current->indx = item_no;
			current->item = &current->item_std;
			current->item_std.data = WT_ITEM_BYTE(item);
			current->item_std.size = item->len;
			break;
		case WT_ITEM_KEY_OVFL:
		case WT_ITEM_DUP_OVFL:
			current->indx = item_no;
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
		    (u_long)item_no, (u_long)addr, (u_long)item->type);
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
 * __wt_db_verify_ovfl --
 *	Verify an overflow item.
 */
static int
__wt_db_verify_ovfl(DB *db, WT_ITEM_OVFL *ovfl, bitstr_t *fragbits)
{
	WT_PAGE *ovfl_page;
	u_int32_t frags;
	int ret, tret;

	WT_OVERFLOW_BYTES_TO_FRAGS(db, ovfl->len, frags);
	if ((ret = __wt_db_page_in(db, ovfl->addr, frags, &ovfl_page, 0)) != 0)
		return (ret);

	ret = __wt_db_verify_page(db, ovfl_page, fragbits);

	if ((tret = __wt_db_page_out(db, ovfl_page, 0)) != 0 && ret == 0)
		ret = tret;

	return (ret);
}

/*
 * __wt_db_verify_checkfrag --
 *	Verify we've checked all the fragments in the file.
 */
static int
__wt_db_verify_checkfrag(DB *db, bitstr_t *fragbits)
{
	IDB *idb;
	int ffc, ffc_start, ffc_end, ret;

	idb = db->idb;
	ret = 0;

	/* Check for page fragments we haven't verified. */
	for (ffc_start = ffc_end = -1;;) {
		bit_ffc(fragbits, idb->frags, &ffc);
		if (ffc != -1) {
			bit_set(fragbits, ffc);
			if (ffc_start == -1) {
				ffc_start = ffc_end = ffc;
				continue;
			}
			if (ffc_end == ffc - 1) {
				ffc_end = ffc;
				continue;
			}
		}
		if (ffc_start != -1) {
			__wt_db_errx(db,
			    "fragments %d to %d were never verified",
			    ffc_start, ffc_end);
			ret = WT_ERROR;
		}
		ffc_start = ffc_end = ffc;
		if (ffc == -1)
			break;
	}
	return (ret);
}
