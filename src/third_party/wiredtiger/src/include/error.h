/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

#define WT_COMPAT_MSG_PREFIX "Version incompatibility detected: "

#define WT_DEBUG_POINT ((void *)(uintptr_t)0xdeadbeef)
#define WT_DEBUG_BYTE (0xab)

#ifdef HAVE_DIAGNOSTIC
#define WT_HAVE_ERROR_LOG
#endif

/* In DIAGNOSTIC mode, yield in places where we want to encourage races (except for with
 * antithesis). */
#if defined HAVE_DIAGNOSTIC && defined NON_BARRIER_DIAGNOSTIC_YIELDS && !defined ENABLE_ANTITHESIS
#define WT_DIAGNOSTIC_YIELD      \
    do {                         \
        __wt_yield_no_barrier(); \
    } while (0)
#elif defined HAVE_DIAGNOSTIC && !defined NON_BARRIER_DIAGNOSTIC_YIELDS && \
  !defined ENABLE_ANTITHESIS
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
    __wt_set_return_func(session, __PRETTY_FUNCTION__, __LINE__, error, #error)

#ifdef WT_HAVE_ERROR_LOG
#define __wt_error_log_add_helper(expr, error, suberror) \
    WT_IGNORE_RET(                                       \
      __wt_error_log_add(__FILE__, __PRETTY_FUNCTION__, __LINE__, expr, error, suberror))
#define __wt_error_log_clear_helper() __wt_error_log_clear()

/*
 * WT_ERROR_LOG_ADD --
 *     Add an entry to the error log. This is useful in places where we assign an error code
 * directly to "ret" and return it, instead of using the WT_ERR() macro. It is also useful in places
 * where we want to log an error but not return it.
 */
#define WT_ERROR_LOG_ADD(expr) \
    __wt_error_log_add(__FILE__, __PRETTY_FUNCTION__, __LINE__, #expr, expr, WT_NONE)

#else
#define __wt_error_log_add_helper(expr, error, suberror) \
    {                                                    \
    }
#define __wt_error_log_clear_helper() \
    {                                 \
    }
#define WT_ERROR_LOG_ADD(expr) (expr)
#endif /* WT_HAVE_ERROR_LOG */

/* Set "ret" and branch-to-err-label tests. */
#define WT_ERR(a)                                        \
    do {                                                 \
        if ((ret = (a)) != 0) {                          \
            __wt_error_log_add_helper(#a, ret, WT_NONE); \
            goto err;                                    \
        }                                                \
    } while (0)
#define WT_ERR_NOLOG(a)       \
    do {                      \
        if ((ret = (a)) != 0) \
            goto err;         \
    } while (0)
#define WT_ERR_MSG(session, v, ...)                                    \
    do {                                                               \
        ret = (v);                                                     \
        __wt_error_log_add_helper(#v, ret, WT_NONE);                   \
        __wt_err(session, ret, __VA_ARGS__);                           \
        __wt_session_set_last_error(session, v, WT_NONE, __VA_ARGS__); \
        goto err;                                                      \
    } while (0)
#define WT_ERR_SUB(session, v, sub_v, ...)                           \
    do {                                                             \
        ret = (v);                                                   \
        __wt_error_log_add_helper(#v, ret, sub_v);                   \
        __wt_session_set_last_error(session, v, sub_v, __VA_ARGS__); \
        goto err;                                                    \
    } while (0)
#define WT_ERR_TEST(a, v, keep)                          \
    do {                                                 \
        if (a) {                                         \
            ret = (v);                                   \
            __wt_error_log_add_helper(#v, ret, WT_NONE); \
            goto err;                                    \
        } else if (!(keep))                              \
            ret = 0;                                     \
    } while (0)
#define WT_ERR_MSG_CHK(session, v, ...)            \
    do {                                           \
        ret = (v);                                 \
        if (ret != 0)                              \
            WT_ERR_MSG(session, ret, __VA_ARGS__); \
    } while (0)
#define WT_ERR_ERROR_OK(a, e, keep) WT_ERR_TEST((ret = (a)) != 0 && ret != (e), ret, keep)
#define WT_ERR_NOTFOUND_OK(a, keep) WT_ERR_ERROR_OK(a, WT_NOTFOUND, keep)

/* Return WT_PANIC regardless of earlier return codes. */
#define WT_ERR_PANIC(session, v, ...) WT_ERR(__wt_panic(session, v, __VA_ARGS__))

/* Return tests. */
#define WT_RET(a)                                          \
    do {                                                   \
        int __ret;                                         \
        if ((__ret = (a)) != 0) {                          \
            __wt_error_log_add_helper(#a, __ret, WT_NONE); \
            return (__ret);                                \
        }                                                  \
    } while (0)
#define WT_RET_NOLOG(a)         \
    do {                        \
        int __ret;              \
        if ((__ret = (a)) != 0) \
            return (__ret);     \
    } while (0)
#define WT_RET_TRACK(a)                                    \
    do {                                                   \
        int __ret;                                         \
        if ((__ret = (a)) != 0) {                          \
            __wt_error_log_add_helper(#a, __ret, WT_NONE); \
            WT_TRACK_OP_END(session);                      \
            return (__ret);                                \
        }                                                  \
    } while (0)
#define WT_RET_MSG(session, v, ...)                                    \
    do {                                                               \
        int __ret = (v);                                               \
        __wt_error_log_add_helper(#v, __ret, WT_NONE);                 \
        __wt_err(session, __ret, __VA_ARGS__);                         \
        __wt_session_set_last_error(session, v, WT_NONE, __VA_ARGS__); \
        return (__ret);                                                \
    } while (0)
#define WT_RET_SUB(session, v, sub_v, ...)                           \
    do {                                                             \
        int __ret = (v);                                             \
        __wt_error_log_add_helper(#v, __ret, sub_v);                 \
        __wt_session_set_last_error(session, v, sub_v, __VA_ARGS__); \
        return (__ret);                                              \
    } while (0)
#define WT_RET_TEST(a, v)                              \
    do {                                               \
        if (a) {                                       \
            __wt_error_log_add_helper(#a, v, WT_NONE); \
            return (v);                                \
        }                                              \
    } while (0)
#define WT_RET_ERROR_OK(a, e)                           \
    do {                                                \
        int __ret = (a);                                \
        WT_RET_TEST(__ret != 0 && __ret != (e), __ret); \
    } while (0)
#define WT_RET_BUSY_OK(a) WT_RET_ERROR_OK(a, EBUSY)
#define WT_RET_NOTFOUND_OK(a) WT_RET_ERROR_OK(a, WT_NOTFOUND)
#define WT_RET_ONLY(a, e)             \
    do {                              \
        int __ret = (a);              \
        WT_RET_TEST(__ret == (e), e); \
    } while (0)

#ifdef INLINE_FUNCTIONS_INSTEAD_OF_MACROS
/* Set "ret" if not already set. */
static WT_INLINE void
__wt_tret(int *pret, int a)
{
    int __ret;
    WT_DECL_RET;

    ret = *pret;
    if ((__ret = (a)) != 0 &&
      (__ret == WT_PANIC || ret == 0 || ret == WT_DUPLICATE_KEY || ret == WT_NOTFOUND ||
        ret == WT_RESTART))
        *pret = __ret;
}
#define WT_TRET(a) __wt_tret(&ret, a)

static WT_INLINE void
__wt_tret_error_ok(int *pret, int a, int e)
{
    int __ret;
    WT_DECL_RET;

    ret = *pret;
    if ((__ret = (a)) != 0 && __ret != (e) &&
      (__ret == WT_PANIC || ret == 0 || ret == WT_DUPLICATE_KEY || ret == WT_NOTFOUND ||
        ret == WT_RESTART))
        *pret = __ret;
}
#define WT_TRET_ERROR_OK(a, e) __wt_tret_error_ok(&ret, a, e)
#else
/* Set "ret" if not already set. */
#define WT_TRET(a)                                                                                \
    do {                                                                                          \
        int __ret;                                                                                \
        if ((__ret = (a)) != 0) {                                                                 \
            __wt_error_log_add_helper(#a, __ret, WT_NONE);                                        \
            if (__ret == WT_PANIC || ret == 0 || ret == WT_DUPLICATE_KEY || ret == WT_NOTFOUND || \
              ret == WT_RESTART)                                                                  \
                ret = __ret;                                                                      \
        }                                                                                         \
    } while (0)
#define WT_TRET_ERROR_OK(a, e)                                                                    \
    do {                                                                                          \
        int __ret;                                                                                \
        if ((__ret = (a)) != 0 && __ret != (e)) {                                                 \
            __wt_error_log_add_helper(#a, __ret, WT_NONE);                                        \
            if (__ret == WT_PANIC || ret == 0 || ret == WT_DUPLICATE_KEY || ret == WT_NOTFOUND || \
              ret == WT_RESTART)                                                                  \
                ret = __ret;                                                                      \
        }                                                                                         \
    } while (0)
#endif /* INLINE_FUNCTIONS_INSTEAD_OF_MACROS */

#define WT_TRET_BUSY_OK(a) WT_TRET_ERROR_OK(a, EBUSY)
#define WT_TRET_NOTFOUND_OK(a) WT_TRET_ERROR_OK(a, WT_NOTFOUND)

/* Return WT_PANIC regardless of earlier return codes. */
#define WT_RET_PANIC(session, v, ...) return (__wt_panic(session, v, __VA_ARGS__))

/* Called on unexpected code path: locate the failure. */
#define __wt_illegal_value(session, v)             \
    __wt_panic(session, EINVAL, "%s: 0x%" PRIxMAX, \
      "encountered an illegal file format or internal value", (uintmax_t)(v))

/*
 * Branch prediction hints. If an expression is likely to return true/false we can use this
 * information to improve performance at runtime. This is not supported for MSVC compilers.
 */
#if !defined(_MSC_VER)
#define WT_LIKELY(x) __builtin_expect(!!(x), 1)
#define WT_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define WT_LIKELY(x) (x)
#define WT_UNLIKELY(x) (x)
#endif

#define WT_ERR_MSG_BUF_LEN 1024

/*
 * BUILD_ASSERTION_STRING --
 *  Append a common prefix to an assertion message and save into the provided buffer.
 */
#define BUILD_ASSERTION_STRING(session, buf, len, exp, ...)                                        \
    do {                                                                                           \
        size_t _offset;                                                                            \
        _offset = 0;                                                                               \
        WT_IGNORE_RET(                                                                             \
          __wt_snprintf_len_set(buf, len, &_offset, "WiredTiger assertion failed: '%s'. ", #exp)); \
        /* If we would overflow, finish with what we have. */                                      \
        if (_offset < len)                                                                         \
            WT_IGNORE_RET(__wt_snprintf(buf + _offset, len - _offset, __VA_ARGS__));               \
    } while (0)

/*
 * TRIGGER_ABORT --
 *  Abort the program.
 *
 * When unit testing assertions we don't want to call __wt_abort, but we do want to track that we
 * should have done so.
 */
#ifdef HAVE_UNITTEST_ASSERTS
#define TRIGGER_ABORT(session, exp, ...)                                                    \
    do {                                                                                    \
        if ((session) == NULL) {                                                            \
            __wt_errx(                                                                      \
              session, "A non-NULL session must be provided when unit testing assertions"); \
            __wt_abort(session);                                                            \
        }                                                                                   \
        BUILD_ASSERTION_STRING(                                                             \
          session, (session)->unittest_assert_msg, WT_ERR_MSG_BUF_LEN, exp, __VA_ARGS__);   \
        (session)->unittest_assert_hit = true;                                              \
    } while (0)
#else
#define TRIGGER_ABORT(session, exp, ...)                                             \
    do {                                                                             \
        char _buf[WT_ERR_MSG_BUF_LEN];                                               \
        BUILD_ASSERTION_STRING(session, _buf, WT_ERR_MSG_BUF_LEN, exp, __VA_ARGS__); \
        __wt_errx(session, "%s", _buf);                                              \
        __wt_abort(session);                                                         \
    } while (0)
#endif

/*
 * EXTRA_DIAGNOSTICS_ENABLED --
 *  Fetch whether diagnostic asserts for the provided category are runtime enabled.
 *  When compiled with HAVE_DIAGNOSTIC=1, the WT_DIAGNOSTIC_ALL category is always set on
 *  the connection and this function will always return true for non-null sessions.
 */
#define EXTRA_DIAGNOSTICS_ENABLED(session, category) \
    ((session != NULL) &&                            \
      WT_UNLIKELY(FLD_ISSET(S2C(session)->extra_diagnostics_flags, category | WT_DIAGNOSTIC_ALL)))

/*
 * WT_ASSERT --
 *  Assert an expression and abort if it fails.
 *  Only enabled when compiled with HAVE_DIAGNOSTIC=1.
 */
#ifdef HAVE_DIAGNOSTIC
#define WT_ASSERT(session, exp)                                       \
    do {                                                              \
        if (WT_UNLIKELY(!(exp)))                                      \
            TRIGGER_ABORT(session, exp, "Expression returned false"); \
    } while (0)
#else
#define WT_ASSERT(session, exp) WT_UNUSED(session)
#endif

/*
 * WT_ASSERT_OPTIONAL --
 *  Assert an expression if the relevant assertion category is enabled.
 */
#define WT_ASSERT_OPTIONAL(session, category, exp, ...)                \
    do {                                                               \
        if (WT_UNLIKELY(EXTRA_DIAGNOSTICS_ENABLED(session, category))) \
            if (WT_UNLIKELY(!(exp)))                                   \
                TRIGGER_ABORT(session, exp, __VA_ARGS__);              \
    } while (0)

/*
 * WT_ASSERT_ALWAYS --
 *  Assert an expression. This is enabled regardless of configuration.
 */
#define WT_ASSERT_ALWAYS(session, exp, ...)           \
    do {                                              \
        if (WT_UNLIKELY(!(exp)))                      \
            TRIGGER_ABORT(session, exp, __VA_ARGS__); \
    } while (0)

/*
 * WT_ERR_ASSERT --
 *  Assert an expression. If the relevant assertion category is
 *  enabled abort the program, otherwise print a message and return WT_ERR.
 */
#define WT_ERR_ASSERT(session, category, exp, v, ...)         \
    do {                                                      \
        if (WT_UNLIKELY(!(exp))) {                            \
            if (EXTRA_DIAGNOSTICS_ENABLED(session, category)) \
                TRIGGER_ABORT(session, exp, __VA_ARGS__);     \
            else                                              \
                WT_ERR_MSG(session, v, __VA_ARGS__);          \
        }                                                     \
    } while (0)

/*
 * WT_RET_ASSERT --
 *  Assert an expression. If the relevant assertion category is enabled
 *  abort the program, otherwise print a message and early return from the function.
 */
#define WT_RET_ASSERT(session, category, exp, v, ...)         \
    do {                                                      \
        if (WT_UNLIKELY(!(exp))) {                            \
            if (EXTRA_DIAGNOSTICS_ENABLED(session, category)) \
                TRIGGER_ABORT(session, exp, __VA_ARGS__);     \
            else                                              \
                WT_RET_MSG(session, v, __VA_ARGS__);          \
        }                                                     \
    } while (0)

/*
 * WT_RET_PANIC_ASSERT --
 *  Assert an expression. If the relevant assertion category is enabled
 *  abort the program, otherwise return WT_PANIC.
 */
#define WT_RET_PANIC_ASSERT(session, category, exp, v, ...)   \
    do {                                                      \
        if (WT_UNLIKELY(!(exp))) {                            \
            if (EXTRA_DIAGNOSTICS_ENABLED(session, category)) \
                TRIGGER_ABORT(session, exp, __VA_ARGS__);     \
            else                                              \
                WT_RET_PANIC(session, v, __VA_ARGS__);        \
        }                                                     \
    } while (0)

/*
 * WT_PREFETCH_ASSERT --
 *  Assert an expression for prefetch if in diagnostic mode, or update the relevant statistic if
 *  not. As pre-fetch is an optional optimization, we want to avoid crashing the application for
 *  an error, but instead swallow errors where possible.
 */
#define WT_PREFETCH_ASSERT(session, exp, stat) \
    do {                                       \
        if (!(exp))                            \
            WT_STAT_CONN_INCR(session, stat);  \
        WT_ASSERT(session, exp);               \
    } while (0)
