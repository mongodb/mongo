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
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/exit_code.h"

namespace mongo {

namespace process_health {

namespace {

static constexpr int kMaxThreadPoolSize = 20;

const auto sFaultManager = ServiceContext::declareDecoration<std::unique_ptr<FaultManager>>();

ServiceContext::ConstructorActionRegisterer faultManagerRegisterer{
    "FaultManagerRegisterer", [](ServiceContext* svcCtx) {
        // construct task executor
        std::shared_ptr<executor::NetworkInterface> networkInterface =
            executor::makeNetworkInterface("FaultManager-TaskExecutor");
        ThreadPool::Options threadPoolOptions;
        threadPoolOptions.maxThreads = kMaxThreadPoolSize;
        threadPoolOptions.threadNamePrefix = "FaultManager-";
        threadPoolOptions.poolName = "FaultManagerThreadPool";
        auto pool = std::make_unique<ThreadPool>(threadPoolOptions);
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
      activeFaultTransition(executor->sleepFor(timeout, cancelActiveFaultTransition.token())
                                .thenRunOn(executor)
                                .then([faultManager]() { faultManager->accept(boost::none); })
                                .onError([](Status status) {
                                    LOGV2_DEBUG(
                                        5937001,
                                        1,
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
    : StateMachine(FaultState::kStartupCheck),
      _config(std::move(config)),
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
    setupStateMachine();
}

FaultManager::FaultManager(ServiceContext* svcCtx,
                           std::shared_ptr<executor::TaskExecutor> taskExecutor,
                           std::unique_ptr<FaultManagerConfig> config,
                           std::function<void(std::string cause)> crashCb)
    : StateMachine(FaultState::kStartupCheck),
      _config(std::move(config)),
      _svcCtx(svcCtx),
      _taskExecutor(taskExecutor),
      _crashCb(crashCb) {
    _lastTransitionTime = _svcCtx->getFastClockSource()->now();
    setupStateMachine();
}

void FaultManager::setupStateMachine() {
    validTransitions({
        {FaultState::kStartupCheck, {FaultState::kOk, FaultState::kActiveFault}},
        {FaultState::kOk, {FaultState::kTransientFault}},
        {FaultState::kTransientFault, {FaultState::kOk, FaultState::kActiveFault}},
        {FaultState::kActiveFault, {}},
    });

    auto bindThis = [&](auto&& pmf) { return [=](auto&&... a) { return (this->*pmf)(a...); }; };

    registerHandler(FaultState::kStartupCheck, bindThis(&FaultManager::handleStartupCheck))
        ->enter(bindThis(&FaultManager::logCurrentState))
        ->exit(bindThis(&FaultManager::setInitialHealthCheckComplete));

    registerHandler(FaultState::kOk, bindThis(&FaultManager::handleOk))
        ->enter(bindThis(&FaultManager::clearTransientFaultDeadline))
        ->enter(bindThis(&FaultManager::logCurrentState));

    registerHandler(FaultState::kTransientFault, bindThis(&FaultManager::handleTransientFault))
        ->enter(bindThis(&FaultManager::setTransientFaultDeadline))
        ->enter(bindThis(&FaultManager::logCurrentState));

    registerHandler(FaultState::kActiveFault,
                    bindThis(&FaultManager::handleActiveFault),
                    true /* transient state */)
        ->enter(bindThis(&FaultManager::logCurrentState));

    start();
}

boost::optional<FaultState> FaultManager::handleStartupCheck(const OptionalMessageType& message) {
    if (!message) {
        return FaultState::kActiveFault;
    }

    HealthCheckStatus status = message.get();

    auto activeObservers = getActiveHealthObservers();
    stdx::unordered_set<FaultFacetType> activeObserversTypes;
    std::for_each(activeObservers.begin(),
                  activeObservers.end(),
                  [&activeObserversTypes](HealthObserver* observer) {
                      activeObserversTypes.insert(observer->getType());
                  });


    auto lk = stdx::lock_guard(_stateMutex);
    logMessageReceived(state(), status);

    if (status.isActiveFault()) {
        _healthyObservations.erase(status.getType());
    } else {
        _healthyObservations.insert(status.getType());
    }

    updateWithCheckStatus(HealthCheckStatus(status));
    auto optionalActiveFault = getFaultFacetsContainer();
    if (optionalActiveFault) {
        optionalActiveFault->garbageCollectResolvedFacets();
    }

    if (optionalActiveFault && hasCriticalFacet(_fault.get()) && !_transientFaultDeadline) {
        setTransientFaultDeadline(
            FaultState::kStartupCheck, FaultState::kStartupCheck, boost::none);
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

    if (activeObserversTypes == _healthyObservations) {
        return FaultState::kOk;
    }
    return boost::none;
}

boost::optional<FaultState> FaultManager::handleOk(const OptionalMessageType& message) {
    invariant(message);

    HealthCheckStatus status = message.get();
    auto lk = stdx::lock_guard(_stateMutex);
    logMessageReceived(state(), status);

    if (!_config->isHealthObserverEnabled(status.getType())) {
        return boost::none;
    }

    updateWithCheckStatus(HealthCheckStatus(status));

    if (!HealthCheckStatus::isResolved(status.getSeverity())) {
        return FaultState::kTransientFault;
    }

    return boost::none;
}

boost::optional<FaultState> FaultManager::handleTransientFault(const OptionalMessageType& message) {
    if (!message) {
        return FaultState::kActiveFault;
    }

    HealthCheckStatus status = message.get();
    auto lk = stdx::lock_guard(_stateMutex);
    logMessageReceived(state(), status);

    updateWithCheckStatus(HealthCheckStatus(status));

    auto optionalActiveFault = getFaultFacetsContainer();
    if (optionalActiveFault) {
        optionalActiveFault->garbageCollectResolvedFacets();
    }

    // If the whole fault becomes resolved, garbage collect it
    // with proper locking.
    if (_fault && _fault->getFacets().empty()) {
        _fault.reset();
        return FaultState::kOk;
    }
    return boost::none;
}

boost::optional<FaultState> FaultManager::handleActiveFault(const OptionalMessageType& message) {
    LOGV2_FATAL(5936509, "Fault manager received active fault");
    return boost::none;
}

void FaultManager::logMessageReceived(FaultState state, const HealthCheckStatus& status) {
    LOGV2_DEBUG(5936504,
                1,
                "Fault manager recieved health check result",
                "state"_attr = (str::stream() << state),
                "result"_attr = status,
                "passed"_attr = (!status.isActiveFault(status.getSeverity())));
}

void FaultManager::logCurrentState(FaultState, FaultState newState, const OptionalMessageType&) {
    LOGV2(5936503, "Fault manager changed state ", "state"_attr = (str::stream() << newState));
}

void FaultManager::setTransientFaultDeadline(FaultState, FaultState, const OptionalMessageType&) {
    _transientFaultDeadline = std::make_unique<TransientFaultDeadline>(
        this, _taskExecutor, _config->getActiveFaultDuration());
}

void FaultManager::clearTransientFaultDeadline(FaultState, FaultState, const OptionalMessageType&) {
    _transientFaultDeadline = nullptr;
}

void FaultManager::setInitialHealthCheckComplete(FaultState,
                                                 FaultState newState,
                                                 const OptionalMessageType&) {
    LOGV2_DEBUG(5936502,
                0,
                "The fault manager initial health checks have completed",
                "state"_attr = (str::stream() << newState));
    _initialHealthCheckCompletedPromise.emplaceValue();
}

void FaultManager::schedulePeriodicHealthCheckThread() {
    if (!feature_flags::gFeatureFlagHealthMonitoring.isEnabled(
            serverGlobalParams.featureCompatibility) ||
        _config->periodicChecksDisabledForTests()) {
        return;
    }

    auto observers = getHealthObservers();
    for (auto observer : observers) {
        LOGV2_DEBUG(
            59365, 1, "starting health observer", "observerType"_attr = observer->getType());

        // TODO (SERVER-59368): The system should properly handle a health checker being turned
        // on/off
        auto token = _managerShuttingDownCancellationSource.token();
        if (_config->isHealthObserverEnabled(observer->getType())) {
            healthCheck(observer, token);
        }
    }
}

FaultManager::~FaultManager() {
    _managerShuttingDownCancellationSource.cancel();
    _taskExecutor->shutdown();

    LOGV2(5936601, "Shutting down periodic health checks");
    for (auto& pair : _healthCheckContexts) {
        auto cbHandle = pair.second.resultStatus;
        if (cbHandle) {
            _taskExecutor->cancel(cbHandle.get());
        }
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
        _initialHealthCheckCompletedPromise.setError(
            {ErrorCodes::CommandFailed, "Fault manager failed initial health check"});
    }
    LOGV2_DEBUG(6136801, 1, "Done shutting down periodic health checks");
}

SharedSemiFuture<void> FaultManager::startPeriodicHealthChecks() {
    if (!feature_flags::gFeatureFlagHealthMonitoring.isEnabled(
            serverGlobalParams.featureCompatibility)) {
        LOGV2_DEBUG(6187201, 1, "Health checks disabled by feature flag");
        return Future<void>::makeReady();
    }

    _taskExecutor->startup();
    invariant(state() == FaultState::kStartupCheck);

    _init();

    if (getActiveHealthObservers().size() == 0) {
        LOGV2_DEBUG(5936511, 2, "No active health observers are configured.");
        setState(FaultState::kOk, HealthCheckStatus(FaultFacetType::kSystem));
    } else {
        schedulePeriodicHealthCheckThread();
    }

    return _initialHealthCheckCompletedPromise.getFuture();
}

FaultState FaultManager::getFaultState() const {
    return state();
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

void FaultManager::healthCheck(HealthObserver* observer, CancellationToken token) {
    auto schedulerCb = [this, observer, token] {
        auto periodicThreadCbHandleStatus = this->_taskExecutor->scheduleWorkAt(
            _taskExecutor->now() + this->_config->kPeriodicHealthCheckInterval,
            [this, observer, token](const mongo::executor::TaskExecutor::CallbackArgs& cbData) {
                if (!cbData.status.isOK()) {
                    return;
                }
                healthCheck(observer, token);
            });

        if (!periodicThreadCbHandleStatus.isOK()) {
            if (ErrorCodes::isA<ErrorCategory::ShutdownError>(
                    periodicThreadCbHandleStatus.getStatus().code())) {
                return;
            }

            uassert(5936101,
                    fmt::format("Failed to initialize periodic health check work. Reason: {}",
                                periodicThreadCbHandleStatus.getStatus().codeString()),
                    periodicThreadCbHandleStatus.isOK());
        }

        _healthCheckContexts.at(observer->getType()).resultStatus =
            std::move(periodicThreadCbHandleStatus.getValue());
    };

    auto acceptNotOKStatus = [this, observer](Status s) {
        auto healthCheckStatus = HealthCheckStatus(observer->getType(), 1.0, s.reason());
        LOGV2_ERROR(
            6007901, "Unexpected failure during health check", "status"_attr = healthCheckStatus);
        this->accept(healthCheckStatus);
        return healthCheckStatus;
    };

    // If health observer is disabled, then do nothing and schedule another run (health observer may
    // become enabled).
    // TODO (SERVER-59368): The system should properly handle a health checker being turned on/off
    if (!_config->isHealthObserverEnabled(observer->getType())) {
        schedulerCb();
        return;
    }

    _healthCheckContexts.insert({observer->getType(), HealthCheckContext(nullptr, boost::none)});
    // Run asynchronous health check.  When complete, check for state transition (and perform if
    // necessary). Then schedule the next run.
    auto healthCheckFuture = observer->periodicCheck(*this, _taskExecutor, token)
                                 .thenRunOn(_taskExecutor)
                                 .onCompletion([this, acceptNotOKStatus, schedulerCb](
                                                   StatusWith<HealthCheckStatus> status) {
                                     ON_BLOCK_EXIT([this, schedulerCb]() {
                                         if (!_config->periodicChecksDisabledForTests()) {
                                             schedulerCb();
                                         }
                                     });

                                     if (!status.isOK()) {
                                         return acceptNotOKStatus(status.getStatus());
                                     }

                                     this->accept(status.getValue());
                                     return status.getValue();
                                 });
    auto futurePtr =
        std::make_unique<ExecutorFuture<HealthCheckStatus>>(std::move(healthCheckFuture));
    _healthCheckContexts.at(observer->getType()).result = std::move(futurePtr);
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

void FaultManager::_init() {
    auto lk = stdx::lock_guard(_mutex);

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
          "managerState"_attr = state(),
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

std::vector<HealthObserver*> FaultManager::getActiveHealthObservers() {
    auto allObservers = getHealthObservers();
    std::vector<HealthObserver*> result;
    result.reserve(allObservers.size());
    for (auto observer : allObservers) {
        if (_config->isHealthObserverEnabled(observer->getType())) {
            result.push_back(observer);
        }
    }
    return result;
}

void FaultManager::progressMonitorCheckForTests(std::function<void(std::string cause)> crashCb) {
    _progressMonitor->progressMonitorCheck(crashCb);
}

}  // namespace process_health
}  // namespace mongo
