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
 * __wt_db_build_verify --
 *	Verify the Btree build itself.
 */
int
__wt_db_build_verify(IENV *ienv)
{
	ENV *env;

	env = ienv->env;

	/*
	 * The compiler had better not have padded our structures -- make
	 * sure the page header structure is exactly what we expect.
	 */
	if (sizeof(WT_PAGE_HDR) != WT_HDR_SIZE) {
		__wt_env_errx(env,
		    "WiredTiger build failed, the WT_PAGE_HDR structure isn't"
		    " %d bytes in size", WT_HDR_SIZE);
		return (WT_ERROR);
	}
	if (WT_ALIGN(sizeof(WT_PAGE_HDR), sizeof(u_int32_t)) != WT_HDR_SIZE) {
		__wt_env_errx(env,
		    "Build verification failed, the WT_PAGE_HDR structure"
		    " isn't aligned correctly");
		return (WT_ERROR);
	}

	return (0);
}

/*
 * __wt_db_hdr_type --
 *	Return a string representing the page type.
 */
const char *
__wt_db_hdr_type(u_int32_t type)
{
	switch (type) {
	case WT_PAGE_INVALID:
		return ("invalid");
	case WT_PAGE_OVFL:
		return ("overflow");
	case WT_PAGE_INT:
		return ("primary internal");
	case WT_PAGE_LEAF:
		return ("primary leaf");
	case WT_PAGE_DUP_INT:
		return ("off-page duplicate internal");
	case WT_PAGE_DUP_LEAF:
		return ("off-page duplicate leaf");
	default:
		break;
	}
	return ("unknown");
}

/*
 * __wt_db_item_type --
 *	Return a string representing the item type.
 */
const char *
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
