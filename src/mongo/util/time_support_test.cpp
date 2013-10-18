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
