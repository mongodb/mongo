/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_ext_transaction_id --
 *	Return the session's transaction ID.
 */
uint64_t
__wt_ext_transaction_id(WT_EXTENSION_API *wt_api, WT_SESSION *wt_session)
{
	WT_SESSION_IMPL *session;

	(void)wt_api;					/* Unused parameters */
	session = (WT_SESSION_IMPL *)wt_session;
	/* Ignore failures: the only case is running out of transaction IDs. */
	(void)__wt_txn_id_check(session);
	return (session->txn.id);
}

/*
 * __wt_ext_transaction_isolation_level --
 *	Return if the current transaction's isolation level.
 */
int
__wt_ext_transaction_isolation_level(
    WT_EXTENSION_API *wt_api, WT_SESSION *wt_session)
{
	WT_SESSION_IMPL *session;
	WT_TXN *txn;

	(void)wt_api;					/* Unused parameters */

	session = (WT_SESSION_IMPL *)wt_session;
	txn = &session->txn;

	if (txn->isolation == WT_ISO_READ_COMMITTED)
	    return (WT_TXN_ISO_READ_COMMITTED);
	if (txn->isolation == WT_ISO_READ_UNCOMMITTED)
	    return (WT_TXN_ISO_READ_UNCOMMITTED);
	return (WT_TXN_ISO_SNAPSHOT);
}

/*
 * __wt_ext_transaction_notify --
 *	Request notification of transaction resolution.
 */
int
__wt_ext_transaction_notify(
    WT_EXTENSION_API *wt_api, WT_SESSION *wt_session, WT_TXN_NOTIFY *notify)
{
	WT_SESSION_IMPL *session;
	WT_TXN *txn;

	(void)wt_api;					/* Unused parameters */

	session = (WT_SESSION_IMPL *)wt_session;
	txn = &session->txn;

	/*
	 * XXX
	 * For now, a single slot for notifications: I'm not bothering with
	 * more than one because more than one data-source in a transaction
	 * doesn't work anyway.
	 */
	if (txn->notify == notify)
		return (0);
	if (txn->notify != NULL)
		return (ENOMEM);

	txn->notify = notify;

	return (0);
}

/*
 * __wt_ext_transaction_oldest --
 *	Return the oldest transaction ID not yet visible to a running
 * transaction.
 */
uint64_t
__wt_ext_transaction_oldest(WT_EXTENSION_API *wt_api)
{
	return (((WT_CONNECTION_IMPL *)wt_api->conn)->txn_global.oldest_id);
}

/*
 * __wt_ext_transaction_visible --
 *	Return if the current transaction can see the given transaction ID.
 */
int
__wt_ext_transaction_visible(
    WT_EXTENSION_API *wt_api, WT_SESSION *wt_session, uint64_t transaction_id)
{
	(void)wt_api;					/* Unused parameters */

	return (__wt_txn_visible(
	    (WT_SESSION_IMPL *)wt_session, transaction_id));
}
