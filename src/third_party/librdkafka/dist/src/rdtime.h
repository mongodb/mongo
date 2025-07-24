/*
 * librd - Rapid Development C library
 *
 * Copyright (c) 2012-2022, Magnus Edenhill
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef _RDTIME_H_
#define _RDTIME_H_


#ifndef TIMEVAL_TO_TIMESPEC
#define TIMEVAL_TO_TIMESPEC(tv, ts)                                            \
        do {                                                                   \
                (ts)->tv_sec  = (tv)->tv_sec;                                  \
                (ts)->tv_nsec = (tv)->tv_usec * 1000;                          \
        } while (0)

#define TIMESPEC_TO_TIMEVAL(tv, ts)                                            \
        do {                                                                   \
                (tv)->tv_sec  = (ts)->tv_sec;                                  \
                (tv)->tv_usec = (ts)->tv_nsec / 1000;                          \
        } while (0)
#endif

#define TIMESPEC_TO_TS(ts)                                                     \
        (((rd_ts_t)(ts)->tv_sec * 1000000LLU) + ((ts)->tv_nsec / 1000))

#define TS_TO_TIMESPEC(ts, tsx)                                                \
        do {                                                                   \
                (ts)->tv_sec  = (tsx) / 1000000;                               \
                (ts)->tv_nsec = ((tsx) % 1000000) * 1000;                      \
                if ((ts)->tv_nsec >= 1000000000LLU) {                          \
                        (ts)->tv_sec++;                                        \
                        (ts)->tv_nsec -= 1000000000LLU;                        \
                }                                                              \
        } while (0)

#define TIMESPEC_CLEAR(ts) ((ts)->tv_sec = (ts)->tv_nsec = 0LLU)


#define RD_POLL_INFINITE -1
#define RD_POLL_NOWAIT   0


#if RD_UNITTEST_QPC_OVERRIDES
/* Overrides for rd_clock() unittest using QPC on Windows */
BOOL rd_ut_QueryPerformanceFrequency(_Out_ LARGE_INTEGER *lpFrequency);
BOOL rd_ut_QueryPerformanceCounter(_Out_ LARGE_INTEGER *lpPerformanceCount);
#define rd_QueryPerformanceFrequency(IFREQ)                                    \
        rd_ut_QueryPerformanceFrequency(IFREQ)
#define rd_QueryPerformanceCounter(PC) rd_ut_QueryPerformanceCounter(PC)
#else
#define rd_QueryPerformanceFrequency(IFREQ) QueryPerformanceFrequency(IFREQ)
#define rd_QueryPerformanceCounter(PC)      QueryPerformanceCounter(PC)
#endif

/**
 * @returns a monotonically increasing clock in microseconds.
 * @remark There is no monotonic clock on OSX, the system time
 *         is returned instead.
 */
static RD_INLINE rd_ts_t rd_clock(void) RD_UNUSED;
static RD_INLINE rd_ts_t rd_clock(void) {
#if defined(__APPLE__) || (defined(__ANDROID__) && __ANDROID_API__ < 29)
        /* No monotonic clock on Darwin */
        struct timeval tv;
        gettimeofday(&tv, NULL);
        return ((rd_ts_t)tv.tv_sec * 1000000LLU) + (rd_ts_t)tv.tv_usec;
#elif defined(_WIN32)
        LARGE_INTEGER now;
        static RD_TLS double freq = 0.0;
        if (!freq) {
                LARGE_INTEGER ifreq;
                rd_QueryPerformanceFrequency(&ifreq);
                /* Convert frequency to double to avoid overflow in
                 * return statement */
                freq = (double)ifreq.QuadPart / 1000000.0;
        }
        rd_QueryPerformanceCounter(&now);
        return (rd_ts_t)((double)now.QuadPart / freq);
#else
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return ((rd_ts_t)ts.tv_sec * 1000000LLU) +
               ((rd_ts_t)ts.tv_nsec / 1000LLU);
#endif
}


