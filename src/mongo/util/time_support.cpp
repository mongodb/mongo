/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include <algorithm>

#include <boost/move/utility_core.hpp>
#include <fmt/compile.h>
#include <fmt/format.h>
#include <sys/types.h>
// IWYU pragma: no_include "bits/types/struct_tm.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/parse_number.h"
#include "mongo/base/status.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#include <cstdio>
#include <cstring>
#include <string>

#if defined(_WIN32)
#include "mongo/util/system_tick_source.h"
#include "mongo/util/timer.h"

#include <mmsystem.h>
#elif defined(__linux__)
#include <ctime>
#elif defined(__APPLE__)
#include <mach/clock.h>
#include <mach/mach.h>
#endif

#if !defined(_WIN32)
#include <sys/time.h>
#endif

/**
 * Unfortunately we cannot use the util/pcre.h wrapper because it depends on `//mongo/src:base`.
 * So as a special case, we must depend on third_party directly.
 */
#define PCRE2_CODE_UNIT_WIDTH 8  // Select 8-bit PCRE2 library.
#include <pcre2.h>

namespace mongo {

AtomicWord<long long> Date_t::lastNowVal;

Date_t Date_t::now() {
    decltype(lastNowVal)::WordType curTime = curTimeMillis64();
    auto oldLastNow = lastNowVal.loadRelaxed();

    // If curTime is different than old last now, unconditionally try to cas it to the new value.
    // This is an optimization to avoid performing stores for multiple clock reads in the same
    // millisecond.
    //
    // It's important that this is a non-equality (rather than a >), so that we avoid stalling time
    // if someone moves the system clock backwards.
    if (curTime != oldLastNow) {
        // If we fail to comp exchange, it means someone else concurrently called Date_t::now(), in
        // which case it's likely their time is also recent.  It's important that we don't loop so
        // that we avoid forcing time backwards if we have multiple callers at a millisecond
        // boundary.
        lastNowVal.compareAndSwap(&oldLastNow, curTime);
    }

    return fromMillisSinceEpoch(curTime);
}

Date_t::Date_t(stdx::chrono::system_clock::time_point tp)
    : millis(durationCount<Milliseconds>(tp - stdx::chrono::system_clock::from_time_t(0))) {}

stdx::chrono::system_clock::time_point Date_t::toSystemTimePoint() const {
    return stdx::chrono::system_clock::from_time_t(0) + toDurationSinceEpoch().toSystemDuration();
}

bool Date_t::isFormattable() const {
    if (millis < 0) {
        return false;
    }
    if (sizeof(time_t) == sizeof(int32_t)) {
        return millis < 2147483647000LL;  // "2038-01-19T03:14:07Z"
    } else {
        return millis < 32535215999000LL;  // "3000-12-31T23:59:59Z"
    }
}


void time_t_to_Struct(time_t t, struct tm* buf, bool local) {
    bool itWorked;
#if defined(_WIN32)
    if (local)
        itWorked = localtime_s(buf, &t) == 0;
    else
        itWorked = gmtime_s(buf, &t) == 0;
#else
    if (local)
        itWorked = localtime_r(&t, buf) != nullptr;
    else
        itWorked = gmtime_r(&t, buf) != nullptr;
#endif

    if (!itWorked) {
        if (t < 0) {
            // Windows docs say it doesn't support these, but empirically it seems to work
            uasserted(1125400, "gmtime failed - your system doesn't support dates before 1970");
        } else {
            uasserted(1125401, str::stream() << "gmtime failed to convert time_t of " << t);
        }
    }
}

std::string time_t_to_String_short(time_t t) {
    char buf[64];
    bool itWorked;
#if defined(_WIN32)
    itWorked = ctime_s(buf, sizeof(buf), &t) == 0;
#else
    itWorked = ctime_r(&t, buf) != nullptr;
#endif

    if (!itWorked) {
        if (t < 0) {
            // Windows docs say it doesn't support these, but empirically it seems to work
            uasserted(1125402, "ctime failed - your system doesn't support dates before 1970");
        } else {
            uasserted(1125403, str::stream() << "ctime failed to convert time_t of " << t);
        }
    }

    buf[19] = 0;
    if (buf[0] && buf[1] && buf[2] && buf[3])
        return buf + 4;  // skip day of week
    return buf;
}

constexpr auto kUTCFilenameFormat = "%Y-%m-%dT%H-%M-%S"_sd;
constexpr auto kUTCFilenameFormatZ = "%Y-%m-%dT%H-%M-%SZ"_sd;

// Produces a UTC datetime string suitable for use in filenames.
std::string terseCurrentTimeForFilename(bool appendZed) {
    struct tm t;
    time_t_to_Struct(time(nullptr), &t);

    const auto fmt = appendZed ? kUTCFilenameFormatZ : kUTCFilenameFormat;
    const std::size_t expLen = appendZed ? 20 : 19;

    char buf[32];
    fassert(16226, strftime(buf, sizeof(buf), fmt.data(), &t) == expLen);
    return buf;
}

DateStringBuffer& DateStringBuffer::iso8601(Date_t date, bool local) {
    invariant(date.isFormattable());

    struct tm t;
    time_t_to_Struct(date.toTimeT(), &t, local);

    char* cur = _data.data();
    char* end = _data.data() + _data.size();

    {
        static constexpr char kIsoDateFmtNoTz[] = "%Y-%m-%dT%H:%M:%S";
        size_t n = strftime(cur, end - cur, kIsoDateFmtNoTz, &t);
        dassert(n > 0);
        cur += n;
    }

    {
        auto res = fmt::format_to_n(cur, end - cur, FMT_COMPILE(".{:03}"), date.asInt64() % 1000);
        cur = res.out;
        dassert(cur < end && res.size > 0);
    }

    if (local) {
        static const size_t localTzSubstrLen = 6;
        dassert(static_cast<size_t>(end - cur) >= localTzSubstrLen + 1);
#ifdef _WIN32
        // NOTE(schwerin): The value stored by _get_timezone is the value one adds to local time
        // to get UTC.  This is opposite of the ISO-8601 meaning of the timezone offset.
        // NOTE(schwerin): Microsoft's timezone code always assumes US rules for daylight
        // savings time.  We can do no better without completely reimplementing localtime_s and
        // related time library functions.
        long msTimeZone;
        int ret = _get_timezone(&msTimeZone);
        if (ret != 0) {
            uasserted(1125404, str::stream() << "_get_timezone failed with errno: " << ret);
        }
        if (t.tm_isdst)
            msTimeZone -= 3600;
        const bool tzIsWestOfUTC = msTimeZone > 0;
        const long tzOffsetSeconds = msTimeZone * (tzIsWestOfUTC ? 1 : -1);
        const long tzOffsetHoursPart = tzOffsetSeconds / 3600;
        const long tzOffsetMinutesPart = (tzOffsetSeconds / 60) % 60;

        // "+hh:mm"
        cur = fmt::format_to_n(cur,
                               localTzSubstrLen + 1,
                               FMT_COMPILE("{}{:02}:{:02}"),
                               tzIsWestOfUTC ? '-' : '+',
                               tzOffsetHoursPart,
                               tzOffsetMinutesPart)
                  .out;
#else
        // ISO 8601 requires the timezone to be in hh:mm format which strftime can't produce
        // See https://tools.ietf.org/html/rfc3339#section-5.6
        strftime(cur, end - cur, "%z:", &t);
        // cur will be written as +hhmm:, transform to +hh:mm
        std::rotate(cur + 3, cur + 5, cur + 6);
        cur += 6;
#endif
    } else {
        dassert(cur + 2 <= end);
        *cur++ = 'Z';
    }

    dassert(cur <= end);
    _size = cur - _data.data();
    return *this;
}

DateStringBuffer& DateStringBuffer::ctime(Date_t date) {
    // "Wed Jun 30 21:49:08 1993\n" // full asctime/ctime format
    // "Wed Jun 30 21:49:08"        // clip after position 19.
    // "Wed Jun 30 21:49:08.996"    // append millis
    //  12345678901234567890123456
    time_t t = date.toTimeT();
    bool itWorked;
#if defined(_WIN32)
    itWorked = ctime_s(_data.data(), _data.size(), &t) == 0;
#else
    itWorked = ctime_r(&t, _data.data()) != nullptr;
#endif

    if (!itWorked) {
        if (t < 0) {
            // Windows docs say it doesn't support these, but empirically it seems to work
            uasserted(1125405, "ctime failed - your system doesn't support dates before 1970");
        } else {
            uasserted(1125406, str::stream() << "ctime failed to convert time_t of " << t);
        }
    }

    static constexpr size_t ctimeSubstrLen = 19;
    static constexpr size_t millisSubstrLen = 4;
    char* milliSecStr = _data.data() + ctimeSubstrLen;
    snprintf(milliSecStr,
             millisSubstrLen + 1,
             ".%03u",
             static_cast<unsigned>(date.toMillisSinceEpoch() % 1000));
    _size = ctimeSubstrLen + millisSubstrLen;
    return *this;
}

namespace {

#if defined(_WIN32)

uint64_t fileTimeToMicroseconds(FILETIME const ft) {
    // Microseconds between 1601-01-01 00:00:00 UTC and 1970-01-01 00:00:00 UTC
    constexpr uint64_t kEpochDifferenceMicros = 11644473600000000ull;

    // Construct a 64 bit value that is the number of nanoseconds from the
    // Windows epoch which is 1601-01-01 00:00:00 UTC
    auto totalMicros = static_cast<uint64_t>(ft.dwHighDateTime) << 32;
    totalMicros |= static_cast<uint64_t>(ft.dwLowDateTime);

    // FILETIME is 100's of nanoseconds since Windows epoch
    totalMicros /= 10;

    // Move it from micros since the Windows epoch to micros since the Unix epoch
    totalMicros -= kEpochDifferenceMicros;

    return totalMicros;
}

#endif

class QuickAndDirtyRegex {
public:
    class Match {
    public:
        Match(const pcre2_code* code, StringData input)
            : _m{pcre2_match_data_create_from_pattern(code, nullptr)},
              _input{input},
              _rc{pcre2_match(code,
                              reinterpret_cast<PCRE2_SPTR>(_input.data()),
                              _input.size(),
                              0,
                              0,
                              &*_m,
                              nullptr)} {}

