/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/topology_time_ticker.h"

#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/vector_clock_mutable.h"
#include "mongo/logv2/log.h"

namespace mongo {

namespace {

// The number of pending topologyTime tick points (stored in the _topologyTimeTickPoints vector) is
// generally expected to be small (since shard topology operations should be infrequent, relative to
// any config server replication lag).  If the size of this vector exceeds this constant (when a
// tick point is added), then a warning (with id 4740600) will be logged.
constexpr size_t kPossiblyExcessiveNumTopologyTimeTickPoints = 3;

const auto serviceDecorator = ServiceContext::declareDecoration<TopologyTimeTicker>();

}  // namespace

namespace topology_time_ticker_utils {

bool inRecoveryMode(OperationContext* opCtx) {
    invariant(opCtx->lockState()->isRSTLLocked());

    const auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (!replCoord->isReplEnabled()) {
        return false;
    }

    const auto memberState = replCoord->getMemberState();
    return memberState.startup2() || memberState.rollback();
}

}  // namespace topology_time_ticker_utils

TopologyTimeTicker& TopologyTimeTicker::get(ServiceContext* serviceContext) {
    return serviceDecorator(serviceContext);
}

TopologyTimeTicker& TopologyTimeTicker::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

void TopologyTimeTicker::onNewLocallyCommittedTopologyTimeAvailable(Timestamp commitTime,
                                                                    Timestamp topologyTime) {
    const auto numTickPoints = [&] {
        stdx::lock_guard lg(_mutex);
        invariant(_topologyTimeTickPoints.size() == 0 ||
                  _topologyTimeTickPoints.back().commitTime < commitTime);
        _topologyTimeTickPoints.push_back({commitTime, topologyTime});
        return _topologyTimeTickPoints.size();
    }();

    if (numTickPoints >= kPossiblyExcessiveNumTopologyTimeTickPoints) {
        LOGV2_WARNING(4740600,
                      "possibly excessive number of topologyTime tick points",
                      "numTickPoints"_attr = numTickPoints,
                      "kPossiblyExcessiveNumTopologyTimeTickPoints"_attr =
                          kPossiblyExcessiveNumTopologyTimeTickPoints);
    }
}

void TopologyTimeTicker::onMajorityCommitPointUpdate(ServiceContext* service,
                                                     const repl::OpTime& newCommitPoint) {
    Timestamp newMajorityTimestamp = newCommitPoint.getTimestamp();
    stdx::lock_guard lg(_mutex);
    if (_topologyTimeTickPoints.empty())
        return;

    // Looking for the first tick point that is not majority committed
    auto itFirstTickPointNonMajorityCommitted =
        std::find_if(_topologyTimeTickPoints.begin(),
                     _topologyTimeTickPoints.end(),
                     [newMajorityTimestamp](const TopologyTimeTickPoint& tick) {
                         return tick.commitTime > newMajorityTimestamp;
                     });

    if (itFirstTickPointNonMajorityCommitted != _topologyTimeTickPoints.begin()) {
        // If some ticks were majority committed, advance the TopologyTime to the most recent one
        const auto maxMajorityCommittedTopologyTime =
            (itFirstTickPointNonMajorityCommitted - 1)->topologyTime;

        VectorClockMutable::get(service)->tickTopologyTimeTo(
            LogicalTime(maxMajorityCommittedTopologyTime));

        _topologyTimeTickPoints.erase(_topologyTimeTickPoints.begin(),
                                      itFirstTickPointNonMajorityCommitted);
    }
}

void TopologyTimeTicker::onReplicationRollback(const repl::OpTime& lastAppliedOpTime) {
    Timestamp newestTimestamp = lastAppliedOpTime.getTimestamp();

    stdx::lock_guard lg(_mutex);
    auto itFirstElemToBeRemoved =
        std::find_if(_topologyTimeTickPoints.begin(),
                     _topologyTimeTickPoints.end(),
                     [newestTimestamp](const TopologyTimeTickPoint& tick) {
                         return newestTimestamp < tick.commitTime;
                     });

    _topologyTimeTickPoints.erase(itFirstElemToBeRemoved, _topologyTimeTickPoints.end());
}

}  // namespace mongo
