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

#include <boost/date_time/posix_time/posix_time_types.hpp>
// IWYU pragma: no_include "cxxabi.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/sharding_environment/sharding_mongos_test_fixture.h"
#include "mongo/db/vector_clock/vector_clock.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/hostandport.h"

#include <memory>
#include <system_error>
#include <vector>

#include <boost/date_time/gregorian/greg_date.hpp>
#include <boost/date_time/posix_time/posix_time_duration.hpp>
#include <boost/date_time/posix_time/ptime.hpp>
#include <boost/date_time/time_duration.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

namespace mongo {
namespace {

using executor::RemoteCommandRequest;
using std::vector;
using unittest::assertGet;

boost::gregorian::date currentDate() {
    boost::posix_time::ptime now = boost::posix_time::second_clock::local_time();
    return now.date();
}
class BalancerSettingsTypeTestFixture : public ServiceContextTest {
public:
    OperationContext* opCtx() {
        if (!_opCtx) {
            _opCtx = makeOperationContext();
        }
        return _opCtx.get();
    }

protected:
    ServiceContext::UniqueOperationContext _opCtx;
};


class BalancerConfigurationTestFixture : public ShardingTestFixture {
protected:
    /**
     * Expects a correct find command to be dispatched for the config.settings namespace and returns
     * the specified result. If an empty boost::optional is passed, returns an empty results.
     */
    void expectSettingsQuery(StringData key, StatusWith<boost::optional<BSONObj>> result) {
        onFindCommand([&](const RemoteCommandRequest& request) {
            auto opMsg = static_cast<OpMsgRequest>(request);
            auto findCommand = query_request_helper::makeFromFindCommandForTests(opMsg.body);

            ASSERT_EQ(findCommand->getNamespaceOrUUID().nss(),
                      NamespaceString::kConfigSettingsNamespace);
            ASSERT_BSONOBJ_EQ(findCommand->getFilter(), BSON("_id" << key));

            checkReadConcern(request.cmdObj,
                             VectorClock::kInitialComponentTime.asTimestamp(),
                             repl::OpTime::kUninitializedTerm);

            if (!result.isOK()) {
                return StatusWith<vector<BSONObj>>(result.getStatus());
            }

            if (result.getValue()) {
                return StatusWith<vector<BSONObj>>(vector<BSONObj>{*(result.getValue())});
            }

            return StatusWith<vector<BSONObj>>(vector<BSONObj>{});
        });
    }
};

TEST_F(BalancerConfigurationTestFixture, NoConfigurationDocuments) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    BalancerConfiguration config;

    auto future = launchAsync([&] { ASSERT_OK(config.refreshAndCheck(operationContext())); });

    expectSettingsQuery(BalancerSettingsType::kKey, boost::optional<BSONObj>());
    expectSettingsQuery(ChunkSizeSettingsType::kKey, boost::optional<BSONObj>());
    expectSettingsQuery(AutoMergeSettingsType::kKey, boost::optional<BSONObj>());

    future.default_timed_get();

    ASSERT(config.shouldBalance(operationContext()));
    ASSERT_EQ(MigrationSecondaryThrottleOptions::kDefault,
              config.getSecondaryThrottle().getSecondaryThrottle());
    ASSERT_EQ(ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes, config.getMaxChunkSizeBytes());
}

TEST_F(BalancerConfigurationTestFixture, ChunkSizeSettingsDocumentOnly) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    BalancerConfiguration config;

    auto future = launchAsync([&] { ASSERT_OK(config.refreshAndCheck(operationContext())); });

    expectSettingsQuery(BalancerSettingsType::kKey, boost::optional<BSONObj>());
    expectSettingsQuery(ChunkSizeSettingsType::kKey, boost::optional<BSONObj>(BSON("value" << 3)));
    expectSettingsQuery(AutoMergeSettingsType::kKey, boost::optional<BSONObj>());

    future.default_timed_get();

    ASSERT(config.shouldBalance(operationContext()));
    ASSERT_EQ(MigrationSecondaryThrottleOptions::kDefault,
              config.getSecondaryThrottle().getSecondaryThrottle());
    ASSERT_EQ(3 * 1024 * 1024ULL, config.getMaxChunkSizeBytes());
}

