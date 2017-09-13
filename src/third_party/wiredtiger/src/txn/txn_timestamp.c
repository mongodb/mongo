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
 * __wt_timestamp_to_hex_string --
 *	Convert a timestamp to hex string representation.
 */
int
__wt_timestamp_to_hex_string(
    WT_SESSION_IMPL *session, char *hex_timestamp, const wt_timestamp_t *ts_src)
{
	wt_timestamp_t ts;

	__wt_timestamp_set(&ts, ts_src);

	if (__wt_timestamp_iszero(&ts)) {
		hex_timestamp[0] = '0';
		hex_timestamp[1] = '\0';
		return (0);
	}

#if WT_TIMESTAMP_SIZE == 8
	{
	char *p, v;

	for (p = hex_timestamp; ts.val != 0; ts.val >>= 4)
		*p++ = (char)__wt_hex((u_char)(ts.val & 0x0f));
	*p = '\0';

	/* Reverse the string. */
	for (--p; p > hex_timestamp;) {
		v = *p;
		*p-- = *hex_timestamp;
		*hex_timestamp++ = v;
	}
	WT_UNUSED(session);
	}
#else
	{
	WT_ITEM hexts;
	size_t len;
	uint8_t *tsp;

	/* Avoid memory allocation: set up an item guaranteed large enough. */
	hexts.data = hexts.mem = hex_timestamp;
	hexts.memsize = 2 * WT_TIMESTAMP_SIZE + 1;
	/* Trim leading zeros. */
	for (tsp = ts.ts, len = WT_TIMESTAMP_SIZE;
	    len > 0 && *tsp == 0;
	    ++tsp, --len)
		;
	WT_RET(__wt_raw_to_hex(session, tsp, len, &hexts));
	}
#endif
	return (0);
}

/*
 * __wt_verbose_timestamp --
 *	Output a verbose message along with the specified timestamp
 */
void
__wt_verbose_timestamp(WT_SESSION_IMPL *session,
    const wt_timestamp_t *ts, const char *msg)
{
#ifdef HAVE_VERBOSE
	char timestamp_buf[2 * WT_TIMESTAMP_SIZE + 1];

	if (__wt_timestamp_to_hex_string(session, timestamp_buf, ts) != 0)
	       return;

	__wt_verbose(session,
	    WT_VERB_TIMESTAMP, "Timestamp %s : %s", timestamp_buf, msg);
#else
	WT_UNUSED(session);
	WT_UNUSED(ts);
	WT_UNUSED(msg);
#endif
}

/*
 * __wt_txn_parse_timestamp --
 *	Decodes and sets a timestamp.
 */
int
__wt_txn_parse_timestamp(WT_SESSION_IMPL *session,
     const char *name, wt_timestamp_t *timestamp, WT_CONFIG_ITEM *cval)
{
	__wt_timestamp_set_zero(timestamp);

	if (cval->len == 0)
		return (0);

	/* Protect against unexpectedly long hex strings. */
	if (cval->len > 2 * WT_TIMESTAMP_SIZE)
		WT_RET_MSG(session, EINVAL,
		    "%s timestamp too long '%.*s'",
		    name, (int)cval->len, cval->str);

#if WT_TIMESTAMP_SIZE == 8
	{
	static const u_char hextable[] = {
	    0,  0,  0,  0,  0,  0,  0,  0,
	    0,  0,  0,  0,  0,  0,  0,  0,
	    0,  0,  0,  0,  0,  0,  0,  0,
	    0,  0,  0,  0,  0,  0,  0,  0,
	    0,  0,  0,  0,  0,  0,  0,  0,
	    0,  0,  0,  0,  0,  0,  0,  0,
	    0,  1,  2,  3,  4,  5,  6,  7,
	    8,  9,  0,  0,  0,  0,  0,  0,
	    0, 10, 11, 12, 13, 14, 15,  0,
	    0,  0,  0,  0,  0,  0,  0,  0,
	    0,  0,  0,  0,  0,  0,  0,  0,
	    0,  0,  0,  0,  0,  0,  0,  0,
	    0, 10, 11, 12, 13, 14, 15
	};
	wt_timestamp_t ts;
	size_t len;
	const char *hex;

	for (ts.val = 0, hex = cval->str, len = cval->len; len > 0; --len)
		ts.val = (ts.val << 4) | hextable[(int)*hex++];
	__wt_timestamp_set(timestamp, &ts);
	}
#else
	{
	WT_DECL_RET;
	WT_ITEM ts;
	wt_timestamp_t tsbuf;
	size_t hexlen;
	const char *hexts;
	char padbuf[2 * WT_TIMESTAMP_SIZE + 1];

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
	ts.data = ts.mem = tsbuf.ts;
	ts.memsize = sizeof(tsbuf.ts);

	if ((ret = __wt_nhex_to_raw(session, hexts, hexlen, &ts)) != 0)
		WT_RET_MSG(session, ret, "Failed to parse %s timestamp '%.*s'",
		    name, (int)cval->len, cval->str);
	WT_ASSERT(session, ts.size <= WT_TIMESTAMP_SIZE);

	/* Copy the raw value to the end of the timestamp. */
	memcpy(timestamp->ts + WT_TIMESTAMP_SIZE - ts.size,
	    ts.data, ts.size);
	}
#endif
	if (__wt_timestamp_iszero(timestamp))
		WT_RET_MSG(session, EINVAL,
		    "Failed to parse %s timestamp '%.*s': zero not permitted",
		    name, (int)cval->len, cval->str);

	return (0);
}

