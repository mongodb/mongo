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

#include "mongo/db/s/drop_collection_legacy.h"

#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/dist_lock_manager.h"
#include "mongo/db/s/sharding_logging.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/set_shard_version_request.h"

namespace mongo {
namespace {

static constexpr int kMaxNumStaleShardVersionRetries = 10;

void sendDropCollectionToAllShards(OperationContext* opCtx,
                                   const NamespaceString& nss,
                                   const std::vector<ShardType>& allShards) {
    const auto dropCommandBSON = [opCtx, &nss] {
        BSONObjBuilder builder;
        builder.append("drop", nss.coll());

        if (!opCtx->getWriteConcern().usedDefault) {
            builder.append(WriteConcernOptions::kWriteConcernField,
                           opCtx->getWriteConcern().toBSON());
        }

        ChunkVersion::IGNORED().appendToCommand(&builder);
        return builder.obj();
    }();

    auto* const shardRegistry = Grid::get(opCtx)->shardRegistry();

    for (const auto& shardEntry : allShards) {
        bool keepTrying;
        size_t numStaleShardVersionAttempts = 0;
        do {
            const auto& shard =
                uassertStatusOK(shardRegistry->getShard(opCtx, shardEntry.getName()));

            auto swDropResult = shard->runCommandWithFixedRetryAttempts(
                opCtx,
                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                nss.db().toString(),
                dropCommandBSON,
                Shard::RetryPolicy::kIdempotent);

            const std::string dropCollectionErrMsg = str::stream()
                << "Error dropping collection on shard " << shardEntry.getName();

            auto dropResult = uassertStatusOKWithContext(swDropResult, dropCollectionErrMsg);
            uassertStatusOKWithContext(dropResult.writeConcernStatus, dropCollectionErrMsg);

            auto dropCommandStatus = std::move(dropResult.commandStatus);

            if (dropCommandStatus.code() == ErrorCodes::NamespaceNotFound) {
                // The dropCollection command on the shard is not idempotent, and can return
                // NamespaceNotFound. We can ignore NamespaceNotFound since we have already asserted
                // that there is no writeConcern error.
                keepTrying = false;
            } else if (ErrorCodes::isStaleShardVersionError(dropCommandStatus.code())) {
                numStaleShardVersionAttempts++;
                if (numStaleShardVersionAttempts == kMaxNumStaleShardVersionRetries) {
                    uassertStatusOKWithContext(dropCommandStatus,
                                               str::stream() << dropCollectionErrMsg
                                                             << " due to exceeded retry attempts");
                }
                // No need to refresh cache, the command was sent with ChunkVersion::IGNORED and the
                // shard is allowed to throw, which means that the drop will serialize behind a
                // refresh.
                keepTrying = true;
            } else {
                uassertStatusOKWithContext(dropCommandStatus, dropCollectionErrMsg);
                keepTrying = false;
            }
        } while (keepTrying);
    }
}

void sendSSVToAllShards(OperationContext* opCtx,
                        const NamespaceString& nss,
                        const std::vector<ShardType>& allShards) {
    auto* const shardRegistry = Grid::get(opCtx)->shardRegistry();

    IgnoreAPIParametersBlock ignoreApiParametersBlock(opCtx);
    for (const auto& shardEntry : allShards) {
        const auto& shard = uassertStatusOK(shardRegistry->getShard(opCtx, shardEntry.getName()));

        SetShardVersionRequest ssv(
            nss, ChunkVersion::UNSHARDED(), true /* isAuthoritative */, true /* forceRefresh */);

        auto ssvResult = shard->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            "admin",
            ssv.toBSON(),
            Shard::RetryPolicy::kIdempotent);

        uassertStatusOK(ssvResult.getStatus());
        uassertStatusOK(ssvResult.getValue().commandStatus);
    }
}

void removeChunksForDroppedCollection(OperationContext* opCtx,
                                      const NamespaceStringOrUUID& nssOrUUID) {
    IgnoreAPIParametersBlock ignoreApiParametersBlock(opCtx);
    const auto catalogClient = Grid::get(opCtx)->catalogClient();

    // Remove chunk data
    const auto chunksQuery = [&]() {
        if (nssOrUUID.uuid()) {
            return BSON(ChunkType::collectionUUID << *nssOrUUID.uuid());
        } else {
            return BSON(ChunkType::ns(nssOrUUID.nss()->ns()));
        }
    }();
    uassertStatusOK(catalogClient->removeConfigDocuments(
        opCtx, ChunkType::ConfigNS, chunksQuery, ShardingCatalogClient::kMajorityWriteConcern));
}

void removeTagsForDroppedCollection(OperationContext* opCtx, const NamespaceString& nss) {
    IgnoreAPIParametersBlock ignoreApiParametersBlock(opCtx);
    const auto catalogClient = Grid::get(opCtx)->catalogClient();

    // Remove tag data
    uassertStatusOK(
        catalogClient->removeConfigDocuments(opCtx,
                                             TagsType::ConfigNS,
                                             BSON(TagsType::ns(nss.ns())),
                                             ShardingCatalogClient::kMajorityWriteConcern));
}


/**
 * Ensures that a namespace that has received a dropCollection, but no longer has an entry in
 * config.collections, has cleared all relevant metadata entries for the corresponding collection.
 * As part of this, sends dropCollection and setShardVersion to all shards -- in case shards didn't
 * receive these commands as part of the original dropCollection.
 *
 * This function does not guarantee that all shards will eventually receive setShardVersion, unless
 * the client infinitely retries until hearing back success. This function does, however, increase
 * the likelihood of shards having received setShardVersion.
 */
void ensureDropCollectionCompleted(OperationContext* opCtx, const NamespaceString& nss) {
    const auto catalogClient = Grid::get(opCtx)->catalogClient();
    auto allShards = uassertStatusOK(catalogClient->getAllShards(
                                         opCtx, repl::ReadConcernLevel::kMajorityReadConcern))
                         .value;

    LOGV2_DEBUG(21929,
                1,
                "Ensuring config entries for {namespace} from previous dropCollection are cleared",
                "Ensuring config entries from previous dropCollection are cleared",
                "namespace"_attr = nss.ns());

    sendDropCollectionToAllShards(opCtx, nss, allShards);

    IgnoreAPIParametersBlock ignoreApiParametersBlock(opCtx);
    removeTagsForDroppedCollection(opCtx, nss);
    sendSSVToAllShards(opCtx, nss, allShards);
}

}  // namespace

