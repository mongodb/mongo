// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/sharding_environment/mongos_hello_response.h"
#include "mongo/rpc/topology_version_gen.h"
#include "mongo/transport/hello_metrics.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <memory>
#include <mutex>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class [[MONGO_MOD_NEEDS_REPLACEMENT]] MongosTopologyCoordinator {
public:
    static MongosTopologyCoordinator* get(OperationContext* opCtx);

    /**
     * Constructs a Mongos Topology Coordinator object.
     **/
    MongosTopologyCoordinator();

    /**
     * Constructs and returns a MongosHelloResponse. Will block until the given deadline waiting
     * for a significant topology change if the 'counter' field of 'clientTopologyVersion' is equal
     * to the current TopologyVersion 'counter' maintained by this class. Returns immediately if
     * 'clientTopologyVersion' < TopologyVersion of this class or if the processId
     * differs.
     *
     * Note that Quiesce Mode is the only valid topology change on mongos.
     */
    std::shared_ptr<const MongosHelloResponse> awaitHelloResponse(
        OperationContext* opCtx,
        boost::optional<TopologyVersion> clientTopologyVersion,
        boost::optional<Date_t> deadline) const;

    /**
     * We only enter quiesce mode during the shutdown process, which means that the
     * MongosTopologyCoordinator will never need to reset _inQuiesceMode. This function causes us
     * to increment the topologyVersion and start failing hello requests with ShutdownInProgress.
     * This will inform clients to route new operations to another mongos.
     *
     * We also sleep for quiesceTime, which allows short running operations to finish.
     */
    void enterQuiesceModeAndWait(OperationContext* opCtx, Milliseconds quiesceTime);

    TopologyVersion getTopologyVersion() const {
        std::lock_guard lk(_mutex);
        return _topologyVersion;
    }

    bool inQuiesceMode() const {
        std::lock_guard lk(_mutex);
        return _inQuiesceMode;
    }


private:
    using SharedPromiseOfMongosHelloResponse =
        SharedPromise<std::shared_ptr<const MongosHelloResponse>>;

    /**
     * Calculates the time (in millis) left in quiesce mode and converts the value to int64.
     */
    long long _calculateRemainingQuiesceTimeMillis() const;

    /**
     * Helper for constructing a MongosHelloResponse.
     **/
    std::shared_ptr<MongosHelloResponse> _makeHelloResponse(WithLock) const;

    //
    // All member variables are labeled with one of the following codes indicating the
    // synchronization rules for accessing them.
    //
    // (M)  Reads and writes guarded by _mutex

    // Protects member data of this MongosTopologyCoordinator.
    mutable std::mutex _mutex;

    // Keeps track of the current mongos TopologyVersion.
    TopologyVersion _topologyVersion;  // (M)

    // True if we're in quiesce mode.  If true, we'll respond to hello requests with ok:0.
    bool _inQuiesceMode;  // (M)

    // The deadline until which quiesce mode will last.
    Date_t _quiesceDeadline;  // (M)

    // The promise waited on by awaitable hello requests on mongos.
    std::shared_ptr<SharedPromiseOfMongosHelloResponse> _promise;  // (M)
};

}  // namespace mongo
