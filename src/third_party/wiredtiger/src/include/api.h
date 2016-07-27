/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/* Standard entry points to the API: declares/initializes local variables. */
#define	API_SESSION_INIT(s, h, n, cur, dh)				\
	WT_DATA_HANDLE *__olddh = (s)->dhandle;				\
	const char *__oldname = (s)->name;				\
	(s)->cursor = (cur);						\
	(s)->dhandle = (dh);						\
	(s)->name = (s)->lastop = #h "." #n;				\

#define	API_CALL_NOCONF(s, h, n, cur, dh) do {				\
	API_SESSION_INIT(s, h, n, cur, dh);				\
	WT_ERR(WT_SESSION_CHECK_PANIC(s));				\
	WT_ERR(__wt_verbose((s), WT_VERB_API, "CALL: " #h ":" #n))

#define	API_CALL(s, h, n, cur, dh, config, cfg) do {			\
	const char *cfg[] =						\
	    { WT_CONFIG_BASE(s, h##_##n), config, NULL };		\
	API_SESSION_INIT(s, h, n, cur, dh);				\
	WT_ERR(WT_SESSION_CHECK_PANIC(s));				\
	if ((config) != NULL)						\
		WT_ERR(__wt_config_check((s),				\
		    WT_CONFIG_REF(session, h##_##n), (config), 0));	\
	WT_ERR(__wt_verbose((s), WT_VERB_API, "CALL: " #h ":" #n))

#define	API_END(s, ret)							\
	if ((s) != NULL) {						\
		(s)->dhandle = __olddh;					\
		(s)->name = __oldname;					\
		if (F_ISSET(&(s)->txn, WT_TXN_RUNNING) &&		\
		    (ret) != 0 &&					\
		    (ret) != WT_NOTFOUND &&				\
		    (ret) != WT_DUPLICATE_KEY)				\
			F_SET(&(s)->txn, WT_TXN_ERROR);			\
	}								\
} while (0)

/* An API call wrapped in a transaction if necessary. */
#define	TXN_API_CALL(s, h, n, cur, bt, config, cfg) do {		\
	bool __autotxn = false;						\
	API_CALL(s, h, n, bt, cur, config, cfg);			\
	__autotxn = !F_ISSET(&(s)->txn, WT_TXN_AUTOCOMMIT | WT_TXN_RUNNING);\
	if (__autotxn)							\
		F_SET(&(s)->txn, WT_TXN_AUTOCOMMIT)

/* An API call wrapped in a transaction if necessary. */
#define	TXN_API_CALL_NOCONF(s, h, n, cur, bt) do {			\
	bool __autotxn = false;						\
	API_CALL_NOCONF(s, h, n, cur, bt);				\
	__autotxn = !F_ISSET(&(s)->txn, WT_TXN_AUTOCOMMIT | WT_TXN_RUNNING);\
	if (__autotxn)							\
		F_SET(&(s)->txn, WT_TXN_AUTOCOMMIT)

/* End a transactional API call, optional retry on deadlock. */
#define	TXN_API_END_RETRY(s, ret, retry)				\
	API_END(s, ret);						\
	if (__autotxn) {						\
		if (F_ISSET(&(s)->txn, WT_TXN_AUTOCOMMIT))		\
			F_CLR(&(s)->txn, WT_TXN_AUTOCOMMIT);		\
		else if (ret == 0 && !F_ISSET(&(s)->txn, WT_TXN_ERROR))	\
			ret = __wt_txn_commit((s), NULL);		\
		else {							\
			WT_TRET(__wt_txn_rollback((s), NULL));		\
			if ((ret == 0 || ret == WT_ROLLBACK) &&		\
			    (retry)) {					\
				ret = 0;				\
				continue;				\
			}						\
			WT_TRET(__wt_session_reset_cursors(s, false));	\
		}							\
	}								\
	break;								\
} while (1)

/* End a transactional API call, retry on deadlock. */
#define	TXN_API_END(s, ret)	TXN_API_END_RETRY(s, ret, 1)

/*
 * In almost all cases, API_END is returning immediately, make it simple.
 * If a session or connection method is about to return WT_NOTFOUND (some
 * underlying object was not found), map it to ENOENT, only cursor methods
 * return WT_NOTFOUND.
 */
#define	API_END_RET(s, ret)						\
	API_END(s, ret);						\
	return (ret)
#define	API_END_RET_NOTFOUND_MAP(s, ret)				\
	API_END(s, ret);						\
	return ((ret) == WT_NOTFOUND ? ENOENT : (ret))

#define	CONNECTION_API_CALL(conn, s, n, config, cfg)			\
	s = (conn)->default_session;					\
	API_CALL(s, WT_CONNECTION, n, NULL, NULL, config, cfg)

#define	CONNECTION_API_CALL_NOCONF(conn, s, n)				\
	s = (conn)->default_session;					\
	API_CALL_NOCONF(s, WT_CONNECTION, n, NULL, NULL)

#define	SESSION_API_CALL(s, n, config, cfg)				\
	API_CALL(s, WT_SESSION, n, NULL, NULL, config, cfg)

#define	SESSION_API_CALL_NOCONF(s, n)					\
	API_CALL_NOCONF(s, WT_SESSION, n, NULL, NULL)

#define	SESSION_TXN_API_CALL(s, n, config, cfg)				\
	TXN_API_CALL(s, WT_SESSION, n, NULL, NULL, config, cfg)

#define	CURSOR_API_CALL(cur, s, n, bt)					\
	(s) = (WT_SESSION_IMPL *)(cur)->session;			\
	API_CALL_NOCONF(s, WT_CURSOR, n, cur,				\
	    ((bt) == NULL) ? NULL : ((WT_BTREE *)(bt))->dhandle)

#define	JOINABLE_CURSOR_CALL_CHECK(cur)					\
	if (F_ISSET(cur, WT_CURSTD_JOINED))				\
		WT_ERR(__wt_curjoin_joined(cur))

#define	JOINABLE_CURSOR_API_CALL(cur, s, n, bt)				\
	CURSOR_API_CALL(cur, s, n, bt);					\
	JOINABLE_CURSOR_CALL_CHECK(cur)

#define	CURSOR_REMOVE_API_CALL(cur, s, bt)				\
	(s) = (WT_SESSION_IMPL *)(cur)->session;			\
	TXN_API_CALL_NOCONF(s, WT_CURSOR, remove, cur,			\
	    ((bt) == NULL) ? NULL : ((WT_BTREE *)(bt))->dhandle);

#define	JOINABLE_CURSOR_REMOVE_API_CALL(cur, s, bt)			\
	CURSOR_REMOVE_API_CALL(cur, s, bt);				\
	JOINABLE_CURSOR_CALL_CHECK(cur)

#define	CURSOR_UPDATE_API_CALL(cur, s, n, bt)				\
	(s) = (WT_SESSION_IMPL *)(cur)->session;			\
	TXN_API_CALL_NOCONF(s, WT_CURSOR, n, cur,			\
	    ((bt) == NULL) ? NULL : ((WT_BTREE *)(bt))->dhandle);	\
	if (F_ISSET(S2C(s), WT_CONN_IN_MEMORY) && __wt_cache_full(s))	\
		WT_ERR(WT_CACHE_FULL);

#define	JOINABLE_CURSOR_UPDATE_API_CALL(cur, s, n, bt)			\
	CURSOR_UPDATE_API_CALL(cur, s, n, bt);				\
	JOINABLE_CURSOR_CALL_CHECK(cur)

#define	CURSOR_UPDATE_API_END(s, ret)					\
	TXN_API_END(s, ret)

#define	ASYNCOP_API_CALL(conn, s, n)					\
	s = (conn)->default_session;					\
	API_CALL_NOCONF(s, asyncop, n, NULL, NULL)
