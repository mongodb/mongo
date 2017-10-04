/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include <sstream>

#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

const TimeZoneDatabase kDefaultTimeZoneDatabase{};

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
    ASSERT_EQ(TimeZoneDatabase::utcZone().formatDate("%Y/%m/%d %H:%M:%S:%L, dayOfYear: %j, "
                                                     "dayOfWeek: %w, week: %U, isoYear: %G, "
                                                     "isoWeek: %V, isoDayOfWeek: %u, percent: %%",
                                                     date),
              "1969/12/30 13:42:23:211, dayOfYear: 364, dayOfWeek: 3, week: 52, isoYear: 1970, "
              "isoWeek: 01, isoDayOfWeek: 2, percent: %");
}

TEST(NewYorkTimeBeforeEpoch, DoesFormatDate) {
    auto newYorkZone = kDefaultTimeZoneDatabase.getTimeZone("America/New_York");

    // Tuesday, Dec 30, 1969 13:42:23:211
    auto date = Date_t::fromMillisSinceEpoch(-123456789LL);
    ASSERT_EQ(newYorkZone.formatDate("%Y/%m/%d %H:%M:%S:%L, dayOfYear: %j, "
                                     "dayOfWeek: %w, week: %U, isoYear: %G, "
                                     "isoWeek: %V, isoDayOfWeek: %u, percent: %%",
                                     date),
              "1969/12/30 08:42:23:211, dayOfYear: 364, dayOfWeek: 3, week: 52, isoYear: 1970, "
              "isoWeek: 01, isoDayOfWeek: 2, percent: %");
}

TEST(UTCTimeBeforeEpoch, DoesOutputFormatDate) {
    // Tuesday, Dec 30, 1969 13:42:23:211
    auto date = Date_t::fromMillisSinceEpoch(-123456789LL);
    std::ostringstream os;
    TimeZoneDatabase::utcZone().outputDateWithFormat(os,
                                                     "%Y/%m/%d %H:%M:%S:%L, dayOfYear: %j, "
                                                     "dayOfWeek: %w, week: %U, isoYear: %G, "
                                                     "isoWeek: %V, isoDayOfWeek: %u, percent: %%",
                                                     date);
    ASSERT_EQ(os.str(),
              "1969/12/30 13:42:23:211, dayOfYear: 364, dayOfWeek: 3, week: 52, isoYear: 1970, "
              "isoWeek: 01, isoDayOfWeek: 2, percent: %");
}

TEST(NewYorkTimeBeforeEpoch, DoesOutputFormatDate) {
    auto newYorkZone = kDefaultTimeZoneDatabase.getTimeZone("America/New_York");

    // Tuesday, Dec 30, 1969 13:42:23:211
    auto date = Date_t::fromMillisSinceEpoch(-123456789LL);
    std::ostringstream os;
    newYorkZone.outputDateWithFormat(os,
                                     "%Y/%m/%d %H:%M:%S:%L, dayOfYear: %j, "
                                     "dayOfWeek: %w, week: %U, isoYear: %G, "
                                     "isoWeek: %V, isoDayOfWeek: %u, percent: %%",
                                     date);
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
    ASSERT_EQ(TimeZoneDatabase::utcZone().formatDate("%Y/%m/%d %H:%M:%S:%L, dayOfYear: %j, "
                                                     "dayOfWeek: %w, week: %U, isoYear: %G, "
                                                     "isoWeek: %V, isoDayOfWeek: %u, percent: %%",
                                                     date),
              "1970/01/01 00:00:00:000, dayOfYear: 001, dayOfWeek: 5, week: 00, isoYear: 1970, "
              "isoWeek: 01, isoDayOfWeek: 4, percent: %");
}

