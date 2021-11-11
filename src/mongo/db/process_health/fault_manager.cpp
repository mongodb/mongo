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

#include "mongo/db/process_health/fault_facet_impl.h"
#include "mongo/db/process_health/fault_impl.h"
#include "mongo/db/process_health/fault_manager_config.h"
#include "mongo/db/process_health/health_monitoring_gen.h"
#include "mongo/db/process_health/health_observer_registration.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_thread_pool.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/util/exit_code.h"

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

        auto faultManager = std::make_unique<FaultManager>(
            svcCtx, taskExecutor, std::make_unique<FaultManagerConfig>());
        FaultManager::set(svcCtx, std::move(faultManager));
    }};

}  // namespace

FaultManager* FaultManager::get(ServiceContext* svcCtx) {
    return sFaultManager(svcCtx).get();
}

void FaultManager::set(ServiceContext* svcCtx, std::unique_ptr<FaultManager> newFaultManager) {
    invariant(newFaultManager);
    auto& faultManager = sFaultManager(svcCtx);
    faultManager = std::move(newFaultManager);
}

FaultManager::TransientFaultDeadline::TransientFaultDeadline(
    FaultManager* faultManager,
    std::shared_ptr<executor::TaskExecutor> executor,
    Milliseconds timeout)
    : cancelActiveFaultTransition(CancellationSource()),
      activeFaultTransition(
          executor->sleepFor(timeout, cancelActiveFaultTransition.token())
              .thenRunOn(executor)
              .then([faultManager]() { faultManager->transitionToState(FaultState::kActiveFault); })
              .onError([](Status status) {
                  LOGV2_WARNING(5937001,
                                "The Fault Manager transient fault deadline was disabled.",
                                "status"_attr = status);
              })) {}

FaultManager::TransientFaultDeadline::~TransientFaultDeadline() {
    if (!cancelActiveFaultTransition.token().isCanceled()) {
        cancelActiveFaultTransition.cancel();
    }
}

FaultManager::FaultManager(ServiceContext* svcCtx,
                           std::shared_ptr<executor::TaskExecutor> taskExecutor,
                           std::unique_ptr<FaultManagerConfig> config)
    : _config(std::move(config)),
      _svcCtx(svcCtx),
      _taskExecutor(taskExecutor),
      _crashCb([](std::string cause) {
          LOGV2_ERROR(5936605,
                      "Fault manager progress monitor is terminating the server",
                      "cause"_attr = cause);
          // This calls the exit_group syscall on Linux
          ::_exit(ExitCode::EXIT_PROCESS_HEALTH_CHECK);
      }) {
    invariant(_svcCtx);
    invariant(_svcCtx->getFastClockSource());
    invariant(_svcCtx->getPreciseClockSource());
    _lastTransitionTime = _svcCtx->getFastClockSource()->now();
}

FaultManager::FaultManager(ServiceContext* svcCtx,
                           std::shared_ptr<executor::TaskExecutor> taskExecutor,
                           std::unique_ptr<FaultManagerConfig> config,
                           std::function<void(std::string cause)> crashCb)
    : _config(std::move(config)), _svcCtx(svcCtx), _taskExecutor(taskExecutor), _crashCb(crashCb) {
    _lastTransitionTime = _svcCtx->getFastClockSource()->now();
}

void FaultManager::schedulePeriodicHealthCheckThread(bool immediately) {
    if (!feature_flags::gFeatureFlagHealthMonitoring.isEnabled(
            serverGlobalParams.featureCompatibility)) {
        return;
    }

    const auto kPeriodicHealthCheckInterval = getConfig().getPeriodicHealthCheckInterval();
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
    _managerShuttingDownCancellationSource.cancel();
    _taskExecutor->shutdown();

    LOGV2(5936601, "Shutting down periodic health checks");
    if (_periodicHealthCheckCbHandle) {
        _taskExecutor->cancel(*_periodicHealthCheckCbHandle);
    }

    // All health checks must use the _taskExecutor, joining it
    // should guarantee that health checks are done. Hovewer, if a health
    // check is stuck in some blocking call the _progressMonitorThread will
    // kill the process after timeout.
    _taskExecutor->join();
    // Must be joined after _taskExecutor.
    if (_progressMonitor) {
        _progressMonitor->join();
    }

    if (!_initialHealthCheckCompletedPromise.getFuture().isReady()) {
        _initialHealthCheckCompletedPromise.emplaceValue();
    }
    LOGV2_DEBUG(6136801, 1, "Done shutting down periodic health checks");
}

void FaultManager::startPeriodicHealthChecks() {
    if (!feature_flags::gFeatureFlagHealthMonitoring.isEnabled(
            serverGlobalParams.featureCompatibility)) {
        return;
    }

    _taskExecutor->startup();
    invariant(getFaultState() == FaultState::kStartupCheck);
    {
        auto lk = stdx::lock_guard(_mutex);
        invariant(!_periodicHealthCheckCbHandle);
    }
    schedulePeriodicHealthCheckThread(true /* immediately */);

    _initialHealthCheckCompletedPromise.getFuture().get();
}

