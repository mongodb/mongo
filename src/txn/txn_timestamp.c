/*-
 * Copyright (c) 2014-2017 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

#ifdef HAVE_TIMESTAMPS
/*
 * __wt_txn_parse_timestamp --
 *	Decodes and sets a timestamp.
 */
int
__wt_txn_parse_timestamp(WT_SESSION_IMPL *session,
     const char *name, uint8_t *timestamp, WT_CONFIG_ITEM *cval)
{
	WT_DECL_RET;
	WT_ITEM ts;
	wt_timestamp_t tsbuf;
	size_t hexlen;
	const char *hexts;
	char padbuf[2 * WT_TIMESTAMP_SIZE + 1];

	__wt_timestamp_set_zero(timestamp);

	if (cval->len == 0)
		return (0);

	/* Protect against unexpectedly long hex strings. */
	if (cval->len > 2 * WT_TIMESTAMP_SIZE)
		WT_RET_MSG(session, EINVAL,
		    "Failed to parse %s timestamp '%.*s': too long",
		    name, (int)cval->len, cval->str);

	/*
	 * The decoding function assumes it is decoding data produced by dump
	 * and so requires an even number of hex digits.
	 */
	if ((cval->len & 1) == 0) {
		hexts = cval->str;
		hexlen = cval->len;
	} else {
		padbuf[0] = '0';
		memcpy(padbuf + 1, cval->str, cval->len);
		hexts = padbuf;
		hexlen = cval->len + 1;
	}

	/* Avoid memory allocation to decode timestamps. */
	ts.data = ts.mem = tsbuf;
	ts.memsize = sizeof(tsbuf);

	if ((ret = __wt_nhex_to_raw(session, hexts, hexlen, &ts)) != 0)
		WT_RET_MSG(session, ret, "Failed to parse %s timestamp '%.*s'",
		    name, (int)cval->len, cval->str);
	WT_ASSERT(session, ts.size <= WT_TIMESTAMP_SIZE);

	/* Copy the raw value to the end of the timestamp. */
	memcpy(timestamp + WT_TIMESTAMP_SIZE - ts.size,
	    ts.data, ts.size);

	if (__wt_timestamp_iszero(timestamp))
		WT_RET_MSG(session, EINVAL,
		    "Failed to parse %s timestamp '%.*s': zero not permitted",
		    name, (int)cval->len, cval->str);

	return (ret);
}

/*
 * __txn_global_query_timestamp --
 *	Query a timestamp.
 */
static int
__txn_global_query_timestamp(
    WT_SESSION_IMPL *session, uint8_t *ts, const char *cfg[])
{
	WT_CONNECTION_IMPL *conn;
	WT_CONFIG_ITEM cval;
	WT_TXN *txn;
	WT_TXN_GLOBAL *txn_global;

	conn = S2C(session);
	txn_global = &conn->txn_global;

	WT_RET(__wt_config_gets(session, cfg, "get", &cval));
	if (WT_STRING_MATCH("all_committed", cval.str, cval.len)) {
		if (!txn_global->has_commit_timestamp)
			return (WT_NOTFOUND);
		__wt_readlock(session, &txn_global->rwlock);
		__wt_timestamp_set(ts, txn_global->commit_timestamp);
		__wt_readunlock(session, &txn_global->rwlock);

		/* Compare with the oldest running transaction. */
		__wt_readlock(session, &txn_global->commit_timestamp_rwlock);
		txn = TAILQ_FIRST(&txn_global->commit_timestamph);
		if (txn != NULL &&
		    __wt_timestamp_cmp(txn->commit_timestamp, ts) < 0)
			__wt_timestamp_set(ts, txn->commit_timestamp);
		__wt_readunlock(session, &txn_global->commit_timestamp_rwlock);
	} else if (WT_STRING_MATCH("oldest_reader", cval.str, cval.len)) {
		if (!txn_global->has_oldest_timestamp)
			return (WT_NOTFOUND);
		__wt_readlock(session, &txn_global->rwlock);
		__wt_timestamp_set(ts, txn_global->oldest_timestamp);

		/* Check for a running checkpoint */
		txn = txn_global->checkpoint_txn;
		if (txn_global->checkpoint_state.pinned_id != WT_TXN_NONE &&
		    !__wt_timestamp_iszero(txn->read_timestamp) &&
		    __wt_timestamp_cmp(txn->read_timestamp, ts) < 0)
			__wt_timestamp_set(ts, txn->read_timestamp);
		__wt_readunlock(session, &txn_global->rwlock);

		/* Look for the oldest ordinary reader. */
		__wt_readlock(session, &txn_global->read_timestamp_rwlock);
		txn = TAILQ_FIRST(&txn_global->read_timestamph);
		if (txn != NULL &&
		    __wt_timestamp_cmp(txn->read_timestamp, ts) < 0)
			__wt_timestamp_set(ts, txn->read_timestamp);
		__wt_readunlock(session, &txn_global->read_timestamp_rwlock);
	} else
		return (__wt_illegal_value(session, NULL));

	return (0);
}
#endif

