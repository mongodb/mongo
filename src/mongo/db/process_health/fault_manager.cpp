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


#include "mongo/platform/basic.h"

#include "mongo/db/process_health/fault_manager.h"

#include <algorithm>

#include "mongo/db/process_health/fault.h"
#include "mongo/db/process_health/fault_facet_impl.h"
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kProcessHealth


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


bool FaultManager::isInitialized() {
    stdx::lock_guard lock(_mutex);
    return _initialized;
}


// Start health checks if observer turned on via setParamater. Cleanup if the observer is turned
// off.
void FaultManager::healthMonitoringIntensitiesUpdated(HealthObserverIntensities oldValue,
                                                      HealthObserverIntensities newValue) {
    if (!hasGlobalServiceContext())
        return;

    auto manager = FaultManager::get(getGlobalServiceContext());
    if (manager && manager->isInitialized()) {
        auto cancellationToken = manager->_managerShuttingDownCancellationSource.token();
        auto findByType =
            [](const auto& values,
               HealthObserverTypeEnum type) -> boost::optional<HealthObserverIntensitySetting> {
            if (!values) {
                return boost::none;
            }
            auto it = std::find_if(values->begin(),
                                   values->end(),
                                   [type](const HealthObserverIntensitySetting& setting) {
                                       return setting.getType() == type;
                                   });
            if (it != values->end()) {
                return *it;
            }
            return boost::none;
        };

        auto optionalNewValues = newValue.getValues();
        if (!optionalNewValues) {
            return;  // Nothing was updated.
        }
        for (auto& setting : *optionalNewValues) {
            auto oldSetting = findByType(oldValue.getValues(), setting.getType());
            if (!oldSetting) {
                continue;
            }
            if (cancellationToken.isCanceled()) {
                break;
            }
            auto oldIntensity = oldSetting->getIntensity();
            auto newIntensity = setting.getIntensity();
            if (oldIntensity != newIntensity) {
                if (oldIntensity == HealthObserverIntensityEnum::kOff) {
                    // off -> {critical, non-critical}
                    if (auto* observer =
                            manager->getHealthObserver(toFaultFacetType(setting.getType()));
                        observer != nullptr) {
                        manager->scheduleNextHealthCheck(
                            observer, cancellationToken, true /* immediate */);
                    }
                } else if (newIntensity == HealthObserverIntensityEnum::kOff) {
                    // {critical, non-critical} -> off
                    // Resolve any faults for this observer with a synthetic health check result.
                    auto successfulHealthCheckResult = HealthCheckStatus(setting.getType());
                    manager->accept(successfulHealthCheckResult);
                }
            }
        }
    }
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
          ::_exit(static_cast<int>(ExitCode::processHealthCheck));
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


    {
        auto lk = stdx::lock_guard(_stateMutex);
        logMessageReceived(state(), status);

        if (status.isActiveFault()) {
            _healthyObservations.erase(status.getType());
        } else {
            _healthyObservations.insert(status.getType());
        }
    }

    updateWithCheckStatus(HealthCheckStatus(status));
    auto optionalFault = getFault();
    if (optionalFault) {
        optionalFault->garbageCollectResolvedFacets();
    }

    if (optionalFault) {
        setTransientFaultDeadline(
            FaultState::kStartupCheck, FaultState::kStartupCheck, boost::none);
    }

    // If the whole fault becomes resolved, garbage collect it
    // with proper locking.
    std::shared_ptr<Fault> faultToDelete;

    {
        auto lk = stdx::lock_guard(_mutex);
        if (_fault && _fault->getFacets().empty()) {
            faultToDelete.swap(_fault);
        }
    }

    auto lk = stdx::lock_guard(_stateMutex);
    if (activeObserversTypes == _healthyObservations) {
        return FaultState::kOk;
    }
    return boost::none;
}

boost::optional<FaultState> FaultManager::handleOk(const OptionalMessageType& message) {
    invariant(message);

    HealthCheckStatus status = message.get();
    {
        auto lk = stdx::lock_guard(_stateMutex);
        logMessageReceived(state(), status);
    }

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

    {
        auto lk = stdx::lock_guard(_stateMutex);
        logMessageReceived(state(), status);
    }

    updateWithCheckStatus(HealthCheckStatus(status));

    auto optionalActiveFault = getFault();
    if (optionalActiveFault) {
        optionalActiveFault->garbageCollectResolvedFacets();
    }

    // If the whole fault becomes resolved, garbage collect it
    // with proper locking.
    auto lk = stdx::lock_guard(_mutex);
    if (_fault && _fault->getFacets().empty()) {
        _fault.reset();
        return FaultState::kOk;
    }
    return boost::none;
}

