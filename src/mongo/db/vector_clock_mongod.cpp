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

#include "mongo/platform/basic.h"

#include "mongo/db/logical_time_validator.h"
#include "mongo/db/replica_set_aware_service.h"
#include "mongo/db/vector_clock_mutable.h"

namespace mongo {
namespace {

/**
 * Vector clock implementation for mongod.
 */
class VectorClockMongoD : public VectorClockMutable,
                          public ReplicaSetAwareService<VectorClockMongoD> {
    VectorClockMongoD(const VectorClockMongoD&) = delete;
    VectorClockMongoD& operator=(const VectorClockMongoD&) = delete;

public:
    static VectorClockMongoD* get(ServiceContext* serviceContext);

    VectorClockMongoD();
    virtual ~VectorClockMongoD();

    LogicalTime tick(Component component, uint64_t nTicks) override;
    void tickTo(Component component, LogicalTime newTime) override;

protected:
    void _gossipOutInternal(BSONObjBuilder* out) const override;
    void _gossipOutExternal(BSONObjBuilder* out) const override;
    LogicalTimeArray _gossipInInternal(const BSONObj& in) override;
    LogicalTimeArray _gossipInExternal(const BSONObj& in) override;

private:
    enum class ReplState {
        Unset,
        StepUpBegin,
        StepUpComplete,
        StepDown,
        Arbiter,
    };

    void onStepUpBegin(OperationContext* opCtx) override;
    void onStepUpComplete(OperationContext* opCtx) override;
    void onStepDown() override;
    void onBecomeArbiter() override;

    ReplState _replState{ReplState::Unset};
};

const auto vectorClockMongoDDecoration = ServiceContext::declareDecoration<VectorClockMongoD>();

const ReplicaSetAwareServiceRegistry::Registerer<VectorClockMongoD> vectorClockMongoDRegisterer(
    "VectorClockMongoD-ReplicaSetAwareServiceRegistration");

VectorClockMongoD* VectorClockMongoD::get(ServiceContext* serviceContext) {
    return &vectorClockMongoDDecoration(serviceContext);
}

ServiceContext::ConstructorActionRegisterer _registerer(
    "VectorClockMongoD-VectorClockRegistration",
    {},
    [](ServiceContext* service) {
        VectorClockMongoD::registerVectorClockOnServiceContext(
            service, &vectorClockMongoDDecoration(service));
    },
    {});

VectorClockMongoD::VectorClockMongoD() = default;

VectorClockMongoD::~VectorClockMongoD() = default;

void VectorClockMongoD::onStepUpBegin(OperationContext* opCtx) {
    _replState = ReplState::StepUpBegin;
}

void VectorClockMongoD::onStepUpComplete(OperationContext* opCtx) {
    _replState = ReplState::StepUpComplete;
}

void VectorClockMongoD::onStepDown() {
    _replState = ReplState::StepDown;
}

void VectorClockMongoD::onBecomeArbiter() {
    _replState = ReplState::Arbiter;

    // The node has become an arbiter, hence will not need logical clock for external operations.
    disable();
}

void VectorClockMongoD::_gossipOutInternal(BSONObjBuilder* out) const {
    VectorTime now = getTime();
    // TODO SERVER-47914: re-enable gossipping of VectorClock's ClusterTime once LogicalClock has
    // been migrated into VectorClock.
    // _gossipOutComponent(out, now, Component::ClusterTime);
    if (serverGlobalParams.clusterRole == ClusterRole::ShardServer ||
        serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
        _gossipOutComponent(out, now, Component::ConfigTime);
    }
}

void VectorClockMongoD::_gossipOutExternal(BSONObjBuilder* out) const {
    // TODO SERVER-47914: re-enable gossipping of VectorClock's ClusterTime once LogicalClock has
    // been migrated into VectorClock.
    // VectorTime now = getTime();
    // _gossipOutComponent(out, now, Component::ClusterTime);
}

VectorClock::LogicalTimeArray VectorClockMongoD::_gossipInInternal(const BSONObj& in) {
    LogicalTimeArray newTime;
    // TODO SERVER-47914: re-enable gossipping of VectorClock's ClusterTime once LogicalClock has
    // been migrated into VectorClock.
    // _gossipInComponent(in, &newTime, Component::ClusterTime);
    if (serverGlobalParams.clusterRole == ClusterRole::ShardServer) {
        _gossipInComponent(in, &newTime, Component::ConfigTime);
    }
    return newTime;
}

VectorClock::LogicalTimeArray VectorClockMongoD::_gossipInExternal(const BSONObj& in) {
    LogicalTimeArray newTime;
    // TODO SERVER-47914: re-enable gossipping of VectorClock's ClusterTime once LogicalClock has
    // been migrated into VectorClock.
    // _gossipInComponent(in, &newTime, Component::ClusterTime);
    return newTime;
}

LogicalTime VectorClockMongoD::tick(Component component, uint64_t nTicks) {
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

void VectorClockMongoD::tickTo(Component component, LogicalTime newTime) {
    if (component == Component::ConfigTime &&
        serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
        _advanceComponentTimeTo(component, std::move(newTime));
        return;
    }

    // tickTo is not permitted in other circumstances.
    MONGO_UNREACHABLE;
}

}  // namespace
}  // namespace mongo