/**
 * @returns UTC wallclock time as number of microseconds since
 *          beginning of the epoch.
 */
static RD_INLINE RD_UNUSED rd_ts_t rd_uclock(void) {
        struct timeval tv;
        rd_gettimeofday(&tv, NULL);
        return ((rd_ts_t)tv.tv_sec * 1000000LLU) + (rd_ts_t)tv.tv_usec;
}



/**
 * Thread-safe version of ctime() that strips the trailing newline.
 */
static RD_INLINE const char *rd_ctime(const time_t *t) RD_UNUSED;
static RD_INLINE const char *rd_ctime(const time_t *t) {
        static RD_TLS char ret[27];

#ifndef _WIN32
        ctime_r(t, ret);
#else
        ctime_s(ret, sizeof(ret), t);
#endif
        ret[25] = '\0';

        return ret;
}


/**
 * @brief Convert a relative millisecond timeout to microseconds,
 *        properly handling RD_POLL_NOWAIT, et.al.
 */
static RD_INLINE rd_ts_t rd_timeout_us(int timeout_ms) {
        if (timeout_ms <= 0)
                return (rd_ts_t)timeout_ms;
        else
                return (rd_ts_t)timeout_ms * 1000;
}

/**
 * @brief Convert a relative microsecond timeout to milliseconds,
 *        properly handling RD_POLL_NOWAIT, et.al.
 */
static RD_INLINE int rd_timeout_ms(rd_ts_t timeout_us) {
        if (timeout_us <= 0)
                return (int)timeout_us;
        else
                /* + 999: Round up to millisecond to
                 * avoid busy-looping during the last
                 * millisecond. */
                return (int)((timeout_us + 999) / 1000);
}

/**
 * @brief Initialize an absolute timeout based on the provided \p timeout_ms
 *        and given clock \p now
 *
 * To be used with rd_timeout_adjust().
 *
 * Honours RD_POLL_INFINITE, RD_POLL_NOWAIT.
 *
 * @returns the absolute timeout which should later be passed
 *          to rd_timeout_adjust().
 */
static RD_INLINE rd_ts_t rd_timeout_init0(rd_ts_t now, int timeout_ms) {
        if (timeout_ms == RD_POLL_INFINITE || timeout_ms == RD_POLL_NOWAIT)
                return timeout_ms;

        return now + ((rd_ts_t)timeout_ms * 1000);
}


/**
 * @brief Initialize an absolute timeout based on the provided \p timeout_ms
 *        and current clock.
 *
 * To be used with rd_timeout_adjust().
 *
 * Honours RD_POLL_INFINITE, RD_POLL_NOWAIT.
 *
 * @returns the absolute timeout which should later be passed
 *          to rd_timeout_adjust().
 */
static RD_INLINE rd_ts_t rd_timeout_init(int timeout_ms) {
        return rd_timeout_init0(rd_clock(), timeout_ms);
}


/**
 * @brief Initialize an absolute timespec timeout based on the provided
 *        relative \p timeout_us.
 *
 * To be used with cnd_timedwait_abs().
 *
 * Honours RD_POLL_INFITE and RD_POLL_NOWAIT (reflected in tspec.tv_sec).
 */
static RD_INLINE void rd_timeout_init_timespec_us(struct timespec *tspec,
                                                  rd_ts_t timeout_us) {
        if (timeout_us == RD_POLL_INFINITE || timeout_us == RD_POLL_NOWAIT) {
                tspec->tv_sec  = timeout_us;
                tspec->tv_nsec = 0;
        } else {
#if defined(__APPLE__) || (defined(__ANDROID__) && __ANDROID_API__ < 29)
                struct timeval tv;
                gettimeofday(&tv, NULL);
                TIMEVAL_TO_TIMESPEC(&tv, tspec);
#else
                timespec_get(tspec, TIME_UTC);
#endif
                tspec->tv_sec += timeout_us / 1000000;
                tspec->tv_nsec += (timeout_us % 1000000) * 1000;
                if (tspec->tv_nsec >= 1000000000) {
                        tspec->tv_nsec -= 1000000000;
                        tspec->tv_sec++;
                }
        }
}

