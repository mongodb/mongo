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

}  // namespace
}  // namespace mongo
