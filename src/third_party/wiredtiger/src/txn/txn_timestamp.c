/*-
 * Copyright (c) 2014-2018 MongoDB, Inc.
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
 *	Output a verbose message along with the specified timestamp.
 */
void
__wt_verbose_timestamp(WT_SESSION_IMPL *session,
    const wt_timestamp_t *ts, const char *msg)
{
	char timestamp_buf[2 * WT_TIMESTAMP_SIZE + 1];

	if (!WT_VERBOSE_ISSET(session, WT_VERB_TIMESTAMP) ||
	    (__wt_timestamp_to_hex_string(session, timestamp_buf, ts) != 0))
	       return;

	__wt_verbose(session,
	    WT_VERB_TIMESTAMP, "Timestamp %s : %s", timestamp_buf, msg);
}

/*
 * __wt_txn_parse_timestamp_raw --
 *	Decodes and sets a timestamp. Don't do any checking.
 */
int
__wt_txn_parse_timestamp_raw(WT_SESSION_IMPL *session, const char *name,
    wt_timestamp_t *timestamp, WT_CONFIG_ITEM *cval)
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
	static const int8_t hextable[] = {
	    -1, -1,  -1,  -1,  -1,  -1,  -1,  -1,
	    -1, -1,  -1,  -1,  -1,  -1,  -1,  -1,
	    -1, -1,  -1,  -1,  -1,  -1,  -1,  -1,
	    -1, -1,  -1,  -1,  -1,  -1,  -1,  -1,
	    -1, -1,  -1,  -1,  -1,  -1,  -1,  -1,
	    -1, -1,  -1,  -1,  -1,  -1,  -1,  -1,
	     0,  1,   2,   3,   4,   5,   6,   7,
	     8,  9,  -1,  -1,  -1,  -1,  -1,  -1,
	    -1, 10,  11,  12,  13,  14,  15,  -1,
	    -1, -1,  -1,  -1,  -1,  -1,  -1,  -1,
	    -1, -1,  -1,  -1,  -1,  -1,  -1,  -1,
	    -1, -1,  -1,  -1,  -1,  -1,  -1,  -1,
	    -1, 10,  11,  12,  13,  14,  15,  -1
	};
	wt_timestamp_t ts;
	size_t len;
	int hex_val;
	const char *hex_itr;

	for (ts.val = 0, hex_itr = cval->str, len = cval->len; len > 0; --len) {
		if ((size_t)*hex_itr < WT_ELEMENTS(hextable))
			hex_val = hextable[(size_t)*hex_itr++];
		else
			hex_val = -1;
		if (hex_val < 0)
			WT_RET_MSG(session, EINVAL,
			    "Failed to parse %s timestamp '%.*s'",
			    name, (int)cval->len, cval->str);
		ts.val = (ts.val << 4) | (uint64_t)hex_val;
	}
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
	return (0);
}

/*
 * __wt_txn_parse_timestamp --
 *	Decodes and sets a timestamp checking it is non-zero.
 */
int
__wt_txn_parse_timestamp(WT_SESSION_IMPL *session, const char *name,
    wt_timestamp_t *timestamp, WT_CONFIG_ITEM *cval)
{
	WT_RET(__wt_txn_parse_timestamp_raw(session, name, timestamp, cval));
	if (cval->len != 0 && __wt_timestamp_iszero(timestamp))
		WT_RET_MSG(session, EINVAL,
		    "Failed to parse %s timestamp '%.*s': zero not permitted",
		    name, (int)cval->len, cval->str);

	return (0);
}

/*
 * __txn_get_pinned_timestamp --
 *	Calculate the current pinned timestamp.
 */
static int
__txn_get_pinned_timestamp(
   WT_SESSION_IMPL *session, wt_timestamp_t *tsp, bool include_checkpoint,
   bool include_oldest)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_TIMESTAMP(tmp_ts)
	WT_TXN *txn;
	WT_TXN_GLOBAL *txn_global;

	conn = S2C(session);
	txn_global = &conn->txn_global;

	if (include_oldest && !txn_global->has_oldest_timestamp)
		return (WT_NOTFOUND);

	__wt_readlock(session, &txn_global->rwlock);
	if (include_oldest)
		__wt_timestamp_set(&tmp_ts, &txn_global->oldest_timestamp);
	else
		__wt_timestamp_set_zero(&tmp_ts);

	/* Check for a running checkpoint */
	if (include_checkpoint &&
	    !__wt_timestamp_iszero(&txn_global->checkpoint_timestamp) &&
	    (__wt_timestamp_iszero(&tmp_ts) ||
	    __wt_timestamp_cmp(&txn_global->checkpoint_timestamp, &tmp_ts) <
	    0))
		__wt_timestamp_set(&tmp_ts, &txn_global->checkpoint_timestamp);
	__wt_readunlock(session, &txn_global->rwlock);

	/* Look for the oldest ordinary reader. */
	__wt_readlock(session, &txn_global->read_timestamp_rwlock);
	TAILQ_FOREACH(txn, &txn_global->read_timestamph, read_timestampq) {
		/*
		 * Skip any transactions on the queue that are not active.
		 */
		if (txn->clear_read_q)
			continue;
		/*
		 * A zero timestamp is possible here only when the oldest
		 * timestamp is not accounted for.
		 */
		if (__wt_timestamp_iszero(&tmp_ts) ||
		    __wt_timestamp_cmp(&txn->read_timestamp, &tmp_ts) < 0)
			__wt_timestamp_set(&tmp_ts, &txn->read_timestamp);
		/*
		 * We break on the first active txn on the list.
		 */
		break;
	}
	__wt_readunlock(session, &txn_global->read_timestamp_rwlock);

	if (!include_oldest && __wt_timestamp_iszero(&tmp_ts))
		return (WT_NOTFOUND);
	__wt_timestamp_set(tsp, &tmp_ts);

	return (0);
}

