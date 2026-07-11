// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/topology/vector_clock/topology_time_ticker.h"

#include "mongo/db/client.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/topology/vector_clock/vector_clock_mutable.h"
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
        std::lock_guard lg(_mutex);

        // Inserts only if commitTime was not present yet.
        auto [iter, inserted] =
            _topologyTimeByLocalCommitTime.try_emplace(commitTime, topologyTime);

        // If the key already existed (!inserted), we overwrite its topologyTime value if the new
        // one is higher.
        if (!inserted && topologyTime > iter->second) {
            iter->second = topologyTime;
        }

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

    const auto optMaxMajorityCommittedTopologyTime = [&]() -> boost::optional<LogicalTime> {
        std::lock_guard lg(_mutex);

        if (_topologyTimeByLocalCommitTime.empty()) {
            return boost::none;
        }

        // Looking for the first tick point that is not majority committed
        auto itFirstTickPointNonMajorityCommitted =
            _topologyTimeByLocalCommitTime.upper_bound(newMajorityTimestamp);

        // If the very first element is > majorityTimestamp, we have nothing to commit.
        if (itFirstTickPointNonMajorityCommitted == _topologyTimeByLocalCommitTime.begin()) {
            return boost::none;
        }

        // We have at least one committed tick. Find the max TopologyTime among them.
        const auto maxMajorityCommittedTopologyTimeIt =
            std::max_element(_topologyTimeByLocalCommitTime.begin(),
                             itFirstTickPointNonMajorityCommitted,
                             [](const auto& a, const auto& b) { return a.second < b.second; });

        LogicalTime maxTopologyTime(maxMajorityCommittedTopologyTimeIt->second);

        // Cleanup processed entries
        _topologyTimeByLocalCommitTime.erase(_topologyTimeByLocalCommitTime.begin(),
                                             itFirstTickPointNonMajorityCommitted);

        return maxTopologyTime;
    }();

    // Notify the VectorClock without holding the mutex.
    if (optMaxMajorityCommittedTopologyTime) {
        VectorClockMutable::get(service)->tickTopologyTimeTo(*optMaxMajorityCommittedTopologyTime);
    }
}

void TopologyTimeTicker::onReplicationRollback(const repl::OpTime& lastAppliedOpTime) {
    Timestamp newestTimestamp = lastAppliedOpTime.getTimestamp();

    std::lock_guard lg(_mutex);
    auto itFirstElemToBeRemoved = _topologyTimeByLocalCommitTime.upper_bound(newestTimestamp);

    _topologyTimeByLocalCommitTime.erase(itFirstElemToBeRemoved,
                                         _topologyTimeByLocalCommitTime.end());
}

std::map<Timestamp, Timestamp> TopologyTimeTicker::getTopologyTimeByLocalCommitTime_forTest()
    const {
    std::lock_guard lg(_mutex);
    return _topologyTimeByLocalCommitTime;
};

}  // namespace mongo
