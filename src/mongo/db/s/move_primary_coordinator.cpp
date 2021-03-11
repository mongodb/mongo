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

#include "mongo/platform/basic.h"

#include "mongo/db/s/move_primary_coordinator.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/s/active_move_primaries_registry.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/dist_lock_manager.h"
#include "mongo/db/s/move_primary_source_manager.h"
#include "mongo/db/s/shard_metadata_util.h"
#include "mongo/db/s/sharding_ddl_util.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/logv2/log.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"
#include "mongo/s/shard_id.h"

namespace mongo {
namespace {
/**
 * If the specified status is not OK logs a warning and throws a DBException corresponding to the
 * specified status.
 */
void uassertStatusOKWithWarning(const Status& status) {
    if (!status.isOK()) {
        LOGV2_WARNING(5275800,
                      "movePrimary failed: {error}",
                      "movePrimary failed",
                      "error"_attr = redact(status));
        uassertStatusOK(status);
    }
}

}  // namespace

MovePrimaryCoordinator::MovePrimaryCoordinator(OperationContext* opCtx,
                                               const NamespaceString& dbNss,
                                               const StringData& toShard)
    : ShardingDDLCoordinator_NORESILIENT(opCtx, dbNss),
      _serviceContext(opCtx->getServiceContext()),
      _toShardId(ShardId(toShard.toString())) {}

SemiFuture<void> MovePrimaryCoordinator::runImpl(std::shared_ptr<executor::TaskExecutor> executor) {
    return ExecutorFuture<void>(executor, Status::OK())
        .then([this, anchor = shared_from_this()]() {
            ThreadClient tc{"MovePrimaryCoordinator", _serviceContext};
            auto opCtxHolder = tc->makeOperationContext();
            auto* opCtx = opCtxHolder.get();
            _forwardableOpMetadata.setOn(opCtx);

            auto distLockManager = DistLockManager::get(opCtx->getServiceContext());
            const auto dbDistLock = uassertStatusOK(distLockManager->lock(
                opCtx, _nss.db(), "MovePrimary", DistLockManager::kDefaultLockTimeout));

            auto const shardRegistry = Grid::get(opCtx)->shardRegistry();
            // Make sure we're as up-to-date as possible with shard information. This catches the
            // case where we might have changed a shard's host by removing/adding a shard with the
            // same name.
            shardRegistry->reload(opCtx);

            const auto& dbName = _nss.db();

            const auto& toShard = [&]() {
                auto toShardStatus = shardRegistry->getShard(opCtx, _toShardId);
                if (!toShardStatus.isOK()) {
                    LOGV2(5275802,
                          "Could not move database {db} to shard {shardId}: {error}",
                          "Could not move database to shard",
                          "db"_attr = dbName,
                          "shardId"_attr = _toShardId,
                          "error"_attr = toShardStatus.getStatus());
                    uassertStatusOKWithContext(toShardStatus.getStatus(),
                                               str::stream()
                                                   << "Could not move database '" << dbName
                                                   << "' to shard '" << _toShardId << "'");
                }
                return toShardStatus.getValue();
            }();

            const auto& selfShardId = ShardingState::get(opCtx)->shardId();
            if (selfShardId == toShard->getId()) {
                LOGV2(5275803,
                      "Database already on the requested primary shard",
                      "db"_attr = dbName,
                      "shardId"_attr = _toShardId);
                // The database primary is already the `to` shard
                return;
            }

            ShardMovePrimary movePrimaryRequest(_nss, _toShardId.toString());

            auto scopedMovePrimary = uassertStatusOK(
                ActiveMovePrimariesRegistry::get(opCtx).registerMovePrimary(movePrimaryRequest));
            // Check if there is an existing movePrimary running and if so, join it
            if (scopedMovePrimary.mustExecute()) {
                auto status = [&] {
                    try {
                        auto primaryId = selfShardId;
                        auto toId = toShard->getId();
                        MovePrimarySourceManager movePrimarySourceManager(
                            opCtx, movePrimaryRequest, dbName, primaryId, toId);
                        uassertStatusOKWithWarning(movePrimarySourceManager.clone(opCtx));
                        uassertStatusOKWithWarning(
                            movePrimarySourceManager.enterCriticalSection(opCtx));
                        uassertStatusOKWithWarning(movePrimarySourceManager.commitOnConfig(opCtx));
                        uassertStatusOKWithWarning(movePrimarySourceManager.cleanStaleData(opCtx));
                        return Status::OK();
                    } catch (const DBException& ex) {
                        return ex.toStatus();
                    }
                }();
                scopedMovePrimary.signalComplete(status);
                uassertStatusOK(status);
            } else {
                uassertStatusOK(scopedMovePrimary.waitForCompletion(opCtx));
            }
        })
        .onError([this, anchor = shared_from_this()](const Status& status) {
            LOGV2_ERROR(5275804,
                        "Error running move primary",
                        "namespace"_attr = _nss,
                        "error"_attr = redact(status));
            return status;
        })
        .semi();
}

}  // namespace mongo
