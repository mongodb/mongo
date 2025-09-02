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


#include "mongo/db/global_catalog/ddl/sharding_ddl_util.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bson_field.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/client.h"
#include "mongo/db/cluster_parameters/sharding_cluster_parameters_gen.h"
#include "mongo/db/cluster_transaction_api.h"
#include "mongo/db/commands.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/ddl/notify_sharding_event_gen.h"
#include "mongo/db/global_catalog/ddl/remove_tags_gen.h"
#include "mongo/db/global_catalog/ddl/set_allow_migrations_gen.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/global_catalog/type_namespace_placement_gen.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/db/repl/change_stream_oplog_notification.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/s/config/initial_split_policy.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/sharding_environment/sharding_logging.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/vector_clock/vector_clock.h"
#include "mongo/db/vector_clock/vector_clock_mutable.h"
#include "mongo/db/versioning_protocol/shard_version_factory.h"
#include "mongo/db/write_concern.h"
#include "mongo/executor/inline_executor.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/rpc/unique_message.h"
#include "mongo/s/analyze_shard_key_documents_gen.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <string>

#include <absl/container/node_hash_map.h>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <fmt/format.h>


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {

namespace sharding_ddl_util {
namespace {

const auto kUnsplittableShardKey = KeyPattern(BSON("_id" << 1));

void deleteChunks(OperationContext* opCtx,
                  const std::shared_ptr<Shard>& configShard,
                  const UUID& collectionUUID,
                  const WriteConcernOptions& writeConcern) {
    // Remove config.chunks entries
    // TODO SERVER-57221 don't use hint if not relevant anymore for delete performances
    auto hint = BSON(ChunkType::collectionUUID() << 1 << ChunkType::min() << 1);

    BatchedCommandRequest request([&] {
        write_ops::DeleteCommandRequest deleteOp(NamespaceString::kConfigsvrChunksNamespace);
        deleteOp.setDeletes({[&] {
            write_ops::DeleteOpEntry entry;
            entry.setQ(BSON(ChunkType::collectionUUID << collectionUUID));
            entry.setHint(hint);
            entry.setMulti(true);
            return entry;
        }()});
        return deleteOp;
    }());

    auto response =
        configShard->runBatchWriteCommand(opCtx,
                                          Milliseconds::max(),
                                          request,
                                          writeConcern,
                                          Shard::RetryPolicy::kIdempotentOrCursorInvalidated);

    uassertStatusOK(response.toStatus());
}

void deleteCollection(OperationContext* opCtx,
                      const NamespaceString& nss,
                      const UUID& uuid,
                      const WriteConcernOptions& writeConcern,
                      const OperationSessionInfo& osi,
                      const std::shared_ptr<executor::TaskExecutor>& executor,
                      bool logCommitOnConfigPlacementHistory) {
    auto transactionChain = [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
        // Remove config.collection entry. Query by 'ns' AND 'uuid' so that the remove can be
        // resolved with an IXSCAN (thanks to the index on '_id') and is idempotent (thanks to the
        // 'uuid')
        const auto deleteCollectionQuery =
            BSON(CollectionType::kNssFieldName
                 << NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault())
                 << CollectionType::kUuidFieldName << uuid);

        write_ops::DeleteCommandRequest deleteOp(CollectionType::ConfigNS);
        deleteOp.setDeletes({[&]() {
            write_ops::DeleteOpEntry entry;
            entry.setMulti(false);
            entry.setQ(deleteCollectionQuery);
            return entry;
        }()});

        return txnClient.runCRUDOp(deleteOp, {0} /*stmtIds*/)
            .thenRunOn(txnExec)
            .then([&](const BatchedCommandResponse& deleteCollResponse) {
                uassertStatusOK(deleteCollResponse.toStatus());

                // Skip the insertion of the placement entry if the previous statement didn't
                // remove any document - we can deduce that the whole transaction was already
                // committed in a previous attempt.
                if (!logCommitOnConfigPlacementHistory || deleteCollResponse.getN() == 0) {
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

                return txnClient.runCRUDOp(insertPlacementEntry, {1} /*stmtIds*/);
            })
            .thenRunOn(txnExec)
            .then([&](const BatchedCommandResponse& insertPlacementEntryResponse) {
                uassertStatusOK(insertPlacementEntryResponse.toStatus());
            })
            .semi();
    };

    runTransactionOnShardingCatalog(
        opCtx, std::move(transactionChain), writeConcern, osi, executor);
}

write_ops::UpdateCommandRequest buildNoopWriteRequestCommand() {
    write_ops::UpdateCommandRequest updateOp(NamespaceString::kServerConfigurationNamespace);
    auto queryFilter = BSON("_id" << "shardingDDLCoordinatorRecoveryDoc");
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
                        const boost::optional<OperationSessionInfo>& osi,
                        bool allowMigrations) {
    ConfigsvrSetAllowMigrations configsvrSetAllowMigrationsCmd(nss, allowMigrations);
    configsvrSetAllowMigrationsCmd.setCollectionUUID(expectedCollectionUUID);
    generic_argument_util::setMajorityWriteConcern(configsvrSetAllowMigrationsCmd);
    if (osi) {
        generic_argument_util::setOperationSessionInfo(configsvrSetAllowMigrationsCmd, *osi);
    }

    const auto swSetAllowMigrationsResult =
        Grid::get(opCtx)->shardRegistry()->getConfigShard()->runCommand(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            DatabaseName::kAdmin,
            configsvrSetAllowMigrationsCmd.toBSON(),
            Shard::RetryPolicy::kIdempotent  // Although ConfigsvrSetAllowMigrations is not really
                                             // idempotent (because it will cause the collection
                                             // version to be bumped), it is safe to be retried.
        );
    try {
        uassertStatusOKWithContext(
            Shard::CommandResponse::getEffectiveStatus(swSetAllowMigrationsResult),
            str::stream() << "Error setting allowMigrations to " << allowMigrations
                          << " for collection " << nss.toStringForErrorMsg());
    } catch (const ExceptionFor<ErrorCodes::NamespaceNotSharded>&) {
        // Collection no longer exists
    } catch (const ExceptionFor<ErrorCodes::ConflictingOperationInProgress>&) {
        // Collection metadata was concurrently dropped
    } catch (const ExceptionFor<ErrorCodes::ChunkMetadataInconsistency>&) {
        // Collection metadata has inconsistencies
    }
}

}  // namespace

Status possiblyTruncateErrorStatus(const Status& status) {
    static const size_t kMaxSerializedStatusSize = 2048ULL;
    auto possiblyTruncatedStatus = status;
    if (const std::string statusStr = possiblyTruncatedStatus.toString();
        statusStr.size() > kMaxSerializedStatusSize) {
        possiblyTruncatedStatus = {ErrorCodes::TruncatedSerialization,
                                   str::UTF8SafeTruncation(statusStr, kMaxSerializedStatusSize)};
    }
    return possiblyTruncatedStatus;
}

void linearizeCSRSReads(OperationContext* opCtx) {
    // Take advantage of ShardingLogging to perform a write to the configsvr with majority read
    // concern to guarantee that any read after this method sees any write performed by the previous
    // primary.
    uassertStatusOK(ShardingLogging::get(opCtx)->logChangeChecked(
        opCtx,
        "Linearize CSRS reads",
        NamespaceString::kServerConfigurationNamespace,
        {},
        defaultMajorityWriteConcernDoNotUse()));
}

void removeTagsMetadataFromConfig(OperationContext* opCtx,
                                  const NamespaceString& nss,
                                  const OperationSessionInfo& osi) {
    auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

    // Remove config.tags entries
    ConfigsvrRemoveTags configsvrRemoveTagsCmd(nss);
    configsvrRemoveTagsCmd.setDbName(DatabaseName::kAdmin);
    generic_argument_util::setMajorityWriteConcern(configsvrRemoveTagsCmd);
    generic_argument_util::setOperationSessionInfo(configsvrRemoveTagsCmd, osi);

    const auto swRemoveTagsResult =
        configShard->runCommand(opCtx,
                                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                DatabaseName::kAdmin,
                                configsvrRemoveTagsCmd.toBSON(),
                                Shard::RetryPolicy::kIdempotent);

    uassertStatusOKWithContext(Shard::CommandResponse::getEffectiveStatus(swRemoveTagsResult),
                               str::stream() << "Error removing tags for collection "
                                             << nss.toStringForErrorMsg());
}

void removeQueryAnalyzerMetadata(OperationContext* opCtx,
                                 const std::vector<UUID>& collectionUUIDs) {
    auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    std::vector<mongo::write_ops::DeleteOpEntry> deleteOps;
    for (size_t i = 0; i < collectionUUIDs.size(); ++i) {
        write_ops::DeleteOpEntry entry;
        entry.setQ(BSON(analyze_shard_key::QueryAnalyzerDocument::kCollectionUuidFieldName
                        << collectionUUIDs[i]));
        entry.setMulti(false);
        deleteOps.push_back(std::move(entry));
        // Ensure that a single batch request does not exceed the maximum BSON size. Considering
        // that each UUID is encoded using 16 bytes, using kMaxWriteBatchSize ensures that each
        // command will weigh around 1.6MB.
        if (deleteOps.size() == write_ops::kMaxWriteBatchSize || i + 1 == collectionUUIDs.size()) {
            write_ops::DeleteCommandRequest deleteCmd(
                NamespaceString::kConfigQueryAnalyzersNamespace);
            generic_argument_util::setMajorityWriteConcern(deleteCmd);
            deleteCmd.setDeletes(std::move(deleteOps));
            const auto deleteResult =
                configShard->runCommand(opCtx,
                                        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                        DatabaseName::kConfig,
                                        deleteCmd.toBSON(),
                                        Shard::RetryPolicy::kIdempotent);

            uassertStatusOKWithContext(Shard::CommandResponse::getEffectiveStatus(deleteResult),
                                       str::stream()
                                           << "Failed to remove query analyzer documents");
            deleteOps.clear();
        }
    }
}

void removeQueryAnalyzerMetadata(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 const OperationSessionInfo& osi) {
    auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    write_ops::DeleteCommandRequest deleteCmd(NamespaceString::kConfigQueryAnalyzersNamespace);
    generic_argument_util::setOperationSessionInfo(deleteCmd, osi);
    generic_argument_util::setMajorityWriteConcern(deleteCmd);
    write_ops::DeleteOpEntry entry;
    entry.setQ(BSON(analyze_shard_key::QueryAnalyzerDocument::kNsFieldName
                    << NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault())));
    entry.setMulti(false);
    deleteCmd.setDeletes({std::move(entry)});
    const auto deleteResult =
        configShard->runCommand(opCtx,
                                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                DatabaseName::kConfig,
                                deleteCmd.toBSON(),
                                Shard::RetryPolicy::kIdempotent);

    uassertStatusOKWithContext(Shard::CommandResponse::getEffectiveStatus(deleteResult),
                               str::stream() << "Failed to remove query analyzer documents");
}

void removeCollAndChunksMetadataFromConfig(OperationContext* opCtx,
                                           const std::shared_ptr<Shard>& configShard,
                                           ShardingCatalogClient* catalogClient,
                                           const CollectionType& coll,
                                           const WriteConcernOptions& writeConcern,
                                           const OperationSessionInfo& osi,
                                           const std::shared_ptr<executor::TaskExecutor>& executor,
                                           bool logCommitOnConfigPlacementHistory) {
    IgnoreAPIParametersBlock ignoreApiParametersBlock(opCtx);
    const auto& nss = coll.getNss();
    const auto& uuid = coll.getUuid();

    ON_BLOCK_EXIT(
        [&] { Grid::get(opCtx)->catalogCache()->invalidateCollectionEntry_LINEARIZABLE(nss); });

    /*
    Data from config.collection are deleted using a transaction to guarantee an atomic update on
    config.placementHistory. In case this operation is run by a ddl coordinator, we can re-use the
    osi in the transaction to guarantee the replay protection.
    */
    deleteCollection(
        opCtx, nss, uuid, writeConcern, osi, executor, logCommitOnConfigPlacementHistory);

    deleteChunks(opCtx, configShard, uuid, writeConcern);
}

void logDropCollectionCommitOnConfigPlacementHistory(
    OperationContext* opCtx,
    const NamespacePlacementType& placementChangeToLog,
    const OperationSessionInfo& osi,
    const std::shared_ptr<executor::TaskExecutor>& executor) {
    //    The operation is performed through a transaction to ensure serialization with concurrent
    //    resetPlacementHistory() requests.
    const auto insertHistoricalPlacementTxn = [&](const txn_api::TransactionClient& txnClient,
                                                  ExecutorPtr txnExec) {
        FindCommandRequest query(NamespaceString::kConfigsvrPlacementHistoryNamespace);
        query.setFilter(
            BSON(NamespacePlacementType::kNssFieldName << NamespaceStringUtil::serialize(
                     placementChangeToLog.getNss(), SerializationContext::stateDefault())));
        query.setSort(BSON(NamespacePlacementType::kTimestampFieldName << -1));
        query.setLimit(1);

        return txnClient.exhaustiveFind(query)
            .thenRunOn(txnExec)
            .then([&](const std::vector<BSONObj>& match) {
                const auto& collUuidToLog = placementChangeToLog.getUuid();
                if (match.size() == 1) {
                    const auto latestEntry = NamespacePlacementType::parse(
                        match[0], IDLParserContext("dropCollectionLocally"));
                    if (collUuidToLog && latestEntry.getUuid() == collUuidToLog.value() &&
                        latestEntry.getShards().empty()) {
                        BatchedCommandResponse noOpResponse;
                        noOpResponse.setStatus(Status::OK());
                        noOpResponse.setN(0);
                        return SemiFuture<BatchedCommandResponse>(std::move(noOpResponse));
                    }
                }

                write_ops::InsertCommandRequest insertPlacementEntry(
                    NamespaceString::kConfigsvrPlacementHistoryNamespace,
                    {placementChangeToLog.toBSON()});
                return txnClient.runCRUDOp(insertPlacementEntry, {1});
            })
            .thenRunOn(txnExec)
            .then([&](const BatchedCommandResponse& insertPlacementEntryResponse) {
                uassertStatusOK(insertPlacementEntryResponse.toStatus());
            })
            .semi();
    };

    auto wc = WriteConcernOptions{WriteConcernOptions::kMajority,
                                  WriteConcernOptions::SyncMode::UNSET,
                                  WriteConcernOptions::kNoTimeout};


    sharding_ddl_util::runTransactionOnShardingCatalog(
        opCtx, std::move(insertHistoricalPlacementTxn), wc, osi, executor);
}


void checkRenamePreconditions(OperationContext* opCtx,
                              const NamespaceString& toNss,
                              const boost::optional<CollectionType>& optToCollType,
                              bool isSourceUnsharded,
                              const bool dropTarget) {
    sharding_ddl_util::assertNamespaceLengthLimit(toNss, isSourceUnsharded);

    if (!dropTarget) {
        // Check that the target collection doesn't exist if dropTarget is not set
        uassert(ErrorCodes::NamespaceExists,
                str::stream() << "Target collection " << toNss.toStringForErrorMsg()
                              << " exists but dropTarget is not set",
                !optToCollType.has_value() &&
                    !CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, toNss));
    }

    // Check that there are no tags associated to the target collection
    auto tags =
        uassertStatusOK(Grid::get(opCtx)->catalogClient()->getTagsForCollection(opCtx, toNss));
    uassert(ErrorCodes::CommandFailed,
            str::stream() << "Can't rename to target collection " << toNss.toStringForErrorMsg()
                          << " because it must not have associated tags",
            tags.empty());
}