/*
 * __txn_global_query_timestamp --
 *	Query a timestamp on the global transaction.
 */
static int
__txn_global_query_timestamp(
    WT_SESSION_IMPL *session, wt_timestamp_t *tsp, const char *cfg[])
{
	WT_CONFIG_ITEM cval;
	WT_CONNECTION_IMPL *conn;
	WT_TXN *txn;
	WT_TXN_GLOBAL *txn_global;
	wt_timestamp_t ts, tmpts;

	conn = S2C(session);
	txn_global = &conn->txn_global;

	WT_STAT_CONN_INCR(session, txn_query_ts);
	WT_RET(__wt_config_gets(session, cfg, "get", &cval));
	if (WT_STRING_MATCH("all_committed", cval.str, cval.len)) {
		if (!txn_global->has_commit_timestamp)
			return (WT_NOTFOUND);
		WT_WITH_TIMESTAMP_READLOCK(session, &txn_global->rwlock,
		    __wt_timestamp_set(&ts, &txn_global->commit_timestamp));
		WT_ASSERT(session, !__wt_timestamp_iszero(&ts));

		/* Skip the lock if there are no running transactions. */
		if (TAILQ_EMPTY(&txn_global->commit_timestamph))
			goto done;

		/* Compare with the oldest running transaction. */
		__wt_readlock(session, &txn_global->commit_timestamp_rwlock);
		TAILQ_FOREACH(txn, &txn_global->commit_timestamph,
		    commit_timestampq) {
			if (txn->clear_commit_q)
				continue;

			__wt_timestamp_set(
			    &tmpts, &txn->first_commit_timestamp);
			WT_ASSERT(session, !__wt_timestamp_iszero(&tmpts));
			__wt_timestamp_subone(&tmpts);

			if (__wt_timestamp_cmp(&tmpts, &ts) < 0)
				__wt_timestamp_set(&ts, &tmpts);
			break;
		}
		__wt_readunlock(session, &txn_global->commit_timestamp_rwlock);
	} else if (WT_STRING_MATCH("last_checkpoint", cval.str, cval.len))
		/* Read-only value forever. No lock needed. */
		__wt_timestamp_set(&ts, &txn_global->last_ckpt_timestamp);
	else if (WT_STRING_MATCH("oldest", cval.str, cval.len)) {
		if (!txn_global->has_oldest_timestamp)
			return (WT_NOTFOUND);
		WT_WITH_TIMESTAMP_READLOCK(session, &txn_global->rwlock,
		    __wt_timestamp_set(&ts, &txn_global->oldest_timestamp));
	} else if (WT_STRING_MATCH("oldest_reader", cval.str, cval.len))
		WT_RET(__txn_get_pinned_timestamp(session, &ts, true, false));
	else if (WT_STRING_MATCH("pinned", cval.str, cval.len))
		WT_RET(__txn_get_pinned_timestamp(session, &ts, true, true));
	else if (WT_STRING_MATCH("recovery", cval.str, cval.len))
		/* Read-only value forever. No lock needed. */
		__wt_timestamp_set(&ts, &txn_global->recovery_timestamp);
	else if (WT_STRING_MATCH("stable", cval.str, cval.len)) {
		if (!txn_global->has_stable_timestamp)
			return (WT_NOTFOUND);
		WT_WITH_TIMESTAMP_READLOCK(session, &txn_global->rwlock,
		    __wt_timestamp_set(&ts, &txn_global->stable_timestamp));
	} else
		WT_RET_MSG(session, EINVAL,
		    "unknown timestamp query %.*s", (int)cval.len, cval.str);

done:	__wt_timestamp_set(tsp, &ts);
	return (0);
}

/*
 * __txn_query_timestamp --
 *	Query a timestamp within this session's transaction.
 */
static int
__txn_query_timestamp(
    WT_SESSION_IMPL *session, wt_timestamp_t *tsp, const char *cfg[])
{
	WT_CONFIG_ITEM cval;
	WT_TXN *txn;

	txn = &session->txn;

	WT_STAT_CONN_INCR(session, session_query_ts);
	if (!F_ISSET(txn, WT_TXN_RUNNING))
		return (WT_NOTFOUND);

	WT_RET(__wt_config_gets(session, cfg, "get", &cval));
	if (WT_STRING_MATCH("commit", cval.str, cval.len))
		__wt_timestamp_set(tsp, &txn->commit_timestamp);
	else if (WT_STRING_MATCH("first_commit", cval.str, cval.len))
		__wt_timestamp_set(tsp, &txn->first_commit_timestamp);
	else if (WT_STRING_MATCH("prepare", cval.str, cval.len))
		__wt_timestamp_set(tsp, &txn->prepare_timestamp);
	else if (WT_STRING_MATCH("read", cval.str, cval.len))
		__wt_timestamp_set(tsp, &txn->read_timestamp);
	else
		WT_RET_MSG(session, EINVAL,
		    "unknown timestamp query %.*s", (int)cval.len, cval.str);

	return (0);
}
#endif

