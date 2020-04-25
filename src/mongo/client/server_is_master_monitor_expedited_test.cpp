/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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
#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork
#include "mongo/client/server_is_master_monitor.h"

#include <boost/optional/optional_io.hpp>

#include "mongo/executor/task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
class SingleServerIsMasterMonitorExpeditedFixture : public unittest::Test {
public:
    struct TestCase {
        boost::optional<Milliseconds> timeElapsedSinceLastIsMaster;
        Milliseconds previousRefreshPeriod;
        boost::optional<Milliseconds> expectedResult;
    };

    void verifyTestCase(TestCase testCase) {
        LOGV2_INFO(4712103,
                   "TestCase",
                   "timeElapsedSinceLastIsMaster"_attr = testCase.timeElapsedSinceLastIsMaster,
                   "previousRefreshPeriod"_attr = testCase.previousRefreshPeriod,
                   "expeditedRefreshPeriod"_attr = kExpeditedRefreshPeriod);
        auto result = SingleServerIsMasterMonitor::calculateExpeditedDelayUntilNextCheck(
            testCase.timeElapsedSinceLastIsMaster,
            kExpeditedRefreshPeriod,
            testCase.previousRefreshPeriod);
        ASSERT_EQUALS(testCase.expectedResult, result);
    }

    static inline const auto kExpeditedRefreshPeriod = Milliseconds(500);
    static inline const auto kLongRefreshPeriod = Milliseconds(10 * 1000);
    static inline const auto kDelta = Milliseconds(100);
    static inline const auto kOneMs = Milliseconds(1);
    static inline const auto kZeroMs = Milliseconds(0);
};

TEST_F(SingleServerIsMasterMonitorExpeditedFixture, NoPreviousRequest) {
    verifyTestCase(TestCase{boost::none, kLongRefreshPeriod, Milliseconds(0)});
    verifyTestCase(TestCase{boost::none, kExpeditedRefreshPeriod, Milliseconds(0)});
}

TEST_F(SingleServerIsMasterMonitorExpeditedFixture,
       PreviousRequestLongerThanExpeditedRefreshPeriod) {
    verifyTestCase(TestCase{kExpeditedRefreshPeriod + kDelta, kLongRefreshPeriod, Milliseconds(0)});
    verifyTestCase(
        TestCase{kExpeditedRefreshPeriod + kDelta, kExpeditedRefreshPeriod, Milliseconds(0)});
}

TEST_F(SingleServerIsMasterMonitorExpeditedFixture, PreviousRequestEqualLongRefreshPeriod) {
    verifyTestCase(TestCase{kLongRefreshPeriod, kLongRefreshPeriod, boost::none});
    verifyTestCase(TestCase{kLongRefreshPeriod, kExpeditedRefreshPeriod, Milliseconds(0)});
}

TEST_F(SingleServerIsMasterMonitorExpeditedFixture, PreviousRequestEqualExpeditedRefreshPeriod) {
    verifyTestCase(TestCase{kExpeditedRefreshPeriod, kLongRefreshPeriod, Milliseconds(0)});
    verifyTestCase(TestCase{kExpeditedRefreshPeriod, kExpeditedRefreshPeriod, boost::none});
}

TEST_F(SingleServerIsMasterMonitorExpeditedFixture,
       PreviousRequestShorterThanExpeditedRefreshPeriod) {
    verifyTestCase(
        TestCase{kExpeditedRefreshPeriod - kDelta, kLongRefreshPeriod, Milliseconds(kDelta)});
    verifyTestCase(
        TestCase{kExpeditedRefreshPeriod - kDelta, kExpeditedRefreshPeriod, boost::none});
}

TEST_F(SingleServerIsMasterMonitorExpeditedFixture, PreviousRequestImmediatelyBefore) {
    verifyTestCase(TestCase{kOneMs, kLongRefreshPeriod, kExpeditedRefreshPeriod - kOneMs});
    verifyTestCase(TestCase{kOneMs, kExpeditedRefreshPeriod, boost::none});
}

TEST_F(SingleServerIsMasterMonitorExpeditedFixture, PreviousRequestConcurrent) {
    verifyTestCase(TestCase{kZeroMs, kLongRefreshPeriod, kExpeditedRefreshPeriod});
    verifyTestCase(TestCase{kZeroMs, kExpeditedRefreshPeriod, boost::none});
}
}  // namespace mongo
