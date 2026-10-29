// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/process_health/health_observer.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/process_health/fault.h"
#include "mongo/db/process_health/fault_manager_test_suite.h"
#include "mongo/db/process_health/health_observer_base.h"
#include "mongo/db/process_health/health_observer_mock.h"
#include "mongo/db/process_health/health_observer_registration.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/future_impl.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {

namespace process_health {

// Using the common fault manager test suite.
using test::FaultManagerTest;
using PeriodicHealthCheckContext = HealthObserverBase::PeriodicHealthCheckContext;

namespace {
// Tests that the mock observer is registered properly.
// This test requires that actual production health observers (e.g. Ldap)
// are not linked with this test, otherwise the count of observers returned
// by the instantiate method below will be greater than expected.
TEST_F(FaultManagerTest, Registration) {
    registerMockHealthObserver(FaultFacetType::kMock1, [] { return Severity::kOk; });
    auto allObservers = HealthObserverRegistration::instantiateAllObservers(getServiceContext());
    ASSERT_EQ(1, allObservers.size());
    ASSERT_EQ(FaultFacetType::kMock1, allObservers[0]->getType());
}

TEST_F(FaultManagerTest, Stats) {
    resetManager(std::make_unique<FaultManagerConfig>());
    auto faultFacetType = FaultFacetType::kMock1;
    Atomic<Severity> mockResult(Severity::kFailure);
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
    Atomic<bool> shouldBlock{true};
    registerMockHealthObserver(FaultFacetType::kMock1, [&shouldBlock] {
        while (shouldBlock.load()) {
            sleepFor(Milliseconds(1));
        }
        return Severity::kFailure;
    });

    // Health check should get stuck here.
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
    unittest::ServerParameterGuard _intervalController{
        "healthMonitoringIntervals",
        BSON("values" << BSON_ARRAY(BSON("type" << "test"
                                                << "interval" << 1)))};
    auto faultFacetType = FaultFacetType::kMock1;
    Atomic<Severity> severity{Severity::kOk};
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
    unittest::ServerParameterGuard _intervalController{
        "healthMonitoringIntervals",
        BSON("values" << BSON_ARRAY(BSON("type" << "test"
                                                << "interval" << 1)))};
    unittest::ServerParameterGuard _serverParamController{"activeFaultDurationSecs", 5};

    Atomic<bool> shouldBlock{true};
    registerMockHealthObserver(
        FaultFacetType::kMock1,
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

TEST_F(FaultManagerTest, HealthCheckThrowingExceptionMakesFailedStatus) {
    resetManager(std::make_unique<FaultManagerConfig>());

    FaultFacetType facetType = FaultFacetType::kMock1;
    Atomic<bool> shouldThrow{false};

    std::string logMsg = "Failed due to exception";

    auto periodicCheckImpl =
        [facetType, &shouldThrow, logMsg](
            PeriodicHealthCheckContext&& periodicHealthCheckCtx) -> Future<HealthCheckStatus> {
        if (shouldThrow.load()) {
            uasserted(ErrorCodes::InternalError, logMsg);
        }
        auto completionPf = makePromiseFuture<HealthCheckStatus>();
        completionPf.promise.emplaceValue(HealthCheckStatus(facetType, Severity::kOk, "success"));
        return std::move(completionPf.future);
    };

    HealthObserverRegistration::registerObserverFactory(
        [facetType, periodicCheckImpl](ServiceContext* svcCtx) {
            return std::make_unique<HealthObserverMock>(
                facetType, svcCtx, periodicCheckImpl, Milliseconds(Seconds(30)));
        });

    assertSoon([this] { return (manager().getFaultState() == FaultState::kStartupCheck); });

    auto initialHealthCheckFuture = manager().startPeriodicHealthChecks();
    assertSoon([this] { return (manager().getFaultState() == FaultState::kOk); });

    auto observer = manager().getHealthObserversTest().front();
    ASSERT_EQ(observer->getStats().completedChecksWithFaultCount, 0);

    shouldThrow.store(true);
    assertSoon([this] { return (manager().getFaultState() == FaultState::kTransientFault); });

    ASSERT_EQ(manager().currentFault()->toBSON()["facets"]["mock1"]["description"].String(),
              "InternalError: Failed due to exception ");

    ASSERT_GTE(observer->getStats().completedChecksWithFaultCount, 1);
    resetManager();
}

}  // namespace
}  // namespace process_health
}  // namespace mongo