/*
 * __wt_txn_global_query_timestamp --
 *	Query a timestamp.
 */
int
__wt_txn_global_query_timestamp(
    WT_SESSION_IMPL *session, char *hex_timestamp, const char *cfg[])
{
#ifdef HAVE_TIMESTAMPS
	WT_ITEM hexts;
	wt_timestamp_t ts;
	size_t len;
	uint8_t *tsp;

	/*
	 * Keep clang-analyzer happy: it can't tell that ts will be set
	 * whenever the call below succeeds.
	 */
	WT_CLEAR(ts);
	WT_RET(__txn_global_query_timestamp(session, ts, cfg));

	/* Avoid memory allocation: set up an item guaranteed large enough. */
	hexts.data = hexts.mem = hex_timestamp;
	hexts.memsize = 2 * WT_TIMESTAMP_SIZE + 1;
	/* Trim leading zeros. */
	for (tsp = ts, len = WT_TIMESTAMP_SIZE;
	    len > 0 && *tsp == 0;
	    ++tsp, --len)
		;
	WT_RET(__wt_raw_to_hex(session, tsp, len, &hexts));
	return (0);
#else
	WT_UNUSED(session);
	WT_UNUSED(hex_timestamp);
	WT_UNUSED(cfg);

	return (ENOTSUP);
#endif
}

#ifdef HAVE_TIMESTAMPS
/*
 * __wt_txn_update_pinned_timestamp --
 *	Update the pinned timestamp (the oldest timestamp that has to be
 *	maintained for current or future readers).
 */
int
__wt_txn_update_pinned_timestamp(WT_SESSION_IMPL *session)
{
	WT_DECL_RET;
	WT_TXN_GLOBAL *txn_global;
	wt_timestamp_t active_timestamp, oldest_timestamp, pinned_timestamp;
	const char *query_cfg[] = { WT_CONFIG_BASE(session,
	    WT_CONNECTION_query_timestamp), "get=oldest_reader", NULL };

	txn_global = &S2C(session)->txn_global;

	/* Skip locking and scanning when the oldest timestamp is pinned. */
	if (txn_global->oldest_is_pinned)
		return (0);

	__wt_readlock(session, &txn_global->rwlock);
	__wt_timestamp_set(oldest_timestamp, txn_global->oldest_timestamp);
	__wt_readunlock(session, &txn_global->rwlock);

	/* Scan to find the global pinned timestamp. */
	if ((ret = __txn_global_query_timestamp(
	    session, active_timestamp, query_cfg)) != 0)
		return (ret == WT_NOTFOUND ? 0 : ret);

	if (__wt_timestamp_cmp(oldest_timestamp, active_timestamp) < 0) {
		__wt_timestamp_set(pinned_timestamp, oldest_timestamp);
	} else
		__wt_timestamp_set(pinned_timestamp, active_timestamp);

	__wt_writelock(session, &txn_global->rwlock);
	if (!txn_global->has_pinned_timestamp || __wt_timestamp_cmp(
	    txn_global->pinned_timestamp, pinned_timestamp) < 0) {
		__wt_timestamp_set(
		    txn_global->pinned_timestamp, pinned_timestamp);
		txn_global->has_pinned_timestamp = true;
		txn_global->oldest_is_pinned = __wt_timestamp_cmp(
		    txn_global->pinned_timestamp,
		    txn_global->oldest_timestamp) == 0;
	}
	__wt_writeunlock(session, &txn_global->rwlock);

	return (0);
}
#endif

/*
 * __wt_txn_global_set_timestamp --
 *	Set a global transaction timestamp.
 */
int
__wt_txn_global_set_timestamp(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_CONFIG_ITEM cval;

	/*
	 * Look for a commit timestamp.
	 */
	WT_RET(
	    __wt_config_gets_def(session, cfg, "oldest_timestamp", 0, &cval));
	if (cval.len != 0) {
#ifdef HAVE_TIMESTAMPS
		WT_TXN_GLOBAL *txn_global;
		wt_timestamp_t oldest_timestamp;

		WT_RET(__wt_txn_parse_timestamp(
		    session, "oldest", oldest_timestamp, &cval));

		/*
		 * This method can be called from multiple threads, check that
		 * we are moving the global oldest timestamp forwards.
		 */
		txn_global = &S2C(session)->txn_global;
		__wt_writelock(session, &txn_global->rwlock);
		if (!txn_global->has_oldest_timestamp || __wt_timestamp_cmp(
		    txn_global->oldest_timestamp, oldest_timestamp) < 0) {
			__wt_timestamp_set(
			    txn_global->oldest_timestamp, oldest_timestamp);
			txn_global->has_oldest_timestamp = true;
			txn_global->oldest_is_pinned = false;
		}
		__wt_writeunlock(session, &txn_global->rwlock);

		WT_RET(__wt_txn_update_pinned_timestamp(session));
#else
		WT_RET_MSG(session, EINVAL, "oldest_timestamp requires a "
		    "version of WiredTiger built with timestamp support");
#endif
	}

	return (0);
}