TEST_F(BalancerConfigurationTestFixture, BalancerSettingsDocumentOnly) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    BalancerConfiguration config;

    auto future = launchAsync([&] { ASSERT_OK(config.refreshAndCheck(operationContext())); });

    expectSettingsQuery(BalancerSettingsType::kKey,
                        boost::optional<BSONObj>(BSON("stopped" << true)));
    expectSettingsQuery(ChunkSizeSettingsType::kKey, boost::optional<BSONObj>());
    expectSettingsQuery(AutoMergeSettingsType::kKey, boost::optional<BSONObj>());

    future.default_timed_get();

    ASSERT(!config.shouldBalance(operationContext()));
    ASSERT_EQ(MigrationSecondaryThrottleOptions::kDefault,
              config.getSecondaryThrottle().getSecondaryThrottle());
    ASSERT_EQ(ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes, config.getMaxChunkSizeBytes());
}

TEST_F(BalancerSettingsTypeTestFixture, Defaults) {
    BalancerSettingsType settings = assertGet(BalancerSettingsType::fromBSON(opCtx(), BSONObj()));
    ASSERT_EQ(BalancerSettingsType::kFull, settings.getMode());
    ASSERT_EQ(MigrationSecondaryThrottleOptions::kDefault,
              settings.getSecondaryThrottle().getSecondaryThrottle());
    ASSERT(!settings.getSecondaryThrottle().isWriteConcernSpecified());

    ASSERT(settings.isTimeInBalancingWindow(
        opCtx(),
        boost::posix_time::ptime(currentDate(),
                                 boost::posix_time::hours(12) + boost::posix_time::minutes(0))));
}

TEST_F(BalancerSettingsTypeTestFixture, BalancerDisabledThroughStoppedOption) {
    BalancerSettingsType settings =
        assertGet(BalancerSettingsType::fromBSON(opCtx(), BSON("stopped" << true)));
    ASSERT_EQ(BalancerSettingsType::kOff, settings.getMode());
}

TEST_F(BalancerSettingsTypeTestFixture, AllValidBalancerModeOptions) {
    ASSERT_EQ(BalancerSettingsType::kFull,
              assertGet(BalancerSettingsType::fromBSON(opCtx(), BSON("mode" << "full"))).getMode());
    ASSERT_EQ(BalancerSettingsType::kOff,
              assertGet(BalancerSettingsType::fromBSON(opCtx(), BSON("mode" << "off"))).getMode());
}

TEST_F(BalancerSettingsTypeTestFixture, InvalidBalancerModeOption) {
    unittest::LogCaptureGuard logs;
    ASSERT_EQ(BalancerSettingsType::kOff,
              assertGet(BalancerSettingsType::fromBSON(opCtx(), BSON("mode" << "BAD"))).getMode());
    logs.stop();
    ASSERT_EQ(1,
              logs.countTextContaining(
                  "Balancer turned off because currently set balancing mode is not valid"));
}


TEST_F(BalancerSettingsTypeTestFixture, BalancingWindowStartLessThanStop) {
    BalancerSettingsType settings =
        assertGet(BalancerSettingsType::fromBSON(opCtx(),
                                                 BSON("activeWindow" << BSON("start" << "9:00"
                                                                                     << "stop"
                                                                                     << "19:00"))));
    ASSERT(settings.isTimeInBalancingWindow(
        opCtx(),
        boost::posix_time::ptime(currentDate(),
                                 boost::posix_time::hours(9) + boost::posix_time::minutes(0))));
    ASSERT(settings.isTimeInBalancingWindow(
        opCtx(),
        boost::posix_time::ptime(currentDate(),
                                 boost::posix_time::hours(10) + boost::posix_time::minutes(30))));
    ASSERT(settings.isTimeInBalancingWindow(
        opCtx(),
        boost::posix_time::ptime(currentDate(),
                                 boost::posix_time::hours(19) + boost::posix_time::minutes(0))));

    ASSERT(!settings.isTimeInBalancingWindow(
        opCtx(),
        boost::posix_time::ptime(currentDate(),
                                 boost::posix_time::hours(8) + boost::posix_time::minutes(59))));
    ASSERT(!settings.isTimeInBalancingWindow(
        opCtx(),
        boost::posix_time::ptime(currentDate(),
                                 boost::posix_time::hours(19) + boost::posix_time::minutes(1))));
}