        Match(Match&&) = delete;
        Match& operator=(Match&&) = delete;

        ~Match() {
            pcre2_match_data_free(_m);
        }

        int rc() const {
            return _rc;
        }

        StringData operator[](size_t i) const {
            iassert(ErrorCodes::NoSuchKey, "Match capture", i < pcre2_get_ovector_count(&*_m));
            size_t* p = pcre2_get_ovector_pointer(&*_m) + 2 * i;
            return p[0] == PCRE2_UNSET ? StringData{} : _input.substr(p[0], p[1] - p[0]);
        }

    private:
        pcre2_match_data* _m;
        StringData _input;
        int _rc;
    };

    explicit QuickAndDirtyRegex(StringData pattern)
        : _code{[&] {
              int err;
              size_t errPos;
              auto code = pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pattern.data()),
                                        pattern.size(),
                                        0,
                                        &err,
                                        &errPos,
                                        nullptr);
              invariant(code);
              return code;
          }()} {}

    QuickAndDirtyRegex(QuickAndDirtyRegex&&) = delete;
    QuickAndDirtyRegex& operator=(QuickAndDirtyRegex&&) = delete;

    ~QuickAndDirtyRegex() {
        pcre2_code_free(_code);
    }

    Match match(StringData input) const {
        return Match{_code, input};
    }

private:
    pcre2_code* _code;
};

