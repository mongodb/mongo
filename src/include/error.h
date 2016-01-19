/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#define	WT_DEBUG_POINT	((void *)0xdeadbeef)
#define	WT_DEBUG_BYTE	(0xab)

/* In DIAGNOSTIC mode, yield in places where we want to encourage races. */
#ifdef HAVE_DIAGNOSTIC
#define	WT_DIAGNOSTIC_YIELD do {					\
	__wt_yield();							\
} while (0)
#else
#define	WT_DIAGNOSTIC_YIELD
#endif

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
#define	WT_ERR_TEST(a, v) do {						\
	if (a) {							\
		ret = (v);						\
		goto err;						\
	} else								\
		ret = 0;						\
} while (0)
#define	WT_ERR_ERROR_OK(a, e)						\
	WT_ERR_TEST((ret = (a)) != 0 && ret != (e), ret)
#define	WT_ERR_BUSY_OK(a)	WT_ERR_ERROR_OK(a, EBUSY)
#define	WT_ERR_NOTFOUND_OK(a)	WT_ERR_ERROR_OK(a, WT_NOTFOUND)

/* Return tests. */
#define	WT_RET(a) do {							\
	int __ret;							\
	if ((__ret = (a)) != 0)						\
		return (__ret);						\
} while (0)
#define	WT_RET_MSG(session, v, ...) do {				\
	int __ret = (v);						\
	__wt_err(session, __ret, __VA_ARGS__);				\
	return (__ret);							\
} while (0)
#define	WT_RET_TEST(a, v) do {						\
	if (a)								\
		return (v);						\
} while (0)
#define	WT_RET_ERROR_OK(a, e) do {					\
	int __ret = (a);						\
	WT_RET_TEST(__ret != 0 && __ret != (e), __ret);			\
} while (0)
#define	WT_RET_BUSY_OK(a)	WT_RET_ERROR_OK(a, EBUSY)
#define	WT_RET_NOTFOUND_OK(a)	WT_RET_ERROR_OK(a, WT_NOTFOUND)

/* Set "ret" if not already set. */
#define	WT_TRET(a) do {							\
	int __ret;							\
	if ((__ret = (a)) != 0 &&					\
	    (__ret == WT_PANIC ||					\
	    ret == 0 || ret == WT_DUPLICATE_KEY || ret == WT_NOTFOUND))	\
		ret = __ret;						\
} while (0)
#define	WT_TRET_ERROR_OK(a, e) do {					\
	int __ret;							\
	if ((__ret = (a)) != 0 && __ret != (e) &&			\
	    (__ret == WT_PANIC ||					\
	    ret == 0 || ret == WT_DUPLICATE_KEY || ret == WT_NOTFOUND))	\
		ret = __ret;						\
} while (0)
#define	WT_TRET_BUSY_OK(a)	WT_TRET_ERROR_OK(a, EBUSY)
#define	WT_TRET_NOTFOUND_OK(a)	WT_TRET_ERROR_OK(a, WT_NOTFOUND)

/* Return and branch-to-err-label cases for switch statements. */
#define	WT_ILLEGAL_VALUE(session)					\
	default:							\
		return (__wt_illegal_value(session, NULL))
#define	WT_ILLEGAL_VALUE_ERR(session)					\
	default:							\
		ret = __wt_illegal_value(session, NULL);		\
		goto err
#define	WT_ILLEGAL_VALUE_SET(session)					\
	default:							\
		ret = __wt_illegal_value(session, NULL);		\
		break

#define	WT_PANIC_MSG(session, v, ...) do {				\
	__wt_err(session, v, __VA_ARGS__);				\
	(void)__wt_panic(session);					\
} while (0)
#define	WT_PANIC_ERR(session, v, ...) do {				\
	WT_PANIC_MSG(session, v, __VA_ARGS__);				\
	WT_ERR(WT_PANIC);						\
} while (0)
#define	WT_PANIC_RET(session, v, ...) do {				\
	WT_PANIC_MSG(session, v, __VA_ARGS__);				\
	/* Return WT_PANIC regardless of earlier return codes. */	\
	return (WT_PANIC);						\
} while (0)

/*
 * WT_ASSERT
 *	Assert an expression, aborting in diagnostic mode.  Otherwise,
 * "use" the session to keep the compiler quiet and don't evaluate the
 * expression.
 */
#ifdef HAVE_DIAGNOSTIC
#define	WT_ASSERT(session, exp) do {					\
	if (!(exp))							\
		__wt_assert(session, 0, __FILE__, __LINE__, "%s", #exp);\
} while (0)
#else
#define	WT_ASSERT(session, exp)						\
	WT_UNUSED(session)
#endif