TEST_F(BalancerSettingsTypeTestFixture, BalancingWindowStopLessThanStart) {
    BalancerSettingsType settings =
        assertGet(BalancerSettingsType::fromBSON(opCtx(),
                                                 BSON("activeWindow" << BSON("start" << "23:00"
                                                                                     << "stop"
                                                                                     << "8:00"))));

    ASSERT(settings.isTimeInBalancingWindow(
        opCtx(),
        boost::posix_time::ptime(currentDate(),
                                 boost::posix_time::hours(23) + boost::posix_time::minutes(0))));
    ASSERT(settings.isTimeInBalancingWindow(
        opCtx(),
        boost::posix_time::ptime(currentDate(),
                                 boost::posix_time::hours(2) + boost::posix_time::minutes(30))));
    ASSERT(settings.isTimeInBalancingWindow(
        opCtx(),
        boost::posix_time::ptime(currentDate(),
                                 boost::posix_time::hours(7) + boost::posix_time::minutes(59))));

    ASSERT(!settings.isTimeInBalancingWindow(
        opCtx(),
        boost::posix_time::ptime(currentDate(),
                                 boost::posix_time::hours(8) + boost::posix_time::minutes(1))));
    ASSERT(!settings.isTimeInBalancingWindow(
        opCtx(),
        boost::posix_time::ptime(currentDate(),
                                 boost::posix_time::hours(22) + boost::posix_time::minutes(00))));
}

TEST_F(BalancerSettingsTypeTestFixture, InvalidBalancingWindowStartEqualsStop) {
    auto status = BalancerSettingsType::fromBSON(opCtx(),
                                                 BSON("activeWindow" << BSON("start" << "00:00"
                                                                                     << "stop"
                                                                                     << "00:00")))
                      .getStatus();
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
    ASSERT_EQ(status.reason(), "start and stop times must be different");
}

TEST_F(BalancerSettingsTypeTestFixture, InvalidBalancingWindowTimeFormat) {
    auto status1 = BalancerSettingsType::fromBSON(opCtx(),
                                                  BSON("activeWindow" << BSON("start" << "23"
                                                                                      << "stop"
                                                                                      << "6")))
                       .getStatus();
    ASSERT_NOT_OK(status1);
    ASSERT_EQUALS(ErrorCodes::BadValue, status1);
    ASSERT_EQ(status1.reason(), "activeWindow format is  { start: \"hh:mm\" , stop: \"hh:mm\" }");

    auto status2 =
        BalancerSettingsType::fromBSON(opCtx(),
                                       BSON("activeWindow" << BSON("start" << 23LL << "stop"
                                                                           << "6:00")))
            .getStatus();
    ASSERT_NOT_OK(status2);
    ASSERT_EQUALS(ErrorCodes::BadValue, status2);
    ASSERT_STRING_CONTAINS(status2.reason(),
                           "must specify both start and stop of balancing window");

    auto status3 =
        BalancerSettingsType::fromBSON(opCtx(),
                                       BSON("activeWindow" << BSON("start" << "23:00"
                                                                           << "stop" << 6LL)))
            .getStatus();
    ASSERT_NOT_OK(status3);
    ASSERT_EQUALS(ErrorCodes::BadValue, status3);
    ASSERT_STRING_CONTAINS(status3.reason(),
                           "must specify both start and stop of balancing window");
}

TEST_F(BalancerSettingsTypeTestFixture, InvalidBalancingWindowFormat) {
    auto status1 = BalancerSettingsType::fromBSON(opCtx(),
                                                  BSON("activeWindow" << BSON("begin" << "23:00"
                                                                                      << "stop"
                                                                                      << "6:00")))
                       .getStatus();
    ASSERT_NOT_OK(status1);
    ASSERT_EQUALS(ErrorCodes::BadValue, status1);
    ASSERT_STRING_CONTAINS(status1.reason(),
                           "must specify both start and stop of balancing window");

    auto status2 = BalancerSettingsType::fromBSON(opCtx(),
                                                  BSON("activeWindow" << BSON("start" << "23:00"
                                                                                      << "end"
                                                                                      << "6:00")))
                       .getStatus();
    ASSERT_NOT_OK(status2);
    ASSERT_EQUALS(ErrorCodes::BadValue, status2);
    ASSERT_STRING_CONTAINS(status2.reason(),
                           "must specify both start and stop of balancing window");
}

