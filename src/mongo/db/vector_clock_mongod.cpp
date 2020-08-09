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

    SharedSemiFuture<void> persist() override;
    void waitForInMemoryVectorClockToBePersisted() override;

    SharedSemiFuture<void> recover() override;
    void waitForVectorClockToBeRecovered() override;

private:
    // VectorClock methods implementation

    bool _gossipOutInternal(OperationContext* opCtx,
                            BSONObjBuilder* out,
                            const LogicalTimeArray& time) const override;
    bool _gossipOutExternal(OperationContext* opCtx,
                            BSONObjBuilder* out,
                            const LogicalTimeArray& time) const override;
    LogicalTimeArray _gossipInInternal(OperationContext* opCtx,
                                       const BSONObj& in,
                                       bool couldBeUnauthenticated) override;
    LogicalTimeArray _gossipInExternal(OperationContext* opCtx,
                                       const BSONObj& in,
                                       bool couldBeUnauthenticated) override;
    bool _permitRefreshDuringGossipOut() const override {
        return false;
    }

    // VectorClockMutable methods implementation

    LogicalTime _tick(Component component, uint64_t nTicks) override;
    void _tickTo(Component component, LogicalTime newTime) override;

    // ReplicaSetAwareService methods implementation

    void onStartup(OperationContext* opCtx) override {}
    void onStepUpBegin(OperationContext* opCtx, long long term) override {}
    void onStepUpComplete(OperationContext* opCtx, long long term) override {}
    void onStepDown() override {}
    void onBecomeArbiter() override;

    void _recoverComponent(OperationContext* opCtx,
                           const BSONObj& in,
                           LogicalTimeArray* newTime,
                           Component component);

    void _persistComponent(OperationContext* opCtx,
                           BSONObjBuilder* out,
                           const VectorTime& time,
                           Component component) const;

    /*
     * Manages the components persistence format, stripping field names of intitial '$' symbol.
     */
    class PersistenceComponentFormat : public VectorClock::ComponentFormat {
    public:
        using ComponentFormat::ComponentFormat;
        virtual ~PersistenceComponentFormat() = default;

        const std::string bsonFieldName = _fieldName[0] == '$' ? _fieldName.substr(1) : _fieldName;

        bool out(ServiceContext* service,
                 OperationContext* opCtx,
                 bool permitRefresh,
                 BSONObjBuilder* out,
                 LogicalTime time,
                 Component component) const override {
            out->append(bsonFieldName, time.asTimestamp());
            return true;
        }

        LogicalTime in(ServiceContext* service,
                       OperationContext* opCtx,
                       const BSONObj& in,
                       bool couldBeUnauthenticated,
                       Component component) const override {
            const auto componentElem(in[bsonFieldName]);

            uassert(ErrorCodes::InvalidBSON,
                    str::stream() << bsonFieldName << " field not found",
                    !componentElem.eoo());

            uassert(ErrorCodes::BadValue,
                    str::stream() << _fieldName << " is not a Timestamp",
                    componentElem.type() == bsonTimestamp);

            return LogicalTime(componentElem.timestamp());
        }
    };

    /*
     * A VectorClockStateOperation represents an asynchronous operation on the VectorClockDocument
     * guarded by a mutex. Calling threads are joining the ongoing operation or - in case no
     * operation is in progress - scheduling a new one.
     */
    class VectorClockStateOperation {
    public:
        VectorClockStateOperation() = default;
        ~VectorClockStateOperation() = default;

        SharedSemiFuture<void> performOperation(VectorClockMongoD* vectorClock,
                                                ServiceContext* serviceContext) {
            stdx::lock_guard<Latch> lk(_opMutex);
            _opFuture =
                _opFuture
                    .thenRunOn(Grid::get(serviceContext)->getExecutorPool()->getFixedExecutor())
                    .then([this, vectorClock, serviceContext, initialGeneration = _generation] {
                        stdx::unique_lock<Latch> lk(_opMutex);

                        invariant(_generation >= initialGeneration);
                        if (_generation > initialGeneration) {
                            // The last run of this operation has definitively observed the
                            // scheduling thread's in-memory state. There is no need to run
                            // this operation again.
                            return;
                        }
                        ++_generation;

                        lk.unlock();

                        ThreadClient tc("VectorClockStateOperation", serviceContext);
                        {
                            stdx::lock_guard<Client> lk(*tc.get());
                            tc->setSystemOperationKillable(lk);
                        }

                        const auto opCtx = tc->makeOperationContext();
                        execute(vectorClock, opCtx.get());
                    })
                    .onError([=](const Status status) {
                        LOGV2(4924402,
                              "Error while performing a VectorClockStateOperation",
                              "opName"_attr = getOperationName(),
                              "error"_attr = status);
                        return status;
                    })
                    .share();

            return _opFuture;
        }

        virtual std::string getOperationName() = 0;

        void waitForCompletion() {
            _opFuture.get();
        }

    private:
        virtual void execute(VectorClockMongoD* vectorClock, OperationContext* opCtx) = 0;

        Mutex _opMutex = MONGO_MAKE_LATCH("VectorClockStateOperation::_opMutex");

        SharedSemiFuture<void> _opFuture;

        size_t _generation = 0;
    };

    /*
     * VectorClockStateOperation persisting configTime and topologyTime in the VectorClockDocument.
     */
    class PersistOperation : public VectorClockStateOperation {
    private:
        void execute(VectorClockMongoD* vectorClock, OperationContext* opCtx) override {
            const auto time = vectorClock->getTime();

            NamespaceString nss(NamespaceString::kVectorClockNamespace);
            PersistentTaskStore<VectorClockDocument> store(nss);

            BSONObjBuilder bob;
            VectorClockDocument vcd;

            vectorClock->_persistComponent(opCtx, &bob, time, Component::ConfigTime);
            vectorClock->_persistComponent(opCtx, &bob, time, Component::TopologyTime);

            auto obj = bob.done();
            vcd.setVectorClock(obj);

            store.update(opCtx,
                         VectorClock::stateQuery(),
                         vcd.toBSON(),
                         WriteConcerns::kMajorityWriteConcern,
                         true);
        }

        std::string getOperationName() override {
            return "localPersist";
        }
    };

    /*
     * VectorClockStateOperation invoking PersistOperation on a shard server's primary.
     */
    class RemotePersistOperation : public VectorClockStateOperation {
    private:
        void execute(VectorClockMongoD* vectorClock, OperationContext* opCtx) override {
            auto const shardingState = ShardingState::get(opCtx);
            invariant(shardingState->enabled());

            auto selfShard = uassertStatusOK(
                Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardingState->shardId()));

            auto cmdResponse = uassertStatusOK(selfShard->runCommandWithFixedRetryAttempts(
                opCtx,
                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                NamespaceString::kVectorClockNamespace.toString(),
                BSON("_vectorClockPersist" << 1),
                Seconds{30},
                Shard::RetryPolicy::kIdempotent));

            uassertStatusOK(cmdResponse.commandStatus);
        }

        std::string getOperationName() override {
            return "remotePersist";
        }
    };

    /*
     * VectorClockStateOperation recovering configTime and topologyTime from the
     * VectorClockDocument.
     */
    class RecoverOperation : public VectorClockStateOperation {
    private:
        void execute(VectorClockMongoD* vectorClock, OperationContext* opCtx) override {
            NamespaceString nss(NamespaceString::kVectorClockNamespace);
            PersistentTaskStore<VectorClockDocument> store(nss);

            int nDocuments = store.count(opCtx, VectorClock::stateQuery());
            if (nDocuments == 0) {
                LOGV2_DEBUG(4924403, 2, "No VectorClockDocument to recover");
                return;
            }
            fassert(4924404, nDocuments == 1);

            store.forEach(opCtx, VectorClock::stateQuery(), [&](const VectorClockDocument& vcd) {
                BSONObj obj = vcd.getVectorClock();

                LogicalTimeArray newTime;
                vectorClock->_recoverComponent(opCtx, obj, &newTime, Component::ConfigTime);
                vectorClock->_recoverComponent(opCtx, obj, &newTime, Component::TopologyTime);
                vectorClock->_advanceTime(std::move(newTime));

                return true;
            });
        }

        std::string getOperationName() override {
            return "recover";
        }
    };

    static const ComponentArray<std::unique_ptr<ComponentFormat>> _vectorClockStateFormatters;

    PersistOperation _persistOperation;
    RemotePersistOperation _remotePersistOperation;
    RecoverOperation _recoverOperation;
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

