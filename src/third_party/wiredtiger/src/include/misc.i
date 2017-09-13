/*-
 * Copyright (c) 2014-2017 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * __wt_cond_wait --
 *	Wait on a mutex, optionally timing out.
 */
static inline void
__wt_cond_wait(WT_SESSION_IMPL *session,
    WT_CONDVAR *cond, uint64_t usecs, bool (*run_func)(WT_SESSION_IMPL *))
{
	bool notused;

	__wt_cond_wait_signal(session, cond, usecs, run_func, &notused);
}

/*
 * __wt_hex --
 *	Convert a byte to a hex character.
 */
static inline u_char
__wt_hex(int c)
{
	return ((u_char)"0123456789abcdef"[c]);
}

/*
 * __wt_strdup --
 *	ANSI strdup function.
 */
static inline int
__wt_strdup(WT_SESSION_IMPL *session, const char *str, void *retp)
{
	return (__wt_strndup(
	    session, str, (str == NULL) ? 0 : strlen(str), retp));
}

/*
 * __wt_snprintf --
 *	snprintf convenience function, ignoring the returned size.
 */
static inline int
__wt_snprintf(char *buf, size_t size, const char *fmt, ...)
    WT_GCC_FUNC_ATTRIBUTE((format (printf, 3, 4)))
{
	WT_DECL_RET;
	size_t len;
	va_list ap;

	len = 0;

	va_start(ap, fmt);
	ret = __wt_vsnprintf_len_incr(buf, size, &len, fmt, ap);
	va_end(ap);
	WT_RET(ret);

	/* It's an error if the buffer couldn't hold everything. */
	return (len >= size ? ERANGE : 0);
}

/*
 * __wt_vsnprintf --
 *	vsnprintf convenience function, ignoring the returned size.
 */
static inline int
__wt_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
	size_t len;

	len = 0;

	WT_RET(__wt_vsnprintf_len_incr(buf, size, &len, fmt, ap));

	/* It's an error if the buffer couldn't hold everything. */
	return (len >= size ? ERANGE : 0);
}

/*
 * __wt_snprintf_len_set --
 *	snprintf convenience function, setting the returned size.
 */
static inline int
__wt_snprintf_len_set(
    char *buf, size_t size, size_t *retsizep, const char *fmt, ...)
    WT_GCC_FUNC_ATTRIBUTE((format (printf, 4, 5)))
{
	WT_DECL_RET;
	va_list ap;

	*retsizep = 0;

	va_start(ap, fmt);
	ret = __wt_vsnprintf_len_incr(buf, size, retsizep, fmt, ap);
	va_end(ap);
	return (ret);
}

/*
 * __wt_vsnprintf_len_set --
 *	vsnprintf convenience function, setting the returned size.
 */
static inline int
__wt_vsnprintf_len_set(
    char *buf, size_t size, size_t *retsizep, const char *fmt, va_list ap)
{
	*retsizep = 0;

	return (__wt_vsnprintf_len_incr(buf, size, retsizep, fmt, ap));
}

/*
 * __wt_snprintf_len_incr --
 *	snprintf convenience function, incrementing the returned size.
 */
static inline int
__wt_snprintf_len_incr(
    char *buf, size_t size, size_t *retsizep, const char *fmt, ...)
    WT_GCC_FUNC_ATTRIBUTE((format (printf, 4, 5)))
{
	WT_DECL_RET;
	va_list ap;

	va_start(ap, fmt);
	ret = __wt_vsnprintf_len_incr(buf, size, retsizep, fmt, ap);
	va_end(ap);
	return (ret);
}

/*
 * __wt_txn_context_check --
 *	Complain if a transaction is/isn't running.
 */
static inline int
__wt_txn_context_check(WT_SESSION_IMPL *session, bool requires_txn)
{
	if (requires_txn && !F_ISSET(&session->txn, WT_TXN_RUNNING))
		WT_RET_MSG(session, EINVAL,
		    "%s: only permitted in a running transaction",
		    session->name);
	if (!requires_txn && F_ISSET(&session->txn, WT_TXN_RUNNING))
		WT_RET_MSG(session, EINVAL,
		    "%s: not permitted in a running transaction",
		    session->name);
	return (0);
}