/**
 * @brief Initialize an absolute timespec timeout based on the provided
 *        relative \p timeout_ms.
 *
 * To be used with cnd_timedwait_abs().
 *
 * Honours RD_POLL_INFITE and RD_POLL_NOWAIT (reflected in tspec.tv_sec).
 */
static RD_INLINE void rd_timeout_init_timespec(struct timespec *tspec,
                                               int timeout_ms) {
        if (timeout_ms == RD_POLL_INFINITE || timeout_ms == RD_POLL_NOWAIT) {
                tspec->tv_sec  = timeout_ms;
                tspec->tv_nsec = 0;
        } else {
#if defined(__APPLE__) || (defined(__ANDROID__) && __ANDROID_API__ < 29)
                struct timeval tv;
                gettimeofday(&tv, NULL);
                TIMEVAL_TO_TIMESPEC(&tv, tspec);
#else
                timespec_get(tspec, TIME_UTC);
#endif
                tspec->tv_sec += timeout_ms / 1000;
                tspec->tv_nsec += (timeout_ms % 1000) * 1000000;
                if (tspec->tv_nsec >= 1000000000) {
                        tspec->tv_nsec -= 1000000000;
                        tspec->tv_sec++;
                }
        }
}


/**
 * @brief Same as rd_timeout_remains() but with microsecond precision
 */
static RD_INLINE rd_ts_t rd_timeout_remains_us(rd_ts_t abs_timeout) {
        rd_ts_t timeout_us;

        if (abs_timeout == RD_POLL_INFINITE || abs_timeout == RD_POLL_NOWAIT)
                return (rd_ts_t)abs_timeout;

        timeout_us = abs_timeout - rd_clock();
        if (timeout_us <= 0)
                return RD_POLL_NOWAIT;
        else
                return timeout_us;
}

/**
 * @returns the remaining timeout for timeout \p abs_timeout previously set
 *          up by rd_timeout_init()
 *
 * Honours RD_POLL_INFINITE, RD_POLL_NOWAIT.
 *
 * @remark Check explicitly for 0 (NOWAIT) to check if there is
 *         no remaining time to wait. Any other value, even negative (INFINITE),
 *         means there is remaining time.
 *         rd_timeout_expired() can be used to check the return value
 *         in a bool fashion.
 */
static RD_INLINE int rd_timeout_remains(rd_ts_t abs_timeout) {
        return rd_timeout_ms(rd_timeout_remains_us(abs_timeout));
}



/**
 * @brief Like rd_timeout_remains() but limits the maximum time to \p limit_ms,
 *        and operates on the return value of rd_timeout_remains().
 */
static RD_INLINE int rd_timeout_remains_limit0(int remains_ms, int limit_ms) {
        if (remains_ms == RD_POLL_INFINITE || remains_ms > limit_ms)
                return limit_ms;
        else
                return remains_ms;
}

/**
 * @brief Like rd_timeout_remains() but limits the maximum time to \p limit_ms
 */
static RD_INLINE int rd_timeout_remains_limit(rd_ts_t abs_timeout,
                                              int limit_ms) {
        return rd_timeout_remains_limit0(rd_timeout_remains(abs_timeout),
                                         limit_ms);
}

/**
 * @returns 1 if the **relative** timeout as returned by rd_timeout_remains()
 *          has timed out / expired, else 0.
 */
static RD_INLINE int rd_timeout_expired(int timeout_ms) {
        return timeout_ms == RD_POLL_NOWAIT;
}

#endif /* _RDTIME_H_ */