struct ParsedTm {
    std::tm tm;
    Milliseconds millis;
    Seconds tzAdj;
};

ParsedTm parseTm(StringData dateString) {
    static const auto& re = *new QuickAndDirtyRegex{R"re((?x)
        ^
        (\d{4})-(\d{2})-(\d{2})        # mandatory YYYY-MM-DD
        (?:                            # maybe time
            T
            (\d{2}):(\d{2})            # hh:mm
            (?:                        # maybe seconds
                :(\d{2})               # :ss
                (?:\.(\d{1,3}))?       # maybe .nnn millis
            )?
        )?
        (?:                            # Z or [+-]hhmm or [+-]hh:mm
            Z |
            ([+-]) (\d{2}) :? (\d{2})
        )
        $
    )re"_sd};
    auto m = re.match(dateString);
    iassert(ErrorCodes::BadValue, fmt::format("failed match \'{}\'", dateString), m.rc() >= 0);
    ParsedTm result{};
    auto cap = [&](int i) {
        return i <= m.rc() ? m[i] : StringData{};
    };

    auto s2i = [](StringData s, StringData name, int min, int max) {
        int i = 0;
        iassert(NumberParser().base(10)(s, &i));
        iassert(ErrorCodes::BadValue,
                fmt::format("{} out of range:  {}", name, i),
                i >= min && i <= max);
        return i;
    };
    result.tm.tm_year = s2i(cap(1), "Year", 1970, 9999) - 1900;
    result.tm.tm_mon = s2i(cap(2), "Month", 1, 12) - 1;
    result.tm.tm_mday = s2i(cap(3), "Day", 1, 31);
    result.tm.tm_hour = s2i(cap(4), "Hour", 0, 23);
    result.tm.tm_min = s2i(cap(5), "Minute", 0, 59);
    if (auto secStr = cap(6); !secStr.empty())
        result.tm.tm_sec = s2i(secStr, "Second", 0, 59);
    if (auto millisStr = cap(7); !millisStr.empty()) {
        unsigned m = 0;
        iassert(NumberParser().base(10)(millisStr, &m));
        for (size_t i = millisStr.size(); i < 3; ++i)
            m *= 10;
        result.millis = Milliseconds{m};
    }
    if (auto signStr = cap(8); !signStr.empty()) {
        result.tzAdj = (signStr == "-" ? -1 : 1) *
            (Hours{s2i(cap(9), "Time zone hours adjustment", 0, 23)} +
             Minutes{s2i(cap(10), "Time zone minutes adjustment", 0, 59)});
    }

    return result;
}

}  // namespace