TEST(ChunkSizeSettingsType, NormalValues) {
    ASSERT_EQ(
        1024 * 1024ULL,
        assertGet(ChunkSizeSettingsType::fromBSON(BSON("value" << 1))).getMaxChunkSizeBytes());
    ASSERT_EQ(
        10 * 1024 * 1024ULL,
        assertGet(ChunkSizeSettingsType::fromBSON(BSON("value" << 10))).getMaxChunkSizeBytes());
    ASSERT_EQ(
        1024 * 1024 * 1024ULL,
        assertGet(ChunkSizeSettingsType::fromBSON(BSON("value" << 1024))).getMaxChunkSizeBytes());
}

TEST(ChunkSizeSettingsType, BackwardsCompatibilityDueToExtraKeys) {
    ASSERT_EQ(1024 * 1024ULL,
              assertGet(ChunkSizeSettingsType::fromBSON(BSON("value" << 1 << "SomeFutureKey"
                                                                     << "SomeFutureValue")))
                  .getMaxChunkSizeBytes());
}

TEST(ChunkSizeSettingsType, IllegalValues) {
    auto status1 = ChunkSizeSettingsType::fromBSON(BSON("value" << 0)).getStatus();
    ASSERT_NOT_OK(status1);
    ASSERT_EQUALS(ErrorCodes::BadValue, status1);
    ASSERT_STRING_CONTAINS(status1.reason(), "is not a valid value for chunksize");

    auto status2 = ChunkSizeSettingsType::fromBSON(BSON("value" << -1)).getStatus();
    ASSERT_NOT_OK(status2);
    ASSERT_EQUALS(ErrorCodes::BadValue, status2);
    ASSERT_STRING_CONTAINS(status2.reason(), "is not a valid value for chunksize");

    auto status3 = ChunkSizeSettingsType::fromBSON(BSON("value" << 1025)).getStatus();
    ASSERT_NOT_OK(status3);
    ASSERT_EQUALS(ErrorCodes::BadValue, status3);
    ASSERT_STRING_CONTAINS(status3.reason(), "is not a valid value for chunksize");

    auto status4 = ChunkSizeSettingsType::fromBSON(BSON("value" << "WrongType")).getStatus();
    ASSERT_NOT_OK(status4);
    ASSERT_EQUALS(ErrorCodes::TypeMismatch, status4);

    auto status5 = ChunkSizeSettingsType::fromBSON(BSON("IllegalKey" << 1)).getStatus();
    ASSERT_NOT_OK(status5);
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, status5);
}

TEST_F(BalancerSettingsTypeTestFixture, BalancingWindowDOWSingleDay) {
    RAIIServerParameterControllerForTest featureFlagController("featureFlagBalancerWindowDOW",
                                                               true);
    BalancerSettingsType settings = assertGet(BalancerSettingsType::fromBSON(
        opCtx(),
        BSON("activeWindowDOW" << BSON_ARRAY(BSON("day" << "Monday"
                                                        << "start"
                                                        << "9:00"
                                                        << "stop"
                                                        << "17:00")))));

    auto today = currentDate();
    bool isMondayToday = (today.day_of_week() == 1);
    if (isMondayToday) {
        ASSERT(settings.isTimeInBalancingWindow(
            opCtx(),
            boost::posix_time::ptime(currentDate(),
                                     boost::posix_time::hours(9) + boost::posix_time::minutes(0))));
        ASSERT(settings.isTimeInBalancingWindow(
            opCtx(),
            boost::posix_time::ptime(
                currentDate(), boost::posix_time::hours(12) + boost::posix_time::minutes(0))));
        ASSERT(settings.isTimeInBalancingWindow(
            opCtx(),
            boost::posix_time::ptime(
                currentDate(), boost::posix_time::hours(17) + boost::posix_time::minutes(0))));

        ASSERT(!settings.isTimeInBalancingWindow(
            opCtx(),
            boost::posix_time::ptime(
                currentDate(), boost::posix_time::hours(8) + boost::posix_time::minutes(59))));
        ASSERT(!settings.isTimeInBalancingWindow(
            opCtx(),
            boost::posix_time::ptime(
                currentDate(), boost::posix_time::hours(17) + boost::posix_time::minutes(1))));
    } else {
        ASSERT(!settings.isTimeInBalancingWindow(
            opCtx(),
            boost::posix_time::ptime(
                currentDate(), boost::posix_time::hours(12) + boost::posix_time::minutes(0))));
    }
}

