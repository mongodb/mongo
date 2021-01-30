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

#include "mongo/db/s/drop_database_coordinator.h"

#include "mongo/db/api_parameters.h"
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
namespace {

void sendCommandToAllShards(OperationContext* opCtx,
                            StringData dbName,
                            StringData cmdName,
                            BSONObj cmd) {
    auto* const shardRegistry = Grid::get(opCtx)->shardRegistry();
    const auto participants = shardRegistry->getAllShardIds(opCtx);

    for (const auto& shardId : participants) {
        const auto& shard = uassertStatusOK(shardRegistry->getShard(opCtx, shardId));

        const auto swDropResult = shard->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            dbName.toString(),
            CommandHelpers::appendMajorityWriteConcern(cmd),
            Shard::RetryPolicy::kIdempotent);

        uassertStatusOKWithContext(
            Shard::CommandResponse::getEffectiveStatus(std::move(swDropResult)),
            str::stream() << "Error processing " << cmdName << " on shard " << shardId);
    }
}

void removeDatabaseMetadataFromConfig(OperationContext* opCtx, StringData dbName) {
    IgnoreAPIParametersBlock ignoreApiParametersBlock(opCtx);
    const auto catalogClient = Grid::get(opCtx)->catalogClient();

    ON_BLOCK_EXIT([&, dbName = dbName.toString()] {
        Grid::get(opCtx)->catalogCache()->purgeDatabase(dbName);
    });

    // Remove the database entry from the metadata.
    const Status status =
        catalogClient->removeConfigDocuments(opCtx,
                                             DatabaseType::ConfigNS,
                                             BSON(DatabaseType::name(dbName.toString())),
                                             ShardingCatalogClient::kMajorityWriteConcern);
    uassertStatusOKWithContext(status,
                               str::stream()
                                   << "Could not remove database metadata from config server for '"
                                   << dbName << "'.");
}

}  // namespace

DropDatabaseCoordinator::DropDatabaseCoordinator(OperationContext* opCtx, StringData dbName)
    : ShardingDDLCoordinator(opCtx, {dbName, ""}), _serviceContext(opCtx->getServiceContext()) {}

SemiFuture<void> DropDatabaseCoordinator::runImpl(
    std::shared_ptr<executor::TaskExecutor> executor) {
    return ExecutorFuture<void>(executor, Status::OK())
        .then([this, anchor = shared_from_this()]() {
            ThreadClient tc{"DropDatabaseCoordinator", _serviceContext};
            auto opCtxHolder = tc->makeOperationContext();
            auto* opCtx = opCtxHolder.get();
            _forwardableOpMetadata.setOn(opCtx);

            const auto dbName = _nss.db();
            auto distLockManager = DistLockManager::get(_serviceContext);
            const auto dbDistLock = uassertStatusOK(distLockManager->lock(
                opCtx, dbName, "DropDatabase", DistLockManager::kDefaultLockTimeout));

            // Drop all collections under this DB
            auto const catalogClient = Grid::get(opCtx)->catalogClient();
            const auto allCollectionsForDb = catalogClient->getCollections(
                opCtx, dbName, repl::ReadConcernLevel::kMajorityReadConcern);

            for (const auto& coll : allCollectionsForDb) {
                if (coll.getDropped()) {
                    continue;
                }

                const auto nss = coll.getNss();
                const auto collectionUUID = coll.getTimestamp().has_value()
                    ? boost::optional<UUID>(coll.getUuid())
                    : boost::none;

                // TODO SERVER-53905 to support failovers here we need to store the
                // current namespace of this loop before to delete it from config server
                // so that on step-up we will remmeber to resume the drop collection for that
                // namespace.
                sharding_ddl_util::removeCollMetadataFromConfig(opCtx, nss, collectionUUID);
                const auto dropCollParticipantCmd = ShardsvrDropCollectionParticipant(nss);
                sendCommandToAllShards(opCtx,
                                       dbName,
                                       ShardsvrDropCollectionParticipant::kCommandName,
                                       dropCollParticipantCmd.toBSON({}));
            }

            // Drop the DB itself.
            // The DistLockManager will prevent to re-create the database before each shard
            // have actually dropped it locally.
            removeDatabaseMetadataFromConfig(opCtx, dbName);
            auto dropDatabaseParticipantCmd = ShardsvrDropDatabaseParticipant();
            dropDatabaseParticipantCmd.setDbName(dbName);
            sendCommandToAllShards(opCtx,
                                   dbName,
                                   ShardsvrDropDatabaseParticipant::kCommandName,
                                   dropDatabaseParticipantCmd.toBSON({}));
        })
        .onError([this, anchor = shared_from_this()](const Status& status) {
            LOGV2_ERROR(5281131,
                        "Error running drop database",
                        "database"_attr = _nss.db(),
                        "error"_attr = redact(status));
            return status;
        })
        .semi();
}

}  // namespace mongo
