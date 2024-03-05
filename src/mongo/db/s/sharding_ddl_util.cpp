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

#include <algorithm>
#include <array>
#include <boost/cstdint.hpp>
#include <boost/smart_ptr.hpp>
#include <cstdint>
#include <iterator>
#include <ostream>
#include <string>
#include <tuple>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/bson/bson_field.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/cluster_role.h"
#include "mongo/db/cluster_transaction_api.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/ops/write_ops_parsers.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/resource_yielder.h"
#include "mongo/db/s/config/initial_split_policy.h"
#include "mongo/db/s/remove_tags_gen.h"
#include "mongo/db/s/sharding_logging.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/vector_clock.h"
#include "mongo/db/vector_clock_mutable.h"
#include "mongo/db/write_concern.h"
#include "mongo/executor/async_rpc.h"
#include "mongo/executor/async_rpc_util.h"
#include "mongo/executor/inline_executor.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/rpc/unique_message.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_collection_gen.h"
#include "mongo/s/catalog/type_database_gen.h"
#include "mongo/s/catalog/type_index_catalog_gen.h"
#include "mongo/s/catalog/type_namespace_placement_gen.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/set_allow_migrations_gen.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/s/sharding_feature_flags_gen.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/read_through_cache.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
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

const auto kUnsplittableShardKey = KeyPattern(BSON("_id" << 1));

void deleteChunks(OperationContext* opCtx,
                  const std::shared_ptr<Shard>& configShard,
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

    auto response = configShard->runBatchWriteCommand(
        opCtx, Milliseconds::max(), request, Shard::RetryPolicy::kIdempotentOrCursorInvalidated);

    uassertStatusOK(response.toStatus());
}

void deleteCollection(OperationContext* opCtx,
                      const NamespaceString& nss,
                      const UUID& uuid,
                      const WriteConcernOptions& writeConcern,
                      const OperationSessionInfo& osi,
                      const std::shared_ptr<executor::TaskExecutor>& executor,
                      bool useClusterTransaction) {
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

                return txnClient.runCRUDOp(insertPlacementEntry, {1} /*stmtIds*/);
            })
            .thenRunOn(txnExec)
            .then([&](const BatchedCommandResponse& insertPlacementEntryResponse) {
                uassertStatusOK(insertPlacementEntryResponse.toStatus());
            })
            .semi();
    };

    runTransactionOnShardingCatalog(
        opCtx, std::move(transactionChain), writeConcern, osi, useClusterTransaction, executor);
}

void deleteShardingIndexCatalogMetadata(OperationContext* opCtx,
                                        const std::shared_ptr<Shard>& configShard,
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
                        const boost::optional<OperationSessionInfo>& osi,
                        bool allowMigrations) {
    ConfigsvrSetAllowMigrations configsvrSetAllowMigrationsCmd(nss, allowMigrations);
    configsvrSetAllowMigrationsCmd.setCollectionUUID(expectedCollectionUUID);

    const auto swSetAllowMigrationsResult =
        Grid::get(opCtx)->shardRegistry()->getConfigShard()->runCommandWithFixedRetryAttempts(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            DatabaseName::kAdmin,
            CommandHelpers::appendMajorityWriteConcern(
                configsvrSetAllowMigrationsCmd.toBSON(osi ? osi->toBSON() : BSONObj())),
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
        NamespaceString::kServerConfigurationNamespace,
        {},
        ShardingCatalogClient::kMajorityWriteConcern));
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
        DatabaseName::kAdmin,
        CommandHelpers::appendMajorityWriteConcern(configsvrRemoveTagsCmd.toBSON(osi.toBSON())),
        Shard::RetryPolicy::kIdempotent);

    uassertStatusOKWithContext(Shard::CommandResponse::getEffectiveStatus(swRemoveTagsResult),
                               str::stream() << "Error removing tags for collection "
                                             << nss.toStringForErrorMsg());
}