TEST_F(BalancerSettingsTypeTestFixture, BalancingWindowDOWMultipleDays) {
    RAIIServerParameterControllerForTest featureFlagController("featureFlagBalancerWindowDOW",
                                                               true);


    BalancerSettingsType settings = assertGet(BalancerSettingsType::fromBSON(
        opCtx(),
        BSON("activeWindowDOW" << BSON_ARRAY(BSON("day" << "Monday"
                                                        << "start"
                                                        << "9:00"
                                                        << "stop"
                                                        << "17:00")
                                             << BSON("day" << "Wednesday"
                                                           << "start"
                                                           << "9:00"
                                                           << "stop"
                                                           << "17:00")
                                             << BSON("day" << "Friday"
                                                           << "start"
                                                           << "9:00"
                                                           << "stop"
                                                           << "17:00")))));

    auto today = currentDate();
    int dayOfWeek = today.day_of_week();
    bool isActiveToday = (dayOfWeek == 1 || dayOfWeek == 3 || dayOfWeek == 5);

    if (isActiveToday) {
        ASSERT(settings.isTimeInBalancingWindow(
            opCtx(),
            boost::posix_time::ptime(
                currentDate(), boost::posix_time::hours(12) + boost::posix_time::minutes(0))));
    } else {
        ASSERT(!settings.isTimeInBalancingWindow(
            opCtx(),
            boost::posix_time::ptime(
                currentDate(), boost::posix_time::hours(12) + boost::posix_time::minutes(0))));
    }
}

TEST_F(BalancerSettingsTypeTestFixture, BalancingWindowDOWOvernightWindow) {
    RAIIServerParameterControllerForTest featureFlagController("featureFlagBalancerWindowDOW",
                                                               true);
    BalancerSettingsType settings = assertGet(BalancerSettingsType::fromBSON(
        opCtx(),
        BSON("activeWindowDOW" << BSON_ARRAY(BSON("day" << "Monday"
                                                        << "start"
                                                        << "22:00"
                                                        << "stop"
                                                        << "23:59")
                                             << BSON("day" << "Tuesday"
                                                           << "start"
                                                           << "00:00"
                                                           << "stop"
                                                           << "6:00")))));

    auto today = currentDate();
    int dayOfWeek = today.day_of_week();
    bool isMonday = (dayOfWeek == 1);
    bool isTuesday = (dayOfWeek == 2);

    if (isMonday) {
        ASSERT(settings.isTimeInBalancingWindow(
            opCtx(),
            boost::posix_time::ptime(
                currentDate(), boost::posix_time::hours(22) + boost::posix_time::minutes(30))));

        ASSERT(!settings.isTimeInBalancingWindow(
            opCtx(),
            boost::posix_time::ptime(
                currentDate(), boost::posix_time::hours(12) + boost::posix_time::minutes(0))));
    } else if (isTuesday) {
        ASSERT(settings.isTimeInBalancingWindow(
            opCtx(),
            boost::posix_time::ptime(
                currentDate(), boost::posix_time::hours(5) + boost::posix_time::minutes(30))));

        ASSERT(!settings.isTimeInBalancingWindow(
            opCtx(),
            (boost::posix_time::ptime(
                currentDate(), boost::posix_time::hours(12) + boost::posix_time::minutes(0)))));
    } else {
        ASSERT(!settings.isTimeInBalancingWindow(
            opCtx(),
            boost::posix_time::ptime(
                currentDate(), boost::posix_time::hours(12) + boost::posix_time::minutes(0))));
    }
}

