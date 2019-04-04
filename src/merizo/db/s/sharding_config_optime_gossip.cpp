/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
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

#define MERIZO_LOG_DEFAULT_COMPONENT ::merizo::logger::LogComponent::kSharding

#include "merizo/platform/basic.h"

#include "merizo/db/s/sharding_config_optime_gossip.h"

#include "merizo/base/status.h"
#include "merizo/db/auth/authorization_session.h"
#include "merizo/db/repl/replication_coordinator.h"
#include "merizo/db/s/sharding_state.h"
#include "merizo/db/server_options.h"
#include "merizo/rpc/metadata/config_server_metadata.h"
#include "merizo/s/grid.h"

namespace merizo {
namespace rpc {

void ShardingEgressMetadataHookForMerizod::_saveGLEStats(const BSONObj& metadata,
                                                        StringData hostString) {}

repl::OpTime ShardingEgressMetadataHookForMerizod::_getConfigServerOpTime() {
    if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
        return repl::ReplicationCoordinator::get(_serviceContext)
            ->getCurrentCommittedSnapshotOpTime();
    }

    invariant(serverGlobalParams.clusterRole == ClusterRole::ShardServer);
    return Grid::get(_serviceContext)->configOpTime();
}

Status ShardingEgressMetadataHookForMerizod::_advanceConfigOptimeFromShard(
    ShardId shardId, const BSONObj& metadataObj) {
    if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer) {
        return Status::OK();
    }

    return ShardingEgressMetadataHook::_advanceConfigOptimeFromShard(shardId, metadataObj);
}

void advanceConfigOptimeFromRequestMetadata(OperationContext* opCtx) {
    auto const shardingState = ShardingState::get(opCtx);

    if (!shardingState->enabled()) {
        // Nothing to do if sharding state has not been initialized
        return;
    }

    boost::optional<repl::OpTime> opTime = rpc::ConfigServerMetadata::get(opCtx).getOpTime();
    if (!opTime)
        return;

    uassert(ErrorCodes::Unauthorized,
            "Unauthorized to update config opTime",
            AuthorizationSession::get(opCtx->getClient())
                ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                   ActionType::internal));

    Grid::get(opCtx)->advanceConfigOpTime(*opTime);
}

}  // namespace rpc
}  // namespace merizo
