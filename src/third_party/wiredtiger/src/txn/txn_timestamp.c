/*-
 * Copyright (c) 2014-2019 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_timestamp_to_string --
 *	Convert a timestamp to the MongoDB string representation.
 */
void
__wt_timestamp_to_string(wt_timestamp_t ts, char *ts_string)
{
	WT_IGNORE_RET(__wt_snprintf(ts_string, WT_TS_INT_STRING_SIZE,
	    "(%" PRIu32 ",%" PRIu32 ")",
	    (uint32_t)((ts >> 32) & 0xffffffff), (uint32_t)(ts & 0xffffffff)));
}

/*
 * __wt_timestamp_to_hex_string --
 *	Convert a timestamp to hex string representation.
 */
void
__wt_timestamp_to_hex_string(wt_timestamp_t ts, char *hex_timestamp)
{
	char *p, v;

	if (ts == 0) {
		hex_timestamp[0] = '0';
		hex_timestamp[1] = '\0';
		return;
	}
	if (ts == WT_TS_MAX) {
#define	WT_TS_MAX_HEX_STRING	"ffffffffffffffff"
		(void)memcpy(hex_timestamp,
		    WT_TS_MAX_HEX_STRING, strlen(WT_TS_MAX_HEX_STRING) + 1);
		return;
	}

	for (p = hex_timestamp; ts != 0; ts >>= 4)
		*p++ = (char)__wt_hex((u_char)(ts & 0x0f));
	*p = '\0';

	/* Reverse the string. */
	for (--p; p > hex_timestamp;) {
		v = *p;
		*p-- = *hex_timestamp;
		*hex_timestamp++ = v;
	}
}

/*
 * __wt_verbose_timestamp --
 *	Output a verbose message along with the specified timestamp.
 */
void
__wt_verbose_timestamp(
    WT_SESSION_IMPL *session, wt_timestamp_t ts, const char *msg)
{
	char ts_string[WT_TS_INT_STRING_SIZE];

	if (!WT_VERBOSE_ISSET(session, WT_VERB_TIMESTAMP))
		return;

	__wt_timestamp_to_string(ts, ts_string);
	__wt_verbose(session,
	    WT_VERB_TIMESTAMP, "Timestamp %s : %s", ts_string, msg);
}

/*
 * __wt_txn_parse_timestamp_raw --
 *	Decodes and sets a timestamp. Don't do any checking.
 */
int
__wt_txn_parse_timestamp_raw(WT_SESSION_IMPL *session, const char *name,
    wt_timestamp_t *timestamp, WT_CONFIG_ITEM *cval)
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

	*timestamp = 0;

	if (cval->len == 0)
		return (0);

	/* Protect against unexpectedly long hex strings. */
	if (cval->len > 2 * sizeof(wt_timestamp_t))
		WT_RET_MSG(session, EINVAL,
		    "%s timestamp too long '%.*s'",
		    name, (int)cval->len, cval->str);

	for (ts = 0, hex_itr = cval->str, len = cval->len; len > 0; --len) {
		if ((size_t)*hex_itr < WT_ELEMENTS(hextable))
			hex_val = hextable[(size_t)*hex_itr++];
		else
			hex_val = -1;
		if (hex_val < 0)
			WT_RET_MSG(session, EINVAL,
			    "Failed to parse %s timestamp '%.*s'",
			    name, (int)cval->len, cval->str);
		ts = (ts << 4) | (uint64_t)hex_val;
	}
	*timestamp = ts;

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
	if (cval->len != 0 && *timestamp == WT_TS_NONE)
		WT_RET_MSG(session, EINVAL,
		    "Failed to parse %s timestamp '%.*s': zero not permitted",
		    name, (int)cval->len, cval->str);

	return (0);
}

/*
 * __txn_get_read_timestamp --
 *	Get the read timestamp from the transaction. Additionally
 *	return bool to specify whether the transaction has set
 *	clear read queue flag.
 */
static bool
__txn_get_read_timestamp(
    WT_TXN *txn, wt_timestamp_t *read_timestampp)
{
	WT_ORDERED_READ(*read_timestampp, txn->read_timestamp);
	return (!txn->clear_read_q);
}

/*
 * __wt_txn_get_pinned_timestamp --
 *	Calculate the current pinned timestamp.
 */
