/*-
 * Public Domain 2014-present MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */
#define	THOUSAND	(1000ULL)
#define	MILLION		(1000000ULL)
#define	BILLION		(1000000000ULL)

#define	NSEC_PER_SEC	BILLION
#define	USEC_PER_SEC	MILLION
#define	MSEC_PER_SEC	THOUSAND

#define	ns_to_ms(v)	((v) / MILLION)
#define	ns_to_sec(v)	((v) / BILLION)
#define	ns_to_us(v)	((v) / THOUSAND)

#define	us_to_ms(v)	((v) / THOUSAND)
#define	us_to_ns(v)	((v) * THOUSAND)
#define	us_to_sec(v)	((v) / MILLION)

#define	ms_to_ns(v)	((v) * MILLION)
#define	ms_to_us(v)	((v) * THOUSAND)
#define	ms_to_sec(v)	((v) / THOUSAND)

#define	sec_to_ns(v)	((v) * BILLION)
#define	sec_to_us(v)	((v) * MILLION)
#define	sec_to_ms(v)	((v) * THOUSAND)

inline std::ostream&
operator<<(std::ostream &os, const timespec &ts)
{
    char oldfill;
    std::streamsize oldwidth;

    os << ts.tv_sec << ".";
    oldfill = os.fill('0');
    oldwidth = os.width(3);
    os << (int)ns_to_ms(ts.tv_nsec);
    os.fill(oldfill);
    os.width(oldwidth);
    return (os);
}

inline timespec
operator-(const timespec &lhs, const timespec &rhs)
{
    timespec ts;

    if (lhs.tv_nsec < rhs.tv_nsec) {
	ts.tv_sec = lhs.tv_sec - rhs.tv_sec - 1;
	ts.tv_nsec = lhs.tv_nsec - rhs.tv_nsec + NSEC_PER_SEC;
    } else {
	ts.tv_sec = lhs.tv_sec - rhs.tv_sec;
	ts.tv_nsec = lhs.tv_nsec - rhs.tv_nsec;
    }
    return (ts);
}

inline timespec
operator+(const timespec &lhs, const int n)
{
    timespec ts = lhs;
    ts.tv_sec += n;
    return (ts);
}

inline bool
operator<(const timespec &lhs, const timespec &rhs)
{
    if (lhs.tv_sec == rhs.tv_sec)
	return (lhs.tv_nsec < rhs.tv_nsec);
    else
	return (lhs.tv_sec < rhs.tv_sec);
}

inline bool
operator>(const timespec &lhs, const timespec &rhs)
{
    if (lhs.tv_sec == rhs.tv_sec)
	return (lhs.tv_nsec > rhs.tv_nsec);
    else
	return (lhs.tv_sec > rhs.tv_sec);
}

inline bool
operator>=(const timespec &lhs, const timespec &rhs)
{
    return (!(lhs < rhs));
}

inline bool
operator<=(const timespec &lhs, const timespec &rhs)
{
    return (!(lhs > rhs));
}

inline bool
operator==(const timespec &lhs, int n)
{
    return (lhs.tv_sec == n && lhs.tv_nsec == 0);
}

inline bool
operator!=(const timespec &lhs, int n)
{
    return (lhs.tv_sec != n || lhs.tv_nsec != 0);
}

inline timespec &
operator+=(timespec &lhs, const int n)
{
    lhs.tv_sec += n;
    return (lhs);
}

inline bool
operator==(const timespec &lhs, const timespec &rhs)
{
    return (lhs.tv_sec == rhs.tv_sec && lhs.tv_nsec == rhs.tv_nsec);
}

inline timespec &
operator-=(timespec &lhs, const timespec &rhs)
{
    lhs.tv_sec -= rhs.tv_sec;
    lhs.tv_nsec -= rhs.tv_nsec;
    if (lhs.tv_nsec < 0) {
	lhs.tv_nsec += NSEC_PER_SEC;
	lhs.tv_sec -= 1;
    }
    return (lhs);
}

inline timespec
ts_add_ms(const timespec &lhs, const uint64_t n)
{
    timespec ts;

    ts.tv_sec = lhs.tv_sec + ms_to_sec(n);
    ts.tv_nsec = lhs.tv_nsec + ms_to_ns(n % THOUSAND);
    while ((unsigned long)ts.tv_nsec > NSEC_PER_SEC) {
	ts.tv_nsec -= NSEC_PER_SEC;
	ts.tv_sec++;
    }
    return (ts);
}

inline void
ts_assign(timespec &lhs, const timespec &rhs)
{
    lhs.tv_sec = rhs.tv_sec;
    lhs.tv_nsec = rhs.tv_nsec;
}

inline void
ts_clear(timespec &ts)
{
    ts.tv_sec = 0;
    ts.tv_nsec = 0;
}

inline uint64_t
ts_sec(const timespec &ts)
{
    return (ns_to_sec(ts.tv_nsec) + ts.tv_sec);
}

inline uint64_t
ts_ms(const timespec &ts)
{
    return (ns_to_ms(ts.tv_nsec) + sec_to_ms(ts.tv_sec));
}

inline uint64_t
ts_us(const timespec &ts)
{
    return (ns_to_us(ts.tv_nsec) + sec_to_us(ts.tv_sec));
}

inline uint64_t
secs_us(double secs)
{
    return (secs * USEC_PER_SEC);
}