boost::optional<CreateCollectionResponse> checkIfCollectionAlreadyTrackedWithOptions(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const BSONObj& key,
    const BSONObj& collation,
    bool unique,
    bool unsplittable) {
    auto cm = uassertStatusOK(
        Grid::get(opCtx)->catalogCache()->getCollectionPlacementInfoWithRefresh(opCtx, nss));

    if (!cm.hasRoutingTable()) {
        return boost::none;
    }

    if (cm.isUnsplittable() && !unsplittable) {
        return boost::none;
    }

    auto defaultCollator =
        cm.getDefaultCollator() ? cm.getDefaultCollator()->getSpec().toBSON() : BSONObj();

    // If the collection is already sharded, fail if the deduced options in this request do not
    // match the options the collection was originally sharded with.
    uassert(ErrorCodes::AlreadyInitialized,
            str::stream() << "collection already exists with different options for collection "
                          << nss.toStringForErrorMsg(),
            SimpleBSONObjComparator::kInstance.evaluate(cm.getShardKeyPattern().toBSON() == key) &&
                SimpleBSONObjComparator::kInstance.evaluate(defaultCollator == collation) &&
                cm.isUnique() == unique && cm.isUnsplittable() == unsplittable);

    CreateCollectionResponse response(ShardVersionFactory::make(cm));
    response.setCollectionUUID(cm.getUUID());
    return response;
}