int
__wt_txn_get_pinned_timestamp(
   WT_SESSION_IMPL *session, wt_timestamp_t *tsp, uint32_t flags)
{
	WT_CONNECTION_IMPL *conn;
	WT_TXN *txn;
	WT_TXN_GLOBAL *txn_global;
	wt_timestamp_t tmp_read_ts, tmp_ts;
	bool include_oldest, txn_has_write_lock;

	conn = S2C(session);
	txn_global = &conn->txn_global;
	include_oldest = LF_ISSET(WT_TXN_TS_INCLUDE_OLDEST);
	txn_has_write_lock = LF_ISSET(WT_TXN_TS_ALREADY_LOCKED);

	if (include_oldest && !txn_global->has_oldest_timestamp)
		return (WT_NOTFOUND);

	if (!txn_has_write_lock)
		__wt_readlock(session, &txn_global->rwlock);

	tmp_ts = include_oldest ? txn_global->oldest_timestamp : 0;

	/* Check for a running checkpoint */
	if (LF_ISSET(WT_TXN_TS_INCLUDE_CKPT) &&
	    txn_global->checkpoint_timestamp != WT_TS_NONE &&
	    (tmp_ts == 0 || txn_global->checkpoint_timestamp < tmp_ts))
		tmp_ts = txn_global->checkpoint_timestamp;
	if (!txn_has_write_lock)
		__wt_readunlock(session, &txn_global->rwlock);

	/* Look for the oldest ordinary reader. */
	__wt_readlock(session, &txn_global->read_timestamp_rwlock);
	TAILQ_FOREACH(txn, &txn_global->read_timestamph, read_timestampq) {
		/*
		 * Skip any transactions on the queue that are not active.
		 * Copy out value of read timestamp to prevent possible
		 * race where a transaction resets its read timestamp while
		 * we traverse the queue.
		 */
		if (!__txn_get_read_timestamp(txn, &tmp_read_ts))
			continue;
		/*
		 * A zero timestamp is possible here only when the oldest
		 * timestamp is not accounted for.
		 */
		if (tmp_ts == 0 || tmp_read_ts < tmp_ts)
			tmp_ts = tmp_read_ts;
		/*
		 * We break on the first active txn on the list.
		 */
		break;
	}
	__wt_readunlock(session, &txn_global->read_timestamp_rwlock);

	if (!include_oldest && tmp_ts == 0)
		return (WT_NOTFOUND);
	*tsp = tmp_ts;

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
		ts = txn_global->commit_timestamp;
		WT_ASSERT(session, ts != 0);

		/* Skip the lock if there are no running transactions. */
		if (TAILQ_EMPTY(&txn_global->commit_timestamph))
			goto done;

		/* Compare with the oldest running transaction. */
		__wt_readlock(session, &txn_global->commit_timestamp_rwlock);
		TAILQ_FOREACH(txn, &txn_global->commit_timestamph,
		    commit_timestampq) {
			if (txn->clear_commit_q)
				continue;

			tmpts = txn->first_commit_timestamp;
			WT_ASSERT(session, tmpts != 0);
			tmpts -= 1;

			if (tmpts < ts)
				ts = tmpts;
			break;
		}
		__wt_readunlock(session, &txn_global->commit_timestamp_rwlock);

		/*
		 * If a transaction is committing with timestamp 1, we could
		 * return zero here, which is unexpected.  Fail instead.
		 */
		if (ts == 0)
			return (WT_NOTFOUND);
	} else if (WT_STRING_MATCH("last_checkpoint", cval.str, cval.len))
		/* Read-only value forever. No lock needed. */
		ts = txn_global->last_ckpt_timestamp;
	else if (WT_STRING_MATCH("oldest", cval.str, cval.len)) {
		if (!txn_global->has_oldest_timestamp)
			return (WT_NOTFOUND);
		ts = txn_global->oldest_timestamp;
	} else if (WT_STRING_MATCH("oldest_reader", cval.str, cval.len))
		WT_RET(__wt_txn_get_pinned_timestamp(
		    session, &ts, WT_TXN_TS_INCLUDE_CKPT));
	else if (WT_STRING_MATCH("pinned", cval.str, cval.len))
		WT_RET(__wt_txn_get_pinned_timestamp(session, &ts,
		    WT_TXN_TS_INCLUDE_CKPT | WT_TXN_TS_INCLUDE_OLDEST));
	else if (WT_STRING_MATCH("recovery", cval.str, cval.len))
		/* Read-only value forever. No lock needed. */
		ts = txn_global->recovery_timestamp;
	else if (WT_STRING_MATCH("stable", cval.str, cval.len)) {
		if (!txn_global->has_stable_timestamp)
			return (WT_NOTFOUND);
		ts = txn_global->stable_timestamp;
	} else
		WT_RET_MSG(session, EINVAL,
		    "unknown timestamp query %.*s", (int)cval.len, cval.str);

