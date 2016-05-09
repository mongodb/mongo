/*    Copyright 2013 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include <cstdlib>
#include <ctime>
#include <string>

#include "mongo/base/init.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/log.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {

const bool isTimeTSmall =
    (sizeof(time_t) == sizeof(int32_t)) && std::numeric_limits<time_t>::is_signed;

/**
 * To make this test deterministic, we set the time zone to America/New_York.
 */
#ifdef _WIN32
char tzEnvString[] = "TZ=EST+5EDT";
#else
char tzEnvString[] = "TZ=America/New_York";
#endif
MONGO_INITIALIZER(SetTimeZoneToEasternForTest)(InitializerContext*) {
    if (-1 == putenv(tzEnvString)) {
        return Status(ErrorCodes::BadValue, errnoWithDescription());
    }
    tzset();
    return Status::OK();
}

TEST(TimeFormatting, DateAsISO8601UTCString) {
    ASSERT_EQUALS(std::string("1970-01-01T00:00:00.000Z"), dateToISOStringUTC(Date_t()));
    ASSERT_EQUALS(std::string("1970-06-30T01:06:40.981Z"),
                  dateToISOStringUTC(Date_t::fromMillisSinceEpoch(15556000981LL)));
    if (!isTimeTSmall)
        ASSERT_EQUALS(std::string("2058-02-20T18:29:11.100Z"),
                      dateToISOStringUTC(Date_t::fromMillisSinceEpoch(2781455351100LL)));
    ASSERT_EQUALS(std::string("2013-02-20T18:29:11.100Z"),
                  dateToISOStringUTC(Date_t::fromMillisSinceEpoch(1361384951100LL)));
}

TEST(TimeFormatting, DateAsISO8601LocalString) {
    ASSERT_EQUALS(std::string("1969-12-31T19:00:00.000-0500"), dateToISOStringLocal(Date_t()));
    ASSERT_EQUALS(std::string("1970-06-29T21:06:40.981-0400"),
                  dateToISOStringLocal(Date_t::fromMillisSinceEpoch(15556000981LL)));
    if (!isTimeTSmall)
        ASSERT_EQUALS(std::string("2058-02-20T13:29:11.100-0500"),
                      dateToISOStringLocal(Date_t::fromMillisSinceEpoch(2781455351100LL)));
    ASSERT_EQUALS(std::string("2013-02-20T13:29:11.100-0500"),
                  dateToISOStringLocal(Date_t::fromMillisSinceEpoch(1361384951100LL)));
}

TEST(TimeFormatting, DateAsCtimeString) {
    ASSERT_EQUALS(std::string("Wed Dec 31 19:00:00.000"), dateToCtimeString(Date_t()));
    ASSERT_EQUALS(std::string("Mon Jun 29 21:06:40.981"),
                  dateToCtimeString(Date_t::fromMillisSinceEpoch(15556000981LL)));
    if (!isTimeTSmall)
        ASSERT_EQUALS(std::string("Wed Feb 20 13:29:11.100"),
                      dateToCtimeString(Date_t::fromMillisSinceEpoch(2781455351100LL)));
    ASSERT_EQUALS(std::string("Wed Feb 20 13:29:11.100"),
                  dateToCtimeString(Date_t::fromMillisSinceEpoch(1361384951100LL)));
}

static std::string stringstreamDate(void (*formatter)(std::ostream&, Date_t), Date_t date) {
    std::ostringstream os;
    formatter(os, date);
    return os.str();
}

TEST(TimeFormatting, DateAsISO8601UTCStream) {
    ASSERT_EQUALS(std::string("1970-01-01T00:00:00.000Z"),
                  stringstreamDate(outputDateAsISOStringUTC, Date_t()));
    ASSERT_EQUALS(
        std::string("1970-06-30T01:06:40.981Z"),
        stringstreamDate(outputDateAsISOStringUTC, Date_t::fromMillisSinceEpoch(15556000981LL)));
    if (!isTimeTSmall)
        ASSERT_EQUALS(std::string("2058-02-20T18:29:11.100Z"),
                      stringstreamDate(outputDateAsISOStringUTC,
                                       Date_t::fromMillisSinceEpoch(2781455351100LL)));
    ASSERT_EQUALS(
        std::string("2013-02-20T18:29:11.100Z"),
        stringstreamDate(outputDateAsISOStringUTC, Date_t::fromMillisSinceEpoch(1361384951100LL)));
}