void stopMigrations(OperationContext* opCtx,
                    const NamespaceString& nss,
                    const boost::optional<UUID>& expectedCollectionUUID,
                    const boost::optional<OperationSessionInfo>& osi) {
    setAllowMigrations(opCtx, nss, expectedCollectionUUID, osi, false);
}

void resumeMigrations(OperationContext* opCtx,
                      const NamespaceString& nss,
                      const boost::optional<UUID>& expectedCollectionUUID,
                      const boost::optional<OperationSessionInfo>& osi) {
    setAllowMigrations(opCtx, nss, expectedCollectionUUID, osi, true);
}

bool checkAllowMigrations(OperationContext* opCtx, const NamespaceString& nss) {
    auto collDoc =
        uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getConfigShard()->exhaustiveFindOnConfig(
                            opCtx,
                            ReadPreferenceSetting(ReadPreference::PrimaryOnly, TagSet{}),
                            repl::ReadConcernLevel::kMajorityReadConcern,
                            CollectionType::ConfigNS,
                            BSON(CollectionType::kNssFieldName << NamespaceStringUtil::serialize(
                                     nss, SerializationContext::stateDefault())),
                            BSONObj(),
                            1))
            .docs;

    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "collection " << nss.toStringForErrorMsg() << " not found",
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
    auto updateOp = buildNoopWriteRequestCommand();
    generic_argument_util::setOperationSessionInfo(updateOp, osi);
    generic_argument_util::setMajorityWriteConcern(updateOp);
    auto opts = std::make_shared<async_rpc::AsyncRPCOptions<write_ops::UpdateCommandRequest>>(
        executor, CancellationToken::uncancelable(), updateOp);
    sharding_ddl_util::sendAuthenticatedCommandToShards(opCtx, opts, shardIds);
}

