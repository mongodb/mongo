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

#include <array>

#include <boost/move/utility_core.hpp>
#include <fmt/format.h>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/unittest/unittest.h"

#include <initializer_list>
#include <limits>
#include <string>

#include <timelib.h>

namespace mongo {
namespace {

const TimeZoneDatabase kDefaultTimeZoneDatabase{};
const TimeZone kDefaultTimeZone = TimeZoneDatabase::utcZone();

TEST(GetTimeZone, DoesReturnKnownTimeZone) {
    // Just asserting that these do not throw exceptions.
    kDefaultTimeZoneDatabase.getTimeZone("UTC");
    kDefaultTimeZoneDatabase.getTimeZone("America/New_York");
    kDefaultTimeZoneDatabase.getTimeZone("Australia/Sydney");
}

TEST(GetTimeZone, DoesParseHourOnlyOffset) {
    auto date = Date_t::fromMillisSinceEpoch(1500371861000LL);

    auto zone = kDefaultTimeZoneDatabase.getTimeZone("+02");
    ASSERT_EQ(durationCount<Hours>(zone.utcOffset(date)), 2);

    zone = kDefaultTimeZoneDatabase.getTimeZone("-02");
    ASSERT_EQ(durationCount<Hours>(zone.utcOffset(date)), -2);

    zone = kDefaultTimeZoneDatabase.getTimeZone("+00");
    ASSERT_EQ(durationCount<Seconds>(zone.utcOffset(date)), 0);

    zone = kDefaultTimeZoneDatabase.getTimeZone("-00");
    ASSERT_EQ(durationCount<Seconds>(zone.utcOffset(date)), 0);
}

TEST(GetTimeZone, DoesParseHourMinuteOffset) {
    auto date = Date_t::fromMillisSinceEpoch(1500371861000LL);

    auto zone = kDefaultTimeZoneDatabase.getTimeZone("+0200");
    ASSERT_EQ(durationCount<Hours>(zone.utcOffset(date)), 2);

    zone = kDefaultTimeZoneDatabase.getTimeZone("-0200");
    ASSERT_EQ(durationCount<Hours>(zone.utcOffset(date)), -2);

    zone = kDefaultTimeZoneDatabase.getTimeZone("+0245");
    ASSERT_EQ(durationCount<Minutes>(zone.utcOffset(date)), 165);

    zone = kDefaultTimeZoneDatabase.getTimeZone("-0245");
    ASSERT_EQ(durationCount<Minutes>(zone.utcOffset(date)), -165);
}

TEST(GetTimeZone, DoesParseHourMinuteOffsetWithColon) {
    auto date = Date_t::fromMillisSinceEpoch(1500371861000LL);

    auto zone = kDefaultTimeZoneDatabase.getTimeZone("+12:00");
    ASSERT_EQ(durationCount<Hours>(zone.utcOffset(date)), 12);

    zone = kDefaultTimeZoneDatabase.getTimeZone("-11:00");
    ASSERT_EQ(durationCount<Hours>(zone.utcOffset(date)), -11);

    zone = kDefaultTimeZoneDatabase.getTimeZone("+09:27");
    ASSERT_EQ(durationCount<Minutes>(zone.utcOffset(date)), 567);

    zone = kDefaultTimeZoneDatabase.getTimeZone("-00:37");
    ASSERT_EQ(durationCount<Minutes>(zone.utcOffset(date)), -37);
}

TEST(GetTimeZone, DoesNotReturnUnknownTimeZone) {
    ASSERT_THROWS_CODE(kDefaultTimeZoneDatabase.getTimeZone("The moon"), AssertionException, 40485);
    ASSERT_THROWS_CODE(kDefaultTimeZoneDatabase.getTimeZone("xyz"), AssertionException, 40485);
    ASSERT_THROWS_CODE(kDefaultTimeZoneDatabase.getTimeZone("Jupiter"), AssertionException, 40485);
}

TEST(GetTimeZone, ThrowsUserExceptionIfGivenUnparsableUtcOffset) {
    ASSERT_THROWS_CODE(kDefaultTimeZoneDatabase.getTimeZone("123"), AssertionException, 40485);
    ASSERT_THROWS_CODE(kDefaultTimeZoneDatabase.getTimeZone("1234"), AssertionException, 40485);
    ASSERT_THROWS_CODE(kDefaultTimeZoneDatabase.getTimeZone("12345"), AssertionException, 40485);
    ASSERT_THROWS_CODE(kDefaultTimeZoneDatabase.getTimeZone("-123"), AssertionException, 40485);
    ASSERT_THROWS_CODE(kDefaultTimeZoneDatabase.getTimeZone("-12*34"), AssertionException, 40485);
    ASSERT_THROWS_CODE(kDefaultTimeZoneDatabase.getTimeZone("-1:23"), AssertionException, 40485);
    ASSERT_THROWS_CODE(kDefaultTimeZoneDatabase.getTimeZone("-12:3"), AssertionException, 40485);
    ASSERT_THROWS_CODE(kDefaultTimeZoneDatabase.getTimeZone("+123"), AssertionException, 40485);
    ASSERT_THROWS_CODE(kDefaultTimeZoneDatabase.getTimeZone("+12*34"), AssertionException, 40485);
    ASSERT_THROWS_CODE(kDefaultTimeZoneDatabase.getTimeZone("+1:23"), AssertionException, 40485);
    ASSERT_THROWS_CODE(kDefaultTimeZoneDatabase.getTimeZone("+12:3"), AssertionException, 40485);
    ASSERT_THROWS_CODE(kDefaultTimeZoneDatabase.getTimeZone("+0x4"), AssertionException, 40485);
    ASSERT_THROWS_CODE(kDefaultTimeZoneDatabase.getTimeZone("-0xa0"), AssertionException, 40485);
}

TEST(UTCTimeBeforeEpoch, DoesExtractDateParts) {
    // Dec 30, 1969 13:42:23:211
    auto date = Date_t::fromMillisSinceEpoch(-123456789LL);
    auto dateParts = TimeZoneDatabase::utcZone().dateParts(date);
    ASSERT_EQ(dateParts.year, 1969);
    ASSERT_EQ(dateParts.month, 12);
    ASSERT_EQ(dateParts.dayOfMonth, 30);
    ASSERT_EQ(dateParts.hour, 13);
    ASSERT_EQ(dateParts.minute, 42);
    ASSERT_EQ(dateParts.second, 23);
    ASSERT_EQ(dateParts.millisecond, 211);
}

TEST(NewYorkTimeBeforeEpoch, DoesExtractDateParts) {
    auto newYorkZone = kDefaultTimeZoneDatabase.getTimeZone("America/New_York");

    // 1969-12-30T13:42:23.211Z.
    auto date = Date_t::fromMillisSinceEpoch(-123456789LL);
    auto dateParts = newYorkZone.dateParts(date);
    ASSERT_EQ(dateParts.year, 1969);
    ASSERT_EQ(dateParts.month, 12);
    ASSERT_EQ(dateParts.dayOfMonth, 30);
    ASSERT_EQ(dateParts.hour, 8);
    ASSERT_EQ(dateParts.minute, 42);
    ASSERT_EQ(dateParts.second, 23);
    ASSERT_EQ(dateParts.millisecond, 211);
}

TEST(UtcOffsetBeforeEpoch, DoesExtractDateParts) {
    auto utcOffsetZone = kDefaultTimeZoneDatabase.getTimeZone("-05:00");

    // 1969-12-30T13:42:23.211Z.
    auto date = Date_t::fromMillisSinceEpoch(-123456789LL);
    auto dateParts = utcOffsetZone.dateParts(date);
    ASSERT_EQ(dateParts.year, 1969);
    ASSERT_EQ(dateParts.month, 12);
    ASSERT_EQ(dateParts.dayOfMonth, 30);
    ASSERT_EQ(dateParts.hour, 8);
    ASSERT_EQ(dateParts.minute, 42);
    ASSERT_EQ(dateParts.second, 23);
    ASSERT_EQ(dateParts.millisecond, 211);
}

TEST(UTCTimeBeforeEpoch, DoesComputeISOYear) {
    // Sunday, December 28, 1969.
    auto date = Date_t::fromMillisSinceEpoch(-345600000LL);
    ASSERT_EQ(TimeZoneDatabase::utcZone().isoYear(date), 1969);
    // Tue, December 30, 1969, part of the following year.
    date = Date_t::fromMillisSinceEpoch(-123456000LL);
    ASSERT_EQ(TimeZoneDatabase::utcZone().isoYear(date), 1970);
    // Saturday, January 1, 1966, part of the previous year.
    date = Date_t::fromMillisSinceEpoch(-126230400000LL);
    ASSERT_EQ(TimeZoneDatabase::utcZone().isoYear(date), 1965);
}

TEST(NewYorkTimeBeforeEpoch, DoesComputeISOYear) {
    auto newYorkZone = kDefaultTimeZoneDatabase.getTimeZone("America/New_York");

    // Sunday, December 28, 1969.
    auto date = Date_t::fromMillisSinceEpoch(-345600000LL);
    ASSERT_EQ(newYorkZone.isoYear(date), 1969);

    // 1969-12-30T13:42:23.211Z (Tuesday), part of the following year.
    date = Date_t::fromMillisSinceEpoch(-123456000LL);
    ASSERT_EQ(newYorkZone.isoYear(date), 1970);

    // 1966-01-01T00:00:00.000Z (Saturday), part of the previous year.
    date = Date_t::fromMillisSinceEpoch(-126230400000LL);
    ASSERT_EQ(newYorkZone.isoYear(date), 1965);
}

TEST(UTCTimeBeforeEpoch, DoesComputeDayOfWeek) {
    // Sunday, December 28, 1969.
    auto date = Date_t::fromMillisSinceEpoch(-345600000LL);
    ASSERT_EQ(TimeZoneDatabase::utcZone().dayOfWeek(date), 1);
    // Tuesday, December 30, 1969.
    date = Date_t::fromMillisSinceEpoch(-123456000LL);
    ASSERT_EQ(TimeZoneDatabase::utcZone().dayOfWeek(date), 3);
    // Saturday, January 1, 1966.
    date = Date_t::fromMillisSinceEpoch(-126230400000LL);
    ASSERT_EQ(TimeZoneDatabase::utcZone().dayOfWeek(date), 7);
}

TEST(NewYorkTimeBeforeEpoch, DoesComputeDayOfWeek) {
    auto newYorkZone = kDefaultTimeZoneDatabase.getTimeZone("America/New_York");

    // 1969-12-28T00:00:00.000Z (Sunday).
    auto date = Date_t::fromMillisSinceEpoch(-345600000LL);
    // Part of the previous day (Saturday) in New York.
    ASSERT_EQ(newYorkZone.dayOfWeek(date), 7);

    // 1969-12-30T13:42:23.211Z (Tuesday).
    date = Date_t::fromMillisSinceEpoch(-123456000LL);
    ASSERT_EQ(newYorkZone.dayOfWeek(date), 3);
}

TEST(UTCTimeBeforeEpoch, DoesComputeISODayOfWeek) {
    // Sunday, December 28, 1969.
    auto date = Date_t::fromMillisSinceEpoch(-345600000LL);
    ASSERT_EQ(TimeZoneDatabase::utcZone().isoDayOfWeek(date), 7);
    // Tue, December 30, 1969.
    date = Date_t::fromMillisSinceEpoch(-123456000LL);
    ASSERT_EQ(TimeZoneDatabase::utcZone().isoDayOfWeek(date), 2);
    // Saturday, January 1, 1966.
    date = Date_t::fromMillisSinceEpoch(-126230400000LL);
    ASSERT_EQ(TimeZoneDatabase::utcZone().isoDayOfWeek(date), 6);
}

TEST(NewYorkTimeBeforeEpoch, DoesComputeISODayOfWeek) {
    auto newYorkZone = kDefaultTimeZoneDatabase.getTimeZone("America/New_York");

    // 1969-12-28T00:00:00.000Z (Sunday).
    auto date = Date_t::fromMillisSinceEpoch(-345600000LL);
    // Part of the previous day (Saturday) in New York.
    ASSERT_EQ(newYorkZone.isoDayOfWeek(date), 6);

    // 1969-12-30T13:42:23.211Z (Tuesday).
    date = Date_t::fromMillisSinceEpoch(-123456000LL);
    ASSERT_EQ(newYorkZone.isoDayOfWeek(date), 2);
}

TEST(UTCTimeBeforeEpoch, DoesComputeDayOfYear) {
    // December 30, 1969.
    auto date = Date_t::fromMillisSinceEpoch(-123456000LL);
    ASSERT_EQ(TimeZoneDatabase::utcZone().dayOfYear(date), 364);
    // January 1, 1966.
    date = Date_t::fromMillisSinceEpoch(-126230400000LL);
    ASSERT_EQ(TimeZoneDatabase::utcZone().dayOfYear(date), 1);

    // Feb 28, 1960 (leap year).
    date = Date_t::fromMillisSinceEpoch(-310608000000LL);
    ASSERT_EQ(TimeZoneDatabase::utcZone().dayOfYear(date), 59);
    // Feb 29, 1960 (leap year).
    date = Date_t::fromMillisSinceEpoch(-310521600000LL);
    ASSERT_EQ(TimeZoneDatabase::utcZone().dayOfYear(date), 60);
    // Mar 1, 1960 (leap year).
    date = Date_t::fromMillisSinceEpoch(-310435200000LL);
    ASSERT_EQ(TimeZoneDatabase::utcZone().dayOfYear(date), 61);
    // December 31, 1960 (leap year).
    date = Date_t::fromMillisSinceEpoch(-284083200000LL);
    ASSERT_EQ(TimeZoneDatabase::utcZone().dayOfYear(date), 366);

    // Feb 28, 1900 (not leap year).
    date = Date_t::fromMillisSinceEpoch(-2203977600000LL);
    ASSERT_EQ(TimeZoneDatabase::utcZone().dayOfYear(date), 59);
    // Mar 1, 1900 (not leap year).
    date = Date_t::fromMillisSinceEpoch(-2203891200000LL);
    ASSERT_EQ(TimeZoneDatabase::utcZone().dayOfYear(date), 60);
    // December 31, 1900 (not leap year).
    date = Date_t::fromMillisSinceEpoch(-2177539200000LL);
    ASSERT_EQ(TimeZoneDatabase::utcZone().dayOfYear(date), 365);
}

TEST(NewYorkTimeBeforeEpoch, DoesComputeDayOfYear) {
    auto newYorkZone = kDefaultTimeZoneDatabase.getTimeZone("America/New_York");

    // 1969-12-28T13:42:24.000Z (Sunday).
    auto date = Date_t::fromMillisSinceEpoch(-123456000LL);
    ASSERT_EQ(newYorkZone.dayOfYear(date), 364);

    // 1966-01-01T00:00:00.000Z (Saturday).
    date = Date_t::fromMillisSinceEpoch(-126230400000LL);
    ASSERT_EQ(newYorkZone.dayOfYear(date), 365);

    // 1960-02-28T00:00:00.000Z (leap year).
    date = Date_t::fromMillisSinceEpoch(-310608000000LL);
    ASSERT_EQ(newYorkZone.dayOfYear(date), 58);
    // 1960-02-29T00:00:00.000Z.
    date = Date_t::fromMillisSinceEpoch(-310521600000LL);
    ASSERT_EQ(newYorkZone.dayOfYear(date), 59);
    // 1960-01-01T00:00:00.000Z.
    date = Date_t::fromMillisSinceEpoch(-310435200000LL);
    ASSERT_EQ(newYorkZone.dayOfYear(date), 60);
    // 1960-12-31T00:00:00.000Z.
    date = Date_t::fromMillisSinceEpoch(-284083200000LL);
    ASSERT_EQ(newYorkZone.dayOfYear(date), 365);

    // 1900-02-28T00:00:00.000Z (not leap year).
    date = Date_t::fromMillisSinceEpoch(-2203977600000LL);
    ASSERT_EQ(newYorkZone.dayOfYear(date), 58);
    // 1900-03-01T00:00:00.000Z.
    date = Date_t::fromMillisSinceEpoch(-2203891200000LL);
    ASSERT_EQ(newYorkZone.dayOfYear(date), 59);
    // 1900-12-31T00:00:00.000Z.
    date = Date_t::fromMillisSinceEpoch(-2177539200000LL);
    ASSERT_EQ(newYorkZone.dayOfYear(date), 364);
}

TEST(UTCTimeBeforeEpoch, DoesComputeWeek) {
    // Sunday, December 28, 1969.
    auto date = Date_t::fromMillisSinceEpoch(-345600000LL);
    ASSERT_EQ(TimeZoneDatabase::utcZone().week(date), 52);
    // Saturday, January 1, 1966.
    date = Date_t::fromMillisSinceEpoch(-126230400000LL);
    ASSERT_EQ(TimeZoneDatabase::utcZone().week(date), 0);
}

TEST(NewYorkTimeBeforeEpoch, DoesComputeWeek) {
    auto newYorkZone = kDefaultTimeZoneDatabase.getTimeZone("America/New_York");

    // 1969-12-28T00:00:00.000Z (Sunday).
    auto date = Date_t::fromMillisSinceEpoch(-345600000LL);
    ASSERT_EQ(newYorkZone.week(date), 51);

    // 1966-01-01T00:00:00.000Z (Saturday).
    date = Date_t::fromMillisSinceEpoch(-126230400000LL);
    ASSERT_EQ(newYorkZone.week(date), 52);
}

TEST(UTCTimeBeforeEpoch, DoesComputeUtcOffset) {
    // Sunday, December 28, 1969.
    auto date = Date_t::fromMillisSinceEpoch(-345600000LL);
    ASSERT_EQ(durationCount<Seconds>(TimeZoneDatabase::utcZone().utcOffset(date)), 0);
}

TEST(NewYorkTimeBeforeEpoch, DoesComputeUtcOffset) {
    auto newYorkZone = kDefaultTimeZoneDatabase.getTimeZone("America/New_York");

    // 1969-06-29T00:00:00.000Z (Sunday).
    auto date = Date_t::fromMillisSinceEpoch(-16070400000LL);
    ASSERT_EQ(durationCount<Hours>(newYorkZone.utcOffset(date)), -4);

    // 1966-01-01T00:00:00.000Z (Saturday).
    date = Date_t::fromMillisSinceEpoch(-126230400000LL);
    ASSERT_EQ(durationCount<Hours>(newYorkZone.utcOffset(date)), -5);
}

TEST(UTCTimeBeforeEpoch, DoesComputeISOWeek) {
    // Sunday, December 28, 1969.
    auto date = Date_t::fromMillisSinceEpoch(-345600000LL);
    ASSERT_EQ(TimeZoneDatabase::utcZone().isoWeek(date), 52);
    // Tuesday, December 30, 1969.
    date = Date_t::fromMillisSinceEpoch(-123456000LL);
    ASSERT_EQ(TimeZoneDatabase::utcZone().isoWeek(date), 1);
    // Saturday, January 1, 1966, part of previous year.
    date = Date_t::fromMillisSinceEpoch(-126230400000LL);
    ASSERT_EQ(TimeZoneDatabase::utcZone().isoWeek(date), 52);
    // Tuesday, December 29, 1959.
    date = Date_t::fromMillisSinceEpoch(-315878400000LL);
    ASSERT_EQ(TimeZoneDatabase::utcZone().isoWeek(date), 53);
    // Saturday, January 2, 1960, part of previous ISO year.
    date = Date_t::fromMillisSinceEpoch(-315532800000LL);
    ASSERT_EQ(TimeZoneDatabase::utcZone().isoWeek(date), 53);
}

TEST(NewYorkTimeBeforeEpoch, DoesComputeISOWeek) {
    auto newYorkZone = kDefaultTimeZoneDatabase.getTimeZone("America/New_York");

    // 1969-12-28T00:00:00.000Z (Sunday).
    auto date = Date_t::fromMillisSinceEpoch(-345600000LL);
    ASSERT_EQ(newYorkZone.isoWeek(date), 52);

    // 1969-12-30T00:00:00.000Z (Tuesday).
    date = Date_t::fromMillisSinceEpoch(-123456000LL);
    ASSERT_EQ(newYorkZone.isoWeek(date), 1);

    // 1966-01-01T00:00:00.000Z (Saturday), part of previous year.
    date = Date_t::fromMillisSinceEpoch(-126230400000LL);
    ASSERT_EQ(newYorkZone.isoWeek(date), 52);
    // 1959-12-29T00:00:00.000Z (Tuesday).
    date = Date_t::fromMillisSinceEpoch(-315878400000LL);
    ASSERT_EQ(newYorkZone.isoWeek(date), 53);
    // 1960-01-02T00:00:00.000Z (Saturday), part of previous year.
    date = Date_t::fromMillisSinceEpoch(-315532800000LL);
    ASSERT_EQ(newYorkZone.isoWeek(date), 53);
}

TEST(UTCTimeBeforeEpoch, DoesFormatDate) {
    // Tuesday, Dec 30, 1969 13:42:23:211
    auto date = Date_t::fromMillisSinceEpoch(-123456789LL);
    auto result = TimeZoneDatabase::utcZone().formatDate(
        "%Y/%m/%d %H:%M:%S:%L, dayOfYear: %j, "
        "dayOfWeek: %w, week: %U, isoYear: %G, "
        "isoWeek: %V, isoDayOfWeek: %u, percent: %%",
        date);
    ASSERT_OK(result);
    ASSERT_EQ(
        "1969/12/30 13:42:23:211, dayOfYear: 364, dayOfWeek: 3, week: 52, isoYear: 1970, "
        "isoWeek: 01, isoDayOfWeek: 2, percent: %",
        result.getValue());
}

TEST(NewYorkTimeBeforeEpoch, DoesFormatDate) {
    auto newYorkZone = kDefaultTimeZoneDatabase.getTimeZone("America/New_York");

    // Tuesday, Dec 30, 1969 13:42:23:211
    auto date = Date_t::fromMillisSinceEpoch(-123456789LL);
    auto result = newYorkZone.formatDate(
        "%Y/%m/%d %H:%M:%S:%L, dayOfYear: %j, "
        "dayOfWeek: %w, week: %U, isoYear: %G, "
        "isoWeek: %V, isoDayOfWeek: %u, percent: %%",
        date);
    ASSERT_OK(result);
    ASSERT_EQ(
        "1969/12/30 08:42:23:211, dayOfYear: 364, dayOfWeek: 3, week: 52, isoYear: 1970, "
        "isoWeek: 01, isoDayOfWeek: 2, percent: %",
        result.getValue());
}

TEST(UTCTimeBeforeEpoch, DoesOutputFormatDate) {
    // Tuesday, Dec 30, 1969 13:42:23:211
    auto date = Date_t::fromMillisSinceEpoch(-123456789LL);
    std::ostringstream os;
    ASSERT_OK(TimeZoneDatabase::utcZone().outputDateWithFormat(
        os,
        "%Y/%m/%d %H:%M:%S:%L, dayOfYear: %j, "
        "dayOfWeek: %w, week: %U, isoYear: %G, "
        "isoWeek: %V, isoDayOfWeek: %u, percent: %%",
        date));
    ASSERT_EQ(os.str(),
              "1969/12/30 13:42:23:211, dayOfYear: 364, dayOfWeek: 3, week: 52, isoYear: 1970, "
              "isoWeek: 01, isoDayOfWeek: 2, percent: %");
}

TEST(NewYorkTimeBeforeEpoch, DoesOutputFormatDate) {
    auto newYorkZone = kDefaultTimeZoneDatabase.getTimeZone("America/New_York");

    // Tuesday, Dec 30, 1969 13:42:23:211
    auto date = Date_t::fromMillisSinceEpoch(-123456789LL);
    std::ostringstream os;
    ASSERT_OK(newYorkZone.outputDateWithFormat(os,
                                               "%Y/%m/%d %H:%M:%S:%L, dayOfYear: %j, "
                                               "dayOfWeek: %w, week: %U, isoYear: %G, "
                                               "isoWeek: %V, isoDayOfWeek: %u, percent: %%",
                                               date));
    ASSERT_EQ(os.str(),
              "1969/12/30 08:42:23:211, dayOfYear: 364, dayOfWeek: 3, week: 52, isoYear: 1970, "
              "isoWeek: 01, isoDayOfWeek: 2, percent: %");
}

TEST(UTCTimeAtEpoch, DoesExtractDateParts) {
    // Jan 1, 1970 00:00:00:000
    auto date = Date_t::fromMillisSinceEpoch(0);
    auto dateParts = TimeZoneDatabase::utcZone().dateParts(date);
    ASSERT_EQ(dateParts.year, 1970);
    ASSERT_EQ(dateParts.month, 1);
    ASSERT_EQ(dateParts.dayOfMonth, 1);
    ASSERT_EQ(dateParts.hour, 0);
    ASSERT_EQ(dateParts.minute, 0);
    ASSERT_EQ(dateParts.second, 0);
    ASSERT_EQ(dateParts.millisecond, 0);
}

TEST(NewYorkTimeAtEpoch, DoesExtractDateParts) {
    auto newYorkZone = kDefaultTimeZoneDatabase.getTimeZone("America/New_York");

    // Jan 1, 1970 00:00:00:000
    auto date = Date_t::fromMillisSinceEpoch(0);
    auto dateParts = newYorkZone.dateParts(date);
    ASSERT_EQ(dateParts.year, 1969);
    ASSERT_EQ(dateParts.month, 12);
    ASSERT_EQ(dateParts.dayOfMonth, 31);
    ASSERT_EQ(dateParts.hour, 19);
    ASSERT_EQ(dateParts.minute, 0);
    ASSERT_EQ(dateParts.second, 0);
    ASSERT_EQ(dateParts.millisecond, 0);
}

TEST(UTCTimeAtEpoch, DoesComputeISOYear) {
    // Thursday, January 1, 1970.
    auto date = Date_t::fromMillisSinceEpoch(0);
    ASSERT_EQ(TimeZoneDatabase::utcZone().isoYear(date), 1970);
}

TEST(NewYorkTimeAtEpoch, DoesComputeISOYear) {
    auto newYorkZone = kDefaultTimeZoneDatabase.getTimeZone("America/New_York");

    // 1970-1-1T00:00:00.000Z (Thursday)
    auto date = Date_t::fromMillisSinceEpoch(0);
    ASSERT_EQ(newYorkZone.isoYear(date), 1970);  // The Wednesday is still considered part of 1970.
}

TEST(UTCTimeAtEpoch, DoesComputeDayOfWeek) {
    // Thursday, January 1, 1970.
    auto date = Date_t::fromMillisSinceEpoch(0);
    ASSERT_EQ(TimeZoneDatabase::utcZone().dayOfWeek(date), 5);
}

TEST(NewYorkTimeAtEpoch, DoesComputeDayOfWeek) {
    auto newYorkZone = kDefaultTimeZoneDatabase.getTimeZone("America/New_York");

    // 1970-1-1T00:00:00.000Z (Thursday)
    auto date = Date_t::fromMillisSinceEpoch(0);
    ASSERT_EQ(newYorkZone.dayOfWeek(date), 4);  // The Wednesday before.
}

TEST(UTCTimeAtEpoch, DoesComputeISODayOfWeek) {
    // Thursday, January 1, 1970.
    auto date = Date_t::fromMillisSinceEpoch(0);
    ASSERT_EQ(TimeZoneDatabase::utcZone().isoDayOfWeek(date), 4);
}

TEST(NewYorkTimeAtEpoch, DoesComputeISODayOfWeek) {
    // 1970-1-1T00:00:00.000Z (Thursday)
    auto date = Date_t::fromMillisSinceEpoch(0);
    auto newYorkZone = kDefaultTimeZoneDatabase.getTimeZone("America/New_York");
    ASSERT_EQ(newYorkZone.isoDayOfWeek(date), 3);  // The Wednesday before.
}

TEST(UTCTimeAtEpoch, DoesComputeDayOfYear) {
    // Thursday, January 1, 1970.
    auto date = Date_t::fromMillisSinceEpoch(0);
    ASSERT_EQ(TimeZoneDatabase::utcZone().dayOfYear(date), 1);
}

TEST(NewYorkTimeAtEpoch, DoesComputeDayOfYear) {
    auto newYorkZone = kDefaultTimeZoneDatabase.getTimeZone("America/New_York");

    // 1970-1-1T00:00:00.000Z
    auto date = Date_t::fromMillisSinceEpoch(0);
    ASSERT_EQ(newYorkZone.dayOfYear(date), 365);
}

TEST(UTCTimeAtEpoch, DoesComputeWeek) {
    // Thursday, January 1, 1970.
    auto date = Date_t::fromMillisSinceEpoch(0);
    ASSERT_EQ(TimeZoneDatabase::utcZone().week(date), 0);
}

TEST(NewYorkTimeAtEpoch, DoesComputeWeek) {
    auto newYorkZone = kDefaultTimeZoneDatabase.getTimeZone("America/New_York");

    // 1970-1-1T00:00:00.000Z
    auto date = Date_t::fromMillisSinceEpoch(0);
    ASSERT_EQ(newYorkZone.week(date), 52);
}

TEST(UTCTimeAtEpoch, DoesComputeUtcOffset) {
    // Thursday, January 1, 1970.
    auto date = Date_t::fromMillisSinceEpoch(0);
    ASSERT_EQ(durationCount<Seconds>(TimeZoneDatabase::utcZone().utcOffset(date)), 0);
}

TEST(NewYorkTimeAtEpoch, DoesComputeUtcOffset) {
    auto newYorkZone = kDefaultTimeZoneDatabase.getTimeZone("America/New_York");

    // 1970-1-1T00:00:00.000Z
    auto date = Date_t::fromMillisSinceEpoch(0);
    ASSERT_EQ(durationCount<Hours>(newYorkZone.utcOffset(date)), -5);
}

TEST(UTCTimeAtEpoch, DoesComputeISOWeek) {
    // Thursday, January 1, 1970.
    auto date = Date_t::fromMillisSinceEpoch(0);
    ASSERT_EQ(TimeZoneDatabase::utcZone().isoWeek(date), 1);
}

TEST(NewYorkTimeAtEpoch, DoesComputeISOWeek) {
    auto newYorkZone = kDefaultTimeZoneDatabase.getTimeZone("America/New_York");

    // Thu, Jan 1, 1970 00:00:00:000Z
    auto date = Date_t::fromMillisSinceEpoch(0);
    // This is Wednesday in New York, but that is still part of the first week.
    ASSERT_EQ(newYorkZone.isoWeek(date), 1);
}

TEST(UTCTimeAtEpoch, DoesFormatDate) {
    // Thu, Jan 1, 1970 00:00:00:000
    auto date = Date_t::fromMillisSinceEpoch(0);
    auto result = TimeZoneDatabase::utcZone().formatDate(
        "%Y/%m/%d %H:%M:%S:%L, dayOfYear: %j, "
        "dayOfWeek: %w, week: %U, isoYear: %G, "
        "isoWeek: %V, isoDayOfWeek: %u, percent: %%",
        date);
    ASSERT_OK(result);
    ASSERT_EQ(
        "1970/01/01 00:00:00:000, dayOfYear: 001, dayOfWeek: 5, week: 00, isoYear: 1970, "
        "isoWeek: 01, isoDayOfWeek: 4, percent: %",
        result.getValue());
}

TEST(NewYorkTimeAtEpoch, DoesFormatDate) {
    auto newYorkZone = kDefaultTimeZoneDatabase.getTimeZone("America/New_York");

    // Thu, Jan 1, 1970 00:00:00:000Z
    auto date = Date_t::fromMillisSinceEpoch(0);
    auto result = newYorkZone.formatDate(
        "%Y/%m/%d %H:%M:%S:%L, dayOfYear: %j, "
        "dayOfWeek: %w, week: %U, isoYear: %G, "
        "isoWeek: %V, isoDayOfWeek: %u, percent: %%",
        date);
    ASSERT_OK(result);
    ASSERT_EQ(
        "1969/12/31 19:00:00:000, dayOfYear: 365, dayOfWeek: 4, week: 52, isoYear: 1970, "
        "isoWeek: 01, isoDayOfWeek: 3, percent: %",
        result.getValue());
}

TEST(UTCTimeAtEpoch, DoesOutputFormatDate) {
    auto date = Date_t::fromMillisSinceEpoch(0);
    std::ostringstream os;
    ASSERT_OK(TimeZoneDatabase::utcZone().outputDateWithFormat(
        os,
        "%Y/%m/%d %H:%M:%S:%L, dayOfYear: %j, "
        "dayOfWeek: %w, week: %U, isoYear: %G, "
        "isoWeek: %V, isoDayOfWeek: %u, percent: %%",
        date));
    ASSERT_EQ(os.str(),
              "1970/01/01 00:00:00:000, dayOfYear: 001, dayOfWeek: 5, week: 00, isoYear: 1970, "
              "isoWeek: 01, isoDayOfWeek: 4, percent: %");
}

TEST(NewYorkTimeAtEpoch, DoesOutputFormatDate) {
    auto newYorkZone = kDefaultTimeZoneDatabase.getTimeZone("America/New_York");
    auto date = Date_t::fromMillisSinceEpoch(0);
    std::ostringstream os;
    ASSERT_OK(newYorkZone.outputDateWithFormat(os,
                                               "%Y/%m/%d %H:%M:%S:%L, dayOfYear: %j, "
                                               "dayOfWeek: %w, week: %U, isoYear: %G, "
                                               "isoWeek: %V, isoDayOfWeek: %u, percent: %%",
                                               date));
    ASSERT_EQ(os.str(),
              "1969/12/31 19:00:00:000, dayOfYear: 365, dayOfWeek: 4, week: 52, isoYear: 1970, "
              "isoWeek: 01, isoDayOfWeek: 3, percent: %");
}

TEST(UTCTimeAfterEpoch, DoesExtractDateParts) {
    // Jun 6, 2017 19:38:43:123.
    auto date = Date_t::fromMillisSinceEpoch(1496777923123LL);
    auto dateParts = TimeZoneDatabase::utcZone().dateParts(date);
    ASSERT_EQ(dateParts.year, 2017);
    ASSERT_EQ(dateParts.month, 6);
    ASSERT_EQ(dateParts.dayOfMonth, 6);
    ASSERT_EQ(dateParts.hour, 19);
    ASSERT_EQ(dateParts.minute, 38);
    ASSERT_EQ(dateParts.second, 43);
    ASSERT_EQ(dateParts.millisecond, 123);
}

TEST(NewYorkTimeAfterEpoch, DoesExtractDateParts) {
    auto newYorkZone = kDefaultTimeZoneDatabase.getTimeZone("America/New_York");

    // Jun 6, 2017 19:38:43:123Z.
    auto date = Date_t::fromMillisSinceEpoch(1496777923123LL);
    auto dateParts = newYorkZone.dateParts(date);
    ASSERT_EQ(dateParts.year, 2017);
    ASSERT_EQ(dateParts.month, 6);
    ASSERT_EQ(dateParts.dayOfMonth, 6);
    ASSERT_EQ(dateParts.hour, 15);
    ASSERT_EQ(dateParts.minute, 38);
    ASSERT_EQ(dateParts.second, 43);
    ASSERT_EQ(dateParts.millisecond, 123);
}

TEST(UTCTimeAfterEpoch, DoesComputeISOYear) {
    // Tuesday, June 6, 2017.
    auto date = Date_t::fromMillisSinceEpoch(1496777923000LL);
    ASSERT_EQ(TimeZoneDatabase::utcZone().isoYear(date), 2017);
    // Saturday, January 1, 2005, part of the previous year.
    date = Date_t::fromMillisSinceEpoch(1104537600000LL);
    ASSERT_EQ(TimeZoneDatabase::utcZone().isoYear(date), 2004);
    // Monday, January 1, 2007.
    date = Date_t::fromMillisSinceEpoch(1167609600000LL);
    ASSERT_EQ(TimeZoneDatabase::utcZone().isoYear(date), 2007);
    // Monday, December 31, 2007, part of the next year.
    date = Date_t::fromMillisSinceEpoch(1199059200000LL);
    ASSERT_EQ(TimeZoneDatabase::utcZone().isoYear(date), 2008);
}

TEST(NewYorkTimeAfterEpoch, DoesComputeISOYear) {
    auto newYorkZone = kDefaultTimeZoneDatabase.getTimeZone("America/New_York");

    // 2017-06-06T19:38:43:123Z (Tuesday).
    auto date = Date_t::fromMillisSinceEpoch(1496777923000LL);
    ASSERT_EQ(newYorkZone.isoYear(date), 2017);

    // 2005-01-01T00:00:00.000Z (Saturday), part of 2004.
    date = Date_t::fromMillisSinceEpoch(1104537600000LL);
    ASSERT_EQ(newYorkZone.isoYear(date), 2004);

    // 2007-01-01T00:00:00.000Z (Monday), part of 2007.
    date = Date_t::fromMillisSinceEpoch(1167609600000LL);
    // ISO weeks are Mon-Sun, so this is part of the previous week in New York, so part of the
    // previous year, 2006.
    ASSERT_EQ(newYorkZone.isoYear(date), 2006);

    // 2007-12-31T00:00:00.000Z (Monday), part of 2007.
    date = Date_t::fromMillisSinceEpoch(1199059200000LL);
    // ISO weeks are Mon-Sun, so this is part of the previous week in New York, so part of the
    // previous year, 2007.
    ASSERT_EQ(newYorkZone.isoYear(date), 2007);
}

TEST(UTCTimeAfterEpoch, DoesComputeDayOfWeek) {
    // Tuesday, June 6, 2017.
    auto date = Date_t::fromMillisSinceEpoch(1496777923000LL);
    ASSERT_EQ(TimeZoneDatabase::utcZone().dayOfWeek(date), 3);
    // Saturday, January 1, 2005.
    date = Date_t::fromMillisSinceEpoch(1104537600000LL);
    ASSERT_EQ(TimeZoneDatabase::utcZone().dayOfWeek(date), 7);
    // Monday, January 1, 2007.
    date = Date_t::fromMillisSinceEpoch(1167609600000LL);
    ASSERT_EQ(TimeZoneDatabase::utcZone().dayOfWeek(date), 2);
}

TEST(NewYorkTimeAfterEpoch, DoesComputeDayOfWeek) {
    auto newYorkZone = kDefaultTimeZoneDatabase.getTimeZone("America/New_York");

    // 2017-06-06T19:38:43.123Z (Tuesday).
    auto date = Date_t::fromMillisSinceEpoch(1496777923000LL);
    ASSERT_EQ(newYorkZone.dayOfWeek(date), 3);

    // 2005-01-01T00:00:00.000Z (Saturday).
    date = Date_t::fromMillisSinceEpoch(1104537600000LL);
    ASSERT_EQ(newYorkZone.dayOfWeek(date), 6);  // Part of the previous day in New York.
}

TEST(UTCTimeAfterEpoch, DoesComputeISODayOfWeek) {
    // Tuesday, June 6, 2017.
    auto date = Date_t::fromMillisSinceEpoch(1496777923000LL);
    ASSERT_EQ(TimeZoneDatabase::utcZone().isoDayOfWeek(date), 2);
    // Saturday, January 1, 2005.
    date = Date_t::fromMillisSinceEpoch(1104537600000LL);
    ASSERT_EQ(TimeZoneDatabase::utcZone().isoDayOfWeek(date), 6);
    // Monday, January 1, 2007.
    date = Date_t::fromMillisSinceEpoch(1167609600000LL);
    ASSERT_EQ(TimeZoneDatabase::utcZone().isoDayOfWeek(date), 1);
}

TEST(NewYorkTimeAfterEpoch, DoesComputeISODayOfWeek) {
    auto newYorkZone = kDefaultTimeZoneDatabase.getTimeZone("America/New_York");

    // 2017-06-06T19:38:43.123Z (Tuesday).
    auto date = Date_t::fromMillisSinceEpoch(1496777923000LL);
    ASSERT_EQ(newYorkZone.isoDayOfWeek(date), 2);

    // 2005-01-01T00:00:00.000Z (Saturday).
    date = Date_t::fromMillisSinceEpoch(1104537600000LL);
    ASSERT_EQ(newYorkZone.isoDayOfWeek(date), 5);  // Part of the previous day in New York.
}

TEST(UTCTimeAfterEpoch, DoesComputeDayOfYear) {
    // June 6, 2017.
    auto date = Date_t::fromMillisSinceEpoch(1496777923000LL);
    ASSERT_EQ(TimeZoneDatabase::utcZone().dayOfYear(date), 157);
    // January 1, 2005.
    date = Date_t::fromMillisSinceEpoch(1104537600000LL);
    ASSERT_EQ(TimeZoneDatabase::utcZone().dayOfYear(date), 1);
    // Feb 28, 2008 (leap year).
    date = Date_t::fromMillisSinceEpoch(1204156800000LL);
    ASSERT_EQ(TimeZoneDatabase::utcZone().dayOfYear(date), 59);
    // Feb 29, 2008 (leap year).
    date = Date_t::fromMillisSinceEpoch(1204243200000LL);
    ASSERT_EQ(TimeZoneDatabase::utcZone().dayOfYear(date), 60);
    // Mar 1, 2008 (leap year).
    date = Date_t::fromMillisSinceEpoch(1204329600000LL);
    ASSERT_EQ(TimeZoneDatabase::utcZone().dayOfYear(date), 61);
    // December 31, 2008 (leap year).
    date = Date_t::fromMillisSinceEpoch(1230681600000LL);
    ASSERT_EQ(TimeZoneDatabase::utcZone().dayOfYear(date), 366);

    // Feb 28, 2001 (not leap year).
    date = Date_t::fromMillisSinceEpoch(983318400000LL);
    ASSERT_EQ(TimeZoneDatabase::utcZone().dayOfYear(date), 59);
    // Mar 1, 2001 (not leap year).
    date = Date_t::fromMillisSinceEpoch(983404800000LL);
    ASSERT_EQ(TimeZoneDatabase::utcZone().dayOfYear(date), 60);
    // December 31, 2001 (not leap year).
    date = Date_t::fromMillisSinceEpoch(1009756800000LL);
    ASSERT_EQ(TimeZoneDatabase::utcZone().dayOfYear(date), 365);
}

TEST(NewYorkTimeAfterEpoch, DoesComputeDayOfYear) {
    auto newYorkZone = kDefaultTimeZoneDatabase.getTimeZone("America/New_York");

    // 2017-06-06T19:38:43.123Z.
    auto date = Date_t::fromMillisSinceEpoch(1496777923000LL);
    ASSERT_EQ(newYorkZone.dayOfYear(date), 157);

    // 2005-01-01T00:00:00.000Z, part of 2004 in New York, which was a leap year.
    date = Date_t::fromMillisSinceEpoch(1104537600000LL);
    ASSERT_EQ(newYorkZone.dayOfYear(date), 366);

    // 2008-02-28T00:00:00.000Z (leap year).
    date = Date_t::fromMillisSinceEpoch(1204156800000LL);
    ASSERT_EQ(newYorkZone.dayOfYear(date), 58);
    // 2008-02-29T00:00:00.000Z.
    date = Date_t::fromMillisSinceEpoch(1204243200000LL);
    ASSERT_EQ(newYorkZone.dayOfYear(date), 59);
    // 2008-03-01T00:00:00.000Z.
    date = Date_t::fromMillisSinceEpoch(1204329600000LL);
    ASSERT_EQ(newYorkZone.dayOfYear(date), 60);
    // 2008-12-31T00:00:00.000Z.
    date = Date_t::fromMillisSinceEpoch(1230681600000LL);
    ASSERT_EQ(newYorkZone.dayOfYear(date), 365);
    // 2009-01-01T00:00:00.000Z, part of the previous year in New York.
    date = Date_t::fromMillisSinceEpoch(1230768000000LL);
    ASSERT_EQ(newYorkZone.dayOfYear(date), 366);

    // 2001-02-28T00:00:00.000Z (not leap year).
    date = Date_t::fromMillisSinceEpoch(983318400000LL);
    ASSERT_EQ(newYorkZone.dayOfYear(date), 58);
    // 2001-03-01T00:00:00.000Z.
    date = Date_t::fromMillisSinceEpoch(983404800000LL);
    ASSERT_EQ(newYorkZone.dayOfYear(date), 59);
    // 2001-12-31T00:00:00.000Z.
    date = Date_t::fromMillisSinceEpoch(1009756800000LL);
    ASSERT_EQ(newYorkZone.dayOfYear(date), 364);
    // 2002-01-01T00:00:00.000Z, part of the previous year in New York.
    date = Date_t::fromMillisSinceEpoch(1009843200000LL);
    ASSERT_EQ(newYorkZone.dayOfYear(date), 365);
}

TEST(UTCTimeAfterEpoch, DoesComputeWeek) {
    // Tuesday, June 6, 2017.
    auto date = Date_t::fromMillisSinceEpoch(1496777923000LL);
    ASSERT_EQ(TimeZoneDatabase::utcZone().week(date), 23);
    // Saturday, January 1, 2005, before first Sunday.
    date = Date_t::fromMillisSinceEpoch(1104537600000LL);
    ASSERT_EQ(TimeZoneDatabase::utcZone().week(date), 0);
    // Monday, January 1, 2007, before first Sunday.
    date = Date_t::fromMillisSinceEpoch(1167609600000LL);
    ASSERT_EQ(TimeZoneDatabase::utcZone().week(date), 0);
}

TEST(NewYorkTimeAfterEpoch, DoesComputeWeek) {
    auto newYorkZone = kDefaultTimeZoneDatabase.getTimeZone("America/New_York");

    // Tuesday, June 6, 2017.
    auto date = Date_t::fromMillisSinceEpoch(1496777923000LL);
    ASSERT_EQ(newYorkZone.week(date), 23);

    // 2005-01-01T00:00:00.00Z (Saturday).
    date = Date_t::fromMillisSinceEpoch(1104537600000LL);
    ASSERT_EQ(newYorkZone.week(date), 52);

    // 2007-01-01T00:00:00.00Z (Monday), the last Sunday of 2006 in New York.
    date = Date_t::fromMillisSinceEpoch(1167609600000LL);
    ASSERT_EQ(newYorkZone.week(date), 53);
}

TEST(UTCTimeAfterEpoch, DoesComputeUtcOffset) {
    // Tuesday, June 6, 2017.
    auto date = Date_t::fromMillisSinceEpoch(1496777923000LL);
    ASSERT_EQ(durationCount<Seconds>(TimeZoneDatabase::utcZone().utcOffset(date)), 0);
}

TEST(NewYorkTimeAfterEpoch, DoesComputeUtcOffset) {
    auto newYorkZone = kDefaultTimeZoneDatabase.getTimeZone("America/New_York");

    // Tuesday, June 6, 2017.
    auto date = Date_t::fromMillisSinceEpoch(1496777923000LL);
    ASSERT_EQ(durationCount<Hours>(newYorkZone.utcOffset(date)), -4);

    // 2005-01-01T00:00:00.00Z (Saturday).
    date = Date_t::fromMillisSinceEpoch(1104537600000LL);
    ASSERT_EQ(durationCount<Hours>(newYorkZone.utcOffset(date)), -5);
}

TEST(UTCTimeAfterEpoch, DoesComputeISOWeek) {
    // Tuesday, June 6, 2017.
    auto date = Date_t::fromMillisSinceEpoch(1496777923000LL);
    ASSERT_EQ(TimeZoneDatabase::utcZone().isoWeek(date), 23);
    // Saturday, January 1, 2005, considered part of 2004, which was a leap year.
    date = Date_t::fromMillisSinceEpoch(1104537600000LL);
    ASSERT_EQ(TimeZoneDatabase::utcZone().isoWeek(date), 53);
    // Monday, January 1, 2007.
    date = Date_t::fromMillisSinceEpoch(1167609600000LL);
    ASSERT_EQ(TimeZoneDatabase::utcZone().isoWeek(date), 1);
}

TEST(NewYorkTimeAfterEpoch, DoesComputeISOWeek) {
    auto newYorkZone = kDefaultTimeZoneDatabase.getTimeZone("America/New_York");

    // Tuesday, June 6, 2017.
    auto date = Date_t::fromMillisSinceEpoch(1496777923000LL);
    ASSERT_EQ(newYorkZone.isoWeek(date), 23);

    // 2005-01-01T00:00:00.000Z (Saturday), part of 2004.
    date = Date_t::fromMillisSinceEpoch(1104537600000LL);
    ASSERT_EQ(newYorkZone.isoWeek(date), 53);

    // 2007-01-01T00:00:00.000Z (Monday), part of 2006 in New York.
    date = Date_t::fromMillisSinceEpoch(1167609600000LL);
    ASSERT_EQ(newYorkZone.isoWeek(date), 52);
}

TEST(UTCTimeAfterEpoch, DoesFormatDate) {
    // Tue, Jun 6, 2017 19:38:43:234.
    auto date = Date_t::fromMillisSinceEpoch(1496777923234LL);
    auto result = TimeZoneDatabase::utcZone().formatDate(
        "%Y/%m/%d %H:%M:%S:%L, dayOfYear: %j, "
        "dayOfWeek: %w, week: %U, isoYear: %G, "
        "isoWeek: %V, isoDayOfWeek: %u, percent: %%",
        date);
    ASSERT_OK(result);
    ASSERT_EQ(
        "2017/06/06 19:38:43:234, dayOfYear: 157, dayOfWeek: 3, week: 23, isoYear: 2017, "
        "isoWeek: 23, isoDayOfWeek: 2, percent: %",
        result.getValue());
}

TEST(NewYorkTimeAfterEpoch, DoesFormatDate) {
    // 2017-06-06T19:38:43:234Z (Tuesday).
    auto date = Date_t::fromMillisSinceEpoch(1496777923234LL);
    auto newYorkZone = kDefaultTimeZoneDatabase.getTimeZone("America/New_York");
    auto result = newYorkZone.formatDate(
        "%Y/%m/%d %H:%M:%S:%L, dayOfYear: %j, "
        "dayOfWeek: %w, week: %U, isoYear: %G, "
        "isoWeek: %V, isoDayOfWeek: %u, percent: %%",
        date);
    ASSERT_OK(result);
    ASSERT_EQ(
        "2017/06/06 15:38:43:234, dayOfYear: 157, dayOfWeek: 3, week: 23, isoYear: 2017, "
        "isoWeek: 23, isoDayOfWeek: 2, percent: %",
        result.getValue());
}

TEST(UTCOffsetAfterEpoch, DoesFormatDate) {
    // 2017-06-06T19:38:43:234Z (Tuesday).
    auto date = Date_t::fromMillisSinceEpoch(1496777923234LL);
    auto offsetSpec = kDefaultTimeZoneDatabase.getTimeZone("+02:30");
    auto result = offsetSpec.formatDate(
        "%Y/%m/%d %H:%M:%S:%L, dayOfYear: %j, "
        "dayOfWeek: %w, week: %U, isoYear: %G, "
        "isoWeek: %V, isoDayOfWeek: %u, percent: %%",
        date);
    ASSERT_OK(result);
    ASSERT_EQ(
        "2017/06/06 22:08:43:234, dayOfYear: 157, dayOfWeek: 3, week: 23, isoYear: 2017, "
        "isoWeek: 23, isoDayOfWeek: 2, percent: %",
        result.getValue());
}

TEST(UTCTimeAfterEpoch, DoesOutputFormatDate) {
    // Tue, Jun 6, 2017 19:38:43:234.
    auto date = Date_t::fromMillisSinceEpoch(1496777923234LL);
    std::ostringstream os;
    ASSERT_OK(TimeZoneDatabase::utcZone().outputDateWithFormat(
        os,
        "%Y/%m/%d %H:%M:%S:%L, dayOfYear: %j, "
        "dayOfWeek: %w, week: %U, isoYear: %G, "
        "isoWeek: %V, isoDayOfWeek: %u, percent: %%",
        date));
    ASSERT_EQ(os.str(),
              "2017/06/06 19:38:43:234, dayOfYear: 157, dayOfWeek: 3, week: 23, isoYear: 2017, "
              "isoWeek: 23, isoDayOfWeek: 2, percent: %");
}

TEST(NewYorkTimeAfterEpoch, DoesOutputFormatDate) {
    // 2017-06-06T19:38:43:234Z (Tuesday).
    auto date = Date_t::fromMillisSinceEpoch(1496777923234LL);
    std::ostringstream os;
    auto newYorkZone = kDefaultTimeZoneDatabase.getTimeZone("America/New_York");
    ASSERT_OK(newYorkZone.outputDateWithFormat(
        os,
        "%Y/%m/%d %H:%M:%S:%L, dayOfYear: %j, "
        "dayOfWeek: %w, week: %U, isoYear: %G, "
        "isoWeek: %V, isoDayOfWeek: %u, monthName: %B, monthNameThreeLetter: %b, percent: %%",
        date));
    ASSERT_EQ(
        os.str(),
        "2017/06/06 15:38:43:234, dayOfYear: 157, dayOfWeek: 3, week: 23, isoYear: 2017, "
        "isoWeek: 23, isoDayOfWeek: 2, monthName: June, monthNameThreeLetter: Jun, percent: %");
}

TEST(DateFormat, ThrowsUserExceptionIfGivenUnrecognizedFormatter) {
    ASSERT_THROWS_CODE(
        TimeZoneDatabase::utcZone().validateToStringFormat("%x"), AssertionException, 18536);
    ASSERT_THROWS_CODE(
        TimeZoneDatabase::utcZone().validateFromStringFormat("%x"), AssertionException, 18536);
}

TEST(DateFormat, ThrowsUserExceptionIfGivenUnmatchedPercent) {
    ASSERT_THROWS_CODE(
        TimeZoneDatabase::utcZone().validateToStringFormat("%"), AssertionException, 18535);
    ASSERT_THROWS_CODE(
        TimeZoneDatabase::utcZone().validateToStringFormat("%%%"), AssertionException, 18535);
    ASSERT_THROWS_CODE(
        TimeZoneDatabase::utcZone().validateToStringFormat("blahblah%"), AssertionException, 18535);

    // Repeat the tests with the format map for $dateFromString.
    ASSERT_THROWS_CODE(
        TimeZoneDatabase::utcZone().validateFromStringFormat("%"), AssertionException, 18535);
    ASSERT_THROWS_CODE(
        TimeZoneDatabase::utcZone().validateFromStringFormat("%%%"), AssertionException, 18535);
    ASSERT_THROWS_CODE(TimeZoneDatabase::utcZone().validateFromStringFormat("blahblah%"),
                       AssertionException,
                       18535);
}

TEST(DateFormat, ProducesNonOKStatusGivenDateBeforeYear0) {
    const long long kMillisPerYear = 31556926000;

    ASSERT_EQ(18537,
              TimeZoneDatabase::utcZone()
                  .formatDate("%Y", Date_t::fromMillisSinceEpoch(-(kMillisPerYear * 1971)))
                  .getStatus()
                  .code());

    ASSERT_EQ(18537,
              TimeZoneDatabase::utcZone()
                  .formatDate("%G", Date_t::fromMillisSinceEpoch(-(kMillisPerYear * 1971)))
                  .getStatus()
                  .code());

    auto result = TimeZoneDatabase::utcZone().formatDate(
        "%Y", Date_t::fromMillisSinceEpoch(-(kMillisPerYear * 1970)));
    ASSERT_OK(result);
    ASSERT_EQ("0000", result.getValue());
}

TEST(DateFormat, ProducesNonOKStatusIfGivenDateAfterYear9999) {
    ASSERT_EQ(18537,
              TimeZoneDatabase::utcZone().formatDate("%Y", Date_t::max()).getStatus().code());

    ASSERT_EQ(18537,
              TimeZoneDatabase::utcZone().formatDate("%G", Date_t::max()).getStatus().code());
}

TEST(DateFromString, CorrectlyParsesStringThatMatchesFormat) {
    auto input = "2017-07-04T10:56:02Z";
    auto format = "%Y-%m-%dT%H:%M:%SZ"_sd;
    auto date = kDefaultTimeZoneDatabase.fromString(input, kDefaultTimeZone, format);
    auto result = TimeZoneDatabase::utcZone().formatDate(format, date);
    ASSERT_OK(result);
    ASSERT_EQ(input, result.getValue());
}

TEST(DateFromString, CorrectlyParsesStringWithDayFromYearFormat) {
    auto input = "2017-302";
    auto expected = "2017, Day 303";
    auto inputFormat = "%Y-%j"_sd;
    auto outputFormat = "%Y, Day %j"_sd;
    auto date = kDefaultTimeZoneDatabase.fromString(input, kDefaultTimeZone, inputFormat);
    auto result = TimeZoneDatabase::utcZone().formatDate(outputFormat, date);
    ASSERT_OK(result);
    ASSERT_EQ(expected, result.getValue());
}

TEST(DateFromString, RejectsStringWithInvalidYearFormat) {
    ASSERT_THROWS_CODE(kDefaultTimeZoneDatabase.fromString("201", kDefaultTimeZone, "%Y"_sd),
                       AssertionException,
                       ErrorCodes::ConversionFailure);
    ASSERT_THROWS_CODE(kDefaultTimeZoneDatabase.fromString("20i7", kDefaultTimeZone, "%Y"_sd),
                       AssertionException,
                       ErrorCodes::ConversionFailure);
}

TEST(DateFromString, RejectsStringWithInvalidMinuteFormat) {
    // Minute must be 2 digits with leading zero.
    ASSERT_THROWS_CODE(kDefaultTimeZoneDatabase.fromString(
                           "2017-01-01T00:1:00", kDefaultTimeZone, "%Y-%m-%dT%H%M%S"_sd),
                       AssertionException,
                       ErrorCodes::ConversionFailure);
    ASSERT_THROWS_CODE(kDefaultTimeZoneDatabase.fromString(
                           "2017-01-01T00:0i:00", kDefaultTimeZone, "%Y-%m-%dT%H%M%S"_sd),
                       AssertionException,
                       ErrorCodes::ConversionFailure);
}

TEST(DateFromString, RejectsStringWithInvalidSecondsFormat) {
    // Seconds must be 2 digits with leading zero.
    ASSERT_THROWS_CODE(kDefaultTimeZoneDatabase.fromString(
                           "2017-01-01T00:00:1", kDefaultTimeZone, "%Y-%m-%dT%H%M%S"_sd),
                       AssertionException,
                       ErrorCodes::ConversionFailure);
    ASSERT_THROWS_CODE(kDefaultTimeZoneDatabase.fromString(
                           "2017-01-01T00:00:i0", kDefaultTimeZone, "%Y-%m-%dT%H%M%S"_sd),
                       AssertionException,
                       ErrorCodes::ConversionFailure);
}

TEST(DateFromString, RejectsStringWithInvalidMillisecondsFormat) {
    ASSERT_THROWS_CODE(kDefaultTimeZoneDatabase.fromString(
                           "2017-01-01T00:00:00.i", kDefaultTimeZone, "%Y-%m-%dT%H:%M:%S.%L"_sd),
                       AssertionException,
                       ErrorCodes::ConversionFailure);
}

TEST(DateFromString, RejectsStringWithInvalidISOYear) {
    ASSERT_THROWS_CODE(kDefaultTimeZoneDatabase.fromString("20i7", kDefaultTimeZone, "%G"_sd),
                       AssertionException,
                       ErrorCodes::ConversionFailure);
}

TEST(DateFromString, RejectsStringWithInvalidISOWeekOfYear) {
    // ISO week of year must be between 1 and 53.
    ASSERT_THROWS_CODE(kDefaultTimeZoneDatabase.fromString("2017-55", kDefaultTimeZone, "%G-%V"_sd),
                       AssertionException,
                       ErrorCodes::ConversionFailure);
    ASSERT_THROWS_CODE(kDefaultTimeZoneDatabase.fromString("2017-FF", kDefaultTimeZone, "%G-%V"_sd),
                       AssertionException,
                       ErrorCodes::ConversionFailure);
}

TEST(DateFromString, RejectsStringWithInvalidISODayOfWeek) {
    // Day of week must be single digit between 1 and 7.
    ASSERT_THROWS_CODE(kDefaultTimeZoneDatabase.fromString("2017-8", kDefaultTimeZone, "%G-%u"_sd),
                       AssertionException,
                       ErrorCodes::ConversionFailure);
    ASSERT_THROWS_CODE(kDefaultTimeZoneDatabase.fromString("2017-0", kDefaultTimeZone, "%G-%u"_sd),
                       AssertionException,
                       ErrorCodes::ConversionFailure);
    ASSERT_THROWS_CODE(kDefaultTimeZoneDatabase.fromString("2017-a", kDefaultTimeZone, "%G-%u"_sd),
                       AssertionException,
                       ErrorCodes::ConversionFailure);
    ASSERT_THROWS_CODE(kDefaultTimeZoneDatabase.fromString("2017-11", kDefaultTimeZone, "%G-%u"_sd),
                       AssertionException,
                       ErrorCodes::ConversionFailure);
    ASSERT_THROWS_CODE(
        kDefaultTimeZoneDatabase.fromString("2017-123", kDefaultTimeZone, "%G-%u"_sd),
        AssertionException,
        ErrorCodes::ConversionFailure);
}

TEST(DateFromString, RejectsStringWithInvalidTimezoneOffset) {
    // Timezone offset minutes (%Z) requires format +/-mmm.
    ASSERT_THROWS_CODE(
        kDefaultTimeZoneDatabase.fromString("2017 500", kDefaultTimeZone, "%G %Z"_sd),
        AssertionException,
        ErrorCodes::ConversionFailure);
    ASSERT_THROWS_CODE(
        kDefaultTimeZoneDatabase.fromString("2017 0500", kDefaultTimeZone, "%G %Z"_sd),
        AssertionException,
        ErrorCodes::ConversionFailure);
    ASSERT_THROWS_CODE(
        kDefaultTimeZoneDatabase.fromString("2017 +i00", kDefaultTimeZone, "%G %Z"_sd),
        AssertionException,
        ErrorCodes::ConversionFailure);
}

TEST(DateFromString, EmptyFormatStringThrowsForAllInputs) {
    ASSERT_THROWS_CODE(kDefaultTimeZoneDatabase.fromString("1/1/2017", kDefaultTimeZone, ""_sd),
                       AssertionException,
                       ErrorCodes::ConversionFailure);
    ASSERT_THROWS_CODE(kDefaultTimeZoneDatabase.fromString("", kDefaultTimeZone, ""_sd),
                       AssertionException,
                       ErrorCodes::ConversionFailure);
}

TEST(DayOfWeek, WeekNumber) {
    long long year = 2019;
    long long week = 1;
    long long day = 1;
    long long hour = 0;
    long long minute = 0;
    long long second = 0;
    long long millisecond = 0;

    Date_t baseline = kDefaultTimeZone.createFromIso8601DateParts(
        year, week, day, hour, minute, second, millisecond);

    long long weekDurationInMillis = 7 * 24 * 60 * 60 * 1000;

    for (int weekIt = -10000; weekIt < 10000; weekIt++) {
        // Calculate a date using the ISO 8601 week-numbered year method.
        Date_t dateFromIso8601 = kDefaultTimeZone.createFromIso8601DateParts(
            year, weekIt, day, hour, minute, second, millisecond);

        // Calculate the same date by adding 'weekDurationInMillis' to 'baseline' for each week past
        // the baseline date.
        Date_t dateFromArithmetic = baseline + Milliseconds(weekDurationInMillis * (weekIt - 1));

        // The two methods should produce the same time.
        ASSERT_EQ(dateFromIso8601, dateFromArithmetic);
    }
}

TEST(DayOfWeek, DayNumber) {
    long long year = 2019;
    long long week = 34;
    long long day = 1;
    long long hour = 0;
    long long minute = 0;
    long long second = 0;
    long long millisecond = 0;

    Date_t baseline = kDefaultTimeZone.createFromIso8601DateParts(
        year, week, day, hour, minute, second, millisecond);

    long long dayDurationInMillis = 24 * 60 * 60 * 1000;

    for (int dayIt = -10000; dayIt < 10000; dayIt++) {
        // Calculate a date using the ISO 8601 week-numbered year method.
        Date_t dateFromIso8601 = kDefaultTimeZone.createFromIso8601DateParts(
            year, week, dayIt, hour, minute, second, millisecond);

        // Calculate the same date by adding 'dayDurationInMillis' to 'baseline' for each day past
        // the baseline date.
        Date_t dateFromArithmetic = baseline + Milliseconds(dayDurationInMillis * (dayIt - 1));

        // The two methods should produce the same time.
        ASSERT_EQ(dateFromIso8601, dateFromArithmetic);
    }
}

std::vector<const TimeZone*> setupTestTimezones() {
    static const std::vector<TimeZone> kAllTimezones = [&]() {
        std::vector<TimeZone> timezones{};
        for (auto&& timeZoneId : kDefaultTimeZoneDatabase.getTimeZoneStrings()) {
            timezones.push_back(kDefaultTimeZoneDatabase.getTimeZone(timeZoneId));
        }
        timezones.push_back(kDefaultTimeZoneDatabase.getTimeZone("-10:00"));
        return timezones;
    }();
    std::vector<const TimeZone*> timezones{};
    for (auto&& timezone : kAllTimezones) {
        timezones.push_back(&timezone);
    }
    return timezones;
}

// Time zones for testing.
const TimeZone kNewYorkTimeZone = kDefaultTimeZoneDatabase.getTimeZone("America/New_York");
const TimeZone kAustraliaEuclaTimeZone =
    kDefaultTimeZoneDatabase.getTimeZone("Australia/Eucla");  // UTC offset +08:45.
const TimeZone kEuropeSofiaTimeZone = kDefaultTimeZoneDatabase.getTimeZone("Europe/Sofia");
const TimeZone kAustraliaSydneyTimeZone =
    kDefaultTimeZoneDatabase.getTimeZone("Australia/Sydney");  // UTC offset +11:00.
const TimeZone kUTCMinus10TimeZone =
    kDefaultTimeZoneDatabase.getTimeZone("-10:00");  // UTC offset -10:00.
const TimeZone kAustraliaLordHoweTimeZone =
    kDefaultTimeZoneDatabase.getTimeZone("Australia/Lord_Howe");
const TimeZone kEuropeMadridTimeZone = kDefaultTimeZoneDatabase.getTimeZone("Europe/Madrid");
const std::vector<const TimeZone*> kTimezones{&kDefaultTimeZone,
                                              &kNewYorkTimeZone,
                                              &kAustraliaEuclaTimeZone,
                                              &kEuropeSofiaTimeZone,
                                              &kAustraliaSydneyTimeZone,
                                              &kUTCMinus10TimeZone};
const std::vector<const TimeZone*> kAllTimezones{setupTestTimezones()};

// Verifies 'dateDiff()' with TimeUnit::year.
TEST(DateDiff, Year) {
    ASSERT_EQ(0,
              dateDiff(kNewYorkTimeZone.createFromDateParts(2000, 12, 31, 23, 59, 59, 500),
                       kNewYorkTimeZone.createFromDateParts(2000, 12, 31, 23, 59, 59, 999),
                       TimeUnit::year,
                       kNewYorkTimeZone));
    ASSERT_EQ(1,
              dateDiff(kNewYorkTimeZone.createFromDateParts(2000, 12, 31, 23, 59, 59, 500),
                       kNewYorkTimeZone.createFromDateParts(2001, 1, 1, 0, 0, 0, 0),
                       TimeUnit::year,
                       kNewYorkTimeZone));
    ASSERT_EQ(-1,
              dateDiff(kNewYorkTimeZone.createFromDateParts(2001, 1, 1, 0, 0, 0, 0),
                       kNewYorkTimeZone.createFromDateParts(2000, 12, 31, 23, 59, 59, 500),
                       TimeUnit::year,
                       kNewYorkTimeZone));
    ASSERT_EQ(999,
              dateDiff(kNewYorkTimeZone.createFromDateParts(1002, 1, 1, 0, 0, 0, 0),
                       kNewYorkTimeZone.createFromDateParts(2001, 1, 1, 0, 0, 0, 0),
                       TimeUnit::year,
                       kNewYorkTimeZone));
}

// Verifies 'dateDiff()' with TimeUnit::month.
TEST(DateDiff, Month) {
    ASSERT_EQ(0,
              dateDiff(kNewYorkTimeZone.createFromDateParts(2020, 2, 28, 23, 59, 59, 500),
                       kNewYorkTimeZone.createFromDateParts(2020, 2, 29, 23, 59, 59, 999),
                       TimeUnit::month,
                       kNewYorkTimeZone));
    ASSERT_EQ(1,
              dateDiff(kNewYorkTimeZone.createFromDateParts(2020, 2, 29, 23, 59, 59, 999),
                       kNewYorkTimeZone.createFromDateParts(2020, 3, 1, 0, 0, 0, 0),
                       TimeUnit::month,
                       kNewYorkTimeZone));
    ASSERT_EQ(-14,
              dateDiff(kNewYorkTimeZone.createFromDateParts(2010, 2, 28, 23, 59, 59, 999),
                       kNewYorkTimeZone.createFromDateParts(2008, 12, 31, 23, 59, 59, 500),
                       TimeUnit::month,
                       kNewYorkTimeZone));
    ASSERT_EQ(1500 * 12,
              dateDiff(kNewYorkTimeZone.createFromDateParts(520, 3, 1, 0, 0, 0, 0),
                       kNewYorkTimeZone.createFromDateParts(2020, 3, 1, 0, 0, 0, 0),
                       TimeUnit::month,
                       kNewYorkTimeZone));
}

// Verifies 'dateDiff()' with TimeUnit::quarter.
TEST(DateDiff, Quarter) {
    ASSERT_EQ(0,
              dateDiff(kNewYorkTimeZone.createFromDateParts(2020, 1, 1, 0, 0, 0, 0),
                       kNewYorkTimeZone.createFromDateParts(2020, 3, 31, 23, 59, 59, 999),
                       TimeUnit::quarter,
                       kNewYorkTimeZone));
    ASSERT_EQ(1,
              dateDiff(kNewYorkTimeZone.createFromDateParts(2020, 1, 1, 0, 0, 0, 0),
                       kNewYorkTimeZone.createFromDateParts(2020, 4, 1, 0, 0, 0, 0),
                       TimeUnit::quarter,
                       kNewYorkTimeZone));
    ASSERT_EQ(-2001,
              dateDiff(kNewYorkTimeZone.createFromDateParts(2000, 12, 31, 23, 59, 59, 500),
                       kNewYorkTimeZone.createFromDateParts(1500, 9, 30, 23, 59, 59, 999),
                       TimeUnit::quarter,
                       kNewYorkTimeZone));
}

// Verifies 'dateDiff()' with TimeUnit::week.
TEST(DateDiff, Week) {
    // Cases when the first day of the week is Monday.
    ASSERT_EQ(1,
              dateDiff(kNewYorkTimeZone.createFromDateParts(2020, 11, 2, 0, 0, 0, 0),
                       kNewYorkTimeZone.createFromDateParts(2020, 11, 9, 0, 0, 0, 0),
                       TimeUnit::week,
                       kNewYorkTimeZone,
                       DayOfWeek::monday));
    ASSERT_EQ(1,
              dateDiff(kNewYorkTimeZone.createFromDateParts(2020, 11, 2, 0, 0, 0, 0),
                       kNewYorkTimeZone.createFromDateParts(2020, 11, 15, 0, 0, 0, 0),
                       TimeUnit::week,
                       kNewYorkTimeZone,
                       DayOfWeek::monday));
    ASSERT_EQ(0,
              dateDiff(kNewYorkTimeZone.createFromDateParts(2020, 11, 2, 0, 0, 0, 0),
                       kNewYorkTimeZone.createFromDateParts(2020, 11, 8, 0, 0, 0, 0),
                       TimeUnit::week,
                       kNewYorkTimeZone,
                       DayOfWeek::monday));
    ASSERT_EQ(0,
              dateDiff(kNewYorkTimeZone.createFromDateParts(2020, 11, 2, 0, 0, 0, 0),
                       kNewYorkTimeZone.createFromDateParts(2020, 11, 2, 0, 0, 0, 0),
                       TimeUnit::week,
                       kNewYorkTimeZone,
                       DayOfWeek::monday));
    ASSERT_EQ(0,
              dateDiff(kNewYorkTimeZone.createFromDateParts(2020, 11, 2, 0, 0, 0, 0),
                       kNewYorkTimeZone.createFromDateParts(2020, 11, 3, 0, 0, 0, 0),
                       TimeUnit::week,
                       kNewYorkTimeZone,
                       DayOfWeek::monday));
    ASSERT_EQ(1,
              dateDiff(kNewYorkTimeZone.createFromDateParts(2020, 11, 8, 0, 0, 0, 0),
                       kNewYorkTimeZone.createFromDateParts(2020, 11, 9, 0, 0, 0, 0),
                       TimeUnit::week,
                       kNewYorkTimeZone,
                       DayOfWeek::monday));
    ASSERT_EQ(1,
              dateDiff(kNewYorkTimeZone.createFromDateParts(2020, 11, 8, 0, 0, 0, 0),
                       kNewYorkTimeZone.createFromDateParts(2020, 11, 15, 0, 0, 0, 0),
                       TimeUnit::week,
                       kNewYorkTimeZone,
                       DayOfWeek::monday));
    ASSERT_EQ(-5,
              dateDiff(kNewYorkTimeZone.createFromDateParts(2020, 11, 10, 0, 0, 0, 0),
                       kNewYorkTimeZone.createFromDateParts(2020, 10, 8, 0, 0, 0, 0),
                       TimeUnit::week,
                       kNewYorkTimeZone,
                       DayOfWeek::monday));

    // Cases when the first day of the week is not Monday.
    ASSERT_EQ(1,
              dateDiff(kNewYorkTimeZone.createFromDateParts(2020, 11, 13, 0, 0, 0, 0),  // Friday.
                       kNewYorkTimeZone.createFromDateParts(2020, 11, 15, 0, 0, 0, 0),  // Sunday.
                       TimeUnit::week,
                       kNewYorkTimeZone,
                       DayOfWeek::sunday));
    ASSERT_EQ(0,
              dateDiff(kNewYorkTimeZone.createFromDateParts(2020, 11, 13, 0, 0, 0, 0),  // Friday.
                       kNewYorkTimeZone.createFromDateParts(2020, 11, 14, 0, 0, 0, 0),  // Saturday.
                       TimeUnit::week,
                       kNewYorkTimeZone,
                       DayOfWeek::sunday));
    ASSERT_EQ(1,
              dateDiff(kNewYorkTimeZone.createFromDateParts(2020, 11, 8, 0, 0, 0, 0),   // Sunday.
                       kNewYorkTimeZone.createFromDateParts(2020, 11, 15, 0, 0, 0, 0),  // Sunday.
                       TimeUnit::week,
                       kNewYorkTimeZone,
                       DayOfWeek::sunday));
    ASSERT_EQ(2,
              dateDiff(kNewYorkTimeZone.createFromDateParts(2020, 11, 7, 0, 0, 0, 0),   // Saturday.
                       kNewYorkTimeZone.createFromDateParts(2020, 11, 15, 0, 0, 0, 0),  // Sunday.
                       TimeUnit::week,
                       kNewYorkTimeZone,
                       DayOfWeek::sunday));
    ASSERT_EQ(0,
              dateDiff(kNewYorkTimeZone.createFromDateParts(2020, 11, 4, 0, 0, 0, 0),  // Wednesday.
                       kNewYorkTimeZone.createFromDateParts(2020, 11, 10, 0, 0, 0, 0),  // Tuesday.
                       TimeUnit::week,
                       kNewYorkTimeZone,
                       DayOfWeek::wednesday));
}

// Verifies 'dateDiff()' with TimeUnit::day.
TEST(DateDiff, Day) {
    ASSERT_EQ(0,
              dateDiff(kNewYorkTimeZone.createFromDateParts(2020, 11, 8, 0, 0, 0, 0),
                       kNewYorkTimeZone.createFromDateParts(2020, 11, 8, 23, 59, 59, 999),
                       TimeUnit::day,
                       kNewYorkTimeZone));
    ASSERT_EQ(1,
              dateDiff(kNewYorkTimeZone.createFromDateParts(2020, 11, 8, 0, 0, 0, 0),
                       kNewYorkTimeZone.createFromDateParts(2020, 11, 9, 0, 0, 0, 0),
                       TimeUnit::day,
                       kNewYorkTimeZone));
    ASSERT_EQ(-1,
              dateDiff(kNewYorkTimeZone.createFromDateParts(2020, 11, 9, 0, 0, 0, 0),
                       kNewYorkTimeZone.createFromDateParts(2020, 11, 8, 23, 59, 59, 999),
                       TimeUnit::day,
                       kNewYorkTimeZone));

    // Verifies number of days in a year calculation.
    ASSERT_EQ(369,
              dateDiff(kNewYorkTimeZone.createFromDateParts(1999, 12, 30, 0, 0, 0, 0),
                       kNewYorkTimeZone.createFromDateParts(2001, 1, 2, 0, 0, 0, 0),
                       TimeUnit::day,
                       kNewYorkTimeZone));
    ASSERT_EQ(6575,
              dateDiff(kNewYorkTimeZone.createFromDateParts(1583, 1, 1, 0, 0, 0, 0),
                       kNewYorkTimeZone.createFromDateParts(1601, 1, 1, 0, 0, 0, 0),
                       TimeUnit::day,
                       kNewYorkTimeZone));
    ASSERT_EQ(-6575,
              dateDiff(kNewYorkTimeZone.createFromDateParts(1601, 1, 1, 0, 0, 0, 0),
                       kNewYorkTimeZone.createFromDateParts(1583, 1, 1, 0, 0, 0, 0),
                       TimeUnit::day,
                       kNewYorkTimeZone));
    ASSERT_EQ(29,
              dateDiff(kNewYorkTimeZone.createFromDateParts(2004, 2, 10, 0, 0, 0, 0),
                       kNewYorkTimeZone.createFromDateParts(2004, 3, 10, 0, 0, 0, 0),
                       TimeUnit::day,
                       kNewYorkTimeZone));
    ASSERT_EQ(28,
              dateDiff(kAustraliaEuclaTimeZone.createFromDateParts(2005, 2, 10, 0, 0, 0, 0),
                       kAustraliaEuclaTimeZone.createFromDateParts(2005, 3, 10, 0, 0, 0, 0),
                       TimeUnit::day,
                       kAustraliaEuclaTimeZone));

    // Use timelib_day_of_year as an oracle to verify day calculations.
    for (int year = -1000; year < 3000; ++year) {
        int expectedNumberOfDays = timelib_day_of_year(year, 12, 31) + 1;
        ASSERT_EQ(expectedNumberOfDays,
                  dateDiff(kNewYorkTimeZone.createFromDateParts(year, 2, 3, 0, 0, 0, 0),
                           kNewYorkTimeZone.createFromDateParts(year + 1, 2, 3, 0, 0, 0, 0),
                           TimeUnit::day,
                           kNewYorkTimeZone));
    }
}

// Verifies 'dateDiff()' with TimeUnit::hour.
TEST(DateDiff, Hour) {
    ASSERT_EQ(0,
              dateDiff(kNewYorkTimeZone.createFromDateParts(2020, 11, 8, 1, 0, 0, 0),
                       kNewYorkTimeZone.createFromDateParts(2020, 11, 8, 1, 59, 59, 999),
                       TimeUnit::hour,
                       kNewYorkTimeZone));
    ASSERT_EQ(1,
              dateDiff(kNewYorkTimeZone.createFromDateParts(2020, 11, 8, 1, 59, 59, 999),
                       kNewYorkTimeZone.createFromDateParts(2020, 11, 8, 2, 0, 0, 0),
                       TimeUnit::hour,
                       kNewYorkTimeZone));
    ASSERT_EQ(-25,
              dateDiff(kNewYorkTimeZone.createFromDateParts(2020, 11, 10, 0, 0, 0, 0),
                       kNewYorkTimeZone.createFromDateParts(2020, 11, 8, 23, 59, 59, 999),
                       TimeUnit::hour,
                       kNewYorkTimeZone));

    // Hour difference calculation in UTC offset +08:45 time zone.
    ASSERT_EQ(
        1,
        dateDiff(
            kAustraliaEuclaTimeZone.createFromDateParts(2020, 11, 10, 20, 55, 0, 0) /*UTC 12:10*/,
            kAustraliaEuclaTimeZone.createFromDateParts(2020, 11, 10, 21, 5, 0, 0) /*UTC 12:20*/,
            TimeUnit::hour,
            kAustraliaEuclaTimeZone));

    // Test of transition from DST to standard time.
    ASSERT_EQ(1,
              dateDiff(kDefaultTimeZone.createFromDateParts(
                           2020, 11, 1, 5, 0, 0, 0) /* America/New_York 1:00AM EDT (UTC-4)*/,
                       kDefaultTimeZone.createFromDateParts(
                           2020, 11, 1, 6, 0, 0, 0) /* America/New_York 1:00AM EST (UTC-5)*/,
                       TimeUnit::hour,
                       kNewYorkTimeZone));
    ASSERT_EQ(-1,
              dateDiff(kDefaultTimeZone.createFromDateParts(
                           2020, 11, 1, 6, 0, 0, 0) /* America/New_York 1:00AM EST (UTC-5)*/,
                       kDefaultTimeZone.createFromDateParts(
                           2020, 11, 1, 5, 0, 0, 0) /* America/New_York 1:00AM EDT (UTC-4)*/,
                       TimeUnit::hour,
                       kNewYorkTimeZone));

    // Test of transition from standard time to DST.
    ASSERT_EQ(1,
              dateDiff(kDefaultTimeZone.createFromDateParts(
                           2020, 3, 8, 6, 45, 0, 0) /* America/New_York 1:45AM EST (UTC-5)*/,
                       kDefaultTimeZone.createFromDateParts(
                           2020, 3, 8, 7, 0, 0, 0) /* America/New_York 3:00AM EDT (UTC-4)*/,
                       TimeUnit::hour,
                       kNewYorkTimeZone));
    ASSERT_EQ(-1,
              dateDiff(kDefaultTimeZone.createFromDateParts(
                           2020, 3, 8, 7, 0, 0, 0) /* America/New_York 3:00AM EDT (UTC-4)*/,
                       kDefaultTimeZone.createFromDateParts(
                           2020, 3, 8, 6, 45, 0, 0) /* America/New_York 1:45AM EST (UTC-5)*/,
                       TimeUnit::hour,
                       kNewYorkTimeZone));

    // Longer period test.
    ASSERT_EQ(17545,
              dateDiff(kNewYorkTimeZone.createFromDateParts(1999, 1, 1, 0, 0, 0, 0),
                       kNewYorkTimeZone.createFromDateParts(2001, 1, 1, 1, 0, 0, 0),
                       TimeUnit::hour,
                       kNewYorkTimeZone));

    // Tests with Australia/Lord_Howe time zone that has 00:30 hour Daylight Savings Time (DST) UTC
    // offset change. 'startDate' and 'endDate' parameters span a transition from/to DST.
    //
    // Verify that even when the UTC offset change is 30 minutes on transition from DST to Standard
    // Time, time difference in hours is based on the local time. In the test 1.5h of real time
    // passes, but the returned difference is 1h.
    ASSERT_EQ(1,
              dateDiff(kAustraliaLordHoweTimeZone.createFromDateParts(2021, 4, 4, 1, 0, 0, 0),
                       kAustraliaLordHoweTimeZone.createFromDateParts(2021, 4, 4, 2, 0, 0, 0),
                       TimeUnit::hour,
                       kAustraliaLordHoweTimeZone));

    // Verify that even when the UTC offset change is 30 minutes on transition from Standard Time to
    // DST, time difference in hours is based on the local time. In the test 1h of real time passes
    // and the returned difference is 1h.
    ASSERT_EQ(1,
              dateDiff(kAustraliaLordHoweTimeZone.createFromDateParts(2021, 10, 3, 1, 0, 0, 0),
                       kAustraliaLordHoweTimeZone.createFromDateParts(2021, 10, 3, 2, 30, 0, 0),
                       TimeUnit::hour,
                       kAustraliaLordHoweTimeZone));
}

// Verifies 'dateDiff()' with TimeUnit::minute.
TEST(DateDiff, Minute) {
    ASSERT_EQ(0,
              dateDiff(kNewYorkTimeZone.createFromDateParts(2020, 11, 8, 1, 30, 0, 0),
                       kNewYorkTimeZone.createFromDateParts(2020, 11, 8, 1, 30, 59, 999),
                       TimeUnit::minute,
                       kNewYorkTimeZone));
    ASSERT_EQ(1,
              dateDiff(kNewYorkTimeZone.createFromDateParts(2020, 11, 8, 1, 30, 59, 999),
                       kNewYorkTimeZone.createFromDateParts(2020, 11, 8, 1, 31, 0, 0),
                       TimeUnit::minute,
                       kNewYorkTimeZone));
    ASSERT_EQ(-25,
              dateDiff(kNewYorkTimeZone.createFromDateParts(2020, 11, 8, 1, 55, 0, 0),
                       kNewYorkTimeZone.createFromDateParts(2020, 11, 8, 1, 30, 59, 999),
                       TimeUnit::minute,
                       kNewYorkTimeZone));
    ASSERT_EQ(234047498,
              dateDiff(kNewYorkTimeZone.createFromDateParts(1585, 11, 8, 1, 55, 0, 0),
                       kNewYorkTimeZone.createFromDateParts(2030, 11, 8, 1, 30, 59, 999),
                       TimeUnit::minute,
                       kNewYorkTimeZone));

    // Tests with Australia/Lord_Howe time zone that has 00:30 hour Daylight Savings Time (DST) UTC
    // offset change. 'startDate' and 'endDate' parameters span a transition from/to DST.
    ASSERT_EQ(90,
              dateDiff(kAustraliaLordHoweTimeZone.createFromDateParts(
                           2021, 4, 4, 1, 0, 0, 0),  // UTC 2021-04-03T14:00:00
                       kAustraliaLordHoweTimeZone.createFromDateParts(
                           2021, 4, 4, 2, 0, 0, 0),  // UTC 2021-04-03T15:30:00
                       TimeUnit::minute,
                       kAustraliaLordHoweTimeZone));
    ASSERT_EQ(60,
              dateDiff(kAustraliaLordHoweTimeZone.createFromDateParts(
                           2021, 10, 3, 1, 0, 0, 0),  // UTC 2021-10-02T14:30:00
                       kAustraliaLordHoweTimeZone.createFromDateParts(
                           2021, 10, 3, 2, 30, 0, 0),  // UTC 2021-10-02T15:30:00
                       TimeUnit::minute,
                       kAustraliaLordHoweTimeZone));
}

// Verifies 'dateDiff()' with TimeUnit::second.
TEST(DateDiff, Second) {
    ASSERT_EQ(0,
              dateDiff(kNewYorkTimeZone.createFromDateParts(2020, 11, 8, 1, 30, 15, 0),
                       kNewYorkTimeZone.createFromDateParts(2020, 11, 8, 1, 30, 15, 999),
                       TimeUnit::second,
                       kNewYorkTimeZone));
    ASSERT_EQ(1,
              dateDiff(kNewYorkTimeZone.createFromDateParts(2020, 11, 8, 1, 30, 15, 999),
                       kNewYorkTimeZone.createFromDateParts(2020, 11, 8, 1, 30, 16, 0),
                       TimeUnit::second,
                       kNewYorkTimeZone));
    ASSERT_EQ(-2401,
              dateDiff(kNewYorkTimeZone.createFromDateParts(2020, 11, 8, 2, 10, 16, 999),
                       kNewYorkTimeZone.createFromDateParts(2020, 11, 8, 1, 30, 15, 999),
                       TimeUnit::second,
                       kNewYorkTimeZone));
    ASSERT_EQ(1604971816,
              dateDiff(kDefaultTimeZone.createFromDateParts(1970, 1, 1, 0, 0, 0, 0),
                       kDefaultTimeZone.createFromDateParts(2020, 11, 10, 1, 30, 16, 0),
                       TimeUnit::second,
                       kNewYorkTimeZone));

    // Verify that negative milliseconds from the Unix Epoch are properly handled.
    ASSERT_EQ(2,
              dateDiff(kDefaultTimeZone.createFromDateParts(1969, 12, 31, 23, 59, 59, 999),
                       kDefaultTimeZone.createFromDateParts(1970, 1, 1, 0, 0, 1, 0),
                       TimeUnit::second,
                       kDefaultTimeZone));

    // Tests with Australia/Lord_Howe time zone that has 00:30 hour Daylight Savings Time (DST) UTC
    // offset change. 'startDate' and 'endDate' parameters span a transition from/to DST.
    const int secondsPerMinute{60};
    ASSERT_EQ(90 * secondsPerMinute,
              dateDiff(kAustraliaLordHoweTimeZone.createFromDateParts(
                           2021, 4, 4, 1, 0, 0, 0),  // UTC 2021-04-03T14:00:00
                       kAustraliaLordHoweTimeZone.createFromDateParts(
                           2021, 4, 4, 2, 0, 0, 0),  // UTC 2021-04-03T15:30:00
                       TimeUnit::second,
                       kAustraliaLordHoweTimeZone));
    ASSERT_EQ(60 * secondsPerMinute,
              dateDiff(kAustraliaLordHoweTimeZone.createFromDateParts(
                           2021, 10, 3, 1, 0, 0, 0),  // UTC 2021-10-02T14:30:00
                       kAustraliaLordHoweTimeZone.createFromDateParts(
                           2021, 10, 3, 2, 30, 0, 0),  // UTC 2021-10-02T15:30:00
                       TimeUnit::second,
                       kAustraliaLordHoweTimeZone));

    // Verify that UTC offset adjustments are properly accounted for when calculating the time
    // difference. Time zone Europe/Madrid skips 0:14:44 hours at 1900-12-31 23:45:15 to change the
    // timezone to UTC.
    ASSERT_EQ(1,
              dateDiff(kEuropeMadridTimeZone.createFromDateParts(1900, 12, 31, 23, 45, 15, 0),
                       kEuropeMadridTimeZone.createFromDateParts(1901, 1, 1, 0, 0, 0, 0),
                       TimeUnit::second,
                       kEuropeMadridTimeZone));
}

// Verifies 'dateDiff()' with TimeUnit::millisecond.
TEST(DateDiff, Millisecond) {
    ASSERT_EQ(100,
              dateDiff(kNewYorkTimeZone.createFromDateParts(2020, 11, 8, 1, 30, 15, 0),
                       kNewYorkTimeZone.createFromDateParts(2020, 11, 8, 1, 30, 15, 100),
                       TimeUnit::millisecond,
                       kNewYorkTimeZone));
    ASSERT_EQ(-1500,
              dateDiff(kNewYorkTimeZone.createFromDateParts(2020, 11, 8, 1, 30, 16, 500),
                       kNewYorkTimeZone.createFromDateParts(2020, 11, 8, 1, 30, 15, 0),
                       TimeUnit::millisecond,
                       kNewYorkTimeZone));
    ASSERT_EQ(1604971816000,
              dateDiff(kDefaultTimeZone.createFromDateParts(1970, 1, 1, 0, 0, 0, 0),
                       kDefaultTimeZone.createFromDateParts(2020, 11, 10, 1, 30, 16, 0),
                       TimeUnit::millisecond,
                       kNewYorkTimeZone));

    // Verifies numeric overflow handling.
    ASSERT_THROWS_CODE(dateDiff(Date_t::fromMillisSinceEpoch(std::numeric_limits<long long>::min()),
                                Date_t::fromMillisSinceEpoch(std::numeric_limits<long long>::max()),
                                TimeUnit::millisecond,
                                kNewYorkTimeZone),
                       AssertionException,
                       5166308);
}

// Verifies 'truncateDate()' with TimeUnit::millisecond.
TEST(TruncateDate, Millisecond) {
    for (auto* timezone : kTimezones) {
        // Value in the middle of a bin above default reference point.
        ASSERT_EQ(timezone->createFromDateParts(2000, 1, 1, 0, 0, 1, 20),
                  truncateDate(timezone->createFromDateParts(2000, 1, 1, 0, 0, 2, 0),
                               TimeUnit::millisecond,
                               1020,
                               *timezone));

        // Value at the lower boundary of a bin.
        ASSERT_EQ(timezone->createFromDateParts(2000, 1, 1, 0, 0, 1, 20),
                  truncateDate(timezone->createFromDateParts(2000, 1, 1, 0, 0, 1, 20),
                               TimeUnit::millisecond,
                               1020,
                               *timezone));

        // Value at the top of a bin.
        ASSERT_EQ(timezone->createFromDateParts(2000, 1, 1, 0, 0, 1, 20),
                  truncateDate(timezone->createFromDateParts(2000, 1, 1, 0, 0, 2, 39),
                               TimeUnit::millisecond,
                               1020,
                               *timezone));

        // Value at the bottom of the next bin.
        ASSERT_EQ(timezone->createFromDateParts(2000, 1, 1, 0, 0, 2, 40),
                  truncateDate(timezone->createFromDateParts(2000, 1, 1, 0, 0, 2, 40),
                               TimeUnit::millisecond,
                               1020,
                               *timezone));

        // Value at the top of a bin below default reference point.
        ASSERT_EQ(timezone->createFromDateParts(1999, 12, 31, 23, 59, 58, 500),
                  truncateDate(timezone->createFromDateParts(1999, 12, 31, 23, 59, 59, 999),
                               TimeUnit::millisecond,
                               1500,
                               *timezone));
    }
}

// Verifies 'truncateDate()' with TimeUnit::second.
TEST(TruncateDate, Second) {
    for (auto* timezone : kTimezones) {
        // Value in the middle of a bin above default reference point.
        ASSERT_EQ(timezone->createFromDateParts(2000, 1, 1, 0, 0, 5, 0),
                  truncateDate(timezone->createFromDateParts(2000, 1, 1, 0, 0, 9, 0),
                               TimeUnit::second,
                               5,
                               *timezone));

        // Value at the lower boundary of a bin.
        ASSERT_EQ(timezone->createFromDateParts(2000, 1, 1, 0, 0, 5, 0),
                  truncateDate(timezone->createFromDateParts(2000, 1, 1, 0, 0, 5, 0),
                               TimeUnit::second,
                               5,
                               *timezone));

        // Value at the top of a bin.
        ASSERT_EQ(timezone->createFromDateParts(2000, 1, 1, 0, 0, 5, 0),
                  truncateDate(timezone->createFromDateParts(2000, 1, 1, 0, 0, 9, 999),
                               TimeUnit::second,
                               5,
                               *timezone));

        // Value at the bottom of the next bin.
        ASSERT_EQ(timezone->createFromDateParts(2000, 1, 1, 0, 0, 10, 0),
                  truncateDate(timezone->createFromDateParts(2000, 1, 1, 0, 0, 10, 0),
                               TimeUnit::second,
                               5,
                               *timezone));

        // Value at the top of a bin below default reference point.
        ASSERT_EQ(timezone->createFromDateParts(1999, 12, 31, 23, 59, 50, 0),
                  truncateDate(timezone->createFromDateParts(1999, 12, 31, 23, 59, 51, 0),
                               TimeUnit::second,
                               5,
                               *timezone));
    }
}

// Verifies 'truncateDate()' with TimeUnit::minute.
TEST(TruncateDate, Minute) {
    for (auto* timezone : kTimezones) {
        // Value in the middle of a bin above default reference point.
        ASSERT_EQ(timezone->createFromDateParts(2000, 1, 1, 0, 5, 0, 0),
                  truncateDate(timezone->createFromDateParts(2000, 1, 1, 0, 9, 0, 0),
                               TimeUnit::minute,
                               5,
                               *timezone));

        // Value at the lower boundary of a bin.
        ASSERT_EQ(timezone->createFromDateParts(2000, 1, 1, 0, 5, 0, 0),
                  truncateDate(timezone->createFromDateParts(2000, 1, 1, 0, 5, 0, 0),
                               TimeUnit::minute,
                               5,
                               *timezone));

        // Value at the top of a bin.
        ASSERT_EQ(timezone->createFromDateParts(2000, 1, 1, 0, 5, 0, 0),
                  truncateDate(timezone->createFromDateParts(2000, 1, 1, 0, 9, 59, 999),
                               TimeUnit::minute,
                               5,
                               *timezone));

        // Value at the bottom of the next bin.
        ASSERT_EQ(timezone->createFromDateParts(2000, 1, 1, 0, 10, 0, 0),
                  truncateDate(timezone->createFromDateParts(2000, 1, 1, 0, 10, 10, 0),
                               TimeUnit::minute,
                               5,
                               *timezone));

        // Value at the top of a bin below default reference point.
        ASSERT_EQ(timezone->createFromDateParts(1999, 12, 31, 23, 50, 0, 0),
                  truncateDate(timezone->createFromDateParts(1999, 12, 31, 23, 54, 0, 0),
                               TimeUnit::minute,
                               5,
                               *timezone));
    }
}

// Verifies 'truncateDate()' with TimeUnit::hour.
TEST(TruncateDate, Hour) {
    for (auto* timezone : kTimezones) {
        // Value in the middle of a bin above default reference point.
        ASSERT_EQ(timezone->createFromDateParts(2020, 1, 1, 2, 0, 0, 0),
                  truncateDate(timezone->createFromDateParts(2020, 1, 1, 3, 0, 0, 0),
                               TimeUnit::hour,
                               2,
                               *timezone));

        // Value at the lower boundary of a bin.
        ASSERT_EQ(timezone->createFromDateParts(2020, 1, 1, 2, 0, 0, 0),
                  truncateDate(timezone->createFromDateParts(2020, 1, 1, 2, 0, 0, 0),
                               TimeUnit::hour,
                               2,
                               *timezone));

        // Value at the top of a bin.
        ASSERT_EQ(timezone->createFromDateParts(2020, 1, 1, 2, 0, 0, 0),
                  truncateDate(timezone->createFromDateParts(2020, 1, 1, 3, 59, 59, 999),
                               TimeUnit::hour,
                               2,
                               *timezone));

        // Value at the bottom of the next bin.
        ASSERT_EQ(timezone->createFromDateParts(2020, 1, 1, 4, 0, 0, 0),
                  truncateDate(timezone->createFromDateParts(2020, 1, 1, 4, 0, 0, 0),
                               TimeUnit::hour,
                               2,
                               *timezone));

        // Value at the top of a bin below default reference point.
        ASSERT_EQ(timezone->createFromDateParts(1999, 12, 31, 22, 0, 0, 0),
                  truncateDate(timezone->createFromDateParts(1999, 12, 31, 23, 59, 51, 0),
                               TimeUnit::hour,
                               2,
                               *timezone));
    }
}

// Verifies 'truncateDate()' with TimeUnit::day.
TEST(TruncateDate, Day) {
    for (auto* timezone : kAllTimezones) {
        // Value in the middle of a bin above default reference point.
        ASSERT_EQ(timezone->createFromDateParts(2000, 1, 7, 0, 0, 0, 0),
                  truncateDate(timezone->createFromDateParts(2000, 1, 8, 0, 0, 2, 0),
                               TimeUnit::day,
                               2,
                               *timezone));

        // Value at the lower boundary of a bin.
        ASSERT_EQ(timezone->createFromDateParts(2000, 1, 9, 0, 0, 0, 0),
                  truncateDate(timezone->createFromDateParts(2000, 1, 9, 0, 0, 0, 0),
                               TimeUnit::day,
                               2,
                               *timezone));

        // Value at the top of a bin.
        ASSERT_EQ(timezone->createFromDateParts(2000, 1, 9, 0, 0, 0, 0),
                  truncateDate(timezone->createFromDateParts(2000, 1, 10, 23, 59, 59, 999),
                               TimeUnit::day,
                               2,
                               *timezone));

        // Value at the top of a bin below default reference point.
        ASSERT_EQ(timezone->createFromDateParts(1999, 12, 28, 0, 0, 0, 0),
                  truncateDate(timezone->createFromDateParts(1999, 12, 29, 23, 59, 59, 999),
                               TimeUnit::day,
                               2,
                               *timezone));
    }

    // Daylight Savings Time tests with America/New_York timezone.
    // Day before DST is on.
    ASSERT_EQ(kNewYorkTimeZone.createFromDateParts(2021, 3, 13, 0, 0, 0, 0),
              truncateDate(kNewYorkTimeZone.createFromDateParts(2021, 3, 13, 4, 0, 0, 0),
                           TimeUnit::day,
                           1,
                           kNewYorkTimeZone));

    // Day when DST is on.
    ASSERT_EQ(kNewYorkTimeZone.createFromDateParts(2021, 3, 14, 0, 0, 0, 0),
              truncateDate(kNewYorkTimeZone.createFromDateParts(2021, 3, 14, 4, 0, 0, 0),
                           TimeUnit::day,
                           1,
                           kNewYorkTimeZone));

    // The next day when DST is on.
    ASSERT_EQ(kNewYorkTimeZone.createFromDateParts(2021, 3, 15, 0, 0, 0, 0),
              truncateDate(kNewYorkTimeZone.createFromDateParts(2021, 3, 15, 1, 0, 0, 0),
                           TimeUnit::day,
                           1,
                           kNewYorkTimeZone));

    // The last day when DST is on.
    ASSERT_EQ(kNewYorkTimeZone.createFromDateParts(2021, 11, 7, 0, 0, 0, 0),
              truncateDate(kNewYorkTimeZone.createFromDateParts(2021, 11, 7, 0, 30, 0, 0),
                           TimeUnit::day,
                           1,
                           kNewYorkTimeZone));

    // The DST is off.
    ASSERT_EQ(kNewYorkTimeZone.createFromDateParts(2021, 11, 7, 0, 0, 0, 0),
              truncateDate(kNewYorkTimeZone.createFromDateParts(2021, 11, 7, 2, 0, 0, 0),
                           TimeUnit::day,
                           1,
                           kNewYorkTimeZone));

    // The next day DST is off.
    ASSERT_EQ(kNewYorkTimeZone.createFromDateParts(2021, 11, 8, 0, 0, 0, 0),
              truncateDate(kNewYorkTimeZone.createFromDateParts(2021, 11, 8, 2, 0, 0, 0),
                           TimeUnit::day,
                           1,
                           kNewYorkTimeZone));


    // Daylight Savings Time tests with Europe/Sofia timezone.
    // Day before DST is on.
    ASSERT_EQ(kEuropeSofiaTimeZone.createFromDateParts(2021, 3, 27, 0, 0, 0, 0),
              truncateDate(kEuropeSofiaTimeZone.createFromDateParts(2021, 3, 27, 22, 0, 0, 0),
                           TimeUnit::day,
                           1,
                           kEuropeSofiaTimeZone));

    // Day when DST is on.
    ASSERT_EQ(kEuropeSofiaTimeZone.createFromDateParts(2021, 3, 28, 0, 0, 0, 0),
              truncateDate(kEuropeSofiaTimeZone.createFromDateParts(2021, 3, 28, 4, 0, 0, 0),
                           TimeUnit::day,
                           1,
                           kEuropeSofiaTimeZone));

    // The next day when DST is on.
    ASSERT_EQ(kEuropeSofiaTimeZone.createFromDateParts(2021, 3, 29, 0, 0, 0, 0),
              truncateDate(kEuropeSofiaTimeZone.createFromDateParts(2021, 3, 29, 1, 0, 0, 0),
                           TimeUnit::day,
                           1,
                           kEuropeSofiaTimeZone));

    // The last day when DST is on.
    ASSERT_EQ(kEuropeSofiaTimeZone.createFromDateParts(2021, 10, 31, 0, 0, 0, 0),
              truncateDate(kEuropeSofiaTimeZone.createFromDateParts(2021, 10, 31, 0, 30, 0, 0),
                           TimeUnit::day,
                           1,
                           kEuropeSofiaTimeZone));

    // The DST is off.
    ASSERT_EQ(kEuropeSofiaTimeZone.createFromDateParts(2021, 11, 1, 0, 0, 0, 0),
              truncateDate(kEuropeSofiaTimeZone.createFromDateParts(2021, 11, 1, 0, 0, 0, 1),
                           TimeUnit::day,
                           1,
                           kEuropeSofiaTimeZone));
}

// Verifies 'truncateDate()' with TimeUnit::week.
TEST(TruncateDate, Week) {
    for (auto* timezone : kAllTimezones) {
        // Value in the middle of a bin above default reference point. The first day of a week is
        // Wednesday.
        ASSERT_EQ(timezone->createFromDateParts(2000, 1, 19, 0, 0, 0, 0),
                  truncateDate(timezone->createFromDateParts(2000, 1, 26, 0, 0, 0, 0),
                               TimeUnit::week,
                               2,
                               *timezone,
                               DayOfWeek::wednesday));

        // Reference point coincides with start of the week (Saturday).
        ASSERT_EQ(timezone->createFromDateParts(2000, 1, 1, 0, 0, 0, 0),
                  truncateDate(timezone->createFromDateParts(2000, 1, 2, 0, 0, 0, 0),
                               TimeUnit::week,
                               1,
                               *timezone,
                               DayOfWeek::saturday));

        // Value at the lower boundary of a bin. The first day of a week is Monday.
        ASSERT_EQ(timezone->createFromDateParts(2021, 7, 26, 0, 0, 0, 0),
                  truncateDate(timezone->createFromDateParts(2021, 7, 26, 0, 0, 0, 0),
                               TimeUnit::week,
                               1,
                               *timezone,
                               DayOfWeek::monday));

        // Value at the top of a bin. The first day of a week is Monday.
        ASSERT_EQ(timezone->createFromDateParts(2021, 7, 26, 0, 0, 0, 0),
                  truncateDate(timezone->createFromDateParts(2021, 8, 1, 23, 59, 59, 999),
                               TimeUnit::week,
                               1,
                               *timezone,
                               DayOfWeek::monday));

        // Value at the top of a bin below default reference point. The first day of a week is
        // Monday.
        ASSERT_EQ(timezone->createFromDateParts(1999, 12, 6, 0, 0, 0, 0),
                  truncateDate(timezone->createFromDateParts(1999, 12, 19, 23, 59, 59, 999),
                               TimeUnit::week,
                               2,
                               *timezone,
                               DayOfWeek::monday));
    }
}

// Verifies 'truncateDate()' with TimeUnit::month.
TEST(TruncateDate, Month) {
    for (auto* timezone : kAllTimezones) {
        // Value in the middle of a bin above default reference point.
        ASSERT_EQ(timezone->createFromDateParts(2021, 3, 1, 0, 0, 0, 0),
                  truncateDate(timezone->createFromDateParts(2021, 4, 1, 0, 0, 0, 0),
                               TimeUnit::month,
                               2,
                               *timezone));

        // Value at the lower boundary of a bin above default reference point.
        ASSERT_EQ(timezone->createFromDateParts(2021, 3, 1, 0, 0, 0, 0),
                  truncateDate(timezone->createFromDateParts(2021, 3, 1, 0, 0, 0, 0),
                               TimeUnit::month,
                               2,
                               *timezone));

        // Value at the upper boundary of a bin above default reference point.
        ASSERT_EQ(timezone->createFromDateParts(2021, 3, 1, 0, 0, 0, 0),
                  truncateDate(timezone->createFromDateParts(2021, 4, 30, 23, 59, 59, 999),
                               TimeUnit::month,
                               2,
                               *timezone));

        // Value at the lower boundary of the next bin.
        ASSERT_EQ(timezone->createFromDateParts(2021, 5, 1, 0, 0, 0, 0),
                  truncateDate(timezone->createFromDateParts(2021, 5, 1, 0, 0, 0, 0),
                               TimeUnit::month,
                               2,
                               *timezone));

        // Value at the lower boundary of a bin.
        ASSERT_EQ(timezone->createFromDateParts(1900, 1, 1, 0, 0, 0, 0),
                  truncateDate(timezone->createFromDateParts(1900, 1, 1, 0, 0, 0, 0),
                               TimeUnit::month,
                               1,
                               *timezone));

        // Value at the top of a bin.
        ASSERT_EQ(timezone->createFromDateParts(1900, 1, 1, 0, 0, 0, 0),
                  truncateDate(timezone->createFromDateParts(1900, 3, 31, 23, 59, 59, 999),
                               TimeUnit::month,
                               3,
                               *timezone));

        // February is atypical.
        ASSERT_EQ(timezone->createFromDateParts(2005, 2, 1, 0, 0, 0, 0),
                  truncateDate(timezone->createFromDateParts(2005, 2, 28, 23, 59, 59, 999),
                               TimeUnit::month,
                               1,
                               *timezone));
    }
}

// Verifies 'truncateDate()' with TimeUnit::quarter.
TEST(TruncateDate, Quarter) {
    for (auto* timezone : kAllTimezones) {
        // Value in the middle of a bin above default reference point.
        ASSERT_EQ(timezone->createFromDateParts(2021, 1, 1, 0, 0, 0, 0),
                  truncateDate(timezone->createFromDateParts(2021, 5, 1, 0, 0, 0, 0),
                               TimeUnit::quarter,
                               2,
                               *timezone));

        // Value at the lower boundary of a bin above default reference point.
        ASSERT_EQ(timezone->createFromDateParts(2021, 1, 1, 0, 0, 0, 0),
                  truncateDate(timezone->createFromDateParts(2021, 1, 1, 0, 0, 0, 0),
                               TimeUnit::quarter,
                               2,
                               *timezone));

        // Value at the upper boundary of a bin above default reference point.
        ASSERT_EQ(timezone->createFromDateParts(2021, 1, 1, 0, 0, 0, 0),
                  truncateDate(timezone->createFromDateParts(2021, 6, 30, 23, 59, 59, 999),
                               TimeUnit::quarter,
                               2,
                               *timezone));

        // Value at the lower boundary of the next bin.
        ASSERT_EQ(timezone->createFromDateParts(2021, 7, 1, 0, 0, 0, 0),
                  truncateDate(timezone->createFromDateParts(2021, 7, 1, 0, 0, 0, 0),
                               TimeUnit::quarter,
                               2,
                               *timezone));

        // Value at the lower boundary of a bin.
        ASSERT_EQ(timezone->createFromDateParts(1900, 1, 1, 0, 0, 0, 0),
                  truncateDate(timezone->createFromDateParts(1900, 1, 1, 0, 0, 0, 0),
                               TimeUnit::quarter,
                               1,
                               *timezone));

        // Value at the top of a bin.
        ASSERT_EQ(timezone->createFromDateParts(1900, 1, 1, 0, 0, 0, 0),
                  truncateDate(timezone->createFromDateParts(1900, 3, 31, 23, 59, 59, 999),
                               TimeUnit::quarter,
                               1,
                               *timezone));
    }
}

// Verifies 'truncateDate()' with TimeUnit::year.
TEST(TruncateDate, Year) {
    for (auto* timezone : kAllTimezones) {
        // Value in the middle of a bin above default reference point.
        ASSERT_EQ(timezone->createFromDateParts(2020, 1, 1, 0, 0, 0, 0),
                  truncateDate(timezone->createFromDateParts(2021, 5, 1, 0, 0, 0, 0),
                               TimeUnit::year,
                               2,
                               *timezone));

        // Value at the lower boundary of a bin above default reference point.
        ASSERT_EQ(timezone->createFromDateParts(2020, 1, 1, 0, 0, 0, 0),
                  truncateDate(timezone->createFromDateParts(2020, 1, 1, 0, 0, 0, 0),
                               TimeUnit::year,
                               2,
                               *timezone));

        // Value at the upper boundary of a bin above default reference point.
        ASSERT_EQ(timezone->createFromDateParts(2020, 1, 1, 0, 0, 0, 0),
                  truncateDate(timezone->createFromDateParts(2021, 12, 31, 23, 59, 59, 999),
                               TimeUnit::year,
                               2,
                               *timezone));

        // Value at the lower boundary of the next bin.
        ASSERT_EQ(timezone->createFromDateParts(2022, 1, 1, 0, 0, 0, 0),
                  truncateDate(timezone->createFromDateParts(2022, 1, 1, 0, 0, 0, 0),
                               TimeUnit::year,
                               2,
                               *timezone));

        // Value at the lower boundary of a bin.
        ASSERT_EQ(timezone->createFromDateParts(1950, 1, 1, 0, 0, 0, 0),
                  truncateDate(timezone->createFromDateParts(1950, 1, 1, 0, 0, 0, 0),
                               TimeUnit::year,
                               1,
                               *timezone));

        // Value at the top of a bin.
        ASSERT_EQ(timezone->createFromDateParts(1950, 1, 1, 0, 0, 0, 0),
                  truncateDate(timezone->createFromDateParts(1950, 12, 31, 23, 59, 59, 999),
                               TimeUnit::year,
                               1,
                               *timezone));
    }
}

// Verifies 'truncateDate()' error handling.
TEST(TruncateDate, ErrorHandling) {
    auto* timezone = &kDefaultTimeZone;
    const auto anyDate = timezone->createFromDateParts(2000, 1, 1, 2, 0, 0, 0);
    const auto dateBeforeReferencePoint = timezone->createFromDateParts(1980, 1, 1, 0, 0, 0, 0);

    // Verify that binSize > 0 constraint is enforced.
    ASSERT_THROWS_CODE(
        truncateDate(anyDate, TimeUnit::day, 0, *timezone), AssertionException, 5439005);

    // Verify millisecond overflow detection.
    const Date_t dateInLongPast =
        Date_t::fromMillisSinceEpoch(std::numeric_limits<long long>::min());
    ASSERT_THROWS_CODE(truncateDate(dateInLongPast, TimeUnit::millisecond, 1, *timezone),
                       AssertionException,
                       5439000);
    ASSERT_THROWS_CODE(
        truncateDate(Date_t::fromMillisSinceEpoch(std::numeric_limits<long long>::min() / 4 * 3),
                     TimeUnit::millisecond,
                     std::numeric_limits<long long>::max() / 5 * 3,
                     *timezone),
        AssertionException,
        5439001);

    // Verify second/minute/hour overflow detection.
    for (auto timeUnit : {TimeUnit::second, TimeUnit::minute, TimeUnit::hour}) {
        ASSERT_THROWS_CODE(
            truncateDate(anyDate, timeUnit, std::numeric_limits<long long>::max() / 64, *timezone),
            AssertionException,
            5439002);
    }

    // Verify day/week/month/quarter/year bin size limits.
    for (auto timeUnit :
         {TimeUnit::day, TimeUnit::week, TimeUnit::month, TimeUnit::quarter, TimeUnit::year}) {
        ASSERT_THROWS_CODE(truncateDate(anyDate, timeUnit, 100'000'000'001ULL, *timezone),
                           AssertionException,
                           5439006);

        // Verify computation with large bin size when the result is in the long past succeeds.
        truncateDate(dateBeforeReferencePoint, timeUnit, 200'000'000ULL, *timezone);

        // Verify computation with large bin size succeeds.
        ASSERT_EQ(timezone->createFromDateParts(2000, 1, 1, 0, 0, 0, 0),
                  truncateDate(timezone->createFromDateParts(2000, 1, 1, 12, 0, 0, 0),
                               timeUnit,
                               100'000'000'000ULL,
                               *timezone,
                               DayOfWeek::saturday));
    }

    // Verify that out-of-range results are detected.
    ASSERT_THROWS_CODE(
        truncateDate(dateBeforeReferencePoint, TimeUnit::year, 1'000'000'000ULL, *timezone),
        AssertionException,
        5976500);

    // Verify computation with large bin size when the result is in the long past succeeds.
    ASSERT_EQ(timezone->createFromDateParts(-200'000'000LL, 1, 1, 0, 0, 0, 0),
              truncateDate(dateBeforeReferencePoint, TimeUnit::year, 200'002'000ULL, *timezone));
}

TEST(DateAdd, DateAddYear) {
    ASSERT_EQ(dateAdd(kDefaultTimeZone.createFromDateParts(2019, 11, 11, 0, 0, 0, 1),
                      TimeUnit::year,
                      1,
                      kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2020, 11, 11, 0, 0, 0, 1));

    ASSERT_EQ(dateAdd(kDefaultTimeZone.createFromDateParts(2019, 11, 11, 0, 0, 0, 0),
                      TimeUnit::year,
                      -1,
                      kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2018, 11, 11, 0, 0, 0, 0));

    ASSERT_EQ(dateAdd(kDefaultTimeZone.createFromDateParts(2016, 2, 29, 0, 0, 0, 0),
                      TimeUnit::year,
                      2,
                      kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2018, 2, 28, 0, 0, 0, 0));

    ASSERT_EQ(dateAdd(kDefaultTimeZone.createFromDateParts(2016, 2, 29, 0, 0, 0, 0),
                      TimeUnit::year,
                      -4,
                      kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2012, 2, 29, 0, 0, 0, 0));
}

TEST(DateAdd, LargeAmountValues) {
    const auto anyDate = kDefaultTimeZone.createFromDateParts(2016, 1, 1, 0, 0, 0, 0);
    const auto smallDate = kDefaultTimeZone.createFromDateParts(-291'000'000, 3, 31, 0, 0, 0, 0);
    struct TestCase {
        TimeUnit unit;
        long long invalidAmount;   // Amount value rejected in initial validation.
        long long overflowAmount;  // Amount to add to date -200'000'000-2-29 00:00:00.000 so the
                                   // result cannot be represented as Date_t.
        long long largeAmount;     // Large amount to add to 'smallDate' so the result is equal to
                                   // 'largeAmountExpectedDate'.
        Date_t largeAmountExpectedDate;
    };
    const auto maxValidYearAmountPlus1{584'942'417LL + 1};
    const auto maxValidDayAmountPlus1{213'503'982'334LL + 1};
    const std::vector<TestCase> testCases{
        {TimeUnit::year,
         maxValidYearAmountPlus1,      // Invalid amount.
         maxValidYearAmountPlus1 - 1,  // Overflow amount.
         550'000'000LL,                // Large amount.
         kDefaultTimeZone.createFromDateParts(-291'000'000 + 550'000'000, 3, 31, 0, 0, 0, 0)},
        {TimeUnit::quarter,
         maxValidYearAmountPlus1 * 4,      // Invalid amount.
         maxValidYearAmountPlus1 * 4 - 1,  // Overflow amount.
         550'000'000LL * 4,                // Large amount.
         kDefaultTimeZone.createFromDateParts(-291'000'000 + 550'000'000, 3, 31, 0, 0, 0, 0)},
        {TimeUnit::month,
         maxValidYearAmountPlus1 * 12,      // Invalid amount.
         maxValidYearAmountPlus1 * 12 - 1,  // Overflow amount.
         550'000'000LL * 12,                // Large amount.
         kDefaultTimeZone.createFromDateParts(-291'000'000 + 550'000'000, 3, 31, 0, 0, 0, 0)},
        {TimeUnit::day,
         maxValidDayAmountPlus1,      // Invalid amount.
         maxValidDayAmountPlus1 - 1,  // Overflow amount.
         250'000'000LL * 365,         // Large amount.
         smallDate + Days(250'000'000LL * 365)},
        {TimeUnit::hour,
         maxValidDayAmountPlus1 * 24,      // Invalid amount.
         maxValidDayAmountPlus1 * 24 - 1,  // Overflow amount.
         250'000'000LL * 365 * 24,         // Large amount.
         smallDate + Days(250'000'000LL * 365)},
        {TimeUnit::minute,
         maxValidDayAmountPlus1 * 24 * 60,      // Invalid amount.
         maxValidDayAmountPlus1 * 24 * 60 - 1,  // Overflow amount.
         250'000'000LL * 365 * 24 * 60,         // Large amount.
         smallDate + Days(250'000'000LL * 365)},
        {TimeUnit::second,
         maxValidDayAmountPlus1 * 24 * 60 * 60,      // Invalid amount.
         maxValidDayAmountPlus1 * 24 * 60 * 60 - 1,  // Overflow amount.
         250'000'000LL * 365 * 24 * 60 * 60,         // Large amount.
         smallDate + Days(250'000'000LL * 365)},
    };
    int testCaseIdx{0};
    for (auto&& testCase : testCases) {
        // Verify that out-of-range amount values are rejected.
        ASSERT_THROWS_CODE(
            dateAdd(anyDate, testCase.unit, testCase.invalidAmount, kDefaultTimeZone),
            AssertionException,
            5976500)
            << " test case# " << testCaseIdx;
        ASSERT_THROWS_CODE(
            dateAdd(anyDate, testCase.unit, -testCase.invalidAmount, kDefaultTimeZone),
            AssertionException,
            5976500)
            << " test case# " << testCaseIdx;

        // Verify that overflow is detected when the result cannot be represented as Date_t.
        ASSERT_THROWS_CODE(
            dateAdd(kDefaultTimeZone.createFromDateParts(-200'000'000, 2, 29, 0, 0, 0, 0),
                    testCase.unit,
                    testCase.overflowAmount,
                    kDefaultTimeZone),
            AssertionException,
            5166406)
            << " test case# " << testCaseIdx;

        // Verify that adding large values works correctly.
        ASSERT_EQ(dateAdd(smallDate, testCase.unit, testCase.largeAmount, kDefaultTimeZone),
                  testCase.largeAmountExpectedDate)
            << " test case# " << testCaseIdx;

        ++testCaseIdx;
    }
}

TEST(DateAdd, DateAddQuarter) {
    auto startDate = kDefaultTimeZone.createFromDateParts(2020, 1, 1, 0, 0, 0, 3);
    ASSERT_EQ(dateAdd(startDate, TimeUnit::quarter, 1, kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2020, 4, 1, 0, 0, 0, 3));

    ASSERT_EQ(dateAdd(startDate, TimeUnit::quarter, -5, kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2018, 10, 1, 0, 0, 0, 3));

    ASSERT_EQ(dateAdd(kDefaultTimeZone.createFromDateParts(2020, 1, 31, 0, 0, 0, 0),
                      TimeUnit::quarter,
                      1,
                      kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2020, 4, 30, 0, 0, 0, 0));

    ASSERT_EQ(dateAdd(kDefaultTimeZone.createFromDateParts(2020, 1, 31, 0, 0, 0, 0),
                      TimeUnit::quarter,
                      -3,
                      kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2019, 4, 30, 0, 0, 0, 0));
}

TEST(DateAdd, DateAddMonth) {
    auto utc2020_08_31 = kDefaultTimeZone.createFromDateParts(2020, 8, 31, 10, 5, 0, 5);
    auto utc2021_04_30 = kDefaultTimeZone.createFromDateParts(2021, 4, 30, 0, 0, 0, 0);
    auto utc2021_03_01 = kDefaultTimeZone.createFromDateParts(2021, 3, 1, 0, 0, 0, 0);

    ASSERT_EQ(dateAdd(utc2020_08_31, TimeUnit::month, 1, kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2020, 9, 30, 10, 5, 0, 5));

    ASSERT_EQ(dateAdd(utc2020_08_31, TimeUnit::month, 5, kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2021, 1, 31, 10, 5, 0, 5));

    ASSERT_EQ(dateAdd(utc2020_08_31, TimeUnit::month, 6, kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2021, 2, 28, 10, 5, 0, 5));

    ASSERT_EQ(dateAdd(utc2020_08_31, TimeUnit::month, -1, kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2020, 7, 31, 10, 5, 0, 5));

    ASSERT_EQ(dateAdd(utc2020_08_31, TimeUnit::month, -4, kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2020, 4, 30, 10, 5, 0, 5));

    ASSERT_EQ(dateAdd(utc2020_08_31, TimeUnit::month, -6, kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2020, 2, 29, 10, 5, 0, 5));

    ASSERT_EQ(dateAdd(utc2020_08_31, TimeUnit::month, -18, kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2019, 2, 28, 10, 5, 0, 5));

    ASSERT_EQ(dateAdd(utc2021_04_30, TimeUnit::month, 1, kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2021, 5, 30, 0, 0, 0, 0));

    ASSERT_EQ(dateAdd(utc2021_04_30, TimeUnit::month, -2, kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2021, 2, 28, 0, 0, 0, 0));

    ASSERT_EQ(dateAdd(utc2021_03_01, TimeUnit::month, 2, kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2021, 5, 1, 0, 0, 0, 0));

    ASSERT_EQ(dateAdd(utc2021_03_01, TimeUnit::month, -1, kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2021, 2, 1, 0, 0, 0, 0));
}

/**
 * Tests for day adjustment in a timezone with units of month, quarter or year.
 */
TEST(DateAdd, DateAddDayAdjustmentWithTimezone) {
    struct TestCase {
        Date_t startDate;
        TimeUnit unit;
        long long amount;
        Date_t expectedDate;
    };

    auto europeAmsterdamZone = kDefaultTimeZoneDatabase.getTimeZone("Europe/Amsterdam");
    // Last day of month in Amsterdam zone 2021-05-31T00:30:00.
    auto ams2021_05_31T00_30 = europeAmsterdamZone.createFromDateParts(2021, 05, 31, 0, 30, 0, 4);
    // First day in Amsterdam zone, last day in UTC.
    auto ams2021_02_01 = europeAmsterdamZone.createFromDateParts(2021, 2, 1, 0, 0, 0, 0);

    const std::vector<TestCase> testsAmsterdamZone{
        {ams2021_05_31T00_30,
         TimeUnit::month,
         1,
         europeAmsterdamZone.createFromDateParts(2021, 06, 30, 0, 30, 0, 4)},
        {ams2021_05_31T00_30,
         TimeUnit::month,
         -1,
         europeAmsterdamZone.createFromDateParts(2021, 04, 30, 0, 30, 0, 4)},
        {ams2021_05_31T00_30,
         TimeUnit::quarter,
         -1,
         europeAmsterdamZone.createFromDateParts(2021, 02, 28, 0, 30, 0, 4)},
        {ams2021_05_31T00_30,
         TimeUnit::quarter,
         -5,
         europeAmsterdamZone.createFromDateParts(2020, 02, 29, 0, 30, 0, 4)},
        {ams2021_02_01,
         TimeUnit::month,
         1,
         europeAmsterdamZone.createFromDateParts(2021, 3, 1, 0, 0, 0, 0)},
        {ams2021_02_01,
         TimeUnit::month,
         -2,
         europeAmsterdamZone.createFromDateParts(2020, 12, 1, 0, 0, 0, 0)},
        {ams2021_02_01,
         TimeUnit::quarter,
         1,
         europeAmsterdamZone.createFromDateParts(2021, 5, 1, 0, 0, 0, 0)},
    };
    for (const auto& test : testsAmsterdamZone) {
        const auto testNumber = &test - &testsAmsterdamZone.front();
        ASSERT_EQ(dateAdd(test.startDate, test.unit, test.amount, europeAmsterdamZone),
                  test.expectedDate)
            << "on test " << testNumber;
    }

    auto europeSofiaZone = kDefaultTimeZoneDatabase.getTimeZone("Europe/Sofia");
    // First day in Sofia timezone, last day of month in UTC.
    auto sofia2021_02_01 = europeSofiaZone.createFromDateParts(2021, 2, 1, 0, 0, 0, 5);
    auto sofia2021_03_01 = europeSofiaZone.createFromDateParts(2021, 3, 1, 0, 0, 0, 0);

    const std::vector<TestCase> testsSofiaZone{
        {sofia2021_02_01,
         TimeUnit::month,
         1,
         europeSofiaZone.createFromDateParts(2021, 3, 1, 0, 0, 0, 5)},
        {sofia2021_02_01,
         TimeUnit::month,
         -2,
         europeSofiaZone.createFromDateParts(2020, 12, 1, 0, 0, 0, 5)},
        {sofia2021_02_01,
         TimeUnit::quarter,
         1,
         europeSofiaZone.createFromDateParts(2021, 5, 1, 0, 0, 0, 5)},
        {sofia2021_03_01,
         TimeUnit::month,
         1,
         europeSofiaZone.createFromDateParts(2021, 4, 1, 0, 0, 0, 0)},
        {sofia2021_03_01,
         TimeUnit::month,
         -1,
         europeSofiaZone.createFromDateParts(2021, 2, 1, 0, 0, 0, 0)},
        {sofia2021_03_01,
         TimeUnit::quarter,
         1,
         europeSofiaZone.createFromDateParts(2021, 6, 1, 0, 0, 0, 0)}};

    for (const auto& test : testsSofiaZone) {
        const auto testNumber = &test - &testsSofiaZone.front();
        ASSERT_EQ(dateAdd(test.startDate, test.unit, test.amount, europeSofiaZone),
                  test.expectedDate)
            << "on test " << testNumber;
    }

    auto newYorkZone = kDefaultTimeZoneDatabase.getTimeZone("America/New_York");
    auto ny2020_12_30T22 = kDefaultTimeZone.createFromDateParts(2020, 12, 31, 3, 0, 0, 0);
    auto ny2021_04_30T20 = newYorkZone.createFromDateParts(2021, 4, 30, 20, 0, 0, 6);
    auto ny2021_03_01 = newYorkZone.createFromDateParts(2021, 3, 1, 0, 0, 0, 0);

    const std::vector<TestCase> testsNYZone{
        {ny2020_12_30T22,
         TimeUnit::month,
         -1,
         kDefaultTimeZone.createFromDateParts(
             2020, 12, 1, 3, 0, 0, 0)},  // 2020-11-30T22:00:00 NY zone
        {ny2020_12_30T22,
         TimeUnit::month,
         2,
         kDefaultTimeZone.createFromDateParts(
             2021, 3, 1, 3, 0, 0, 0)},  // 2021-02-28T22:00:00 NY zone
        {ny2020_12_30T22,
         TimeUnit::quarter,
         -2,
         kDefaultTimeZone.createFromDateParts(
             2020, 7, 1, 2, 0, 0, 0)},  // 2020-06-30T22:00:00 NY zone
        {ny2020_12_30T22,
         TimeUnit::month,
         2,
         kDefaultTimeZone.createFromDateParts(
             2021, 3, 1, 3, 0, 0, 0)},  // 2021-02-28T22:00:00 NY zone
        {ny2021_04_30T20,
         TimeUnit::month,
         1,
         newYorkZone.createFromDateParts(2021, 5, 30, 20, 0, 0, 6)},
        {ny2021_04_30T20,
         TimeUnit::month,
         -2,
         newYorkZone.createFromDateParts(2021, 2, 28, 20, 0, 0, 6)},
        {ny2021_03_01, TimeUnit::month, 1, newYorkZone.createFromDateParts(2021, 4, 1, 0, 0, 0, 0)},
        {ny2021_03_01,
         TimeUnit::month,
         -1,
         newYorkZone.createFromDateParts(2021, 2, 1, 0, 0, 0, 0)},
        {ny2021_03_01,
         TimeUnit::quarter,
         1,
         newYorkZone.createFromDateParts(2021, 6, 1, 0, 0, 0, 0)},
    };

    for (const auto& test : testsNYZone) {
        const auto testNumber = &test - &testsNYZone.front();
        ASSERT_EQ(dateAdd(test.startDate, test.unit, test.amount, newYorkZone), test.expectedDate)
            << "on test " << testNumber;
    }

    auto australiaEuclaZone = kDefaultTimeZoneDatabase.getTimeZone("Australia/Eucla");
    auto eucla2021_02_01 = australiaEuclaZone.createFromDateParts(2021, 2, 1, 0, 0, 0, 7);
    auto eucla2021_04_30 = australiaEuclaZone.createFromDateParts(2021, 4, 30, 0, 0, 0, 0);
    const std::vector<TestCase> testsEuclaZone{
        {eucla2021_02_01,
         TimeUnit::month,
         1,
         australiaEuclaZone.createFromDateParts(2021, 3, 1, 0, 0, 0, 7)},
        {eucla2021_02_01,
         TimeUnit::month,
         -2,
         australiaEuclaZone.createFromDateParts(2020, 12, 1, 0, 0, 0, 7)},
        {eucla2021_02_01,
         TimeUnit::quarter,
         1,
         australiaEuclaZone.createFromDateParts(2021, 5, 1, 0, 0, 0, 7)},
        {eucla2021_04_30,
         TimeUnit::month,
         1,
         australiaEuclaZone.createFromDateParts(2021, 5, 30, 0, 0, 0, 0)},
        {eucla2021_04_30,
         TimeUnit::month,
         -2,
         australiaEuclaZone.createFromDateParts(2021, 2, 28, 0, 0, 0, 0)},
        {eucla2021_04_30,
         TimeUnit::quarter,
         1,
         australiaEuclaZone.createFromDateParts(2021, 7, 30, 0, 0, 0, 0)}};

    for (const auto& test : testsEuclaZone) {
        const auto testNumber = &test - &testsEuclaZone.front();
        ASSERT_EQ(dateAdd(test.startDate, test.unit, test.amount, australiaEuclaZone),
                  test.expectedDate)
            << "on test " << testNumber;
    }
}

TEST(DateAdd, DateAddDay) {
    auto startDate = kDefaultTimeZone.createFromDateParts(2020, 8, 31, 10, 5, 0, 7);
    ASSERT_EQ(dateAdd(startDate, TimeUnit::day, 1, kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2020, 9, 1, 10, 5, 0, 7));

    ASSERT_EQ(dateAdd(startDate, TimeUnit::day, -1, kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2020, 8, 30, 10, 5, 0, 7));

    ASSERT_EQ(dateAdd(startDate, TimeUnit::day, -366, kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2019, 8, 31, 10, 5, 0, 7));
}

TEST(DateAdd, DateAddHour) {
    struct TestCase {
        Date_t startDate;
        long long amount;
        Date_t expectedDate;
    };

    auto utcStartDate = kDefaultTimeZone.createFromDateParts(2020, 8, 31, 10, 5, 0, 9);

    std::vector<TestCase> tests{
        {utcStartDate, 1, kDefaultTimeZone.createFromDateParts(2020, 8, 31, 11, 5, 0, 9)},
        {utcStartDate, -1, kDefaultTimeZone.createFromDateParts(2020, 8, 31, 9, 5, 0, 9)},
        {utcStartDate, 168, kDefaultTimeZone.createFromDateParts(2020, 9, 7, 10, 5, 0, 9)},
    };

    for (auto&& test : tests) {
        ASSERT_EQ(dateAdd(test.startDate, TimeUnit::hour, test.amount, kDefaultTimeZone),
                  test.expectedDate);
    }
}

TEST(DateAdd, DateAddMinute) {
    auto startDate = kDefaultTimeZone.createFromDateParts(2020, 12, 31, 23, 55, 15, 8);
    ASSERT_EQ(dateAdd(startDate, TimeUnit::minute, 1, kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2020, 12, 31, 23, 56, 15, 8));

    ASSERT_EQ(dateAdd(startDate, TimeUnit::minute, -1, kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2020, 12, 31, 23, 54, 15, 8));

    ASSERT_EQ(dateAdd(startDate, TimeUnit::minute, 10, kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2021, 1, 1, 0, 5, 15, 8));

    ASSERT_EQ(dateAdd(startDate, TimeUnit::minute, -1440, kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2020, 12, 30, 23, 55, 15, 8));
}

/**
 * Tests for dateAdd operation in a timezone different from UTC.
 */
TEST(DateAdd, DateAddWithTimezoneDST) {
    struct TestCase {
        Date_t startDate;
        TimeUnit unit;
        long long amount;
        Date_t expectedDate;
    };

    auto europeAmsterdamZone = kDefaultTimeZoneDatabase.getTimeZone("Europe/Amsterdam");
    // The test uses the DST change in Amsterdam zone that happens at 2020-10-25T01:00:00 UTC.
    // Pick two time instants in UTC just before and after the DST change.
    auto startDateBeforeDSTChange = kDefaultTimeZone.createFromDateParts(2020, 10, 25, 0, 50, 5, 0);
    auto startDateAfterDSTChange = kDefaultTimeZone.createFromDateParts(2020, 10, 25, 1, 10, 5, 0);

    std::vector<TestCase> tests{
        // DST to Standard change in Amsterdam zone that happens at 2020-10-25T01:00:00 UTC.
        // 2020-10-25T03:00:00 -> 2020-10-25T02:00:00 in Amsterdam.
        {startDateBeforeDSTChange,
         TimeUnit::minute,
         10,
         kDefaultTimeZone.createFromDateParts(2020, 10, 25, 1, 0, 5, 0)},
        {startDateBeforeDSTChange,
         TimeUnit::hour,
         1,
         kDefaultTimeZone.createFromDateParts(2020, 10, 25, 1, 50, 5, 0)},
        {startDateAfterDSTChange,
         TimeUnit::minute,
         -20,
         kDefaultTimeZone.createFromDateParts(2020, 10, 25, 0, 50, 5, 0)},
        {startDateAfterDSTChange,
         TimeUnit::hour,
         -1,
         kDefaultTimeZone.createFromDateParts(2020, 10, 25, 0, 10, 5, 0)},

        // For units of day or larger, compute DST correction to produce the same local time in
        // Amsterdam timezone.
        {europeAmsterdamZone.createFromDateParts(2020, 10, 25, 1, 50, 0, 0),
         TimeUnit::day,
         1,
         europeAmsterdamZone.createFromDateParts(2020, 10, 26, 1, 50, 0, 0)},
        {europeAmsterdamZone.createFromDateParts(2020, 10, 25, 2, 30, 0, 0),
         TimeUnit::day,
         1,
         europeAmsterdamZone.createFromDateParts(2020, 10, 26, 2, 30, 0, 0)},
        {europeAmsterdamZone.createFromDateParts(2020, 10, 24, 2, 0, 1, 0),
         TimeUnit::day,
         1,
         europeAmsterdamZone.createFromDateParts(2020, 10, 25, 1, 59, 59, 0) +
             Milliseconds{2000}},  // as this date is ambiguous (it could in both timezones, with or
                                   // without DST) and the computation is expected to return the
                                   // "with DST" one, obtain it via a computation
        {europeAmsterdamZone.createFromDateParts(2020, 10, 24, 3, 0, 1, 0),
         TimeUnit::day,
         1,
         kDefaultTimeZone.createFromDateParts(2020, 10, 25, 2, 0, 1, 0)},
        {europeAmsterdamZone.createFromDateParts(2020, 10, 24, 3, 0, 1, 0),
         TimeUnit::day,
         1,
         // The same as above in Amsterdam zone.
         europeAmsterdamZone.createFromDateParts(2020, 10, 25, 3, 0, 1, 0)},
        {europeAmsterdamZone.createFromDateParts(2020, 10, 25, 3, 30, 0, 0),
         TimeUnit::day,
         -1,
         europeAmsterdamZone.createFromDateParts(2020, 10, 24, 3, 30, 0, 0)},
        {europeAmsterdamZone.createFromDateParts(2020, 10, 25, 2, 30, 0, 0),
         TimeUnit::day,
         -1,
         europeAmsterdamZone.createFromDateParts(2020, 10, 24, 2, 30, 0, 0)},
        {europeAmsterdamZone.createFromDateParts(2020, 10, 26, 2, 30, 0, 0),
         TimeUnit::day,
         -1,
         europeAmsterdamZone.createFromDateParts(2020, 10, 25, 2, 30, 0, 0)},

        // Standard to DST change at 2021-03-28T02:00:00 -> 2021-03-28T03:00:00 Amsterdam timezone.
        {europeAmsterdamZone.createFromDateParts(2021, 3, 28, 1, 20, 0, 0),
         TimeUnit::day,
         1,
         europeAmsterdamZone.createFromDateParts(2021, 3, 29, 1, 20, 0, 0)},
        {europeAmsterdamZone.createFromDateParts(2021, 3, 27, 2, 20, 0, 0),
         TimeUnit::day,
         1,
         // Computed time falls into the missing hour: forward the clock 1 hour.
         europeAmsterdamZone.createFromDateParts(2021, 3, 28, 3, 20, 0, 0)},
        {europeAmsterdamZone.createFromDateParts(2021, 3, 28, 3, 20, 0, 0),
         TimeUnit::day,
         -1,
         europeAmsterdamZone.createFromDateParts(2021, 3, 27, 3, 20, 0, 0)},
        {europeAmsterdamZone.createFromDateParts(2021, 3, 29, 2, 20, 0, 0),
         TimeUnit::day,
         -1,
         // Computed time falls into the missing hour: forward the clock 1 hour.
         europeAmsterdamZone.createFromDateParts(2021, 3, 28, 3, 20, 0, 0)},
        {europeAmsterdamZone.createFromDateParts(2020, 11, 28, 2, 20, 0, 0),
         TimeUnit::month,
         4,
         // Computed time falls into the missing hour: forward the clock 1 hour.
         europeAmsterdamZone.createFromDateParts(2021, 3, 28, 3, 20, 0, 0)},
    };

    for (auto&& test : tests) {
        const auto testNumber = &test - &tests.front();
        ASSERT_EQ(dateAdd(test.startDate, test.unit, test.amount, europeAmsterdamZone),
                  test.expectedDate)
            << " on test " << testNumber << " in Amsterdam timezone.";
    }

    auto newYorkZone = kDefaultTimeZoneDatabase.getTimeZone("America/New_York");
    // The following test uses the DST change in New York at 2020-03-08T07:00:00Z.
    // Pick two time instants in UTC just before and after the DST change.
    auto startDateInEST = kDefaultTimeZone.createFromDateParts(2020, 3, 8, 6, 50, 0, 0);
    auto startDateInEDT = kDefaultTimeZone.createFromDateParts(2020, 3, 8, 7, 20, 0, 0);

    std::vector<TestCase> testsNYZone{
        {startDateInEST,
         TimeUnit::minute,
         15,
         kDefaultTimeZone.createFromDateParts(2020, 3, 8, 7, 5, 0, 0)},
        {startDateInEST,
         TimeUnit::hour,
         1,
         kDefaultTimeZone.createFromDateParts(2020, 3, 8, 7, 50, 0, 0)},
        {startDateInEDT,
         TimeUnit::minute,
         -25,
         kDefaultTimeZone.createFromDateParts(2020, 3, 8, 6, 55, 0, 0)},
        {startDateInEDT,
         TimeUnit::hour,
         -1,
         kDefaultTimeZone.createFromDateParts(2020, 3, 8, 6, 20, 0, 0)},
        // For units of day or larger, compute DST correction to produce the same local time in New
        // York timezone.

        // DST to Standard change in New York zone at 2020-11-01T06:00:00 UTC,
        // 2020-11-01T02:00:00 -> 2020-11-01T01:00:00 New York.
        {newYorkZone.createFromDateParts(2020, 11, 1, 0, 30, 0, 0),
         TimeUnit::day,
         1,
         newYorkZone.createFromDateParts(2020, 11, 2, 0, 30, 0, 0)},
        {newYorkZone.createFromDateParts(2020, 11, 1, 1, 30, 0, 0),
         TimeUnit::day,
         1,
         newYorkZone.createFromDateParts(2020, 11, 2, 1, 30, 0, 0)},
        {newYorkZone.createFromDateParts(2020, 10, 31, 1, 0, 1, 0),
         TimeUnit::day,
         1,
         newYorkZone.createFromDateParts(2020, 11, 1, 0, 59, 59, 0) +
             Milliseconds{2000}},  // as this date is ambiguous (it could in both timezones, with or
                                   // without DST) and the computation is expected to return the
                                   // "with DST" one, obtain it via a computation
        {newYorkZone.createFromDateParts(2020, 11, 1, 3, 0, 0, 0),
         TimeUnit::day,
         -1,
         newYorkZone.createFromDateParts(2020, 10, 31, 3, 0, 0, 0)},
        {newYorkZone.createFromDateParts(2020, 11, 1, 1, 30, 0, 0),
         TimeUnit::day,
         -1,
         newYorkZone.createFromDateParts(2020, 10, 31, 1, 30, 0, 0)},
        {newYorkZone.createFromDateParts(2020, 11, 2, 1, 30, 0, 0),
         TimeUnit::day,
         -1,
         newYorkZone.createFromDateParts(2020, 11, 1, 1, 30, 0, 0)},

        // Standard to DST change: 2021-03-14T02:00:00 -> 2021-03-14T03:00:00 New York timezone.
        {newYorkZone.createFromDateParts(2021, 3, 14, 1, 30, 0, 0),
         TimeUnit::day,
         1,
         newYorkZone.createFromDateParts(2021, 3, 15, 1, 30, 0, 0)},
        {newYorkZone.createFromDateParts(2021, 3, 13, 2, 20, 0, 0),
         TimeUnit::day,
         1,
         // Computed time falls into the missing hour: forward the clock 1 hour.
         newYorkZone.createFromDateParts(2021, 3, 14, 3, 20, 0, 0)},
        {newYorkZone.createFromDateParts(2021, 3, 14, 3, 20, 0, 0),
         TimeUnit::day,
         -1,
         newYorkZone.createFromDateParts(2021, 3, 13, 3, 20, 0, 0)},
        {newYorkZone.createFromDateParts(2021, 3, 15, 2, 20, 0, 0),
         TimeUnit::day,
         -1,
         // Computed time falls into the missing hour: forward the clock 1 hour.
         newYorkZone.createFromDateParts(2021, 3, 14, 3, 20, 0, 0)},
        {newYorkZone.createFromDateParts(2021, 1, 14, 2, 20, 0, 0),
         TimeUnit::month,
         2,
         // Computed time falls into the missing hour: forward the clock 1 hour.
         newYorkZone.createFromDateParts(2021, 3, 14, 3, 20, 0, 0)},
    };

    for (auto&& test : testsNYZone) {
        const auto testNumber = &test - &testsNYZone.front();
        ASSERT_EQ(dateAdd(test.startDate, test.unit, test.amount, newYorkZone), test.expectedDate)
            << " on test " << testNumber << " in New York timezone.";
    }
}

/**
 * Tests for dateAdd operation in Lord Howe timezone with 00:30 DST change.
 */
TEST(DateAdd, DateAdd_LordHoweTimezoneDST) {
    struct TestCase {
        Date_t startDate;
        TimeUnit unit;
        long long amount;
        Date_t expectedDate;
    };

    auto australiaLordHoweZone = kDefaultTimeZoneDatabase.getTimeZone("Australia/Lord_Howe");
    std::vector<TestCase> tests{
        // DST to Standard change: 2021-04-04T02:00:00 -> 2021-04-04T01:30:00 Lord Howe timezone.
        {australiaLordHoweZone.createFromDateParts(2021, 4, 4, 1, 29, 59, 0) +
             Milliseconds{1000},  // as this date is ambiguous (it could in both timezones, with or
                                  // without DST) and the computation is expected to start from the
                                  // "with DST" one, obtain it via a computation
         TimeUnit::day,
         1,
         australiaLordHoweZone.createFromDateParts(2021, 4, 5, 1, 30, 0, 0)},
        {australiaLordHoweZone.createFromDateParts(2021, 4, 3, 1, 30, 1, 0),
         TimeUnit::day,
         1,
         australiaLordHoweZone.createFromDateParts(2021, 4, 4, 1, 29, 59, 0) +
             Milliseconds{2000}},  // as this date is ambiguous (it could in both timezones, with or
                                   // without DST) and the computation is expected to return the
                                   // "with DST" one, obtain it via a computation
        {australiaLordHoweZone.createFromDateParts(2021, 4, 5, 1, 0, 0, 0),
         TimeUnit::day,
         -1,
         australiaLordHoweZone.createFromDateParts(2021, 4, 4, 1, 0, 0, 0)},
        {australiaLordHoweZone.createFromDateParts(2021, 3, 4, 5, 30, 0, 5),
         TimeUnit::month,
         1,
         australiaLordHoweZone.createFromDateParts(2021, 4, 4, 5, 30, 0, 5)},

        // DST to Standard change: 2021-04-03T15:00:00 UTC time.
        {kDefaultTimeZone.createFromDateParts(2021, 4, 3, 14, 50, 5, 0),
         TimeUnit::minute,
         20,
         kDefaultTimeZone.createFromDateParts(2021, 4, 3, 15, 10, 5, 0)},
        {kDefaultTimeZone.createFromDateParts(2021, 4, 3, 14, 50, 5, 0),
         // = 2021-04-04T01:50:00 Australia/Lord_Howe, DST offset +11:00.
         TimeUnit::minute,
         20,
         // Same as above, the result date falls into repeated 1/2 hour, reported with standard time
         // offset of +10:30.
         australiaLordHoweZone.createFromDateParts(2021, 4, 4, 1, 40, 5, 0)},
        {kDefaultTimeZone.createFromDateParts(2021, 4, 3, 14, 50, 5, 0),
         TimeUnit::hour,
         1,
         kDefaultTimeZone.createFromDateParts(2021, 4, 3, 15, 50, 5, 0)},
        {kDefaultTimeZone.createFromDateParts(2021, 4, 3, 14, 50, 5, 0),
         // = 2021-04-04T01:50:00 Australia/Lord_Howe.
         TimeUnit::hour,
         1,
         // Same as above, the result local time has offset of +10:30 - Standard time.
         australiaLordHoweZone.createFromDateParts(2021, 4, 4, 2, 20, 5, 0)},

        // Standard to DST change: 2020-10-04T02:00:00 -> 2020-10-04T02:30:00 Lord Howe timezone.
        {australiaLordHoweZone.createFromDateParts(2020, 10, 4, 1, 30, 0, 0),
         TimeUnit::day,
         1,
         australiaLordHoweZone.createFromDateParts(2020, 10, 5, 1, 30, 0, 0)},
        {australiaLordHoweZone.createFromDateParts(2020, 10, 3, 2, 15, 0, 0),
         TimeUnit::day,
         1,
         // Computed time falls into the missing 1/2 hour: move the clock forward by 30 min.
         australiaLordHoweZone.createFromDateParts(2020, 10, 4, 2, 45, 0, 0)},
        {australiaLordHoweZone.createFromDateParts(2020, 10, 5, 1, 0, 0, 8),
         TimeUnit::day,
         -1,
         australiaLordHoweZone.createFromDateParts(2020, 10, 4, 1, 0, 0, 8)},
        {australiaLordHoweZone.createFromDateParts(2021, 1, 3, 18, 25, 0, 0),
         TimeUnit::month,
         -3,
         australiaLordHoweZone.createFromDateParts(2020, 10, 3, 18, 25, 0, 0)},

        // Standard to DST change: 2020-10-03T15:30:00 UTC time.
        {kDefaultTimeZone.createFromDateParts(2020, 10, 3, 15, 20, 5, 0),
         TimeUnit::minute,
         20,
         kDefaultTimeZone.createFromDateParts(2020, 10, 3, 15, 40, 5, 0)},
        {kDefaultTimeZone.createFromDateParts(2020, 10, 3, 15, 20, 5, 0),
         TimeUnit::hour,
         1,
         kDefaultTimeZone.createFromDateParts(2020, 10, 3, 16, 20, 5, 0)},
        {kDefaultTimeZone.createFromDateParts(2020, 10, 3, 15, 30, 20, 0),
         TimeUnit::second,
         -50,
         kDefaultTimeZone.createFromDateParts(2020, 10, 3, 15, 29, 30, 0)},
    };

    for (auto&& test : tests) {
        const auto testNumber = &test - &tests.front();
        ASSERT_EQ(dateAdd(test.startDate, test.unit, test.amount, australiaLordHoweZone),
                  test.expectedDate)
            << " on test " << testNumber << " in Australia/Lord_Howe timezone.";
    }
}

/**
 * Tests for dateAdd operation in a timezone with fixed offset.
 */
TEST(DateAdd, DateAdd_offsetTimezone) {
    struct TestCase {
        Date_t startDate;
        TimeUnit unit;
        long long amount;
        Date_t expectedDate;
    };

    auto plus2TimeZone = kDefaultTimeZoneDatabase.getTimeZone("+02:00");
    std::vector<TestCase> tests{
        {plus2TimeZone.createFromDateParts(2021, 3, 1, 1, 30, 0, 0),
         TimeUnit::month,
         1,
         plus2TimeZone.createFromDateParts(2021, 4, 1, 1, 30, 0, 0)},
        {plus2TimeZone.createFromDateParts(2021, 3, 31, 1, 30, 0, 0),
         TimeUnit::month,
         1,
         plus2TimeZone.createFromDateParts(2021, 4, 30, 1, 30, 0, 0)},
        {plus2TimeZone.createFromDateParts(2021, 3, 28, 1, 30, 0, 0),
         TimeUnit::day,
         1,
         plus2TimeZone.createFromDateParts(2021, 3, 29, 1, 30, 0, 0)},
        {plus2TimeZone.createFromDateParts(2021, 3, 28, 1, 30, 0, 0),
         TimeUnit::hour,
         1,
         plus2TimeZone.createFromDateParts(2021, 3, 28, 2, 30, 0, 0)},
    };

    for (auto&& test : tests) {
        const auto testNumber = &test - &tests.front();
        ASSERT_EQ(dateAdd(test.startDate, test.unit, test.amount, plus2TimeZone), test.expectedDate)
            << " on test " << testNumber << " in timezone +02:00.";
    }
}

TEST(DateAdd, DateAddSecond) {
    auto startDate = kDefaultTimeZone.createFromDateParts(2020, 12, 31, 23, 55, 15, 2);
    ASSERT_EQ(dateAdd(startDate, TimeUnit::second, 1, kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2020, 12, 31, 23, 55, 16, 2));

    ASSERT_EQ(dateAdd(startDate, TimeUnit::second, -1, kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2020, 12, 31, 23, 55, 14, 2));

    ASSERT_EQ(dateAdd(startDate, TimeUnit::second, 300, kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2021, 1, 1, 0, 0, 15, 2));

    ASSERT_EQ(dateAdd(startDate, TimeUnit::second, -195, kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2020, 12, 31, 23, 52, 0, 2));
}

TEST(DateAdd, DateAddMillisecond) {
    auto startDate = kDefaultTimeZone.createFromDateParts(2020, 12, 31, 23, 59, 15, 1);
    ASSERT_EQ(dateAdd(startDate, TimeUnit::millisecond, 1, kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2020, 12, 31, 23, 59, 15, 2));

    ASSERT_EQ(dateAdd(startDate, TimeUnit::millisecond, -2, kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2020, 12, 31, 23, 59, 14, 999));

    ASSERT_EQ(dateAdd(startDate, TimeUnit::millisecond, 45000, kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2021, 1, 1, 0, 0, 0, 1));

    ASSERT_EQ(dateAdd(startDate, TimeUnit::millisecond, -1501, kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2020, 12, 31, 23, 59, 13, 500));

    // Verify that an overflow is detected.
    ASSERT_THROWS_CODE(dateAdd(startDate,
                               TimeUnit::millisecond,
                               std::numeric_limits<long long>::max(),
                               kDefaultTimeZone),
                       AssertionException,
                       ErrorCodes::Error::DurationOverflow);
}

TEST(IsValidDayOfWeek, Basic) {
    ASSERT(isValidDayOfWeek("monday"));
    ASSERT(isValidDayOfWeek("tue"));
    ASSERT(isValidDayOfWeek("Wednesday"));
    ASSERT(isValidDayOfWeek("FRIDAY"));
    ASSERT(!isValidDayOfWeek(""));
    ASSERT(!isValidDayOfWeek("SND"));
}

TEST(ParseDayOfWeek, Basic) {
    ASSERT(DayOfWeek::thursday == parseDayOfWeek("thursday"));
    ASSERT(DayOfWeek::saturday == parseDayOfWeek("SAT"));
    ASSERT_THROWS_CODE(parseDayOfWeek(""), AssertionException, ErrorCodes::FailedToParse);
}

TEST(ParseUtcOffset, InvalidInputs) {
    for (auto&& value :
         {"",       " ",       "  ",      "🤡",     "a",       "ABC",     "\0",     "\n",
          " +",     "\n+",     "+",       "-",      ":",       "++",      "--",     "+ 0",
          "- 0",    "+0:",     "+a",      "+aa",    "+0a",     "0",       "0:",     "+0:0",
          "+0a:0",  "+00:a",   "+00:0",   "-00:0",  "+01:000", "-12:000", "-01:0a", "+09:ab",
          "+00:0a", "+00:00a", "-12:34x", "+xx:xx", "-  :  ",  "+00:-00", "-00:+00"}) {
        ASSERT_EQ(boost::none, kDefaultTimeZoneDatabase.parseUtcOffset(value));
    }
}

TEST(ParseUtcOffset, HourInputs) {
    // '-99:00' is a valid input, as is '+99:00'.
    for (int i = -99; i <= 99; ++i) {
        // '+/-HH':
        auto value = fmt::format("{:+03}", i);
        auto offset = kDefaultTimeZoneDatabase.parseUtcOffset(value);
        ASSERT_TRUE(offset.has_value());
        ASSERT_EQ(Seconds(i * 3600), *offset);

        // '+HHMM' / '-HHMM':
        value = fmt::format("{:+03}00", i);
        offset = kDefaultTimeZoneDatabase.parseUtcOffset(value);
        ASSERT_TRUE(offset.has_value());
        ASSERT_EQ(Seconds(i * 3600), *offset);

        // '+HH:MM' / '-HH:MM':
        value = fmt::format("{:+03}:00", i);
        offset = kDefaultTimeZoneDatabase.parseUtcOffset(value);
        ASSERT_TRUE(offset.has_value());
        ASSERT_EQ(Seconds(i * 3600), *offset);

        // '+HH:99' / '-HH:99':
        value = fmt::format("{:+03}:99", i);
        offset = kDefaultTimeZoneDatabase.parseUtcOffset(value);
        ASSERT_TRUE(offset.has_value());
        ASSERT_EQ(Seconds(i * 3600 + 60 * (i >= 0 ? 99 : -99)), *offset);

        // '+HHMM' / '-HH:MM':
        value = fmt::format("{:+03}99", i);
        offset = kDefaultTimeZoneDatabase.parseUtcOffset(value);
        ASSERT_TRUE(offset.has_value());
        ASSERT_EQ(Seconds(i * 3600 + 60 * (i >= 0 ? 99 : -99)), *offset);

        // '+HH:MM:99' / '-HH:MM:99':
        value = fmt::format("{:+03}:99", i);
        offset = kDefaultTimeZoneDatabase.parseUtcOffset(value);
        ASSERT_TRUE(offset.has_value());
        ASSERT_EQ(Seconds(i * 3600 + 60 * (i >= 0 ? 99 : -99)), *offset);
    }
}

TEST(ParseUtcOffset, MinuteInputs) {
    // '-00:99' is a valid input, as is '+00:99'.
    for (int i = 0; i <= 99; ++i) {
        // '+HHMM'
        auto value = fmt::format("+01{:02}", i);
        auto offset = kDefaultTimeZoneDatabase.parseUtcOffset(value);
        ASSERT_TRUE(offset.has_value());
        ASSERT_EQ(Seconds(3600 + i * 60), *offset);

        // '-HH:MM':
        value = fmt::format("-11:{:02}", i);
        offset = kDefaultTimeZoneDatabase.parseUtcOffset(value);
        ASSERT_TRUE(offset.has_value());
        ASSERT_EQ(Seconds(-11 * 3600 - i * 60), *offset);

        // '+HH:MM'
        value = fmt::format("+04:{:02}", i);
        offset = kDefaultTimeZoneDatabase.parseUtcOffset(value);
        ASSERT_TRUE(offset.has_value());
        ASSERT_EQ(Seconds(4 * 3600 + i * 60), *offset);

        // '-HH:MM'
        value = fmt::format("-77:{:02}", i);
        offset = kDefaultTimeZoneDatabase.parseUtcOffset(value);
        ASSERT_TRUE(offset.has_value());
        ASSERT_EQ(Seconds(-77 * 3600 - i * 60), *offset);
    }
}

TEST(TimeZoneToString, Basic) {
    // Just asserting that these do not throw exceptions.
    ASSERT_EQ(kDefaultTimeZoneDatabase.getTimeZone("UTC").toString(), "TimeZone(UTC)");
    ASSERT_EQ(kDefaultTimeZoneDatabase.getTimeZone("America/New_York").toString(),
              "TimeZone(name=America/New_York)");
    ASSERT_EQ(kDefaultTimeZoneDatabase.getTimeZone("+02").toString(), "TimeZone(utcOffset=7200s)");
}
}  // namespace
}  // namespace mongo
