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

#include <cstdlib>
#include <ctime>
#include <string>

#include "mongo/base/init.h"
#include "mongo/bson/util/misc.h"
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

    TEST(TimeFormatting, TimeTAsISO8601Zulu) {
        ASSERT_EQUALS(std::string("1970-01-01T00:00:00Z"), timeToISOString(0));
        ASSERT_EQUALS(std::string("1970-06-30T01:06:40Z"), timeToISOString(15556000));
        if (!isTimeTSmall)
            ASSERT_EQUALS(std::string("2058-02-20T18:29:11Z"), timeToISOString(2781455351LL));
        ASSERT_EQUALS(std::string("2013-02-20T18:29:11Z"), timeToISOString(1361384951));
    }

    TEST(TimeFormatting, DateAsISO8601UTCString) {
        ASSERT_EQUALS(std::string("1970-01-01T00:00:00.000Z"),
                      dateToISOStringUTC(Date_t(0)));
        ASSERT_EQUALS(std::string("1970-06-30T01:06:40.981Z"),
                      dateToISOStringUTC(Date_t(15556000981ULL)));
        if (!isTimeTSmall)
            ASSERT_EQUALS(std::string("2058-02-20T18:29:11.100Z"),
                          dateToISOStringUTC(Date_t(2781455351100ULL)));
        ASSERT_EQUALS(std::string("2013-02-20T18:29:11.100Z"),
                      dateToISOStringUTC(Date_t(1361384951100ULL)));
    }

    TEST(TimeFormatting, DateAsISO8601LocalString) {
        ASSERT_EQUALS(std::string("1969-12-31T19:00:00.000-0500"),
                      dateToISOStringLocal(Date_t(0)));
        ASSERT_EQUALS(std::string("1970-06-29T21:06:40.981-0400"),
                      dateToISOStringLocal(Date_t(15556000981ULL)));
        if (!isTimeTSmall)
            ASSERT_EQUALS(std::string("2058-02-20T13:29:11.100-0500"),
                          dateToISOStringLocal(Date_t(2781455351100ULL)));
        ASSERT_EQUALS(std::string("2013-02-20T13:29:11.100-0500"),
                      dateToISOStringLocal(Date_t(1361384951100ULL)));
    }

    TEST(TimeFormatting, DateAsCtimeString) {
        ASSERT_EQUALS(std::string("Wed Dec 31 19:00:00.000"), dateToCtimeString(Date_t(0)));
        ASSERT_EQUALS(std::string("Mon Jun 29 21:06:40.981"),
                      dateToCtimeString(Date_t(15556000981ULL)));
        if (!isTimeTSmall)
            ASSERT_EQUALS(std::string("Wed Feb 20 13:29:11.100"),
                          dateToCtimeString(Date_t(2781455351100ULL)));
        ASSERT_EQUALS(std::string("Wed Feb 20 13:29:11.100"),
                      dateToCtimeString(Date_t(1361384951100ULL)));
    }

    static std::string stringstreamDate(void (*formatter)(std::ostream&, Date_t), Date_t date) {
        std::ostringstream os;
        formatter(os, date);
        return os.str();
    }

    TEST(TimeFormatting, DateAsISO8601UTCStream) {
        ASSERT_EQUALS(std::string("1970-01-01T00:00:00.000Z"),
                      stringstreamDate(outputDateAsISOStringUTC, Date_t(0)));
        ASSERT_EQUALS(std::string("1970-06-30T01:06:40.981Z"),
                      stringstreamDate(outputDateAsISOStringUTC, Date_t(15556000981ULL)));
        if (!isTimeTSmall)
            ASSERT_EQUALS(std::string("2058-02-20T18:29:11.100Z"),
                          stringstreamDate(outputDateAsISOStringUTC, Date_t(2781455351100ULL)));
        ASSERT_EQUALS(std::string("2013-02-20T18:29:11.100Z"),
                      stringstreamDate(outputDateAsISOStringUTC, Date_t(1361384951100ULL)));
    }

    TEST(TimeFormatting, DateAsISO8601LocalStream) {
        ASSERT_EQUALS(std::string("1969-12-31T19:00:00.000-0500"),
                      stringstreamDate(outputDateAsISOStringLocal, Date_t(0)));
        ASSERT_EQUALS(std::string("1970-06-29T21:06:40.981-0400"),
                      stringstreamDate(outputDateAsISOStringLocal, Date_t(15556000981ULL)));
        if (!isTimeTSmall)
            ASSERT_EQUALS(std::string("2058-02-20T13:29:11.100-0500"),
                          stringstreamDate(outputDateAsISOStringLocal, Date_t(2781455351100ULL)));
        ASSERT_EQUALS(std::string("2013-02-20T13:29:11.100-0500"),
                      stringstreamDate(outputDateAsISOStringLocal, Date_t(1361384951100ULL)));
    }

    TEST(TimeFormatting, DateAsCtimeStream) {
        ASSERT_EQUALS(std::string("Wed Dec 31 19:00:00.000"),
                      stringstreamDate(outputDateAsCtime, Date_t(0)));
        ASSERT_EQUALS(std::string("Mon Jun 29 21:06:40.981"),
                      stringstreamDate(outputDateAsCtime, Date_t(15556000981ULL)));
        if (!isTimeTSmall)
            ASSERT_EQUALS(std::string("Wed Feb 20 13:29:11.100"),
                          stringstreamDate(outputDateAsCtime, Date_t(2781455351100ULL)));
        ASSERT_EQUALS(std::string("Wed Feb 20 13:29:11.100"),
                      stringstreamDate(outputDateAsCtime, Date_t(1361384951100ULL)));
    }

    TEST(TimeParsing, DateAsISO8601UTC) {
        // Allowed date format:
        // YYYY-MM-DDTHH:MM[:SS[.m[m[m]]]]Z
        // Year, month, day, hour, and minute are required, while the seconds component and one to
        // three milliseconds are optional.

        StatusWith<Date_t> swull = dateFromISOString("1971-02-03T04:05:06.789Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue(), 34401906789ULL);

        swull = dateFromISOString("1971-02-03T04:05:06.78Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue(), 34401906780ULL);

        swull = dateFromISOString("1971-02-03T04:05:06.7Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue(), 34401906700ULL);

        swull = dateFromISOString("1971-02-03T04:05:06Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue(), 34401906000ULL);

        swull = dateFromISOString("1971-02-03T04:05Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue(), 34401900000ULL);

        swull = dateFromISOString("1970-01-01T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue(), 0ULL);

        swull = dateFromISOString("1970-06-30T01:06:40.981Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue(), 15556000981ULL);

        if (!isTimeTSmall) {
            swull = dateFromISOString("2058-02-20T18:29:11.100Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 2781455351100ULL);

            swull = dateFromISOString("3001-01-01T08:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 32535244800000ULL);
        }

        swull = dateFromISOString("2013-02-20T18:29:11.100Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue(), 1361384951100ULL);
    }

    TEST(TimeParsing, DateAsISO8601Local) {
        // Allowed date format:
        // YYYY-MM-DDTHH:MM[:SS[.m[m[m]]]]+HHMM
        // Year, month, day, hour, and minute are required, while the seconds component and one to
        // three milliseconds are optional.  The time zone offset must be four digits.

        StatusWith<Date_t> swull = dateFromISOString("1971-02-03T09:16:06.789+0511");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue(), 34401906789ULL);

        swull = dateFromISOString("1971-02-03T09:16:06.78+0511");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue(), 34401906780ULL);

        swull = dateFromISOString("1971-02-03T09:16:06.7+0511");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue(), 34401906700ULL);

        swull = dateFromISOString("1971-02-03T09:16:06+0511");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue(), 34401906000ULL);

        swull = dateFromISOString("1971-02-03T09:16+0511");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue(), 34401900000ULL);

        swull = dateFromISOString("1970-01-01T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue(), 0ULL);

        swull = dateFromISOString("1970-06-30T01:06:40.981Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue(), 15556000981ULL);

        // Local times not supported
        //swull = dateFromISOString("1970-01-01T00:00:00.001");
        //ASSERT_OK(swull.getStatus());
        //ASSERT_EQUALS(swull.getValue(), 18000001ULL);

        //swull = dateFromISOString("1970-01-01T00:00:00.01");
        //ASSERT_OK(swull.getStatus());
        //ASSERT_EQUALS(swull.getValue(), 18000010ULL);

        //swull = dateFromISOString("1970-01-01T00:00:00.1");
        //ASSERT_OK(swull.getStatus());
        //ASSERT_EQUALS(swull.getValue(), 18000100ULL);

        //swull = dateFromISOString("1970-01-01T00:00:01");
        //ASSERT_OK(swull.getStatus());
        //ASSERT_EQUALS(swull.getValue(), 18001000ULL);

        //swull = dateFromISOString("1970-01-01T00:01");
        //ASSERT_OK(swull.getStatus());
        //ASSERT_EQUALS(swull.getValue(), 18060000ULL);

        swull = dateFromISOString("1970-06-29T21:06:40.981-0400");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue(), 15556000981ULL);

        if (!isTimeTSmall) {
            swull = dateFromISOString("2058-02-20T13:29:11.100-0500");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 2781455351100ULL);

            swull = dateFromISOString("3000-12-31T23:59:59Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 32535215999000ULL);
        }
        else {
            swull = dateFromISOString("2038-01-19T03:14:07Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 2147483647000ULL);
        }

        swull = dateFromISOString("2013-02-20T13:29:11.100-0500");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue(), 1361384951100ULL);

        swull = dateFromISOString("2013-02-20T13:29:11.100-0501");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue(), 1361385011100ULL);
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
        ASSERT_EQUALS(swull.getValue(), 68169600000ULL);

        swull = dateFromISOString("1976-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue(), 194400000000ULL);

        swull = dateFromISOString("1980-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue(), 320630400000ULL);

        swull = dateFromISOString("1984-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue(), 446860800000ULL);

        swull = dateFromISOString("1988-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue(), 573091200000ULL);

        swull = dateFromISOString("1992-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue(), 699321600000ULL);

        swull = dateFromISOString("1996-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue(), 825552000000ULL);

        swull = dateFromISOString("2000-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue(), 951782400000ULL);

        swull = dateFromISOString("2004-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue(), 1078012800000ULL);

        swull = dateFromISOString("2008-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue(), 1204243200000ULL);

        swull = dateFromISOString("2012-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue(), 1330473600000ULL);

        swull = dateFromISOString("2016-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue(), 1456704000000ULL);

        swull = dateFromISOString("2020-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue(), 1582934400000ULL);

        swull = dateFromISOString("2024-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue(), 1709164800000ULL);

        swull = dateFromISOString("2028-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue(), 1835395200000ULL);

        swull = dateFromISOString("2032-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue(), 1961625600000ULL);

        swull = dateFromISOString("2036-02-29T00:00:00.000Z");
        ASSERT_OK(swull.getStatus());
        ASSERT_EQUALS(swull.getValue(), 2087856000000ULL);

        if (!isTimeTSmall) {
            swull = dateFromISOString("2040-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 2214086400000ULL);

            swull = dateFromISOString("2044-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 2340316800000ULL);

            swull = dateFromISOString("2048-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 2466547200000ULL);

            swull = dateFromISOString("2052-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 2592777600000ULL);

            swull = dateFromISOString("2056-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 2719008000000ULL);

            swull = dateFromISOString("2060-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 2845238400000ULL);

            swull = dateFromISOString("2064-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 2971468800000ULL);

            swull = dateFromISOString("2068-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 3097699200000ULL);

            swull = dateFromISOString("2072-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 3223929600000ULL);

            swull = dateFromISOString("2076-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 3350160000000ULL);

            swull = dateFromISOString("2080-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 3476390400000ULL);

            swull = dateFromISOString("2084-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 3602620800000ULL);

            swull = dateFromISOString("2088-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 3728851200000ULL);

            swull = dateFromISOString("2092-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 3855081600000ULL);

            swull = dateFromISOString("2096-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 3981312000000ULL);

            swull = dateFromISOString("2104-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 4233686400000ULL);

            swull = dateFromISOString("2108-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 4359916800000ULL);

            swull = dateFromISOString("2112-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 4486147200000ULL);

            swull = dateFromISOString("2116-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 4612377600000ULL);

            swull = dateFromISOString("2120-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 4738608000000ULL);

            swull = dateFromISOString("2124-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 4864838400000ULL);

            swull = dateFromISOString("2128-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 4991068800000ULL);

            swull = dateFromISOString("2132-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 5117299200000ULL);

            swull = dateFromISOString("2136-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 5243529600000ULL);

            swull = dateFromISOString("2140-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 5369760000000ULL);

            swull = dateFromISOString("2144-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 5495990400000ULL);

            swull = dateFromISOString("2148-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 5622220800000ULL);

            swull = dateFromISOString("2152-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 5748451200000ULL);

            swull = dateFromISOString("2156-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 5874681600000ULL);

            swull = dateFromISOString("2160-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 6000912000000ULL);

            swull = dateFromISOString("2164-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 6127142400000ULL);

            swull = dateFromISOString("2168-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 6253372800000ULL);

            swull = dateFromISOString("2172-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 6379603200000ULL);

            swull = dateFromISOString("2176-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 6505833600000ULL);

            swull = dateFromISOString("2180-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 6632064000000ULL);

            swull = dateFromISOString("2184-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 6758294400000ULL);

            swull = dateFromISOString("2188-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 6884524800000ULL);

            swull = dateFromISOString("2192-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 7010755200000ULL);

            swull = dateFromISOString("2196-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 7136985600000ULL);

            swull = dateFromISOString("2204-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 7389360000000ULL);

            swull = dateFromISOString("2208-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 7515590400000ULL);

            swull = dateFromISOString("2212-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 7641820800000ULL);

            swull = dateFromISOString("2216-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 7768051200000ULL);

            swull = dateFromISOString("2220-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 7894281600000ULL);

            swull = dateFromISOString("2224-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 8020512000000ULL);

            swull = dateFromISOString("2228-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 8146742400000ULL);

            swull = dateFromISOString("2232-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 8272972800000ULL);

            swull = dateFromISOString("2236-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 8399203200000ULL);

            swull = dateFromISOString("2240-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 8525433600000ULL);

            swull = dateFromISOString("2244-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 8651664000000ULL);

            swull = dateFromISOString("2248-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 8777894400000ULL);

            swull = dateFromISOString("2252-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 8904124800000ULL);

            swull = dateFromISOString("2256-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 9030355200000ULL);

            swull = dateFromISOString("2260-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 9156585600000ULL);

            swull = dateFromISOString("2264-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 9282816000000ULL);

            swull = dateFromISOString("2268-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 9409046400000ULL);

            swull = dateFromISOString("2272-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 9535276800000ULL);

            swull = dateFromISOString("2276-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 9661507200000ULL);

            swull = dateFromISOString("2280-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 9787737600000ULL);

            swull = dateFromISOString("2284-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 9913968000000ULL);

            swull = dateFromISOString("2288-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 10040198400000ULL);

            swull = dateFromISOString("2292-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 10166428800000ULL);

            swull = dateFromISOString("2296-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 10292659200000ULL);

            swull = dateFromISOString("2304-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 10545033600000ULL);

            swull = dateFromISOString("2308-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 10671264000000ULL);

            swull = dateFromISOString("2312-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 10797494400000ULL);

            swull = dateFromISOString("2316-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 10923724800000ULL);

            swull = dateFromISOString("2320-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 11049955200000ULL);

            swull = dateFromISOString("2324-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 11176185600000ULL);

            swull = dateFromISOString("2328-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 11302416000000ULL);

            swull = dateFromISOString("2332-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 11428646400000ULL);

            swull = dateFromISOString("2336-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 11554876800000ULL);

            swull = dateFromISOString("2340-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 11681107200000ULL);

            swull = dateFromISOString("2344-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 11807337600000ULL);

            swull = dateFromISOString("2348-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 11933568000000ULL);

            swull = dateFromISOString("2352-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 12059798400000ULL);

            swull = dateFromISOString("2356-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 12186028800000ULL);

            swull = dateFromISOString("2360-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 12312259200000ULL);

            swull = dateFromISOString("2364-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 12438489600000ULL);

            swull = dateFromISOString("2368-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 12564720000000ULL);

            swull = dateFromISOString("2372-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 12690950400000ULL);

            swull = dateFromISOString("2376-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 12817180800000ULL);

            swull = dateFromISOString("2380-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 12943411200000ULL);

            swull = dateFromISOString("2384-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 13069641600000ULL);

            swull = dateFromISOString("2388-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 13195872000000ULL);

            swull = dateFromISOString("2392-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 13322102400000ULL);

            swull = dateFromISOString("2396-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 13448332800000ULL);

            swull = dateFromISOString("2400-02-29T00:00:00.000Z");
            ASSERT_OK(swull.getStatus());
            ASSERT_EQUALS(swull.getValue(), 13574563200000ULL);
        }
    }

}  // namespace
}  // namespace mongo
