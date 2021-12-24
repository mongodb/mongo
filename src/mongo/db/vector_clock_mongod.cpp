/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/db/logical_time_validator.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/repl/replica_set_aware_service.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/vector_clock_document_gen.h"
#include "mongo/db/vector_clock_mutable.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/s/grid.h"

namespace mongo {
namespace {

class VectorClockMongoD : public VectorClockMutable,
                          public ReplicaSetAwareService<VectorClockMongoD> {
    VectorClockMongoD(const VectorClockMongoD&) = delete;
    VectorClockMongoD& operator=(const VectorClockMongoD&) = delete;

public:
    static VectorClockMongoD* get(ServiceContext* serviceContext);

    VectorClockMongoD();
    virtual ~VectorClockMongoD();

private:
    // VectorClock methods implementation

    ComponentSet _gossipOutInternal() const override;
    ComponentSet _gossipInInternal() const override;

    bool _permitGossipClusterTimeWithExternalClients() const override {
        // If this node is in an unreadable state, skip gossiping because it may require reading a
        // signing key from the keys collection.
        auto replicationCoordinator = repl::ReplicationCoordinator::get(_service);
        return !replicationCoordinator ||
            (replicationCoordinator->getReplicationMode() ==
                 repl::ReplicationCoordinator::modeReplSet &&
             // Check repl status without locks to prevent deadlocks. This is a best effort check
             // as the repl state can change right after this check even when inspected under a
             // lock or mutex.
             replicationCoordinator->isInPrimaryOrSecondaryState_UNSAFE());
    }

    bool _permitRefreshDuringGossipOut() const override {
        return false;
    }

    // VectorClockMutable methods implementation

    SharedSemiFuture<void> waitForDurableConfigTime() override;
    SharedSemiFuture<void> waitForDurableTopologyTime() override;
    SharedSemiFuture<void> waitForDurable() override;
    SharedSemiFuture<void> recover() override;

    LogicalTime _tick(Component component, uint64_t nTicks) override;
    void _tickTo(Component component, LogicalTime newTime) override;

    // ReplicaSetAwareService methods implementation

    void onStartup(OperationContext* opCtx) override {}
    void onShutdown() override {}
    void onStepUpBegin(OperationContext* opCtx, long long term) override;
    void onStepUpComplete(OperationContext* opCtx, long long term) override {}
    void onStepDown() override;
    void onBecomeArbiter() override;

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
    Future<void> _doWhileQueueNotEmptyOrError(ServiceContext* service);

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
};

const auto vectorClockMongoDDecoration = ServiceContext::declareDecoration<VectorClockMongoD>();

const ReplicaSetAwareServiceRegistry::Registerer<VectorClockMongoD>
    vectorClockMongoDServiceRegisterer("VectorClockMongoD-ReplicaSetAwareServiceRegistration");

const ServiceContext::ConstructorActionRegisterer vectorClockMongoDRegisterer(
    "VectorClockMongoD-VectorClockRegistration",
    {},
    [](ServiceContext* service) {
        VectorClockMongoD::registerVectorClockOnServiceContext(
            service, &vectorClockMongoDDecoration(service));
    },
    {});

VectorClockMongoD* VectorClockMongoD::get(ServiceContext* serviceContext) {
    return &vectorClockMongoDDecoration(serviceContext);
}

VectorClockMongoD::VectorClockMongoD() = default;

VectorClockMongoD::~VectorClockMongoD() = default;

void VectorClockMongoD::onStepUpBegin(OperationContext* opCtx, long long term) {
    stdx::lock_guard lg(_mutex);
    _durableTime.reset();
}

void VectorClockMongoD::onStepDown() {
    stdx::lock_guard lg(_mutex);
    _durableTime.reset();
}

void VectorClockMongoD::onBecomeArbiter() {
    // The node has become an arbiter, hence will not need logical clock for external operations.
    _disable();

    if (auto validator = LogicalTimeValidator::get(_service)) {
        validator->stopKeyManager();
    }
}

SharedSemiFuture<void> VectorClockMongoD::waitForDurableConfigTime() {
    auto time = getTime();

    stdx::unique_lock ul(_mutex);
    if (_durableTime && _durableTime->configTime() >= time.configTime())
        return SharedSemiFuture<void>();

    return _enqueueWaiterAndScheduleLoopIfNeeded(std::move(ul), std::move(time));
}

SharedSemiFuture<void> VectorClockMongoD::waitForDurableTopologyTime() {
    auto time = getTime();

    stdx::unique_lock ul(_mutex);
    if (_durableTime && _durableTime->topologyTime() >= time.topologyTime())
        return SharedSemiFuture<void>();

    return _enqueueWaiterAndScheduleLoopIfNeeded(std::move(ul), std::move(time));
}

SharedSemiFuture<void> VectorClockMongoD::waitForDurable() {
    auto time = getTime();

    stdx::unique_lock ul(_mutex);
    if (_durableTime && _durableTime->configTime() >= time.configTime() &&
        _durableTime->topologyTime() >= time.topologyTime())
        return SharedSemiFuture<void>();

    return _enqueueWaiterAndScheduleLoopIfNeeded(std::move(ul), std::move(time));
}

SharedSemiFuture<void> VectorClockMongoD::recover() {
    stdx::unique_lock ul(_mutex);
    if (_durableTime)
        return SharedSemiFuture<void>();

    return _enqueueWaiterAndScheduleLoopIfNeeded(std::move(ul), VectorTime());
}

SharedSemiFuture<void> VectorClockMongoD::_enqueueWaiterAndScheduleLoopIfNeeded(
    stdx::unique_lock<Mutex> ul, VectorTime time) {
    auto [it, unusedEmplaced] =
        _queue.try_emplace({std::move(time)}, std::make_unique<SharedPromise<void>>());

    if (!_loopScheduled) {
        _loopScheduled = true;

        auto joinPreviousLoop(_currentWhileLoop ? std::move(*_currentWhileLoop)
                                                : Future<void>::makeReady());

        _currentWhileLoop.emplace(std::move(joinPreviousLoop).onCompletion([this](auto) {
            return _doWhileQueueNotEmptyOrError(vectorClockMongoDDecoration.owner(this));
        }));
    }

    return it->second->getFuture();
}

Future<void> VectorClockMongoD::_doWhileQueueNotEmptyOrError(ServiceContext* service) {
    auto [p, f] = makePromiseFuture<VectorTime>();
    auto future = std::move(f)
                      .then([this](VectorTime newDurableTime) {
                          stdx::unique_lock ul(_mutex);
                          _durableTime.emplace(newDurableTime);

                          ComparableVectorTime time{*_durableTime};

                          std::vector<Queue::value_type::second_type> promises;
                          for (auto it = _queue.begin(); it != _queue.end();) {
                              if (it->first > time)
                                  break;
                              promises.emplace_back(std::move(it->second));
                              it = _queue.erase(it);
                          }
                          ul.unlock();

                          // Make sure the VectorClock advances at least up to the just recovered
                          // durable time
                          _advanceTime({newDurableTime.clusterTime(),
                                        newDurableTime.configTime(),
                                        newDurableTime.topologyTime()});

                          for (auto& p : promises)
                              p->emplaceValue();
                      })
                      .onError([this](Status status) {
                          stdx::unique_lock ul(_mutex);
                          std::vector<Queue::value_type::second_type> promises;
                          for (auto it = _queue.begin(); it != _queue.end();) {
                              promises.emplace_back(std::move(it->second));
                              it = _queue.erase(it);
                          }
                          ul.unlock();

                          for (auto& p : promises)
                              p->setError(status);
                      })
                      .onCompletion([this, service](auto) {
                          {
                              stdx::lock_guard lg(_mutex);
                              if (_queue.empty()) {
                                  _loopScheduled = false;
                                  return Future<void>::makeReady();
                              }
                          }
                          return _doWhileQueueNotEmptyOrError(service);
                      });

    // Blocking work to recover and/or persist the current vector time
    ExecutorFuture<void>(Grid::get(service)->getExecutorPool()->getFixedExecutor())
        .then([this, service] {
            auto mustRecoverDurableTime = [&] {
                stdx::lock_guard lg(_mutex);
                return !_durableTime;
            }();

            ThreadClient tc("VectorClockStateOperation", service);

            {
                stdx::lock_guard<Client> lk(*tc.get());
                tc->setSystemOperationKillableByStepdown(lk);
            }

            const auto opCtxHolder = tc->makeOperationContext();
            auto* const opCtx = opCtxHolder.get();

            if (mustRecoverDurableTime) {
                VectorClockDocument durableVectorClock;

                PersistentTaskStore<VectorClockDocument> store(
                    NamespaceString::kVectorClockNamespace);
                store.forEach(
                    opCtx,
                    BSON(VectorClockDocument::k_idFieldName << durableVectorClock.get_id()),
                    [&, numDocsFound = 0](const auto& doc) mutable {
                        invariant(++numDocsFound == 1);
                        durableVectorClock = doc;
                        return true;
                    });

                return VectorTime({LogicalTime(Timestamp(0)),
                                   LogicalTime(durableVectorClock.getConfigTime()),
                                   LogicalTime(durableVectorClock.getTopologyTime())});
            }

            auto vectorTime = getTime();
            const VectorClockDocument vcd(vectorTime.configTime().asTimestamp(),
                                          vectorTime.topologyTime().asTimestamp());

            PersistentTaskStore<VectorClockDocument> store(NamespaceString::kVectorClockNamespace);
            store.upsert(opCtx,
                         BSON(VectorClockDocument::k_idFieldName << vcd.get_id()),
                         vcd.toBSON(),
                         WriteConcerns::kMajorityWriteConcernNoTimeout);
            return vectorTime;
        })
        .getAsync([this, promise = std::move(p)](StatusWith<VectorTime> swResult) mutable {
            promise.setFrom(std::move(swResult));
        });

    return future;
}

VectorClock::ComponentSet VectorClockMongoD::_gossipOutInternal() const {
    VectorClock::ComponentSet toGossip{Component::ClusterTime};
    if (serverGlobalParams.clusterRole == ClusterRole::ShardServer ||
        serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
        toGossip.insert(Component::ConfigTime);
        toGossip.insert(Component::TopologyTime);
    }
    return toGossip;
}

VectorClock::ComponentSet VectorClockMongoD::_gossipInInternal() const {
    VectorClock::ComponentSet toGossip{Component::ClusterTime};
    if (serverGlobalParams.clusterRole == ClusterRole::ShardServer) {
        toGossip.insert(Component::ConfigTime);
        toGossip.insert(Component::TopologyTime);
    }
    return toGossip;
}

LogicalTime VectorClockMongoD::_tick(Component component, uint64_t nTicks) {
    if (component == Component::ClusterTime) {
        // Although conceptually ClusterTime can only be ticked when a mongod is able to take writes
        // (ie. primary, or standalone), this is handled at a higher layer.
        //
        // ClusterTime is ticked when replacing zero-valued Timestamps with the current time, which
        // is usually but not necessarily associated with writes.
        //
        // ClusterTime is ticked after winning an election, while persisting the stepUp to the
        // oplog, which is slightly before the repl state is changed to primary.
        //
        // As such, ticking ClusterTime is not restricted here based on repl state.

        return _advanceComponentTimeByTicks(component, nTicks);
    }

    // tick is not permitted in other circumstances.
    MONGO_UNREACHABLE;
}

void VectorClockMongoD::_tickTo(Component component, LogicalTime newTime) {
    if (component == Component::ClusterTime) {
        // The ClusterTime is allowed to tickTo in certain very limited and trusted cases (eg.
        // initializing based on oplog timestamps), so we have to allow it here.
        _advanceComponentTimeTo(component, std::move(newTime));
        return;
    }

    if (component == Component::ConfigTime) {
        // The ConfigTime is allowed to be tickTo on the ConfigServer and on the shard only when
        // called from ShardingStateRecovery
        // TODO SERVER-60110 re-add clusterRole == configServer condition like for TopologyTime
        _advanceComponentTimeTo(component, std::move(newTime));
        return;
    }

    if (component == Component::TopologyTime &&
        serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
        _advanceComponentTimeTo(component, std::move(newTime));
        return;
    }

    // tickTo is not permitted in other circumstances.
    MONGO_UNREACHABLE;
}

}  // namespace
}  // namespace mongo