TEST(NewYorkTimeAtEpoch, DoesFormatDate) {
    auto newYorkZone = kDefaultTimeZoneDatabase.getTimeZone("America/New_York");

    // Thu, Jan 1, 1970 00:00:00:000Z
    auto date = Date_t::fromMillisSinceEpoch(0);
    ASSERT_EQ(newYorkZone.formatDate("%Y/%m/%d %H:%M:%S:%L, dayOfYear: %j, "
                                     "dayOfWeek: %w, week: %U, isoYear: %G, "
                                     "isoWeek: %V, isoDayOfWeek: %u, percent: %%",
                                     date),
              "1969/12/31 19:00:00:000, dayOfYear: 365, dayOfWeek: 4, week: 52, isoYear: 1970, "
              "isoWeek: 01, isoDayOfWeek: 3, percent: %");
}

TEST(UTCTimeAtEpoch, DoesOutputFormatDate) {
    auto date = Date_t::fromMillisSinceEpoch(0);
    std::ostringstream os;
    TimeZoneDatabase::utcZone().outputDateWithFormat(os,
                                                     "%Y/%m/%d %H:%M:%S:%L, dayOfYear: %j, "
                                                     "dayOfWeek: %w, week: %U, isoYear: %G, "
                                                     "isoWeek: %V, isoDayOfWeek: %u, percent: %%",
                                                     date);
    ASSERT_EQ(os.str(),
              "1970/01/01 00:00:00:000, dayOfYear: 001, dayOfWeek: 5, week: 00, isoYear: 1970, "
              "isoWeek: 01, isoDayOfWeek: 4, percent: %");
}

TEST(NewYorkTimeAtEpoch, DoesOutputFormatDate) {
    auto newYorkZone = kDefaultTimeZoneDatabase.getTimeZone("America/New_York");
    auto date = Date_t::fromMillisSinceEpoch(0);
    std::ostringstream os;
    newYorkZone.outputDateWithFormat(os,
                                     "%Y/%m/%d %H:%M:%S:%L, dayOfYear: %j, "
                                     "dayOfWeek: %w, week: %U, isoYear: %G, "
                                     "isoWeek: %V, isoDayOfWeek: %u, percent: %%",
                                     date);
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
    ASSERT_EQ(TimeZoneDatabase::utcZone().formatDate("%Y/%m/%d %H:%M:%S:%L, dayOfYear: %j, "
                                                     "dayOfWeek: %w, week: %U, isoYear: %G, "
                                                     "isoWeek: %V, isoDayOfWeek: %u, percent: %%",
                                                     date),
              "2017/06/06 19:38:43:234, dayOfYear: 157, dayOfWeek: 3, week: 23, isoYear: 2017, "
              "isoWeek: 23, isoDayOfWeek: 2, percent: %");
}

TEST(NewYorkTimeAfterEpoch, DoesFormatDate) {
    // 2017-06-06T19:38:43:234Z (Tuesday).
    auto date = Date_t::fromMillisSinceEpoch(1496777923234LL);
    auto newYorkZone = kDefaultTimeZoneDatabase.getTimeZone("America/New_York");
    ASSERT_EQ(newYorkZone.formatDate("%Y/%m/%d %H:%M:%S:%L, dayOfYear: %j, "
                                     "dayOfWeek: %w, week: %U, isoYear: %G, "
                                     "isoWeek: %V, isoDayOfWeek: %u, percent: %%",
                                     date),
              "2017/06/06 15:38:43:234, dayOfYear: 157, dayOfWeek: 3, week: 23, isoYear: 2017, "
              "isoWeek: 23, isoDayOfWeek: 2, percent: %");
}

TEST(UTCOffsetAfterEpoch, DoesFormatDate) {
    // 2017-06-06T19:38:43:234Z (Tuesday).
    auto date = Date_t::fromMillisSinceEpoch(1496777923234LL);
    auto offsetSpec = kDefaultTimeZoneDatabase.getTimeZone("+02:30");
    ASSERT_EQ(offsetSpec.formatDate("%Y/%m/%d %H:%M:%S:%L, dayOfYear: %j, "
                                    "dayOfWeek: %w, week: %U, isoYear: %G, "
                                    "isoWeek: %V, isoDayOfWeek: %u, percent: %%",
                                    date),
              "2017/06/06 22:08:43:234, dayOfYear: 157, dayOfWeek: 3, week: 23, isoYear: 2017, "
              "isoWeek: 23, isoDayOfWeek: 2, percent: %");
}

