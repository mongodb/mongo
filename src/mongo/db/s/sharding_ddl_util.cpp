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

#include "mongo/platform/basic.h"

#include "mongo/db/s/sharding_ddl_util.h"

#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"
#include "mongo/db/s/sharding_logging.h"
#include "mongo/db/s/sharding_util.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/metadata/impersonated_user_metadata.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/set_allow_migrations_gen.h"
#include "mongo/s/write_ops/batch_write_exec.h"

namespace mongo {

namespace sharding_ddl_util {

namespace {

void updateTags(OperationContext* opCtx,
                const NamespaceString& fromNss,
                const NamespaceString& toNss) {
    const auto query = BSON(TagsType::ns(fromNss.ns()));
    const auto update = BSON("$set" << BSON(TagsType::ns(toNss.ns())));

    BatchedCommandRequest request([&] {
        write_ops::UpdateCommandRequest updateOp(TagsType::ConfigNS);
        updateOp.setUpdates({[&] {
            write_ops::UpdateOpEntry entry;
            entry.setQ(query);
            entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(update));
            entry.setUpsert(false);
            entry.setMulti(true);
            return entry;
        }()});
        return updateOp;
    }());
    request.setWriteConcern(ShardingCatalogClient::kMajorityWriteConcern.toBSON());

    auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    auto response = configShard->runBatchWriteCommand(
        opCtx, Milliseconds::max(), request, Shard::RetryPolicy::kIdempotentOrCursorInvalidated);

