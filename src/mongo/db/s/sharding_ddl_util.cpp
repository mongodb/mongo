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


#include "mongo/db/s/sharding_ddl_util.h"

#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/remove_tags_gen.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"
#include "mongo/db/s/sharding_logging.h"
#include "mongo/db/s/sharding_util.h"
#include "mongo/db/vector_clock.h"
#include "mongo/db/write_block_bypass.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/metadata/impersonated_user_metadata.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/set_allow_migrations_gen.h"
#include "mongo/s/write_ops/batch_write_exec.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace sharding_ddl_util {
namespace {

void updateTags(OperationContext* opCtx,
                const NamespaceString& fromNss,
                const NamespaceString& toNss,
                const WriteConcernOptions& writeConcern) {
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
    request.setWriteConcern(writeConcern.toBSON());

    auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    auto response = configShard->runBatchWriteCommand(
        opCtx, Milliseconds::max(), request, Shard::RetryPolicy::kIdempotentOrCursorInvalidated);

    uassertStatusOK(response.toStatus());
}

void deleteChunks(OperationContext* opCtx,
                  const UUID& collectionUUID,
                  const WriteConcernOptions& writeConcern) {
    // Remove config.chunks entries
    // TODO SERVER-57221 don't use hint if not relevant anymore for delete performances
    auto hint = BSON(ChunkType::collectionUUID() << 1 << ChunkType::min() << 1);

    BatchedCommandRequest request([&] {
        write_ops::DeleteCommandRequest deleteOp(ChunkType::ConfigNS);
        deleteOp.setDeletes({[&] {
            write_ops::DeleteOpEntry entry;
            entry.setQ(BSON(ChunkType::collectionUUID << collectionUUID));
            entry.setHint(hint);
            entry.setMulti(true);
            return entry;
        }()});
        return deleteOp;
    }());

    request.setWriteConcern(writeConcern.toBSON());

    auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    auto response = configShard->runBatchWriteCommand(
        opCtx, Milliseconds::max(), request, Shard::RetryPolicy::kIdempotentOrCursorInvalidated);

    uassertStatusOK(response.toStatus());
}

void deleteCollection(OperationContext* opCtx,
                      const NamespaceString& nss,
                      const UUID& uuid,
                      const WriteConcernOptions& writeConcern) {
    const auto catalogClient = Grid::get(opCtx)->catalogClient();

    // Remove config.collection entry. Query by 'ns' AND 'uuid' so that the remove can be resolved
    // with an IXSCAN (thanks to the index on '_id') and is idempotent (thanks to the 'uuid')
    uassertStatusOK(catalogClient->removeConfigDocuments(
        opCtx,
        CollectionType::ConfigNS,
        BSON(CollectionType::kNssFieldName << nss.ns() << CollectionType::kUuidFieldName << uuid),
        writeConcern));
}

write_ops::UpdateCommandRequest buildNoopWriteRequestCommand() {
    write_ops::UpdateCommandRequest updateOp(NamespaceString::kServerConfigurationNamespace);
    auto queryFilter = BSON("_id"
                            << "shardingDDLCoordinatorRecoveryDoc");
    auto updateModification =
        write_ops::UpdateModification(write_ops::UpdateModification::parseFromClassicUpdate(
            BSON("$inc" << BSON("noopWriteCount" << 1))));

    write_ops::UpdateOpEntry updateEntry(queryFilter, updateModification);
    updateEntry.setMulti(false);
    updateEntry.setUpsert(true);
    updateOp.setUpdates({updateEntry});

    return updateOp;
}

void setAllowMigrations(OperationContext* opCtx,
                        const NamespaceString& nss,
                        const boost::optional<UUID>& expectedCollectionUUID,
                        bool allowMigrations) {
    ConfigsvrSetAllowMigrations configsvrSetAllowMigrationsCmd(nss, allowMigrations);
    configsvrSetAllowMigrationsCmd.setCollectionUUID(expectedCollectionUUID);

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
    try {
        uassertStatusOKWithContext(
            Shard::CommandResponse::getEffectiveStatus(std::move(swSetAllowMigrationsResult)),
            str::stream() << "Error setting allowMigrations to " << allowMigrations
                          << " for collection " << nss.toString());
    } catch (const ExceptionFor<ErrorCodes::NamespaceNotSharded>&) {
        // Collection no longer exists
    } catch (const ExceptionFor<ErrorCodes::ConflictingOperationInProgress>&) {
        // Collection metadata was concurrently dropped
    }
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

std::vector<AsyncRequestsSender::Response> sendAuthenticatedCommandToShards(
    OperationContext* opCtx,
    StringData dbName,
    const BSONObj& command,
    const std::vector<ShardId>& shardIds,
    const std::shared_ptr<executor::TaskExecutor>& executor) {

    // The AsyncRequestsSender ignore impersonation metadata so we need to manually attach them to
    // the command
    BSONObjBuilder bob(command);
    rpc::writeAuthDataToImpersonatedUserMetadata(opCtx, &bob);
    WriteBlockBypass::get(opCtx).writeAsMetadata(&bob);
    auto authenticatedCommand = bob.obj();
    return sharding_util::sendCommandToShards(
        opCtx, dbName, authenticatedCommand, shardIds, executor);
}

void removeTagsMetadataFromConfig(OperationContext* opCtx,
                                  const NamespaceString& nss,
                                  const OperationSessionInfo& osi) {
    auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

    // Remove config.tags entries
    ConfigsvrRemoveTags configsvrRemoveTagsCmd(nss);
    configsvrRemoveTagsCmd.setDbName(NamespaceString::kAdminDb);

    const auto swRemoveTagsResult = configShard->runCommandWithFixedRetryAttempts(
        opCtx,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        NamespaceString::kAdminDb.toString(),
        CommandHelpers::appendMajorityWriteConcern(configsvrRemoveTagsCmd.toBSON(osi.toBSON())),
        Shard::RetryPolicy::kIdempotent);

    uassertStatusOKWithContext(
        Shard::CommandResponse::getEffectiveStatus(std::move(swRemoveTagsResult)),
        str::stream() << "Error removing tags for collection " << nss.toString());
}

void removeTagsMetadataFromConfig_notIdempotent(OperationContext* opCtx,
                                                const NamespaceString& nss,
                                                const WriteConcernOptions& writeConcern) {
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

    request.setWriteConcern(writeConcern.toBSON());

    auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    auto response = configShard->runBatchWriteCommand(
        opCtx, Milliseconds::max(), request, Shard::RetryPolicy::kIdempotentOrCursorInvalidated);

    uassertStatusOK(response.toStatus());
}

void removeCollAndChunksMetadataFromConfig(OperationContext* opCtx,
                                           const CollectionType& coll,
                                           const WriteConcernOptions& writeConcern) {
    IgnoreAPIParametersBlock ignoreApiParametersBlock(opCtx);
    const auto& nss = coll.getNss();
    const auto& uuid = coll.getUuid();

    ON_BLOCK_EXIT(
        [&] { Grid::get(opCtx)->catalogCache()->invalidateCollectionEntry_LINEARIZABLE(nss); });

    deleteCollection(opCtx, nss, uuid, writeConcern);

    deleteChunks(opCtx, uuid, writeConcern);
}

bool removeCollAndChunksMetadataFromConfig_notIdempotent(OperationContext* opCtx,
                                                         const NamespaceString& nss,
                                                         const WriteConcernOptions& writeConcern) {
    invariant(serverGlobalParams.clusterRole == ClusterRole::ConfigServer);
    IgnoreAPIParametersBlock ignoreApiParametersBlock(opCtx);
    const auto catalogClient = Grid::get(opCtx)->catalogClient();

    ON_BLOCK_EXIT(
        [&] { Grid::get(opCtx)->catalogCache()->invalidateCollectionEntry_LINEARIZABLE(nss); });

    try {
        auto coll =
            catalogClient->getCollection(opCtx, nss, repl::ReadConcernLevel::kLocalReadConcern);

        removeCollAndChunksMetadataFromConfig(opCtx, coll, writeConcern);
        return true;
    } catch (ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        // The collection is not sharded or doesn't exist
        return false;
    }
}

void shardedRenameMetadata(OperationContext* opCtx,
                           CollectionType& fromCollType,
                           const NamespaceString& toNss,
                           const WriteConcernOptions& writeConcern) {
    invariant(serverGlobalParams.clusterRole == ClusterRole::ConfigServer);

    auto catalogClient = Grid::get(opCtx)->catalogClient();
    auto fromNss = fromCollType.getNss();
    auto fromUUID = fromCollType.getUuid();

    // Delete eventual TO chunk/collection entries referring a dropped collection
    try {
        auto coll =
            catalogClient->getCollection(opCtx, toNss, repl::ReadConcernLevel::kLocalReadConcern);

        if (coll.getUuid() == fromCollType.getUuid()) {
            // Metadata rename already happened
            return;
        }

        // Delete TO chunk/collection entries referring a dropped collection
        removeCollAndChunksMetadataFromConfig_notIdempotent(opCtx, toNss, writeConcern);
    } catch (ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        // The TO collection is not sharded or doesn't exist
    }

    // Delete FROM collection entry
    deleteCollection(opCtx, fromNss, fromUUID, writeConcern);

    // Update FROM tags to TO
    updateTags(opCtx, fromNss, toNss, writeConcern);

    // Update namespace and bump timestamp of the FROM collection entry
    fromCollType.setNss(toNss);
    auto now = VectorClock::get(opCtx)->getTime();
    auto newTimestamp = now.clusterTime().asTimestamp();
    fromCollType.setTimestamp(newTimestamp);
    fromCollType.setEpoch(OID::gen());

    // Insert the TO collection entry
    uassertStatusOK(catalogClient->insertConfigDocument(
        opCtx, CollectionType::ConfigNS, fromCollType.toBSON(), writeConcern));
}

void checkRenamePreconditions(OperationContext* opCtx,
                              bool sourceIsSharded,
                              const NamespaceString& toNss,
                              const bool dropTarget) {
    if (sourceIsSharded) {
        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Namespace of target collection too long. Namespace: " << toNss
                              << " Max: " << NamespaceString::MaxNsShardedCollectionLen,
                toNss.size() <= NamespaceString::MaxNsShardedCollectionLen);
    }

    auto catalogClient = Grid::get(opCtx)->catalogClient();
    if (!dropTarget) {
        // Check that the sharded target collection doesn't exist
        try {
            catalogClient->getCollection(opCtx, toNss);
            // If no exception is thrown, the collection exists and is sharded
            uasserted(ErrorCodes::NamespaceExists,
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
        uassert(ErrorCodes::NamespaceExists,
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
            fromDB->getPrimary() == toDB->getPrimary());
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

void stopMigrations(OperationContext* opCtx,
                    const NamespaceString& nss,
                    const boost::optional<UUID>& expectedCollectionUUID) {
    setAllowMigrations(opCtx, nss, expectedCollectionUUID, false);
}

void resumeMigrations(OperationContext* opCtx,
                      const NamespaceString& nss,
                      const boost::optional<UUID>& expectedCollectionUUID) {
    setAllowMigrations(opCtx, nss, expectedCollectionUUID, true);
}

boost::optional<UUID> getCollectionUUID(OperationContext* opCtx,
                                        const NamespaceString& nss,
                                        bool allowViews) {
    AutoGetCollection autoColl(opCtx,
                               nss,
                               MODE_IS,
                               allowViews ? AutoGetCollectionViewMode::kViewsPermitted
                                          : AutoGetCollectionViewMode::kViewsForbidden);
    return autoColl ? boost::make_optional(autoColl->uuid()) : boost::none;
}

void performNoopRetryableWriteOnShards(OperationContext* opCtx,
                                       const std::vector<ShardId>& shardIds,
                                       const OperationSessionInfo& osi,
                                       const std::shared_ptr<executor::TaskExecutor>& executor) {
    const auto updateOp = buildNoopWriteRequestCommand();

    sharding_ddl_util::sendAuthenticatedCommandToShards(
        opCtx,
        updateOp.getDbName(),
        CommandHelpers::appendMajorityWriteConcern(updateOp.toBSON(osi.toBSON())),
        shardIds,
        executor);
}

void performNoopMajorityWriteLocally(OperationContext* opCtx) {
    const auto updateOp = buildNoopWriteRequestCommand();

    DBDirectClient client(opCtx);
    const auto commandResponse =
        client.runCommand(OpMsgRequest::fromDBAndBody(updateOp.getDbName(), updateOp.toBSON({})));

    const auto commandReply = commandResponse->getCommandReply();
    uassertStatusOK(getStatusFromWriteCommandReply(commandReply));

    WriteConcernResult ignoreResult;
    const WriteConcernOptions majorityWriteConcern{
        WriteConcernOptions::kMajority,
        WriteConcernOptions::SyncMode::UNSET,
        WriteConcernOptions::kWriteConcernTimeoutSharding};
    auto latestOpTime = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
    uassertStatusOK(waitForWriteConcern(opCtx, latestOpTime, majorityWriteConcern, &ignoreResult));
}

void sendDropCollectionParticipantCommandToShards(OperationContext* opCtx,
                                                  const NamespaceString& nss,
                                                  const std::vector<ShardId>& shardIds,
                                                  std::shared_ptr<executor::TaskExecutor> executor,
                                                  const OperationSessionInfo& osi) {
    const ShardsvrDropCollectionParticipant dropCollectionParticipant(nss);
    const auto cmdObj =
        CommandHelpers::appendMajorityWriteConcern(dropCollectionParticipant.toBSON({}));

    sharding_ddl_util::sendAuthenticatedCommandToShards(
        opCtx, nss.db(), cmdObj.addFields(osi.toBSON()), shardIds, executor);
}

BSONObj getCriticalSectionReasonForRename(const NamespaceString& from, const NamespaceString& to) {
    return BSON("command"
                << "rename"
                << "from" << from.toString() << "to" << to.toString());
}

}  // namespace sharding_ddl_util
}  // namespace mongo
