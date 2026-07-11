// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/client/server_discovery_monitor.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


namespace mongo::sdam {
class SingleServerDiscoveryMonitorExpeditedFixture : public unittest::Test {
public:
    struct TestCase {
        boost::optional<Milliseconds> timeElapsedSinceLastHello;
        Milliseconds previousRefreshPeriod;
        boost::optional<Milliseconds> expectedResult;
    };

    void verifyTestCase(TestCase testCase) {
        LOGV2_INFO(4712103,
                   "TestCase",
                   "timeElapsedSinceLastHello"_attr = testCase.timeElapsedSinceLastHello,
                   "previousRefreshPeriod"_attr = testCase.previousRefreshPeriod,
                   "expeditedRefreshPeriod"_attr = kExpeditedRefreshPeriod);
        auto result = SingleServerDiscoveryMonitor::calculateExpeditedDelayUntilNextCheck(
            testCase.timeElapsedSinceLastHello,
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

TEST_F(SingleServerDiscoveryMonitorExpeditedFixture, NoPreviousRequest) {
    verifyTestCase(TestCase{boost::none, kLongRefreshPeriod, Milliseconds(0)});
    verifyTestCase(TestCase{boost::none, kExpeditedRefreshPeriod, Milliseconds(0)});
}

TEST_F(SingleServerDiscoveryMonitorExpeditedFixture,
       PreviousRequestLongerThanExpeditedRefreshPeriod) {
    verifyTestCase(TestCase{kExpeditedRefreshPeriod + kDelta, kLongRefreshPeriod, Milliseconds(0)});
    verifyTestCase(
        TestCase{kExpeditedRefreshPeriod + kDelta, kExpeditedRefreshPeriod, Milliseconds(0)});
}

TEST_F(SingleServerDiscoveryMonitorExpeditedFixture, PreviousRequestEqualLongRefreshPeriod) {
    verifyTestCase(TestCase{kLongRefreshPeriod, kLongRefreshPeriod, boost::none});
    verifyTestCase(TestCase{kLongRefreshPeriod, kExpeditedRefreshPeriod, Milliseconds(0)});
}

TEST_F(SingleServerDiscoveryMonitorExpeditedFixture, PreviousRequestEqualExpeditedRefreshPeriod) {
    verifyTestCase(TestCase{kExpeditedRefreshPeriod, kLongRefreshPeriod, Milliseconds(0)});
    verifyTestCase(TestCase{kExpeditedRefreshPeriod, kExpeditedRefreshPeriod, boost::none});
}

TEST_F(SingleServerDiscoveryMonitorExpeditedFixture,
       PreviousRequestShorterThanExpeditedRefreshPeriod) {
    verifyTestCase(
        TestCase{kExpeditedRefreshPeriod - kDelta, kLongRefreshPeriod, Milliseconds(kDelta)});
    verifyTestCase(
        TestCase{kExpeditedRefreshPeriod - kDelta, kExpeditedRefreshPeriod, boost::none});
}

TEST_F(SingleServerDiscoveryMonitorExpeditedFixture, PreviousRequestImmediatelyBefore) {
    verifyTestCase(TestCase{kOneMs, kLongRefreshPeriod, kExpeditedRefreshPeriod - kOneMs});
    verifyTestCase(TestCase{kOneMs, kExpeditedRefreshPeriod, boost::none});
}

TEST_F(SingleServerDiscoveryMonitorExpeditedFixture, PreviousRequestConcurrent) {
    verifyTestCase(TestCase{kZeroMs, kLongRefreshPeriod, kExpeditedRefreshPeriod});
    verifyTestCase(TestCase{kZeroMs, kExpeditedRefreshPeriod, boost::none});
}
}  // namespace mongo::sdam
