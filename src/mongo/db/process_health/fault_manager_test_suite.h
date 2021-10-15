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
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/tick_source_mock.h"

namespace mongo {

using executor::TaskExecutor;
using executor::ThreadPoolExecutorTest;

namespace process_health {

namespace test {

/**
 * Test wrapper class for FaultManager that has access to protected methods
 * for testing.
 */
class FaultManagerTestImpl : public FaultManager {
public:
    FaultManagerTestImpl(ServiceContext* svcCtx,
                         std::shared_ptr<executor::TaskExecutor> taskExecutor)
        : FaultManager(svcCtx, taskExecutor, std::make_unique<FaultManagerConfig>()) {}

    void transitionStateTest(FaultState newState) {
        transitionToState(newState);
    }

    void healthCheckTest() {
        healthCheck();
    }

    std::vector<HealthObserver*> getHealthObserversTest() {
        return getHealthObservers();
    }

    void processFaultExistsEventTest() {
        processFaultExistsEvent();
    }

    void processFaultIsResolvedEventTest() {
        return processFaultIsResolvedEvent();
    }

    FaultFacetsContainerPtr getOrCreateFaultFacetsContainerTest() {
        return getOrCreateFaultFacetsContainer();
    }

    FaultInternal& getFault() {
        FaultFacetsContainerPtr fault = getFaultFacetsContainer();
        invariant(fault);
        return *(static_cast<FaultInternal*>(fault.get()));
    }

    FaultManagerConfig* getConfig() {
        return _config.get();
    }
};

/**
 * Test suite for fault manager.
 */
class FaultManagerTest : public unittest::Test {
public:
    void setUp() override {
        RAIIServerParameterControllerForTest _controller{"featureFlagHealthMonitoring", true};
        HealthObserverRegistration::resetObserverFactoriesForTest();

        _svcCtx = ServiceContext::make();
        _svcCtx->setFastClockSource(std::make_unique<ClockSourceMock>());
        _svcCtx->setTickSource(std::make_unique<TickSourceMock<Milliseconds>>());

        resetManager();
        _executor->startup();
    }

    void tearDown() override {
        LOGV2(6007905, "Clean up test resources");
        // Shutdown the executor before the context is deleted.
        resetManager();
    }

    void resetManager() {
        // Construct task executor
        auto network = std::make_unique<executor::NetworkInterfaceMock>();
        _net = network.get();
        _executor = makeSharedThreadPoolTestExecutor(std::move(network));

        invariant(_svcCtx->getFastClockSource());
        FaultManager::set(_svcCtx.get(),
                          std::make_unique<FaultManagerTestImpl>(_svcCtx.get(), _executor));
    }

    void registerMockHealthObserver(FaultFacetType mockType,
                                    std::function<double()> getSeverityCallback) {
        HealthObserverRegistration::registerObserverFactory(
            [mockType, getSeverityCallback](ClockSource* clockSource, TickSource* tickSource) {
                return std::make_unique<HealthObserverMock>(
                    mockType, clockSource, tickSource, getSeverityCallback);
            });
    }

    FaultManagerTestImpl& manager() {
        return *static_cast<FaultManagerTestImpl*>(FaultManager::get(_svcCtx.get()));
    }

    ClockSourceMock& clockSource() {
        return *static_cast<ClockSourceMock*>(_svcCtx->getFastClockSource());
    }

    TickSourceMock<Milliseconds>& tickSource() {
        return *static_cast<TickSourceMock<Milliseconds>*>(_svcCtx->getTickSource());
    }

    template <typename Duration>
    void advanceTime(Duration d) {
        executor::NetworkInterfaceMock::InNetworkGuard guard(_net);
        _net->advanceTime(_net->now() + d);
        clockSource().advance(d);
        tickSource().advance(d);
    }

    void assertInvalidStateTransition(FaultState newState) {
        try {
            manager().transitionStateTest(newState);
            ASSERT(false);
        } catch (const DBException& ex) {
            ASSERT(ex.code() == ErrorCodes::BadValue);
            // expected exception
        }
    }

    static inline const Seconds kWaitTimeout{30};
    static inline const Milliseconds kSleepTime{1};
    void assertSoon(std::function<bool()> predicate, Milliseconds timeout = kWaitTimeout) {
        Timer t;
        while (t.elapsed() < timeout) {
            if (predicate())
                return;
            sleepFor(kSleepTime);
        }
        ASSERT(false);
    }

    static inline const Milliseconds kCheckTimeIncrement{100};
    void assertSoonWithHealthCheck(std::function<bool()> predicate,
                                   Milliseconds timeout = kWaitTimeout) {
        auto predicate2 = [=]() {
            if (predicate())
                return true;
            else {
                advanceTime(kCheckTimeIncrement);
                manager().healthCheckTest();
                return false;
            }
        };
        assertSoon(predicate2, timeout);
    }

    bool hasFault() {
        return static_cast<bool>(manager().currentFault());
    }

    void waitForFaultBeingResolved() {
        assertSoon([this]() { return !hasFault(); });
    }

    void waitForFaultBeingCreated() {
        assertSoon([this]() { return hasFault(); });
    }

    void waitForTransitionIntoState(FaultState state) {
        assertSoonWithHealthCheck([=]() { return manager().getFaultState() == state; });
    }

private:
    ServiceContext::UniqueServiceContext _svcCtx;
    executor::NetworkInterfaceMock* _net;
    std::shared_ptr<executor::ThreadPoolTaskExecutor> _executor;
};

}  // namespace test
}  // namespace process_health
}  // namespace mongo
