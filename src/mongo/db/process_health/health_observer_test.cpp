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

namespace mongo {

namespace process_health {

// Using the common fault manager test suite.
using test::FaultManagerTest;

namespace {

class FaultManagerTestWithObserversReset : public FaultManagerTest {
public:
    void setUp() override {
        HealthObserverRegistration::resetObserverFactoriesForTest();
        FaultManagerTest::setUp();
    }
};

// Tests that the mock observer is registered properly.
// This test requires that actual production health observers (e.g. Ldap)
// are not linked with this test, otherwise the count of observers returned
// by the instantiate method below will be greater than expected.
TEST_F(FaultManagerTestWithObserversReset, Registration) {
    registerMockHealthObserver(FaultFacetType::kMock1, [] { return 0; });
    auto allObservers = HealthObserverRegistration::instantiateAllObservers(&clockSource());
    ASSERT_EQ(1, allObservers.size());
    ASSERT_EQ(FaultFacetType::kMock1, allObservers[0]->getType());
}

TEST_F(FaultManagerTestWithObserversReset, HealthCheckCreatesObservers) {
    registerMockHealthObserver(FaultFacetType::kMock1, [] { return 0.1; });
    ASSERT_EQ(0, manager().getHealthObserversTest().size());

    // Trigger periodic health check.
    manager().healthCheckTest();
    ASSERT_EQ(1, manager().getHealthObserversTest().size());
}

TEST_F(FaultManagerTestWithObserversReset, HealthCheckCreatesFacetOnHealthCheckFoundFault) {
    registerMockHealthObserver(FaultFacetType::kMock1, [] { return 0.1; });
    auto activeFault = manager().activeFault();
    ASSERT_TRUE(!activeFault);  // Not created yet.

    manager().healthCheckTest();
    activeFault = manager().activeFault();
    ASSERT_TRUE(activeFault);  // Is created.
}

TEST_F(FaultManagerTestWithObserversReset, StateTransitionOnHealthCheckFoundFault) {
    registerMockHealthObserver(FaultFacetType::kMock1, [] { return 0.1; });
    ASSERT_EQ(FaultState::kStartupCheck, manager().getFaultState());

    manager().healthCheckTest();
    ASSERT_EQ(FaultState::kTransientFault, manager().getFaultState());
}

TEST_F(FaultManagerTestWithObserversReset, HealthCheckCreatesCorrectFacetOnHealthCheckFoundFault) {
    registerMockHealthObserver(FaultFacetType::kMock1, [] { return 0.1; });
    registerMockHealthObserver(FaultFacetType::kMock2, [] { return 0.0; });
    manager().healthCheckTest();
    auto activeFault = manager().activeFault();
    ASSERT_TRUE(activeFault);  // Is created.

    FaultInternal& internalFault = manager().getFault();
    ASSERT_TRUE(internalFault.getFaultFacet(FaultFacetType::kMock1));
    ASSERT_FALSE(internalFault.getFaultFacet(FaultFacetType::kMock2));
}

TEST_F(FaultManagerTestWithObserversReset, SeverityIsMaxFromAllFacetsSeverity) {
    registerMockHealthObserver(FaultFacetType::kMock1, [] { return 0.8; });
    registerMockHealthObserver(FaultFacetType::kMock2, [] { return 0.5; });
    manager().healthCheckTest();
    auto activeFault = manager().activeFault();

    ASSERT_APPROX_EQUAL(0.8, activeFault->getSeverity(), 0.001);
}

TEST_F(FaultManagerTestWithObserversReset, HealthCheckCreatesFacetThenIsGarbageCollected) {
    AtomicDouble severity{0.1};
    registerMockHealthObserver(FaultFacetType::kMock1, [&severity] { return severity.load(); });
    manager().healthCheckTest();
    ASSERT_TRUE(manager().activeFault());  // Is created.

    // Resolve and it should be garbage collected.
    severity.store(0.0);
    manager().healthCheckTest();
    ASSERT_FALSE(manager().activeFault());
}

TEST_F(FaultManagerTestWithObserversReset, StateTransitionOnGarbageCollection) {
    AtomicDouble severity{0.1};
    registerMockHealthObserver(FaultFacetType::kMock1, [&severity] { return severity.load(); });
    manager().healthCheckTest();
    ASSERT_EQ(FaultState::kTransientFault, manager().getFaultState());

    // Resolve, it should be garbage collected and the state should change.
    severity.store(0.0);
    manager().healthCheckTest();
    ASSERT_EQ(FaultState::kOk, manager().getFaultState());
}

TEST_F(FaultManagerTestWithObserversReset, HealthCheckCreates2FacetsThenIsGarbageCollected) {
    AtomicDouble severity1{0.1};
    AtomicDouble severity2{0.1};
    registerMockHealthObserver(FaultFacetType::kMock1, [&severity1] { return severity1.load(); });
    registerMockHealthObserver(FaultFacetType::kMock2, [&severity2] { return severity2.load(); });
    manager().healthCheckTest();

    FaultInternal& internalFault = manager().getFault();
    ASSERT_TRUE(internalFault.getFaultFacet(FaultFacetType::kMock1));
    ASSERT_TRUE(internalFault.getFaultFacet(FaultFacetType::kMock2));

    // Resolve one facet and it should be garbage collected.
    severity1.store(0.0);
    manager().healthCheckTest();
    ASSERT_FALSE(internalFault.getFaultFacet(FaultFacetType::kMock1));
    ASSERT_TRUE(internalFault.getFaultFacet(FaultFacetType::kMock2));
}

}  // namespace
}  // namespace process_health
}  // namespace mongo