void performNoopMajorityWriteLocally(OperationContext* opCtx) {
    const auto updateOp = buildNoopWriteRequestCommand();

    DBDirectClient client(opCtx);
    const auto commandResponse = client.runCommand(OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::kNotRequired, updateOp.getDbName(), updateOp.toBSON()));

    const auto commandReply = commandResponse->getCommandReply();
    uassertStatusOK(getStatusFromWriteCommandReply(commandReply));

    WriteConcernResult ignoreResult;
    auto latestOpTime = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
    uassertStatusOK(waitForWriteConcern(
        opCtx, latestOpTime, defaultMajorityWriteConcernDoNotUse(), &ignoreResult));
}

void sendDropCollectionParticipantCommandToShards(OperationContext* opCtx,
                                                  const NamespaceString& nss,
                                                  const std::vector<ShardId>& shardIds,
                                                  std::shared_ptr<executor::TaskExecutor> executor,
                                                  const OperationSessionInfo& osi,
                                                  bool fromMigrate,
                                                  bool dropSystemCollections,
                                                  const boost::optional<UUID>& collectionUUID,
                                                  bool requireCollectionEmpty) {
    ShardsvrDropCollectionParticipant dropCollectionParticipant(nss);
    dropCollectionParticipant.setFromMigrate(fromMigrate);
    dropCollectionParticipant.setDropSystemCollections(dropSystemCollections);
    dropCollectionParticipant.setCollectionUUID(collectionUUID);
    dropCollectionParticipant.setRequireCollectionEmpty(requireCollectionEmpty);
    generic_argument_util::setOperationSessionInfo(dropCollectionParticipant, osi);
    generic_argument_util::setMajorityWriteConcern(dropCollectionParticipant);
    auto opts = std::make_shared<async_rpc::AsyncRPCOptions<ShardsvrDropCollectionParticipant>>(
        executor, CancellationToken::uncancelable(), dropCollectionParticipant);
    sharding_ddl_util::sendAuthenticatedCommandToShards(opCtx, opts, shardIds);
}