/*
 * __txn_global_query_timestamp --
 *	Query a timestamp.
 */
static int
__txn_global_query_timestamp(
    WT_SESSION_IMPL *session, wt_timestamp_t *tsp, const char *cfg[])
{
	WT_CONNECTION_IMPL *conn;
	WT_CONFIG_ITEM cval;
	WT_TXN *txn;
	WT_TXN_GLOBAL *txn_global;
	wt_timestamp_t ts;

	conn = S2C(session);
	txn_global = &conn->txn_global;

	WT_RET(__wt_config_gets(session, cfg, "get", &cval));
	if (WT_STRING_MATCH("all_committed", cval.str, cval.len)) {
		if (!txn_global->has_commit_timestamp)
			return (WT_NOTFOUND);
		WT_WITH_TIMESTAMP_READLOCK(session, &txn_global->rwlock,
		    __wt_timestamp_set(&ts, &txn_global->commit_timestamp));
		WT_ASSERT(session, !__wt_timestamp_iszero(&ts));

		/* Compare with the oldest running transaction. */
		__wt_readlock(session, &txn_global->commit_timestamp_rwlock);
		txn = TAILQ_FIRST(&txn_global->commit_timestamph);
		if (txn != NULL &&
		    __wt_timestamp_cmp(&txn->first_commit_timestamp, &ts) < 0) {
			__wt_timestamp_set(&ts, &txn->first_commit_timestamp);
			WT_ASSERT(session, !__wt_timestamp_iszero(&ts));
		}
		__wt_readunlock(session, &txn_global->commit_timestamp_rwlock);
	} else if (WT_STRING_MATCH("oldest_reader", cval.str, cval.len)) {
		if (!txn_global->has_oldest_timestamp)
			return (WT_NOTFOUND);
		__wt_readlock(session, &txn_global->rwlock);
		__wt_timestamp_set(&ts, &txn_global->oldest_timestamp);

		/* Check for a running checkpoint */
		txn = txn_global->checkpoint_txn;
		if (txn_global->checkpoint_state.pinned_id != WT_TXN_NONE &&
		    !__wt_timestamp_iszero(&txn->read_timestamp) &&
		    __wt_timestamp_cmp(&txn->read_timestamp, &ts) < 0)
			__wt_timestamp_set(&ts, &txn->read_timestamp);
		__wt_readunlock(session, &txn_global->rwlock);

		/* Look for the oldest ordinary reader. */
		__wt_readlock(session, &txn_global->read_timestamp_rwlock);
		txn = TAILQ_FIRST(&txn_global->read_timestamph);
		if (txn != NULL &&
		    __wt_timestamp_cmp(&txn->read_timestamp, &ts) < 0)
			__wt_timestamp_set(&ts, &txn->read_timestamp);
		__wt_readunlock(session, &txn_global->read_timestamp_rwlock);
	} else if (WT_STRING_MATCH("stable", cval.str, cval.len)) {
		if (!txn_global->has_stable_timestamp)
			return (WT_NOTFOUND);
		WT_WITH_TIMESTAMP_READLOCK(session, &txn_global->rwlock,
		    __wt_timestamp_set(&ts, &txn_global->stable_timestamp));
	} else
		WT_RET_MSG(session, EINVAL,
		    "unknown timestamp query %.*s", (int)cval.len, cval.str);

	__wt_timestamp_set(tsp, &ts);
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
	wt_timestamp_t ts;

	WT_RET(__txn_global_query_timestamp(session, &ts, cfg));
	return (__wt_timestamp_to_hex_string(session, hex_timestamp, &ts));
#else
	WT_UNUSED(hex_timestamp);
	WT_UNUSED(cfg);

	WT_RET_MSG(session, ENOTSUP,
	    "requires a version of WiredTiger built with timestamp support");
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

	WT_WITH_TIMESTAMP_READLOCK(session, &txn_global->rwlock,
	    __wt_timestamp_set(
		&oldest_timestamp, &txn_global->oldest_timestamp));

	/* Scan to find the global pinned timestamp. */
	if ((ret = __txn_global_query_timestamp(
	    session, &active_timestamp, query_cfg)) != 0)
		return (ret == WT_NOTFOUND ? 0 : ret);

	if (__wt_timestamp_cmp(&oldest_timestamp, &active_timestamp) < 0) {
		__wt_timestamp_set(&pinned_timestamp, &oldest_timestamp);
	} else
		__wt_timestamp_set(&pinned_timestamp, &active_timestamp);

	__wt_writelock(session, &txn_global->rwlock);
	if (!txn_global->has_pinned_timestamp || __wt_timestamp_cmp(
	    &txn_global->pinned_timestamp, &pinned_timestamp) < 0) {
		__wt_timestamp_set(
		    &txn_global->pinned_timestamp, &pinned_timestamp);
		txn_global->has_pinned_timestamp = true;
		txn_global->oldest_is_pinned = __wt_timestamp_cmp(
		    &txn_global->pinned_timestamp,
		    &txn_global->oldest_timestamp) == 0;
		__wt_verbose_timestamp(session,
		    &pinned_timestamp, "Updated pinned timestamp");
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
	WT_CONFIG_ITEM commit_cval, oldest_cval, stable_cval;
	bool has_commit, has_oldest, has_stable;

	WT_RET(__wt_config_gets_def(session,
	    cfg, "commit_timestamp", 0, &commit_cval));
	has_commit = commit_cval.len != 0;

	WT_RET(__wt_config_gets_def(session,
	    cfg, "oldest_timestamp", 0, &oldest_cval));
	has_oldest = oldest_cval.len != 0;

	WT_RET(__wt_config_gets_def(session,
	    cfg, "stable_timestamp", 0, &stable_cval));
	has_stable = stable_cval.len != 0;

	/* If no timestamp was supplied, there's nothing to do. */
	if (!has_commit && !has_oldest && !has_stable)
		return (0);

#ifdef HAVE_TIMESTAMPS
	{
	WT_TXN_GLOBAL *txn_global;
	wt_timestamp_t commit_ts, oldest_ts, stable_ts;

	txn_global = &S2C(session)->txn_global;
	/*
	 * Parsing will initialize the timestamp to zero even if
	 * it is not configured.
	 */
	WT_RET(__wt_txn_parse_timestamp(
	    session, "commit", &commit_ts, &commit_cval));
	WT_RET(__wt_txn_parse_timestamp(
	    session, "oldest", &oldest_ts, &oldest_cval));
	WT_RET(__wt_txn_parse_timestamp(
	    session, "stable", &stable_ts, &stable_cval));
	__wt_writelock(session, &txn_global->rwlock);

	/*
	 * First do error checking on the timestamp values.  The
	 * oldest timestamp must always be less than or equal to
	 * the stable timestamp.  If we're only setting one
	 * then compare against the system timestamp.  If we're
	 * setting both then compare the passed in values.
	 */
	if (!has_commit && txn_global->has_commit_timestamp)
		__wt_timestamp_set(&commit_ts, &txn_global->commit_timestamp);
	if (!has_oldest && txn_global->has_oldest_timestamp)
		__wt_timestamp_set(&oldest_ts, &txn_global->oldest_timestamp);
	if (!has_stable && txn_global->has_oldest_timestamp)
		__wt_timestamp_set(&stable_ts, &txn_global->stable_timestamp);

	/*
	 * If a commit timestamp was supplied, check that it is no older than
	 * either the stable timestamp or the oldest timestamp.
	 */
	if (has_commit && (has_oldest || txn_global->has_oldest_timestamp) &&
	    __wt_timestamp_cmp(&oldest_ts, &commit_ts) > 0) {
		__wt_writeunlock(session, &txn_global->rwlock);
		WT_RET_MSG(session, EINVAL,
		    "set_timestamp: oldest timestamp must not be later than "
		    "commit timestamp");
	}

	if (has_commit && (has_stable || txn_global->has_stable_timestamp) &&
	    __wt_timestamp_cmp(&stable_ts, &commit_ts) > 0) {
		__wt_writeunlock(session, &txn_global->rwlock);
		WT_RET_MSG(session, EINVAL,
		    "set_timestamp: stable timestamp must not be later than "
		    "commit timestamp");
	}

	/*
	 * The oldest and stable timestamps must always satisfy the condition
	 * that oldest <= stable.
	 */
	if ((has_oldest || has_stable) &&
	    (has_oldest || txn_global->has_oldest_timestamp) &&
	    (has_stable || txn_global->has_stable_timestamp) &&
	    __wt_timestamp_cmp(&oldest_ts, &stable_ts) > 0) {
		__wt_writeunlock(session, &txn_global->rwlock);
		WT_RET_MSG(session, EINVAL,
		    "set_timestamp: oldest timestamp must not be later than "
		    "stable timestamp");
	}

	/*
	 * This method can be called from multiple threads, check that we are
	 * moving the global timestamps forwards.
	 *
	 * The exception is the commit timestamp, where the application can
	 * move it backwards (in fact, it only really makes sense to explicitly
	 * move it backwards because it otherwise tracks the largest
	 * commit_timestamp so it moves forward whenever transactions are
	 * assigned timestamps).
	 */
	if (has_commit) {
		__wt_timestamp_set(&txn_global->commit_timestamp, &commit_ts);
		txn_global->has_commit_timestamp = true;
		__wt_verbose_timestamp(session, &commit_ts,
		    "Updated global commit timestamp");
	}

	if (has_oldest && (!txn_global->has_oldest_timestamp ||
	    __wt_timestamp_cmp(
	    &oldest_ts, &txn_global->oldest_timestamp) > 0)) {
		__wt_timestamp_set(&txn_global->oldest_timestamp, &oldest_ts);
		txn_global->has_oldest_timestamp = true;
		txn_global->oldest_is_pinned = false;
		__wt_verbose_timestamp(session, &oldest_ts,
		    "Updated global oldest timestamp");
	}

	if (has_stable && (!txn_global->has_stable_timestamp ||
	    __wt_timestamp_cmp(
	    &stable_ts, &txn_global->stable_timestamp) > 0)) {
		__wt_timestamp_set(&txn_global->stable_timestamp, &stable_ts);
		txn_global->has_stable_timestamp = true;
		txn_global->stable_is_pinned = false;
		__wt_verbose_timestamp(session, &stable_ts,
		    "Updated global stable timestamp");
	}
	__wt_writeunlock(session, &txn_global->rwlock);

	if (has_oldest || has_stable)
		WT_RET(__wt_txn_update_pinned_timestamp(session));
	}
#else
		WT_RET_MSG(session, ENOTSUP, "set_timestamp requires a "
		    "version of WiredTiger built with timestamp support");
#endif
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

		if (!F_ISSET(txn, WT_TXN_RUNNING))
			WT_RET_MSG(session, EINVAL,
			    "Transaction must be running "
			    "to set a commit_timestamp");
		WT_RET(__wt_txn_parse_timestamp(
		    session, "commit", &txn->commit_timestamp, &cval));
		__wt_txn_set_commit_timestamp(session);
#else
		WT_RET_MSG(session, ENOTSUP, "commit_timestamp requires a "
		    "version of WiredTiger built with timestamp support");
#endif
	}
	WT_RET_NOTFOUND_OK(ret);

	return (0);
}

