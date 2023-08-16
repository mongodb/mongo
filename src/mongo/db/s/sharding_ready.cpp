/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/s/sharding_ready.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/client_strand.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/service_context.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/logv2/log.h"
#include "mongo/s/grid.h"
#include "mongo/util/future.h"


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

const auto shardingReady = ServiceContext::declareDecoration<ShardingReady>();

}  // namespace

ShardingReady* ShardingReady::get(ServiceContext* serviceContext) {
    return &shardingReady(serviceContext);
}

ShardingReady* ShardingReady::get(OperationContext* opCtx) {
    return ShardingReady::get(opCtx->getServiceContext());
}


void ShardingReady::scheduleTransitionToConfigShard(OperationContext* opCtx) {
    auto catalogManager = ShardingCatalogManager::get(opCtx);
    auto getShards = catalogManager->localCatalogClient()->getAllShards(
        opCtx, repl::ReadConcernLevel::kLocalReadConcern);
    uassertStatusOK(getShards);

    // Only transition to config shard if we have no existing data shards.
    if (getShards.getValue().value.empty()) {
        auto executor =
            Grid::get(opCtx->getServiceContext())->getExecutorPool()->getFixedExecutor();
        // TODO SERVER-79109: Expose readiness future via 'isReady' function.
        auto f = ExecutorFuture(executor).then([this, serviceContext = opCtx->getServiceContext()] {
            transitionToConfigShard(serviceContext);
        });
    }
}

// TODO SERVER-80111: Add retry loop to 'transitionFromDedicatedConfigServer' local command.
void ShardingReady::transitionToConfigShard(ServiceContext* serviceContext) {
    BSONObjBuilder builder;
    // TODO SERVER-80110: Allow 'transitionFromDedicatedConfigSever' to succeed with
    // {w: 1} during auto-bootstrap.
    const WriteConcernOptions wc(WriteConcernOptions::kMajority,
                                 WriteConcernOptions::SyncMode::NONE,
                                 WriteConcernOptions::kNoTimeout);
    builder.append("_configsvrTransitionFromDedicatedConfigServer", 1);
    builder.append(WriteConcernOptions::kWriteConcernField, wc.toBSON());

    // Since this function is async, we need to create a new client and operation context to run
    // 'transitionFromDedicatedConfigServer'.
    auto clientStrand = ClientStrand::make(serviceContext->makeClient("ShardingReady"));
    auto clientGuard = clientStrand->bind();
    auto uniqueOpCtx = clientGuard->makeOperationContext();
    DBDirectClient client(uniqueOpCtx.get());
    BSONObj result;
    client.runCommand(DatabaseName::kAdmin, builder.obj(), result);
    uassertStatusOK(getStatusFromWriteCommandReply(result));

    LOGV2(7910800, "Auto-bootstrap to config shard complete.");
}

}  // namespace mongo
