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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kProcessHealth

#include "mongo/platform/basic.h"

#include "mongo/db/process_health/fault_manager.h"

#include "mongo/db/process_health/fault_impl.h"
#include "mongo/db/process_health/health_monitoring_gen.h"
#include "mongo/db/process_health/health_observer_registration.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_thread_pool.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/logv2/log.h"

namespace mongo {

namespace process_health {

namespace {

const auto sFaultManager = ServiceContext::declareDecoration<std::unique_ptr<FaultManager>>();

ServiceContext::ConstructorActionRegisterer faultManagerRegisterer{
    "FaultManagerRegisterer", [](ServiceContext* svcCtx) {
        // construct task executor
        std::shared_ptr<executor::NetworkInterface> networkInterface =
            executor::makeNetworkInterface("FaultManager-TaskExecutor");
        auto pool = std::make_unique<executor::NetworkInterfaceThreadPool>(networkInterface.get());
        auto taskExecutor =
            std::make_shared<executor::ThreadPoolTaskExecutor>(std::move(pool), networkInterface);
        taskExecutor->startup();

        auto faultManager = std::make_unique<FaultManager>(svcCtx, taskExecutor);
        FaultManager::set(svcCtx, std::move(faultManager));
    }};

}  // namespace

static constexpr auto kPeriodicHealthCheckInterval{Milliseconds(50)};

FaultManager* FaultManager::get(ServiceContext* svcCtx) {
    return sFaultManager(svcCtx).get();
}

void FaultManager::set(ServiceContext* svcCtx, std::unique_ptr<FaultManager> newFaultManager) {
    invariant(newFaultManager);
    auto& faultManager = sFaultManager(svcCtx);
    faultManager = std::move(newFaultManager);
}

FaultManager::FaultManager(ServiceContext* svcCtx,
                           std::shared_ptr<executor::TaskExecutor> taskExecutor)
    : _svcCtx(svcCtx), _taskExecutor(taskExecutor) {
    invariant(_svcCtx);
    invariant(_svcCtx->getFastClockSource());
}

void FaultManager::schedulePeriodicHealthCheckThread(bool immediately) {
    if (!feature_flags::gFeatureFlagHealthMonitoring.isEnabled(
            serverGlobalParams.featureCompatibility)) {
        return;
    }

    auto lk = stdx::lock_guard(_mutex);

    const auto cb = [this](const mongo::executor::TaskExecutor::CallbackArgs& cbData) {
        if (!cbData.status.isOK()) {
            return;
        }

        healthCheck();
    };

    auto periodicThreadCbHandleStatus = immediately
        ? _taskExecutor->scheduleWork(cb)
        : _taskExecutor->scheduleWorkAt(_taskExecutor->now() + kPeriodicHealthCheckInterval, cb);

    uassert(5936101,
            "Failed to initialize periodic health check work.",
            periodicThreadCbHandleStatus.isOK());
    _periodicHealthCheckCbHandle = periodicThreadCbHandleStatus.getValue();
}

FaultManager::~FaultManager() {
    _taskExecutor->shutdown();
    if (_periodicHealthCheckCbHandle) {
        _taskExecutor->cancel(*_periodicHealthCheckCbHandle);
    }
    _taskExecutor->join();
}

void FaultManager::startPeriodicHealthChecks() {
    invariant(getFaultState() == FaultState::kStartupCheck);
    {
        auto lk = stdx::lock_guard(_mutex);
        invariant(!_periodicHealthCheckCbHandle);
    }
    schedulePeriodicHealthCheckThread(true /* immediately */);
}

FaultState FaultManager::getFaultState() const {
    stdx::lock_guard<Latch> lk(_stateMutex);
    return _currentState;
}

FaultConstPtr FaultManager::currentFault() const {
    auto lk = stdx::lock_guard(_mutex);
    return std::static_pointer_cast<const Fault>(_fault);
}

FaultFacetsContainerPtr FaultManager::getFaultFacetsContainer() const {
    auto lk = stdx::lock_guard(_mutex);
    return std::static_pointer_cast<FaultFacetsContainer>(_fault);
}

FaultFacetsContainerPtr FaultManager::getOrCreateFaultFacetsContainer() {
    auto lk = stdx::lock_guard(_mutex);
    if (!_fault) {
        // Create a new one.
        _fault = std::make_shared<FaultImpl>(_svcCtx->getFastClockSource());
    }
    return std::static_pointer_cast<FaultFacetsContainer>(_fault);
}

void FaultManager::healthCheck() {
    // One time init.
    _initHealthObserversIfNeeded();

    ON_BLOCK_EXIT([this] { schedulePeriodicHealthCheckThread(); });

    std::vector<HealthObserver*> observers = FaultManager::getHealthObservers();

    // Start checks outside of lock.
    for (auto observer : observers) {
        observer->periodicCheck(*this, _taskExecutor);
    }

    // Garbage collect all resolved fault facets.
    auto optionalActiveFault = getFaultFacetsContainer();
    if (optionalActiveFault) {
        optionalActiveFault->garbageCollectResolvedFacets();
    }

    // If the whole fault becomes resolved, garbage collect it
    // with proper locking.
    std::shared_ptr<FaultInternal> faultToDelete;
    {
        auto lk = stdx::lock_guard(_mutex);
        if (_fault && _fault->getFacets().empty()) {
            faultToDelete.swap(_fault);
        }
    }

    // Actions above can result in a state change.
    checkForStateTransition();
}

void FaultManager::updateWithCheckStatus(HealthCheckStatus&& checkStatus) {
    auto container = getFaultFacetsContainer();
    if (HealthCheckStatus::isResolved(checkStatus.getSeverity())) {
        if (container) {
            container->updateWithSuppliedFacet(checkStatus.getType(), nullptr);
        }
        return;
    }

    // TODO(SERVER-60587): implement Facet properly.
    class Impl : public FaultFacet {
    public:
        Impl(HealthCheckStatus status) : _status(status) {}

        FaultFacetType getType() const override {
            return _status.getType();
        }

        HealthCheckStatus getStatus() const override {
            return _status;
        }

    private:
        HealthCheckStatus _status;
    };

    if (!container) {
        // Need to create container first.
        container = getOrCreateFaultFacetsContainer();
    }

    auto facet = container->getFaultFacet(checkStatus.getType());
    if (!facet) {
        auto newFacet = FaultFacetPtr(new Impl(checkStatus));
        container->updateWithSuppliedFacet(checkStatus.getType(), newFacet);
    }
    // TODO(SERVER-60587): update facet with new check status.
}

void FaultManager::checkForStateTransition() {
    FaultConstPtr fault = currentFault();
    if (fault && !HealthCheckStatus::isResolved(fault->getSeverity())) {
        processFaultExistsEvent();
    } else if (!fault || HealthCheckStatus::isResolved(fault->getSeverity())) {
        processFaultIsResolvedEvent();
    }
}

void FaultManager::processFaultExistsEvent() {
    FaultState currentState = getFaultState();

    switch (currentState) {
        case FaultState::kStartupCheck:
        case FaultState::kOk:
            transitionToState(FaultState::kTransientFault);
            break;
        case FaultState::kTransientFault:
        case FaultState::kActiveFault:
            // NOP.
            break;
        default:
            MONGO_UNREACHABLE;
            break;
    }
}

void FaultManager::processFaultIsResolvedEvent() {
    FaultState currentState = getFaultState();

    switch (currentState) {
        case FaultState::kOk:
            // NOP.
            break;
        case FaultState::kStartupCheck:
        case FaultState::kTransientFault:
            transitionToState(FaultState::kOk);
            break;
        case FaultState::kActiveFault:
            // Too late, this state cannot be resolved to Ok.
            break;
        default:
            MONGO_UNREACHABLE;
            break;
    }
}

void FaultManager::transitionToState(FaultState newState) {
    // Maps currentState to valid newStates
    static stdx::unordered_map<FaultState, std::vector<FaultState>> validTransitions = {
        {FaultState::kStartupCheck, {FaultState::kOk, FaultState::kTransientFault}},
        {FaultState::kOk, {FaultState::kTransientFault}},
        {FaultState::kTransientFault, {FaultState::kOk, FaultState::kActiveFault}},
        {FaultState::kActiveFault, {}},
    };

    stdx::lock_guard<Latch> lk(_stateMutex);
    const auto& validStates = validTransitions.at(_currentState);
    auto validIt = std::find(validStates.begin(), validStates.end(), newState);
    uassert(ErrorCodes::BadValue,
            str::stream() << "Invalid fault manager transition from " << _currentState << " to "
                          << newState,
            validIt != validStates.end());

    LOGV2(5936201,
          "Transitioned fault manager state",
          "newState"_attr = str::stream() << newState,
          "oldState"_attr = str::stream() << _currentState);
    _currentState = newState;
}

void FaultManager::_initHealthObserversIfNeeded() {
    if (_initializedAllHealthObservers.load()) {
        return;
    }

    auto lk = stdx::lock_guard(_mutex);
    // One more time under lock to avoid race.
    if (_initializedAllHealthObservers.load()) {
        return;
    }
    _initializedAllHealthObservers.store(true);

    _observers = HealthObserverRegistration::instantiateAllObservers(_svcCtx->getFastClockSource(),
                                                                     _svcCtx->getTickSource());

    // Verify that all observer types are unique.
    std::set<FaultFacetType> allTypes;
    for (const auto& observer : _observers) {
        allTypes.insert(observer->getType());
    }
    invariant(allTypes.size() == _observers.size());

    auto lk2 = stdx::lock_guard(_stateMutex);
    LOGV2(5956701,
          "Instantiated health observers, periodic health checking starts",
          "managerState"_attr = _currentState,
          "observersCount"_attr = _observers.size());
}

std::vector<HealthObserver*> FaultManager::getHealthObservers() {
    std::vector<HealthObserver*> result;
    stdx::lock_guard<Latch> lk(_mutex);
    result.reserve(_observers.size());
    std::transform(_observers.cbegin(),
                   _observers.cend(),
                   std::back_inserter(result),
                   [](const std::unique_ptr<HealthObserver>& value) { return value.get(); });
    return result;
}

StringBuilder& operator<<(StringBuilder& s, const FaultState& state) {
    switch (state) {
        case FaultState::kOk:
            return s << "Ok"_sd;
        case FaultState::kStartupCheck:
            return s << "StartupCheck"_sd;
        case FaultState::kTransientFault:
            return s << "TransientFault"_sd;
        case FaultState::kActiveFault:
            return s << "ActiveFault"_sd;
        default:
            const bool kStateIsValid = false;
            invariant(kStateIsValid);
            return s;
    }
}

std::ostream& operator<<(std::ostream& os, const FaultState& state) {
    StringBuilder sb;
    sb << state;
    return os << sb.stringData();
}

}  // namespace process_health
}  // namespace mongo
