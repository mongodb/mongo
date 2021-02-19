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

#include "mongo/db/s/rename_collection_coordinator.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/rename_collection.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/dist_lock_manager.h"
#include "mongo/db/s/shard_metadata_util.h"
#include "mongo/db/s/sharding_ddl_util.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/logv2/log.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"
#include "mongo/s/sharded_collections_ddl_parameters_gen.h"

namespace mongo {
namespace {

void sendCommandToParticipants(OperationContext* opCtx,
                               StringData db,
                               StringData cmdName,
                               const BSONObj& cmd) {
    const auto selfShardId = ShardingState::get(opCtx)->shardId();
    auto shardRegistry = Grid::get(opCtx)->shardRegistry();
    const auto allShardIds = shardRegistry->getAllShardIds(opCtx);

    for (const auto& shardId : allShardIds) {
        if (shardId == selfShardId) {
            continue;
        }

        auto shard = uassertStatusOK(shardRegistry->getShard(opCtx, shardId));
        const auto cmdResponse = uassertStatusOK(shard->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting(ReadPreference::PrimaryOnly),
            db.toString(),
            CommandHelpers::appendMajorityWriteConcern(cmd),
            Shard::RetryPolicy::kNoRetry));
        uassertStatusOKWithContext(Shard::CommandResponse::getEffectiveStatus(cmdResponse),
                                   str::stream()
                                       << "Error processing " << cmdName << " on shard" << shardId);
    }
}

void sendDropCollToParticipants(OperationContext* opCtx,
                                const NamespaceString& nss,
                                const std::vector<ShardId>& participants) {
    auto* const shardRegistry = Grid::get(opCtx)->shardRegistry();

    const ShardsvrDropCollectionParticipant dropCollectionParticipant(nss);

    for (const auto& shardId : participants) {
        const auto& shard = uassertStatusOK(shardRegistry->getShard(opCtx, shardId));

        const auto swDropResult = shard->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            nss.db().toString(),
            CommandHelpers::appendMajorityWriteConcern(dropCollectionParticipant.toBSON({})),
            Shard::RetryPolicy::kIdempotent);

        uassertStatusOKWithContext(
            Shard::CommandResponse::getEffectiveStatus(std::move(swDropResult)),
            str::stream() << "Error dropping collection " << nss.toString()
                          << " on participant shard " << shardId << " during rename");
    }
}

bool isSharded(OperationContext* opCtx, const NamespaceString& nss) {
    try {
        Grid::get(opCtx)->catalogClient()->getCollection(opCtx, nss);
        return true;
    } catch (ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        // The collection is unsharded or doesn't exist
        return false;
    }
}

}  // namespace

RenameCollectionCoordinator::RenameCollectionCoordinator(OperationContext* opCtx,
                                                         const NamespaceString& _nss,
                                                         const NamespaceString& toNss,
                                                         bool dropTarget,
                                                         bool stayTemp)
    : ShardingDDLCoordinator_NORESILIENT(opCtx, _nss),
      _serviceContext(opCtx->getServiceContext()),
      _toNss(toNss),
      _dropTarget(dropTarget),
      _stayTemp(stayTemp){};

void RenameCollectionCoordinator::_renameUnshardedCollection(OperationContext* opCtx) {
    const auto fromDB =
        uassertStatusOK(Grid::get(opCtx)->catalogCache()->getDatabaseWithRefresh(opCtx, _nss.db()));

    const auto toDB = uassertStatusOK(
        Grid::get(opCtx)->catalogCache()->getDatabaseWithRefresh(opCtx, _toNss.db()));

    uassert(5448802,
            "Source and destination collections must be on same shard",
            fromDB.primaryId() == toDB.primaryId());

    if (_dropTarget) {
        const std::vector<mongo::ShardId> participants = [&]() {
            const auto catalogClient = Grid::get(opCtx)->catalogClient();
            try {
                auto coll = catalogClient->getCollection(opCtx, _toNss);
                sharding_ddl_util::removeCollMetadataFromConfig(opCtx, coll);
                return Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
            } catch (ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
                // The collection is not sharded or doesn't exist
                return std::vector<mongo::ShardId>{ShardingState::get(opCtx)->shardId()};
            }
        }();
    }

    validateAndRunRenameCollection(opCtx, _nss, _toNss, {_dropTarget, _stayTemp});
}