TEST_F(BalancerSettingsTypeTestFixture, MultipleTimeWindowsForSameDay) {
    RAIIServerParameterControllerForTest featureFlagController("featureFlagBalancerWindowDOW",
                                                               true);
    BalancerSettingsType settings = assertGet(BalancerSettingsType::fromBSON(
        opCtx(),
        BSON("activeWindowDOW" << BSON_ARRAY(BSON("day" << "Monday"
                                                        << "start"
                                                        << "09:00"
                                                        << "stop"
                                                        << "11:00")
                                             << BSON("day" << "Monday"
                                                           << "start"
                                                           << "14:00"
                                                           << "stop"
                                                           << "16:00")))));

    auto today = currentDate();
    bool isMondayToday = (today.day_of_week() == 1);

    if (isMondayToday) {
        ASSERT(settings.isTimeInBalancingWindow(
            opCtx(),
            boost::posix_time::ptime(
                currentDate(), boost::posix_time::hours(9) + boost::posix_time::minutes(30))));
        ASSERT(settings.isTimeInBalancingWindow(
            opCtx(),
            boost::posix_time::ptime(
                currentDate(), boost::posix_time::hours(10) + boost::posix_time::minutes(45))));

        ASSERT(settings.isTimeInBalancingWindow(
            opCtx(),
            boost::posix_time::ptime(
                currentDate(), boost::posix_time::hours(14) + boost::posix_time::minutes(15))));
        ASSERT(settings.isTimeInBalancingWindow(
            opCtx(),
            boost::posix_time::ptime(
                currentDate(), boost::posix_time::hours(15) + boost::posix_time::minutes(59))));

        ASSERT(!settings.isTimeInBalancingWindow(
            opCtx(),
            boost::posix_time::ptime(
                currentDate(), boost::posix_time::hours(8) + boost::posix_time::minutes(59))));
        ASSERT(!settings.isTimeInBalancingWindow(
            opCtx(),
            boost::posix_time::ptime(
                currentDate(), boost::posix_time::hours(12) + boost::posix_time::minutes(0))));
        ASSERT(!settings.isTimeInBalancingWindow(
            opCtx(),
            boost::posix_time::ptime(
                currentDate(), boost::posix_time::hours(16) + boost::posix_time::minutes(1))));
    } else {
        ASSERT(!settings.isTimeInBalancingWindow(
            opCtx(),
            boost::posix_time::ptime(
                currentDate(), boost::posix_time::hours(10) + boost::posix_time::minutes(0))));
        ASSERT(!settings.isTimeInBalancingWindow(
            opCtx(),
            boost::posix_time::ptime(
                currentDate(), boost::posix_time::hours(15) + boost::posix_time::minutes(0))));
    }
}

