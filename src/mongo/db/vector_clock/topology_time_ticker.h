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

#pragma once

#include "mongo/bson/timestamp.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/mutex.h"

#include <map>
#include <vector>

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
class TopologyTimeTicker {
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

private:
    stdx::mutex _mutex;
    /**
     * Container that stores time-related information about a topology change in a sharded cluster.
     * More specifically, the vector clock should start gossiping a new topologyTime once the
     * related local commitTime is majority committed.
     */
    std::map<Timestamp, Timestamp> _topologyTimeByLocalCommitTime;
};
}  // namespace mongo
