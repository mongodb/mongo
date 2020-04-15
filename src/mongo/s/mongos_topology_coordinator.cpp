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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/logv2/log.h"

#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/s/mongos_topology_coordinator.h"
#include "mongo/util/fail_point.h"

namespace mongo {

namespace {

const auto getMongosTopologyCoordinator =
    ServiceContext::declareDecoration<MongosTopologyCoordinator>();

/**
 * Generate an identifier unique to this instance.
 */
OID instanceId;

MONGO_INITIALIZER(GenerateMongosInstanceId)(InitializerContext*) {
    instanceId = OID::gen();
    return Status::OK();
}

// Awaitable isMaster requests with the proper topologyVersions are expected to wait for
// maxAwaitTimeMS on mongos. When set, this failpoint will hang right before waiting on a
// topology change.
MONGO_FAIL_POINT_DEFINE(hangWhileWaitingForIsMasterResponse);

}  // namespace

// Make MongosTopologyCoordinator a decoration on the ServiceContext.
MongosTopologyCoordinator* MongosTopologyCoordinator::get(OperationContext* opCtx) {
    return &getMongosTopologyCoordinator(opCtx->getClient()->getServiceContext());
}

MongosTopologyCoordinator::MongosTopologyCoordinator() : _topologyVersion(instanceId, 0) {}

std::shared_ptr<MongosIsMasterResponse> MongosTopologyCoordinator::_makeIsMasterResponse(
    WithLock lock) const {
    auto response = std::make_shared<MongosIsMasterResponse>(_topologyVersion);
    return response;
}

std::shared_ptr<const MongosIsMasterResponse> MongosTopologyCoordinator::awaitIsMasterResponse(
    OperationContext* opCtx,
    boost::optional<TopologyVersion> clientTopologyVersion,
    boost::optional<long long> maxAwaitTimeMS) const {
    stdx::unique_lock lk(_mutex);

    // Respond immediately if:
    // (1) There is no clientTopologyVersion, which indicates that the client is not using
    //     awaitable ismaster.
    // (2) The process IDs are different.
    // (3) The clientTopologyVersion counter is less than mongos' counter.
    if (!clientTopologyVersion ||
        clientTopologyVersion->getProcessId() != _topologyVersion.getProcessId() ||
        clientTopologyVersion->getCounter() < _topologyVersion.getCounter()) {
        return _makeIsMasterResponse(lk);
    }
    uassert(51761,
            str::stream() << "Received a topology version with counter: "
                          << clientTopologyVersion->getCounter()
                          << " which is greater than the mongos topology version counter: "
                          << _topologyVersion.getCounter(),
            clientTopologyVersion->getCounter() == _topologyVersion.getCounter());

    lk.unlock();

    // At this point, we have verified that clientTopologyVersion is not none. It this is true,
    // maxAwaitTimeMS must also be not none.
    invariant(maxAwaitTimeMS);

    IsMasterMetrics::get(opCtx)->incrementNumAwaitingTopologyChanges();

    ON_BLOCK_EXIT([&] { IsMasterMetrics::get(opCtx)->decrementNumAwaitingTopologyChanges(); });

    if (MONGO_unlikely(hangWhileWaitingForIsMasterResponse.shouldFail())) {
        LOGV2(4695501, "hangWhileWaitingForIsMasterResponse failpoint enabled");
        hangWhileWaitingForIsMasterResponse.pauseWhileSet(opCtx);
    }

    // Sleep for maxTimeMS.
    LOGV2_DEBUG(4695502,
                1,
                "Waiting for an isMaster response for maxAwaitTimeMS",
                "maxAwaitTimeMS"_attr = maxAwaitTimeMS.get(),
                "currentMongosTopologyVersionCounter"_attr = _topologyVersion.getCounter());

    opCtx->sleepFor(Milliseconds(*maxAwaitTimeMS));

    lk.lock();
    return _makeIsMasterResponse(lk);
}

}  // namespace mongo
