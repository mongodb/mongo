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

#include "mongo/db/process_health/fault.h"
#include "mongo/db/process_health/fault_facet.h"
#include "mongo/db/process_health/fault_facet_container.h"
#include "mongo/db/process_health/fault_manager_config.h"
#include "mongo/db/process_health/health_monitoring_server_parameters_gen.h"
#include "mongo/db/process_health/health_observer.h"
#include "mongo/db/process_health/progress_monitor.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/task_executor.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"

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
class FaultManager : protected FaultFacetsContainerFactory {
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

    // Start periodic health checks, invoke it once during server startup.
    // It is unsafe to start health checks immediately during ServiceContext creation
    // because some ServiceContext fields might not be initialized yet.
    // Health checks cannot be stopped but could be effectively disabled with health-checker
    // specific flags.
    void startPeriodicHealthChecks();

    static FaultManager* get(ServiceContext* svcCtx);

    // Replace the FaultManager for the 'svcCtx'. This functionality
    // is exposed for testing and initial bootstrap.
    static void set(ServiceContext* svcCtx, std::unique_ptr<FaultManager> newFaultManager);

    // Returns the current fault state for the server.
    FaultState getFaultState() const;

    // Returns the current fault, if any. Otherwise returns an empty pointer.
    FaultConstPtr currentFault() const;

    // All observers remain valid for the manager lifetime, thus returning
    // just pointers is safe, as long as they are used while manager exists.
    std::vector<HealthObserver*> getHealthObservers();

    // Gets the aggregate configuration for all process health environment.
    FaultManagerConfig getConfig() const;

    // Gets the timestamp of the last transition
    Date_t getLastTransitionTime() const;

protected:
    // Starts the health check sequence and updates the internal state on completion.
    // This is invoked by the internal timer.
    virtual void healthCheck();

    // Protected interface FaultFacetsContainerFactory implementation.

    // The interface FaultFacetsContainerFactory is implemented by the member '_fault'.
    FaultFacetsContainerPtr getFaultFacetsContainer() const override;

    FaultFacetsContainerPtr getOrCreateFaultFacetsContainer() override;

    void updateWithCheckStatus(HealthCheckStatus&& checkStatus) override;

    // State machine related.

    void checkForStateTransition();  // Invoked by periodic thread.

    // Methods that represent particular events that trigger state transition.
    void processFaultExistsEvent();
    void processFaultIsResolvedEvent();

    // Makes a valid state transition or returns an error.
    // State transition should be triggered by events above.
    void transitionToState(FaultState newState);

    void schedulePeriodicHealthCheckThread(bool immediately = false);

    // TODO: move this into fault class; refactor to remove FaultInternal
    bool hasCriticalFacet(const FaultInternal* fault) const;

    void progressMonitorCheckForTests(std::function<void(std::string cause)> crashCb);

private:
    // One time init.
    void _firstTimeInitIfNeeded();

    std::unique_ptr<FaultManagerConfig> _config;
    ServiceContext* const _svcCtx;
    std::shared_ptr<executor::TaskExecutor> _taskExecutor;
    // Callback used to crash the server.
    std::function<void(std::string cause)> _crashCb;

    mutable Mutex _mutex =
        MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(5), "FaultManager::_mutex");

    std::shared_ptr<FaultInternal> _fault;
    // We lazily init all health observers.
    AtomicWord<bool> _firstTimeInitExecuted{false};
    // This source is canceled before the _taskExecutor shutdown(). It
    // can be used to check for the start of the shutdown sequence.
    CancellationSource _managerShuttingDownCancellationSource;
    // Manager owns all observer instances.
    std::vector<std::unique_ptr<HealthObserver>> _observers;
    boost::optional<executor::TaskExecutor::CallbackHandle> _periodicHealthCheckCbHandle;
    SharedPromise<void> _initialHealthCheckCompletedPromise;

    // Protects the state below.
    mutable Mutex _stateMutex =
        MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(0), "FaultManager::_stateMutex");
    FaultState _currentState = FaultState::kStartupCheck;

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
};

}  // namespace process_health
}  // namespace mongo
