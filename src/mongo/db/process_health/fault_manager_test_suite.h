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

#include "mongo/db/process_health/fault_manager.h"
#include "mongo/db/process_health/health_observer_mock.h"
#include "mongo/db/process_health/health_observer_registration.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/logv2/log.h"
#include "mongo/transport/transport_layer_manager_impl.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/tick_source_mock.h"

#include <memory>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {

using executor::TaskExecutor;
using executor::ThreadPoolExecutorTest;

namespace process_health {

namespace test {

inline std::unique_ptr<FaultManagerConfig> getConfigWithDisabledPeriodicChecks() {
    auto config = std::make_unique<FaultManagerConfig>();
    config->disablePeriodicChecksForTests();
    return config;
}

/**
 * Test wrapper class for FaultManager that has access to protected methods
 * for testing.
 */
class FaultManagerTestImpl : public FaultManager {
public:
    FaultManagerTestImpl(ServiceContext* svcCtx,
                         std::shared_ptr<executor::TaskExecutor> taskExecutor,
                         std::unique_ptr<FaultManagerConfig> config)
        : FaultManager(
              svcCtx,
              taskExecutor,
              [&config]() -> std::unique_ptr<FaultManagerConfig> {
                  if (config)
                      return std::move(config);
                  else
                      return getConfigWithDisabledPeriodicChecks();
              }(),
              [](std::string cause) {
                  // In tests, do not crash.
                  LOGV2(5936606,
                        "Fault manager progress monitor triggered the termination",
                        "cause"_attr = cause);
              }) {}

    void healthCheckTest(HealthObserver* observer, CancellationToken token) {
        healthCheck(observer, token);
    }

    void schedulePeriodicHealthCheckThreadTest() {
        schedulePeriodicHealthCheckThread();
    }

    std::vector<HealthObserver*> getHealthObserversTest() {
        return getHealthObservers();
    }

    FaultPtr getOrCreateFaultTest() {
        return getOrCreateFault();
    }

    Fault& getFault() {
        FaultPtr fault = FaultManager::getFault();
        invariant(fault);
        return *(static_cast<Fault*>(fault.get()));
    }

    void progressMonitorCheckTest(std::function<void(std::string cause)> crashCb) {
        progressMonitorCheckForTests(crashCb);
    }

    const FaultManagerConfig& getConfigTest() {
        return getConfig();
    }

    FaultState acceptTest(const HealthCheckStatus& message) {
        return accept(message);
    }

    void scheduleNextImmediateCheckForTest(HealthObserver* observer) {
        scheduleNextHealthCheck(observer, CancellationToken::uncancelable(), true);
    }
};

/**
 * Test suite for fault manager.
 */
class FaultManagerTest : service_context_test::WithSetupTransportLayer,
                         service_context_test::RouterRoleOverride,
                         public ClockSourceMockServiceContextTest {
public:
    void setUp() override {
        HealthObserverRegistration::resetObserverFactoriesForTest();

        advanceTime(Seconds(100));
        bumpUpLogging();
        resetManager();
    }

    void bumpUpLogging() {
        logv2::LogManager::global().getGlobalSettings().setMinimumLoggedSeverity(
            mongo::logv2::LogComponent::kProcessHealth, logv2::LogSeverity::Debug(3));
        logv2::LogManager::global().getGlobalSettings().setMinimumLoggedSeverity(
            mongo::logv2::LogComponent::kAccessControl, logv2::LogSeverity::Debug(3));
    }

    void tearDown() override {
        LOGV2(6007905, "Clean up test resources");
        // Shutdown the executor before the context is deleted.
        resetManager();
    }

    void constructTaskExecutor() {
        if (_executor) {
            _executor->shutdown();
            _executor->join();
        }

        auto network = std::shared_ptr<executor::NetworkInterface>(
            executor::makeNetworkInterface("FaultManagerTest").release());
        ThreadPool::Options options;
        auto pool = std::make_unique<ThreadPool>(options);

        _executor = executor::ThreadPoolTaskExecutor::create(std::move(pool), std::move(network));
    }

    void resetManager(std::unique_ptr<FaultManagerConfig> config = nullptr) {
        constructTaskExecutor();
        FaultManager::set(getServiceContext(),
                          std::make_unique<FaultManagerTestImpl>(
                              getServiceContext(), _executor, std::move(config)));
    }

    void registerMockHealthObserver(FaultFacetType mockType,
                                    std::function<Severity()> getSeverityCallback,
                                    Milliseconds timeout) {
        HealthObserverRegistration::registerObserverFactory(
            [mockType, getSeverityCallback, timeout](ServiceContext* svcCtx) {
                return std::make_unique<HealthObserverMock>(
                    mockType, svcCtx, getSeverityCallback, timeout);
            });
    }

    void registerMockHealthObserver(FaultFacetType mockType,
                                    std::function<Severity()> getSeverityCallback) {
        registerMockHealthObserver(mockType, getSeverityCallback, Milliseconds(Seconds(30)));
    }

    template <typename Observer>
    void scheduleNextImmediateCheck(FaultFacetType type) {
        auto& obsrv = observer<Observer>(type);
        manager().scheduleNextImmediateCheckForTest(&obsrv);
    }

    template <typename Observer>
    void registerHealthObserver() {
        HealthObserverRegistration::registerObserverFactory(
            [](ServiceContext* svcCtx) { return std::make_unique<Observer>(svcCtx); });
    }

    FaultManagerTestImpl& manager() {
        return *static_cast<FaultManagerTestImpl*>(FaultManager::get(getServiceContext()));
    }

    ClockSourceMock& clockSource() {
        return *static_cast<ClockSourceMock*>(getServiceContext()->getFastClockSource());
    }

    TickSourceMock<Milliseconds>& tickSource() {
        return *static_cast<TickSourceMock<Milliseconds>*>(getServiceContext()->getTickSource());
    }

    template <typename Observer>
    Observer& observer(FaultFacetType type) {
        std::vector<HealthObserver*> observers = manager().getHealthObserversTest();
        ASSERT_TRUE(!observers.empty());
        auto it = std::find_if(observers.begin(), observers.end(), [type](const HealthObserver* o) {
            return o->getType() == type;
        });
        ASSERT_TRUE(it != observers.end());
        return *static_cast<Observer*>(*it);
    }

    HealthObserverBase::PeriodicHealthCheckContext checkContext() {
        HealthObserverBase::PeriodicHealthCheckContext ctx{CancellationToken::uncancelable(),
                                                           _executor};
        return ctx;
    }

    template <typename Duration>
    void advanceTime(Duration d) {
        clockSource().advance(d);
        static_cast<ClockSourceMock*>(getServiceContext()->getPreciseClockSource())->advance(d);
        tickSource().advance(d);
    }

    static inline const Seconds kWaitTimeout{35};
    static inline const Milliseconds kSleepTime{1};

    static inline const int kActiveFaultDurationSecs = 5;

    RAIIServerParameterControllerForTest serverParamController{"activeFaultDurationSecs",
                                                               kActiveFaultDurationSecs};

    void assertSoon(std::function<bool()> predicate, Milliseconds timeout = kWaitTimeout) {
        Timer t;
        while (t.elapsed() < timeout) {
            if (predicate())
                return;
            sleepFor(kSleepTime);
        }
        ASSERT(false);
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
        assertSoon([=, this]() { return manager().getFaultState() == state; });
    }

private:
    std::shared_ptr<executor::ThreadPoolTaskExecutor> _executor;
};

}  // namespace test
}  // namespace process_health
}  // namespace mongo

#undef MONGO_LOGV2_DEFAULT_COMPONENT