StatusWith<Date_t> dateFromISOString(StringData dateString) {
    ParsedTm parsed{};
    try {
        parsed = parseTm(dateString);
    } catch (const DBException& ex) {
        return Status{ErrorCodes::BadValue, ex.toStatus().reason()};
    }
    const auto& theTime = parsed.tm;

    unsigned long long resultMillis = 0;

#if defined(_WIN32)
    SYSTEMTIME dateStruct;
    dateStruct.wMilliseconds = durationCount<Milliseconds>(parsed.millis);
    dateStruct.wSecond = theTime.tm_sec;
    dateStruct.wMinute = theTime.tm_min;
    dateStruct.wHour = theTime.tm_hour;
    dateStruct.wDay = theTime.tm_mday;
    dateStruct.wDayOfWeek = -1; /* ignored */
    dateStruct.wMonth = theTime.tm_mon + 1;
    dateStruct.wYear = theTime.tm_year + 1900;

    // Output parameter for SystemTimeToFileTime
    FILETIME fileTime;

    // the wDayOfWeek member of SYSTEMTIME is ignored by this function
    if (SystemTimeToFileTime(&dateStruct, &fileTime) == 0) {
        StringBuilder sb;
        sb << "Error converting Windows system time to file time for date:  " << dateString
           << ".  Error code:  " << GetLastError();
        return StatusWith<Date_t>(ErrorCodes::BadValue, sb.str());
    }

    // The Windows FILETIME structure contains two parts of a 64-bit value representing the
    // number of 100-nanosecond intervals since January 1, 1601
    unsigned long long windowsTimeOffset =
        (static_cast<unsigned long long>(fileTime.dwHighDateTime) << 32) | fileTime.dwLowDateTime;

    // There are 11644473600 seconds between the unix epoch and the windows epoch
    // 100-nanoseconds = milliseconds * 10000
    unsigned long long epochDifference = 11644473600000 * 10000;

    // removes the diff between 1970 and 1601
    windowsTimeOffset -= epochDifference;

    // 1 milliseconds = 1000000 nanoseconds = 10000 100-nanosecond intervals
    resultMillis = windowsTimeOffset / 10000;
#else
    struct tm dateStruct = {0};
    dateStruct.tm_sec = theTime.tm_sec;
    dateStruct.tm_min = theTime.tm_min;
    dateStruct.tm_hour = theTime.tm_hour;
    dateStruct.tm_mday = theTime.tm_mday;
    dateStruct.tm_mon = theTime.tm_mon;
    dateStruct.tm_year = theTime.tm_year;
    dateStruct.tm_wday = 0;
    dateStruct.tm_yday = 0;

    time_t calendarTime = timegm(&dateStruct);
    if (calendarTime == -1) {
        uasserted(1125407, str::stream() << "timegm failed with errno: " << errno);
    }

    resultMillis = durationCount<Milliseconds>(Seconds(calendarTime) + parsed.millis);
#endif

    resultMillis -= durationCount<Milliseconds>(parsed.tzAdj);

    if (resultMillis > static_cast<unsigned long long>(std::numeric_limits<long long>::max())) {
        return {ErrorCodes::BadValue, str::stream() << dateString << " is too far in the future"};
    }
    return Date_t::fromMillisSinceEpoch(static_cast<long long>(resultMillis));
}

