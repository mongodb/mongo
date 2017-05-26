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

TEST(UTCTimeBeforeEpoch, DoesComputeWeek) {
    // Sunday, December 28, 1969.
    auto date = Date_t::fromMillisSinceEpoch(-345600000LL);
    ASSERT_EQ(TimeZoneDatabase::utcZone().week(date), 52);
    // Saturday, January 1, 1966.
    date = Date_t::fromMillisSinceEpoch(-126230400000LL);
    ASSERT_EQ(TimeZoneDatabase::utcZone().week(date), 0);
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

TEST(UTCTimeAtEpoch, DoesComputeISOYear) {
    // Thursday, January 1, 1970.
    auto date = Date_t::fromMillisSinceEpoch(0);
    ASSERT_EQ(TimeZoneDatabase::utcZone().isoYear(date), 1970);
}

TEST(UTCTimeAtEpoch, DoesComputeDayOfWeek) {
    // Thursday, January 1, 1970.
    auto date = Date_t::fromMillisSinceEpoch(0);
    ASSERT_EQ(TimeZoneDatabase::utcZone().dayOfWeek(date), 5);
}

TEST(UTCTimeAtEpoch, DoesComputeISODayOfWeek) {
    // Thursday, January 1, 1970.
    auto date = Date_t::fromMillisSinceEpoch(0);
    ASSERT_EQ(TimeZoneDatabase::utcZone().isoDayOfWeek(date), 4);
}

TEST(UTCTimeAtEpoch, DoesComputeDayOfYear) {
    // Thursday, January 1, 1970.
    auto date = Date_t::fromMillisSinceEpoch(0);
    ASSERT_EQ(TimeZoneDatabase::utcZone().dayOfYear(date), 1);
}

TEST(UTCTimeAtEpoch, DoesComputeWeek) {
    // Thursday, January 1, 1970.
    auto date = Date_t::fromMillisSinceEpoch(0);
    ASSERT_EQ(TimeZoneDatabase::utcZone().week(date), 0);
}

TEST(UTCTimeAtEpoch, DoesComputeISOWeek) {
    // Thursday, January 1, 1970.
    auto date = Date_t::fromMillisSinceEpoch(0);
    ASSERT_EQ(TimeZoneDatabase::utcZone().isoWeek(date), 1);
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

TEST(DateFormat, ThrowsUserExceptionIfGivenUnrecognizedFormatter) {
    ASSERT_THROWS_CODE(TimeZoneDatabase::utcZone().validateFormat("%x"), UserException, 18536);
}

TEST(DateFormat, ThrowsUserExceptionIfGivenUnmatchedPercent) {
    ASSERT_THROWS_CODE(TimeZoneDatabase::utcZone().validateFormat("%"), UserException, 18535);
    ASSERT_THROWS_CODE(TimeZoneDatabase::utcZone().validateFormat("%%%"), UserException, 18535);
    ASSERT_THROWS_CODE(
        TimeZoneDatabase::utcZone().validateFormat("blahblah%"), UserException, 18535);
}

TEST(DateFormat, ThrowsUserExceptionIfGivenDateBeforeYear0) {
    const long long kMillisPerYear = 31556926000;
    ASSERT_THROWS_CODE(TimeZoneDatabase::utcZone().formatDate(
                           "%Y", Date_t::fromMillisSinceEpoch(-(kMillisPerYear * 1971))),
                       UserException,
                       18537);
    ASSERT_EQ("0000",
              TimeZoneDatabase::utcZone().formatDate(
                  "%Y", Date_t::fromMillisSinceEpoch(-(kMillisPerYear * 1970))));
}

TEST(DateFormat, ThrowsUserExceptionIfGivenDateAfterYear9999) {
    ASSERT_THROWS_CODE(
        TimeZoneDatabase::utcZone().formatDate("%Y", Date_t::max()), UserException, 18537);
}
}  // namespace
}  // namespace mongo
