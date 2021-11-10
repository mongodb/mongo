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
    registerMockHealthObserver(FaultFacetType::kMock1, [] { return 0; });
    auto allObservers = HealthObserverRegistration::instantiateAllObservers(svcCtx());
    ASSERT_EQ(1, allObservers.size());
    ASSERT_EQ(FaultFacetType::kMock1, allObservers[0]->getType());
}

TEST_F(FaultManagerTest, HealthCheckCreatesObservers) {
    registerMockHealthObserver(FaultFacetType::kMock1, [] { return 0.1; });
    ASSERT_EQ(0, manager().getHealthObserversTest().size());

    advanceTime(Milliseconds(100));
    manager().healthCheckTest();
    ASSERT_EQ(1, manager().getHealthObserversTest().size());
}

TEST_F(FaultManagerTest, HealthCheckCreatesFacetOnHealthCheckFoundFault) {
    registerMockHealthObserver(FaultFacetType::kMock1, [] { return 0.1; });

    advanceTime(Milliseconds(100));
    manager().healthCheckTest();
    waitForTransitionIntoState(FaultState::kTransientFault);
    auto currentFault = manager().currentFault();
    ASSERT_TRUE(currentFault);  // Is created.
}

TEST_F(FaultManagerTest, StateTransitionOnHealthCheckFoundFault) {
    registerMockHealthObserver(FaultFacetType::kMock1, [] { return 0.1; });
    ASSERT_EQ(FaultState::kStartupCheck, manager().getFaultState());

    waitForTransitionIntoState(FaultState::kTransientFault);
    ASSERT_EQ(FaultState::kTransientFault, manager().getFaultState());
}

TEST_F(FaultManagerTest, HealthCheckCreatesCorrectFacetOnHealthCheckFoundFault) {
    registerMockHealthObserver(FaultFacetType::kMock1, [] { return 0.1; });
    registerMockHealthObserver(FaultFacetType::kMock2, [] { return 0.0; });
    waitForTransitionIntoState(FaultState::kTransientFault);

    FaultInternal& internalFault = manager().getFault();
    ASSERT_TRUE(internalFault.getFaultFacet(FaultFacetType::kMock1));
    ASSERT_FALSE(internalFault.getFaultFacet(FaultFacetType::kMock2));
}

TEST_F(FaultManagerTest, SeverityIsMaxFromAllFacetsSeverity) {
    registerMockHealthObserver(FaultFacetType::kMock1, [] { return 0.8; });
    registerMockHealthObserver(FaultFacetType::kMock2, [] { return 0.5; });
    advanceTime(Milliseconds(100));
    manager().healthCheckTest();
    do {
        waitForFaultBeingCreated();
        advanceTime(Milliseconds(100));
        manager().healthCheckTest();
    } while (manager().getFault().getFacets().size() != 2);  // Race between two facets.
    auto currentFault = manager().currentFault();

    ASSERT_APPROX_EQUAL(0.8, currentFault->getSeverity(), 0.001);
}

TEST_F(FaultManagerTest, HealthCheckCreatesFacetThenIsGarbageCollectedAndStateTransitioned) {
    AtomicDouble severity{0.1};
    registerMockHealthObserver(FaultFacetType::kMock1, [&severity] { return severity.load(); });
    advanceTime(Milliseconds(100));
    manager().healthCheckTest();
    waitForFaultBeingCreated();
    ASSERT_TRUE(manager().currentFault());  // Is created.

    // Resolve and it should be garbage collected.
    severity.store(0.0);

    waitForTransitionIntoState(FaultState::kOk);

    assertSoonWithHealthCheck([this]() { return !hasFault(); });

    // State is transitioned.
    ASSERT_EQ(FaultState::kOk, manager().getFaultState());
    resetManager();  // Before atomic fields above go out of scope.
}

TEST_F(FaultManagerTest, HealthCheckCreates2FacetsThenIsGarbageCollected) {
    AtomicDouble severity1{0.1};
    AtomicDouble severity2{0.1};
    registerMockHealthObserver(FaultFacetType::kMock1, [&severity1] { return severity1.load(); });
    registerMockHealthObserver(FaultFacetType::kMock2, [&severity2] { return severity2.load(); });
    manager().healthCheckTest();
    waitForFaultBeingCreated();

    while (manager().getFault().getFacets().size() != 2) {
        sleepFor(Milliseconds(1));
        advanceTime(Milliseconds(100));
        manager().healthCheckTest();
    }

    // Resolve one facet and it should be garbage collected.
    severity1.store(0.0);
    advanceTime(Milliseconds(100));
    manager().healthCheckTest();

    FaultInternal& internalFault = manager().getFault();
    while (internalFault.getFaultFacet(FaultFacetType::kMock1)) {
        sleepFor(Milliseconds(1));
        // Check is async, needs more turns for garbage collection to work.
        advanceTime(Milliseconds(100));
        manager().healthCheckTest();
    }
    ASSERT_FALSE(internalFault.getFaultFacet(FaultFacetType::kMock1));
    ASSERT_TRUE(internalFault.getFaultFacet(FaultFacetType::kMock2));
    resetManager();  // Before atomic fields above go out of scope.
}

TEST_F(FaultManagerTest, HealthCheckWithOffFacetCreatesNoFault) {
    registerMockHealthObserver(FaultFacetType::kMock1, [] { return 0.0; });
    manager().healthCheckTest();
    waitForFaultBeingResolved();
    auto currentFault = manager().currentFault();
    ASSERT_TRUE(!currentFault);  // Is not created.
}

