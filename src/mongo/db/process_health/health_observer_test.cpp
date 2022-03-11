/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/process_health/health_observer.h"

#include "mongo/db/process_health/fault_manager_test_suite.h"
#include "mongo/db/process_health/health_observer_mock.h"
#include "mongo/db/process_health/health_observer_registration.h"
#include "mongo/db/service_context.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/timer.h"

namespace mongo {

namespace process_health {

// Using the common fault manager test suite.
using test::FaultManagerTest;

namespace {
// Tests that the mock observer is registered properly.
// This test requires that actual production health observers (e.g. Ldap)
// are not linked with this test, otherwise the count of observers returned
// by the instantiate method below will be greater than expected.
TEST_F(FaultManagerTest, Registration) {
    registerMockHealthObserver(FaultFacetType::kMock1, [] { return Severity::kOk; });
    auto allObservers = HealthObserverRegistration::instantiateAllObservers(svcCtx());
    ASSERT_EQ(1, allObservers.size());
    ASSERT_EQ(FaultFacetType::kMock1, allObservers[0]->getType());
}

TEST_F(FaultManagerTest, InitialHealthCheckDoesNotRunIfFeatureFlagNotEnabled) {
    resetManager();
    RAIIServerParameterControllerForTest _controller{"featureFlagHealthMonitoring", false};

    registerMockHealthObserver(FaultFacetType::kMock1, [] { return Severity::kOk; });
    static_cast<void>(manager().schedulePeriodicHealthCheckThreadTest());

    auto currentFault = manager().currentFault();
    ASSERT_TRUE(!currentFault);  // Is not created.
    ASSERT_TRUE(manager().getFaultState() == FaultState::kStartupCheck);
}

TEST_F(FaultManagerTest, Stats) {
    RAIIServerParameterControllerForTest _controller{"featureFlagHealthMonitoring", true};
    resetManager(std::make_unique<FaultManagerConfig>());
    auto faultFacetType = FaultFacetType::kMock1;
    AtomicWord<Severity> mockResult(Severity::kFailure);
    registerMockHealthObserver(faultFacetType, [&mockResult] { return mockResult.load(); });

    auto initialHealthCheckFuture = manager().startPeriodicHealthChecks();
    auto observer = manager().getHealthObserversTest()[0];

    // Initial checks should fail; There must have been at least 1 to generate the fault.
    assertSoon([this, &observer] {
        return hasFault() && !observer->getStats().currentlyRunningHealthCheck;
    });

    // Make sure we are still in startup check state.
    waitForTransitionIntoState(FaultState::kStartupCheck);

    auto initialStats = observer->getStats();
    LOGV2_DEBUG(6331901, 0, "stats after detecting fault", "stats"_attr = initialStats);
    ASSERT_TRUE(manager().getConfig().isHealthObserverEnabled(observer->getType()));
    ASSERT_EQ(initialStats.lastTimeCheckStarted, clockSource().now());
    ASSERT_EQ(initialStats.lastTimeCheckCompleted, initialStats.lastTimeCheckStarted);
    ASSERT_GTE(initialStats.completedChecksCount, 1);
    ASSERT_GTE(initialStats.completedChecksWithFaultCount, 1);

    // To complete initial health check.
    mockResult.store(Severity::kOk);

    waitForTransitionIntoState(FaultState::kOk);
    auto okStats = observer->getStats();
    LOGV2_DEBUG(6331902, 0, "stats after ok state", "stats"_attr = okStats);
    advanceTime(Milliseconds(100));

    assertSoon([observer, okStats]() {
        auto stats = observer->getStats();
        return stats.completedChecksCount > okStats.completedChecksCount;
    });

    auto finalStats = observer->getStats();
    LOGV2_DEBUG(6331903, 0, "stats after final state", "stats"_attr = finalStats);
    ASSERT_GT(finalStats.lastTimeCheckStarted, okStats.lastTimeCheckStarted);
    ASSERT_GT(finalStats.lastTimeCheckCompleted, okStats.lastTimeCheckCompleted);
    ASSERT_GTE(finalStats.completedChecksCount, okStats.completedChecksCount);
    ASSERT_GTE(finalStats.completedChecksWithFaultCount, okStats.completedChecksWithFaultCount);
}

TEST_F(FaultManagerTest, ProgressMonitorCheck) {
    AtomicWord<bool> shouldBlock{true};
    registerMockHealthObserver(FaultFacetType::kMock1, [&shouldBlock] {
        while (shouldBlock.load()) {
            sleepFor(Milliseconds(1));
        }
        return Severity::kFailure;
    });

    // Health check should get stuck here.
    RAIIServerParameterControllerForTest _controller{"featureFlagHealthMonitoring", true};
    auto initialHealthCheckFuture = manager().startPeriodicHealthChecks();
    auto observer = manager().getHealthObserversTest()[0];
    manager().healthCheckTest(observer, CancellationToken::uncancelable());

    // Verify that the 'crash callback' is invoked after timeout.
    bool crashTriggered = false;
    std::function<void(std::string cause)> crashCb = [&crashTriggered](std::string) {
        crashTriggered = true;
    };
    manager().progressMonitorCheckTest(crashCb);
    // The progress check passed because the simulated time did not advance.
    ASSERT_FALSE(crashTriggered);
    advanceTime(manager().getConfig().getPeriodicLivenessDeadline() + Seconds(1));
    manager().progressMonitorCheckTest(crashCb);
    // The progress check simulated a crash.
    ASSERT_TRUE(crashTriggered);
    shouldBlock.store(false);
    resetManager();  // Before fields above go out of scope.
}

TEST_F(FaultManagerTest, HealthCheckRunsPeriodically) {
    resetManager(std::make_unique<FaultManagerConfig>());
    RAIIServerParameterControllerForTest _intervalController{
        "healthMonitoringIntervals",
        BSON("values" << BSON_ARRAY(BSON("type"
                                         << "test"
                                         << "interval" << 1)))};
    RAIIServerParameterControllerForTest _controller{"featureFlagHealthMonitoring", true};
    auto faultFacetType = FaultFacetType::kMock1;
    AtomicWord<Severity> severity{Severity::kOk};
    registerMockHealthObserver(faultFacetType, [&severity] { return severity.load(); });

    assertSoon([this] { return (manager().getFaultState() == FaultState::kStartupCheck); });

    auto initialHealthCheckFuture = manager().startPeriodicHealthChecks();
    assertSoon([this] { return (manager().getFaultState() == FaultState::kOk); });

    severity.store(Severity::kFailure);
    assertSoon([this] { return (manager().getFaultState() == FaultState::kTransientFault); });
    resetManager();  // Before fields above go out of scope.
}

TEST_F(FaultManagerTest, PeriodicHealthCheckOnErrorMakesBadHealthStatus) {
    resetManager(std::make_unique<FaultManagerConfig>());
    RAIIServerParameterControllerForTest _controller{"featureFlagHealthMonitoring", true};
    auto faultFacetType = FaultFacetType::kMock1;

    registerMockHealthObserver(faultFacetType, [] {
        uassert(ErrorCodes::InternalError, "test exception", false);
        return Severity::kFailure;
    });

    ASSERT_TRUE(manager().getFaultState() == FaultState::kStartupCheck);

    auto initialHealthCheckFuture = manager().startPeriodicHealthChecks();
    assertSoon([this] {
        return manager().currentFault() && manager().getFaultState() == FaultState::kStartupCheck;
    });
}

TEST_F(FaultManagerTest,
       DeadlineFutureCausesTransientFaultWhenObserverBlocksAndGetsResolvedWhenObserverUnblocked) {
    resetManager(std::make_unique<FaultManagerConfig>());
    RAIIServerParameterControllerForTest _intervalController{
        "healthMonitoringIntervals",
        BSON("values" << BSON_ARRAY(BSON("type"
                                         << "test"
                                         << "interval" << 1)))};
    RAIIServerParameterControllerForTest _flagController{"featureFlagHealthMonitoring", true};
    RAIIServerParameterControllerForTest _serverParamController{"activeFaultDurationSecs", 5};

    AtomicWord<bool> shouldBlock{true};
    registerMockHealthObserver(FaultFacetType::kMock1,
                               [&shouldBlock] {
                                   while (shouldBlock.load()) {
                                       sleepFor(Milliseconds(1));
                                   }
                                   return Severity::kOk;
                               },
                               Milliseconds(100));

    ASSERT_TRUE(manager().getFaultState() == FaultState::kStartupCheck);

    auto initialHealthCheckFuture = manager().startPeriodicHealthChecks();

    assertSoon([this] {
        return manager().currentFault() && manager().getFaultState() == FaultState::kStartupCheck;
    });

    shouldBlock.store(false);

    assertSoon([this] { return manager().getFaultState() == FaultState::kOk; });

    resetManager();  // Before fields above go out of scope.
}

TEST_F(FaultManagerTest, SchedulingDuplicateHealthChecksRejected) {
    static constexpr int kLoops = 1000;
    resetManager(std::make_unique<FaultManagerConfig>());
    registerMockHealthObserver(FaultFacetType::kMock1, [] { return Severity::kOk; });
    auto initialHealthCheckFuture = manager().startPeriodicHealthChecks();
    waitForTransitionIntoState(FaultState::kOk);

    auto observer = manager().getHealthObserversTest()[0];
    auto initialStats = observer->getStats();

    for (int i = 0; i < kLoops; ++i) {
        // A check will not be scheduled if another check is running.
        // Default interval is 1 sec so only 1/500 of checks will be scheduled.
        scheduleNextImmediateCheck<HealthObserverMock>(FaultFacetType::kMock1);
        sleepFor(Milliseconds(2));
    }

    // Sleep time here is not introducing flakiness - it only shows that even after
    // waiting the total count of completed tests is lower than the total we scheduled.
    sleepFor(Milliseconds(100));
    auto finalStats = observer->getStats();

    const auto totalCompletedCount =
        finalStats.completedChecksCount - initialStats.completedChecksCount;
    ASSERT_LT(totalCompletedCount, kLoops);
    ASSERT_GT(totalCompletedCount, 0);
    LOGV2(6418205, "Total completed checks count", "count"_attr = totalCompletedCount);
}

}  // namespace
}  // namespace process_health
}  // namespace mongo
