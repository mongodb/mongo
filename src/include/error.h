/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#define	WT_DEBUG_POINT	((void *)0xdeadbeef)
#define	WT_DEBUG_BYTE	(0xab)

/* In DIAGNOSTIC mode, yield in places where we want to encourage races. */
#ifdef HAVE_DIAGNOSTIC
#define	WT_HAVE_DIAGNOSTIC_YIELD do {					\
	__wt_yield();							\
} while (0)
#else
#define	WT_HAVE_DIAGNOSTIC_YIELD
#endif

/* Return and branch-to-err-label cases for switch statements. */
#define	WT_ILLEGAL_VALUE(session)					\
	default:							\
		return (__wt_illegal_value(session, NULL))
#define	WT_ILLEGAL_VALUE_ERR(session)					\
	default:							\
		ret = __wt_illegal_value(session, NULL);		\
		goto err

/* Set "ret" and branch-to-err-label tests. */
#define	WT_ERR(a) do {							\
	if ((ret = (a)) != 0)						\
		goto err;						\
} while (0)
#define	WT_ERR_MSG(session, v, ...) do {				\
	ret = (v);							\
	__wt_err(session, ret, __VA_ARGS__);				\
	goto err;							\
} while (0)
#define	WT_ERR_NOTFOUND_OK(a) do {					\
	if ((ret = (a)) != 0) {						\
		if (ret == WT_NOTFOUND)					\
			ret = 0;					\
		else							\
			goto err;					\
	}								\
} while (0)
#define	WT_ERR_TEST(a, v) do {						\
	if (a) {							\
		ret = (v);						\
		goto err;						\
	}								\
} while (0)

/* Return tests. */
#define	WT_RET(a) do {							\
	int __ret;							\
	if ((__ret = (a)) != 0)						\
		return (__ret);						\
} while (0)
#define	WT_RET_TEST(a, v) do {						\
	if (a)								\
		return (v);						\
} while (0)
#define	WT_RET_MSG(session, v, ...) do {				\
	int __ret = (v);						\
	__wt_err(session, __ret, __VA_ARGS__);				\
	return (__ret);							\
} while (0)
#define	WT_RET_NOTFOUND_OK(a) do {					\
	int __ret;							\
	if ((__ret = (a)) != 0 && __ret != WT_NOTFOUND)			\
		return (__ret);						\
} while (0)

/* Set "ret" if not already set. */
#define	WT_TRET(a) do {							\
	int __ret;							\
	if ((__ret = (a)) != 0 &&					\
	    (ret == 0 || ret == WT_DUPLICATE_KEY || ret == WT_NOTFOUND))\
		ret = __ret;						\
} while (0)
#define	WT_TRET_NOTFOUND_OK(a) do {					\
	int __ret;							\
	if ((__ret = (a)) != 0 && __ret != WT_NOTFOUND &&		\
	    (ret == 0 || ret == WT_DUPLICATE_KEY || ret == WT_NOTFOUND))\
		ret = __ret;						\
} while (0)

#define	WT_PANIC_ERR(session, v, ...) do {				\
	__wt_err(session, v, __VA_ARGS__);				\
	(void)__wt_panic(session);					\
} while (0)
#define	WT_PANIC_RETX(session, ...) do {				\
	__wt_errx(session, __VA_ARGS__);				\
	/* Return WT_PANIC regardless of earlier return codes. */	\
	return (__wt_panic(session));					\
} while (0)

/*
 * WT_ASSERT, WT_ASSERT_ERR, WT_ASSERT_RET --
 *	Assert an expression, abort in diagnostic mode, otherwise, optionally
 *	return an error.
 */
#define	WT_ASSERT(session, exp) do {					\
	if (!(exp))							\
		__wt_assert(session, 0, __FILE__, __LINE__, "%s", #exp);\
} while (0)
#define	WT_ASSERT_ERR(session, exp) do {				\
	if (!(exp)) {							\
		__wt_assert(						\
		    session, WT_ERROR, __FILE__, __LINE__, "%s", #exp);	\
		WT_ERR(WT_ERROR);					\
	}								\
} while (0)
#define	WT_ASSERT_RET(session, exp) do {				\
	if (!(exp)) {							\
		__wt_assert(						\
		    session, WT_ERROR, __FILE__, __LINE__, "%s", #exp);	\
		return (WT_ERROR);					\
	}								\
} while (0)