TEST(TimeFormatting, DateAsISO8601LocalStream) {
    ASSERT_EQUALS(std::string("1969-12-31T19:00:00.000-0500"),
                  stringstreamDate(outputDateAsISOStringLocal, Date_t()));
    ASSERT_EQUALS(
        std::string("1970-06-29T21:06:40.981-0400"),
        stringstreamDate(outputDateAsISOStringLocal, Date_t::fromMillisSinceEpoch(15556000981LL)));
    if (!isTimeTSmall)
        ASSERT_EQUALS(std::string("2058-02-20T13:29:11.100-0500"),
                      stringstreamDate(outputDateAsISOStringLocal,
                                       Date_t::fromMillisSinceEpoch(2781455351100LL)));
    ASSERT_EQUALS(std::string("2013-02-20T13:29:11.100-0500"),
                  stringstreamDate(outputDateAsISOStringLocal,
                                   Date_t::fromMillisSinceEpoch(1361384951100LL)));
}

TEST(TimeFormatting, DateAsCtimeStream) {
    ASSERT_EQUALS(std::string("Wed Dec 31 19:00:00.000"),
                  stringstreamDate(outputDateAsCtime, Date_t::fromMillisSinceEpoch(0)));
    ASSERT_EQUALS(std::string("Mon Jun 29 21:06:40.981"),
                  stringstreamDate(outputDateAsCtime, Date_t::fromMillisSinceEpoch(15556000981LL)));
    if (!isTimeTSmall)
        ASSERT_EQUALS(
            std::string("Wed Feb 20 13:29:11.100"),
            stringstreamDate(outputDateAsCtime, Date_t::fromMillisSinceEpoch(2781455351100LL)));
    ASSERT_EQUALS(
        std::string("Wed Feb 20 13:29:11.100"),
        stringstreamDate(outputDateAsCtime, Date_t::fromMillisSinceEpoch(1361384951100LL)));
}