done:	*tsp = ts;
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
		*tsp = txn->commit_timestamp;
	else if (WT_STRING_MATCH("first_commit", cval.str, cval.len))
		*tsp = txn->first_commit_timestamp;
	else if (WT_STRING_MATCH("prepare", cval.str, cval.len))
		*tsp = txn->prepare_timestamp;
	else if (WT_STRING_MATCH("read", cval.str, cval.len))
		*tsp = txn->read_timestamp;
	else
		WT_RET_MSG(session, EINVAL,
		    "unknown timestamp query %.*s", (int)cval.len, cval.str);

	return (0);
}

/*
 * __wt_txn_query_timestamp --
 *	Query a timestamp. The caller may query the global transaction or the
 *      session's transaction.
 */
int
__wt_txn_query_timestamp(WT_SESSION_IMPL *session,
    char *hex_timestamp, const char *cfg[], bool global_txn)
{
	wt_timestamp_t ts;

	if (global_txn)
		WT_RET(__txn_global_query_timestamp(session, &ts, cfg));
	else
		WT_RET(__txn_query_timestamp(session, &ts, cfg));

	__wt_timestamp_to_hex_string(ts, hex_timestamp);
	return (0);
}

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
	wt_timestamp_t last_pinned_timestamp, pinned_timestamp;

	txn_global = &S2C(session)->txn_global;

	/* Skip locking and scanning when the oldest timestamp is pinned. */
	if (txn_global->oldest_is_pinned)
		return (0);

	/* Scan to find the global pinned timestamp. */
	if ((ret = __wt_txn_get_pinned_timestamp(
	    session, &pinned_timestamp, WT_TXN_TS_INCLUDE_OLDEST)) != 0)
		return (ret == WT_NOTFOUND ? 0 : ret);

	if (txn_global->has_pinned_timestamp && !force) {
		last_pinned_timestamp = txn_global->pinned_timestamp;

		if (pinned_timestamp <= last_pinned_timestamp)
			return (0);
	}

	__wt_writelock(session, &txn_global->rwlock);
	/*
	 * Scan the global pinned timestamp again, it's possible that it got
	 * changed after the previous scan.
	 */
	if ((ret = __wt_txn_get_pinned_timestamp(session, &pinned_timestamp,
	    WT_TXN_TS_ALREADY_LOCKED | WT_TXN_TS_INCLUDE_OLDEST)) != 0) {
		__wt_writeunlock(session, &txn_global->rwlock);
		return (ret == WT_NOTFOUND ? 0 : ret);
	}

	if (!txn_global->has_pinned_timestamp || force ||
	    txn_global->pinned_timestamp < pinned_timestamp) {
		txn_global->pinned_timestamp = pinned_timestamp;
		txn_global->has_pinned_timestamp = true;
		txn_global->oldest_is_pinned =
		    txn_global->pinned_timestamp ==
		    txn_global->oldest_timestamp;
		txn_global->stable_is_pinned =
		    txn_global->pinned_timestamp ==
		    txn_global->stable_timestamp;
		__wt_verbose_timestamp(session,
		    pinned_timestamp, "Updated pinned timestamp");
	}
	__wt_writeunlock(session, &txn_global->rwlock);

	return (0);
}

/*
 * __wt_txn_global_set_timestamp --
 *	Set a global transaction timestamp.
 */
