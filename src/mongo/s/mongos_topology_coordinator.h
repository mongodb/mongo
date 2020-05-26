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

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/platform/mutex.h"
#include "mongo/rpc/topology_version_gen.h"
#include "mongo/s/mongos_is_master_response.h"
#include "mongo/transport/ismaster_metrics.h"
#include "mongo/util/concurrency/with_lock.h"
#include <memory>

namespace mongo {

class MongosTopologyCoordinator {
public:
    static MongosTopologyCoordinator* get(OperationContext* opCtx);

    /**
     * Constructs a Mongos Topology Coordinator object.
     **/
    MongosTopologyCoordinator();

    /**
     * Constructs and returns a MongosIsMasterResponse. Will block until the given deadline waiting
     * for a significant topology change if the 'counter' field of 'clientTopologyVersion' is equal
     * to the current TopologyVersion 'counter' maintained by this class. Returns immediately if
     * 'clientTopologyVersion' < TopologyVersion of this class or if the processId
     * differs.
     *
     * Note that Quiesce Mode is the only valid topology change on mongos.
     */
    std::shared_ptr<const MongosIsMasterResponse> awaitIsMasterResponse(
        OperationContext* opCtx,
        boost::optional<TopologyVersion> clientTopologyVersion,
        boost::optional<Date_t> deadline) const;

    /**
     * We only enter quiesce mode during the shutdown process, which means that the
     * MongosTopologyCoordinator will never need to reset _inQuiesceMode. This function causes us
     * to increment the topologyVersion and start failing isMaster requests with ShutdownInProgress.
     * This will inform clients to route new operations to another mongos.
     *
     * We also sleep for quiesceTime, which allows short running operations to finish.
     */
    void enterQuiesceModeAndWait(OperationContext* opCtx, Milliseconds quiesceTime);

    TopologyVersion getTopologyVersion() const {
        stdx::lock_guard lk(_mutex);
        return _topologyVersion;
    }

    bool inQuiesceMode() const {
        stdx::lock_guard lk(_mutex);
        return _inQuiesceMode;
    }


private:
    using SharedPromiseOfMongosIsMasterResponse =
        SharedPromise<std::shared_ptr<const MongosIsMasterResponse>>;

    /**
     * Helper for constructing a MongosIsMasterResponse.
     **/
    std::shared_ptr<MongosIsMasterResponse> _makeIsMasterResponse(WithLock) const;

    //
    // All member variables are labeled with one of the following codes indicating the
    // synchronization rules for accessing them.
    //
    // (M)  Reads and writes guarded by _mutex

    // Protects member data of this MongosTopologyCoordinator.
    mutable Mutex _mutex = MONGO_MAKE_LATCH("MongosTopologyCoordinator::_mutex");

    // Keeps track of the current mongos TopologyVersion.
    TopologyVersion _topologyVersion;  // (M)

    // True if we're in quiesce mode.  If true, we'll respond to isMaster requests with ok:0.
    bool _inQuiesceMode;  // (M)

    // The promise waited on by awaitable isMaster requests on mongos.
    std::shared_ptr<SharedPromiseOfMongosIsMasterResponse> _promise;  // (M)
};

}  // namespace mongo
