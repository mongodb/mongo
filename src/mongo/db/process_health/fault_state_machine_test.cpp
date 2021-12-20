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

#include "mongo/db/process_health/fault_manager.h"

#include "mongo/db/process_health/fault_manager_test_suite.h"
#include "mongo/db/process_health/health_check_status.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace process_health {

using test::FaultManagerTest;
using test::FaultManagerTestImpl;

namespace {

TEST_F(FaultManagerTest, TransitionsFromStartupCheckToOkWhenAllObserversAreSuccessful) {
    RAIIServerParameterControllerForTest _controller{"featureFlagHealthMonitoring", true};
    registerMockHealthObserver(FaultFacetType::kMock1, [] { return 0; });
    registerMockHealthObserver(FaultFacetType::kMock2, [] { return 0; });

    auto initialHealthCheckFuture = manager().startPeriodicHealthChecks();
    std::vector<FaultFacetType> faultFacetTypes{FaultFacetType::kMock1, FaultFacetType::kMock2};

    ASSERT(manager().getFaultState() == FaultState::kStartupCheck);

    // send successful health check response from each
    for (auto faultFacetType : faultFacetTypes) {
        manager().acceptTest(HealthCheckStatus(faultFacetType));
        advanceTime(Milliseconds(100));
        ASSERT(!hasFault());
        if (faultFacetType != faultFacetTypes.back()) {
            ASSERT(manager().getFaultState() == FaultState::kStartupCheck);
        }
    }

    assertSoon([this]() { return manager().getFaultState() == FaultState::kOk; });
    ASSERT(initialHealthCheckFuture.isReady());
}

TEST_F(FaultManagerTest, TransitionsFromStartupCheckToOkAfterFailureThenSuccess) {
    RAIIServerParameterControllerForTest _controller{"featureFlagHealthMonitoring", true};
    const auto faultFacetType = FaultFacetType::kMock1;
    registerMockHealthObserver(faultFacetType, [] { return 0; });

    auto initialHealthCheckFuture = manager().startPeriodicHealthChecks();

    ASSERT(manager().getFaultState() == FaultState::kStartupCheck);
    manager().acceptTest(HealthCheckStatus(faultFacetType, 1.0, "error"));
    advanceTime(Milliseconds(100));
    ASSERT(manager().getFaultState() == FaultState::kStartupCheck);
    ASSERT(hasFault());
    manager().acceptTest(HealthCheckStatus(faultFacetType));
    advanceTime(Milliseconds(100));
    ASSERT(!hasFault());

    assertSoon([this]() { return manager().getFaultState() == FaultState::kOk; });
    ASSERT(initialHealthCheckFuture.isReady());
}

TEST_F(FaultManagerTest, TransitionsFromOkToTransientFaultAfterSuccessThenFailure) {
    RAIIServerParameterControllerForTest _controller{"featureFlagHealthMonitoring", true};

    const auto faultFacetType = FaultFacetType::kMock1;
    registerMockHealthObserver(faultFacetType, [] { return 0; });

    auto initialHealthCheckFuture = manager().startPeriodicHealthChecks();

    ASSERT(manager().getFaultState() == FaultState::kStartupCheck);
    manager().acceptTest(HealthCheckStatus(faultFacetType));
    advanceTime(Milliseconds(100));
    ASSERT(!hasFault());

    assertSoon([this]() { return manager().getFaultState() == FaultState::kOk; });
    ASSERT(initialHealthCheckFuture.isReady());

    manager().acceptTest(HealthCheckStatus(faultFacetType, 1.0, "error"));
    advanceTime(Milliseconds(100));
    assertSoon([this]() {
        return hasFault() && manager().getFaultState() == FaultState::kTransientFault;
    });
}

TEST_F(FaultManagerTest, StaysInOkOnSuccess) {
    RAIIServerParameterControllerForTest _controller{"featureFlagHealthMonitoring", true};
    const auto faultFacetType = FaultFacetType::kMock1;
    registerMockHealthObserver(faultFacetType, [] { return 0; });

    auto initialHealthCheckFuture = manager().startPeriodicHealthChecks();

    ASSERT(manager().getFaultState() == FaultState::kStartupCheck);
    manager().acceptTest(HealthCheckStatus(faultFacetType));
    advanceTime(Milliseconds(100));
    ASSERT(!hasFault());

    assertSoon([this]() { return manager().getFaultState() == FaultState::kOk; });
    ASSERT(initialHealthCheckFuture.isReady());

    manager().acceptTest(HealthCheckStatus(faultFacetType));
    advanceTime(Milliseconds(100));

    ASSERT(!hasFault());
    assertSoon([this]() { return manager().getFaultState() == FaultState::kOk; });
}

TEST_F(FaultManagerTest, StaysInTransientFault) {
    RAIIServerParameterControllerForTest _controller{"featureFlagHealthMonitoring", true};
    const auto faultFacetType = FaultFacetType::kMock1;
    registerMockHealthObserver(faultFacetType, [] { return 0; });

    auto initialHealthCheckFuture = manager().startPeriodicHealthChecks();

    ASSERT(manager().getFaultState() == FaultState::kStartupCheck);
    manager().acceptTest(HealthCheckStatus(faultFacetType));
    advanceTime(Milliseconds(100));
    ASSERT(!hasFault());

    assertSoon([this]() { return manager().getFaultState() == FaultState::kOk; });
    ASSERT(initialHealthCheckFuture.isReady());

    manager().acceptTest(HealthCheckStatus(faultFacetType, 1.0, "error"));
    advanceTime(Milliseconds(100));
    assertSoon([this]() {
        return hasFault() && manager().getFaultState() == FaultState::kTransientFault;
    });

    manager().acceptTest(HealthCheckStatus(faultFacetType, 1.0, "error"));
    advanceTime(Milliseconds(100));
    assertSoon([this]() {
        return hasFault() && manager().getFaultState() == FaultState::kTransientFault;
    });
}

TEST_F(FaultManagerTest, TransitionsFromTransientFaultToOkOnFailureThenSuccess) {
    RAIIServerParameterControllerForTest _controller{"featureFlagHealthMonitoring", true};
    const auto faultFacetType = FaultFacetType::kMock1;
    registerMockHealthObserver(faultFacetType, [] { return 0; });

    auto initialHealthCheckFuture = manager().startPeriodicHealthChecks();

    ASSERT(manager().getFaultState() == FaultState::kStartupCheck);
    manager().acceptTest(HealthCheckStatus(faultFacetType));
    advanceTime(Milliseconds(100));
    ASSERT(!hasFault());

    assertSoon([this]() { return manager().getFaultState() == FaultState::kOk; });
    ASSERT(initialHealthCheckFuture.isReady());

    manager().acceptTest(HealthCheckStatus(faultFacetType, 1.0, "error"));
    advanceTime(Milliseconds(100));
    assertSoon([this]() {
        return hasFault() && manager().getFaultState() == FaultState::kTransientFault;
    });

    manager().acceptTest(HealthCheckStatus(faultFacetType));
    advanceTime(Milliseconds(100));

    ASSERT(!hasFault());
    assertSoon([this]() { return manager().getFaultState() == FaultState::kOk; });
}

TEST_F(FaultManagerTest, OneFacetIsResolved) {
    RAIIServerParameterControllerForTest _controller{"featureFlagHealthMonitoring", true};
    registerMockHealthObserver(FaultFacetType::kMock1, [] { return 1.1; });
    registerMockHealthObserver(FaultFacetType::kMock2, [] { return 1.1; });


    auto initialHealthCheckFuture = manager().startPeriodicHealthChecks();

    ASSERT(manager().getFaultState() == FaultState::kStartupCheck);
    manager().acceptTest(HealthCheckStatus(FaultFacetType::kMock1, 1.1, "failing health check 1"));
    manager().acceptTest(HealthCheckStatus(FaultFacetType::kMock2, 1.1, "failing health check 2"));
    assertSoon([this] {
        return manager().getOrCreateFaultFacetsContainerTest()->getFacets().size() == 2;
    });
    manager().acceptTest(HealthCheckStatus(FaultFacetType::kMock1));
    assertSoon([this] {
        return manager().getOrCreateFaultFacetsContainerTest()->getFacets().front()->getType() ==
            FaultFacetType::kMock2;
    });
    ASSERT(manager().getFaultState() == FaultState::kStartupCheck);
}

DEATH_TEST_F(FaultManagerTest, TransitionsToActiveFaultAfterTimeoutFromTransientFault, "Fatal") {
    RAIIServerParameterControllerForTest _controller{"featureFlagHealthMonitoring", true};
    auto faultFacetType = FaultFacetType::kMock1;
    auto config = test::getConfigWithDisabledPeriodicChecks();
    auto activeFaultDuration = Milliseconds(100);
    config->setActiveFaultDurationForTests(activeFaultDuration);
    resetManager(std::move(config));

    registerMockHealthObserver(faultFacetType, [] { return 1.1; });
    auto initialHealthCheckFuture = manager().startPeriodicHealthChecks();
    manager().acceptTest(HealthCheckStatus(faultFacetType));
    ASSERT(manager().getFaultState() == FaultState::kOk);

    manager().acceptTest(HealthCheckStatus(faultFacetType, 1.0, "error"));
    ASSERT(manager().getFaultState() == FaultState::kTransientFault);

    advanceTime(activeFaultDuration);
    waitForTransitionIntoState(FaultState::kActiveFault);
}

DEATH_TEST_F(FaultManagerTest, TransitionsToActiveFaultAfterTimeoutFromStartupCheck, "Fatal") {
    RAIIServerParameterControllerForTest _controller{"featureFlagHealthMonitoring", true};
    auto faultFacetType = FaultFacetType::kMock1;
    auto config = test::getConfigWithDisabledPeriodicChecks();
    auto activeFaultDuration = Milliseconds(100);
    config->setActiveFaultDurationForTests(activeFaultDuration);
    resetManager(std::move(config));

    registerMockHealthObserver(faultFacetType, [] { return 1.1; });
    auto initialHealthCheckFuture = manager().startPeriodicHealthChecks();
    manager().acceptTest(HealthCheckStatus(faultFacetType, 1.0, "error"));
    ASSERT(manager().getFaultState() == FaultState::kStartupCheck);

    advanceTime(activeFaultDuration);
    waitForTransitionIntoState(FaultState::kActiveFault);
}

TEST_F(FaultManagerTest, DoesNotTransitionToActiveFaultIfResolved) {
    RAIIServerParameterControllerForTest _controller{"featureFlagHealthMonitoring", true};
    auto faultFacetType = FaultFacetType::kMock1;
    auto config = test::getConfigWithDisabledPeriodicChecks();
    auto activeFaultDuration = Milliseconds(100);
    config->setActiveFaultDurationForTests(activeFaultDuration);
    resetManager(std::move(config));

    registerMockHealthObserver(faultFacetType, [] { return 1.1; });
    auto initialHealthCheckFuture = manager().startPeriodicHealthChecks();
    manager().acceptTest(HealthCheckStatus(faultFacetType));
    ASSERT(manager().getFaultState() == FaultState::kOk);

    manager().acceptTest(HealthCheckStatus(faultFacetType, 1.0, "error"));
    ASSERT(manager().getFaultState() == FaultState::kTransientFault);

    advanceTime(activeFaultDuration / 2);
    manager().acceptTest(HealthCheckStatus(faultFacetType));

    advanceTime(activeFaultDuration);

    ASSERT(manager().getFaultState() == FaultState::kOk);
}

TEST_F(FaultManagerTest, HealthCheckWithOffFacetCreatesNoFault) {
    RAIIServerParameterControllerForTest _controller{"featureFlagHealthMonitoring", true};
    const auto faultFacetType = FaultFacetType::kMock1;
    auto config = std::make_unique<FaultManagerConfig>();
    config->setIntensityForType(faultFacetType, HealthObserverIntensityEnum::kOff);
    resetManager(std::move(config));

    registerMockHealthObserver(faultFacetType, [] { return 1.0; });
    // kSystem is enabled.
    registerMockHealthObserver(FaultFacetType::kSystem, [] { return 0; });

    ASSERT(manager().getFaultState() == FaultState::kStartupCheck);
    auto initialHealthCheckFuture = manager().startPeriodicHealthChecks();

    assertSoon([this]() { return manager().getFaultState() == FaultState::kOk; });
    ASSERT(initialHealthCheckFuture.isReady());
}

TEST_F(FaultManagerTest, AllOffFacetsSkipStartupCheck) {
    RAIIServerParameterControllerForTest _controller{"featureFlagHealthMonitoring", true};
    const auto faultFacetType = FaultFacetType::kMock1;
    auto config = std::make_unique<FaultManagerConfig>();
    config->setIntensityForType(faultFacetType, HealthObserverIntensityEnum::kOff);
    resetManager(std::move(config));

    registerMockHealthObserver(faultFacetType, [] { return 0; });

    auto initialHealthCheckFuture = manager().startPeriodicHealthChecks();

    assertSoon([this]() { return manager().getFaultState() == FaultState::kOk; });
}

TEST_F(FaultManagerTest, HealthCheckWithOffFacetCreatesNoFaultInOk) {
    RAIIServerParameterControllerForTest _controller{"featureFlagHealthMonitoring", true};
    const auto faultFacetType = FaultFacetType::kMock1;
    auto config = std::make_unique<FaultManagerConfig>();
    config->disablePeriodicChecksForTests();
    auto configPtr = config.get();
    resetManager(std::move(config));

    registerMockHealthObserver(faultFacetType, [] { return 0; });

    auto initialHealthCheckFuture = manager().startPeriodicHealthChecks();

    ASSERT(manager().getFaultState() == FaultState::kStartupCheck);
    manager().acceptTest(HealthCheckStatus(faultFacetType));
    advanceTime(Milliseconds(100));
    ASSERT(!hasFault());

    assertSoon([this]() { return manager().getFaultState() == FaultState::kOk; });
    ASSERT(initialHealthCheckFuture.isReady());

    configPtr->setIntensityForType(faultFacetType, HealthObserverIntensityEnum::kOff);
    manager().acceptTest(HealthCheckStatus(faultFacetType, 1.0, "error"));
    ASSERT(manager().getFaultState() == FaultState::kOk);
}

}  // namespace
}  // namespace process_health
}  // namespace mongo