TEST(TimeParsing, DateAsISO8601UTC) {
    // Allowed date format:
    // YYYY-MM-DDTHH:MM[:SS[.m[m[m]]]]Z
    // Year, month, day, hour, and minute are required, while the seconds component and one to
    // three milliseconds are optional.

    StatusWith<Date_t> swull = dateFromISOString("1971-02-03T04:05:06.789Z");
    ASSERT_OK(swull.getStatus());
    ASSERT_EQUALS(swull.getValue().asInt64(), 34401906789LL);

    swull = dateFromISOString("1971-02-03T04:05:06.78Z");
    ASSERT_OK(swull.getStatus());
    ASSERT_EQUALS(swull.getValue().asInt64(), 34401906780LL);

    swull = dateFromISOString("1971-02-03T04:05:06.7Z");
    ASSERT_OK(swull.getStatus());
    ASSERT_EQUALS(swull.getValue().asInt64(), 34401906700LL);

    swull = dateFromISOString("1971-02-03T04:05:06Z");
    ASSERT_OK(swull.getStatus());
    ASSERT_EQUALS(swull.getValue().asInt64(), 34401906000LL);

    swull = dateFromISOString("1971-02-03T04:05Z");
    ASSERT_OK(swull.getStatus());
    ASSERT_EQUALS(swull.getValue().asInt64(), 34401900000LL);

    swull = dateFromISOString("1970-01-01T00:00:00.000Z");
    ASSERT_OK(swull.getStatus());
    ASSERT_EQUALS(swull.getValue().asInt64(), 0LL);

    swull = dateFromISOString("1970-06-30T01:06:40.981Z");
    ASSERT_OK(swull.getStatus());
    ASSERT_EQUALS(swull.getValue().asInt64(), 15556000981LL);

    if (!isTimeTSmall) {
        swull = dateFromISOString("2058-02-20T18:29:11.100Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 2781455351100LL);

        swull = dateFromISOString("3001-01-01T08:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 32535244800000LL);
    }

    swull = dateFromISOString("2013-02-20T18:29:11.100Z");
    ASSERT_OK(swull.getStatus());
    ASSERT_EQUALS(swull.getValue().asInt64(), 1361384951100LL);
}

TEST(TimeParsing, DateAsISO8601Local) {
    // Allowed date format:
    // YYYY-MM-DDTHH:MM[:SS[.m[m[m]]]]+HHMM
    // Year, month, day, hour, and minute are required, while the seconds component and one to
    // three milliseconds are optional.  The time zone offset must be four digits.

    StatusWith<Date_t> swull = dateFromISOString("1971-02-03T09:16:06.789+0511");
    ASSERT_OK(swull.getStatus());
    ASSERT_EQUALS(swull.getValue().asInt64(), 34401906789LL);

    swull = dateFromISOString("1971-02-03T09:16:06.78+0511");
    ASSERT_OK(swull.getStatus());
    ASSERT_EQUALS(swull.getValue().asInt64(), 34401906780LL);

    swull = dateFromISOString("1971-02-03T09:16:06.7+0511");
    ASSERT_OK(swull.getStatus());
    ASSERT_EQUALS(swull.getValue().asInt64(), 34401906700LL);

    swull = dateFromISOString("1971-02-03T09:16:06+0511");
    ASSERT_OK(swull.getStatus());
    ASSERT_EQUALS(swull.getValue().asInt64(), 34401906000LL);

    swull = dateFromISOString("1971-02-03T09:16+0511");
    ASSERT_OK(swull.getStatus());
    ASSERT_EQUALS(swull.getValue().asInt64(), 34401900000LL);

    swull = dateFromISOString("1970-01-01T00:00:00.000Z");
    ASSERT_OK(swull.getStatus());
    ASSERT_EQUALS(swull.getValue().asInt64(), 0LL);

    swull = dateFromISOString("1970-06-30T01:06:40.981Z");
    ASSERT_OK(swull.getStatus());
    ASSERT_EQUALS(swull.getValue().asInt64(), 15556000981LL);

    // Local times not supported
    // swull = dateFromISOString("1970-01-01T00:00:00.001");
    // ASSERT_OK(swull.getStatus());
    // ASSERT_EQUALS(swull.getValue().asInt64(), 18000001LL);

    // swull = dateFromISOString("1970-01-01T00:00:00.01");
    // ASSERT_OK(swull.getStatus());
    // ASSERT_EQUALS(swull.getValue().asInt64(), 18000010LL);

    // swull = dateFromISOString("1970-01-01T00:00:00.1");
    // ASSERT_OK(swull.getStatus());
    // ASSERT_EQUALS(swull.getValue().asInt64(), 18000100LL);

    // swull = dateFromISOString("1970-01-01T00:00:01");
    // ASSERT_OK(swull.getStatus());
    // ASSERT_EQUALS(swull.getValue().asInt64(), 18001000LL);

    // swull = dateFromISOString("1970-01-01T00:01");
    // ASSERT_OK(swull.getStatus());
    // ASSERT_EQUALS(swull.getValue().asInt64(), 18060000LL);

    swull = dateFromISOString("1970-06-29T21:06:40.981-0400");
    ASSERT_OK(swull.getStatus());
    ASSERT_EQUALS(swull.getValue().asInt64(), 15556000981LL);

    if (!isTimeTSmall) {
        swull = dateFromISOString("2058-02-20T13:29:11.100-0500");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 2781455351100LL);

        swull = dateFromISOString("3000-12-31T23:59:59Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 32535215999000LL);
    } else {
        swull = dateFromISOString("2038-01-19T03:14:07Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 2147483647000LL);
    }

    swull = dateFromISOString("2013-02-20T13:29:11.100-0500");
    ASSERT_OK(swull.getStatus());
    ASSERT_EQUALS(swull.getValue().asInt64(), 1361384951100LL);

    swull = dateFromISOString("2013-02-20T13:29:11.100-0501");
    ASSERT_OK(swull.getStatus());
    ASSERT_EQUALS(swull.getValue().asInt64(), 1361385011100LL);
}

TEST(TimeParsing, InvalidDates) {
    // Invalid decimal
    ASSERT_NOT_OK(dateFromISOString("1970-01-01T00:00:00.0.0Z").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-01T00:00:.0.000Z").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-01T00:.0:00.000Z").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-01T.0:00:00.000Z").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-.1T00:00:00.000Z").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-.1-01T00:00:00.000Z").getStatus());
    ASSERT_NOT_OK(dateFromISOString(".970-01-01T00:00:00.000Z").getStatus());

    // Extra sign characters
    ASSERT_NOT_OK(dateFromISOString("1970-01-01T00:00:00.+00Z").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-01T00:00:+0.000Z").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-01T00:+0:00.000Z").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-01T+0:00:00.000Z").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-+1T00:00:00.000Z").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-+1-01T00:00:00.000Z").getStatus());
    ASSERT_NOT_OK(dateFromISOString("+970-01-01T00:00:00.000Z").getStatus());

    ASSERT_NOT_OK(dateFromISOString("1970-01-01T00:00:00.-00Z").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-01T00:00:-0.000Z").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-01T00:-0:00.000Z").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-01T-0:00:00.000Z").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01--1T00:00:00.000Z").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970--1-01T00:00:00.000Z").getStatus());
    ASSERT_NOT_OK(dateFromISOString("-970-01-01T00:00:00.000Z").getStatus());

    // Out of range
    ASSERT_NOT_OK(dateFromISOString("1970-01-01T00:00:60.000Z").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-01T00:60:00.000Z").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-01T24:00:00.000Z").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-32T00:00:00.000Z").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-00T00:00:00.000Z").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-13-01T00:00:00.000Z").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-00-01T00:00:00.000Z").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1969-01-01T00:00:00.000Z").getStatus());

    // Invalid lengths
    ASSERT_NOT_OK(dateFromISOString("01970-01-01T00:00:00.000Z").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-001-01T00:00:00.000Z").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-001T00:00:00.000Z").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-01T000:00:00.000Z").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-01T00:000:00.000Z").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-01T00:00:000.000Z").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-01T00:00:00.0000Z").getStatus());
    ASSERT_NOT_OK(dateFromISOString("197-01-01T00:00:00.000Z").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-1-01T00:00:00.000Z").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-1T00:00:00.000Z").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-01T0:00:00.000Z").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-01T00:0:00.000Z").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-01T00:00:0.000Z").getStatus());

    // Invalid delimiters
    ASSERT_NOT_OK(dateFromISOString("1970+01-01T00:00:00.000Z").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01+01T00:00:00.000Z").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-01Q00:00:00.000Z").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-01T00-00:00.000Z").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-01T00:00-00.000Z").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-01T00:00:00-000Z").getStatus());

    // Missing numbers
    ASSERT_NOT_OK(dateFromISOString("1970--01T00:00:00.000Z").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-T00:00:00.000Z").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-01T:00:00.000Z").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-01T00::00.000Z").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-01T00:00:.000Z").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-01T00:00:00.Z").getStatus());

    // Bad time offset field
    ASSERT_NOT_OK(dateFromISOString("1970-01-01T05:00:01ZZ").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-01T05:00:01+").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-01T05:00:01-").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-01T05:00:01-11111").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-01T05:00:01Z1111").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-01T05:00:01+111").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-01T05:00:01+1160").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-01T05:00:01+2400").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-01T05:00:01+00+0").getStatus());

    // Bad prefixes
    ASSERT_NOT_OK(dateFromISOString("1970-01-01T05:00:01.").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-01T05:00:").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-01T05:").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-01T").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-01T05+0500").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-01+0500").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01+0500").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970+0500").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-01T01Z").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-01Z").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01Z").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970Z").getStatus());

    // No local time
    ASSERT_NOT_OK(dateFromISOString("1970-01-01T00:00:00.000").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-01T00:00:00").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-01T00:00").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-01T00").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-01").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970").getStatus());

    // Invalid hex base specifiers
    ASSERT_NOT_OK(dateFromISOString("x970-01-01T00:00:00.000Z").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-x1-01T00:00:00.000Z").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-x1T00:00:00.000Z").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-01Tx0:00:00.000Z").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-01T00:x0:00.000Z").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-01T00:00:x0.000Z").getStatus());
    ASSERT_NOT_OK(dateFromISOString("1970-01-01T00:00:00.x00Z").getStatus());
}