BSONObj getCriticalSectionReasonForRename(const NamespaceString& from, const NamespaceString& to) {
    return BSON(
        "command" << "rename"
                  << "from"
                  << NamespaceStringUtil::serialize(from, SerializationContext::stateDefault())
                  << "to"
                  << NamespaceStringUtil::serialize(to, SerializationContext::stateDefault()));
}

void runTransactionOnShardingCatalog(OperationContext* opCtx,
                                     txn_api::Callback&& transactionChain,
                                     const WriteConcernOptions& writeConcern,
                                     const OperationSessionInfo& osi,
                                     const std::shared_ptr<executor::TaskExecutor>& inputExecutor) {
    // The Internal Transactions API receives the write concern option and osi through the
    // passed Operation context. We opt for creating a new one to avoid any possible side
    // effects.
    auto newClient = opCtx->getServiceContext()
                         ->getService(ClusterRole::ShardServer)
                         ->makeClient("ShardingCatalogTransaction");

    AuthorizationSession::get(newClient.get())->grantInternalAuthorization();
    AlternativeClientRegion acr(newClient);

    auto newOpCtxHolder = CancelableOperationContext(
        cc().makeOperationContext(),
        opCtx->getCancellationToken(),
        Grid::get(opCtx->getServiceContext())->getExecutorPool()->getFixedExecutor());
    auto newOpCtx = newOpCtxHolder.get();
    newOpCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

    // if executor is provided, use it, otherwise use the fixed executor
    const auto& executor = [&inputExecutor, ctx = newOpCtx]() {
        if (inputExecutor)
            return inputExecutor;

        return Grid::get(ctx)->getExecutorPool()->getFixedExecutor();
    }();

    auto inlineExecutor = std::make_shared<executor::InlineExecutor>();

    // Instantiate the right custom TXN client to ensure that the queries to the config DB will be
    // routed to the CSRS.
    auto customTxnClient = [&]() -> std::unique_ptr<txn_api::TransactionClient> {
        auto sleepInlineExecutor = inlineExecutor->getSleepableExecutor(executor);
        return std::make_unique<txn_api::details::SEPTransactionClient>(
            newOpCtx,
            inlineExecutor,
            sleepInlineExecutor,
            executor,
            std::make_unique<txn_api::details::ClusterSEPTransactionClientBehaviors>(
                newOpCtx->getServiceContext()));
    }();

    if (osi.getSessionId()) {
        auto lk = stdx::lock_guard(*newOpCtx->getClient());
        newOpCtx->setLogicalSessionId(*osi.getSessionId());
        newOpCtx->setTxnNumber(*osi.getTxnNumber());
    }
    newOpCtx->setWriteConcern(writeConcern);

    txn_api::SyncTransactionWithRetries txn(newOpCtx,
                                            executor,
                                            nullptr /*resourceYielder*/,
                                            inlineExecutor,
                                            std::move(customTxnClient));
    txn.run(newOpCtx, std::move(transactionChain));

    VectorClockMutable::get(opCtx)->waitForDurableConfigTime().get(opCtx);
}