const VectorClock::ComponentArray<std::unique_ptr<VectorClock::ComponentFormat>>
    VectorClockMongoD::_vectorClockStateFormatters{
        std::make_unique<VectorClockMongoD::PersistenceComponentFormat>(
            VectorClock::kClusterTimeFieldName),
        std::make_unique<VectorClockMongoD::PersistenceComponentFormat>(
            VectorClock::kConfigTimeFieldName),
        std::make_unique<VectorClockMongoD::PersistenceComponentFormat>(
            VectorClock::kTopologyTimeFieldName)};

VectorClockMongoD* VectorClockMongoD::get(ServiceContext* serviceContext) {
    return &vectorClockMongoDDecoration(serviceContext);
}

VectorClockMongoD::VectorClockMongoD() = default;

VectorClockMongoD::~VectorClockMongoD() = default;

void VectorClockMongoD::onBecomeArbiter() {
    // The node has become an arbiter, hence will not need logical clock for external operations.
    _disable();
    if (auto validator = LogicalTimeValidator::get(_service)) {
        validator->stopKeyManager();
    }
}

bool VectorClockMongoD::_gossipOutInternal(OperationContext* opCtx,
                                           BSONObjBuilder* out,
                                           const LogicalTimeArray& time) const {
    bool wasClusterTimeOutput = _gossipOutComponent(opCtx, out, time, Component::ClusterTime);
    if (serverGlobalParams.clusterRole == ClusterRole::ShardServer ||
        serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
        _gossipOutComponent(opCtx, out, time, Component::ConfigTime);
        _gossipOutComponent(opCtx, out, time, Component::TopologyTime);
    }
    return wasClusterTimeOutput;
}