TEST(UTCTimeAfterEpoch, DoesOutputFormatDate) {
    // Tue, Jun 6, 2017 19:38:43:234.
    auto date = Date_t::fromMillisSinceEpoch(1496777923234LL);
    std::ostringstream os;
    TimeZoneDatabase::utcZone().outputDateWithFormat(os,
                                                     "%Y/%m/%d %H:%M:%S:%L, dayOfYear: %j, "
                                                     "dayOfWeek: %w, week: %U, isoYear: %G, "
                                                     "isoWeek: %V, isoDayOfWeek: %u, percent: %%",
                                                     date);
    ASSERT_EQ(os.str(),
              "2017/06/06 19:38:43:234, dayOfYear: 157, dayOfWeek: 3, week: 23, isoYear: 2017, "
              "isoWeek: 23, isoDayOfWeek: 2, percent: %");
}

TEST(NewYorkTimeAfterEpoch, DoesOutputFormatDate) {
    // 2017-06-06T19:38:43:234Z (Tuesday).
    auto date = Date_t::fromMillisSinceEpoch(1496777923234LL);
    std::ostringstream os;
    auto newYorkZone = kDefaultTimeZoneDatabase.getTimeZone("America/New_York");
    newYorkZone.outputDateWithFormat(os,
                                     "%Y/%m/%d %H:%M:%S:%L, dayOfYear: %j, "
                                     "dayOfWeek: %w, week: %U, isoYear: %G, "
                                     "isoWeek: %V, isoDayOfWeek: %u, percent: %%",
                                     date);
    ASSERT_EQ(os.str(),
              "2017/06/06 15:38:43:234, dayOfYear: 157, dayOfWeek: 3, week: 23, isoYear: 2017, "
              "isoWeek: 23, isoDayOfWeek: 2, percent: %");
}

TEST(DateFormat, ThrowsUserExceptionIfGivenUnrecognizedFormatter) {
    ASSERT_THROWS_CODE(TimeZoneDatabase::utcZone().validateFormat("%x"), AssertionException, 18536);
}

TEST(DateFormat, ThrowsUserExceptionIfGivenUnmatchedPercent) {
    ASSERT_THROWS_CODE(TimeZoneDatabase::utcZone().validateFormat("%"), AssertionException, 18535);
    ASSERT_THROWS_CODE(
        TimeZoneDatabase::utcZone().validateFormat("%%%"), AssertionException, 18535);
    ASSERT_THROWS_CODE(
        TimeZoneDatabase::utcZone().validateFormat("blahblah%"), AssertionException, 18535);
}

TEST(DateFormat, ThrowsUserExceptionIfGivenDateBeforeYear0) {
    const long long kMillisPerYear = 31556926000;

    ASSERT_THROWS_CODE(TimeZoneDatabase::utcZone().formatDate(
                           "%Y", Date_t::fromMillisSinceEpoch(-(kMillisPerYear * 1971))),
                       AssertionException,
                       18537);

    ASSERT_THROWS_CODE(TimeZoneDatabase::utcZone().formatDate(
                           "%G", Date_t::fromMillisSinceEpoch(-(kMillisPerYear * 1971))),
                       AssertionException,
                       18537);

    ASSERT_EQ("0000",
              TimeZoneDatabase::utcZone().formatDate(
                  "%Y", Date_t::fromMillisSinceEpoch(-(kMillisPerYear * 1970))));
}

TEST(DateFormat, ThrowsUserExceptionIfGivenDateAfterYear9999) {
    ASSERT_THROWS_CODE(
        TimeZoneDatabase::utcZone().formatDate("%Y", Date_t::max()), AssertionException, 18537);

    ASSERT_THROWS_CODE(
        TimeZoneDatabase::utcZone().formatDate("%G", Date_t::max()), AssertionException, 18537);
}

}  // namespace
}  // namespace mongo
