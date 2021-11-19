/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */
#define WT_COMPAT_MSG_PREFIX "Version incompatibility detected: "

#define WT_DEBUG_POINT ((void *)(uintptr_t)0xdeadbeef)
#define WT_DEBUG_BYTE (0xab)

/* In DIAGNOSTIC mode, yield in places where we want to encourage races. */
#ifdef HAVE_DIAGNOSTIC
#define WT_DIAGNOSTIC_YIELD \
    do {                    \
        __wt_yield();       \
    } while (0)
#else
#define WT_DIAGNOSTIC_YIELD
#endif

#define __wt_err(session, error, ...) \
    __wt_err_func(                    \
      session, error, __PRETTY_FUNCTION__, __LINE__, WT_VERBOSE_CATEGORY_DEFAULT, __VA_ARGS__)
#define __wt_errx(session, ...) \
    __wt_errx_func(session, __PRETTY_FUNCTION__, __LINE__, WT_VERBOSE_CATEGORY_DEFAULT, __VA_ARGS__)
#define __wt_panic(session, error, ...) \
    __wt_panic_func(                    \
      session, error, __PRETTY_FUNCTION__, __LINE__, WT_VERBOSE_CATEGORY_DEFAULT, __VA_ARGS__)
#define __wt_set_return(session, error) \
    __wt_set_return_func(session, __PRETTY_FUNCTION__, __LINE__, error)

/* Set "ret" and branch-to-err-label tests. */
#define WT_ERR(a)             \
    do {                      \
        if ((ret = (a)) != 0) \
            goto err;         \
    } while (0)
#define WT_ERR_MSG(session, v, ...)          \
    do {                                     \
        ret = (v);                           \
        __wt_err(session, ret, __VA_ARGS__); \
        goto err;                            \
    } while (0)
#define WT_ERR_TEST(a, v, keep) \
    do {                        \
        if (a) {                \
            ret = (v);          \
            goto err;           \
        } else if (!(keep))     \
            ret = 0;            \
    } while (0)
#define WT_ERR_ERROR_OK(a, e, keep) WT_ERR_TEST((ret = (a)) != 0 && ret != (e), ret, keep)
#define WT_ERR_NOTFOUND_OK(a, keep) WT_ERR_ERROR_OK(a, WT_NOTFOUND, keep)

/* Return WT_PANIC regardless of earlier return codes. */
#define WT_ERR_PANIC(session, v, ...) WT_ERR(__wt_panic(session, v, __VA_ARGS__))

/* Return tests. */
#define WT_RET(a)               \
    do {                        \
        int __ret;              \
        if ((__ret = (a)) != 0) \
            return (__ret);     \
    } while (0)
#define WT_RET_TRACK(a)               \
    do {                              \
        int __ret;                    \
        if ((__ret = (a)) != 0) {     \
            WT_TRACK_OP_END(session); \
            return (__ret);           \
        }                             \
    } while (0)
#define WT_RET_MSG(session, v, ...)            \
    do {                                       \
        int __ret = (v);                       \
        __wt_err(session, __ret, __VA_ARGS__); \
        return (__ret);                        \
    } while (0)
#define WT_RET_TEST(a, v) \
    do {                  \
        if (a)            \
            return (v);   \
    } while (0)
#define WT_RET_ERROR_OK(a, e)                           \
    do {                                                \
        int __ret = (a);                                \
        WT_RET_TEST(__ret != 0 && __ret != (e), __ret); \
    } while (0)
#define WT_RET_BUSY_OK(a) WT_RET_ERROR_OK(a, EBUSY)
#define WT_RET_NOTFOUND_OK(a) WT_RET_ERROR_OK(a, WT_NOTFOUND)
/* Set "ret" if not already set. */
#define WT_TRET(a)                                                                           \
    do {                                                                                     \
        int __ret;                                                                           \
        if ((__ret = (a)) != 0 &&                                                            \
          (__ret == WT_PANIC || ret == 0 || ret == WT_DUPLICATE_KEY || ret == WT_NOTFOUND || \
            ret == WT_RESTART))                                                              \
            ret = __ret;                                                                     \
    } while (0)
#define WT_TRET_ERROR_OK(a, e)                                                               \
    do {                                                                                     \
        int __ret;                                                                           \
        if ((__ret = (a)) != 0 && __ret != (e) &&                                            \
          (__ret == WT_PANIC || ret == 0 || ret == WT_DUPLICATE_KEY || ret == WT_NOTFOUND || \
            ret == WT_RESTART))                                                              \
            ret = __ret;                                                                     \
    } while (0)
#define WT_TRET_BUSY_OK(a) WT_TRET_ERROR_OK(a, EBUSY)
#define WT_TRET_NOTFOUND_OK(a) WT_TRET_ERROR_OK(a, WT_NOTFOUND)

/* Return WT_PANIC regardless of earlier return codes. */
#define WT_RET_PANIC(session, v, ...) return (__wt_panic(session, v, __VA_ARGS__))

/* Called on unexpected code path: locate the failure. */
#define __wt_illegal_value(session, v)             \
    __wt_panic(session, EINVAL, "%s: 0x%" PRIxMAX, \
      "encountered an illegal file format or internal value", (uintmax_t)(v))

/*
 * WT_ERR_ASSERT, WT_RET_ASSERT, WT_ASSERT
 *	Assert an expression, aborting in diagnostic mode and otherwise exiting
 * the function with an error. WT_ASSERT is deprecated, and should be used only
 * where required for performance.
 */
#ifdef HAVE_DIAGNOSTIC
#define WT_ASSERT(session, exp)             \
    do {                                    \
        if (!(exp)) {                       \
            __wt_errx(session, "%s", #exp); \
            __wt_abort(session);            \
        }                                   \
    } while (0)
#define WT_ERR_ASSERT(session, exp, v, ...)    \
    do {                                       \
        if (!(exp)) {                          \
            __wt_err(session, v, __VA_ARGS__); \
            __wt_abort(session);               \
        }                                      \
    } while (0)
#define WT_RET_ASSERT(session, exp, v, ...)    \
    do {                                       \
        if (!(exp)) {                          \
            __wt_err(session, v, __VA_ARGS__); \
            __wt_abort(session);               \
        }                                      \
    } while (0)
#define WT_RET_PANIC_ASSERT(session, exp, v, ...) \
    do {                                          \
        if (!(exp)) {                             \
            __wt_err(session, v, __VA_ARGS__);    \
            __wt_abort(session);                  \
        }                                         \
    } while (0)
#else
#define WT_ASSERT(session, exp) WT_UNUSED(session)
#define WT_ERR_ASSERT(session, exp, v, ...)      \
    do {                                         \
        if (!(exp))                              \
            WT_ERR_MSG(session, v, __VA_ARGS__); \
    } while (0)
#define WT_RET_ASSERT(session, exp, v, ...)      \
    do {                                         \
        if (!(exp))                              \
            WT_RET_MSG(session, v, __VA_ARGS__); \
    } while (0)
#define WT_RET_PANIC_ASSERT(session, exp, v, ...)  \
    do {                                           \
        if (!(exp))                                \
            WT_RET_PANIC(session, v, __VA_ARGS__); \
    } while (0)
#endif