int
__wt_txn_global_set_timestamp(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_CONFIG_ITEM commit_cval, oldest_cval, stable_cval;
	WT_CONFIG_ITEM cval;
	WT_TXN_GLOBAL *txn_global;
	wt_timestamp_t commit_ts, oldest_ts, stable_ts;
	wt_timestamp_t last_oldest_ts, last_stable_ts;
	char ts_string[2][WT_TS_INT_STRING_SIZE];
	bool force, has_commit, has_oldest, has_stable;

	txn_global = &S2C(session)->txn_global;

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

	last_oldest_ts = txn_global->oldest_timestamp;
	last_stable_ts = txn_global->stable_timestamp;

	/*
	 * First do error checking on the timestamp values.  The
	 * oldest timestamp must always be less than or equal to
	 * the stable timestamp.  If we're only setting one
	 * then compare against the system timestamp.  If we're
	 * setting both then compare the passed in values.
	 */
	if (!has_commit && txn_global->has_commit_timestamp)
		commit_ts = txn_global->commit_timestamp;
	if (!has_oldest && txn_global->has_oldest_timestamp)
		oldest_ts = last_oldest_ts;
	if (!has_stable && txn_global->has_stable_timestamp)
		stable_ts = last_stable_ts;

	/*
	 * If a commit timestamp was supplied, check that it is no older than
	 * either the stable timestamp or the oldest timestamp.
	 */
	if (has_commit && (has_oldest ||
	    txn_global->has_oldest_timestamp) && oldest_ts > commit_ts) {
		__wt_readunlock(session, &txn_global->rwlock);
		__wt_timestamp_to_string(oldest_ts, ts_string[0]);
		__wt_timestamp_to_string(commit_ts, ts_string[1]);
		WT_RET_MSG(session, EINVAL,
		    "set_timestamp: oldest timestamp %s must not be later than "
		    "commit timestamp %s", ts_string[0], ts_string[1]);
	}

	if (has_commit && (has_stable ||
	    txn_global->has_stable_timestamp) && stable_ts > commit_ts) {
		__wt_readunlock(session, &txn_global->rwlock);
		__wt_timestamp_to_string(stable_ts, ts_string[0]);
		__wt_timestamp_to_string(commit_ts, ts_string[1]);
		WT_RET_MSG(session, EINVAL,
		    "set_timestamp: stable timestamp %s must not be later than "
		    "commit timestamp %s", ts_string[0], ts_string[1]);
	}

	/*
	 * The oldest and stable timestamps must always satisfy the condition
	 * that oldest <= stable.
	 */
	if ((has_oldest || has_stable) &&
	    (has_oldest || txn_global->has_oldest_timestamp) &&
	    (has_stable ||
	    txn_global->has_stable_timestamp) && oldest_ts > stable_ts) {
		__wt_readunlock(session, &txn_global->rwlock);
		__wt_timestamp_to_string(oldest_ts, ts_string[0]);
		__wt_timestamp_to_string(stable_ts, ts_string[1]);
		WT_RET_MSG(session, EINVAL,
		    "set_timestamp: oldest timestamp %s must not be later than "
		    "stable timestamp %s", ts_string[0], ts_string[1]);
	}

	__wt_readunlock(session, &txn_global->rwlock);

	/* Check if we are actually updating anything. */
	if (has_oldest &&
	    txn_global->has_oldest_timestamp && oldest_ts <= last_oldest_ts)
		has_oldest = false;

	if (has_stable &&
	    txn_global->has_stable_timestamp && stable_ts <= last_stable_ts)
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
		txn_global->commit_timestamp = commit_ts;
		txn_global->has_commit_timestamp = true;
		WT_STAT_CONN_INCR(session, txn_set_ts_commit_upd);
		__wt_verbose_timestamp(session, commit_ts,
		    "Updated global commit timestamp");
	}

	if (has_oldest && (!txn_global->has_oldest_timestamp || force ||
	    oldest_ts > txn_global->oldest_timestamp)) {
		txn_global->oldest_timestamp = oldest_ts;
		WT_STAT_CONN_INCR(session, txn_set_ts_oldest_upd);
		txn_global->has_oldest_timestamp = true;
		txn_global->oldest_is_pinned = false;
		__wt_verbose_timestamp(session, oldest_ts,
		    "Updated global oldest timestamp");
	}

	if (has_stable && (!txn_global->has_stable_timestamp || force ||
	    stable_ts > txn_global->stable_timestamp)) {
		txn_global->stable_timestamp = stable_ts;
		WT_STAT_CONN_INCR(session, txn_set_ts_stable_upd);
		txn_global->has_stable_timestamp = true;
		txn_global->stable_is_pinned = false;
		__wt_verbose_timestamp(session, stable_ts,
		    "Updated global stable timestamp");
	}
	__wt_writeunlock(session, &txn_global->rwlock);

	if (has_oldest || has_stable)
		WT_RET(__wt_txn_update_pinned_timestamp(session, force));

	return (0);
}

