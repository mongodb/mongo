/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/s/reshard_collection_coordinator.h"
#include "mongo/logv2/log.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/reshard_collection_gen.h"


namespace mongo {

ReshardCollectionCoordinator::ReshardCollectionCoordinator(
    OperationContext* opCtx, const ShardsvrReshardCollection& reshardCollectionParams)
    : ShardingDDLCoordinator_NORESILIENT(opCtx, reshardCollectionParams.getCommandParameter()),
      _serviceContext(opCtx->getServiceContext()),
      _requestObj(reshardCollectionParams.serialize({})),
      _request(ShardsvrReshardCollection::parse(IDLParserErrorContext("_shardsvrReshardCollection"),
                                                _requestObj)),
      _nss(_request.getCommandParameter()) {}

SemiFuture<void> ReshardCollectionCoordinator::runImpl(
    std::shared_ptr<executor::TaskExecutor> executor) {
    return ExecutorFuture<void>(executor, Status::OK())
        .then([this, anchor = shared_from_this()]() {
            ThreadClient tc("ReshardCollectionCoordinator", _serviceContext);
            auto opCtxHolder = tc->makeOperationContext();
            auto* opCtx = opCtxHolder.get();
            _forwardableOpMetadata.setOn(opCtx);

            ConfigsvrReshardCollection configsvrReshardCollection(_nss, _request.getKey());
            configsvrReshardCollection.setDbName(_request.getDbName());
            configsvrReshardCollection.setUnique(_request.getUnique());
            configsvrReshardCollection.setCollation(_request.getCollation());
            configsvrReshardCollection.set_presetReshardedChunks(
                _request.get_presetReshardedChunks());
            configsvrReshardCollection.setZones(_request.getZones());
            configsvrReshardCollection.setNumInitialChunks(_request.getNumInitialChunks());

            auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
            const auto cmdResponse = uassertStatusOK(configShard->runCommandWithFixedRetryAttempts(
                opCtx,
                ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                "admin",
                CommandHelpers::appendMajorityWriteConcern(configsvrReshardCollection.toBSON({}),
                                                           opCtx->getWriteConcern()),
                Shard::RetryPolicy::kIdempotent));
            uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(std::move(cmdResponse)));
        })
        .onError([this, anchor = shared_from_this()](const Status& status) {
            LOGV2_ERROR(5276001,
                        "Error running reshard collection",
                        "namespace"_attr = _nss,
                        "error"_attr = redact(status));
            return status;
        })
        .semi();
}

}  // namespace mongo
