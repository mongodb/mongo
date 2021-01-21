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

#include "mongo/platform/basic.h"

#include <limits>
#include <sstream>
#include <timelib.h>

#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/unittest/unittest.h"

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
    ASSERT_OK(newYorkZone.outputDateWithFormat(os,
                                               "%Y/%m/%d %H:%M:%S:%L, dayOfYear: %j, "
                                               "dayOfWeek: %w, week: %U, isoYear: %G, "
                                               "isoWeek: %V, isoDayOfWeek: %u, percent: %%",
                                               date));
    ASSERT_EQ(os.str(),
              "2017/06/06 15:38:43:234, dayOfYear: 157, dayOfWeek: 3, week: 23, isoYear: 2017, "
              "isoWeek: 23, isoDayOfWeek: 2, percent: %");
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

// Time zones for testing 'dateDiff()'.
const TimeZone kNewYorkTimeZone = kDefaultTimeZoneDatabase.getTimeZone("America/New_York");
const TimeZone kAustraliaEuclaTimeZone =
    kDefaultTimeZoneDatabase.getTimeZone("Australia/Eucla");  // UTC offset +08:45

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
    ASSERT_EQ(234047495,
              dateDiff(kNewYorkTimeZone.createFromDateParts(1585, 11, 8, 1, 55, 0, 0),
                       kNewYorkTimeZone.createFromDateParts(2030, 11, 8, 1, 30, 59, 999),
                       TimeUnit::minute,
                       kNewYorkTimeZone));
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