bool VectorClockMongoD::_gossipOutExternal(OperationContext* opCtx,
                                           BSONObjBuilder* out,
                                           const LogicalTimeArray& time) const {
    return _gossipOutComponent(opCtx, out, time, Component::ClusterTime);
}

VectorClock::LogicalTimeArray VectorClockMongoD::_gossipInInternal(OperationContext* opCtx,
                                                                   const BSONObj& in,
                                                                   bool couldBeUnauthenticated) {
    LogicalTimeArray newTime;
    _gossipInComponent(opCtx, in, couldBeUnauthenticated, &newTime, Component::ClusterTime);
    if (serverGlobalParams.clusterRole == ClusterRole::ShardServer) {
        _gossipInComponent(opCtx, in, couldBeUnauthenticated, &newTime, Component::ConfigTime);
        _gossipInComponent(opCtx, in, couldBeUnauthenticated, &newTime, Component::TopologyTime);
    }
    return newTime;
}

VectorClock::LogicalTimeArray VectorClockMongoD::_gossipInExternal(OperationContext* opCtx,
                                                                   const BSONObj& in,
                                                                   bool couldBeUnauthenticated) {
    LogicalTimeArray newTime;
    _gossipInComponent(opCtx, in, couldBeUnauthenticated, &newTime, Component::ClusterTime);
    return newTime;
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

    if (component == Component::ConfigTime &&
        serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
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

void VectorClockMongoD::_persistComponent(OperationContext* opCtx,
                                          BSONObjBuilder* out,
                                          const VectorTime& time,
                                          Component component) const {
    _vectorClockStateFormatters[component]->out(
        _service, opCtx, true, out, time[component], component);
}

void VectorClockMongoD::_recoverComponent(OperationContext* opCtx,
                                          const BSONObj& in,
                                          LogicalTimeArray* newTime,
                                          Component component) {
    (*newTime)[component] = _vectorClockStateFormatters[component]->in(
        _service, opCtx, in, true /*couldBeUnauthenticated*/, component);
}

SharedSemiFuture<void> VectorClockMongoD::persist() {
    if (serverGlobalParams.clusterRole == ClusterRole::ShardServer) {
        auto serviceContext = vectorClockMongoDDecoration.owner(this);

        const auto replCoord = repl::ReplicationCoordinator::get(serviceContext);
        if (replCoord->getMemberState().primary()) {
            return _persistOperation.performOperation(this, serviceContext);
        }

        return _remotePersistOperation.performOperation(this, serviceContext);
    }

    return SharedSemiFuture<void>();
}

void VectorClockMongoD::waitForInMemoryVectorClockToBePersisted() {
    _persistOperation.waitForCompletion();
}

SharedSemiFuture<void> VectorClockMongoD::recover() {
    auto serviceContext = vectorClockMongoDDecoration.owner(this);
    return _recoverOperation.performOperation(this, serviceContext);
}

void VectorClockMongoD::waitForVectorClockToBeRecovered() {
    _recoverOperation.waitForCompletion();
}

}  // namespace
}  // namespace mongo
