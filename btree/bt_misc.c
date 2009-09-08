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
 * __wt_bt_build_verify --
 *	Verify the Btree build itself.
 */
int
__wt_bt_build_verify(void)
{
	static struct {
		u_int s, c;
		char *name;
	} list[] = {
		{ sizeof(WT_ITEM), WT_ITEM_SIZE, "WT_ITEM" },
		{ sizeof(WT_PAGE_DESC), WT_PAGE_DESC_SIZE, "WT_PAGE_DESC" },
		{ sizeof(WT_PAGE_HDR), WT_PAGE_HDR_SIZE, "WT_PAGE_HDR" },
		{ sizeof(WT_ITEM_OFFP), WT_ITEM_OFFP_SIZE, "WT_ITEM_OFFP" },
		{ sizeof(WT_ITEM_OVFL), WT_ITEM_OVFL_SIZE, "WT_ITEM_OVFL" }
	}, *lp;
		
	/*
	 * The compiler had better not have padded our structures -- make
	 * sure the page header structure is exactly what we expect.
	 */
	for (lp = list; lp < list + sizeof(list) / sizeof(list[0]); ++lp) {
		if (lp->s == lp->c)
			continue;
		__wt_env_errx(NULL,
		    "WiredTiger build failed, the %s header structure is not "
		    "the correct size (expected %u, got %u)",
		    lp->name, lp->c, lp->s);
		return (WT_ERROR);
	}
	if (WT_ALIGN(
	    sizeof(WT_PAGE_HDR), sizeof(u_int32_t)) != WT_PAGE_HDR_SIZE) {
		__wt_env_errx(NULL,
		    "Build verification failed, the WT_PAGE_HDR structure"
		    " isn't aligned correctly");
		return (WT_ERROR);
	}

	/*
	 * We mix-and-match 32-bit unsigned values and size_t's, mostly because
	 * we allocate and handle 32-bit objects, and lots of the underlying C
	 * library expects size_t values for the length of memory objects.  We
	 * check, just to be sure.
	 */
	if (sizeof(size_t) < sizeof(u_int32_t)) {
		__wt_env_errx(NULL, "%s",
		    "Build verification failed, a size_t is smaller than "
		    "4-bytes");
		return (WT_ERROR);
	}

	return (0);
}

/*
 * __wt_bt_data_copy_to_dbt --
 *	Copy a data/length pair into allocated memory in a DBT.
 */
int
__wt_bt_data_copy_to_dbt(DB *db, u_int8_t *data, size_t len, DBT *copy)
{
	ENV *env;

	env = db->env;

	if (copy->data == NULL || copy->data_len < len) {
		WT_RET(__wt_realloc(env, copy->data_len, len, &copy->data));
		copy->data_len = len;
	}
	memcpy(copy->data, data, copy->size = len);

	return (0);
}

/*
 * __wt_bt_first_offp --
 *	In a couple of places in the code, we're trying to walk down the
 *	internal pages from the root, and we need to get the WT_ITEM_OFFP
 *	information for the first key/WT_ITEM_OFFP pair on the page.
 */
void
__wt_bt_first_offp(WT_PAGE *page, u_int32_t *addrp, int *isleafp)
{
	WT_ITEM *item;

	item = (WT_ITEM *)WT_PAGE_BYTE(page);
	item = WT_ITEM_NEXT(item);

	*addrp = ((WT_ITEM_OFFP *)WT_ITEM_BYTE(item))->addr;
	*isleafp = WT_ITEM_TYPE(item) == WT_ITEM_OFFP_LEAF ? 1 : 0;
}

/*
 * __wt_set_ff_and_sa_from_addr --
 *	Set the page's first-free and space-available values from an
 *	address positioned one past the last used byte on the page.
 */
void
__wt_set_ff_and_sa_from_addr(DB *db, WT_PAGE *page, u_int8_t *addr)
{
	page->first_free = addr;
	page->space_avail = page->bytes - (addr - (u_int8_t *)page->hdr);
}

/*
 * __wt_bt_hdr_type --
 *	Return a string representing the page type.
 */
const char *
__wt_bt_hdr_type(WT_PAGE_HDR *hdr)
{
	switch (hdr->type) {
	case WT_PAGE_INVALID:
		return ("invalid");
	case WT_PAGE_OVFL:
		return ("overflow");
	case WT_PAGE_INT:
		return ("primary internal");
	case WT_PAGE_LEAF:
		return ("primary leaf");
	case WT_PAGE_DUP_INT:
		return ("duplicate internal");
	case WT_PAGE_DUP_LEAF:
		return ("duplicate leaf");
	default:
		break;
	}
	return ("unknown");
}

/*
 * __wt_bt_item_type --
 *	Return a string representing the item type.
 */
const char *
__wt_bt_item_type(WT_ITEM *item)
{
	switch (WT_ITEM_TYPE(item)) {
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
	case WT_ITEM_OFFP_INTL:
		return ("offpage-tree");
	case WT_ITEM_OFFP_LEAF:
		return ("offpage-leaf");
	default:
		break;
	}
	return ("unknown");
}
