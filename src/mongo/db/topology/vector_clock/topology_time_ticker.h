// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/timestamp.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/service_context.h"
#include "mongo/util/modules.h"

#include <map>
#include <mutex>

namespace mongo {

/**
 * This object is responsible for ticking the topology time of the Vector Clock when needed. It only
 * exists on the CSRS nodes and it is associated with the mongod instance.
 *
 * Every time there is a local change on the topology of a sharded cluster this class registers a
 * new TopologyTimeTickPoint. Once the oplog entry associated to that change is majority committed,
 * we can advance the topology time.
 * Since the tick points represent non-majority committed changes, this class has to handle what
 * happens on rollback.
 */
class [[MONGO_MOD_PARENT_PRIVATE]] TopologyTimeTicker {
public:
    TopologyTimeTicker() = default;

    static TopologyTimeTicker& get(ServiceContext* serviceContext);
    static TopologyTimeTicker& get(OperationContext* opCtx);

    /**
     * Registers a new tick point on _topologyTimeByLocalCommitTime.
     * This method is invoked from two places:
     *    - From the OpObservers when writes modify config.shards
     *    - As part of the initialization of the VectorClock on the CSRS.
     */
    void onNewLocallyCommittedTopologyTimeAvailable(Timestamp commitTime, Timestamp topologyTime);

    /**
     *  Advances the topology time if one or more tick points have been majority committed.
     */
    void onMajorityCommitPointUpdate(ServiceContext* service, const repl::OpTime& newCommitPoint);

    /**
     *  Removes from _topologyTimeByLocalCommitTime any topology change that was rollbacked.
     */
    void onReplicationRollback(const repl::OpTime& lastAppliedOpTime);

    std::map<Timestamp, Timestamp> getTopologyTimeByLocalCommitTime_forTest() const;

private:
    mutable std::mutex _mutex;

    /**
     * Container that stores time-related information about a topology change in a sharded cluster.
     * More specifically, the vector clock should start gossiping a new topologyTime once the
     * related local commitTime is majority committed.
     */
    std::map<Timestamp, Timestamp> _topologyTimeByLocalCommitTime;
};
}  // namespace mongo