#ifdef HAVE_TIMESTAMPS
/*
 * __wt_txn_set_commit_timestamp --
 *	Publish a transaction's commit timestamp.
 */
void
__wt_txn_set_commit_timestamp(WT_SESSION_IMPL *session)
{
	wt_timestamp_t ts;
	WT_TXN *prev, *txn;
	WT_TXN_GLOBAL *txn_global;

	txn = &session->txn;
	txn_global = &S2C(session)->txn_global;

	if (F_ISSET(txn, WT_TXN_PUBLIC_TS_COMMIT))
		return;

	/*
	 * Copy the current commit timestamp (which can change while the
	 * transaction is running) into the first_commit_timestamp, which is
	 * fixed.
	 */
	__wt_timestamp_set(&ts, &txn->commit_timestamp);
	__wt_timestamp_set(&txn->first_commit_timestamp, &ts);

	__wt_writelock(session, &txn_global->commit_timestamp_rwlock);
	for (prev = TAILQ_LAST(&txn_global->commit_timestamph, __wt_txn_cts_qh);
	    prev != NULL &&
	    __wt_timestamp_cmp(&prev->first_commit_timestamp, &ts) > 0;
	    prev = TAILQ_PREV(prev, __wt_txn_cts_qh, commit_timestampq))
		;
	if (prev == NULL)
		TAILQ_INSERT_HEAD(
		    &txn_global->commit_timestamph, txn, commit_timestampq);
	else
		TAILQ_INSERT_AFTER(&txn_global->commit_timestamph,
		    prev, txn, commit_timestampq);
	__wt_writeunlock(session, &txn_global->commit_timestamp_rwlock);
	F_SET(txn, WT_TXN_HAS_TS_COMMIT | WT_TXN_PUBLIC_TS_COMMIT);
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

	if (!F_ISSET(txn, WT_TXN_PUBLIC_TS_COMMIT))
		return;

	__wt_writelock(session, &txn_global->commit_timestamp_rwlock);
	TAILQ_REMOVE(&txn_global->commit_timestamph, txn, commit_timestampq);
	__wt_writeunlock(session, &txn_global->commit_timestamp_rwlock);
	F_CLR(txn, WT_TXN_PUBLIC_TS_COMMIT);
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

	if (F_ISSET(txn, WT_TXN_PUBLIC_TS_READ))
		return;

	__wt_writelock(session, &txn_global->read_timestamp_rwlock);
	for (prev = TAILQ_LAST(&txn_global->read_timestamph, __wt_txn_rts_qh);
	    prev != NULL && __wt_timestamp_cmp(
	    &prev->read_timestamp, &txn->read_timestamp) > 0;
	    prev = TAILQ_PREV(prev, __wt_txn_rts_qh, read_timestampq))
		;
	if (prev == NULL)
		TAILQ_INSERT_HEAD(
		    &txn_global->read_timestamph, txn, read_timestampq);
	else
		TAILQ_INSERT_AFTER(
		    &txn_global->read_timestamph, prev, txn, read_timestampq);
	__wt_writeunlock(session, &txn_global->read_timestamp_rwlock);
	F_SET(txn, WT_TXN_HAS_TS_READ | WT_TXN_PUBLIC_TS_READ);
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

	if (!F_ISSET(txn, WT_TXN_PUBLIC_TS_READ))
		return;

	__wt_writelock(session, &txn_global->read_timestamp_rwlock);
	TAILQ_REMOVE(&txn_global->read_timestamph, txn, read_timestampq);
	__wt_writeunlock(session, &txn_global->read_timestamp_rwlock);
	F_CLR(txn, WT_TXN_PUBLIC_TS_READ);
}
#endif