void removeQueryAnalyzerMetadataFromConfig(OperationContext* opCtx, const BSONObj& filter) {
    auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    write_ops::DeleteCommandRequest deleteCmd(NamespaceString::kConfigQueryAnalyzersNamespace);
    deleteCmd.setDeletes({[&] {
        write_ops::DeleteOpEntry entry;
        entry.setQ(filter);
        entry.setMulti(true);
        return entry;
    }()});

    const auto deleteResult = configShard->runCommandWithFixedRetryAttempts(
        opCtx,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        DatabaseName::kConfig,
        CommandHelpers::appendMajorityWriteConcern(deleteCmd.toBSON({})),
        Shard::RetryPolicy::kIdempotent);

    uassertStatusOKWithContext(
        Shard::CommandResponse::getEffectiveStatus(deleteResult),
        str::stream() << "Failed to remove query analyzer documents that match the filter"
                      << filter);
}

void removeCollAndChunksMetadataFromConfig(
    OperationContext* opCtx,
    const std::shared_ptr<Shard>& configShard,
    ShardingCatalogClient* catalogClient,
    const CollectionType& coll,
    const WriteConcernOptions& writeConcern,
    const OperationSessionInfo& osi,
    bool useClusterTransaction,
    const std::shared_ptr<executor::TaskExecutor>& executor) {
    IgnoreAPIParametersBlock ignoreApiParametersBlock(opCtx);
    const auto& nss = coll.getNss();
    const auto& uuid = coll.getUuid();

    ON_BLOCK_EXIT([&] {
        Grid::get(opCtx)->catalogCache()->invalidateCollectionEntry_LINEARIZABLE(nss);
        Grid::get(opCtx)->catalogCache()->invalidateIndexEntry_LINEARIZABLE(nss);
    });

    /*
    Data from config.collection are deleted using a transaction to guarantee an atomic update on
    config.placementHistory. In case this operation is run by a ddl coordinator, we can re-use the
    osi in the transaction to guarantee the replay protection.
    */
    deleteCollection(opCtx, nss, uuid, writeConcern, osi, executor, useClusterTransaction);

    deleteChunks(opCtx, configShard, uuid, writeConcern);

    deleteShardingIndexCatalogMetadata(opCtx, configShard, uuid, writeConcern);
}

void checkRenamePreconditions(OperationContext* opCtx,
                              const NamespaceString& toNss,
                              const boost::optional<CollectionType>& optToCollType,
                              const bool dropTarget) {
    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << "Namespace of target collection too long. Namespace: "
                          << toNss.toStringForErrorMsg()
                          << " Max: " << NamespaceString::MaxNsShardedCollectionLen,
            toNss.size() <= NamespaceString::MaxNsShardedCollectionLen);

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
    auto cri = uassertStatusOK(
        Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfoWithRefresh(opCtx, nss));
    const auto& cm = cri.cm;

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

    CreateCollectionResponse response(cri.getCollectionVersion());
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
    const auto updateOp = buildNoopWriteRequestCommand();
    async_rpc::GenericArgs args;
    async_rpc::AsyncRPCCommandHelpers::appendOSI(args, osi);
    async_rpc::AsyncRPCCommandHelpers::appendMajorityWriteConcern(args);
    auto opts = std::make_shared<async_rpc::AsyncRPCOptions<write_ops::UpdateCommandRequest>>(
        executor, CancellationToken::uncancelable(), updateOp, args);
    sharding_ddl_util::sendAuthenticatedCommandToShards(opCtx, opts, shardIds);
}

