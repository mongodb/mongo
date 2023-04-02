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

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/cluster_transaction_api.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/distinct_command_gen.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/remove_tags_gen.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"
#include "mongo/db/s/sharding_logging.h"
#include "mongo/db/s/sharding_util.h"
#include "mongo/db/transaction/transaction_api.h"
#include "mongo/db/vector_clock.h"
#include "mongo/db/write_block_bypass.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/metadata/impersonated_user_metadata.h"
#include "mongo/s/analyze_shard_key_documents_gen.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_index_catalog.h"
#include "mongo/s/catalog/type_namespace_placement_gen.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/set_allow_migrations_gen.h"
#include "mongo/s/write_ops/batch_write_exec.h"
#include "mongo/util/uuid.h"


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {

static const size_t kSerializedErrorStatusMaxSize = 1024 * 2;

void sharding_ddl_util_serializeErrorStatusToBSON(const Status& status,
                                                  StringData fieldName,
                                                  BSONObjBuilder* bsonBuilder) {
    uassert(7418500, "Status must be an error", !status.isOK());

    BSONObjBuilder tmpBuilder;
    status.serialize(&tmpBuilder);

    if (status != ErrorCodes::TruncatedSerialization &&
        (size_t)tmpBuilder.asTempObj().objsize() > kSerializedErrorStatusMaxSize) {
        const auto statusStr = status.toString();
        const auto truncatedStatusStr =
            str::UTF8SafeTruncation(statusStr, kSerializedErrorStatusMaxSize);
        const Status truncatedStatus{ErrorCodes::TruncatedSerialization, truncatedStatusStr};

        tmpBuilder.resetToEmpty();
        truncatedStatus.serializeErrorToBSON(&tmpBuilder);
    }

    bsonBuilder->append(fieldName, tmpBuilder.obj());
}

Status sharding_ddl_util_deserializeErrorStatusFromBSON(const BSONElement& bsonElem) {
    const auto& bsonObj = bsonElem.Obj();

    long long code;
    uassertStatusOK(bsonExtractIntegerField(bsonObj, "code", &code));
    uassert(7418501, "Status must be an error", code != ErrorCodes::OK);

    std::string errmsg;
    uassertStatusOK(bsonExtractStringField(bsonObj, "errmsg", &errmsg));

    return {ErrorCodes::Error(code), errmsg, bsonObj};
}

namespace sharding_ddl_util {
namespace {

void runTransactionOnShardingCatalog(OperationContext* opCtx,
                                     txn_api::Callback&& transactionChain,
                                     const WriteConcernOptions& writeConcern) {
    // The Internal Transactions API receives the write concern option through the passed Operation
    // context.
    WriteConcernOptions originalWC = opCtx->getWriteConcern();
    opCtx->setWriteConcern(writeConcern);
    ScopeGuard guard([opCtx, originalWC] { opCtx->setWriteConcern(originalWC); });
    auto& executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();

    auto inlineExecutor = std::make_shared<executor::InlineExecutor>();
    auto sleepInlineExecutor = inlineExecutor->getSleepableExecutor(executor);

    // Instantiate the right custom TXN client to ensure that the queries to the config DB will be
    // routed to the CSRS.
    auto customTxnClient = [&]() -> std::unique_ptr<txn_api::TransactionClient> {
        if (serverGlobalParams.clusterRole.exclusivelyHasShardRole()) {
            return std::make_unique<txn_api::details::SEPTransactionClient>(
                opCtx,
                inlineExecutor,
                sleepInlineExecutor,
                std::make_unique<txn_api::details::ClusterSEPTransactionClientBehaviors>(
                    opCtx->getServiceContext()));
        }

        invariant(serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));
        return nullptr;
    }();

    txn_api::SyncTransactionWithRetries txn(opCtx,
                                            sleepInlineExecutor,
                                            nullptr /*resourceYielder*/,
                                            inlineExecutor,
                                            std::move(customTxnClient));
    txn.run(opCtx, std::move(transactionChain));
}

void updateTags(OperationContext* opCtx,
                Shard* configShard,
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
    /* Perform a transaction to delete the collection and append a new placement entry.
     * NOTE: deleteCollectionFn may be run on a separate thread than the one serving
     * deleteCollection(). For this reason, all the referenced parameters have to
     * be captured by value.
     * TODO SERVER-75189: replace capture list with a single '&'.
     */
    auto transactionChain = [nss, uuid](const txn_api::TransactionClient& txnClient,
                                        ExecutorPtr txnExec) {
        // Remove config.collection entry. Query by 'ns' AND 'uuid' so that the remove can be
        // resolved with an IXSCAN (thanks to the index on '_id') and is idempotent (thanks to the
        // 'uuid')
        const auto deleteCollectionQuery = BSON(
            CollectionType::kNssFieldName << nss.ns() << CollectionType::kUuidFieldName << uuid);

        write_ops::DeleteCommandRequest deleteOp(CollectionType::ConfigNS);
        deleteOp.setDeletes({[&]() {
            write_ops::DeleteOpEntry entry;
            entry.setMulti(false);
            entry.setQ(deleteCollectionQuery);
            return entry;
        }()});

        return txnClient.runCRUDOp(deleteOp, {} /*stmtIds*/)
            .thenRunOn(txnExec)
            .then([&](const BatchedCommandResponse& deleteCollResponse) {
                uassertStatusOK(deleteCollResponse.toStatus());

                // Skip the insertion of the placement entry if the previous statement didn't
                // remove any document - we can deduce that the whole transaction was already
                // committed in a previous attempt.
                if (deleteCollResponse.getN() == 0) {
                    BatchedCommandResponse noOpResponse;
                    noOpResponse.setStatus(Status::OK());
                    noOpResponse.setN(0);
                    return SemiFuture<BatchedCommandResponse>(std::move(noOpResponse));
                }

                auto now = VectorClock::get(getGlobalServiceContext())->getTime();
                const auto clusterTime = now.clusterTime().asTimestamp();
                NamespacePlacementType placementInfo(
                    NamespaceString(nss), clusterTime, {} /*shards*/);
                placementInfo.setUuid(uuid);
                write_ops::InsertCommandRequest insertPlacementEntry(
                    NamespaceString::kConfigsvrPlacementHistoryNamespace, {placementInfo.toBSON()});

                return txnClient.runCRUDOp(insertPlacementEntry, {} /*stmtIds*/);
            })
            .thenRunOn(txnExec)
            .then([&](const BatchedCommandResponse& insertPlacementEntryResponse) {
                uassertStatusOK(insertPlacementEntryResponse.toStatus());
            })
            .semi();
    };

    runTransactionOnShardingCatalog(opCtx, std::move(transactionChain), writeConcern);
}

void deleteShardingIndexCatalogMetadata(OperationContext* opCtx,
                                        const UUID& uuid,
                                        const WriteConcernOptions& writeConcern) {
    BatchedCommandRequest request([&] {
        write_ops::DeleteCommandRequest deleteOp(NamespaceString::kConfigsvrIndexCatalogNamespace);
        deleteOp.setDeletes({[&] {
            write_ops::DeleteOpEntry entry;
            entry.setQ(BSON(IndexCatalogType::kCollectionUUIDFieldName << uuid));
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
            DatabaseName::kAdmin.toString(),
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


// Check that the collection UUID is the same in every shard knowing the collection
void checkCollectionUUIDConsistencyAcrossShards(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const UUID& collectionUuid,
    const std::vector<mongo::ShardId>& shardIds,
    std::shared_ptr<executor::ScopedTaskExecutor> executor) {
    const BSONObj filterObj = BSON("name" << nss.coll());
    BSONObj cmdObj = BSON("listCollections" << 1 << "filter" << filterObj);

    auto responses = sharding_ddl_util::sendAuthenticatedCommandToShards(
        opCtx, nss.db().toString(), cmdObj, shardIds, **executor);

    struct MismatchedShard {
        std::string shardId;
        std::string uuid;
    };

    std::vector<MismatchedShard> mismatches;

    for (const auto& cmdResponse : responses) {
        auto responseData = uassertStatusOK(cmdResponse.swResponse);
        auto collectionVector = responseData.data.firstElement()["firstBatch"].Array();
        auto shardId = cmdResponse.shardId;

        if (collectionVector.empty()) {
            // Collection does not exist on the shard
            continue;
        }

        auto bsonCollectionUuid = collectionVector.front()["info"]["uuid"];
        if (collectionUuid.data() != bsonCollectionUuid.uuid()) {
            mismatches.push_back({shardId.toString(), bsonCollectionUuid.toString()});
        }
    }

    if (!mismatches.empty()) {
        std::stringstream errorMessage;
        errorMessage << "The collection " << nss.toString()
                     << " with expected UUID: " << collectionUuid.toString()
                     << " has different UUIDs on the following shards: [";

        for (const auto& mismatch : mismatches) {
            errorMessage << "{ " << mismatch.shardId << ":" << mismatch.uuid << " },";
        }
        errorMessage << "]";
        uasserted(ErrorCodes::InvalidUUID, errorMessage.str());
    }
}


// Check the collection does not exist in any shard when `dropTarget` is set to false
void checkTargetCollectionDoesNotExistInCluster(
    OperationContext* opCtx,
    const NamespaceString& toNss,
    const std::vector<mongo::ShardId>& shardIds,
    std::shared_ptr<executor::ScopedTaskExecutor> executor) {
    const BSONObj filterObj = BSON("name" << toNss.coll());
    BSONObj cmdObj = BSON("listCollections" << 1 << "filter" << filterObj);

    auto responses = sharding_ddl_util::sendAuthenticatedCommandToShards(
        opCtx, toNss.db(), cmdObj, shardIds, **executor);

    std::vector<std::string> shardsContainingTargetCollection;
    for (const auto& cmdResponse : responses) {
        uassertStatusOK(cmdResponse.swResponse);
        auto responseData = uassertStatusOK(cmdResponse.swResponse);
        auto collectionVector = responseData.data.firstElement()["firstBatch"].Array();

        if (!collectionVector.empty()) {
            shardsContainingTargetCollection.push_back(cmdResponse.shardId.toString());
        }
    }

    if (!shardsContainingTargetCollection.empty()) {
        std::stringstream errorMessage;
        errorMessage << "The collection " << toNss.toString()
                     << " already exists in the following shards: [";
        std::move(shardsContainingTargetCollection.begin(),
                  shardsContainingTargetCollection.end(),
                  std::ostream_iterator<std::string>(errorMessage, ", "));
        errorMessage << "]";
        uasserted(ErrorCodes::NamespaceExists, errorMessage.str());
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
    configsvrRemoveTagsCmd.setDbName(DatabaseName::kAdmin);

    const auto swRemoveTagsResult = configShard->runCommandWithFixedRetryAttempts(
        opCtx,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        DatabaseName::kAdmin.toString(),
        CommandHelpers::appendMajorityWriteConcern(configsvrRemoveTagsCmd.toBSON(osi.toBSON())),
        Shard::RetryPolicy::kIdempotent);

    uassertStatusOKWithContext(
        Shard::CommandResponse::getEffectiveStatus(std::move(swRemoveTagsResult)),
        str::stream() << "Error removing tags for collection " << nss.toString());
}

void removeQueryAnalyzerMetadataFromConfig(OperationContext* opCtx,
                                           const NamespaceString& nss,
                                           const boost::optional<UUID>& uuid) {
    auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    write_ops::DeleteCommandRequest deleteCmd(NamespaceString::kConfigQueryAnalyzersNamespace);
    if (uuid) {
        deleteCmd.setDeletes({[&] {
            write_ops::DeleteOpEntry entry;
            entry.setQ(
                BSON(analyze_shard_key::QueryAnalyzerDocument::kCollectionUuidFieldName << *uuid));
            entry.setMulti(false);
            return entry;
        }()});
    } else {
        deleteCmd.setDeletes({[&] {
            write_ops::DeleteOpEntry entry;
            entry.setQ(
                BSON(analyze_shard_key::QueryAnalyzerDocument::kNsFieldName << nss.toString()));
            entry.setMulti(true);
            return entry;
        }()});
    }

    const auto deleteResult = configShard->runCommandWithFixedRetryAttempts(
        opCtx,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        DatabaseName::kConfig.toString(),
        CommandHelpers::appendMajorityWriteConcern(deleteCmd.toBSON({})),
        Shard::RetryPolicy::kIdempotent);

    uassertStatusOKWithContext(Shard::CommandResponse::getEffectiveStatus(std::move(deleteResult)),
                               str::stream()
                                   << "Error removing query analyzer configurations for collection "
                                   << nss.toString());
}

void removeTagsMetadataFromConfig_notIdempotent(OperationContext* opCtx,
                                                Shard* configShard,
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

    ON_BLOCK_EXIT([&] {
        Grid::get(opCtx)->catalogCache()->invalidateCollectionEntry_LINEARIZABLE(nss);
        Grid::get(opCtx)->catalogCache()->invalidateIndexEntry_LINEARIZABLE(nss);
    });

    deleteCollection(opCtx, nss, uuid, writeConcern);

    deleteChunks(opCtx, uuid, writeConcern);

    deleteShardingIndexCatalogMetadata(opCtx, uuid, writeConcern);
}

bool removeCollAndChunksMetadataFromConfig_notIdempotent(OperationContext* opCtx,
                                                         ShardingCatalogClient* catalogClient,
                                                         const NamespaceString& nss,
                                                         const WriteConcernOptions& writeConcern) {
    invariant(serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));
    IgnoreAPIParametersBlock ignoreApiParametersBlock(opCtx);

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
                           Shard* configShard,
                           ShardingCatalogClient* catalogClient,
                           CollectionType& fromCollType,
                           const NamespaceString& toNss,
                           const WriteConcernOptions& writeConcern) {
    invariant(serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));

    auto fromNss = fromCollType.getNss();
    auto fromUUID = fromCollType.getUuid();

    // Delete eventual "TO" chunk/collection entries referring a dropped collection
    try {
        auto coll =
            catalogClient->getCollection(opCtx, toNss, repl::ReadConcernLevel::kLocalReadConcern);

        if (coll.getUuid() == fromCollType.getUuid()) {
            // shardedRenameMetadata() was already completed in a previous commit attempt.
            return;
        }

        // Delete "TO" chunk/collection entries referring a dropped collection
        removeCollAndChunksMetadataFromConfig_notIdempotent(
            opCtx, catalogClient, toNss, writeConcern);
    } catch (ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        // The "TO" collection is not sharded or doesn't exist
    }

    // Delete "FROM" from config.collections.
    deleteCollection(opCtx, fromNss, fromUUID, writeConcern);

    // Update "FROM" tags to "TO".
    updateTags(opCtx, configShard, fromNss, toNss, writeConcern);

    auto renamedCollPlacementInfo = [&]() {
        // Retrieve the latest placement document about "FROM" prior to its deletion (which will
        // have left an entry with an empty set of shards).
        auto query = BSON(NamespacePlacementType::kNssFieldName
                          << fromNss.ns() << NamespacePlacementType::kShardsFieldName
                          << BSON("$ne" << BSONArray()));

        auto queryResponse =
            uassertStatusOK(configShard->exhaustiveFindOnConfig(
                                opCtx,
                                ReadPreferenceSetting(ReadPreference::Nearest, TagSet{}),
                                repl::ReadConcernLevel::kMajorityReadConcern,
                                NamespaceString::kConfigsvrPlacementHistoryNamespace,
                                query,
                                BSON(NamespacePlacementType::kTimestampFieldName << -1) /*sort*/,
                                1 /*limit*/))
                .docs;

        if (!queryResponse.empty()) {
            return NamespacePlacementType::parse(IDLParserContext("shardedRenameMetadata"),
                                                 queryResponse.back());
        }

        // Persisted placement information may be unavailable as a consequence of FCV
        // transitions. Use the content of config.chunks as a fallback.
        DistinctCommandRequest distinctRequest(ChunkType::ConfigNS);
        distinctRequest.setKey(ChunkType::shard.name());
        distinctRequest.setQuery(BSON(ChunkType::collectionUUID.name() << fromUUID));

        auto reply = uassertStatusOK(configShard->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting(ReadPreference::PrimaryOnly, TagSet{}),
            DatabaseName::kConfig.toString(),
            distinctRequest.toBSON({}),
            Shard::RetryPolicy::kIdempotent));

        uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(reply));
        std::vector<ShardId> shardIds;
        for (const auto& valueElement : reply.response.getField("values").Array()) {
            shardIds.emplace_back(valueElement.String());
        }

        // Compose a placement info object based on the retrieved information; the timestamp
        // field may be disregarded, since it will be overwritten by the caller before being
        // consumed.
        NamespacePlacementType placementInfo(fromNss, Timestamp(), std::move(shardIds));
        placementInfo.setUuid(fromUUID);
        return placementInfo;
    }();

    // Rename namespace and bump timestamp in the original collection and placement entries of
    // "FROM".
    fromCollType.setNss(toNss);
    auto now = VectorClock::get(opCtx)->getTime();
    auto newTimestamp = now.clusterTime().asTimestamp();
    fromCollType.setTimestamp(newTimestamp);
    fromCollType.setEpoch(OID::gen());

    renamedCollPlacementInfo.setNss(toNss);
    renamedCollPlacementInfo.setTimestamp(newTimestamp);

    // Use the modified entries to insert collection and placement entries for "TO".
    auto transactionChain = [collInfo = std::move(fromCollType),
                             placementInfo = std::move(renamedCollPlacementInfo)](
                                const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
        write_ops::InsertCommandRequest insertConfigCollectionDoc(CollectionType::ConfigNS,
                                                                  {collInfo.toBSON()});
        return txnClient.runCRUDOp(insertConfigCollectionDoc, {} /*stmtIds*/)
            .thenRunOn(txnExec)
            .then([&](const BatchedCommandResponse& insertCollResponse) {
                uassertStatusOK(insertCollResponse.toStatus());
                if (insertCollResponse.getN() == 0) {
                    BatchedCommandResponse noOpResponse;
                    noOpResponse.setStatus(Status::OK());
                    noOpResponse.setN(0);
                    return SemiFuture<BatchedCommandResponse>(std::move(noOpResponse));
                }
                write_ops::InsertCommandRequest insertPlacementEntry(
                    NamespaceString::kConfigsvrPlacementHistoryNamespace, {placementInfo.toBSON()});
                return txnClient.runCRUDOp(insertPlacementEntry, {} /*stmtIds*/);
            })
            .thenRunOn(txnExec)
            .then([&](const BatchedCommandResponse& insertPlacementResponse) {
                uassertStatusOK(insertPlacementResponse.toStatus());
            })
            .semi();
    };

    runTransactionOnShardingCatalog(opCtx, std::move(transactionChain), writeConcern);
}

void checkCatalogConsistencyAcrossShardsForRename(
    OperationContext* opCtx,
    const NamespaceString& fromNss,
    const NamespaceString& toNss,
    const bool dropTarget,
    std::shared_ptr<executor::ScopedTaskExecutor> executor) {

    auto participants = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);

    auto sourceCollUuid = *getCollectionUUID(opCtx, fromNss);
    checkCollectionUUIDConsistencyAcrossShards(
        opCtx, fromNss, sourceCollUuid, participants, executor);

    if (!dropTarget) {
        checkTargetCollectionDoesNotExistInCluster(opCtx, toNss, participants, executor);
    }
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
    auto cri = uassertStatusOK(
        Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfoWithRefresh(opCtx, nss));
    const auto& cm = cri.cm;

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

    CreateCollectionResponse response(cri.getCollectionVersion());
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

bool checkAllowMigrations(OperationContext* opCtx, const NamespaceString& nss) {
    auto collDoc =
        uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getConfigShard()->exhaustiveFindOnConfig(
                            opCtx,
                            ReadPreferenceSetting(ReadPreference::PrimaryOnly, TagSet{}),
                            repl::ReadConcernLevel::kMajorityReadConcern,
                            CollectionType::ConfigNS,
                            BSON(CollectionType::kNssFieldName << nss.ns()),
                            BSONObj(),
                            1))
            .docs;

    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "collection " << nss.ns() << " not found",
            !collDoc.empty());

    auto coll = CollectionType(collDoc[0]);
    return coll.getAllowMigrations();
}

boost::optional<UUID> getCollectionUUID(OperationContext* opCtx,
                                        const NamespaceString& nss,
                                        bool allowViews) {
    AutoGetCollection autoColl(opCtx,
                               nss,
                               MODE_IS,
                               AutoGetCollection::Options{}.viewMode(
                                   allowViews ? auto_get_collection::ViewMode::kViewsPermitted
                                              : auto_get_collection::ViewMode::kViewsForbidden));
    return autoColl ? boost::make_optional(autoColl->uuid()) : boost::none;
}

void performNoopRetryableWriteOnShards(OperationContext* opCtx,
                                       const std::vector<ShardId>& shardIds,
                                       const OperationSessionInfo& osi,
                                       const std::shared_ptr<executor::TaskExecutor>& executor) {
    const auto updateOp = buildNoopWriteRequestCommand();

    sharding_ddl_util::sendAuthenticatedCommandToShards(
        opCtx,
        updateOp.getDbName().db(),
        CommandHelpers::appendMajorityWriteConcern(updateOp.toBSON(osi.toBSON())),
        shardIds,
        executor);
}

void performNoopMajorityWriteLocally(OperationContext* opCtx) {
    const auto updateOp = buildNoopWriteRequestCommand();

    DBDirectClient client(opCtx);
    const auto commandResponse = client.runCommand(
        OpMsgRequest::fromDBAndBody(updateOp.getDbName().db(), updateOp.toBSON({})));

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
                                                  const OperationSessionInfo& osi,
                                                  bool fromMigrate) {
    ShardsvrDropCollectionParticipant dropCollectionParticipant(nss);
    dropCollectionParticipant.setFromMigrate(fromMigrate);

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