    uassertStatusOK(response.toStatus());
}

void deleteChunks(OperationContext* opCtx, const NamespaceStringOrUUID& nssOrUUID) {
    // Remove config.chunks entries
    const auto chunksQuery = [&]() {
        auto optUUID = nssOrUUID.uuid();
        if (optUUID) {
            return BSON(ChunkType::collectionUUID << *optUUID);
        }

        auto optNss = nssOrUUID.nss();
        invariant(optNss);
        return BSON(ChunkType::ns(optNss->ns()));
    }();

    // TODO SERVER-57221 don't use hint if not relevant anymore for delete performances
    auto hint = BSON(ChunkType::collectionUUID() << 1 << ChunkType::min() << 1);

    BatchedCommandRequest request([&] {
        write_ops::DeleteCommandRequest deleteOp(ChunkType::ConfigNS);
        deleteOp.setDeletes({[&] {
            write_ops::DeleteOpEntry entry;
            entry.setQ(chunksQuery);
            entry.setHint(hint);
            entry.setMulti(true);
            return entry;
        }()});
        return deleteOp;
    }());

    request.setWriteConcern(ShardingCatalogClient::kMajorityWriteConcern.toBSON());

    auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    auto response = configShard->runBatchWriteCommand(
        opCtx, Milliseconds::max(), request, Shard::RetryPolicy::kIdempotentOrCursorInvalidated);

    uassertStatusOK(response.toStatus());
}

void deleteCollection(OperationContext* opCtx, const NamespaceString& nss) {
    const auto catalogClient = Grid::get(opCtx)->catalogClient();

    // Remove config.collection entry
    uassertStatusOK(
        catalogClient->removeConfigDocuments(opCtx,
                                             CollectionType::ConfigNS,
                                             BSON(CollectionType::kNssFieldName << nss.ns()),
                                             ShardingCatalogClient::kMajorityWriteConcern));
}

}  // namespace

void linearizeCSRSReads(OperationContext* opCtx) {
    // Take advantage of ShardingLogging to perform a write to the configsvr with majority read
    // concern to guarantee that any read after this method sees any write performed by the previous
    // primary.
    uassertStatusOK(ShardingLogging::get(opCtx)->logChangeChecked(
        opCtx,
        "Linearize CSRS reads",
        NamespaceString::kServerConfigurationNamespace.ns(),
        {},
        ShardingCatalogClient::kMajorityWriteConcern));
}

void sendAuthenticatedCommandToShards(OperationContext* opCtx,
                                      StringData dbName,
                                      const BSONObj& command,
                                      const std::vector<ShardId>& shardIds,
                                      const std::shared_ptr<executor::TaskExecutor>& executor) {
    // The AsyncRequestsSender ignore impersonation metadata so we need to manually attach them to
    // the command
    BSONObjBuilder bob(command);
    rpc::writeAuthDataToImpersonatedUserMetadata(opCtx, &bob);
    auto authenticatedCommand = bob.obj();
    sharding_util::sendCommandToShards(opCtx, dbName, authenticatedCommand, shardIds, executor);
}

void removeTagsMetadataFromConfig(OperationContext* opCtx, const NamespaceString& nss) {
    // Remove config.tags entries
    const auto query = BSON(TagsType::ns(nss.ns()));
    const auto hint = BSON(TagsType::ns() << 1 << TagsType::min() << 1);

    BatchedCommandRequest request([&] {
        write_ops::DeleteCommandRequest deleteOp(TagsType::ConfigNS);
        deleteOp.setDeletes({[&] {
            write_ops::DeleteOpEntry entry;
            entry.setQ(query);
            entry.setMulti(true);
            entry.setHint(hint);
            return entry;
        }()});
        return deleteOp;
    }());

    request.setWriteConcern(ShardingCatalogClient::kMajorityWriteConcern.toBSON());

    auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    auto response = configShard->runBatchWriteCommand(
        opCtx, Milliseconds::max(), request, Shard::RetryPolicy::kIdempotentOrCursorInvalidated);

    uassertStatusOK(response.toStatus());
}

void removeCollMetadataFromConfig(OperationContext* opCtx, const CollectionType& coll) {
    IgnoreAPIParametersBlock ignoreApiParametersBlock(opCtx);
    const auto& nss = coll.getNss();

    ON_BLOCK_EXIT(
        [&] { Grid::get(opCtx)->catalogCache()->invalidateCollectionEntry_LINEARIZABLE(nss); });

    const NamespaceStringOrUUID nssOrUUID = coll.getTimestamp()
        ? NamespaceStringOrUUID(nss.db().toString(), coll.getUuid())
        : NamespaceStringOrUUID(nss);

    deleteCollection(opCtx, nss);

    deleteChunks(opCtx, nssOrUUID);

    removeTagsMetadataFromConfig(opCtx, nss);
}

bool removeCollMetadataFromConfig(OperationContext* opCtx, const NamespaceString& nss) {
    IgnoreAPIParametersBlock ignoreApiParametersBlock(opCtx);
    const auto catalogClient = Grid::get(opCtx)->catalogClient();

    ON_BLOCK_EXIT(
        [&] { Grid::get(opCtx)->catalogCache()->invalidateCollectionEntry_LINEARIZABLE(nss); });

    try {
        auto coll = catalogClient->getCollection(opCtx, nss);
        removeCollMetadataFromConfig(opCtx, coll);
        return true;
    } catch (ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        // The collection is not sharded or doesn't exist, just tags need to be removed
        removeTagsMetadataFromConfig(opCtx, nss);
        return false;
    }
}

void shardedRenameMetadata(OperationContext* opCtx,
                           CollectionType& fromCollType,
                           const NamespaceString& toNss) {
    auto catalogClient = Grid::get(opCtx)->catalogClient();
    auto fromNss = fromCollType.getNss();

    // Delete eventual TO chunk/collection entries referring a dropped collection
    try {
        auto coll = catalogClient->getCollection(opCtx, toNss);

        if (coll.getUuid() == fromCollType.getUuid()) {
            // Metadata rename already happened
            return;
        }

        // Delete TO chunk/collection entries referring a dropped collection
        removeCollMetadataFromConfig(opCtx, toNss);
    } catch (ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        // The TO collection is not sharded or doesn't exist
    }

    // Delete FROM collection entry
    deleteCollection(opCtx, fromNss);

    // Update FROM tags to TO
    updateTags(opCtx, fromNss, toNss);

    // Insert the TO collection entry
    fromCollType.setNss(toNss);
    uassertStatusOK(
        catalogClient->insertConfigDocument(opCtx,
                                            CollectionType::ConfigNS,
                                            fromCollType.toBSON(),
                                            ShardingCatalogClient::kMajorityWriteConcern));
}

void checkShardedRenamePreconditions(OperationContext* opCtx,
                                     const NamespaceString& toNss,
                                     const bool dropTarget) {
    auto catalogClient = Grid::get(opCtx)->catalogClient();
    if (!dropTarget) {
        // Check that the sharded target collection doesn't exist
        try {
            catalogClient->getCollection(opCtx, toNss);
            // If no exception is thrown, the collection exists and is sharded
            uasserted(ErrorCodes::CommandFailed,
                      str::stream() << "Sharded target collection " << toNss.ns()
                                    << " exists but dropTarget is not set");
        } catch (const DBException& ex) {
            auto code = ex.code();
            if (code != ErrorCodes::NamespaceNotFound && code != ErrorCodes::NamespaceNotSharded) {
                throw;
            }
        }

        // Check that the unsharded target collection doesn't exist
        auto collectionCatalog = CollectionCatalog::get(opCtx);
        auto targetColl = collectionCatalog->lookupCollectionByNamespace(opCtx, toNss);
        uassert(ErrorCodes::CommandFailed,
                str::stream() << "Target collection " << toNss.ns()
                              << " exists but dropTarget is not set",
                !targetColl);
    }

    // Check that there are no tags associated to the target collection
    auto tags = uassertStatusOK(catalogClient->getTagsForCollection(opCtx, toNss));
    uassert(ErrorCodes::CommandFailed,
            str::stream() << "Can't rename to target collection " << toNss.ns()
                          << " because it must not have associated tags",
            tags.empty());
}

void checkDbPrimariesOnTheSameShard(OperationContext* opCtx,
                                    const NamespaceString& fromNss,
                                    const NamespaceString& toNss) {
    const auto fromDB =
        uassertStatusOK(Grid::get(opCtx)->catalogCache()->getDatabase(opCtx, fromNss.db()));

    const auto toDB = uassertStatusOK(
        Grid::get(opCtx)->catalogCache()->getDatabaseWithRefresh(opCtx, toNss.db()));

    uassert(ErrorCodes::CommandFailed,
            "Source and destination collections must be on same shard",
            fromDB.primaryId() == toDB.primaryId());
}

boost::optional<CreateCollectionResponse> checkIfCollectionAlreadySharded(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const BSONObj& key,
    const BSONObj& collation,
    bool unique) {
    auto cm = uassertStatusOK(
        Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfoWithRefresh(opCtx, nss));

    if (!cm.isSharded()) {
        return boost::none;
    }

    auto defaultCollator =
        cm.getDefaultCollator() ? cm.getDefaultCollator()->getSpec().toBSON() : BSONObj();

    // If the collection is already sharded, fail if the deduced options in this request do not
    // match the options the collection was originally sharded with.
    uassert(ErrorCodes::AlreadyInitialized,
            str::stream() << "sharding already enabled for collection " << nss,
            SimpleBSONObjComparator::kInstance.evaluate(cm.getShardKeyPattern().toBSON() == key) &&
                SimpleBSONObjComparator::kInstance.evaluate(defaultCollator == collation) &&
                cm.isUnique() == unique);

    CreateCollectionResponse response(cm.getVersion());
    response.setCollectionUUID(cm.getUUID());
    return response;
}

void stopMigrations(OperationContext* opCtx, const NamespaceString& nss) {
    const ConfigsvrSetAllowMigrations configsvrSetAllowMigrationsCmd(nss,
                                                                     false /* allowMigrations */);
    const auto swSetAllowMigrationsResult =
        Grid::get(opCtx)->shardRegistry()->getConfigShard()->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            NamespaceString::kAdminDb.toString(),
            CommandHelpers::appendMajorityWriteConcern(configsvrSetAllowMigrationsCmd.toBSON({})),
            Shard::RetryPolicy::kIdempotent  // Although ConfigsvrSetAllowMigrations is not really
                                             // idempotent (because it will cause the collection
                                             // version to be bumped), it is safe to be retried.
        );