void dropCollectionLegacy(OperationContext* opCtx, const NamespaceString& nss) {
    auto dbDistLock = uassertStatusOK(DistLockManager::get(opCtx)->lock(
        opCtx, nss.db(), "dropCollection", DistLockManager::kDefaultLockTimeout));
    auto collDistLock = uassertStatusOK(DistLockManager::get(opCtx)->lock(
        opCtx, nss.ns(), "dropCollection", DistLockManager::kDefaultLockTimeout));

    ON_BLOCK_EXIT([opCtx, nss] {
        Grid::get(opCtx)->catalogCache()->invalidateCollectionEntry_LINEARIZABLE(nss);
    });

    auto const catalogClient = Grid::get(opCtx)->catalogClient();

    CollectionType collection;
    try {
        catalogClient->getCollection(opCtx, nss, repl::ReadConcernLevel::kMajorityReadConcern);
        dropCollectionNoDistLock(opCtx, nss);
    } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        // If the DB isn't in the sharding catalog either, consider the drop a success.
        DatabaseType dbt;
        try {
            dbt = catalogClient->getDatabase(
                opCtx, nss.db().toString(), repl::ReadConcernLevel::kMajorityReadConcern);
        } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
            return;
        }

        // If we found the DB but not the collection, and the primary shard for the database is the
        // config server, run the drop only against the config server unless the collection is
        // config.system.sessions, since no other collections whose primary shard is the config
        // server can have been sharded.
        if (dbt.getPrimary() == ShardId::kConfigServerId &&
            nss != NamespaceString::kLogicalSessionsNamespace) {
            auto cmdDropResult =
                uassertStatusOK(Grid::get(opCtx)
                                    ->shardRegistry()
                                    ->getConfigShard()
                                    ->runCommandWithFixedRetryAttempts(
                                        opCtx,
                                        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                        nss.db().toString(),
                                        BSON("drop" << nss.coll()),
                                        Shard::RetryPolicy::kIdempotent));

            // If the collection doesn't exist, consider the drop a success.
            if (cmdDropResult.commandStatus == ErrorCodes::NamespaceNotFound) {
                return;
            }
            uassertStatusOK(cmdDropResult.commandStatus);
            return;
        }

        ensureDropCollectionCompleted(opCtx, nss);
    }
}

void dropCollectionNoDistLock(OperationContext* opCtx, const NamespaceString& nss) {
    uassertStatusOK(ShardingLogging::get(opCtx)->logChangeChecked(
        opCtx,
        "dropCollection.start",
        nss.ns(),
        BSONObj(),
        ShardingCatalogClient::kMajorityWriteConcern));

    LOGV2_DEBUG(21924,
                1,
                "dropCollection {namespace} started",
                "dropCollection started",
                "namespace"_attr = nss.ns());

    const auto catalogClient = Grid::get(opCtx)->catalogClient();
    auto allShards = uassertStatusOK(catalogClient->getAllShards(
                                         opCtx, repl::ReadConcernLevel::kMajorityReadConcern))
                         .value;

    sendDropCollectionToAllShards(opCtx, nss, allShards);

    LOGV2_DEBUG(21925,
                1,
                "dropCollection {namespace} shard data deleted",
                "dropCollection shard data deleted",
                "namespace"_attr = nss.ns());

    try {
        auto collType = catalogClient->getCollection(opCtx, nss);
        const auto nssOrUUID = [&]() {
            if (collType.getTimestamp()) {
                return NamespaceStringOrUUID(std::string(), collType.getUuid());
            } else {
                return NamespaceStringOrUUID(collType.getNss());
            }
        }();
        removeChunksForDroppedCollection(opCtx, nssOrUUID);
        removeTagsForDroppedCollection(opCtx, nss);

        LOGV2_DEBUG(21926,
                    1,
                    "dropCollection {namespace} chunk and tag data deleted",
                    "dropCollection chunk and tag data deleted",
                    "namespace"_attr = nss.ns());

        uassertStatusOK(
            catalogClient->removeConfigDocuments(opCtx,
                                                 CollectionType::ConfigNS,
                                                 BSON(CollectionType::kNssFieldName << nss.ns()),
                                                 ShardingCatalogClient::kMajorityWriteConcern));
        LOGV2_DEBUG(21927,
                    1,
                    "dropCollection {namespace} collection entry deleted",
                    "dropCollection collection entry deleted",
                    "namespace"_attr = nss.ns());
    } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        LOGV2(5310500,
              "dropCollection {namespace} collection entry not found",
              "dropCollection {namespace} collection entry not found",
              "namespace"_attr = nss.ns());
    }

    sendSSVToAllShards(opCtx, nss, allShards);

    LOGV2_DEBUG(21928,
                1,
                "dropCollection {namespace} completed",
                "dropCollection completed",
                "namespace"_attr = nss.ns());

    ShardingLogging::get(opCtx)->logChange(
        opCtx, "dropCollection", nss.ns(), BSONObj(), ShardingCatalogClient::kMajorityWriteConcern);
}

}  // namespace mongo