std::string Date_t::toString() const {
    if (isFormattable()) {
        return dateToISOStringLocal(*this);
    } else {
        return str::stream() << "Date(" << millis << ")";
    }
}

time_t Date_t::toTimeT() const {
    const auto secs = millis / 1000;
    MONGO_verify(secs >= std::numeric_limits<time_t>::min());
    MONGO_verify(secs <= std::numeric_limits<time_t>::max());
    return secs;
}

void sleepsecs(int s) {
    stdx::this_thread::sleep_for(Seconds(s).toSystemDuration());
}

void sleepmillis(long long s) {
    stdx::this_thread::sleep_for(Milliseconds(s).toSystemDuration());
}
void sleepmicros(long long s) {
    stdx::this_thread::sleep_for(Microseconds(s).toSystemDuration());
}

Milliseconds Backoff::nextSleep() {
    // Get the current time
    unsigned long long currTimeMillis = curTimeMillis64();

    int lastSleepMillis = _lastSleepMillis;

    if (!_lastErrorTimeMillis || _lastErrorTimeMillis > currTimeMillis /* VM bugs exist */)
        _lastErrorTimeMillis = currTimeMillis;

    unsigned long long lastErrorTimeMillis = _lastErrorTimeMillis;
    _lastErrorTimeMillis = currTimeMillis;

    lastSleepMillis = getNextSleepMillis(lastSleepMillis, currTimeMillis, lastErrorTimeMillis);

    // Store the last slept time
    _lastSleepMillis = lastSleepMillis;
    return Milliseconds(lastSleepMillis);
}

int Backoff::getNextSleepMillis(long long lastSleepMillis,
                                unsigned long long currTimeMillis,
                                unsigned long long lastErrorTimeMillis) const {
    // Backoff logic

    // Get the time since the last error
    const long long timeSinceLastErrorMillis = currTimeMillis - lastErrorTimeMillis;

    // If we haven't seen another error recently (3x the max wait time), reset our wait counter
    if (timeSinceLastErrorMillis > _resetAfterMillis)
        lastSleepMillis = 0;

    // Wait a power of two millis
    if (lastSleepMillis == 0)
        lastSleepMillis = 1;
    else
        lastSleepMillis = std::min(lastSleepMillis * 2, _maxSleepMillis);

    return lastSleepMillis;
}

#ifdef _WIN32  // no gettimeofday on windows
unsigned long long curTimeMillis64() {
    using stdx::chrono::system_clock;
    return static_cast<unsigned long long>(
        durationCount<Milliseconds>(system_clock::now() - system_clock::from_time_t(0)));
}