const KeyPattern& unsplittableCollectionShardKey() {
    return kUnsplittableShardKey;
}

boost::optional<CollectionType> getCollectionFromConfigServer(OperationContext* opCtx,
                                                              const NamespaceString& nss) {
    try {
        return Grid::get(opCtx)->catalogClient()->getCollection(opCtx, nss);
    } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        // The collection is not tracked by the config server or doesn't exist.
        return boost::none;
    }
}

std::vector<BatchedCommandRequest> getOperationsToCreateOrShardCollectionOnShardingCatalog(
    const CollectionType& coll,
    const std::vector<ChunkType>& chunks,
    const ChunkVersion& placementVersion,
    const std::set<ShardId>& shardIds) {
    const auto& nss = coll.getNss();
    const auto& uuid = coll.getUuid();
    tassert(8377600,
            str::stream() << "Can't create collection " << toStringForLogging(nss)
                          << " without chunks",
            !chunks.empty());
    tassert(8377601,
            str::stream() << "Can't create collection " << toStringForLogging(nss)
                          << " without shards owning data",
            !shardIds.empty());

    std::vector<BSONObj> chunkEntries;
    chunkEntries.reserve(chunks.size());
    for (const auto& chunk : chunks) {
        chunkEntries.push_back(chunk.toConfigBSON());
    }

    write_ops::DeleteCommandRequest deleteChunkIfTheCollectionExistsAsUnsplittable(
        NamespaceString::kConfigsvrChunksNamespace);
    deleteChunkIfTheCollectionExistsAsUnsplittable.setDeletes({[&] {
        auto deleteQuery = BSON(ChunkType::collectionUUID.name() << uuid);
        return write_ops::DeleteOpEntry{std::move(deleteQuery), false /* multi */};
    }()});

    write_ops::InsertCommandRequest insertChunks(NamespaceString::kConfigsvrChunksNamespace,
                                                 std::move(chunkEntries));
    insertChunks.setWriteCommandRequestBase([] {
        write_ops::WriteCommandRequestBase wcb;
        wcb.setOrdered(false);
        return wcb;
    }());

    write_ops::UpdateCommandRequest upsertCollection(CollectionType::ConfigNS);
    upsertCollection.setUpdates({[&] {
        auto updateQuery =
            BSON(CollectionType::kNssFieldName
                 << NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault())
                 << CollectionType::kUuidFieldName << uuid);
        mongo::write_ops::UpdateModification updateModification{
            coll.toBSON(), write_ops::UpdateModification::ReplacementTag{}};
        write_ops::UpdateOpEntry updateEntry{std::move(updateQuery), std::move(updateModification)};
        updateEntry.setUpsert(true);
        return updateEntry;
    }()});

    write_ops::InsertCommandRequest insertPlacementHistory = [&]() {
        NamespacePlacementType placementInfo{
            nss,
            placementVersion.getTimestamp(),
            std::vector<mongo::ShardId>(shardIds.cbegin(), shardIds.cend())};
        placementInfo.setUuid(uuid);
        return write_ops::InsertCommandRequest(NamespaceString::kConfigsvrPlacementHistoryNamespace,
                                               {placementInfo.toBSON()});
    }();

    std::vector<BatchedCommandRequest> ret;
    ret.emplace_back(std::move(deleteChunkIfTheCollectionExistsAsUnsplittable));
    ret.emplace_back(std::move(insertChunks));
    ret.emplace_back(std::move(upsertCollection));
    ret.emplace_back(std::move(insertPlacementHistory));
    return ret;
}