boost::optional<FaultState> FaultManager::handleActiveFault(const OptionalMessageType& message) {
    invariant(_fault);
    LOGV2_FATAL(5936509, "Halting Process due to ongoing fault", "fault"_attr = *_fault);
    return boost::none;
}

void FaultManager::logMessageReceived(FaultState state, const HealthCheckStatus& status) {
    LOGV2_DEBUG(5936504,
                1,
                "Fault manager received health check result",
                "state"_attr = (str::stream() << state),
                "observer_type"_attr = (str::stream() << status.getType()),
                "result"_attr = status,
                "passed"_attr = (!status.isActiveFault(status.getSeverity())));
}

void FaultManager::logCurrentState(FaultState, FaultState newState, const OptionalMessageType&) {
    {
        stdx::lock_guard<Latch> lk(_stateMutex);
        _lastTransitionTime = _svcCtx->getFastClockSource()->now();
    }
    if (_fault) {
        LOGV2(5939703,
              "Fault manager changed state ",
              "state"_attr = (str::stream() << newState),
              "fault"_attr = *_fault);
    } else {
        LOGV2(5936503, "Fault manager changed state ", "state"_attr = (str::stream() << newState));
    }
}

void FaultManager::setTransientFaultDeadline(FaultState, FaultState, const OptionalMessageType&) {
    if (_fault->hasCriticalFacet(getConfig()) && !_transientFaultDeadline) {
        _transientFaultDeadline = std::make_unique<TransientFaultDeadline>(
            this, _taskExecutor, _config->getActiveFaultDuration());
    }
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

    auto observers = getActiveHealthObservers();
    if (observers.size() == 0) {
        LOGV2(5936511, "No active health observers are configured.");
        setState(FaultState::kOk, HealthCheckStatus(FaultFacetType::kSystem));
        return;
    }

    str::stream listOfActiveObservers;
    for (auto observer : observers) {
        LOGV2_DEBUG(5936501,
                    1,
                    "starting health observer",
                    "observerType"_attr = str::stream() << observer->getType());
        listOfActiveObservers << observer->getType() << " ";

        auto token = _managerShuttingDownCancellationSource.token();
        if (!observer->isConfigured()) {
            // Transition to an active fault if a health observer is not configured properly.
            updateWithCheckStatus(HealthCheckStatus(
                observer->getType(),
                Severity::kFailure,
                "Health observer failed to start because it was not configured properly."_sd));
            setState(FaultState::kActiveFault, HealthCheckStatus(observer->getType()));
            return;
        }
        scheduleNextHealthCheck(observer, token, true /* immediate */);
    }
    LOGV2(5936804, "Health observers started", "detail"_attr = listOfActiveObservers);
}