void performNoopMajorityWriteLocally(OperationContext* opCtx) {
    const auto updateOp = buildNoopWriteRequestCommand();

    DBDirectClient client(opCtx);
    const auto commandResponse = client.runCommand(OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::kNotRequired, updateOp.getDbName(), updateOp.toBSON({})));

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
                                                  bool fromMigrate,
                                                  bool dropSystemCollections,
                                                  const boost::optional<UUID>& collectionUUID) {
    ShardsvrDropCollectionParticipant dropCollectionParticipant(nss);
    dropCollectionParticipant.setFromMigrate(fromMigrate);
    dropCollectionParticipant.setDropSystemCollections(dropSystemCollections);
    dropCollectionParticipant.setCollectionUUID(collectionUUID);
    async_rpc::GenericArgs args;
    async_rpc::AsyncRPCCommandHelpers::appendOSI(args, osi);
    async_rpc::AsyncRPCCommandHelpers::appendMajorityWriteConcern(args);
    auto opts = std::make_shared<async_rpc::AsyncRPCOptions<ShardsvrDropCollectionParticipant>>(
        executor, CancellationToken::uncancelable(), dropCollectionParticipant, args);
    sharding_ddl_util::sendAuthenticatedCommandToShards(opCtx, opts, shardIds);
}

BSONObj getCriticalSectionReasonForRename(const NamespaceString& from, const NamespaceString& to) {
    return BSON(
        "command"
        << "rename"
        << "from" << NamespaceStringUtil::serialize(from, SerializationContext::stateDefault())
        << "to" << NamespaceStringUtil::serialize(to, SerializationContext::stateDefault()));
}

void runTransactionOnShardingCatalog(OperationContext* opCtx,
                                     txn_api::Callback&& transactionChain,
                                     const WriteConcernOptions& writeConcern,
                                     const OperationSessionInfo& osi,
                                     bool useClusterTransaction,
                                     const std::shared_ptr<executor::TaskExecutor>& inputExecutor) {
    // The Internal Transactions API receives the write concern option and osi through the
    // passed Operation context. We opt for creating a new one to avoid any possible side
    // effects.
    auto newClient = [&]() {
        if (auto service = opCtx->getServiceContext()->getService(ClusterRole::RouterServer)) {
            return service->makeClient("ShardingCatalogTransaction");
        }
        return opCtx->getServiceContext()
            ->getService(ClusterRole::ShardServer)
            ->makeClient("ShardingCatalogTransaction");
    }();

    AuthorizationSession::get(newClient.get())->grantInternalAuthorization(newClient.get());
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
        if (!useClusterTransaction) {
            tassert(7591900,
                    "Can only use local transaction client for sharding catalog operations on a "
                    "config server",
                    serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));
            return nullptr;
        }

        auto sleepInlineExecutor = inlineExecutor->getSleepableExecutor(executor);
        return std::make_unique<txn_api::details::SEPTransactionClient>(
            newOpCtx,
            inlineExecutor,
            sleepInlineExecutor,
            executor,
            std::make_unique<txn_api::details::ClusterSEPTransactionClientBehaviors>(newOpCtx));
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
        ChunkType::ConfigNS);
    deleteChunkIfTheCollectionExistsAsUnsplittable.setDeletes({[&] {
        auto deleteQuery = BSON(ChunkType::collectionUUID.name() << uuid);
        return write_ops::DeleteOpEntry{std::move(deleteQuery), false /* multi */};
    }()});

    write_ops::InsertCommandRequest insertChunks(ChunkType::ConfigNS, std::move(chunkEntries));
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

std::vector<BatchedCommandRequest> getOperationsToCreateUnsplittableCollectionOnShardingCatalog(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const UUID& collectionUuid,
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

    return getOperationsToCreateOrShardCollectionOnShardingCatalog(
        coll, initialChunks.chunks, placementVersion, {shardId});
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

    // This always runs in the shard role so should use a cluster transaction to guarantee targeting
    // the config server.
    bool useClusterTransaction = true;
    sharding_ddl_util::runTransactionOnShardingCatalog(
        opCtx, std::move(transactionChain), wc, osi, useClusterTransaction, executor);
}

}  // namespace sharding_ddl_util
}  // namespace mongo