TEST_F(FaultManagerTest, DoesNotRestartCheckBeforeIntervalExpired) {
    AtomicDouble severity{0.0};
    registerMockHealthObserver(FaultFacetType::kMock1, [&severity] { return severity.load(); });
    manager().healthCheckTest();
    waitForFaultBeingResolved();
    auto currentFault = manager().currentFault();
    ASSERT_TRUE(!currentFault);  // Is not created.

    severity.store(0.1);
    manager().healthCheckTest();
    currentFault = manager().currentFault();
    // The check did not run because the delay interval did not expire.
    ASSERT_TRUE(!currentFault);

    advanceTime(Milliseconds(100));
    assertSoonWithHealthCheck([this]() { return hasFault(); });
    currentFault = manager().currentFault();
    ASSERT_TRUE(currentFault);  // The fault was created.
    resetManager();             // Before atomic fields above go out of scope.
}

TEST_F(FaultManagerTest, HealthCheckWithCriticalFacetCreatesFault) {
    registerMockHealthObserver(FaultFacetType::kMock1, [] { return 1.1; });
    manager().healthCheckTest();
    waitForFaultBeingCreated();
    auto currentFault = manager().currentFault();
    ASSERT_TRUE(currentFault);
}

TEST_F(FaultManagerTest, InitialHealthCheckDoesNotBlockIfTransitionToOkSucceeds) {
    resetManager();
    RAIIServerParameterControllerForTest _controller{"featureFlagHealthMonitoring", true};

    registerMockHealthObserver(FaultFacetType::kMock1, [] { return 0.0; });
    manager().healthCheckTest();

    auto currentFault = manager().currentFault();
    ASSERT_TRUE(!currentFault);  // Is not created.
    ASSERT_TRUE(manager().getFaultState() == FaultState::kOk);
}

TEST_F(FaultManagerTest, InitialHealthCheckDoesNotRunIfFeatureFlagNotEnabled) {
    resetManager();
    RAIIServerParameterControllerForTest _controller{"featureFlagHealthMonitoring", false};

    registerMockHealthObserver(FaultFacetType::kMock1, [] { return 0.0; });
    manager().startPeriodicHealthChecks();

    auto currentFault = manager().currentFault();
    ASSERT_TRUE(!currentFault);  // Is not created.
    ASSERT_TRUE(manager().getFaultState() == FaultState::kStartupCheck);
}

TEST_F(FaultManagerTest, Stats) {
    advanceTime(Milliseconds(100));
    registerMockHealthObserver(FaultFacetType::kMock1, [] { return 0.1; });
    waitForTransitionIntoState(FaultState::kTransientFault);

    auto observer = manager().getHealthObserversTest()[0];
    auto stats = observer->getStats();
    ASSERT_TRUE(manager().getConfig().isHealthObserverEnabled(observer->getType()));
    ASSERT_FALSE(stats.currentlyRunningHealthCheck);
    ASSERT_TRUE(stats.lastTimeCheckStarted >= clockSource().now());
    ASSERT_TRUE(stats.lastTimeCheckCompleted >= stats.lastTimeCheckStarted);
    ASSERT_TRUE(stats.completedChecksCount >= 1);
    ASSERT_TRUE(stats.completedChecksWithFaultCount >= 1);
}

TEST_F(FaultManagerTest, ProgressMonitorCheck) {
    AtomicWord<bool> shouldBlock{true};
    registerMockHealthObserver(FaultFacetType::kMock1, [this, &shouldBlock] {
        while (shouldBlock.load()) {
            sleepFor(Milliseconds(1));
        }
        return 0.1;
    });

    // Health check should get stuck here.
    manager().healthCheckTest();
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

TEST_F(FaultManagerTest, TransitionsToActiveFaultAfterTimeout) {
    auto config = test::getConfigWithDisabledPeriodicChecks();
    config->setActiveFaultDurationForTests(Milliseconds(10));
    resetManager(std::move(config));
    registerMockHealthObserver(FaultFacetType::kMock1, [] { return 1.1; });
    waitForTransitionIntoState(FaultState::kTransientFault);
    ASSERT_TRUE(manager().getFaultState() == FaultState::kTransientFault);
    advanceTime(Milliseconds(10));
    waitForTransitionIntoState(FaultState::kActiveFault);
}

TEST_F(FaultManagerTest, DoesNotTransitionToActiveFaultIfResolved) {
    const auto activeFaultDuration = manager().getConfigTest().getActiveFaultDuration();
    const auto start = clockSource().now();

    // Initially unhealthy; Transitions to healthy before the active fault timeout.
    registerMockHealthObserver(FaultFacetType::kMock1, [=] {
        auto now = clockSource().now();
        auto elapsed = now - start;
        auto quarterActiveFaultDuration =
            Milliseconds(durationCount<Milliseconds>(activeFaultDuration) / 4);
        if (elapsed < quarterActiveFaultDuration) {
            return 1.1;
        }
        return 0.0;
    });
    waitForTransitionIntoState(FaultState::kTransientFault);
    assertSoonWithHealthCheck([this]() { return manager().getFaultState() == FaultState::kOk; });
    advanceTime(activeFaultDuration);
    assertSoonWithHealthCheck([this]() { return manager().getFaultState() == FaultState::kOk; });
}

}  // namespace
}  // namespace process_health
}  // namespace mongo