FaultState FaultManager::getFaultState() const {
    stdx::lock_guard<Latch> lk(_stateMutex);
    return _currentState;
}

Date_t FaultManager::getLastTransitionTime() const {
    stdx::lock_guard<Latch> lk(_stateMutex);
    return _lastTransitionTime;
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
    _firstTimeInitIfNeeded();

    ON_BLOCK_EXIT([this] {
        if (!_config->periodicChecksDisabledForTests()) {
            schedulePeriodicHealthCheckThread();
        }
    });

    std::vector<HealthObserver*> observers = FaultManager::getHealthObservers();

    // Start checks outside of lock.
    auto token = _managerShuttingDownCancellationSource.token();
    for (auto observer : observers) {
        // TODO: SERVER-59368, fix bug where health observer is turned off when in transient fault
        // state
        if (_config->getHealthObserverIntensity(observer->getType()) !=
            HealthObserverIntensityEnum::kOff)
            observer->periodicCheck(*this, _taskExecutor, token);
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
    if (HealthCheckStatus::isResolved(checkStatus.getSeverity())) {
        auto container = getFaultFacetsContainer();
        if (container) {
            container->updateWithSuppliedFacet(checkStatus.getType(), nullptr);
        }
        return;
    }

    auto container = getOrCreateFaultFacetsContainer();
    auto facet = container->getFaultFacet(checkStatus.getType());
    if (!facet) {
        const auto type = checkStatus.getType();
        auto newFacet =
            new FaultFacetImpl(type, _svcCtx->getFastClockSource(), std::move(checkStatus));
        container->updateWithSuppliedFacet(type, FaultFacetPtr(newFacet));
    } else {
        facet->update(std::move(checkStatus));
    }
}

void FaultManager::checkForStateTransition() {
    FaultConstPtr fault = currentFault();
    if (fault && !HealthCheckStatus::isResolved(fault->getSeverity())) {
        processFaultExistsEvent();
    } else if (!fault || HealthCheckStatus::isResolved(fault->getSeverity())) {
        processFaultIsResolvedEvent();
    }
}

bool FaultManager::hasCriticalFacet(const FaultInternal* fault) const {
    invariant(fault);
    const auto& facets = fault->getFacets();
    for (const auto& facet : facets) {
        auto facetType = facet->getType();
        if (_config->getHealthObserverIntensity(facetType) ==
            HealthObserverIntensityEnum::kCritical)
            return true;
    }
    return false;
}

FaultManagerConfig FaultManager::getConfig() const {
    auto lk = stdx::lock_guard(_mutex);
    return *_config;
}

void FaultManager::processFaultExistsEvent() {
    FaultState currentState = getFaultState();

    switch (currentState) {
        case FaultState::kStartupCheck:
        case FaultState::kOk: {
            transitionToState(FaultState::kTransientFault);
            if (hasCriticalFacet(_fault.get())) {
                // This will transition the FaultManager to ActiveFault state after the timeout
                // occurs.
                _transientFaultDeadline = std::make_unique<TransientFaultDeadline>(
                    this, _taskExecutor, _config->getActiveFaultDuration());
            }
            break;
        }
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
            transitionToState(FaultState::kOk);
            _initialHealthCheckCompletedPromise.emplaceValue();
            break;
        case FaultState::kTransientFault:
            // Clear the transient fault deadline timer.
            _transientFaultDeadline.reset();
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
    static const stdx::unordered_map<FaultState, std::vector<FaultState>> validTransitions = {
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

    _lastTransitionTime = _svcCtx->getFastClockSource()->now();
    _currentState = newState;
}

void FaultManager::_firstTimeInitIfNeeded() {
    if (_firstTimeInitExecuted.load()) {
        return;
    }

    auto lk = stdx::lock_guard(_mutex);
    // One more time under lock to avoid race.
    if (_firstTimeInitExecuted.load()) {
        return;
    }
    _firstTimeInitExecuted.store(true);

    _observers = HealthObserverRegistration::instantiateAllObservers(_svcCtx);

    // Verify that all observer types are unique.
    std::set<FaultFacetType> allTypes;
    for (const auto& observer : _observers) {
        allTypes.insert(observer->getType());
    }
    invariant(allTypes.size() == _observers.size());

    // Start the monitor thread after all observers are initialized.
    _progressMonitor = std::make_unique<ProgressMonitor>(this, _svcCtx, _crashCb);

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

void FaultManager::progressMonitorCheckForTests(std::function<void(std::string cause)> crashCb) {
    _progressMonitor->progressMonitorCheck(crashCb);
}

}  // namespace process_health
}  // namespace mongo