/*
 * __wt_txn_query_timestamp --
 *	Query a timestamp. The caller may query the global transaction or the
 *      session's transaction.
 */
int
__wt_txn_query_timestamp(WT_SESSION_IMPL *session,
    char *hex_timestamp, const char *cfg[], bool global_txn)
{
#ifdef HAVE_TIMESTAMPS
	wt_timestamp_t ts;

	if (global_txn)
		WT_RET(__txn_global_query_timestamp(session, &ts, cfg));
	else
		WT_RET(__txn_query_timestamp(session, &ts, cfg));

	return (__wt_timestamp_to_hex_string(session, hex_timestamp, &ts));
#else
	WT_UNUSED(hex_timestamp);
	WT_UNUSED(cfg);
	WT_UNUSED(global_txn);

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
__wt_txn_update_pinned_timestamp(WT_SESSION_IMPL *session, bool force)
{
	WT_DECL_RET;
	WT_TXN_GLOBAL *txn_global;
	wt_timestamp_t active_timestamp, last_pinned_timestamp;
	wt_timestamp_t oldest_timestamp, pinned_timestamp;

	txn_global = &S2C(session)->txn_global;

	/* Skip locking and scanning when the oldest timestamp is pinned. */
	if (txn_global->oldest_is_pinned)
		return (0);

	WT_WITH_TIMESTAMP_READLOCK(session, &txn_global->rwlock,
	    __wt_timestamp_set(
	    &oldest_timestamp, &txn_global->oldest_timestamp));

	/* Scan to find the global pinned timestamp. */
	if ((ret = __txn_get_pinned_timestamp(
	    session, &active_timestamp, false, true)) != 0)
		return (ret == WT_NOTFOUND ? 0 : ret);

	if (__wt_timestamp_cmp(&oldest_timestamp, &active_timestamp) < 0)
		__wt_timestamp_set(&pinned_timestamp, &oldest_timestamp);
	else
		__wt_timestamp_set(&pinned_timestamp, &active_timestamp);

	if (txn_global->has_pinned_timestamp && !force) {
		WT_WITH_TIMESTAMP_READLOCK(session, &txn_global->rwlock,
		    __wt_timestamp_set(
		    &last_pinned_timestamp, &txn_global->pinned_timestamp));

		if (__wt_timestamp_cmp(
		    &pinned_timestamp, &last_pinned_timestamp) <= 0)
			return (0);
	}

	__wt_writelock(session, &txn_global->rwlock);
	if (!txn_global->has_pinned_timestamp || force || __wt_timestamp_cmp(
	    &txn_global->pinned_timestamp, &pinned_timestamp) < 0) {
		__wt_timestamp_set(
		    &txn_global->pinned_timestamp, &pinned_timestamp);
		txn_global->has_pinned_timestamp = true;
		txn_global->oldest_is_pinned = __wt_timestamp_cmp(
		    &txn_global->pinned_timestamp,
		    &txn_global->oldest_timestamp) == 0;
		txn_global->stable_is_pinned = __wt_timestamp_cmp(
		    &txn_global->pinned_timestamp,
		    &txn_global->stable_timestamp) == 0;
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

	WT_STAT_CONN_INCR(session, txn_set_ts);
	WT_RET(__wt_config_gets_def(session,
	    cfg, "commit_timestamp", 0, &commit_cval));
	has_commit = commit_cval.len != 0;
	if (has_commit)
		WT_STAT_CONN_INCR(session, txn_set_ts_commit);

	WT_RET(__wt_config_gets_def(session,
	    cfg, "oldest_timestamp", 0, &oldest_cval));
	has_oldest = oldest_cval.len != 0;
	if (has_oldest)
		WT_STAT_CONN_INCR(session, txn_set_ts_oldest);

	WT_RET(__wt_config_gets_def(session,
	    cfg, "stable_timestamp", 0, &stable_cval));
	has_stable = stable_cval.len != 0;
	if (has_stable)
		WT_STAT_CONN_INCR(session, txn_set_ts_stable);

	/* If no timestamp was supplied, there's nothing to do. */
	if (!has_commit && !has_oldest && !has_stable)
		return (0);

#ifdef HAVE_TIMESTAMPS
	{
	WT_CONFIG_ITEM cval;
	WT_TXN_GLOBAL *txn_global;
	wt_timestamp_t commit_ts, oldest_ts, stable_ts;
	wt_timestamp_t last_oldest_ts, last_stable_ts;
	char hex_timestamp[2][2 * WT_TIMESTAMP_SIZE + 1];
	bool force;

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

	WT_RET(__wt_config_gets_def(session,
	    cfg, "force", 0, &cval));
	force = cval.val != 0;

	if (force)
		goto set;

	__wt_readlock(session, &txn_global->rwlock);

	__wt_timestamp_set(&last_oldest_ts, &txn_global->oldest_timestamp);
	__wt_timestamp_set(&last_stable_ts, &txn_global->stable_timestamp);

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
		__wt_timestamp_set(&oldest_ts, &last_oldest_ts);
	if (!has_stable && txn_global->has_stable_timestamp)
		__wt_timestamp_set(&stable_ts, &last_stable_ts);

	/*
	 * If a commit timestamp was supplied, check that it is no older than
	 * either the stable timestamp or the oldest timestamp.
	 */
	if (has_commit && (has_oldest || txn_global->has_oldest_timestamp) &&
	    __wt_timestamp_cmp(&oldest_ts, &commit_ts) > 0) {
		__wt_readunlock(session, &txn_global->rwlock);
		WT_RET(__wt_timestamp_to_hex_string(
		    session, hex_timestamp[0], &oldest_ts));
		WT_RET(__wt_timestamp_to_hex_string(
		    session, hex_timestamp[1], &commit_ts));
		WT_RET_MSG(session, EINVAL,
		    "set_timestamp: oldest timestamp %s must not be later than "
		    "commit timestamp %s", hex_timestamp[0], hex_timestamp[1]);
	}

	if (has_commit && (has_stable || txn_global->has_stable_timestamp) &&
	    __wt_timestamp_cmp(&stable_ts, &commit_ts) > 0) {
		__wt_readunlock(session, &txn_global->rwlock);
		WT_RET(__wt_timestamp_to_hex_string(
		    session, hex_timestamp[0], &stable_ts));
		WT_RET(__wt_timestamp_to_hex_string(
		    session, hex_timestamp[1], &commit_ts));
		WT_RET_MSG(session, EINVAL,
		    "set_timestamp: stable timestamp %s must not be later than "
		    "commit timestamp %s", hex_timestamp[0], hex_timestamp[1]);
	}

	/*
	 * The oldest and stable timestamps must always satisfy the condition
	 * that oldest <= stable.
	 */
	if ((has_oldest || has_stable) &&
	    (has_oldest || txn_global->has_oldest_timestamp) &&
	    (has_stable || txn_global->has_stable_timestamp) &&
	    __wt_timestamp_cmp(&oldest_ts, &stable_ts) > 0) {
		__wt_readunlock(session, &txn_global->rwlock);
		WT_RET(__wt_timestamp_to_hex_string(
		    session, hex_timestamp[0], &oldest_ts));
		WT_RET(__wt_timestamp_to_hex_string(
		    session, hex_timestamp[1], &stable_ts));
		WT_RET_MSG(session, EINVAL,
		    "set_timestamp: oldest timestamp %s must not be later than "
		    "stable timestamp %s", hex_timestamp[0], hex_timestamp[1]);
	}

	__wt_readunlock(session, &txn_global->rwlock);

	/* Check if we are actually updating anything. */
	if (has_oldest && txn_global->has_oldest_timestamp &&
	    __wt_timestamp_cmp(&oldest_ts, &last_oldest_ts) <= 0)
		has_oldest = false;

	if (has_stable && txn_global->has_stable_timestamp &&
	    __wt_timestamp_cmp(&stable_ts, &last_stable_ts) <= 0)
		has_stable = false;

	if (!has_commit && !has_oldest && !has_stable)
		return (0);

set:	__wt_writelock(session, &txn_global->rwlock);
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
		WT_STAT_CONN_INCR(session, txn_set_ts_commit_upd);
		__wt_verbose_timestamp(session, &commit_ts,
		    "Updated global commit timestamp");
	}

	if (has_oldest && (!txn_global->has_oldest_timestamp ||
	    force || __wt_timestamp_cmp(
	    &oldest_ts, &txn_global->oldest_timestamp) > 0)) {
		__wt_timestamp_set(&txn_global->oldest_timestamp, &oldest_ts);
		WT_STAT_CONN_INCR(session, txn_set_ts_oldest_upd);
		txn_global->has_oldest_timestamp = true;
		txn_global->oldest_is_pinned = false;
		__wt_verbose_timestamp(session, &oldest_ts,
		    "Updated global oldest timestamp");
	}

	if (has_stable && (!txn_global->has_stable_timestamp ||
	    force || __wt_timestamp_cmp(
	    &stable_ts, &txn_global->stable_timestamp) > 0)) {
		__wt_timestamp_set(&txn_global->stable_timestamp, &stable_ts);
		WT_STAT_CONN_INCR(session, txn_set_ts_stable_upd);
		txn_global->has_stable_timestamp = true;
		txn_global->stable_is_pinned = false;
		__wt_verbose_timestamp(session, &stable_ts,
		    "Updated global stable timestamp");
	}
	__wt_writeunlock(session, &txn_global->rwlock);

	if (has_oldest || has_stable)
		WT_RET(__wt_txn_update_pinned_timestamp(session, force));
	}
	return (0);
#else
	WT_RET_MSG(session, ENOTSUP, "set_timestamp requires a "
	    "version of WiredTiger built with timestamp support");
#endif
}

#ifdef HAVE_TIMESTAMPS
/*
 * __wt_timestamp_validate --
 *	Validate a timestamp to be not older than the global oldest and global
 *	stable and running transaction commit timestamp and running transaction
 *	prepare timestamp.
 */
int
__wt_timestamp_validate(WT_SESSION_IMPL *session, const char *name,
    wt_timestamp_t *ts, WT_CONFIG_ITEM *cval)
{
	WT_TXN *txn = &session->txn;
	WT_TXN_GLOBAL *txn_global = &S2C(session)->txn_global;
	wt_timestamp_t oldest_ts, stable_ts;
	char hex_timestamp[2 * WT_TIMESTAMP_SIZE + 1];
	bool has_oldest_ts, has_stable_ts;

	/*
	 * Added this redundant initialization to circumvent build failure.
	 */
	__wt_timestamp_set_zero(&oldest_ts);
	__wt_timestamp_set_zero(&stable_ts);
	/*
	 * Compare against the oldest and the stable timestamp. Return an error
	 * if the given timestamp is older than oldest and/or stable timestamp.
	 */
	WT_WITH_TIMESTAMP_READLOCK(session, &txn_global->rwlock,
	    if ((has_oldest_ts = txn_global->has_oldest_timestamp))
		__wt_timestamp_set(&oldest_ts, &txn_global->oldest_timestamp);
	    if ((has_stable_ts = txn_global->has_stable_timestamp))
		__wt_timestamp_set(&stable_ts, &txn_global->stable_timestamp));

	if (has_oldest_ts && __wt_timestamp_cmp(ts, &oldest_ts) < 0) {
		WT_RET(__wt_timestamp_to_hex_string(session, hex_timestamp,
		    &oldest_ts));
		WT_RET_MSG(session, EINVAL,
		    "%s timestamp %.*s older than oldest timestamp %s",
		    name, (int)cval->len, cval->str, hex_timestamp);
	}
	if (has_stable_ts && __wt_timestamp_cmp(ts, &stable_ts) < 0) {
		WT_RET(__wt_timestamp_to_hex_string(session, hex_timestamp,
		    &stable_ts));
		WT_RET_MSG(session, EINVAL,
		    "%s timestamp %.*s older than stable timestamp %s",
		    name, (int)cval->len, cval->str, hex_timestamp);
	}

	/*
	 * Compare against the commit timestamp of the current transaction.
	 * Return an error if the given timestamp is older than the first
	 * commit timestamp.
	 */
	if (F_ISSET(txn, WT_TXN_HAS_TS_COMMIT) &&
	    __wt_timestamp_cmp(ts, &txn->first_commit_timestamp) < 0) {
		WT_RET(__wt_timestamp_to_hex_string(
		    session, hex_timestamp, &txn->first_commit_timestamp));
		WT_RET_MSG(session, EINVAL,
		    "%s timestamp %.*s older than the first "
		    "commit timestamp %s for this transaction",
		    name, (int)cval->len, cval->str, hex_timestamp);
	}

	/*
	 * Compare against the prepare timestamp of the current transaction.
	 * Return an error if the given timestamp is older than the prepare
	 * timestamp.
	 */
	if (F_ISSET(txn, WT_TXN_PREPARE) &&
	    __wt_timestamp_cmp(ts, &txn->prepare_timestamp) < 0) {
		WT_RET(__wt_timestamp_to_hex_string(
		    session, hex_timestamp, &txn->prepare_timestamp));
		WT_RET_MSG(session, EINVAL,
		    "%s timestamp %.*s older than the prepare timestamp %s "
		    "for this transaction",
		    name, (int)cval->len, cval->str, hex_timestamp);
	}

	return (0);
}
#endif

/*
 * __wt_txn_set_timestamp --
 *	Parse a request to set a timestamp in a transaction.
 */
int
__wt_txn_set_timestamp(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_CONFIG_ITEM cval;
	WT_DECL_RET;

	/* Look for a commit timestamp. */
	ret = __wt_config_gets_def(session, cfg, "commit_timestamp", 0, &cval);
	WT_RET_NOTFOUND_OK(ret);
	if (ret == 0 && cval.len != 0) {
#ifdef HAVE_TIMESTAMPS
		WT_TXN *txn = &session->txn;
		wt_timestamp_t ts;

		WT_TRET(__wt_txn_context_check(session, true));
		WT_RET(__wt_txn_parse_timestamp(session, "commit", &ts, &cval));
		WT_RET(__wt_timestamp_validate(session, "commit", &ts, &cval));
		__wt_timestamp_set(&txn->commit_timestamp, &ts);
		__wt_txn_set_commit_timestamp(session);
#else
		WT_RET_MSG(session, ENOTSUP, "commit_timestamp requires a "
		    "version of WiredTiger built with timestamp support");
#endif
	} else
		/*
		 * We allow setting the commit timestamp after a prepare
		 * but no other timestamp.
		 */
		WT_RET(__wt_txn_context_prepare_check(session));

	/* Look for a read timestamp. */
	WT_RET(__wt_txn_parse_read_timestamp(session, cfg));

	return (0);
}

/*
 * __wt_txn_parse_prepare_timestamp --
 *	Parse a request to set a transaction's prepare_timestamp.
 */
int
__wt_txn_parse_prepare_timestamp(
    WT_SESSION_IMPL *session, const char *cfg[], wt_timestamp_t *timestamp)
{
	WT_CONFIG_ITEM cval;

	WT_RET(__wt_config_gets_def(session,
	    cfg, "prepare_timestamp", 0, &cval));
	if (cval.len > 0) {
#ifdef HAVE_TIMESTAMPS
		WT_TXN *prev;
		WT_TXN_GLOBAL *txn_global;
		wt_timestamp_t oldest_ts;
		char hex_timestamp[2 * WT_TIMESTAMP_SIZE + 1];

		txn_global = &S2C(session)->txn_global;

		if (F_ISSET(&session->txn, WT_TXN_HAS_TS_COMMIT))
			WT_RET_MSG(session, EINVAL,
			    "commit timestamp should not have been set before "
			    "prepare transaction");

		WT_RET(__wt_txn_parse_timestamp(
		    session, "prepare", timestamp, &cval));

		/*
		 * Prepare timestamp must be later/greater than latest active
		 * read timestamp.
		 */
		__wt_readlock(session, &txn_global->read_timestamp_rwlock);
		prev = TAILQ_LAST(&txn_global->read_timestamph,
		    __wt_txn_rts_qh);
		while (prev != NULL) {
			/*
			 * Skip any transactions that are not active.
			 */
			if (prev->clear_read_q) {
				prev = TAILQ_PREV(
				    prev, __wt_txn_rts_qh, read_timestampq);
				continue;
			}
			if (__wt_timestamp_cmp(
			    &prev->read_timestamp, timestamp) >= 0) {
				__wt_readunlock(session,
				    &txn_global->read_timestamp_rwlock);
				WT_RET(__wt_timestamp_to_hex_string(session,
				    hex_timestamp, &prev->read_timestamp));
				WT_RET_MSG(session, EINVAL,
				    "prepare timestamp %.*s not later than "
				    "an active read timestamp %s ",
				    (int)cval.len, cval.str, hex_timestamp);
			}
			break;
		}
		__wt_readunlock(session, &txn_global->read_timestamp_rwlock);

		/*
		 * If there are no active readers, prepare timestamp must not
		 * be older than oldest timestamp.
		 */
		if (prev == NULL) {
			WT_WITH_TIMESTAMP_READLOCK(session, &txn_global->rwlock,
			    __wt_timestamp_set(&oldest_ts,
			    &txn_global->oldest_timestamp));

			if (__wt_timestamp_cmp(timestamp, &oldest_ts) < 0) {
				WT_RET(__wt_timestamp_to_hex_string(session,
				    hex_timestamp, &oldest_ts));
				WT_RET_MSG(session, EINVAL,
				    "prepare timestamp %.*s is older than the "
				    "oldest timestamp %s ", (int)cval.len,
				    cval.str, hex_timestamp);
			}
		 }
#else
		WT_UNUSED(timestamp);
		WT_RET_MSG(session, EINVAL, "prepare_timestamp requires a "
		    "version of WiredTiger built with timestamp support");
#endif
	} else
		WT_RET_MSG(session, EINVAL, "prepare timestamp is required");

	return (0);
}
/*
 * __wt_txn_parse_read_timestamp --
 *	Parse a request to set a transaction's read_timestamp.
 */
int
__wt_txn_parse_read_timestamp(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_CONFIG_ITEM cval;
	WT_TXN *txn;

	txn = &session->txn;

	WT_RET(__wt_config_gets_def(session, cfg, "read_timestamp", 0, &cval));
	if (cval.len > 0) {
#ifdef HAVE_TIMESTAMPS
		wt_timestamp_t ts;
		WT_TXN_GLOBAL *txn_global;
		char hex_timestamp[2][2 * WT_TIMESTAMP_SIZE + 1];
		bool round_to_oldest;

		txn_global = &S2C(session)->txn_global;
		WT_RET(__wt_txn_parse_timestamp(session, "read", &ts, &cval));

		/* Read timestamps imply / require snapshot isolation. */
		if (!F_ISSET(txn, WT_TXN_RUNNING))
			txn->isolation = WT_ISO_SNAPSHOT;
		else if (txn->isolation != WT_ISO_SNAPSHOT)
			WT_RET_MSG(session, EINVAL, "setting a read_timestamp"
			    " requires a transaction running at snapshot"
			    " isolation");

		/* Read timestamps can't change once set. */
		if (F_ISSET(txn, WT_TXN_HAS_TS_READ))
			WT_RET_MSG(session, EINVAL, "a read_timestamp"
			    " may only be set once per transaction");

		/*
		 * Read the configuration here to reduce the span of the
		 * critical section.
		 */
		WT_RET(__wt_config_gets_def(session,
		    cfg, "round_to_oldest", 0, &cval));
		round_to_oldest = cval.val;
		/*
		 * This code is not using the timestamp validate function to
		 * avoid a race between checking and setting transaction
		 * timestamp.
		 */
		WT_RET(__wt_timestamp_to_hex_string(session,
		    hex_timestamp[0], &ts));
		__wt_readlock(session, &txn_global->rwlock);
		if (__wt_timestamp_cmp(
		    &ts, &txn_global->oldest_timestamp) < 0) {
			WT_RET(__wt_timestamp_to_hex_string(session,
			    hex_timestamp[1], &txn_global->oldest_timestamp));
			/*
			 * If given read timestamp is earlier than oldest
			 * timestamp then round the read timestamp to
			 * oldest timestamp.
			 */
			if (round_to_oldest)
				__wt_timestamp_set(&txn->read_timestamp,
				    &txn_global->oldest_timestamp);
			else {
				__wt_readunlock(session, &txn_global->rwlock);
				WT_RET_MSG(session, EINVAL, "read timestamp "
				    "%s older than oldest timestamp %s",
				    hex_timestamp[0], hex_timestamp[1]);
			}
		} else {
			__wt_timestamp_set(&txn->read_timestamp, &ts);
			/*
			 * Reset to avoid a verbose message as read
			 * timestamp is not rounded to oldest timestamp.
			 */
			round_to_oldest = false;
		}

		__wt_txn_set_read_timestamp(session);
		__wt_readunlock(session, &txn_global->rwlock);
		if (round_to_oldest) {
			/*
			 * This message is generated here to reduce the span of
			 * critical section.
			 */
			__wt_verbose(session, WT_VERB_TIMESTAMP, "Read "
			    "timestamp %s : Rounded to oldest timestamp %s",
			    hex_timestamp[0], hex_timestamp[1]);
		}

		/*
		 * If we already have a snapshot, it may be too early to match
		 * the timestamp (including the one we just read, if rounding
		 * to oldest).  Get a new one.
		 */
		if (F_ISSET(txn, WT_TXN_RUNNING))
			__wt_txn_get_snapshot(session);

#else
		WT_UNUSED(txn);
		WT_RET_MSG(session, EINVAL, "read_timestamp requires a "
		    "version of WiredTiger built with timestamp support");
#endif
	}

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
	WT_TXN *qtxn, *txn, *txn_tmp;
	WT_TXN_GLOBAL *txn_global;
	wt_timestamp_t ts;
	uint64_t walked;

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

	__wt_writelock(session, &txn_global->commit_timestamp_rwlock);
	/*
	 * If our transaction is on the queue remove it first. The timestamp
	 * may move earlier so we otherwise might not remove ourselves before
	 * finding where to insert ourselves (which would result in a list
	 * loop) and we don't want to walk more of the list than needed.
	 */
	if (txn->clear_commit_q) {
		TAILQ_REMOVE(&txn_global->commit_timestamph,
		    txn, commit_timestampq);
		WT_PUBLISH(txn->clear_commit_q, false);
		--txn_global->commit_timestampq_len;
	}
	/*
	 * Walk the list to look for where to insert our own transaction
	 * and remove any transactions that are not active.  We stop when
	 * we get to the location where we want to insert.
	 */
	if (TAILQ_EMPTY(&txn_global->commit_timestamph)) {
		TAILQ_INSERT_HEAD(
		    &txn_global->commit_timestamph, txn, commit_timestampq);
		WT_STAT_CONN_INCR(session, txn_commit_queue_empty);
	} else {
		/* Walk from the start, removing cleared entries. */
		walked = 0;
		TAILQ_FOREACH_SAFE(qtxn, &txn_global->commit_timestamph,
		    commit_timestampq, txn_tmp) {
			++walked;
			/*
			 * Stop on the first entry that we cannot clear.
			 */
			if (!qtxn->clear_commit_q)
				break;

			TAILQ_REMOVE(&txn_global->commit_timestamph,
			    qtxn, commit_timestampq);
			WT_PUBLISH(qtxn->clear_commit_q, false);
			--txn_global->commit_timestampq_len;
		}

		/*
		 * Now walk backwards from the end to find the correct position
		 * for the insert.
		 */
		qtxn = TAILQ_LAST(
		     &txn_global->commit_timestamph, __wt_txn_cts_qh);
		while (qtxn != NULL && __wt_timestamp_cmp(
		    &qtxn->first_commit_timestamp, &ts) > 0) {
			++walked;
			qtxn = TAILQ_PREV(
			    qtxn, __wt_txn_cts_qh, commit_timestampq);
		}
		if (qtxn == NULL) {
			TAILQ_INSERT_HEAD(&txn_global->commit_timestamph,
			   txn, commit_timestampq);
			WT_STAT_CONN_INCR(session, txn_commit_queue_head);
		} else
			TAILQ_INSERT_AFTER(&txn_global->commit_timestamph,
			    qtxn, txn, commit_timestampq);
		WT_STAT_CONN_INCRV(session, txn_commit_queue_walked, walked);
	}
	__wt_timestamp_set(&txn->first_commit_timestamp, &ts);
	++txn_global->commit_timestampq_len;
	WT_STAT_CONN_INCR(session, txn_commit_queue_inserts);
	txn->clear_commit_q = false;
	F_SET(txn, WT_TXN_HAS_TS_COMMIT | WT_TXN_PUBLIC_TS_COMMIT);
	__wt_writeunlock(session, &txn_global->commit_timestamp_rwlock);
}

/*
 * __wt_txn_clear_commit_timestamp --
 *	Clear a transaction's published commit timestamp.
 */
void
__wt_txn_clear_commit_timestamp(WT_SESSION_IMPL *session)
{
	WT_TXN *txn;
	uint32_t flags;

	txn = &session->txn;

	if (!F_ISSET(txn, WT_TXN_PUBLIC_TS_COMMIT))
		return;
	flags = txn->flags;
	LF_CLR(WT_TXN_PUBLIC_TS_COMMIT);

	/*
	 * Notify other threads that our transaction is inactive and can be
	 * cleaned up safely from the commit timestamp queue whenever the next
	 * thread walks the queue. We do not need to remove it now.
	 */
	WT_PUBLISH(txn->clear_commit_q, true);
	WT_PUBLISH(txn->flags, flags);
}

/*
 * __wt_txn_set_read_timestamp --
 *	Publish a transaction's read timestamp.
 */
void
__wt_txn_set_read_timestamp(WT_SESSION_IMPL *session)
{
	WT_TXN *qtxn, *txn, *txn_tmp;
	WT_TXN_GLOBAL *txn_global;
	uint64_t walked;

	txn = &session->txn;
	txn_global = &S2C(session)->txn_global;

	if (F_ISSET(txn, WT_TXN_PUBLIC_TS_READ))
		return;

	__wt_writelock(session, &txn_global->read_timestamp_rwlock);
	/*
	 * If our transaction is on the queue remove it first. The timestamp
	 * may move earlier so we otherwise might not remove ourselves before
	 * finding where to insert ourselves (which would result in a list
	 * loop) and we don't want to walk more of the list than needed.
	 */
	if (txn->clear_read_q) {
		TAILQ_REMOVE(&txn_global->read_timestamph,
		    txn, read_timestampq);
		WT_PUBLISH(txn->clear_read_q, false);
		--txn_global->read_timestampq_len;
	}
	/*
	 * Walk the list to look for where to insert our own transaction
	 * and remove any transactions that are not active.  We stop when
	 * we get to the location where we want to insert.
	 */
	if (TAILQ_EMPTY(&txn_global->read_timestamph)) {
		TAILQ_INSERT_HEAD(
		    &txn_global->read_timestamph, txn, read_timestampq);
		WT_STAT_CONN_INCR(session, txn_read_queue_empty);
	} else {
		/* Walk from the start, removing cleared entries. */
		walked = 0;
		TAILQ_FOREACH_SAFE(qtxn, &txn_global->read_timestamph,
		    read_timestampq, txn_tmp) {
			++walked;
			if (!qtxn->clear_read_q)
				break;

			TAILQ_REMOVE(&txn_global->read_timestamph,
			    qtxn, read_timestampq);
			WT_PUBLISH(qtxn->clear_read_q, false);
			--txn_global->read_timestampq_len;
		}

		/*
		 * Now walk backwards from the end to find the correct position
		 * for the insert.
		 */
		qtxn = TAILQ_LAST(
		     &txn_global->read_timestamph, __wt_txn_rts_qh);
		while (qtxn != NULL &&
		    __wt_timestamp_cmp(&qtxn->read_timestamp,
		    &txn->read_timestamp) > 0) {
			++walked;
			qtxn = TAILQ_PREV(
			    qtxn, __wt_txn_rts_qh, read_timestampq);
		}
		if (qtxn == NULL) {
			TAILQ_INSERT_HEAD(&txn_global->read_timestamph,
			   txn, read_timestampq);
			WT_STAT_CONN_INCR(session, txn_read_queue_head);
		} else
			TAILQ_INSERT_AFTER(&txn_global->read_timestamph,
			    qtxn, txn, read_timestampq);
		WT_STAT_CONN_INCRV(session, txn_read_queue_walked, walked);
	}
	/*
	 * We do not set the read timestamp here. It has been set in the caller
	 * because special processing for round to oldest.
	 */
	++txn_global->read_timestampq_len;
	WT_STAT_CONN_INCR(session, txn_read_queue_inserts);
	txn->clear_read_q = false;
	F_SET(txn, WT_TXN_HAS_TS_READ | WT_TXN_PUBLIC_TS_READ);
	__wt_writeunlock(session, &txn_global->read_timestamp_rwlock);
}

/*
 * __wt_txn_clear_read_timestamp --
 *	Clear a transaction's published read timestamp.
 */
void
__wt_txn_clear_read_timestamp(WT_SESSION_IMPL *session)
{
	WT_TXN *txn;
	uint32_t flags;

	txn = &session->txn;

	if (!F_ISSET(txn, WT_TXN_PUBLIC_TS_READ))
		return;

#ifdef HAVE_DIAGNOSTIC
	{
	WT_TXN_GLOBAL *txn_global;
	wt_timestamp_t pinned_ts;

	txn_global = &S2C(session)->txn_global;
	WT_WITH_TIMESTAMP_READLOCK(session, &txn_global->rwlock,
	    __wt_timestamp_set(&pinned_ts, &txn_global->pinned_timestamp));
	WT_ASSERT(session,
	    __wt_timestamp_cmp(&txn->read_timestamp, &pinned_ts) >= 0);
	}
#endif
	flags = txn->flags;
	LF_CLR(WT_TXN_PUBLIC_TS_READ);

	/*
	 * Notify other threads that our transaction is inactive and can be
	 * cleaned up safely from the read timestamp queue whenever the
	 * next thread walks the queue. We do not need to remove it now.
	 */
	WT_PUBLISH(txn->clear_read_q, true);
	WT_PUBLISH(txn->flags, flags);
}
#endif

/*
 * __wt_txn_clear_timestamp_queues --
 *	We're about to clear the session and overwrite the txn structure.
 *	Remove ourselves from the commit timestamp queue and the read
 *	timestamp queue if we're on either of them.
 */
void
__wt_txn_clear_timestamp_queues(WT_SESSION_IMPL *session)
{
	WT_TXN *txn;
	WT_TXN_GLOBAL *txn_global;

	txn = &session->txn;
	txn_global = &S2C(session)->txn_global;

	if (!txn->clear_commit_q && !txn->clear_read_q)
		return;

	if (txn->clear_commit_q) {
		__wt_writelock(session, &txn_global->commit_timestamp_rwlock);
		/*
		 * Recheck after acquiring the lock.
		 */
		if (txn->clear_commit_q) {
			TAILQ_REMOVE(&txn_global->commit_timestamph,
			    txn, commit_timestampq);
			--txn_global->commit_timestampq_len;
			txn->clear_commit_q = false;
		}
		__wt_writeunlock(session, &txn_global->commit_timestamp_rwlock);
	}
	if (txn->clear_read_q) {
		__wt_writelock(session, &txn_global->read_timestamp_rwlock);
		/*
		 * Recheck after acquiring the lock.
		 */
		if (txn->clear_read_q) {
			TAILQ_REMOVE(
			    &txn_global->read_timestamph, txn, read_timestampq);
			--txn_global->read_timestampq_len;
			txn->clear_read_q = false;
		}
		__wt_writeunlock(session, &txn_global->read_timestamp_rwlock);
	}
}