TEST(TimeParsing, LeapYears) {
    StatusWith<Date_t> swull = dateFromISOString("1972-02-29T00:00:00.000Z");
    ASSERT_OK(swull.getStatus());
    ASSERT_EQUALS(swull.getValue().asInt64(), 68169600000LL);

    swull = dateFromISOString("1976-02-29T00:00:00.000Z");
    ASSERT_OK(swull.getStatus());
    ASSERT_EQUALS(swull.getValue().asInt64(), 194400000000LL);

    swull = dateFromISOString("1980-02-29T00:00:00.000Z");
    ASSERT_OK(swull.getStatus());
    ASSERT_EQUALS(swull.getValue().asInt64(), 320630400000LL);

    swull = dateFromISOString("1984-02-29T00:00:00.000Z");
    ASSERT_OK(swull.getStatus());
    ASSERT_EQUALS(swull.getValue().asInt64(), 446860800000LL);

    swull = dateFromISOString("1988-02-29T00:00:00.000Z");
    ASSERT_OK(swull.getStatus());
    ASSERT_EQUALS(swull.getValue().asInt64(), 573091200000LL);

    swull = dateFromISOString("1992-02-29T00:00:00.000Z");
    ASSERT_OK(swull.getStatus());
    ASSERT_EQUALS(swull.getValue().asInt64(), 699321600000LL);

    swull = dateFromISOString("1996-02-29T00:00:00.000Z");
    ASSERT_OK(swull.getStatus());
    ASSERT_EQUALS(swull.getValue().asInt64(), 825552000000LL);

    swull = dateFromISOString("2000-02-29T00:00:00.000Z");
    ASSERT_OK(swull.getStatus());
    ASSERT_EQUALS(swull.getValue().asInt64(), 951782400000LL);

    swull = dateFromISOString("2004-02-29T00:00:00.000Z");
    ASSERT_OK(swull.getStatus());
    ASSERT_EQUALS(swull.getValue().asInt64(), 1078012800000LL);

    swull = dateFromISOString("2008-02-29T00:00:00.000Z");
    ASSERT_OK(swull.getStatus());
    ASSERT_EQUALS(swull.getValue().asInt64(), 1204243200000LL);

    swull = dateFromISOString("2012-02-29T00:00:00.000Z");
    ASSERT_OK(swull.getStatus());
    ASSERT_EQUALS(swull.getValue().asInt64(), 1330473600000LL);

    swull = dateFromISOString("2016-02-29T00:00:00.000Z");
    ASSERT_OK(swull.getStatus());
    ASSERT_EQUALS(swull.getValue().asInt64(), 1456704000000LL);

    swull = dateFromISOString("2020-02-29T00:00:00.000Z");
    ASSERT_OK(swull.getStatus());
    ASSERT_EQUALS(swull.getValue().asInt64(), 1582934400000LL);

    swull = dateFromISOString("2024-02-29T00:00:00.000Z");
    ASSERT_OK(swull.getStatus());
    ASSERT_EQUALS(swull.getValue().asInt64(), 1709164800000LL);

    swull = dateFromISOString("2028-02-29T00:00:00.000Z");
    ASSERT_OK(swull.getStatus());
    ASSERT_EQUALS(swull.getValue().asInt64(), 1835395200000LL);

    swull = dateFromISOString("2032-02-29T00:00:00.000Z");
    ASSERT_OK(swull.getStatus());
    ASSERT_EQUALS(swull.getValue().asInt64(), 1961625600000LL);

    swull = dateFromISOString("2036-02-29T00:00:00.000Z");
    ASSERT_OK(swull.getStatus());
    ASSERT_EQUALS(swull.getValue().asInt64(), 2087856000000LL);

    if (!isTimeTSmall) {
        swull = dateFromISOString("2040-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 2214086400000LL);

        swull = dateFromISOString("2044-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 2340316800000LL);

        swull = dateFromISOString("2048-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 2466547200000LL);

        swull = dateFromISOString("2052-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 2592777600000LL);

        swull = dateFromISOString("2056-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 2719008000000LL);

        swull = dateFromISOString("2060-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 2845238400000LL);

        swull = dateFromISOString("2064-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 2971468800000LL);

        swull = dateFromISOString("2068-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 3097699200000LL);

        swull = dateFromISOString("2072-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 3223929600000LL);

        swull = dateFromISOString("2076-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 3350160000000LL);

        swull = dateFromISOString("2080-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 3476390400000LL);

        swull = dateFromISOString("2084-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 3602620800000LL);

        swull = dateFromISOString("2088-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 3728851200000LL);

        swull = dateFromISOString("2092-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 3855081600000LL);

        swull = dateFromISOString("2096-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 3981312000000LL);

        swull = dateFromISOString("2104-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 4233686400000LL);

        swull = dateFromISOString("2108-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 4359916800000LL);

        swull = dateFromISOString("2112-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 4486147200000LL);

        swull = dateFromISOString("2116-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 4612377600000LL);

        swull = dateFromISOString("2120-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 4738608000000LL);

        swull = dateFromISOString("2124-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 4864838400000LL);

        swull = dateFromISOString("2128-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 4991068800000LL);

        swull = dateFromISOString("2132-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 5117299200000LL);

        swull = dateFromISOString("2136-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 5243529600000LL);

        swull = dateFromISOString("2140-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 5369760000000LL);

        swull = dateFromISOString("2144-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 5495990400000LL);

        swull = dateFromISOString("2148-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 5622220800000LL);

        swull = dateFromISOString("2152-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 5748451200000LL);

        swull = dateFromISOString("2156-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 5874681600000LL);

        swull = dateFromISOString("2160-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 6000912000000LL);

        swull = dateFromISOString("2164-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 6127142400000LL);

        swull = dateFromISOString("2168-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 6253372800000LL);

        swull = dateFromISOString("2172-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 6379603200000LL);

        swull = dateFromISOString("2176-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 6505833600000LL);

        swull = dateFromISOString("2180-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 6632064000000LL);

        swull = dateFromISOString("2184-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 6758294400000LL);

        swull = dateFromISOString("2188-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 6884524800000LL);

        swull = dateFromISOString("2192-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 7010755200000LL);

        swull = dateFromISOString("2196-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 7136985600000LL);

        swull = dateFromISOString("2204-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 7389360000000LL);

        swull = dateFromISOString("2208-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 7515590400000LL);

        swull = dateFromISOString("2212-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 7641820800000LL);

        swull = dateFromISOString("2216-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 7768051200000LL);

        swull = dateFromISOString("2220-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 7894281600000LL);

        swull = dateFromISOString("2224-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 8020512000000LL);

        swull = dateFromISOString("2228-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 8146742400000LL);

        swull = dateFromISOString("2232-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 8272972800000LL);

        swull = dateFromISOString("2236-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 8399203200000LL);

        swull = dateFromISOString("2240-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 8525433600000LL);

        swull = dateFromISOString("2244-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 8651664000000LL);

        swull = dateFromISOString("2248-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 8777894400000LL);

        swull = dateFromISOString("2252-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 8904124800000LL);

        swull = dateFromISOString("2256-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 9030355200000LL);

        swull = dateFromISOString("2260-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 9156585600000LL);

        swull = dateFromISOString("2264-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 9282816000000LL);

        swull = dateFromISOString("2268-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 9409046400000LL);

        swull = dateFromISOString("2272-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 9535276800000LL);

        swull = dateFromISOString("2276-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 9661507200000LL);

        swull = dateFromISOString("2280-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 9787737600000LL);

        swull = dateFromISOString("2284-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 9913968000000LL);

        swull = dateFromISOString("2288-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 10040198400000LL);

        swull = dateFromISOString("2292-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 10166428800000LL);

        swull = dateFromISOString("2296-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 10292659200000LL);

        swull = dateFromISOString("2304-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 10545033600000LL);

        swull = dateFromISOString("2308-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 10671264000000LL);

        swull = dateFromISOString("2312-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 10797494400000LL);

        swull = dateFromISOString("2316-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 10923724800000LL);

        swull = dateFromISOString("2320-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 11049955200000LL);

        swull = dateFromISOString("2324-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 11176185600000LL);

        swull = dateFromISOString("2328-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 11302416000000LL);

        swull = dateFromISOString("2332-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 11428646400000LL);

        swull = dateFromISOString("2336-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 11554876800000LL);

        swull = dateFromISOString("2340-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 11681107200000LL);

        swull = dateFromISOString("2344-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 11807337600000LL);

        swull = dateFromISOString("2348-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 11933568000000LL);

        swull = dateFromISOString("2352-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 12059798400000LL);

        swull = dateFromISOString("2356-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 12186028800000LL);

        swull = dateFromISOString("2360-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 12312259200000LL);

        swull = dateFromISOString("2364-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 12438489600000LL);

        swull = dateFromISOString("2368-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 12564720000000LL);

        swull = dateFromISOString("2372-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 12690950400000LL);

        swull = dateFromISOString("2376-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 12817180800000LL);

        swull = dateFromISOString("2380-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 12943411200000LL);

        swull = dateFromISOString("2384-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 13069641600000LL);

        swull = dateFromISOString("2388-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 13195872000000LL);

        swull = dateFromISOString("2392-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 13322102400000LL);

        swull = dateFromISOString("2396-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 13448332800000LL);

        swull = dateFromISOString("2400-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue().asInt64(), 13574563200000LL);
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

TEST(SystemTime, ConvertDateToSystemTime) {
    const std::string isoTimeString = "2015-05-14T17:28:33.123Z";
    const Date_t aDate = unittest::assertGet(dateFromISOString(isoTimeString));
    const auto aTimePoint = aDate.toSystemTimePoint();
    const auto actual = aTimePoint - stdx::chrono::system_clock::from_time_t(0);
    ASSERT(aDate.toDurationSinceEpoch().toSystemDuration() == actual)
        << "Expected " << aDate << "; but found " << Date_t::fromDurationSinceEpoch(actual);
    ASSERT_EQUALS(aDate, Date_t(aTimePoint));
}

}  // namespace
}  // namespace mongo