TEST_F(BalancerSettingsTypeTestFixture, BalancingWindowDOWInvalidFormat) {
    RAIIServerParameterControllerForTest featureFlagController("featureFlagBalancerWindowDOW",
                                                               true);
    // Test with invalid day name.
    auto status1 = BalancerSettingsType::fromBSON(
                       opCtx(),
                       BSON("activeWindowDOW" << BSON_ARRAY(BSON("day" << "InvalidDay"
                                                                       << "start"
                                                                       << "9:00"
                                                                       << "stop"
                                                                       << "17:00"))))
                       .getStatus();
    ASSERT_NOT_OK(status1);
    ASSERT_EQUALS(ErrorCodes::BadValue, status1);
    ASSERT_EQ(status1.reason(), "invalid day of week: InvalidDay");

    // Test with missing day field.
    auto status2 = BalancerSettingsType::fromBSON(
                       opCtx(),
                       BSON("activeWindowDOW" << BSON_ARRAY(BSON("start" << "9:00"
                                                                         << "stop"
                                                                         << "17:00"))))
                       .getStatus();
    ASSERT_NOT_OK(status2);
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, status2);

    // Test with missing start field.
    auto status3 = BalancerSettingsType::fromBSON(
                       opCtx(),
                       BSON("activeWindowDOW" << BSON_ARRAY(BSON("day" << "Monday"
                                                                       << "stop"
                                                                       << "17:00"))))
                       .getStatus();
    ASSERT_NOT_OK(status3);
    ASSERT_EQUALS(ErrorCodes::BadValue, status3);
    ASSERT_STRING_CONTAINS(status3.reason(),
                           "must specify both start and stop of balancing window");

    // Test with missing stop field.
    auto status4 =
        BalancerSettingsType::fromBSON(opCtx(),
                                       BSON("activeWindowDOW" << BSON_ARRAY(BSON("day" << "Monday"
                                                                                       << "start"
                                                                                       << "9:00"))))
            .getStatus();
    ASSERT_NOT_OK(status4);
    ASSERT_EQUALS(ErrorCodes::BadValue, status4);
    ASSERT_STRING_CONTAINS(status4.reason(),
                           "must specify both start and stop of balancing window");

    // Test with invalid time format.
    auto status5 = BalancerSettingsType::fromBSON(
                       opCtx(),
                       BSON("activeWindowDOW" << BSON_ARRAY(BSON("day" << "Monday"
                                                                       << "start"
                                                                       << "9"
                                                                       << "stop"
                                                                       << "17:00"))))
                       .getStatus();
    ASSERT_NOT_OK(status5);
    ASSERT_EQUALS(ErrorCodes::BadValue, status5);
    ASSERT_EQ(status5.reason(), "time format must be \"hh:mm\" in activeWindowDOW");

    // Test with same start and stop time.
    auto status6 =
        BalancerSettingsType::fromBSON(opCtx(),
                                       BSON("activeWindowDOW" << BSON_ARRAY(BSON("day" << "Monday"
                                                                                       << "start"
                                                                                       << "9:00"
                                                                                       << "stop"
                                                                                       << "9:00"))))
            .getStatus();
    ASSERT_NOT_OK(status6);
    ASSERT_EQUALS(ErrorCodes::BadValue, status6);
    ASSERT_EQ(status6.reason(), "start and stop times must be different in activeWindowDOW");
}

TEST_F(BalancerSettingsTypeTestFixture, ActiveWindowAndActiveWindowDOWPrecedence) {
    RAIIServerParameterControllerForTest featureFlagController("featureFlagBalancerWindowDOW",
                                                               true);
    BalancerSettingsType settings = assertGet(BalancerSettingsType::fromBSON(
        opCtx(),
        BSON("activeWindow" << BSON("start" << "9:00"
                                            << "stop"
                                            << "17:00")
                            << "activeWindowDOW"
                            << BSON_ARRAY(BSON("day" << "Saturday"
                                                     << "start"
                                                     << "10:00"
                                                     << "stop"
                                                     << "18:00")
                                          << BSON("day" << "Sunday"
                                                        << "start"
                                                        << "10:00"
                                                        << "stop"
                                                        << "18:00")))));

    auto today = currentDate();
    int dayOfWeek = today.day_of_week();
    bool isWeekend = (dayOfWeek == 0 || dayOfWeek == 6);

    if (isWeekend) {
        ASSERT(settings.isTimeInBalancingWindow(
            opCtx(),
            boost::posix_time::ptime(
                currentDate(), boost::posix_time::hours(12) + boost::posix_time::minutes(0))));

        ASSERT(!settings.isTimeInBalancingWindow(
            opCtx(),
            boost::posix_time::ptime(currentDate(),
                                     boost::posix_time::hours(8) + boost::posix_time::minutes(0))));
    } else {
        ASSERT(!settings.isTimeInBalancingWindow(
            opCtx(),
            boost::posix_time::ptime(
                currentDate(), boost::posix_time::hours(12) + boost::posix_time::minutes(0))));
    }
}


// ===== Feature Flag Disabled Tests =====
TEST_F(BalancerSettingsTypeTestFixture, BalancingWindowDOWDisabled_IgnoresActiveWindowDOW) {
    unittest::LogCaptureGuard logs;
    BalancerSettingsType settings = assertGet(BalancerSettingsType::fromBSON(
        opCtx(),
        BSON("activeWindowDOW" << BSON_ARRAY(BSON("day" << "Monday"
                                                        << "start"
                                                        << "9:00"
                                                        << "stop"
                                                        << "17:00")))));

    logs.stop();
    ASSERT_EQ(1,
              logs.countTextContaining("Ignoring activeWindowDOW settings for versions under 8.3"));
}


