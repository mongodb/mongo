/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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


#include "mongo/bson/timestamp.h"
#include "mongo/db/client.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/replica_set_aware_service.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/db/vector_clock/vector_clock.h"
#include "mongo/db/vector_clock/vector_clock_document_gen.h"
#include "mongo/db/vector_clock/vector_clock_mutable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/decorable.h"


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
    stdx::mutex _mutex;

    // If boost::none, means the durable time needs to be recovered from disk, otherwise contains
    // the latest-known durable time
    boost::optional<VectorTime> _durableTime;

    Queue _queue;

    Atomic<bool> _shutdownInitiated{false};

    /**
     * This is a shared state between threads so any change on this boolean must be guarded by the
     * _mutex. The value of true means there is an async persister task running.
     *
     * After onShutdown the false -> true transition is prohibited (no new persister task after
     * shutdown initiated). The destructor will wait for this flag to be true in order to not to
     * destroy the class while a persister task is still running.
     */
    WaitableAtomic<bool> _taskIsRunning{false};
};

}  // namespace mongo
