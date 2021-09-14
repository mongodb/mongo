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
#pragma once

#include <memory>

#include "mongo/db/process_health/fault_manager.h"

#include "mongo/db/process_health/health_observer_mock.h"
#include "mongo/db/process_health/health_observer_registration.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace process_health {

namespace test {

/**
 * Test wrapper class for FaultManager that has access to protected methods
 * for testing.
 */
class FaultManagerTestImpl : public FaultManager {
public:
    FaultManagerTestImpl(ServiceContext* svcCtx) : FaultManager(svcCtx) {}

    Status transitionStateTest(FaultState newState) {
        return transitionToState(newState);
    }

    FaultState getFaultStateTest() {
        return getFaultState();
    }

    void healthCheckTest() {
        healthCheck();
    }

    std::vector<HealthObserver*> getHealthObserversTest() {
        return getHealthObservers();
    }

    Status processFaultExistsEventTest() {
        return processFaultExistsEvent();
    }

    Status processFaultIsResolvedEventTest() {
        return processFaultIsResolvedEvent();
    }

    FaultInternal& getFault() {
        FaultFacetsContainerPtr fault = getFaultFacetsContainer();
        invariant(fault);
        return *(static_cast<FaultInternal*>(fault.get()));
    }
};

/**
 * Test suite for fault manager.
 */
class FaultManagerTest : public unittest::Test {
public:
    void setUp() override {
        _svcCtx = ServiceContext::make();
        _svcCtx->setFastClockSource(std::make_unique<ClockSourceMock>());
        resetManager();
    }

    void resetManager() {
        FaultManager::set(_svcCtx.get(), std::make_unique<FaultManagerTestImpl>(_svcCtx.get()));
    }

    void registerMockHealthObserver(FaultFacetType mockType,
                                    std::function<double()> getSeverityCallback) {
        HealthObserverRegistration::registerObserverFactory([mockType, getSeverityCallback](
                                                                ClockSource* clockSource) {
            return std::make_unique<HealthObserverMock>(mockType, clockSource, getSeverityCallback);
        });
    }

    FaultManagerTestImpl& manager() {
        return *static_cast<FaultManagerTestImpl*>(FaultManager::get(_svcCtx.get()));
    }

    ClockSourceMock& clockSource() {
        return *static_cast<ClockSourceMock*>(_svcCtx->getFastClockSource());
    }

private:
    ServiceContext::UniqueServiceContext _svcCtx;
};

}  // namespace test
}  // namespace process_health
}  // namespace mongo
