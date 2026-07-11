// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/process_health/dns_health_observer.h"
#include "mongo/db/process_health/fault.h"
#include "mongo/db/process_health/fault_facet.h"
#include "mongo/db/process_health/fault_manager_config.h"
#include "mongo/db/process_health/fault_manager_test_suite.h"
#include "mongo/db/process_health/health_check_status.h"
#include "mongo/db/process_health/health_monitoring_server_parameters_gen.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/tenant_id.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/time_support.h"

#include <memory>
#include <ratio>
#include <string>
#include <utility>
#include <vector>

namespace mongo {

namespace process_health {

using test::FaultManagerTest;

namespace {

TEST_F(FaultManagerTest, TransitionsFromStartupCheckToOkWhenAllObserversAreSuccessful) {
    registerMockHealthObserver(FaultFacetType::kMock1, [] { return Severity::kOk; });
    registerMockHealthObserver(FaultFacetType::kMock2, [] { return Severity::kOk; });

    auto initialHealthCheckFuture = manager().startPeriodicHealthChecks();
    std::vector<FaultFacetType> faultFacetTypes{FaultFacetType::kMock1, FaultFacetType::kMock2};

    ASSERT_EQ(manager().getFaultState(), FaultState::kStartupCheck);

    // send successful health check response from each
    for (auto faultFacetType : faultFacetTypes) {
        manager().acceptTest(HealthCheckStatus(faultFacetType));
        advanceTime(Milliseconds(100));
        ASSERT(!hasFault());
        if (faultFacetType != faultFacetTypes.back()) {
            ASSERT_EQ(manager().getFaultState(), FaultState::kStartupCheck);
        }
    }

    assertSoon([this]() { return manager().getFaultState() == FaultState::kOk; });
    ASSERT(initialHealthCheckFuture.isReady());
}

TEST_F(FaultManagerTest, TransitionsFromStartupCheckToOkAfterFailureThenSuccess) {
    const auto faultFacetType = FaultFacetType::kMock1;
    registerMockHealthObserver(faultFacetType, [] { return Severity::kOk; });

    auto initialHealthCheckFuture = manager().startPeriodicHealthChecks();

    ASSERT_EQ(manager().getFaultState(), FaultState::kStartupCheck);
    manager().acceptTest(HealthCheckStatus(faultFacetType, Severity::kFailure, "error"));
    advanceTime(Milliseconds(100));
    ASSERT_EQ(manager().getFaultState(), FaultState::kStartupCheck);
    ASSERT(hasFault());
    manager().acceptTest(HealthCheckStatus(faultFacetType));
    advanceTime(Milliseconds(100));
    ASSERT(!hasFault());

    assertSoon([this]() { return manager().getFaultState() == FaultState::kOk; });
    ASSERT(initialHealthCheckFuture.isReady());
}

TEST_F(FaultManagerTest, TransitionsFromOkToTransientFaultAfterSuccessThenFailure) {
    const auto faultFacetType = FaultFacetType::kMock1;
    registerMockHealthObserver(faultFacetType, [] { return Severity::kOk; });

    auto initialHealthCheckFuture = manager().startPeriodicHealthChecks();

    ASSERT_EQ(manager().getFaultState(), FaultState::kStartupCheck);
    manager().acceptTest(HealthCheckStatus(faultFacetType));
    advanceTime(Milliseconds(100));
    ASSERT(!hasFault());

    assertSoon([this]() { return manager().getFaultState() == FaultState::kOk; });
    ASSERT(initialHealthCheckFuture.isReady());

    manager().acceptTest(HealthCheckStatus(faultFacetType, Severity::kFailure, "error"));
    advanceTime(Milliseconds(100));
    assertSoon([this]() {
        return hasFault() && manager().getFaultState() == FaultState::kTransientFault;
    });
}

TEST_F(FaultManagerTest, StaysInOkOnSuccess) {
    const auto faultFacetType = FaultFacetType::kMock1;
    registerMockHealthObserver(faultFacetType, [] { return Severity::kOk; });

    auto initialHealthCheckFuture = manager().startPeriodicHealthChecks();

    ASSERT_EQ(manager().getFaultState(), FaultState::kStartupCheck);
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
    const auto faultFacetType = FaultFacetType::kMock1;
    registerMockHealthObserver(faultFacetType, [] { return Severity::kOk; });

    auto initialHealthCheckFuture = manager().startPeriodicHealthChecks();

    ASSERT_EQ(manager().getFaultState(), FaultState::kStartupCheck);
    manager().acceptTest(HealthCheckStatus(faultFacetType));
    advanceTime(Milliseconds(100));
    ASSERT(!hasFault());

    assertSoon([this]() { return manager().getFaultState() == FaultState::kOk; });
    ASSERT(initialHealthCheckFuture.isReady());

    manager().acceptTest(HealthCheckStatus(faultFacetType, Severity::kFailure, "error"));
    advanceTime(Milliseconds(100));
    assertSoon([this]() {
        return hasFault() && manager().getFaultState() == FaultState::kTransientFault;
    });

    manager().acceptTest(HealthCheckStatus(faultFacetType, Severity::kFailure, "error"));
    advanceTime(Milliseconds(100));
    assertSoon([this]() {
        return hasFault() && manager().getFaultState() == FaultState::kTransientFault;
    });
}

TEST_F(FaultManagerTest, TransitionsFromTransientFaultToOkOnFailureThenSuccess) {
    const auto faultFacetType = FaultFacetType::kMock1;
    registerMockHealthObserver(faultFacetType, [] { return Severity::kOk; });

    auto initialHealthCheckFuture = manager().startPeriodicHealthChecks();

    ASSERT_EQ(manager().getFaultState(), FaultState::kStartupCheck);
    manager().acceptTest(HealthCheckStatus(faultFacetType));
    advanceTime(Milliseconds(100));
    ASSERT(!hasFault());

    assertSoon([this]() { return manager().getFaultState() == FaultState::kOk; });
    ASSERT(initialHealthCheckFuture.isReady());

    manager().acceptTest(HealthCheckStatus(faultFacetType, Severity::kFailure, "error"));
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
    registerMockHealthObserver(FaultFacetType::kMock1, [] { return Severity::kFailure; });
    registerMockHealthObserver(FaultFacetType::kMock2, [] { return Severity::kFailure; });


    auto initialHealthCheckFuture = manager().startPeriodicHealthChecks();

    ASSERT_EQ(manager().getFaultState(), FaultState::kStartupCheck);
    manager().acceptTest(
        HealthCheckStatus(FaultFacetType::kMock1, Severity::kFailure, "failing health check 1"));
    manager().acceptTest(
        HealthCheckStatus(FaultFacetType::kMock2, Severity::kFailure, "failing health check 2"));
    assertSoon([this] { return manager().getOrCreateFaultTest()->getFacets().size() == 2; });

    manager().acceptTest(HealthCheckStatus(FaultFacetType::kMock1));
    assertSoon([this] {
        return manager().getOrCreateFaultTest()->getFacets().front()->getType() ==
            FaultFacetType::kMock2;
    });
    ASSERT_EQ(manager().getFaultState(), FaultState::kStartupCheck);
}

using FaultManagerTestDeathTest = FaultManagerTest;
DEATH_TEST_F(FaultManagerTestDeathTest,
             TransitionsToActiveFaultAfterTimeoutFromTransientFault,
             "Fatal") {
    auto faultFacetType = FaultFacetType::kMock1;

    registerMockHealthObserver(faultFacetType, [] { return Severity::kFailure; });
    auto initialHealthCheckFuture = manager().startPeriodicHealthChecks();
    manager().acceptTest(HealthCheckStatus(faultFacetType));
    ASSERT_EQ(manager().getFaultState(), FaultState::kOk);

    manager().acceptTest(HealthCheckStatus(faultFacetType, Severity::kFailure, "error"));
    ASSERT_EQ(manager().getFaultState(), FaultState::kTransientFault);

    advanceTime(Seconds(kActiveFaultDurationSecs));
    waitForTransitionIntoState(FaultState::kActiveFault);
}

TEST_F(FaultManagerTest,
       NonCriticalFacetDoesNotTransitionToActiveFaultAfterTimeoutFromTransientFault) {
    auto faultFacetType = FaultFacetType::kMock1;
    auto config = test::getConfigWithDisabledPeriodicChecks();
    config->setIntensityForType(faultFacetType, HealthObserverIntensityEnum::kNonCritical);
    resetManager(std::move(config));

    registerMockHealthObserver(faultFacetType, [] { return Severity::kFailure; });
    auto initialHealthCheckFuture = manager().startPeriodicHealthChecks();
    manager().acceptTest(HealthCheckStatus(faultFacetType));
    ASSERT_EQ(manager().getFaultState(), FaultState::kOk);

    manager().acceptTest(HealthCheckStatus(faultFacetType, Severity::kFailure, "error"));
    ASSERT_EQ(manager().getFaultState(), FaultState::kTransientFault);

    advanceTime(Seconds(kActiveFaultDurationSecs));
    // Should be enough time to move to Active fault if we were going to crash.
    sleepFor(Seconds(1));
    ASSERT_EQ(manager().getFaultState(), FaultState::kTransientFault);
}

DEATH_TEST_F(FaultManagerTestDeathTest,
             TransitionsToActiveFaultAfterTimeoutFromStartupCheck,
             "Fatal") {
    auto faultFacetType = FaultFacetType::kMock1;

    registerMockHealthObserver(faultFacetType, [] { return Severity::kFailure; });
    auto initialHealthCheckFuture = manager().startPeriodicHealthChecks();
    manager().acceptTest(HealthCheckStatus(faultFacetType, Severity::kFailure, "error"));
    ASSERT_EQ(manager().getFaultState(), FaultState::kStartupCheck);

    advanceTime(Seconds(kActiveFaultDurationSecs));
    waitForTransitionIntoState(FaultState::kActiveFault);
}

TEST_F(FaultManagerTest,
       NonCriticalFacetDoesNotTransitionToActiveFaultAfterTimeoutFromStartupCheck) {
    auto faultFacetType = FaultFacetType::kMock1;
    auto config = test::getConfigWithDisabledPeriodicChecks();
    config->setIntensityForType(faultFacetType, HealthObserverIntensityEnum::kNonCritical);
    resetManager(std::move(config));

    registerMockHealthObserver(faultFacetType, [] { return Severity::kFailure; });
    auto initialHealthCheckFuture = manager().startPeriodicHealthChecks();
    manager().acceptTest(HealthCheckStatus(faultFacetType, Severity::kFailure, "error"));
    ASSERT_EQ(manager().getFaultState(), FaultState::kStartupCheck);

    advanceTime(Seconds(kActiveFaultDurationSecs) * 10);
    // Should be enough time to move to Active fault if we were going to crash.
    sleepFor(Seconds(1));
    ASSERT_EQ(manager().getFaultState(), FaultState::kStartupCheck);
}

TEST_F(FaultManagerTest, DoesNotTransitionToActiveFaultIfResolved) {
    auto faultFacetType = FaultFacetType::kMock1;

    registerMockHealthObserver(faultFacetType, [] { return Severity::kFailure; });
    auto initialHealthCheckFuture = manager().startPeriodicHealthChecks();
    manager().acceptTest(HealthCheckStatus(faultFacetType));
    ASSERT_EQ(manager().getFaultState(), FaultState::kOk);

    manager().acceptTest(HealthCheckStatus(faultFacetType, Severity::kFailure, "error"));
    ASSERT_EQ(manager().getFaultState(), FaultState::kTransientFault);

    advanceTime(Seconds(kActiveFaultDurationSecs / 2));
    manager().acceptTest(HealthCheckStatus(faultFacetType));

    advanceTime(Seconds(kActiveFaultDurationSecs));

    ASSERT_EQ(manager().getFaultState(), FaultState::kOk);
}

TEST_F(FaultManagerTest, HealthCheckWithOffFacetCreatesNoFault) {
    const auto faultFacetType = FaultFacetType::kMock1;
    auto config = std::make_unique<FaultManagerConfig>();
    config->setIntensityForType(faultFacetType, HealthObserverIntensityEnum::kOff);
    resetManager(std::move(config));

    registerMockHealthObserver(faultFacetType, [] { return Severity::kFailure; });
    // kSystem is enabled.
    registerMockHealthObserver(FaultFacetType::kSystem, [] { return Severity::kOk; });

    ASSERT_EQ(manager().getFaultState(), FaultState::kStartupCheck);
    auto initialHealthCheckFuture = manager().startPeriodicHealthChecks();

    assertSoon([this]() { return manager().getFaultState() == FaultState::kOk; });
    ASSERT(initialHealthCheckFuture.isReady());
}

TEST_F(FaultManagerTest, AllOffFacetsSkipStartupCheck) {
    const auto faultFacetType = FaultFacetType::kMock1;
    auto config = std::make_unique<FaultManagerConfig>();
    config->setIntensityForType(faultFacetType, HealthObserverIntensityEnum::kOff);
    resetManager(std::move(config));

    registerMockHealthObserver(faultFacetType, [] { return Severity::kOk; });

    auto initialHealthCheckFuture = manager().startPeriodicHealthChecks();

    assertSoon([this]() { return manager().getFaultState() == FaultState::kOk; });
}

TEST_F(FaultManagerTest, HealthCheckWithOffFacetCreatesNoFaultInOk) {
    const auto faultFacetType = FaultFacetType::kMock1;
    auto config = std::make_unique<FaultManagerConfig>();
    config->disablePeriodicChecksForTests();
    auto configPtr = config.get();
    resetManager(std::move(config));

    registerMockHealthObserver(faultFacetType, [] { return Severity::kOk; });

    auto initialHealthCheckFuture = manager().startPeriodicHealthChecks();

    ASSERT_EQ(manager().getFaultState(), FaultState::kStartupCheck);
    manager().acceptTest(HealthCheckStatus(faultFacetType));
    advanceTime(Milliseconds(100));
    ASSERT(!hasFault());

    assertSoon([this]() { return manager().getFaultState() == FaultState::kOk; });
    ASSERT(initialHealthCheckFuture.isReady());

    configPtr->setIntensityForType(faultFacetType, HealthObserverIntensityEnum::kOff);
    manager().acceptTest(HealthCheckStatus(faultFacetType, Severity::kFailure, "error"));
    ASSERT_EQ(manager().getFaultState(), FaultState::kOk);
    resetManager();
}

TEST_F(FaultManagerTest, DNSHealthCheckWithBadHostNameFailsAndGoodHostNameSuccess) {
    unittest::ServerParameterGuard serverParamController{"activeFaultDurationSecs", 30};
    const auto faultFacetType = FaultFacetType::kDns;
    auto config = std::make_unique<FaultManagerConfig>();
    config->setIntensityForType(faultFacetType, HealthObserverIntensityEnum::kCritical);
    resetManager(std::move(config));

    auto serverParam =
        ServerParameterSet::getNodeParameterSet()->get<PeriodicHealthCheckIntervalsServerParameter>(
            "healthMonitoringIntervals");
    auto bsonOBj = BSON("values" << BSON_ARRAY(BSON("type" << "dns"
                                                           << "interval" << 1000)));
    const BSONObj newParameterObj = BSON("key" << bsonOBj);
    auto element = newParameterObj.getField("key");
    uassertStatusOK(serverParam->set(element, boost::none));

    registerHealthObserver<DnsHealthObserver>();
    globalFailPointRegistry()
        .find("dnsHealthObserverFp")
        ->setMode(FailPoint::alwaysOn, 0, BSON("hostname" << "yahoo.com"));

    auto initialHealthCheckFuture = manager().startPeriodicHealthChecks();
    assertSoon([this]() { return manager().getFaultState() == FaultState::kOk; });

    globalFailPointRegistry()
        .find("dnsHealthObserverFp")
        ->setMode(FailPoint::alwaysOn, 0, BSON("hostname" << "badhostname.invalid"));
    sleepFor(Seconds(1));
    assertSoon([this]() { return manager().getFaultState() == FaultState::kTransientFault; });

    globalFailPointRegistry()
        .find("dnsHealthObserverFp")
        ->setMode(FailPoint::alwaysOn, 0, BSON("hostname" << "yahoo.com"));
    assertSoon([this]() { return manager().getFaultState() == FaultState::kOk; });
}

}  // namespace
}  // namespace process_health
}  // namespace mongo