/*
 * __wt_txn_set_commit_timestamp --
 *	Validate the commit timestamp of a transaction.
 *	If the commit timestamp is less than the oldest timestamp and
 *	transaction is configured to roundup timestamps of a prepared
 *	transaction, then we will roundup the commit timestamp to the prepare
 *	timestamp of the transaction.
 */
int
__wt_txn_set_commit_timestamp(
    WT_SESSION_IMPL *session, wt_timestamp_t commit_ts)
{
	WT_TXN *txn = &session->txn;
	WT_TXN_GLOBAL *txn_global = &S2C(session)->txn_global;
	wt_timestamp_t oldest_ts, stable_ts;
	char ts_string[2][WT_TS_INT_STRING_SIZE];
	bool has_oldest_ts, has_stable_ts;

	/* Added this redundant initialization to circumvent build failure. */
	oldest_ts = stable_ts = 0;
	/*
	 * Compare against the oldest and the stable timestamp. Return an error
	 * if the given timestamp is less than oldest and/or stable timestamp.
	 */
	has_oldest_ts = txn_global->has_oldest_timestamp;
	if (has_oldest_ts)
		oldest_ts = txn_global->oldest_timestamp;
	has_stable_ts = txn_global->has_stable_timestamp;
	if (has_stable_ts)
		stable_ts = txn_global->stable_timestamp;

	if (!F_ISSET(txn, WT_TXN_HAS_TS_PREPARE)) {
		/*
		 * For a non-prepared transactions the commit timestamp should
		 * not be less than the stable timestamp.
		 */
		if (has_oldest_ts && commit_ts < oldest_ts) {
			__wt_timestamp_to_string(commit_ts, ts_string[0]);
			__wt_timestamp_to_string(oldest_ts, ts_string[1]);
			WT_RET_MSG(session, EINVAL,
			    "commit timestamp %s is less than the oldest "
			    "timestamp %s",
			    ts_string[0], ts_string[1]);
		}

		if (has_stable_ts && commit_ts < stable_ts) {
			__wt_timestamp_to_string(commit_ts, ts_string[0]);
			__wt_timestamp_to_string(stable_ts, ts_string[1]);
			WT_RET_MSG(session, EINVAL,
			    "commit timestamp %s is less than the stable "
			    "timestamp %s",
			    ts_string[0], ts_string[1]);
		}

		/*
		 * Compare against the commit timestamp of the current
		 * transaction.  Return an error if the given timestamp is
		 * older than the first commit timestamp.
		 */
		if (F_ISSET(txn, WT_TXN_HAS_TS_COMMIT) &&
		    commit_ts < txn->first_commit_timestamp) {
			__wt_timestamp_to_string(commit_ts, ts_string[0]);
			__wt_timestamp_to_string(
			    txn->first_commit_timestamp, ts_string[1]);
			WT_RET_MSG(session, EINVAL,
			    "commit timestamp %s older than the first "
			    "commit timestamp %s for this transaction",
			    ts_string[0], ts_string[1]);
		}
	} else {
		/*
		 * For a prepared transaction, the commit timestamp should not
		 * be less than the prepare timestamp.
		 */
		if (txn->prepare_timestamp > commit_ts) {
			if (!F_ISSET(txn, WT_TXN_TS_ROUND_PREPARED)) {
				__wt_timestamp_to_string(
				    commit_ts, ts_string[0]);
				__wt_timestamp_to_string(
				    txn->prepare_timestamp, ts_string[1]);
				WT_RET_MSG(session, EINVAL,
				    "commit timestamp %s is less than the "
				    "prepare timestamp %s for this transaction",
				    ts_string[0], ts_string[1]);
			}
			commit_ts = txn->prepare_timestamp;
		}
	}
	WT_ASSERT(session, txn->durable_timestamp == WT_TS_NONE ||
	    txn->durable_timestamp == txn->commit_timestamp);
	txn->durable_timestamp = txn->commit_timestamp = commit_ts;
	F_SET(txn, WT_TXN_HAS_TS_COMMIT);
	return (0);
}

/*
 * __wt_txn_set_durable_timestamp --
 *	Validate the durable timestamp of a transaction.
 */