TEST_F(BalancerSettingsTypeTestFixture, BalancingWindowDOWDisabled_FallsBackToActiveWindow) {
    unittest::LogCaptureGuard logs;

    BalancerSettingsType settings = assertGet(
        BalancerSettingsType::fromBSON(opCtx(),
                                       BSON("activeWindow" << BSON("start" << "9:00"
                                                                           << "stop"
                                                                           << "17:00")
                                                           << "activeWindowDOW"
                                                           << BSON_ARRAY(BSON("day" << "Monday"
                                                                                    << "start"
                                                                                    << "22:00"
                                                                                    << "stop"
                                                                                    << "23:00")))));

    logs.stop();

    ASSERT_EQ(1,
              logs.countTextContaining("Ignoring activeWindowDOW settings for versions under 8.3"));

    ASSERT(settings.isTimeInBalancingWindow(
        opCtx(),
        boost::posix_time::ptime(currentDate(),
                                 boost::posix_time::hours(12) + boost::posix_time::minutes(0))));

    ASSERT(!settings.isTimeInBalancingWindow(
        opCtx(),
        boost::posix_time::ptime(currentDate(),
                                 boost::posix_time::hours(8) + boost::posix_time::minutes(0))));
    ASSERT(!settings.isTimeInBalancingWindow(
        opCtx(),
        boost::posix_time::ptime(currentDate(),
                                 boost::posix_time::hours(18) + boost::posix_time::minutes(0))));

    ASSERT(!settings.isTimeInBalancingWindow(
        opCtx(),
        boost::posix_time::ptime(currentDate(),
                                 boost::posix_time::hours(22) + boost::posix_time::minutes(30))));
}

TEST_F(BalancerSettingsTypeTestFixture, BalancingWindowDOWDisabled_InvalidDayNamesAccepted) {
    unittest::LogCaptureGuard logs;

    BalancerSettingsType settings = assertGet(BalancerSettingsType::fromBSON(
        opCtx(),
        BSON("activeWindowDOW" << BSON_ARRAY(BSON("day" << "InvalidDay"
                                                        << "start"
                                                        << "9:00"
                                                        << "stop"
                                                        << "17:00")))));

    logs.stop();

    ASSERT_EQ(1,
              logs.countTextContaining("Ignoring activeWindowDOW settings for versions under 8.3"));

    ASSERT(settings.isTimeInBalancingWindow(
        opCtx(),
        boost::posix_time::ptime(currentDate(),
                                 boost::posix_time::hours(12) + boost::posix_time::minutes(0))));
}

TEST_F(BalancerSettingsTypeTestFixture, BalancingWindowDOWDisabled_MalformedDataAccepted) {
    unittest::LogCaptureGuard logs;

    BalancerSettingsType settings = assertGet(BalancerSettingsType::fromBSON(
        opCtx(),
        BSON("activeWindowDOW" << BSON_ARRAY(BSON("invalidField" << "Monday"
                                                                 << "wrongStart"
                                                                 << "9:00"
                                                                 << "wrongStop"
                                                                 << "17:00")))));

    logs.stop();

    ASSERT_EQ(1,
              logs.countTextContaining("Ignoring activeWindowDOW settings for versions under 8.3"));

    ASSERT(settings.isTimeInBalancingWindow(
        opCtx(),
        boost::posix_time::ptime(currentDate(),
                                 boost::posix_time::hours(12) + boost::posix_time::minutes(0))));
}

TEST_F(BalancerSettingsTypeTestFixture, NoBalancingWindowConfigured_AlwaysOn) {
    BalancerSettingsType settings = assertGet(BalancerSettingsType::fromBSON(opCtx(), BSONObj()));
    ASSERT(settings.isTimeInBalancingWindow(
        opCtx(),
        boost::posix_time::ptime(currentDate(),
                                 boost::posix_time::hours(0) + boost::posix_time::minutes(0))));
    ASSERT(settings.isTimeInBalancingWindow(
        opCtx(),
        boost::posix_time::ptime(currentDate(),
                                 boost::posix_time::hours(12) + boost::posix_time::minutes(0))));
    ASSERT(settings.isTimeInBalancingWindow(
        opCtx(),
        boost::posix_time::ptime(currentDate(),
                                 boost::posix_time::hours(23) + boost::posix_time::minutes(59))));
}
}  // namespace
}  // namespace mongo
