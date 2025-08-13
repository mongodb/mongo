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


#include "mongo/db/vector_clock/topology_time_ticker.h"

#include "mongo/db/client.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/vector_clock/vector_clock_mutable.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"

#include <cstddef>
#include <iterator>
#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {

namespace {

// The number of pending topologyTime tick points (stored in _topologyTimeByLocalCommitTime) is
// generally expected to be small (since shard topology operations should be infrequent, relative to
// any config server replication lag).  If its size exceeds this constant (when a tick point is
// added), then a warning (with id 4740600) will be logged.
constexpr size_t kPossiblyExcessiveNumTopologyTimeTickPoints = 3;

const auto serviceDecorator = ServiceContext::declareDecoration<TopologyTimeTicker>();

}  // namespace

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
        bool skipCausalConsistencyCheck = [] {
            auto opCtx = cc().getOperationContext();
            if (!opCtx || opCtx->isEnforcingConstraints()) {
                // Default case.
                return false;
            }

            // The callback is being invoked within the context of an oplog application, where
            // entries may be received in non strict order for optimisation reasons. Such case may
            // be considered safe.
            return true;
        }();

        invariant(skipCausalConsistencyCheck || _topologyTimeByLocalCommitTime.size() == 0 ||
                  _topologyTimeByLocalCommitTime.crbegin()->first < commitTime);
        _topologyTimeByLocalCommitTime.emplace(commitTime, topologyTime);
        return _topologyTimeByLocalCommitTime.size();
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
    if (_topologyTimeByLocalCommitTime.empty())
        return;

    // Looking for the first tick point that is not majority committed
    auto itFirstTickPointNonMajorityCommitted =
        _topologyTimeByLocalCommitTime.upper_bound(newMajorityTimestamp);


    if (itFirstTickPointNonMajorityCommitted != _topologyTimeByLocalCommitTime.begin()) {
        // If some ticks were majority committed, advance the TopologyTime to the most recent one
        const auto maxMajorityCommittedTopologyTime =
            std::prev(itFirstTickPointNonMajorityCommitted)->second;

        VectorClockMutable::get(service)->tickTopologyTimeTo(
            LogicalTime(maxMajorityCommittedTopologyTime));

        _topologyTimeByLocalCommitTime.erase(_topologyTimeByLocalCommitTime.begin(),
                                             itFirstTickPointNonMajorityCommitted);
    }
}

void TopologyTimeTicker::onReplicationRollback(const repl::OpTime& lastAppliedOpTime) {
    Timestamp newestTimestamp = lastAppliedOpTime.getTimestamp();

    stdx::lock_guard lg(_mutex);
    auto itFirstElemToBeRemoved = _topologyTimeByLocalCommitTime.upper_bound(newestTimestamp);

    _topologyTimeByLocalCommitTime.erase(itFirstElemToBeRemoved,
                                         _topologyTimeByLocalCommitTime.end());
}

}  // namespace mongo