FaultManager::~FaultManager() {
    _managerShuttingDownCancellationSource.cancel();
    _taskExecutor->shutdown();

    LOGV2(5936601, "Shutting down periodic health checks");
    {
        stdx::lock_guard lock(_mutex);
        for (auto& pair : _healthCheckContexts) {
            auto cbHandle = pair.second.callbackHandle;
            if (cbHandle) {
                _taskExecutor->cancel(cbHandle.value());
            }
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
    schedulePeriodicHealthCheckThread();

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
    return _fault;
}

FaultPtr FaultManager::getFault() const {
    auto lk = stdx::lock_guard(_mutex);
    return _fault;
}

FaultPtr FaultManager::createFault() {
    auto lk = stdx::lock_guard(_mutex);
    _fault = std::make_shared<Fault>(_svcCtx->getFastClockSource());
    return _fault;
}

FaultPtr FaultManager::getOrCreateFault() {
    auto lk = stdx::lock_guard(_mutex);
    if (!_fault) {
        // Create a new one.
        _fault = std::make_shared<Fault>(_svcCtx->getFastClockSource());
    }
    return _fault;
}

void FaultManager::scheduleNextHealthCheck(HealthObserver* observer,
                                           CancellationToken token,
                                           bool immediately) {
    stdx::lock_guard lock(_mutex);

    // Check that context callbackHandle is not set and if future exists, it is ready.
    auto existingIt = _healthCheckContexts.find(observer->getType());
    if (existingIt != _healthCheckContexts.end()) {
        if (existingIt->second.callbackHandle) {
            LOGV2_WARNING(6418201,
                          "Cannot schedule health check while another one is in queue",
                          "observerType"_attr = str::stream() << observer->getType());
            return;
        }
        if (existingIt->second.result && !existingIt->second.result->isReady()) {
            LOGV2_WARNING(6418202,
                          "Cannot schedule health check while another one is currently executing",
                          "observerType"_attr = str::stream() << observer->getType());
            return;
        }
    }
    _healthCheckContexts.insert_or_assign(observer->getType(),
                                          HealthCheckContext(nullptr, boost::none));

    auto scheduledTime = immediately
        ? _taskExecutor->now()
        : _taskExecutor->now() + _config->getPeriodicHealthCheckInterval(observer->getType()) +
            std::min(observer->healthCheckJitter(),
                     FaultManagerConfig::kPeriodicHealthCheckMaxJitter);
    LOGV2_DEBUG(5939701,
                3,
                "Schedule next health check",
                "observerType"_attr = str::stream() << observer->getType(),
                "scheduledTime"_attr = scheduledTime);

    auto periodicThreadCbHandleStatus = _taskExecutor->scheduleWorkAt(
        scheduledTime,
        [this, observer, token](const mongo::executor::TaskExecutor::CallbackArgs& cbData) {
            if (!cbData.status.isOK()) {
                LOGV2_DEBUG(
                    5939702, 1, "Fault manager received an error", "status"_attr = cbData.status);
                if (ErrorCodes::isA<ErrorCategory::CancellationError>(cbData.status.code())) {
                    return;
                }
                // continue health checking otherwise
            }
            healthCheck(observer, token);
        });

    if (!periodicThreadCbHandleStatus.isOK()) {
        if (ErrorCodes::isA<ErrorCategory::ShutdownError>(
                periodicThreadCbHandleStatus.getStatus().code())) {
            LOGV2_DEBUG(6418203, 1, "Not scheduling health check because of shutdown");
            return;
        }

        uassert(5936101,
                str::stream() << "Failed to schedule periodic health check for "
                              << observer->getType() << ": "
                              << periodicThreadCbHandleStatus.getStatus().codeString(),
                periodicThreadCbHandleStatus.isOK());
    }

    _healthCheckContexts.at(observer->getType()).callbackHandle =
        std::move(periodicThreadCbHandleStatus.getValue());
}

void FaultManager::healthCheck(HealthObserver* observer, CancellationToken token) {
    auto acceptNotOKStatus = [this, observer](Status s) {
        auto healthCheckStatus =
            HealthCheckStatus(observer->getType(), Severity::kFailure, s.reason());
        LOGV2_ERROR(
            6007901, "Unexpected failure during health check", "status"_attr = healthCheckStatus);
        accept(healthCheckStatus);
        return healthCheckStatus;
    };


    // Run asynchronous health check.  Send output to the state machine. Schedule next run.
    auto healthCheckFuture = observer->periodicCheck(_taskExecutor, token);

    stdx::lock_guard lock(_mutex);
    auto contextIt = _healthCheckContexts.find(observer->getType());
    if (contextIt == _healthCheckContexts.end()) {
        LOGV2_ERROR(6418204, "Unexpected failure during health check: context not found");
        return;
    }
    contextIt->second.result =
        std::make_unique<SharedSemiFuture<HealthCheckStatus>>(std::move(healthCheckFuture));

    contextIt->second.result->thenRunOn(_taskExecutor)
        .onCompletion(
            [this, acceptNotOKStatus, observer, token](StatusWith<HealthCheckStatus> status) {
                ON_BLOCK_EXIT([this, observer, token]() {
                    {
                        stdx::lock_guard lock(_mutex);
                        // Rescheduling requires the previous handle to be cleaned.
                        auto contextIt = _healthCheckContexts.find(observer->getType());
                        if (contextIt != _healthCheckContexts.end()) {
                            contextIt->second.callbackHandle = {};
                        }
                    }
                    if (!_config->periodicChecksDisabledForTests() &&
                        _config->isHealthObserverEnabled(observer->getType())) {
                        scheduleNextHealthCheck(observer, token, false /* immediate */);
                    }
                });

                if (!status.isOK()) {
                    return acceptNotOKStatus(status.getStatus());
                }

                accept(status.getValue());
                return status.getValue();
            })
        .getAsync([](StatusOrStatusWith<mongo::process_health::HealthCheckStatus>) {});
}

void FaultManager::updateWithCheckStatus(HealthCheckStatus&& checkStatus) {
    auto fault = getFault();
    // Remove resolved facet from the fault.
    if (HealthCheckStatus::isResolved(checkStatus.getSeverity())) {
        if (fault) {
            fault->removeFacet(checkStatus.getType());
        }
        return;
    }

    if (!_fault) {
        fault = createFault();  // Create fault if it doesn't exist.
    }

    const auto type = checkStatus.getType();
    fault->upsertFacet(std::make_shared<FaultFacetImpl>(
        type, _svcCtx->getFastClockSource(), std::move(checkStatus)));
}

const FaultManagerConfig& FaultManager::getConfig() const {
    auto lk = stdx::lock_guard(_mutex);
    return *_config;
}

void FaultManager::_init() {
    std::set<FaultFacetType> allTypes;
    std::vector<std::unique_ptr<HealthObserver>>::size_type observersSize = _observers.size();
    {
        auto lk = stdx::lock_guard(_mutex);

        _observers = HealthObserverRegistration::instantiateAllObservers(_svcCtx);

        for (const auto& observer : _observers) {
            allTypes.insert(observer->getType());
        }
        observersSize = _observers.size();
    }

    // Verify that all observer types are unique.
    invariant(allTypes.size() == observersSize);

    // Start the monitor thread after all observers are initialized.
    _progressMonitor = std::make_unique<ProgressMonitor>(this, _svcCtx, _crashCb);

    _initialized = true;

    LOGV2_DEBUG(5956701,
                1,
                "Instantiated health observers",
                "managerState"_attr = str::stream() << state(),
                "observersCount"_attr = observersSize);
}

std::vector<HealthObserver*> FaultManager::getHealthObservers() const {
    std::vector<HealthObserver*> result;
    stdx::lock_guard<Latch> lk(_mutex);
    result.reserve(_observers.size());
    std::transform(_observers.cbegin(),
                   _observers.cend(),
                   std::back_inserter(result),
                   [](const std::unique_ptr<HealthObserver>& value) { return value.get(); });
    return result;
}

std::vector<HealthObserver*> FaultManager::getActiveHealthObservers() const {
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

HealthObserver* FaultManager::getHealthObserver(FaultFacetType type) const {
    stdx::lock_guard<Latch> lk(_mutex);
    auto observerIt = std::find_if(
        _observers.begin(), _observers.end(), [type](auto& o) { return o->getType() == type; });
    if (observerIt != _observers.end()) {
        return (*observerIt).get();
    }
    return nullptr;
}

void FaultManager::appendDescription(BSONObjBuilder* result, bool appendDetails) const {
    static constexpr auto kDurationThreshold = Hours{24};
    const auto now = _svcCtx->getFastClockSource()->now();
    StringBuilder faultStateStr;
    faultStateStr << getFaultState();

    result->append("state", faultStateStr.str());
    result->appendDate("enteredStateAtTime", getLastTransitionTime());

    auto fault = currentFault();
    if (fault) {
        BSONObjBuilder sub_result;
        fault->appendDescription(&sub_result);
        result->append("faultInformation", sub_result.obj());
    }

    auto allObservers = getHealthObservers();
    for (auto observer : allObservers) {
        if (!appendDetails && !_config->isHealthObserverEnabled(observer->getType())) {
            continue;
        }
        BSONObjBuilder sub_result;
        sub_result.append("intensity",
                          HealthObserverIntensity_serializer(
                              _config->getHealthObserverIntensity(observer->getType())));

        HealthObserverLivenessStats stats = observer->getStats();
        sub_result.append("totalChecks", stats.completedChecksCount);
        if (appendDetails) {
            sub_result.append("totalChecksWithFailure", stats.completedChecksWithFaultCount);
            if (now - stats.lastTimeCheckStarted < kDurationThreshold) {
                sub_result.append("timeSinceLastCheckStartedMs",
                                  durationCount<Milliseconds>(now - stats.lastTimeCheckStarted));
                sub_result.append("timeSinceLastCheckCompletedMs",
                                  durationCount<Milliseconds>(now - stats.lastTimeCheckCompleted));
            }
        }
        // Report how long the current check is running, if it's longer than 10% of deadline.
        if (stats.currentlyRunningHealthCheck &&
            now - stats.lastTimeCheckStarted > getConfig().getPeriodicLivenessDeadline() / 10) {
            sub_result.append("runningCheckForMs",
                              durationCount<Milliseconds>(now - stats.lastTimeCheckStarted));
        }
        result->append(FaultFacetType_serializer(observer->getType()), sub_result.obj());
    }
}

void FaultManager::progressMonitorCheckForTests(std::function<void(std::string cause)> crashCb) {
    _progressMonitor->progressMonitorCheck(crashCb);
}

}  // namespace process_health
}  // namespace mongo
