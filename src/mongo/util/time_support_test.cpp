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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include <cstdlib>
#include <ctime>
#include <fmt/format.h>
#include <string>

#include "mongo/base/init.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {

using namespace fmt::literals;

const bool isTimeTSmall = std::numeric_limits<time_t>::digits == 31;

constexpr bool isLeap(int y) {
    return !(y % 400) || (y % 100 && !(y % 4));
}

constexpr long long daysBeforeYear(int y) {
    --y;
    return y * 365 + y / 4 - y / 100 + y / 400;
}

constexpr long long unixDaysBeforeYear(int y) {
    return daysBeforeYear(y) - daysBeforeYear(1970);
}

constexpr Date_t mkDate(long long ms) {
    return Date_t::fromMillisSinceEpoch(ms);
}

struct DateParts {
    int year, mon, dom, h, m, s, ms;
    int tzMinutes;
};

constexpr Date_t mkDate(DateParts dp) {
    auto [year, mon, dom, h, m, s, ms, tzMinutes] = dp;
    const int daysInMonths[12]{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    long long r = unixDaysBeforeYear(year);
    for (int i = 1; i < mon; ++i)
        r += daysInMonths[i - 1];
    if (mon > 2 && isLeap(year))
        ++r;
    r = r + dom - 1;
    r = r * 24 + h;
    r = r * 60 + m - tzMinutes;
    r = r * 60 + s;
    r = r * 1000 + ms;
    return mkDate(r);
}

/**
 * To make this test deterministic, we set the time zone to America/New_York.
 */
#ifdef _WIN32
char tzEnvString[] = "TZ=EST+5EDT";
#else
char tzEnvString[] = "TZ=America/New_York";
#endif

MONGO_INITIALIZER(SetTimeZoneToEasternForTest)(InitializerContext*) {
#ifdef _WIN32
    int ret = _putenv(tzEnvString);
#else
    int ret = putenv(tzEnvString);
#endif
    if (ret == -1) {
        auto ec = lastPosixError();
        uasserted(ErrorCodes::BadValue, errorMessage(ec));
    }
    tzset();
}

static std::string stringstreamDate(void (*formatter)(std::ostream&, Date_t), Date_t date) {
    std::ostringstream os;
    formatter(os, date);
    return os.str();
}

TEST(TimeFormattingTestHelpers, MkDate) {
    static_assert(!isLeap(1970));
    static_assert(isLeap(1972));
    static_assert(!isLeap(1900));
    static_assert(isLeap(2000));
    ASSERT_EQ(mkDate(0), mkDate({1970, 1, 1, 0, 0, 0, 0}));
    ASSERT_EQ(mkDate(15556000981), mkDate({1970, 6, 30, 1, 6, 40, 981}));
    ASSERT_EQ(mkDate(1361384951100), mkDate({2013, 2, 20, 18, 29, 11, 100}));
    ASSERT_EQ(mkDate(2781455351100), mkDate({2058, 2, 20, 18, 29, 11, 100}));
}

template <typename Run, typename GenExpected>
void runFormatTest(Run run, GenExpected gen) {
    struct DateFormatsRec {
        DateParts parts;
        std::string isoUtc;
        std::string isoLocal;
        std::string ctime;
    };

    std::vector<DateFormatsRec> v{
        {
            {1970, 1, 1, 0, 0, 0, 0},
            "1970-01-01T00:00:00.000Z",
            "1969-12-31T19:00:00.000-05:00",
            "Wed Dec 31 19:00:00.000",
        },
        {
            {1970, 6, 30, 1, 6, 40, 981},
            "1970-06-30T01:06:40.981Z",
            "1970-06-29T21:06:40.981-04:00",
            "Mon Jun 29 21:06:40.981",
        },
        {
            {2013, 2, 20, 18, 29, 11, 100},
            "2013-02-20T18:29:11.100Z",
            "2013-02-20T13:29:11.100-05:00",
            "Wed Feb 20 13:29:11.100",
        },
    };
    if (!isTimeTSmall) {
        v.push_back({
            {2058, 2, 20, 18, 29, 11, 100},
            "2058-02-20T18:29:11.100Z",
            "2058-02-20T13:29:11.100-05:00",
            "Wed Feb 20 13:29:11.100",
        });
    }

    for (auto&& rec : v) {
        ASSERT_EQ(run(mkDate(rec.parts)), gen(rec));
    }
}

TEST(TimeFormatting, DateAsISO8601UTCString) {
    runFormatTest([](Date_t d) { return dateToISOStringUTC(d); },
                  [](const auto& rec) { return rec.isoUtc; });
}

TEST(TimeFormatting, DateAsISO8601LocalString) {
    runFormatTest([](Date_t d) { return dateToISOStringLocal(d); },
                  [](const auto& rec) { return rec.isoLocal; });
}

TEST(TimeFormatting, DateAsCtimeString) {
    runFormatTest([](Date_t d) { return dateToCtimeString(d); },
                  [](const auto& rec) { return rec.ctime; });
}

TEST(TimeFormatting, DateAsISO8601UTCStream) {
    runFormatTest([](Date_t d) { return stringstreamDate(outputDateAsISOStringUTC, d); },
                  [](const auto& rec) { return rec.isoUtc; });
}

TEST(TimeFormatting, DateAsISO8601LocalStream) {
    runFormatTest([](Date_t d) { return stringstreamDate(outputDateAsISOStringLocal, d); },
                  [](const auto& rec) { return rec.isoLocal; });
}

TEST(TimeFormatting, DateAsCtimeStream) {
    runFormatTest([](Date_t d) { return stringstreamDate(outputDateAsCtime, d); },
                  [](const auto& rec) { return rec.ctime; });
}

struct ParseRec {
    std::string in;
    DateParts out;
};

TEST(TimeParsing, DateAsISO8601UTC) {
    // Allowed date format:
    // YYYY-MM-DDTHH:MM[:SS[.m[m[m]]]]Z
    // Year, month, day, hour, and minute are required, while the seconds component and one to
    // three milliseconds are optional.
    std::vector<ParseRec> v{
        {"1971-02-03T04:05:06.789Z", {1971, 2, 3, 4, 5, 6, 789}},
        {"1971-02-03T04:05:06.78Z", {1971, 2, 3, 4, 5, 6, 780}},
        {"1971-02-03T04:05:06.7Z", {1971, 2, 3, 4, 5, 6, 700}},
        {"1971-02-03T04:05:06Z", {1971, 2, 3, 4, 5, 6, 0}},
        {"1971-02-03T04:05Z", {1971, 2, 3, 4, 5, 0, 0}},
        {"1970-01-01T00:00:00.000Z", {1970, 1, 1, 0, 0, 0, 0}},
        {"1970-06-30T01:06:40.981Z", {1970, 6, 30, 1, 6, 40, 981}},
        {"2013-02-20T18:29:11.100Z", {2013, 2, 20, 18, 29, 11, 100}},
    };
    if (!isTimeTSmall) {
        v.insert(v.end(),
                 {
                     {"2058-02-20T18:29:11.100Z", {2058, 2, 20, 18, 29, 11, 100}},
                     {"3001-01-01T08:00:00.000Z", {3001, 1, 1, 8, 0, 0, 0}},
                 });
    }

    for (const auto& [in, out] : v) {
        auto d = dateFromISOString(in);
        ASSERT_OK(d.getStatus()) << in;
        ASSERT_EQUALS(d.getValue(), mkDate(out)) << in;
    }
}

TEST(TimeParsing, DateAsISO8601Local) {
    // Allowed date format:
    // YYYY-MM-DDTHH:MM[:SS[.m[m[m]]]]+HH[:]MM
    // Year, month, day, hour, and minute are required, while the seconds component and one to
    // three milliseconds are optional.  The time zone offset must be four digits.
    // Test with colon in timezone offset, new for mongod version 4.4
    std::vector<ParseRec> v{
        {"1971-02-03T09:16:06.789+05:11", {1971, 2, 3, 9, 16, 6, 789, 5 * 60 + 11}},
        {"1971-02-03T09:16:06.78+05:11", {1971, 2, 3, 9, 16, 6, 780, 5 * 60 + 11}},
        {"1971-02-03T09:16:06.7+05:11", {1971, 2, 3, 9, 16, 6, 700, 5 * 60 + 11}},
        {"1971-02-03T09:16:06+05:11", {1971, 2, 3, 9, 16, 6, 0, 5 * 60 + 11}},
        {"1971-02-03T09:16+05:11", {1971, 2, 3, 9, 16, 0, 0, 5 * 60 + 11}},
        {"1970-01-01T00:00:00.000Z", {1970, 1, 1, 0, 0, 0, 0}},
        {"1970-06-30T01:06:40.981Z", {1970, 6, 30, 1, 6, 40, 981}},
        {"1970-06-29T21:06:40.981-04:00", {1970, 6, 29, 21, 6, 40, 981, -4 * 60}},
        {"2038-01-19T03:14:07Z", {2038, 1, 19, 3, 14, 7, 0}},
        {"2013-02-20T13:29:11.100-05:00", {2013, 2, 20, 13, 29, 11, 100, -5 * 60}},
        {"2013-02-20T13:29:11.100-05:01", {2013, 2, 20, 13, 29, 11, 100, -(5 * 60 + 1)}},
    };
    if (!isTimeTSmall) {
        v.insert(v.end(),
                 {
                     {"2058-02-20T13:29:11.100-05:00", {2058, 2, 20, 13, 29, 11, 100, -5 * 60}},
                     {"3000-12-31T23:59:59Z", {3000, 12, 31, 23, 59, 59}},
                 });
    }
#if 0
    // Local times not supported
    v.insert(v.end(),
             {
                 {"1970-01-01T00:00:00.001", {1970, 1, 1, 0, 0, 0, 1}, 0},
                 {"1970-01-01T00:00:00.01", {1970, 1, 1, 0, 0, 0, 10}, 0},
                 {"1970-01-01T00:00:00.1", {1970, 1, 1, 0, 0, 0, 100}, 0},
                 {"1970-01-01T00:00:01", {1970, 1, 1, 0, 0, 1, 0}, 0},
                 {"1970-01-01T00:01", {1970, 1, 1, 0, 1, 0, 0}, 0},
             });
#endif
    for (const auto& [in, outBase] : v) {
        StatusWith<Date_t> d = dateFromISOString(in);
        ASSERT_OK(d.getStatus()) << in;
        ASSERT_EQUALS(d.getValue(), mkDate(outBase)) << in;
    };
}

TEST(TimeParsing, DateAsISO8601LocalNoColon) {
    // Allowed date format:
    // YYYY-MM-DDTHH:MM[:SS[.m[m[m]]]]+HH[:]MM
    // Year, month, day, hour, and minute are required, while the seconds component and one to
    // three milliseconds are optional.  The time zone offset must be four digits.
    // Test with colon in timezone offset, the format used by mongod version < 4.4
    std::vector<ParseRec> v{
        {"1971-02-03T09:16:06.789+0511", {1971, 2, 3, 9, 16, 6, 789, 5 * 60 + 11}},
        {"1971-02-03T09:16:06.78+0511", {1971, 2, 3, 9, 16, 6, 780, 5 * 60 + 11}},
        {"1971-02-03T09:16:06.7+0511", {1971, 2, 3, 9, 16, 6, 700, 5 * 60 + 11}},
        {"1971-02-03T09:16:06+0511", {1971, 2, 3, 9, 16, 6, 0, 5 * 60 + 11}},
        {"1971-02-03T09:16+0511", {1971, 2, 3, 9, 16, 0, 0, 5 * 60 + 11}},
        {"1970-06-29T21:06:40.981-0400", {1970, 6, 29, 21, 6, 40, 981, -4 * 60}},
        {"2013-02-20T13:29:11.100-0500", {2013, 2, 20, 13, 29, 11, 100, -5 * 60}},
        {"2013-02-20T13:29:11.100-0501", {2013, 2, 20, 13, 29, 11, 100, -(5 * 60 + 1)}},
    };
    if (!isTimeTSmall) {
        v.insert(v.end(),
                 {
                     {"2058-02-20T13:29:11.100-0500", {2058, 2, 20, 13, 29, 11, 100, -5 * 60}},
                 });
    }
}

TEST(TimeParsing, InvalidDates) {
    static constexpr const char* badDates[]{
        // Invalid decimal
        "1970-01-01T00:00:00.0.0Z",
        "1970-01-01T00:00:.0.000Z",
        "1970-01-01T00:.0:00.000Z",
        "1970-01-01T.0:00:00.000Z",
        "1970-01-.1T00:00:00.000Z",
        "1970-.1-01T00:00:00.000Z",
        ".970-01-01T00:00:00.000Z",

        // Extra sign characters
        "1970-01-01T00:00:00.+00Z",
        "1970-01-01T00:00:+0.000Z",
        "1970-01-01T00:+0:00.000Z",
        "1970-01-01T+0:00:00.000Z",
        "1970-01-+1T00:00:00.000Z",
        "1970-+1-01T00:00:00.000Z",
        "+970-01-01T00:00:00.000Z",

        "1970-01-01T00:00:00.-00Z",
        "1970-01-01T00:00:-0.000Z",
        "1970-01-01T00:-0:00.000Z",
        "1970-01-01T-0:00:00.000Z",
        "1970-01--1T00:00:00.000Z",
        "1970--1-01T00:00:00.000Z",
        "-970-01-01T00:00:00.000Z",

        // Out of range
        "1970-01-01T00:00:60.000Z",
        "1970-01-01T00:60:00.000Z",
        "1970-01-01T24:00:00.000Z",
        "1970-01-32T00:00:00.000Z",
        "1970-01-00T00:00:00.000Z",
        "1970-13-01T00:00:00.000Z",
        "1970-00-01T00:00:00.000Z",
        "1969-01-01T00:00:00.000Z",

        // Invalid lengths
        "01970-01-01T00:00:00.000Z",
        "1970-001-01T00:00:00.000Z",
        "1970-01-001T00:00:00.000Z",
        "1970-01-01T000:00:00.000Z",
        "1970-01-01T00:000:00.000Z",
        "1970-01-01T00:00:000.000Z",
        "1970-01-01T00:00:00.0000Z",
        "197-01-01T00:00:00.000Z",
        "1970-1-01T00:00:00.000Z",
        "1970-01-1T00:00:00.000Z",
        "1970-01-01T0:00:00.000Z",
        "1970-01-01T00:0:00.000Z",
        "1970-01-01T00:00:0.000Z",

        // Invalid delimiters
        "1970+01-01T00:00:00.000Z",
        "1970-01+01T00:00:00.000Z",
        "1970-01-01Q00:00:00.000Z",
        "1970-01-01T00-00:00.000Z",
        "1970-01-01T00:00-00.000Z",
        "1970-01-01T00:00:00-000Z",

        // Missing numbers
        "1970--01T00:00:00.000Z",
        "1970-01-T00:00:00.000Z",
        "1970-01-01T:00:00.000Z",
        "1970-01-01T00::00.000Z",
        "1970-01-01T00:00:.000Z",
        "1970-01-01T00:00:00.Z",

        // Bad time offset field
        "1970-01-01T05:00:01ZZ",
        "1970-01-01T05:00:01+",
        "1970-01-01T05:00:01-",
        "1970-01-01T05:00:01-11111",
        "1970-01-01T05:00:01Z1111",
        "1970-01-01T05:00:01+111",
        "1970-01-01T05:00:01+1160",
        "1970-01-01T05:00:01+2400",
        "1970-01-01T05:00:01+00+0",

        // Bad prefixes
        "1970-01-01T05:00:01.",
        "1970-01-01T05:00:",
        "1970-01-01T05:",
        "1970-01-01T",
        "1970-01-",
        "1970-",
        "1970-01-01T05+0500",
        "1970-01-01+0500",
        "1970-01+0500",
        "1970+0500",
        "1970-01-01T01Z",
        "1970-01-01Z",
        "1970-01Z",
        "1970Z",

        // No local time
        "1970-01-01T00:00:00.000",
        "1970-01-01T00:00:00",
        "1970-01-01T00:00",
        "1970-01-01T00",
        "1970-01-01",
        "1970-01",
        "1970",

        // Invalid hex base specifiers
        "x970-01-01T00:00:00.000Z",
        "1970-x1-01T00:00:00.000Z",
        "1970-01-x1T00:00:00.000Z",
        "1970-01-01Tx0:00:00.000Z",
        "1970-01-01T00:x0:00.000Z",
        "1970-01-01T00:00:x0.000Z",
        "1970-01-01T00:00:00.x00Z",
    };

    for (const char* s : badDates) {
        ASSERT_NOT_OK(dateFromISOString(s)) << s;
    }
}

TEST(TimeParsing, LeapYears) {
    int maxYear = isTimeTSmall ? 2036 : 9999;
    for (int y = 1972; y <= maxYear; y += 4) {
        if (!isLeap(y))
            continue;
        std::string in = "{:04}-02-29T00:00:00.000Z"_format(y);
        StatusWith<Date_t> d = dateFromISOString(in);
        ASSERT_OK(d.getStatus()) << y;
        ASSERT_EQUALS(d.getValue(), mkDate({y, 2, 29})) << y;
    }
}

TEST(TimeFormatting, DurationFormatting) {
    ASSERT_EQUALS("52\xce\xbcs", static_cast<std::string>(str::stream() << Microseconds(52)));
    ASSERT_EQUALS("52ms", static_cast<std::string>(str::stream() << Milliseconds(52)));
    ASSERT_EQUALS("52s", static_cast<std::string>(str::stream() << Seconds(52)));

    std::ostringstream os;
    os << Milliseconds(52) << Microseconds(52) << Seconds(52);
    ASSERT_EQUALS("52ms52\xce\xbcs52s", os.str());
}

TEST(TimeFormatting, WriteToStream) {
    const std::vector<std::string> dateStrings = {
        "1996-04-07T00:00:00.000Z",
        "1996-05-02T00:00:00.000Z",
        "1997-06-23T07:55:00.000Z",
        "2015-05-14T17:28:33.123Z",
        "2036-02-29T00:00:00.000Z",
    };

    for (const std::string& isoTimeString : dateStrings) {
        const Date_t aDate = unittest::assertGet(dateFromISOString(isoTimeString));
        std::ostringstream testStream;
        testStream << aDate;
        std::string streamOut = testStream.str();
        ASSERT_EQUALS(aDate.toString(), streamOut);
    }
}

TEST(SystemTime, ConvertDateToSystemTime) {
    const std::string isoTimeString = "2015-05-14T17:28:33.123Z";
    const Date_t aDate = unittest::assertGet(dateFromISOString(isoTimeString));
    const auto aTimePoint = aDate.toSystemTimePoint();
    const auto actual = aTimePoint - stdx::chrono::system_clock::from_time_t(0);
    ASSERT(aDate.toDurationSinceEpoch().toSystemDuration() == actual)
        << "Expected " << aDate << "; but found " << Date_t::fromDurationSinceEpoch(actual);
    ASSERT_EQUALS(aDate, Date_t(aTimePoint));
}

TEST(DateTArithmetic, AdditionNoOverflowSucceeds) {
    auto dateFromMillis = [](long long ms) {
        return Date_t::fromDurationSinceEpoch(Milliseconds{ms});
    };

    // Test operator+
    ASSERT_EQ(dateFromMillis(1001), dateFromMillis(1000) + Milliseconds{1});
    // Test operator+=
    auto dateToIncrement = dateFromMillis(1000);
    dateToIncrement += Milliseconds(1);
    ASSERT_EQ(dateFromMillis(1001), dateToIncrement);
}

TEST(DateTArithmetic, AdditionOverflowThrows) {
    // Test operator+
    ASSERT_THROWS_CODE(Date_t::max() + Milliseconds(1), DBException, ErrorCodes::DurationOverflow);
    // Test operator+=
    auto dateToIncrement = Date_t::max();
    ASSERT_THROWS_CODE(
        dateToIncrement += Milliseconds(1), DBException, ErrorCodes::DurationOverflow);
    ASSERT_THROWS_CODE(Date_t::fromDurationSinceEpoch(Milliseconds::min()) + Milliseconds(-1),
                       DBException,
                       ErrorCodes::DurationOverflow);
}

TEST(DateTArithmetic, SubtractionOverflowThrows) {
    ASSERT_THROWS_CODE(Date_t::fromDurationSinceEpoch(Milliseconds::min()) - Milliseconds(1),
                       DBException,
                       ErrorCodes::DurationOverflow);
    ASSERT_THROWS_CODE(Date_t::max() - Milliseconds(-1), DBException, ErrorCodes::DurationOverflow);
}

TEST(Backoff, NextSleep) {
    Backoff backoff(Milliseconds(8), Milliseconds::max());
    ASSERT_EQ(Milliseconds(1), backoff.nextSleep());
    ASSERT_EQ(Milliseconds(2), backoff.nextSleep());
    ASSERT_EQ(Milliseconds(4), backoff.nextSleep());
    ASSERT_EQ(Milliseconds(8), backoff.nextSleep());
    ASSERT_EQ(Milliseconds(8), backoff.nextSleep());
}

TEST(Backoff, SleepBackoffTest) {
    const int maxSleepTimeMillis = 1000;
    Backoff backoff(Milliseconds(maxSleepTimeMillis), Milliseconds(maxSleepTimeMillis * 2));

    // Double previous sleep duration
    ASSERT_EQUALS(backoff.getNextSleepMillis(0, 0, 0), 1);
    ASSERT_EQUALS(backoff.getNextSleepMillis(2, 0, 0), 4);
    ASSERT_EQUALS(backoff.getNextSleepMillis(256, 0, 0), 512);

    // Make sure our backoff increases to the maximum value
    ASSERT_EQUALS(backoff.getNextSleepMillis(maxSleepTimeMillis - 200, 0, 0), maxSleepTimeMillis);
    ASSERT_EQUALS(backoff.getNextSleepMillis(maxSleepTimeMillis * 2, 0, 0), maxSleepTimeMillis);

    // Make sure that our backoff gets reset if we wait much longer than the maximum wait
    const unsigned long long resetAfterMillis = maxSleepTimeMillis * 2;
    ASSERT_EQUALS(backoff.getNextSleepMillis(20, resetAfterMillis, 0), 40);     // no reset here
    ASSERT_EQUALS(backoff.getNextSleepMillis(20, resetAfterMillis + 1, 0), 1);  // reset expected
}

TEST(BasicNow, NowUpdatesLastNow) {
    const auto then = Date_t::now();
    ASSERT_EQ(then, Date_t::lastNowForTest());
    sleepFor(Milliseconds(100));
    ASSERT_EQ(then, Date_t::lastNowForTest());
    const auto now = Date_t::now();
    ASSERT_EQ(now, Date_t::lastNowForTest());
    ASSERT_GT(now, then);
}

}  // namespace
}  // namespace mongo
