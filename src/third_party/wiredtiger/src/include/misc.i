/*-
 * Copyright (c) 2014-2018 MongoDB, Inc.
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
 * __wt_rdtsc --
 *      Get a timestamp from CPU registers.
 */
static inline uint64_t
__wt_rdtsc(void) {
#if defined (__i386)
	{
	uint64_t x;

	__asm__ volatile ("rdtsc" : "=A" (x));
	return (x);
	}
#elif defined (__amd64)
	{
	uint64_t a, d;

	__asm__ volatile ("rdtsc" : "=a" (a), "=d" (d));
	return ((d << 32) | a);
	}
#else
	return (0);
#endif
}

/*
 * __wt_clock --
 *       Obtain a timestamp via either a CPU register or via a system call on
 *       platforms where obtaining it directly from the hardware register is
 *       not supported.
 */
static inline uint64_t
__wt_clock(WT_SESSION_IMPL *session)
{
	struct timespec tsp;

	if (__wt_process.use_epochtime) {
		__wt_epoch(session, &tsp);
		return ((uint64_t)(tsp.tv_sec * WT_BILLION + tsp.tv_nsec));
	}
	return (__wt_rdtsc());
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
 * __wt_strnlen --
 *      Determine the length of a fixed-size string
 */
static inline size_t
__wt_strnlen(const char *s, size_t maxlen)
{
	size_t i;

	for (i = 0; i < maxlen && *s != '\0'; i++, s++)
		;
	return (i);
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
 * __wt_txn_context_prepare_check --
 *	Return an error if the current transaction is in the prepare state.
 */
static inline int
__wt_txn_context_prepare_check( WT_SESSION_IMPL *session)
{
#ifdef HAVE_TIMESTAMPS
	if (F_ISSET(&session->txn, WT_TXN_PREPARE))
		WT_RET_MSG(session, EINVAL,
		    "%s: not permitted in a prepared transaction",
		    session->name);
#else
	WT_UNUSED(session);
#endif
	return (0);
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

/*
 * __wt_spin_backoff --
 *	Back off while spinning for a resource. This is used to avoid busy
 *	waiting loops that can consume enough CPU to block real work being
 *	done. The algorithm spins a few times, then yields for a while, then
 *	falls back to sleeping.
 */
static inline void
__wt_spin_backoff(uint64_t *yield_count, uint64_t *sleep_usecs)
{
	if ((*yield_count) < 10) {
		(*yield_count)++;
		return;
	}

	if ((*yield_count) < WT_THOUSAND) {
		(*yield_count)++;
		__wt_yield();
		return;
	}

	(*sleep_usecs) = WT_MIN((*sleep_usecs) + 100, WT_THOUSAND);
	__wt_sleep(0, (*sleep_usecs));
}
