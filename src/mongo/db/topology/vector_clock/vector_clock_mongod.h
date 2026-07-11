// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/client.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/repl/replica_set_aware_service.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/db/topology/vector_clock/vector_clock.h"
#include "mongo/db/topology/vector_clock/vector_clock_mutable.h"
#include "mongo/util/modules.h"

#include <mutex>

namespace mongo {

class VectorClockMongoD : public VectorClockMutable,
                          public ReplicaSetAwareService<VectorClockMongoD> {
public:
    static VectorClockMongoD* get(ServiceContext* serviceContext);

    VectorClockMongoD() = default;
    ~VectorClockMongoD() override;

private:
    /**
     * Structure used as keys for the map of waiters for VectorClock durability.
     */
    struct ComparableVectorTime {
        bool operator<(const ComparableVectorTime& other) const {
            return vt.configTime() < other.vt.configTime() ||
                vt.topologyTime() < other.vt.topologyTime();
        }
        bool operator>=(const ComparableVectorTime& other) const {
            return vt.configTime() >= other.vt.configTime() ||
                vt.topologyTime() >= other.vt.topologyTime();
        }

        VectorTime vt;
    };

    struct QueueElement {
        ComparableVectorTime _time;
        std::unique_ptr<SharedPromise<void>> _promise;

        QueueElement(ComparableVectorTime time, std::unique_ptr<SharedPromise<void>> promise)
            : _time(std::move(time)), _promise(std::move(promise)) {}

        bool operator<(const QueueElement& other) const {
            return _time < other._time;
        }
    };
    using Queue = std::list<QueueElement>;

    VectorClockMongoD(const VectorClockMongoD&) = delete;
    VectorClockMongoD& operator=(const VectorClockMongoD&) = delete;

    // VectorClockMutable methods implementation
    SharedSemiFuture<void> waitForDurableConfigTime() override;
    SharedSemiFuture<void> waitForDurableTopologyTime() override;
    SharedSemiFuture<void> waitForDurable() override;
    VectorClock::VectorTime recoverDirect(OperationContext* opCtx) override;

    LogicalTime _tick(Component component, uint64_t nTicks) override;
    void _tickTo(Component component, LogicalTime newTime) override;

    // ReplicaSetAwareService methods implementation
    void onStartup(OperationContext* opCtx) override {}
    void onSetCurrentConfig(OperationContext* opCtx) override {}
    void onConsistentDataAvailable(OperationContext* opCtx,
                                   bool isMajority,
                                   bool isRollback) override;
    void onShutdown() override;
    void onStepUpBegin(OperationContext* opCtx, long long term) override;
    void onStepUpComplete(OperationContext* opCtx, long long term) override {}
    void onStepDown() override;
    void onRollbackBegin() override {}
    void onBecomeArbiter() override;
    inline std::string getServiceName() const final {
        return "VectorClockMongoD";
    }

    /**
     * The way the VectorClock durability works is by maintaining an `_queue` of callers, which wait
     * for a particular VectorTime to become durable.
     *
     * When the queue is empty, there is no persistence activity going on. The first caller, who
     * finds `_persisterTask` empty starts a new async task.
     *
     * After the `onShutdown` we do not accept any more durability request.
     */
    SharedSemiFuture<void> _enqueueWaiterAndStartDurableTaskIfNeeded(WithLock lk, VectorTime time);

    ExecutorFuture<void> _createPersisterTask();

    // Protects the shared state below
    std::mutex _durableTimeMutex;

    // If boost::none, means the durable time needs to be recovered from disk, otherwise contains
    // the latest-known durable time
    boost::optional<VectorTime> _durableTime;

    Queue _queue;

    Atomic<bool> _shutdownInitiated{false};

    /**
     * This is a shared state between threads so any change on this boolean must be guarded by the
     * _durableTimeMutex. The value of true means there is an async persister task running.
     *
     * After onShutdown the false -> true transition is prohibited (no new persister task after
     * shutdown initiated). The destructor will wait for this flag to be true in order to not to
     * destroy the class while a persister task is still running.
     */
    WaitableAtomic<bool> _taskIsRunning{false};
};

}  // namespace mongo
