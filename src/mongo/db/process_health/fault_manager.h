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
#include "mongo/db/process_health/health_observer.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/task_executor.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"

namespace mongo {
namespace process_health {

/**
 * Current fault state of the server in a simple actionable form.
 */
enum class FaultState {
    kOk = 0,

    // The manager conducts startup checks, new connections should be refused.
    kStartupCheck,

    // The manager detected a fault, however the fault is either not severe
    // enough or is not observed for sufficiently long period of time.
    kTransientFault,

    // The manager detected a severe fault, which made the server unusable.
    kActiveFault
};

StringBuilder& operator<<(StringBuilder& s, const FaultState& state);

std::ostream& operator<<(std::ostream& os, const FaultState& state);

/**
 * FaultManager is a singleton constantly monitoring the health of the
 * system.
 *
 * It supports pluggable 'facets', which are responsible to check various
 * aspects of the server health. The aggregate outcome of all periodic checks
 * is accesible as FaultState, which should be used to gate access to the server.
 *
 * If an active fault state persists, FaultManager puts the server into quiesce
 * mode and shuts it down.
 */
class FaultManager : protected FaultFacetsContainerFactory {
    FaultManager(const FaultManager&) = delete;
    FaultManager& operator=(const FaultManager&) = delete;

public:
    explicit FaultManager(ServiceContext* svcCtx,
                          std::shared_ptr<executor::TaskExecutor> taskExecutor);
    virtual ~FaultManager();

    static FaultManager* get(ServiceContext* svcCtx);

    static void set(ServiceContext* svcCtx, std::unique_ptr<FaultManager> newFaultManager);

    // Returns the current fault state for the server.
    virtual FaultState getFaultState() const;

    // Returns the current fault, if any. Otherwise returns an empty pointer.
    virtual FaultConstPtr currentFault() const;

protected:
    // Starts the health check sequence and updates the internal state on completion.
    // This is invoked by the internal timer.
    virtual void healthCheck();

    // All observers remain valid for the manager lifetime, thus returning
    // just pointers is safe, as long as they are used while manager exists.
    std::vector<HealthObserver*> getHealthObservers();

    // Protected interface FaultFacetsContainerFactory implementation.

    // The interface FaultFacetsContainerFactory is implemented by the member '_fault'.
    FaultFacetsContainerPtr getFaultFacetsContainer() override;

    FaultFacetsContainerPtr getOrCreateFaultFacetsContainer() override;

    // State machine related.

    void checkForStateTransition();  // Invoked by periodic thread.

    // Methods that represent particular events that trigger state transition.
    Status processFaultExistsEvent();
    Status processFaultIsResolvedEvent();

    // Makes a valid state transition or returns an error.
    // State transition should be triggered by events above.
    Status transitionToState(FaultState newState);

    void schedulePeriodicHealthCheckThread();

private:
    // State machine related.
    Status _transitionToKOk();
    Status _transitionToKTransientFault();
    Status _transitionToKActiveFault();

    // One time init.
    void _initHealthObserversIfNeeded();

    ServiceContext* const _svcCtx;

    mutable Mutex _mutex =
        MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(1), "FaultManager::_mutex");
    std::shared_ptr<FaultInternal> _fault;
    // We lazily init all health observers.
    AtomicWord<bool> _initializedAllHealthObservers{false};
    // Manager owns all observer instances.
    std::vector<std::unique_ptr<HealthObserver>> _observers;
    std::shared_ptr<executor::TaskExecutor> _taskExecutor;
    boost::optional<executor::TaskExecutor::CallbackHandle> _periodicHealthCheckCbHandle;

    // Protects the current state of fault manager.
    mutable Mutex _stateMutex =
        MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(0), "FaultManager::_stateMutex");
    FaultState _currentState = FaultState::kStartupCheck;
};

}  // namespace process_health
}  // namespace mongo
