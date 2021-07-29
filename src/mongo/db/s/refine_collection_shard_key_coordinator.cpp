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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/db/s/refine_collection_shard_key_coordinator.h"

#include "mongo/db/commands.h"
#include "mongo/db/s/dist_lock_manager.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/refine_collection_shard_key_gen.h"

namespace mongo {

RefineCollectionShardKeyCoordinator::RefineCollectionShardKeyCoordinator(
    OperationContext* opCtx, const NamespaceString& nss, const KeyPattern newShardKey)
    : ShardingDDLCoordinator_NORESILIENT(opCtx, nss),
      _serviceContext(opCtx->getServiceContext()),
      _newShardKey(std::move(newShardKey)){};

SemiFuture<void> RefineCollectionShardKeyCoordinator::runImpl(
    std::shared_ptr<executor::TaskExecutor> executor) {
    return ExecutorFuture<void>(executor, Status::OK())
        .then([this, anchor = shared_from_this()]() {
            ThreadClient tc{"RefineCollectionShardKeyCoordinator", _serviceContext};
            {
                stdx::lock_guard<Client> lk(*tc.get());
                tc->setSystemOperationKillableByStepdown(lk);
            }
            auto opCtxHolder = tc->makeOperationContext();
            auto* opCtx = opCtxHolder.get();
            _forwardableOpMetadata.setOn(opCtx);

            auto distLockManager = DistLockManager::get(opCtx->getServiceContext());
            const auto dbDistLock =
                uassertStatusOK(distLockManager->lock(opCtx,
                                                      _nss.db(),
                                                      "RefineCollectionShardKey",
                                                      DistLockManager::kDefaultLockTimeout));
            const auto collDistLock =
                uassertStatusOK(distLockManager->lock(opCtx,
                                                      _nss.ns(),
                                                      "RefineCollectionShardKey",
                                                      DistLockManager::kDefaultLockTimeout));

            const auto cm = uassertStatusOK(
                Grid::get(opCtx)->catalogCache()->getShardedCollectionRoutingInfoWithRefresh(opCtx,
                                                                                             _nss));
            ConfigsvrRefineCollectionShardKey configsvrRefineCollShardKey(
                _nss, _newShardKey.toBSON(), cm.getVersion().epoch());
            configsvrRefineCollShardKey.setDbName(_nss.db().toString());
            // TODO SERVER-54810 don't set `setIsFromPrimaryShard` once 5.0 becomes last-LTS
            configsvrRefineCollShardKey.setIsFromPrimaryShard(true);

            auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
            auto cmdResponse = uassertStatusOK(configShard->runCommandWithFixedRetryAttempts(
                opCtx,
                ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                "admin",
                CommandHelpers::appendMajorityWriteConcern(configsvrRefineCollShardKey.toBSON({}),
                                                           opCtx->getWriteConcern()),
                Shard::RetryPolicy::kIdempotent));

            uassertStatusOK(cmdResponse.commandStatus);
            uassertStatusOK(cmdResponse.writeConcernStatus);
        })
        .onError([this, anchor = shared_from_this()](const Status& status) {
            LOGV2_ERROR(5277700,
                        "Error running refine collection shard key",
                        "namespace"_attr = _nss,
                        "error"_attr = redact(status));
            return status;
        })
        .semi();
}

}  // namespace mongo