    uassertStatusOKWithContext(
        Shard::CommandResponse::getEffectiveStatus(std::move(swSetAllowMigrationsResult)),
        str::stream() << "Error setting allowMigrations to false for collection "
                      << nss.toString());
}

DropReply dropCollectionLocally(OperationContext* opCtx, const NamespaceString& nss) {
    DropReply result;
    uassertStatusOK(dropCollection(
        opCtx, nss, &result, DropCollectionSystemCollectionMode::kDisallowSystemCollectionDrops));

    {
        // Clear CollectionShardingRuntime entry
        UninterruptibleLockGuard noInterrupt(opCtx->lockState());
        Lock::DBLock dbLock(opCtx, nss.db(), MODE_IX);
        Lock::CollectionLock collLock(opCtx, nss, MODE_IX);
        auto* csr = CollectionShardingRuntime::get(opCtx, nss);
        csr->clearFilteringMetadata(opCtx);
    }

    return result;
}

boost::optional<UUID> getCollectionUUID(OperationContext* opCtx, const NamespaceString& nss) {
    AutoGetCollection autoColl(opCtx, nss, MODE_IS, AutoGetCollectionViewMode::kViewsForbidden);
    return autoColl ? boost::make_optional(autoColl->uuid()) : boost::none;
}
}  // namespace sharding_ddl_util
}  // namespace mongo
