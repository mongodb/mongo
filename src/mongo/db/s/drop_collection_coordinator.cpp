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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/db/s/drop_collection_coordinator.h"

#include "mongo/db/api_parameters.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/s/dist_lock_manager.h"
#include "mongo/db/s/drop_collection_coordinator.h"
#include "mongo/db/s/sharding_ddl_util.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"
#include "mongo/util/assert_util.h"

namespace mongo {

DropCollectionCoordinator::DropCollectionCoordinator(OperationContext* opCtx,
                                                     const NamespaceString& nss)
    : ShardingDDLCoordinator(nss), _serviceContext(opCtx->getServiceContext()) {
    auto authSession = AuthorizationSession::get(opCtx->getClient());
    _users =
        userNameIteratorToContainer<std::vector<UserName>>(authSession->getImpersonatedUserNames());
    _roles =
        roleNameIteratorToContainer<std::vector<RoleName>>(authSession->getImpersonatedRoleNames());
}

void DropCollectionCoordinator::_sendDropCollToParticipants(OperationContext* opCtx) {
    auto* const shardRegistry = Grid::get(opCtx)->shardRegistry();

    const ShardsvrDropCollectionParticipant dropCollectionParticipant(_nss);

    for (const auto& shardId : _participants) {
        const auto& shard = uassertStatusOK(shardRegistry->getShard(opCtx, shardId));

        const auto swDropResult = shard->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            _nss.db().toString(),
            CommandHelpers::appendMajorityWriteConcern(dropCollectionParticipant.toBSON({})),
            Shard::RetryPolicy::kIdempotent);

        uassertStatusOKWithContext(
            Shard::CommandResponse::getEffectiveStatus(std::move(swDropResult)),
            str::stream() << "Error dropping collection " << _nss.toString()
                          << " on participant shard " << shardId);
    }
}

void DropCollectionCoordinator::_stopMigrations(OperationContext* opCtx) {
    // TODO SERVER-53861 this will not stop current ongoing migrations
    uassertStatusOK(Grid::get(opCtx)->catalogClient()->updateConfigDocument(
        opCtx,
        CollectionType::ConfigNS,
        BSON(CollectionType::kNssFieldName << _nss.ns()),
        BSON("$set" << BSON(CollectionType::kAllowMigrationsFieldName << false)),
        false /* upsert */,
        ShardingCatalogClient::kMajorityWriteConcern));
}

SemiFuture<void> DropCollectionCoordinator::runImpl(
    std::shared_ptr<executor::TaskExecutor> executor) {
    return ExecutorFuture<void>(executor, Status::OK())
        .then([this, anchor = shared_from_this()]() {
            ThreadClient tc{"DropCollectionCoordinator", _serviceContext};
            auto opCtxHolder = tc->makeOperationContext();
            auto* opCtx = opCtxHolder.get();

            auto authSession = AuthorizationSession::get(opCtx->getClient());
            authSession->setImpersonatedUserData(_users, _roles);

            auto distLockManager = DistLockManager::get(_serviceContext);
            const auto dbDistLock = uassertStatusOK(distLockManager->lock(
                opCtx, _nss.db(), "DropCollection", DistLockManager::kDefaultLockTimeout));
            const auto collDistLock = uassertStatusOK(distLockManager->lock(
                opCtx, _nss.ns(), "DropCollection", DistLockManager::kDefaultLockTimeout));

            _stopMigrations(opCtx);

            const auto routingInfo = uassertStatusOK(
                Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfoWithRefresh(opCtx, _nss));

            const boost::optional<UUID> collectionUUID = [&]() {
                if (routingInfo.isSharded() && routingInfo.getVersion().getTimestamp()) {
                    invariant(routingInfo.getUUID());
                    return routingInfo.getUUID();
                } else {
                    return boost::optional<UUID>(boost::none);
                }
            }();

            sharding_ddl_util::removeCollMetadataFromConfig(opCtx, _nss, collectionUUID);

            if (routingInfo.isSharded()) {
                _participants = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
            } else {
                _participants = {routingInfo.dbPrimary()};
            }

            _sendDropCollToParticipants(opCtx);
        })
        .onError([this, anchor = shared_from_this()](const Status& status) {
            LOGV2_ERROR(5280901,
                        "Error running drop collection",
                        "namespace"_attr = _nss,
                        "error"_attr = redact(status));
            return status;
        })
        .semi();
}

}  // namespace mongo