/*
 * __wt_txn_set_timestamp --
 *	Set a transaction's timestamp.
 */
int
__wt_txn_set_timestamp(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_CONFIG_ITEM cval;
	WT_DECL_RET;

	/*
	 * Look for a commit timestamp.
	 */
	ret = __wt_config_gets(session, cfg, "commit_timestamp", &cval);
	if (ret == 0 && cval.len != 0) {
#ifdef HAVE_TIMESTAMPS
		WT_TXN *txn = &session->txn;

		WT_RET(__wt_txn_parse_timestamp(
		    session, "commit", txn->commit_timestamp, &cval));
		__wt_txn_set_commit_timestamp(session);
#else
		WT_RET_MSG(session, EINVAL, "commit_timestamp requires a "
		    "version of WiredTiger built with timestamp support");
#endif
	}
	WT_RET_NOTFOUND_OK(ret);

	return (0);
}

/*
 * __wt_txn_set_commit_timestamp --
 *	Publish a transaction's commit timestamp.
 */
void
__wt_txn_set_commit_timestamp(WT_SESSION_IMPL *session)
{
	WT_TXN *prev, *txn;
	WT_TXN_GLOBAL *txn_global;

	txn = &session->txn;
	txn_global = &S2C(session)->txn_global;

	if (F_ISSET(txn, WT_TXN_HAS_TS_COMMIT))
		return;

	__wt_writelock(session, &txn_global->commit_timestamp_rwlock);
	for (prev = TAILQ_LAST(&txn_global->commit_timestamph, __wt_txn_cts_qh);
	    prev != NULL && __wt_timestamp_cmp(
	    prev->commit_timestamp, txn->commit_timestamp) > 0;
	    prev = TAILQ_PREV(prev, __wt_txn_cts_qh, commit_timestampq))
		;
	if (prev == NULL)
		TAILQ_INSERT_HEAD(
		    &txn_global->commit_timestamph, txn, commit_timestampq);
	else
		TAILQ_INSERT_AFTER(&txn_global->commit_timestamph,
		    prev, txn, commit_timestampq);
	__wt_writeunlock(session, &txn_global->commit_timestamp_rwlock);
	F_SET(txn, WT_TXN_HAS_TS_COMMIT);
}

/*
 * __wt_txn_clear_commit_timestamp --
 *	Clear a transaction's published commit timestamp.
 */
void
__wt_txn_clear_commit_timestamp(WT_SESSION_IMPL *session)
{
	WT_TXN *txn;
	WT_TXN_GLOBAL *txn_global;

	txn = &session->txn;
	txn_global = &S2C(session)->txn_global;

	if (!F_ISSET(txn, WT_TXN_HAS_TS_COMMIT))
		return;

	__wt_writelock(session, &txn_global->commit_timestamp_rwlock);
	TAILQ_REMOVE(&txn_global->commit_timestamph, txn, commit_timestampq);
	__wt_writeunlock(session, &txn_global->commit_timestamp_rwlock);
}

/*
 * __wt_txn_set_read_timestamp --
 *	Publish a transaction's read timestamp.
 */
void
__wt_txn_set_read_timestamp(WT_SESSION_IMPL *session)
{
	WT_TXN *prev, *txn;
	WT_TXN_GLOBAL *txn_global;

	txn = &session->txn;
	txn_global = &S2C(session)->txn_global;

	if (F_ISSET(txn, WT_TXN_HAS_TS_READ))
		return;

	__wt_writelock(session, &txn_global->read_timestamp_rwlock);
	for (prev = TAILQ_LAST(&txn_global->read_timestamph, __wt_txn_rts_qh);
	    prev != NULL && __wt_timestamp_cmp(
	    prev->read_timestamp, txn->read_timestamp) > 0;
	    prev = TAILQ_PREV(prev, __wt_txn_rts_qh, read_timestampq))
		;
	if (prev == NULL)
		TAILQ_INSERT_HEAD(
		    &txn_global->read_timestamph, txn, read_timestampq);
	else
		TAILQ_INSERT_AFTER(
		    &txn_global->read_timestamph, prev, txn, read_timestampq);
	__wt_writeunlock(session, &txn_global->read_timestamp_rwlock);
	F_SET(txn, WT_TXN_HAS_TS_READ);
}

/*
 * __wt_txn_clear_read_timestamp --
 *	Clear a transaction's published read timestamp.
 */
void
__wt_txn_clear_read_timestamp(WT_SESSION_IMPL *session)
{
	WT_TXN *txn;
	WT_TXN_GLOBAL *txn_global;

	txn = &session->txn;
	txn_global = &S2C(session)->txn_global;

	if (!F_ISSET(txn, WT_TXN_HAS_TS_READ))
		return;

	__wt_writelock(session, &txn_global->read_timestamp_rwlock);
	TAILQ_REMOVE(&txn_global->read_timestamph, txn, read_timestampq);
	__wt_writeunlock(session, &txn_global->read_timestamp_rwlock);
	F_CLR(txn, WT_TXN_HAS_TS_READ);
}