void RenameCollectionCoordinator::_renameShardedCollection(OperationContext* opCtx) {
    uassert(ErrorCodes::CommandFailed,
            "Source and destination collections must be on the same database.",
            _nss.db() == _toNss.db());

    // Rename the collection locally and clear the cache
    validateAndRunRenameCollection(opCtx, _nss, _toNss, {_dropTarget, _stayTemp});
    shardmetadatautil::dropChunksAndDeleteCollectionsEntry(opCtx, _nss).ignore();

    // Rename the collection locally on all other shards
    ShardsvrRenameCollectionParticipant renameCollParticipantRequest(_nss);
    renameCollParticipantRequest.setDbName(_nss.db());
    renameCollParticipantRequest.setDropTarget(_dropTarget);
    renameCollParticipantRequest.setStayTemp(_stayTemp);
    renameCollParticipantRequest.setTo(_toNss);
    sendCommandToParticipants(opCtx,
                              _nss.db(),
                              ShardsvrRenameCollectionParticipant::kCommandName,
                              renameCollParticipantRequest.toBSON({}));

    // Rename CSRS metadata
    sharding_ddl_util::shardedRenameMetadata(opCtx, _nss, _toNss);

    // Unblock participants for r/w on source and destination collections
    ShardsvrRenameCollectionUnblockParticipant unblockParticipantRequest(_nss);
    unblockParticipantRequest.setDbName(_nss.db());
    unblockParticipantRequest.setTo(_toNss);
    sendCommandToParticipants(opCtx,
                              _nss.db(),
                              ShardsvrRenameCollectionUnblockParticipant::kCommandName,
                              unblockParticipantRequest.toBSON({}));
}

SemiFuture<void> RenameCollectionCoordinator::runImpl(
    std::shared_ptr<executor::TaskExecutor> executor) {
    return ExecutorFuture<void>(executor, Status::OK())
        .then([this, anchor = shared_from_this()]() {
            ThreadClient tc{"RenameCollectionCoordinator", _serviceContext};
            auto opCtxHolder = tc->makeOperationContext();
            auto* opCtx = opCtxHolder.get();
            _forwardableOpMetadata.setOn(opCtx);

            auto distLockManager = DistLockManager::get(opCtx->getServiceContext());
            const auto dbDistLock = uassertStatusOK(distLockManager->lock(
                opCtx, _nss.db(), "RenameCollection", DistLockManager::kDefaultLockTimeout));
            const auto fromCollDistLock = uassertStatusOK(distLockManager->lock(
                opCtx, _nss.ns(), "RenameCollection", DistLockManager::kDefaultLockTimeout));
            const auto toCollDistLock = uassertStatusOK(distLockManager->lock(
                opCtx, _toNss.ns(), "RenameCollection", DistLockManager::kDefaultLockTimeout));

            // Make sure that the source collection exists
            const auto sourceIsSharded = isSharded(opCtx, _nss);
            if (!sourceIsSharded) {
                const auto sourceCollPtr =
                    CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, _nss);
                uassert(ErrorCodes::CommandFailed,
                        str::stream() << "Collection " << _nss << " doesn't exist.",
                        sourceCollPtr);
            }

            sharding_ddl_util::checkShardedRenamePreconditions(opCtx, _toNss, _dropTarget);

            // Acquire source/target critical sections
            sharding_ddl_util::acquireCriticalSection(opCtx, _nss);
            sharding_ddl_util::acquireCriticalSection(opCtx, _toNss);

            {
                // Release critical sections both in case of rename success/failure
                ON_BLOCK_EXIT([&] {
                    sharding_ddl_util::releaseCriticalSection(opCtx, _nss);
                    sharding_ddl_util::releaseCriticalSection(opCtx, _toNss);
                });

                if (sourceIsSharded) {
                    _renameShardedCollection(opCtx);
                } else {
                    _renameUnshardedCollection(opCtx);
                }
            }

            const auto catalog = Grid::get(opCtx)->catalogCache();
            const auto cm =
                uassertStatusOK(catalog->getCollectionRoutingInfoWithRefresh(opCtx, _toNss));
            const auto version = cm.isSharded() ? cm.getVersion() : ChunkVersion::UNSHARDED();
            _response.emplaceValue(RenameCollectionResponse(version));
        })
        .onError([this, anchor = shared_from_this()](const Status& status) {
            LOGV2_ERROR(5438700,
                        "Error running rename collection",
                        "namespace"_attr = _nss,
                        "error"_attr = redact(status));
            _response.setError(status);
            return status;
        })
        .semi();
}

}  // namespace mongo