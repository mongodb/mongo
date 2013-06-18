/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_ext_transaction_id --
 *	Return the session's transaction ID.
 */
int
__wt_ext_transaction_id(WT_EXTENSION_API *wt_api,
    WT_SESSION *wt_session, uint64_t *transaction_idp)
{
	(void)wt_api;					/* Unused parameters */

	*transaction_idp = ((WT_SESSION_IMPL *)wt_session)->txn.id;
	return (0);
}

/*
 * __wt_ext_transaction_visible --
 *	Return if the current transaction can see the given transaction ID.
 */
int
__wt_ext_transaction_visible(WT_EXTENSION_API *wt_api,
    WT_SESSION *wt_session, uint64_t transaction_id)
{
	(void)wt_api;					/* Unused parameters */

	return (__wt_txn_visible(
	    (WT_SESSION_IMPL *)wt_session, transaction_id));
}