TEST(DateAdd, DateAddYear) {
    ASSERT_EQ(dateAdd(kDefaultTimeZone.createFromDateParts(2019, 11, 11, 0, 0, 0, 0),
                      TimeUnit::year,
                      1,
                      kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2020, 11, 11, 0, 0, 0, 0));

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

TEST(DateAdd, DateAddQuarter) {
    auto startDate = kDefaultTimeZone.createFromDateParts(2020, 1, 1, 0, 0, 0, 0);
    ASSERT_EQ(dateAdd(startDate, TimeUnit::quarter, 1, kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2020, 4, 1, 0, 0, 0, 0));

    ASSERT_EQ(dateAdd(startDate, TimeUnit::quarter, -5, kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2018, 10, 1, 0, 0, 0, 0));

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
    auto startDate = kDefaultTimeZone.createFromDateParts(2020, 8, 31, 10, 5, 0, 0);
    ASSERT_EQ(dateAdd(startDate, TimeUnit::month, 1, kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2020, 9, 30, 10, 5, 0, 0));

    ASSERT_EQ(dateAdd(startDate, TimeUnit::month, 5, kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2021, 1, 31, 10, 5, 0, 0));

    ASSERT_EQ(dateAdd(startDate, TimeUnit::month, 6, kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2021, 2, 28, 10, 5, 0, 0));

    ASSERT_EQ(dateAdd(startDate, TimeUnit::month, -1, kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2020, 7, 31, 10, 5, 0, 0));

    ASSERT_EQ(dateAdd(startDate, TimeUnit::month, -4, kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2020, 4, 30, 10, 5, 0, 0));

    ASSERT_EQ(dateAdd(startDate, TimeUnit::month, -18, kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2019, 2, 28, 10, 5, 0, 0));

    ASSERT_EQ(dateAdd(kDefaultTimeZone.createFromDateParts(2020, 1, 30, 10, 5, 0, 0),
                      TimeUnit::month,
                      1,
                      kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2020, 2, 29, 10, 5, 0, 0));
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

    auto amsZone = kDefaultTimeZoneDatabase.getTimeZone("Europe/Amsterdam");
    // Last day of month in Amsterdam zone 2021-05-31T00:30:00
    auto startDate = kDefaultTimeZone.createFromDateParts(2021, 05, 30, 22, 30, 0, 0);

    const std::vector<TestCase> tests{
        {startDate,
         TimeUnit::month,
         1,
         kDefaultTimeZone.createFromDateParts(
             2021, 06, 29, 22, 30, 0, 0)},  // 2021-06-30T00:30:00 Ams
        {startDate,
         TimeUnit::month,
         -1,
         kDefaultTimeZone.createFromDateParts(
             2021, 04, 29, 22, 30, 0, 0)},  // 2021-04-30T00:30:00 Ams
        {startDate,
         TimeUnit::quarter,
         -1,
         kDefaultTimeZone.createFromDateParts(
             2021, 02, 27, 23, 30, 0, 0)},  // 2021-02-28T00:30:00 Ams
        {startDate,
         TimeUnit::quarter,
         -5,
         kDefaultTimeZone.createFromDateParts(
             2020, 02, 28, 23, 30, 0, 0)},  // 2020-02-29T00:30:00 Ams
    };
    for (const auto& test : tests) {
        const auto testNumber = &test - &tests.front();
        ASSERT_EQ(dateAdd(test.startDate, test.unit, test.amount, amsZone), test.expectedDate)
            << "on test " << testNumber;
    }

    auto nyZone = kDefaultTimeZoneDatabase.getTimeZone("America/New_York");
    // Instant in NY zone 2020-12-30T22:00:00
    auto startDateNY = kDefaultTimeZone.createFromDateParts(2020, 12, 31, 3, 0, 0, 0);

    const std::vector<TestCase> tests2{
        {startDateNY,
         TimeUnit::month,
         -1,
         kDefaultTimeZone.createFromDateParts(
             2020, 12, 1, 3, 0, 0, 0)},  // 2020-11-30T22:00:00 NY zone
        {startDateNY,
         TimeUnit::month,
         2,
         kDefaultTimeZone.createFromDateParts(
             2021, 3, 1, 3, 0, 0, 0)},  // 2021-02-28T22:00:00 NY zone
        {startDateNY,
         TimeUnit::quarter,
         -2,
         kDefaultTimeZone.createFromDateParts(
             2020, 7, 1, 2, 0, 0, 0)},  // 2020-06-30T22:00:00 NY zone
    };

    for (const auto& test : tests2) {
        const auto testNumber = &test - &tests2.front();
        ASSERT_EQ(dateAdd(test.startDate, test.unit, test.amount, nyZone), test.expectedDate)
            << "on test " << testNumber;
    }
}

TEST(DateAdd, DateAddDay) {
    auto startDate = kDefaultTimeZone.createFromDateParts(2020, 8, 31, 10, 5, 0, 0);
    ASSERT_EQ(dateAdd(startDate, TimeUnit::day, 1, kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2020, 9, 1, 10, 5, 0, 0));

    ASSERT_EQ(dateAdd(startDate, TimeUnit::day, -1, kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2020, 8, 30, 10, 5, 0, 0));

    ASSERT_EQ(dateAdd(startDate, TimeUnit::day, -366, kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2019, 8, 31, 10, 5, 0, 0));
}

TEST(DateAdd, DateAddHour) {
    struct TestCase {
        Date_t startDate;
        long long amount;
        Date_t expectedDate;
    };

    auto utcStartDate = kDefaultTimeZone.createFromDateParts(2020, 8, 31, 10, 5, 0, 0);

    std::vector<TestCase> tests{
        {utcStartDate, 1, kDefaultTimeZone.createFromDateParts(2020, 8, 31, 11, 5, 0, 0)},
        {utcStartDate, -1, kDefaultTimeZone.createFromDateParts(2020, 8, 31, 9, 5, 0, 0)},
        {utcStartDate, 168, kDefaultTimeZone.createFromDateParts(2020, 9, 7, 10, 5, 0, 0)},
    };

    for (auto&& test : tests) {
        ASSERT_EQ(dateAdd(test.startDate, TimeUnit::hour, test.amount, kDefaultTimeZone),
                  test.expectedDate);
    }
}

TEST(DateAdd, DateAddMinute) {
    auto startDate = kDefaultTimeZone.createFromDateParts(2020, 12, 31, 23, 55, 15, 0);
    ASSERT_EQ(dateAdd(startDate, TimeUnit::minute, 1, kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2020, 12, 31, 23, 56, 15, 0));

    ASSERT_EQ(dateAdd(startDate, TimeUnit::minute, -1, kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2020, 12, 31, 23, 54, 15, 0));

    ASSERT_EQ(dateAdd(startDate, TimeUnit::minute, 10, kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2021, 1, 1, 0, 5, 15, 0));

    ASSERT_EQ(dateAdd(startDate, TimeUnit::minute, -1440, kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2020, 12, 30, 23, 55, 15, 0));
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

    auto amsZone = kDefaultTimeZoneDatabase.getTimeZone("Europe/Amsterdam");
    // The test uses the DST change in Amsterdam zone that happens at 2020-10-25T01:00:00 UTC.
    // Pick two time instants in UTC just before and after the DST change.
    auto startDateBeforeDSTChange = kDefaultTimeZone.createFromDateParts(2020, 10, 25, 0, 50, 5, 0);
    auto startDateAfterDSTChange = kDefaultTimeZone.createFromDateParts(2020, 10, 25, 1, 10, 5, 0);

    std::vector<TestCase> tests{
        {startDateBeforeDSTChange,
         TimeUnit::minute,
         10,
         kDefaultTimeZone.createFromDateParts(2020, 10, 25, 1, 0, 5, 0)},
        {startDateBeforeDSTChange,
         TimeUnit::hour,
         1,
         kDefaultTimeZone.createFromDateParts(2020, 10, 25, 1, 50, 5, 0)},
        {startDateBeforeDSTChange,
         TimeUnit::day,
         1,
         // Adds an hour DST correction to produce the same local time next day in Ams timezone.
         kDefaultTimeZone.createFromDateParts(2020, 10, 26, 1, 50, 5, 0)},
        {startDateAfterDSTChange,
         TimeUnit::minute,
         -20,
         kDefaultTimeZone.createFromDateParts(2020, 10, 25, 0, 50, 5, 0)},
        {startDateAfterDSTChange,
         TimeUnit::hour,
         -1,
         kDefaultTimeZone.createFromDateParts(2020, 10, 25, 0, 10, 5, 0)},
        {startDateAfterDSTChange,
         TimeUnit::day,
         -1,
         // Subtracts an hour DST correction to produce the same local time previous day in Ams
         // timezone.
         kDefaultTimeZone.createFromDateParts(2020, 10, 24, 0, 10, 5, 0)},
    };

    for (auto&& test : tests) {
        ASSERT_EQ(dateAdd(test.startDate, test.unit, test.amount, amsZone), test.expectedDate);
    }

    auto nyZone = kDefaultTimeZoneDatabase.getTimeZone("America/New_York");
    // The following test uses the DST change in New York at 2020-03-08T07:00:00Z.
    // Pick two time instants in UTC just before and after the DST change.
    auto startDateInEST = kDefaultTimeZone.createFromDateParts(2020, 3, 8, 6, 50, 0, 0);
    auto startDateInEDT = kDefaultTimeZone.createFromDateParts(2020, 3, 8, 7, 20, 0, 0);

    std::vector<TestCase> tests2{
        {startDateInEST,
         TimeUnit::minute,
         15,
         kDefaultTimeZone.createFromDateParts(2020, 3, 8, 7, 5, 0, 0)},
        {startDateInEST,
         TimeUnit::hour,
         1,
         kDefaultTimeZone.createFromDateParts(2020, 3, 8, 7, 50, 0, 0)},
        {startDateInEST,
         TimeUnit::day,
         1,
         kDefaultTimeZone.createFromDateParts(2020, 3, 9, 5, 50, 0, 0)},
        {startDateInEDT,
         TimeUnit::minute,
         -25,
         kDefaultTimeZone.createFromDateParts(2020, 3, 8, 6, 55, 0, 0)},
        {startDateInEDT,
         TimeUnit::hour,
         -1,
         kDefaultTimeZone.createFromDateParts(2020, 3, 8, 6, 20, 0, 0)},
        {startDateInEDT,
         TimeUnit::day,
         -1,
         kDefaultTimeZone.createFromDateParts(2020, 3, 7, 8, 20, 0, 0)},
    };

    for (auto&& test : tests2) {
        ASSERT_EQ(dateAdd(test.startDate, test.unit, test.amount, nyZone), test.expectedDate);
    }
}

TEST(DateAdd, DateAddSecond) {
    auto startDate = kDefaultTimeZone.createFromDateParts(2020, 12, 31, 23, 55, 15, 0);
    ASSERT_EQ(dateAdd(startDate, TimeUnit::second, 1, kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2020, 12, 31, 23, 55, 16, 0));

    ASSERT_EQ(dateAdd(startDate, TimeUnit::second, -1, kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2020, 12, 31, 23, 55, 14, 0));

    ASSERT_EQ(dateAdd(startDate, TimeUnit::second, 300, kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2021, 1, 1, 0, 0, 15, 0));

    ASSERT_EQ(dateAdd(startDate, TimeUnit::second, -195, kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2020, 12, 31, 23, 52, 0, 0));
}

TEST(DateAdd, DateAddMillisecond) {
    auto startDate = kDefaultTimeZone.createFromDateParts(2020, 12, 31, 23, 59, 15, 0);
    ASSERT_EQ(dateAdd(startDate, TimeUnit::millisecond, 1, kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2020, 12, 31, 23, 59, 15, 1));

    ASSERT_EQ(dateAdd(startDate, TimeUnit::millisecond, -1, kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2020, 12, 31, 23, 59, 14, 999));

    ASSERT_EQ(dateAdd(startDate, TimeUnit::millisecond, 45001, kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2021, 1, 1, 0, 0, 0, 1));

    ASSERT_EQ(dateAdd(startDate, TimeUnit::millisecond, -1500, kDefaultTimeZone),
              kDefaultTimeZone.createFromDateParts(2020, 12, 31, 23, 59, 13, 500));
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
}  // namespace
}  // namespace mongo