int
__wt_txn_set_durable_timestamp(
    WT_SESSION_IMPL *session, wt_timestamp_t durable_ts)
{
	WT_TXN *txn = &session->txn;
	WT_TXN_GLOBAL *txn_global = &S2C(session)->txn_global;
	wt_timestamp_t oldest_ts, stable_ts;
	char ts_string[2][WT_TS_INT_STRING_SIZE];
	bool has_oldest_ts, has_stable_ts;

	/* Added this redundant initialization to circumvent build failure. */
	oldest_ts = stable_ts = 0;

	if (!F_ISSET(txn, WT_TXN_PREPARE))
		WT_RET_MSG(session, EINVAL,
		    "durable timestamp should not be specified for "
		    "non-prepared transaction");

	if (!F_ISSET(txn, WT_TXN_HAS_TS_COMMIT))
		WT_RET_MSG(session, EINVAL,
		    "commit timestamp is needed before the durable timestamp");

	/*
	 * Compare against the oldest and the stable timestamp. Return an error
	 * if the given timestamp is less than oldest and/or stable timestamp.
	 */
	has_oldest_ts = txn_global->has_oldest_timestamp;
	if (has_oldest_ts)
		oldest_ts = txn_global->oldest_timestamp;
	has_stable_ts = txn_global->has_stable_timestamp;
	if (has_stable_ts)
		stable_ts = txn_global->stable_timestamp;

	/*
	 * For a non-prepared transactions the commit timestamp should
	 * not be less than the stable timestamp.
	 */
	if (has_oldest_ts && durable_ts < oldest_ts) {
		__wt_timestamp_to_string(durable_ts, ts_string[0]);
		__wt_timestamp_to_string(oldest_ts, ts_string[1]);
		WT_RET_MSG(session, EINVAL,
		    "durable timestamp %s is less than the oldest timestamp %s",
		    ts_string[0], ts_string[1]);
	}

	if (has_stable_ts && durable_ts < stable_ts) {
		__wt_timestamp_to_string(durable_ts, ts_string[0]);
		__wt_timestamp_to_string(stable_ts, ts_string[1]);
		WT_RET_MSG(session, EINVAL,
		    "durable timestamp %s is less than the stable timestamp %s",
		    ts_string[0], ts_string[1]);
	}

	/* Check if the durable timestamp is less than the commit timestamp. */
	if (durable_ts < txn->commit_timestamp) {
		__wt_timestamp_to_string(durable_ts, ts_string[0]);
		__wt_timestamp_to_string(txn->commit_timestamp, ts_string[1]);
		WT_RET_MSG(session, EINVAL,
		    "durable timestamp %s is less than the commit timestamp %s "
		    "for this transaction",
		    ts_string[0], ts_string[1]);
	}
	txn->durable_timestamp = durable_ts;
	F_SET(txn, WT_TXN_HAS_TS_DURABLE);

	return (0);
}

/*
 * __wt_txn_set_prepare_timestamp --
 *	Validate and set the prepare timestamp of a transaction.
 */
