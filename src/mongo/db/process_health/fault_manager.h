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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/process_health/fault.h"
#include "mongo/db/process_health/fault_facet.h"
#include "mongo/db/process_health/fault_manager_config.h"
#include "mongo/db/process_health/health_check_status.h"
#include "mongo/db/process_health/health_monitoring_server_parameters_gen.h"
#include "mongo/db/process_health/health_observer.h"
#include "mongo/db/process_health/progress_monitor.h"
#include "mongo/db/process_health/state_machine.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/task_executor.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/hierarchical_acquisition.h"
#include "mongo/util/time_support.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace process_health {

/**
 * FaultManager is a singleton constantly monitoring the health of the
 * system.
 *
 * It supports pluggable 'HealthObservers', which are responsible to check various
 * aspects of the server health. The aggregate outcome of all periodic checks
 * is accesible as FaultState.
 *
 * If an active fault state persists, FaultManager will terminate the server process.
 */
class FaultManager : protected StateMachine<HealthCheckStatus, FaultState> {
    FaultManager(const FaultManager&) = delete;
    FaultManager& operator=(const FaultManager&) = delete;

public:
    // The taskExecutor provided should not be already started.
    FaultManager(ServiceContext* svcCtx,
                 std::shared_ptr<executor::TaskExecutor> taskExecutor,
                 std::unique_ptr<FaultManagerConfig> config);
    // Variant with explicit crash callback for tests.
    FaultManager(ServiceContext* svcCtx,
                 std::shared_ptr<executor::TaskExecutor> taskExecutor,
                 std::unique_ptr<FaultManagerConfig> config,
                 std::function<void(std::string cause)> crashCb);
    virtual ~FaultManager();

    void setupStateMachine();

    boost::optional<FaultState> handleStartupCheck(const OptionalMessageType& message);
    boost::optional<FaultState> handleOk(const OptionalMessageType& message);
    boost::optional<FaultState> handleTransientFault(const OptionalMessageType& message);
    boost::optional<FaultState> handleActiveFault(const OptionalMessageType& message);

    void setInitialHealthCheckComplete(FaultState, FaultState, const OptionalMessageType&);
    void logCurrentState(FaultState, FaultState, const OptionalMessageType&);
    void logMessageReceived(FaultState state, const HealthCheckStatus& status);
    void setTransientFaultDeadline(FaultState, FaultState, const OptionalMessageType&);
    void clearTransientFaultDeadline(FaultState, FaultState, const OptionalMessageType&);

    // Start periodic health checks, invoke it once during server startup.
    // It is unsafe to start health checks immediately during ServiceContext creation
    // because some ServiceContext fields might not be initialized yet.
    // Health checks cannot be stopped but could be effectively disabled with health-checker
    // specific flags.
    SharedSemiFuture<void> startPeriodicHealthChecks();

    bool isInitialized();


    static FaultManager* get(ServiceContext* svcCtx);

    // Replace the FaultManager for the 'svcCtx'. This functionality
    // is exposed for testing and initial bootstrap.
    static void set(ServiceContext* svcCtx, std::unique_ptr<FaultManager> newFaultManager);

    // Signals that the intensity for a health observer has been updated.
    static void healthMonitoringIntensitiesUpdated(HealthObserverIntensities oldValue,
                                                   HealthObserverIntensities newValue);

    // Returns the current fault state for the server.
    FaultState getFaultState() const;

    // Returns the current fault, if any. Otherwise returns an empty pointer.
    FaultConstPtr currentFault() const;

    // All observers remain valid for the manager lifetime, thus returning
    // just pointers is safe, as long as they are used while manager exists.
    std::vector<HealthObserver*> getHealthObservers() const;

    // Gets the aggregate configuration for all process health environment.
    const FaultManagerConfig& getConfig() const;

    // Gets the timestamp of the last transition
    Date_t getLastTransitionTime() const;

    /**
     * Generate the `serverStatus` section for the fault manager.
     * @param appendDetails is true when the section is generated with:
     *     health: {details: true}
     * thus it is ok to add any verbose information here.
     */
    void appendDescription(BSONObjBuilder* builder, bool appendDetails) const;

protected:
    // Returns all health observers not configured as Off
    std::vector<HealthObserver*> getActiveHealthObservers() const;
    HealthObserver* getHealthObserver(FaultFacetType type) const;

    // Runs a particular health observer.  Then attempts to transition states. Then schedules next
    // run.
    virtual void healthCheck(HealthObserver* observer, CancellationToken token);

    FaultPtr getFault() const;

    FaultPtr createFault();

    FaultPtr getOrCreateFault();

    /**
     * Update the active fault with supplied check result.
     * Create or delete existing facet depending on the status.
     */
    void updateWithCheckStatus(HealthCheckStatus&& checkStatus);

    void schedulePeriodicHealthCheckThread();

    void progressMonitorCheckForTests(std::function<void(std::string cause)> crashCb);

    void scheduleNextHealthCheck(HealthObserver* observer,
                                 CancellationToken token,
                                 bool immediately);

private:
    // One time init.
    void _init();

    std::unique_ptr<FaultManagerConfig> _config;
    ServiceContext* const _svcCtx;
    std::shared_ptr<executor::TaskExecutor> _taskExecutor;
    // Callback used to crash the server.
    std::function<void(std::string cause)> _crashCb;

    mutable stdx::mutex _mutex;

    std::shared_ptr<Fault> _fault;
    // This source is canceled before the _taskExecutor shutdown(). It
    // can be used to check for the start of the shutdown sequence.
    CancellationSource _managerShuttingDownCancellationSource;
    // Manager owns all observer instances.
    std::vector<std::unique_ptr<HealthObserver>> _observers;
    SharedPromise<void> _initialHealthCheckCompletedPromise;

    // Protects the state below.
    mutable stdx::mutex _stateMutex;

    bool _initialized = false;
    Date_t _lastTransitionTime;

    // Responsible for transitioning the state of FaultManager to ActiveFault after a
    // timeout while in the TransientFault state. The timer is canceled when the instance is
    // destroyed.
    struct TransientFaultDeadline {
        TransientFaultDeadline() = delete;
        TransientFaultDeadline(FaultManager* faultManager,
                               std::shared_ptr<executor::TaskExecutor> executor,
                               Milliseconds timeout);
        virtual ~TransientFaultDeadline();

    protected:
        // Cancel timer for transitioning to ActiveFault
        CancellationSource cancelActiveFaultTransition;

        // Fufilled when we should transition to ActiveFault
        ExecutorFuture<void> activeFaultTransition;
    };
    std::unique_ptr<TransientFaultDeadline> _transientFaultDeadline;

    std::unique_ptr<ProgressMonitor> _progressMonitor;
    stdx::unordered_set<FaultFacetType> _healthyObservations;

    // The stages of health check context modifications:
    // 1. Schedule and set callbackHandle
    // 2. When scheduled check starts, reset callbackHandle and set result future
    // 3. When result is ready, repeat
    struct HealthCheckContext {
        std::unique_ptr<SharedSemiFuture<HealthCheckStatus>> result;
        boost::optional<executor::TaskExecutor::CallbackHandle> callbackHandle;
        HealthCheckContext(std::unique_ptr<SharedSemiFuture<HealthCheckStatus>> future,
                           boost::optional<executor::TaskExecutor::CallbackHandle> cbHandle)
            : result(std::move(future)), callbackHandle(cbHandle) {};
    };

    stdx::unordered_map<FaultFacetType, HealthCheckContext> _healthCheckContexts;
};

}  // namespace process_health
}  // namespace mongo