unsigned long long curTimeMicros64() {
    // Windows 8/2012 & later support a <1us time function
    FILETIME time;
    GetSystemTimePreciseAsFileTime(&time);
    return fileTimeToMicroseconds(time);
}

#else

namespace {

Microseconds curTimeDuration() {
    timeval tv;
    if (MONGO_unlikely(gettimeofday(&tv, nullptr) < 0)) {
        // only possible error is EFAULT, we're passing a pointer to stack memory
        auto e = lastSystemError();
        fasserted(1125408,
                  {ErrorCodes::InternalError, fmt::format("gettimeofday: {}", errorMessage(e))});
    }

    return Seconds(tv.tv_sec) + Microseconds(tv.tv_usec);
}

}  // namespace

unsigned long long curTimeMillis64() {
    return static_cast<unsigned long long>(durationCount<Milliseconds>(curTimeDuration()));
}

unsigned long long curTimeMicros64() {
    return static_cast<unsigned long long>(durationCount<Microseconds>(curTimeDuration()));
}
#endif

#if defined(__APPLE__)
template <typename T>
class MachPort {
public:
    MachPort(T port) : _port(std::move(port)) {}
    ~MachPort() {
        mach_port_deallocate(mach_task_self(), _port);
    }
    operator T&() {
        return _port;
    }

private:
    T _port;
};
#endif

// Find minimum timer resolution of OS
Nanoseconds getMinimumTimerResolution() {
    Nanoseconds minTimerResolution;
#if defined(__linux__) || defined(__FreeBSD__) || defined(__EMSCRIPTEN__)
    struct timespec tp;
    int ret = clock_getres(CLOCK_REALTIME, &tp);
    if (ret == -1) {
        uasserted(1125409, str::stream() << "clock_getres failed with errno: " << errno);
    }
    minTimerResolution = Nanoseconds{tp.tv_nsec};
#elif defined(_WIN32)
    // see https://msdn.microsoft.com/en-us/library/windows/desktop/dd743626(v=vs.85).aspx
    TIMECAPS tc;
    Milliseconds resMillis;
    invariant(timeGetDevCaps(&tc, sizeof(tc)) == MMSYSERR_NOERROR);
    resMillis = Milliseconds{static_cast<int64_t>(tc.wPeriodMin)};
    minTimerResolution = duration_cast<Nanoseconds>(resMillis);
#elif defined(__APPLE__)
    // see "Mac OSX Internals: a Systems Approach" for functions and types
    kern_return_t kr;
    MachPort<host_name_port_t> myhost(mach_host_self());
    MachPort<clock_serv_t> clk_system([&myhost] {
        host_name_port_t clk;
        invariant(host_get_clock_service(myhost, SYSTEM_CLOCK, &clk) == 0);
        return clk;
    }());
    natural_t attribute[4];
    mach_msg_type_number_t count;

    count = sizeof(attribute) / sizeof(natural_t);
    kr = clock_get_attributes(clk_system, CLOCK_GET_TIME_RES, (clock_attr_t)attribute, &count);
    invariant(kr == 0);

    minTimerResolution = Nanoseconds{attribute[0]};
#else
#error Dont know how to get the minimum timer resolution on this platform
#endif
    return minTimerResolution;
}

std::string dateToISOStringUTC(Date_t date) {
    return std::string{DateStringBuffer{}.iso8601(date, false)};
}
std::string dateToISOStringLocal(Date_t date) {
    return std::string{DateStringBuffer{}.iso8601(date, true)};
}
std::string dateToCtimeString(Date_t date) {
    return std::string{DateStringBuffer{}.ctime(date)};
}

void outputDateAsISOStringUTC(std::ostream& os, Date_t date) {
    os << StringData{DateStringBuffer{}.iso8601(date, false)};
}
void outputDateAsISOStringLocal(std::ostream& os, Date_t date) {
    os << StringData{DateStringBuffer{}.iso8601(date, true)};
}
void outputDateAsCtime(std::ostream& os, Date_t date) {
    os << StringData{DateStringBuffer{}.ctime(date)};
}

}  // namespace mongo