int
__wt_txn_set_prepare_timestamp(
    WT_SESSION_IMPL *session, wt_timestamp_t prepare_ts)
{
	WT_TXN *prev, *txn = &session->txn;
	WT_TXN_GLOBAL *txn_global = &S2C(session)->txn_global;
	wt_timestamp_t oldest_ts, tmp_timestamp;
	char ts_string[2][WT_TS_INT_STRING_SIZE];

	WT_RET(__wt_txn_context_prepare_check(session));

	if (F_ISSET(txn, WT_TXN_HAS_TS_PREPARE))
		WT_RET_MSG(session, EINVAL, "prepare timestamp is already set");

	if (F_ISSET(txn, WT_TXN_HAS_TS_COMMIT))
		WT_RET_MSG(session, EINVAL, "commit timestamp "
		    "should not have been set before the prepare timestamp");

	__wt_readlock(session, &txn_global->read_timestamp_rwlock);
	oldest_ts = txn_global->oldest_timestamp;
	/*
	 * Prepare timestamp must be greater than the latest active read
	 * timestamp, if any.
	 */
	prev = TAILQ_LAST(
	    &txn_global->read_timestamph, __wt_txn_rts_qh);
	while (prev != NULL) {
		/*
		 * Skip self and non-active transactions. Copy out value of
		 * read timestamp to prevent possible race where a transaction
		 * resets its read timestamp while we traverse the queue.
		 */
		if (!__txn_get_read_timestamp(prev, &tmp_timestamp) ||
		    prev == txn) {
			prev = TAILQ_PREV(
			    prev, __wt_txn_rts_qh, read_timestampq);
			continue;
		}

		if (tmp_timestamp >= prepare_ts) {
			__wt_readunlock(session,
			    &txn_global->read_timestamp_rwlock);
			__wt_timestamp_to_string(prepare_ts, ts_string[0]);
			__wt_timestamp_to_string(tmp_timestamp, ts_string[1]);
			WT_RET_MSG(session, EINVAL,
			    "prepare timestamp %s must be greater than the "
			    "latest active read timestamp %s ",
			    ts_string[0], ts_string[1]);
		}
		break;
	}

	/* Unlock here to have less code branches. */
	__wt_readunlock(session, &txn_global->read_timestamp_rwlock);
	/*
	 * Check whether the prepare timestamp is less than the oldest
	 * timestamp.
	 */
	if (prepare_ts < oldest_ts) {
		/*
		 * Check whether the prepare timestamp needs to be rounded up to
		 * the oldest timestamp.
		 */
		if (F_ISSET(txn, WT_TXN_TS_ROUND_PREPARED)) {
			/*
			 * Check that there are no active readers. That would
			 * be a violation of preconditions for rounding
			 * timestamps of prepared transactions.
			 */
			WT_ASSERT(session, prev == NULL);

			if (WT_VERBOSE_ISSET(session, WT_VERB_TIMESTAMP)) {
				__wt_timestamp_to_string(
				    prepare_ts, ts_string[0]);
				__wt_timestamp_to_string(
				    oldest_ts, ts_string[1]);
				__wt_verbose(session, WT_VERB_TIMESTAMP,
				    "prepare timestamp %s rounded to oldest "
				    "timestamp %s", ts_string[0], ts_string[1]);
			}
			prepare_ts = oldest_ts;
		} else {
			__wt_timestamp_to_string(prepare_ts, ts_string[0]);
			__wt_timestamp_to_string(oldest_ts, ts_string[0]);
			WT_RET_MSG(session, EINVAL,
			    "prepare timestamp %s is older than the oldest "
			    "timestamp %s ", ts_string[0], ts_string[1]);
		}
	}
	txn->prepare_timestamp = prepare_ts;
	F_SET(txn, WT_TXN_HAS_TS_PREPARE);

	return (0);
}

/*
 * __wt_txn_set_read_timestamp --
 *	Parse a request to set a transaction's read_timestamp.
 */
int
__wt_txn_set_read_timestamp(
    WT_SESSION_IMPL *session, wt_timestamp_t read_ts)
{
	WT_TXN *txn = &session->txn;
	WT_TXN_GLOBAL *txn_global = &S2C(session)->txn_global;
	wt_timestamp_t ts_oldest;
	char ts_string[2][WT_TS_INT_STRING_SIZE];
	bool did_roundup_to_oldest;

	WT_RET(__wt_txn_context_prepare_check(session));

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
	 * This code is not using the timestamp validate function to
	 * avoid a race between checking and setting transaction
	 * timestamp.
	 */
	__wt_readlock(session, &txn_global->rwlock);
	ts_oldest = txn_global->oldest_timestamp;
	did_roundup_to_oldest = false;
	if (read_ts < ts_oldest) {
		/*
		 * If given read timestamp is earlier than oldest
		 * timestamp then round the read timestamp to
		 * oldest timestamp.
		 */
		if (F_ISSET(txn, WT_TXN_TS_ROUND_READ)) {
			txn->read_timestamp = ts_oldest;
			did_roundup_to_oldest = true;
		} else {
			__wt_readunlock(session, &txn_global->rwlock);
			__wt_timestamp_to_string(read_ts, ts_string[0]);
			__wt_timestamp_to_string(ts_oldest, ts_string[1]);
			WT_RET_MSG(session, EINVAL, "read timestamp "
			    "%s less than the oldest timestamp %s",
			    ts_string[0], ts_string[1]);
		}
	} else
		txn->read_timestamp = read_ts;

	__wt_txn_publish_read_timestamp(session);
	__wt_readunlock(session, &txn_global->rwlock);
	if (did_roundup_to_oldest &&
	    WT_VERBOSE_ISSET(session, WT_VERB_TIMESTAMP)) {
		/*
		 * This message is generated here to reduce the span of
		 * critical section.
		 */
		__wt_timestamp_to_string(read_ts, ts_string[0]);
		__wt_timestamp_to_string(ts_oldest, ts_string[1]);
		__wt_verbose(session, WT_VERB_TIMESTAMP, "read "
		    "timestamp %s : rounded to oldest timestamp %s",
		    ts_string[0], ts_string[1]);
	}

	/*
	 * If we already have a snapshot, it may be too early to match
	 * the timestamp (including the one we just read, if rounding
	 * to oldest).  Get a new one.
	 */
	if (F_ISSET(txn, WT_TXN_RUNNING))
		__wt_txn_get_snapshot(session);

	return (0);
}

