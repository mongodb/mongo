/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * __wt_cond_wait --
 *     Wait on a mutex, optionally timing out.
 */
static inline void
__wt_cond_wait(
  WT_SESSION_IMPL *session, WT_CONDVAR *cond, uint64_t usecs, bool (*run_func)(WT_SESSION_IMPL *))
{
    bool notused;

    __wt_cond_wait_signal(session, cond, usecs, run_func, &notused);
}

/*
 * __wt_hex --
 *     Convert a byte to a hex character.
 */
static inline u_char
__wt_hex(int c)
{
    return ((u_char) "0123456789abcdef"[c]);
}

/*
 * __wt_safe_sub --
 *     Subtract unsigned integers, rounding to zero if the result would be negative.
 */
static inline uint64_t
__wt_safe_sub(uint64_t v1, uint64_t v2)
{
    return (v2 > v1 ? 0 : v1 - v2);
}

/*
 * __wt_strdup --
 *     ANSI strdup function.
 */
static inline int
__wt_strdup(WT_SESSION_IMPL *session, const char *str, void *retp)
{
    return (__wt_strndup(session, str, (str == NULL) ? 0 : strlen(str), retp));
}

/*
 * __wt_strnlen --
 *     Determine the length of a fixed-size string
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
 *     snprintf convenience function, ignoring the returned size.
 */
static inline int
__wt_snprintf(char *buf, size_t size, const char *fmt, ...)
  WT_GCC_FUNC_ATTRIBUTE((format(printf, 3, 4)))
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
 *     vsnprintf convenience function, ignoring the returned size.
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
 *     snprintf convenience function, setting the returned size.
 */
static inline int
__wt_snprintf_len_set(char *buf, size_t size, size_t *retsizep, const char *fmt, ...)
  WT_GCC_FUNC_ATTRIBUTE((format(printf, 4, 5)))
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
 *     vsnprintf convenience function, setting the returned size.
 */
static inline int
__wt_vsnprintf_len_set(char *buf, size_t size, size_t *retsizep, const char *fmt, va_list ap)
{
    *retsizep = 0;

    return (__wt_vsnprintf_len_incr(buf, size, retsizep, fmt, ap));
}

/*
 * __wt_snprintf_len_incr --
 *     snprintf convenience function, incrementing the returned size.
 */
static inline int
__wt_snprintf_len_incr(char *buf, size_t size, size_t *retsizep, const char *fmt, ...)
  WT_GCC_FUNC_ATTRIBUTE((format(printf, 4, 5)))
{
    WT_DECL_RET;
    va_list ap;

    va_start(ap, fmt);
    ret = __wt_vsnprintf_len_incr(buf, size, retsizep, fmt, ap);
    va_end(ap);
    return (ret);
}

/*
 * __wt_spin_backoff --
 *     Back off while spinning for a resource. This is used to avoid busy waiting loops that can
 *     consume enough CPU to block real work being done. The algorithm spins a few times, then
 *     yields for a while, then falls back to sleeping.
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

/* Maximum stress delay is 1/10 of a second. */
#define WT_TIMING_STRESS_MAX_DELAY (100000)

/*
 * __wt_timing_stress --
 *     Optionally add delay to stress code paths.
 */
static inline void
__wt_timing_stress(WT_SESSION_IMPL *session, u_int flag)
{
    double pct;
    uint64_t i, max;

    /* Optionally only sleep when a specified configuration flag is set. */
    if (flag != 0 && !FLD_ISSET(S2C(session)->timing_stress_flags, flag))
        return;

    /*
     * If there is a lot of cache pressure, don't let the sleep time get too large. If the cache is
     * totally full, return.
     */
    pct = 0.0;
    if (__wt_eviction_needed(session, false, false, &pct))
        max = 5;
    else
        max = 9;
    if (pct > 100.0)
        return;

    /*
     * We need a fast way to choose a sleep time. We want to sleep a short period most of the time,
     * but occasionally wait longer. Divide the maximum period of time into 10 buckets (where bucket
     * 0 doesn't sleep at all), and roll dice, advancing to the next bucket 50% of the time. That
     * means we'll hit the maximum roughly every 1K calls.
     */
    for (i = 0;;)
        if (__wt_random(&session->rnd) & 0x1 || ++i > max)
            break;

    if (i == 0)
        __wt_yield();
    else
        /* The default maximum delay is 1/10th of a second. */
        __wt_sleep(0, i * (WT_TIMING_STRESS_MAX_DELAY / 10));
}

/*
 * __wt_failpoint --
 *     A generic failpoint function, it will return true if the failpoint triggers. Takes an
 *     unsigned integer from 0 to 10000 representing an X in 10000 chance of occurring.
 */
static inline bool
__wt_failpoint(WT_SESSION_IMPL *session, uint64_t conn_flag, u_int probability)
{
    WT_CONNECTION_IMPL *conn;

    conn = S2C(session);

    if (!FLD_ISSET(conn->timing_stress_flags, conn_flag))
        return (false);

    /* Assert that the given probability is sane. */
    WT_ASSERT(session, probability <= 10000);

    return (__wt_random(&session->rnd) % 10000 <= probability);
}

/*
 * The hardware-accelerated checksum code that originally shipped on Windows did not correctly
 * handle memory that wasn't 8B aligned and a multiple of 8B. It's likely that calculations were
 * always 8B aligned, but there's some risk.
 *
 * What we do is always write the correct checksum, and if a checksum test fails, check it against
 * the alternate version have before failing.
 */

#if defined(_M_AMD64) && !defined(HAVE_NO_CRC32_HARDWARE)
/*
 * __wt_checksum_match --
 *     Return if a checksum matches either the primary or alternate values.
 */
static inline bool
__wt_checksum_match(const void *chunk, size_t len, uint32_t v)
{
    return (__wt_checksum(chunk, len) == v || __wt_checksum_alt_match(chunk, len, v));
}

#else

/*
 * __wt_checksum_match --
 *     Return if a checksum matches.
 */
static inline bool
__wt_checksum_match(const void *chunk, size_t len, uint32_t v)
{
    return (__wt_checksum(chunk, len) == v);
}
#endif