std::pair<CollectionType, std::vector<ChunkType>> generateMetadataForUnsplittableCollectionCreation(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const UUID& collectionUuid,
    const BSONObj& defaultCollation,
    const ShardId& shardId) {
    const auto unsplittableShardKeyPattern = ShardKeyPattern(unsplittableCollectionShardKey());
    const auto initialChunks = SingleChunkOnPrimarySplitPolicy().createFirstChunks(
        opCtx, unsplittableShardKeyPattern, {collectionUuid, shardId});
    invariant(initialChunks.chunks.size() == 1);
    const auto& placementVersion = initialChunks.chunks.front().getVersion();

    auto coll = CollectionType(nss,
                               placementVersion.epoch(),
                               placementVersion.getTimestamp(),
                               Date_t::now(),
                               collectionUuid,
                               unsplittableCollectionShardKey().toBSON());
    coll.setUnsplittable(true);
    coll.setDefaultCollation(defaultCollation);

    return std::make_pair(std::move(coll), std::move(initialChunks.chunks));
}

void runTransactionWithStmtIdsOnShardingCatalog(
    OperationContext* opCtx,
    const std::shared_ptr<executor::TaskExecutor>& executor,
    const OperationSessionInfo& osi,
    const std::vector<BatchedCommandRequest>&& ops) {
    const auto transactionChain = [&](const txn_api::TransactionClient& txnClient,
                                      ExecutorPtr txnExec) {
        StmtId statementsCounter = 0;
        for (auto&& op : ops) {
            const auto numOps = op.sizeWriteOps();
            std::vector<StmtId> statementIds(numOps);
            std::iota(statementIds.begin(), statementIds.end(), statementsCounter);
            statementsCounter += numOps;
            const auto response = txnClient.runCRUDOpSync(op, std::move(statementIds));
            uassertStatusOK(response.toStatus());
        }

        return SemiFuture<void>::makeReady();
    };

    // Ensure that this function will only return once the transaction gets majority committed
    auto wc = WriteConcernOptions{WriteConcernOptions::kMajority,
                                  WriteConcernOptions::SyncMode::UNSET,
                                  WriteConcernOptions::kNoTimeout};

    sharding_ddl_util::runTransactionOnShardingCatalog(
        opCtx, std::move(transactionChain), wc, osi, executor);
}

void assertDataMovementAllowed() {
    bool clusterHasTwoOrMoreShards = [&]() {
        auto* clusterParameters = ServerParameterSet::getClusterParameterSet();
        auto* clusterCardinalityParam =
            clusterParameters->get<ClusterParameterWithStorage<ShardedClusterCardinalityParam>>(
                "shardedClusterCardinalityForDirectConns");
        return clusterCardinalityParam->getValue(boost::none).getHasTwoOrMoreShards();
    }();

    uassert(ErrorCodes::IllegalOperation,
            "Cannot migrate data in a cluster before a second shard has been successfully added",
            clusterHasTwoOrMoreShards);
}


void assertNamespaceLengthLimit(const NamespaceString& nss, bool isUnsharded) {
    auto maxNsLen = isUnsharded ? NamespaceString::MaxUserNsCollectionLen
                                : NamespaceString::MaxUserNsShardedCollectionLen;
    uassert(ErrorCodes::InvalidNamespace,
            fmt::format("Namespace too log. The namespace {} is {} characters long, but the "
                        "maximum allowed is {}",
                        nss.toStringForErrorMsg(),
                        nss.size(),
                        maxNsLen),
            nss.size() <= maxNsLen);
}

void commitCreateDatabaseMetadataToShardCatalog(
    OperationContext* opCtx,
    const DatabaseType& db,
    const OperationSessionInfo& osi,
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    const CancellationToken& token) {
    ShardsvrCommitCreateDatabaseMetadata shardsvrRequest;
    shardsvrRequest.setDbName(db.getDbName());
    shardsvrRequest.setDbVersion(db.getVersion());

    generic_argument_util::setMajorityWriteConcern(shardsvrRequest);
    generic_argument_util::setOperationSessionInfo(shardsvrRequest, osi);

    auto opts = std::make_shared<async_rpc::AsyncRPCOptions<ShardsvrCommitCreateDatabaseMetadata>>(
        **executor, token, std::move(shardsvrRequest));

    sendAuthenticatedCommandToShards(opCtx, opts, {db.getPrimary()});
}

void commitDropDatabaseMetadataToShardCatalog(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    const ShardId& shardId,
    const OperationSessionInfo& osi,
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    const CancellationToken& token) {
    ShardsvrCommitDropDatabaseMetadata shardsvrRequest;
    shardsvrRequest.setDbName(dbName);

    generic_argument_util::setMajorityWriteConcern(shardsvrRequest);
    generic_argument_util::setOperationSessionInfo(shardsvrRequest, osi);

    auto opts = std::make_shared<async_rpc::AsyncRPCOptions<ShardsvrCommitDropDatabaseMetadata>>(
        **executor, token, std::move(shardsvrRequest));

    sendAuthenticatedCommandToShards(opCtx, opts, {shardId});
}