/*
 * __wt_txn_set_timestamp --
 *	Parse a request to set a timestamp in a transaction.
 */
int
__wt_txn_set_timestamp(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_CONFIG_ITEM cval;
	WT_DECL_RET;
	wt_timestamp_t ts;

	WT_TRET(__wt_txn_context_check(session, true));

	/* Look for a commit timestamp. */
	ret = __wt_config_gets_def(session, cfg, "commit_timestamp", 0, &cval);
	WT_RET_NOTFOUND_OK(ret);
	if (ret == 0 && cval.len != 0) {
		WT_RET(__wt_txn_parse_timestamp(session, "commit", &ts, &cval));
		WT_RET(__wt_txn_set_commit_timestamp(session, ts));
		__wt_txn_publish_commit_timestamp(session);
	}

	/*
	 * Look for a durable timestamp. Durable timestamp should be set only
	 * after setting the commit timestamp.
	 */
	ret = __wt_config_gets_def(
	    session, cfg, "durable_timestamp", 0, &cval);
	WT_RET_NOTFOUND_OK(ret);
	if (ret == 0 && cval.len != 0) {
		WT_RET(__wt_txn_parse_timestamp(
		    session, "durable", &ts, &cval));
		WT_RET(__wt_txn_set_durable_timestamp(session, ts));
	}

	/* Look for a read timestamp. */
	WT_RET(__wt_config_gets_def(session, cfg, "read_timestamp", 0, &cval));
	if (ret == 0 && cval.len != 0) {
		WT_RET(__wt_txn_parse_timestamp(session, "read", &ts, &cval));
		WT_RET(__wt_txn_set_read_timestamp(session, ts));
	}

	/* Look for a prepare timestamp. */
	WT_RET(__wt_config_gets_def(session,
	    cfg, "prepare_timestamp", 0, &cval));
	if (ret == 0 && cval.len != 0) {
		WT_RET(__wt_txn_parse_timestamp(
		    session, "prepare", &ts, &cval));
		WT_RET(__wt_txn_set_prepare_timestamp(session, ts));
	}

	return (0);
}

/*
 * __wt_txn_publish_commit_timestamp --
 *	Publish a transaction's commit timestamp.
 */
void
__wt_txn_publish_commit_timestamp(WT_SESSION_IMPL *session)
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
	ts = txn->commit_timestamp;

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
		while (qtxn != NULL && qtxn->first_commit_timestamp > ts) {
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
	txn->first_commit_timestamp = ts;
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
 * __wt_txn_publish_read_timestamp --
 *	Publish a transaction's read timestamp.
 */
void
__wt_txn_publish_read_timestamp(WT_SESSION_IMPL *session)
{
	WT_TXN *qtxn, *txn, *txn_tmp;
	WT_TXN_GLOBAL *txn_global;
	wt_timestamp_t tmp_timestamp;
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
		while (qtxn != NULL) {
			if (!__txn_get_read_timestamp(qtxn, &tmp_timestamp) ||
			    tmp_timestamp > txn->read_timestamp) {
				++walked;
				qtxn = TAILQ_PREV(qtxn,
				    __wt_txn_rts_qh, read_timestampq);
			} else
				break;
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

	if (!F_ISSET(txn, WT_TXN_PUBLIC_TS_READ)) {
		txn->read_timestamp = WT_TS_NONE;
		return;
	}
#ifdef HAVE_DIAGNOSTIC
	{
	WT_TXN_GLOBAL *txn_global;
	wt_timestamp_t pinned_ts;

	txn_global = &S2C(session)->txn_global;
	pinned_ts = txn_global->pinned_timestamp;
	WT_ASSERT(session, txn->read_timestamp >= pinned_ts);
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
	txn->read_timestamp = WT_TS_NONE;
}

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
