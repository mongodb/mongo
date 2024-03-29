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


#include "mongo/bson/timestamp.h"
#include "mongo/db/client.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/replica_set_aware_service.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/db/vector_clock.h"
#include "mongo/db/vector_clock_document_gen.h"
#include "mongo/db/vector_clock_mutable.h"
#include "mongo/platform/mutex.h"
#include "mongo/util/decorable.h"


namespace mongo {
namespace {

class VectorClockMongoD : public VectorClockMutable,
                          public ReplicaSetAwareService<VectorClockMongoD>,
                          public std::enable_shared_from_this<VectorClockMongoD> {
    VectorClockMongoD(const VectorClockMongoD&) = delete;
    VectorClockMongoD& operator=(const VectorClockMongoD&) = delete;

public:
    static VectorClockMongoD* get(ServiceContext* serviceContext);

    VectorClockMongoD(ServiceContext* ctx);
    ~VectorClockMongoD() override = default;

private:
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
    void onInitialDataAvailable(OperationContext* opCtx, bool isMajorityDataAvailable) override;
    void onShutdown() override;
    void onStepUpBegin(OperationContext* opCtx, long long term) override;
    void onStepUpComplete(OperationContext* opCtx, long long term) override {}
    void onStepDown() override;
    void onRollback() override {}
    void onBecomeArbiter() override;
    inline std::string getServiceName() const final {
        return "VectorClockMongoD";
    }

    /**
     * Structure used as keys for the map of waiters for VectorClock durability.
     */
    struct ComparableVectorTime {
        bool operator<(const ComparableVectorTime& other) const {
            return vt.configTime() < other.vt.configTime() ||
                vt.topologyTime() < other.vt.topologyTime();
        }
        bool operator>(const ComparableVectorTime& other) const {
            return vt.configTime() > other.vt.configTime() ||
                vt.topologyTime() > other.vt.topologyTime();
        }
        bool operator==(const ComparableVectorTime& other) const {
            return vt.configTime() == other.vt.configTime() &&
                vt.topologyTime() == other.vt.topologyTime();
        }

        VectorTime vt;
    };

    /**
     * The way the VectorClock durability works is by maintaining an `_queue` of callers, which wait
     * for a particular VectorTime to become durable.
     *
     * When the queue is empty, there is no persistence activity going on. The first caller, who
     * finds `_loopScheduled` to be false, will set it to true, indicating it will schedule the
     * asynchronous persistence task. The asynchronous persistence task is effectively the following
     * loop:
     *
     *  while (!_queue.empty()) {
     *      timeToPersist = getTime();
     *      persistTime(timeToPersist);
     *      _durableTime = timeToPersist;
     *      // Notify entries in _queue, whose time is <= _durableTime and remove them
     *  }
     */
    SharedSemiFuture<void> _enqueueWaiterAndScheduleLoopIfNeeded(stdx::unique_lock<Mutex> ul,
                                                                 VectorTime time);
    Future<void> _doWhileQueueNotEmptyOrError();

    // Protects the shared state below
    Mutex _mutex = MONGO_MAKE_LATCH("VectorClockMongoD::_mutex");

    // If set to true, means that another operation already scheduled the `_queue` draining loop, if
    // false it means that this operation must do it
    bool _loopScheduled{false};

    // This value is only boost::none once, just after the object is constructuted. From the moment,
    // the first operation schedules the `_queue`-draining loop, it will be set to a future, which
    // will be signaled when the previously-scheduled `_queue` draining loop completes.
    boost::optional<Future<void>> _currentWhileLoop;

    // If boost::none, means the durable time needs to be recovered from disk, otherwise contains
    // the latest-known durable time
    boost::optional<VectorTime> _durableTime;

    // Queue ordered in increasing order of the VectorTimes, which are waiting to be persisted
    using Queue = std::map<ComparableVectorTime, std::unique_ptr<SharedPromise<void>>>;
    Queue _queue;

    ServiceContext* _serviceContext;

    Atomic<bool> _shutdownInitiated{false};
};

}  // namespace
}  // namespace mongo