void sendFetchCollMetadataToShards(OperationContext* opCtx,
                                   const NamespaceString& nss,
                                   const std::vector<ShardId>& shardIds,
                                   const OperationSessionInfo& osi,
                                   const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
                                   const CancellationToken& token) {
    ShardsvrFetchCollMetadata request(nss);
    request.setDbName(DatabaseName::kAdmin);

    generic_argument_util::setMajorityWriteConcern(request);
    generic_argument_util::setOperationSessionInfo(request, osi);

    auto opts = std::make_shared<async_rpc::AsyncRPCOptions<ShardsvrFetchCollMetadata>>(
        **executor, token, std::move(request));

    sendAuthenticatedCommandToShards(opCtx, opts, shardIds);
}

AuthoritativeMetadataAccessLevelEnum getGrantedAuthoritativeMetadataAccessLevel(
    const VersionContext& vCtx, const ServerGlobalParams::FCVSnapshot& snapshot) {
    const bool isAuthoritativeDDLEnabled =
        feature_flags::gShardAuthoritativeDbMetadataDDL.isEnabled(vCtx, snapshot);
    const bool isAuthoritativeCRUDEnabled =
        feature_flags::gShardAuthoritativeDbMetadataCRUD.isEnabled(vCtx, snapshot);

    tassert(10162502,
            "shardAuthoritativeDbMetadataCRUD should not be enabled if "
            "shardAuthoritativeDbMetadataDDL is disabled",
            isAuthoritativeDDLEnabled || !isAuthoritativeCRUDEnabled);

    if (!isAuthoritativeDDLEnabled) {
        return AuthoritativeMetadataAccessLevelEnum::kNone;
    }

    if (!isAuthoritativeCRUDEnabled) {
        return AuthoritativeMetadataAccessLevelEnum::kWritesAllowed;
    }

    return AuthoritativeMetadataAccessLevelEnum::kWritesAndReadsAllowed;
}

boost::optional<ShardId> pickShardOwningCollectionChunks(OperationContext* opCtx,
                                                         const UUID& collUuid) {
    const Timestamp dummyTimestamp;
    const OID dummyEpoch;
    auto chunks = uassertStatusOK(Grid::get(opCtx)->catalogClient()->getChunks(
        opCtx,
        BSON(ChunkType::collectionUUID() << collUuid) /*query*/,
        BSON(ChunkType::min() << 1) /*sort*/,
        1 /*limit*/,
        nullptr /*opTime*/,
        dummyEpoch,
        dummyTimestamp,
        repl::ReadConcernLevelEnum::kMajorityReadConcern));
    return chunks.empty() ? boost::none : boost::optional<ShardId>(chunks[0].getShard());
}

void generatePlacementChangeNotificationOnShard(
    OperationContext* opCtx,
    const NamespacePlacementChanged& placementChangeNotification,
    const ShardId& shard,
    std::function<OperationSessionInfo(OperationContext*)> buildNewSessionFn,
    const std::shared_ptr<executor::ScopedTaskExecutor>& executor,
    const CancellationToken& token) {
    LOGV2(10386900,
          "Sending namespacePlacementChange notification to shard",
          "notification"_attr = placementChangeNotification,
          "recipientShard"_attr = shard);

    if (const auto thisShardId = ShardingState::get(opCtx)->shardId(); thisShardId == shard) {
        // The request can be resolved into a local function call.
        notifyChangeStreamsOnNamespacePlacementChanged(opCtx, placementChangeNotification);
        return;
    }

    ShardsvrNotifyShardingEventRequest request(notify_sharding_event::kNamespacePlacementChanged,
                                               placementChangeNotification.toBSON());
    request.setDbName(DatabaseName::kAdmin);

    generic_argument_util::setMajorityWriteConcern(request);
    const auto osi = buildNewSessionFn(opCtx);
    generic_argument_util::setOperationSessionInfo(request, osi);

    auto opts = std::make_shared<async_rpc::AsyncRPCOptions<ShardsvrNotifyShardingEventRequest>>(
        **executor, token, std::move(request));

    try {
        sendAuthenticatedCommandToShards(opCtx, opts, {shard});
    } catch (const ExceptionFor<ErrorCodes::UnsupportedShardingEventNotification>& e) {
        // Swallow the error, which is expected when the recipient runs a legacy binary that does
        // not support the kNamespacePlacementChanged notification type.
        LOGV2_WARNING(10386901,
                      "Skipping namespacePlacementChange notification",
                      "error"_attr = redact(e.toStatus()));
    }
}


}  // namespace sharding_ddl_util
}  // namespace mongo
