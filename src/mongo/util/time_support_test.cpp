/*    Copyright 2013 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
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

    TEST(TimeFormatting, DateAsISO8601UTC) {
        ASSERT_EQUALS(std::string("1970-01-01T00:00:00.000Z"),
                      dateToISOStringUTC(Date_t(0)));
        ASSERT_EQUALS(std::string("1970-06-30T01:06:40.981Z"),
                      dateToISOStringUTC(Date_t(15556000981ULL)));
        if (!isTimeTSmall)
            ASSERT_EQUALS(std::string("2058-02-20T18:29:11.100Z"),
                          dateToISOStringUTC(Date_t(2781455351100ULL)));
        ASSERT_EQUALS(std::string("2013-02-20T18:29:11.100Z"),
                      dateToISOStringUTC(Date_t(1361384951100ULL)));

        // Basic test
#ifndef _WIN32 // Negative Dates don't currently work on Windows
        ASSERT_EQUALS(std::string("1960-01-02T03:04:05.006Z"),
                      dateToISOStringUTC(Date_t(-315521754994LL)));
#endif

        // Testing special rounding rules for seconds
#ifndef _WIN32 // Negative Dates don't currently work on Windows
        ASSERT_EQUALS(std::string("1960-01-02T03:04:04.999Z"),
                      dateToISOStringUTC(Date_t(-315521755001LL))); // second = 4
        ASSERT_EQUALS(std::string("1960-01-02T03:04:05.000Z"),
                      dateToISOStringUTC(Date_t(-315521755000LL))); // second = 5
        ASSERT_EQUALS(std::string("1960-01-02T03:04:05.001Z"),
                      dateToISOStringUTC(Date_t(-315521754999LL))); // second = 5
        ASSERT_EQUALS(std::string("1960-01-02T03:04:05.999Z"),
                      dateToISOStringUTC(Date_t(-315521754001LL))); // second = 5
#endif

        // Test date before 1900 (negative tm_year values from gmtime)
#ifndef _WIN32 // Negative Dates don't currently work on Windows
        if (!isTimeTSmall)
            ASSERT_EQUALS(std::string("1860-01-02T03:04:05.006Z"),
                          dateToISOStringUTC(Date_t(-3471195354994LL)));
#endif

        // Test with time_t == -1
#ifndef _WIN32 // Negative Dates don't currently work on Windows
        ASSERT_EQUALS(std::string("1969-12-31T23:59:59.000Z"),
                      dateToISOStringUTC(Date_t(-1000LL)));
#endif

        // Testing dates between 1970 and 2000
        ASSERT_EQUALS(std::string("1970-01-01T00:00:00.000Z"),
                      dateToISOStringUTC(Date_t(0ULL)));
        ASSERT_EQUALS(std::string("1970-01-01T00:00:00.999Z"),
                      dateToISOStringUTC(Date_t(999ULL)));
        ASSERT_EQUALS(std::string("1980-05-20T12:54:04.834Z"),
                      dateToISOStringUTC(Date_t(327675244834ULL)));
        ASSERT_EQUALS(std::string("1999-12-31T00:00:00.000Z"),
                      dateToISOStringUTC(Date_t(946598400000ULL)));
        ASSERT_EQUALS(std::string("1999-12-31T23:59:59.999Z"),
                      dateToISOStringUTC(Date_t(946684799999ULL)));

        // Test date > 2000 for completeness (using now)
        ASSERT_EQUALS(std::string("2013-10-11T23:20:12.072Z"),
                      dateToISOStringUTC(Date_t(1381533612072ULL)));
    }

    TEST(TimeFormatting, DateAsISO8601Local) {
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

    TEST(TimeFormatting, DateAsCtime) {
        ASSERT_EQUALS(std::string("Wed Dec 31 19:00:00.000"), dateToCtimeString(Date_t(0)));
        ASSERT_EQUALS(std::string("Mon Jun 29 21:06:40.981"),
                      dateToCtimeString(Date_t(15556000981ULL)));
        if (!isTimeTSmall)
            ASSERT_EQUALS(std::string("Wed Feb 20 13:29:11.100"),
                          dateToCtimeString(Date_t(2781455351100ULL)));
        ASSERT_EQUALS(std::string("Wed Feb 20 13:29:11.100"),
                      dateToCtimeString(Date_t(1361384951100ULL)));
    }

}  // namespace
}  // namespace mongo
