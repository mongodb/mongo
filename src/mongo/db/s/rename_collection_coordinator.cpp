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


#include <absl/container/node_hash_map.h>
#include <algorithm>
#include <boost/none.hpp>
#include <boost/smart_ptr.hpp>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/bson/bson_field.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/collection_uuid_mismatch.h"
#include "mongo/db/catalog/rename_collection.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/database_name.h"
#include "mongo/db/list_collections_gen.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/ops/write_ops_parsers.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/query/distinct_command_gen.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/s/forwardable_operation_metadata.h"
#include "mongo/db/s/rename_collection_coordinator.h"
#include "mongo/db/s/sharded_index_catalog_commands_gen.h"
#include "mongo/db/s/sharding_ddl_coordinator.h"
#include "mongo/db/s/sharding_ddl_util.h"
#include "mongo/db/s/sharding_logging.h"
#include "mongo/db/s/sharding_recovery_service.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/shard_id.h"
#include "mongo/db/transaction/transaction_api.h"
#include "mongo/db/vector_clock.h"
#include "mongo/db/vector_clock_mutable.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/async_rpc.h"
#include "mongo/executor/async_rpc_util.h"
#include "mongo/executor/task_executor.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_component.h"
#include "mongo/s/analyze_shard_key_documents_gen.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_collection_gen.h"
#include "mongo/s/catalog/type_index_catalog_gen.h"
#include "mongo/s/catalog/type_namespace_placement_gen.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/sharded_ddl_commands_gen.h"
#include "mongo/s/router_role.h"
#include "mongo/s/shard_version.h"
#include "mongo/s/sharding_feature_flags_gen.h"
#include "mongo/s/sharding_state.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

// TODO (SERVER-80704): Get rid of isCollectionSharded function once targetIsSharded field is
// deprecated.
bool isCollectionSharded(boost::optional<CollectionType> const& optCollectionType) {
    if (!optCollectionType.has_value()) {
        return false;
    }
    const bool isUnsplittable = optCollectionType->getUnsplittable().value_or(false);
    return !isUnsplittable;
}

boost::optional<UUID> getCollectionUUID(OperationContext* opCtx,
                                        NamespaceString const& nss,
                                        boost::optional<CollectionType> const& optCollectionType,
                                        bool throwOnNotFound = true) {
    if (optCollectionType) {
        return optCollectionType->getUuid();
    }
    Lock::DBLock dbLock(opCtx, nss.dbName(), MODE_IS);
    Lock::CollectionLock collLock(opCtx, nss, MODE_IS);
    const auto collPtr = CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss);
    if (!collPtr && !throwOnNotFound) {
        return boost::none;
    }

    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "Collection " << nss.toStringForErrorMsg() << " doesn't exist.",
            collPtr);

    return collPtr->uuid();
}

/**
 * Checks that both collections are part of the same database when the source collection is sharded
 * or must have the same database primary shard when the source collection is unsharded.
 */
void checkDatabaseRestrictions(OperationContext* opCtx,
                               const NamespaceString& fromNss,
                               const boost::optional<CollectionType>& fromCollType,
                               const NamespaceString& toNss) {
    if (!fromCollType || fromCollType->getUnsplittable().value_or(false)) {
        // TODO (SERVER-84243): Replace with the dedicated cache for filtering information and avoid
        // to refresh.
        const auto toDB = uassertStatusOK(
            Grid::get(opCtx)->catalogCache()->getDatabaseWithRefresh(opCtx, toNss.dbName()));

        uassert(ErrorCodes::CommandFailed,
                "Source and destination collections must be on same shard",
                ShardingState::get(opCtx)->shardId() == toDB->getPrimary());
    } else {
        uassert(ErrorCodes::CommandFailed,
                str::stream() << "Source and destination collections must be on "
                                 "the same database because "
                              << fromNss.toStringForErrorMsg() << " is sharded.",
                fromNss.db_forSharding() == toNss.db_forSharding());
    }
}

/**
 * Checks that the collection UUID is the same in every shard knowing the collection.
 */
void checkCollectionUUIDConsistencyAcrossShards(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const UUID& collectionUuid,
    const std::vector<mongo::ShardId>& shardIds,
    std::shared_ptr<executor::ScopedTaskExecutor> executor) {
    const BSONObj filterObj = BSON("name" << nss.coll());
    ListCollections command;
    command.setFilter(filterObj);
    command.setDbName(nss.dbName());
    auto opts = std::make_shared<async_rpc::AsyncRPCOptions<ListCollections>>(
        **executor, CancellationToken::uncancelable(), command);
    auto responses = sharding_ddl_util::sendAuthenticatedCommandToShards(opCtx, opts, shardIds);

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
        errorMessage << "The collection " << nss.toStringForErrorMsg()
                     << " with expected UUID: " << collectionUuid.toString()
                     << " has different UUIDs on the following shards: [";

        for (const auto& mismatch : mismatches) {
            errorMessage << "{ " << mismatch.shardId << ":" << mismatch.uuid << " },";
        }
        errorMessage << "]";
        uasserted(ErrorCodes::InvalidUUID, errorMessage.str());
    }
}

/**
 * Checks that the collection does not exist in any shard when `dropTarget` is set to false.
 */
void checkTargetCollectionDoesNotExistInCluster(
    OperationContext* opCtx,
    const NamespaceString& toNss,
    const std::vector<mongo::ShardId>& shardIds,
    std::shared_ptr<executor::ScopedTaskExecutor> executor) {
    const BSONObj filterObj = BSON("name" << toNss.coll());
    ListCollections command;
    command.setFilter(filterObj);
    command.setDbName(toNss.dbName());
    auto opts = std::make_shared<async_rpc::AsyncRPCOptions<ListCollections>>(
        **executor, CancellationToken::uncancelable(), command);
    auto responses = sharding_ddl_util::sendAuthenticatedCommandToShards(opCtx, opts, shardIds);

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
        errorMessage << "The collection " << toNss.toStringForErrorMsg()
                     << " already exists in the following shards: [";
        std::move(shardsContainingTargetCollection.begin(),
                  shardsContainingTargetCollection.end(),
                  std::ostream_iterator<std::string>(errorMessage, ", "));
        errorMessage << "]";
        uasserted(ErrorCodes::NamespaceExists, errorMessage.str());
    }
}

/**
 * Ensures that 1) the source collection UUID is consistent on every shard and 2) the target
 * collection is not present on any shard when `dropTarget` is false.
 */
void checkCatalogConsistencyAcrossShards(OperationContext* opCtx,
                                         const NamespaceString& fromNss,
                                         const boost::optional<CollectionType>& fromCollType,
                                         const NamespaceString& toNss,
                                         const bool dropTarget,
                                         std::shared_ptr<executor::ScopedTaskExecutor> executor) {

    auto participants = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);

    auto sourceCollUuid = *getCollectionUUID(opCtx, fromNss, fromCollType);
    checkCollectionUUIDConsistencyAcrossShards(
        opCtx, fromNss, sourceCollUuid, participants, executor);

    if (!dropTarget) {
        checkTargetCollectionDoesNotExistInCluster(opCtx, toNss, participants, executor);
    }
}

void renameIndexMetadataInShards(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 const RenameCollectionRequest& request,
                                 const OperationSessionInfo& osi,
                                 const std::shared_ptr<executor::TaskExecutor>& executor,
                                 RenameCollectionCoordinatorDocument* doc,
                                 const CancellationToken& token) {
    const auto [configTime, newIndexVersion] = [opCtx]() -> std::pair<LogicalTime, Timestamp> {
        VectorClock::VectorTime vt = VectorClock::get(opCtx)->getTime();
        return {vt.configTime(), vt.clusterTime().asTimestamp()};
    }();

    // Bump the index version only if there are indexes in the source collection.
    auto optTrackedCollInfo = doc->getOptTrackedCollInfo();
    if (optTrackedCollInfo && optTrackedCollInfo->getIndexVersion()) {
        // Bump sharding catalog's index version on the config server if the source collection is
        // sharded. It will be updated later on.
        optTrackedCollInfo->setIndexVersion(
            {doc->getNewTargetCollectionUuid().get_value_or(optTrackedCollInfo->getUuid()),
             newIndexVersion});
        doc->setOptTrackedCollInfo(optTrackedCollInfo);
    }

    // Update global index metadata in shards.
    auto& toNss = request.getTo();

    auto participants = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
    ShardsvrRenameIndexMetadata renameIndexCatalogReq(
        nss,
        toNss,
        {doc->getNewTargetCollectionUuid().get_value_or(doc->getSourceUUID().value()),
         newIndexVersion});
    renameIndexCatalogReq.setDbName(toNss.dbName());
    async_rpc::GenericArgs args;
    async_rpc::AsyncRPCCommandHelpers::appendMajorityWriteConcern(args);
    async_rpc::AsyncRPCCommandHelpers::appendOSI(args, osi);
    auto opts = std::make_shared<async_rpc::AsyncRPCOptions<ShardsvrRenameIndexMetadata>>(
        executor, token, renameIndexCatalogReq, args);
    sharding_ddl_util::sendAuthenticatedCommandToShards(opCtx, opts, participants);
}

std::vector<ShardId> getLatestCollectionPlacementInfoFor(OperationContext* opCtx,
                                                         const NamespaceString& nss,
                                                         const UUID& uuid) {
    // Use the content of config.chunks to obtain the placement of the collection being renamed.
    // The request is equivalent to 'configDb.chunks.distinct("shard", {uuid:collectionUuid})'.
    auto query = BSON(NamespacePlacementType::kNssFieldName
                      << NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault()));

    auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();


    DistinctCommandRequest distinctRequest(ChunkType::ConfigNS);
    distinctRequest.setKey(ChunkType::shard.name());
    distinctRequest.setQuery(BSON(ChunkType::collectionUUID.name() << uuid));
    auto rc = BSON(repl::ReadConcernArgs::kReadConcernFieldName << repl::ReadConcernArgs::kLocal);

    auto reply = uassertStatusOK(configShard->runCommandWithFixedRetryAttempts(
        opCtx,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly, TagSet{}),
        DatabaseName::kConfig,
        distinctRequest.toBSON({rc}),
        Shard::RetryPolicy::kIdempotent));

    uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(reply));
    std::vector<ShardId> shardIds;
    for (const auto& valueElement : reply.response.getField("values").Array()) {
        shardIds.emplace_back(valueElement.String());
    }

    return shardIds;
}

SemiFuture<BatchedCommandResponse> noOpStatement() {
    BatchedCommandResponse noOpResponse;
    noOpResponse.setStatus(Status::OK());
    noOpResponse.setN(0);
    return SemiFuture<BatchedCommandResponse>(std::move(noOpResponse));
}

SemiFuture<BatchedCommandResponse> deleteTrackedCollectionStatement(
    const txn_api::TransactionClient& txnClient,
    const NamespaceString& nss,
    const boost::optional<UUID>& uuid,
    int stmtId) {

    if (uuid) {
        const auto deleteCollectionQuery =
            BSON(CollectionType::kNssFieldName
                 << NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault())
                 << CollectionType::kUuidFieldName << *uuid);

        write_ops::DeleteCommandRequest deleteOp(CollectionType::ConfigNS);
        deleteOp.setDeletes({[&]() {
            write_ops::DeleteOpEntry entry;
            entry.setMulti(false);
            entry.setQ(deleteCollectionQuery);
            return entry;
        }()});

        return txnClient.runCRUDOp(deleteOp, {stmtId});
    } else {
        return noOpStatement();
    }
}

SemiFuture<BatchedCommandResponse> renameTrackedCollectionStatement(
    const txn_api::TransactionClient& txnClient,
    const CollectionType& oldCollection,
    const NamespaceString& newNss,
    const boost::optional<UUID>& newTargetCollectionUuid,
    const Timestamp& timeInsertion,
    int stmtId) {
    auto newCollectionType = oldCollection;
    newCollectionType.setNss(newNss);
    newCollectionType.setTimestamp(timeInsertion);
    newCollectionType.setEpoch(OID::gen());
    if (newTargetCollectionUuid.has_value()) {
        newCollectionType.setUuid(newTargetCollectionUuid.get());
    }

    // Implemented as an upsert to be idempotent
    auto query = BSON(CollectionType::kNssFieldName << NamespaceStringUtil::serialize(
                          newNss, SerializationContext::stateDefault()));
    write_ops::UpdateCommandRequest updateOp(CollectionType::ConfigNS);
    updateOp.setUpdates({[&] {
        write_ops::UpdateOpEntry entry;
        entry.setQ(query);
        entry.setU(
            write_ops::UpdateModification::parseFromClassicUpdate(newCollectionType.toBSON()));
        entry.setUpsert(true);
        entry.setMulti(false);
        return entry;
    }()});

    return txnClient.runCRUDOp(updateOp, {stmtId} /*stmtIds*/);
}

SemiFuture<BatchedCommandResponse> updateChunksUuid(
    const txn_api::TransactionClient& txnClient,
    const CollectionType& oldCollection,
    const boost::optional<UUID>& newTargetCollectionUuid) {
    if (newTargetCollectionUuid.has_value() &&
        newTargetCollectionUuid.get() != oldCollection.getUuid()) {
        const auto query = BSON(ChunkType::collectionUUID() << oldCollection.getUuid());
        const auto update =
            BSON("$set" << BSON(ChunkType::collectionUUID() << *newTargetCollectionUuid));

        // This query is expected to target unsplittable collections with one chunk.
        // Don't use this for updating a high amount of chunks because the transaction
        // may abort due to hitting the `transactionLifetimeLimitSeconds`.
        BatchedCommandRequest request([&] {
            write_ops::UpdateCommandRequest updateOp(ChunkType::ConfigNS);
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

        return txnClient.runCRUDOp(request, {-1} /*stmtIds*/);
    }

    return noOpStatement();
}

SemiFuture<BatchedCommandResponse> insertToPlacementHistoryStatement(
    const txn_api::TransactionClient& txnClient,
    const NamespaceString& nss,
    const boost::optional<UUID>& uuid,
    const Timestamp& clusterTime,
    const std::vector<ShardId>& shards,
    int stmtId,
    const BatchedCommandResponse& previousOperationResult) {

    // Skip the insertion of the placement entry if the previous statement didn't change any
    // document - we can deduce that the whole transaction was already committed in a previous
    // attempt.
    if (previousOperationResult.getN() == 0) {
        return noOpStatement();
    }

    NamespacePlacementType placementInfo(NamespaceString(nss), clusterTime, shards);
    if (uuid)
        placementInfo.setUuid(*uuid);
    write_ops::InsertCommandRequest insertPlacementEntry(
        NamespaceString::kConfigsvrPlacementHistoryNamespace, {placementInfo.toBSON()});

    return txnClient.runCRUDOp(insertPlacementEntry, {stmtId} /*stmtIds*/);
}


SemiFuture<BatchedCommandResponse> updateZonesStatement(const txn_api::TransactionClient& txnClient,
                                                        const NamespaceString& oldNss,
                                                        const NamespaceString& newNss) {

    const auto query = BSON(
        TagsType::ns(NamespaceStringUtil::serialize(oldNss, SerializationContext::stateDefault())));
    const auto update = BSON("$set" << BSON(TagsType::ns(NamespaceStringUtil::serialize(
                                 newNss, SerializationContext::stateDefault()))));

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
    return txnClient.runCRUDOp(request, {-1} /*stmtIds*/);
}

SemiFuture<BatchedCommandResponse> deleteZonesStatement(const txn_api::TransactionClient& txnClient,
                                                        const NamespaceString& nss) {

    const auto query = BSON(
        TagsType::ns(NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault())));
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

    return txnClient.runCRUDOp(request, {-1});
}

SemiFuture<BatchedCommandResponse> deleteShardingIndexCatalogMetadataStatement(
    const txn_api::TransactionClient& txnClient, const boost::optional<UUID>& uuid) {
    if (uuid) {
        // delete index catalog metadata
        BatchedCommandRequest request([&] {
            write_ops::DeleteCommandRequest deleteOp(
                NamespaceString::kConfigsvrIndexCatalogNamespace);
            deleteOp.setDeletes({[&] {
                write_ops::DeleteOpEntry entry;
                entry.setQ(BSON(IndexCatalogType::kCollectionUUIDFieldName << *uuid));
                entry.setMulti(true);
                return entry;
            }()});
            return deleteOp;
        }());

        return txnClient.runCRUDOp(request, {-1});
    } else {
        return noOpStatement();
    }
}


void renameCollectionMetadataInTransaction(OperationContext* opCtx,
                                           const boost::optional<CollectionType>& optFromCollType,
                                           const NamespaceString& fromNss,
                                           const NamespaceString& toNss,
                                           const boost::optional<UUID>& droppedTargetUUID,
                                           const boost::optional<UUID>& newTargetCollectionUuid,
                                           const WriteConcernOptions& writeConcern,
                                           const std::shared_ptr<executor::TaskExecutor>& executor,
                                           const OperationSessionInfo& osi) {

    std::string logMsg = str::stream()
        << toStringForLogging(fromNss) << " to " << toStringForLogging(toNss);
    if (optFromCollType) {
        // Case FROM collection is tracked by the config server
        auto fromUUID = optFromCollType->getUuid();

        // Every statement in the transaction runs under the same clusterTime. To ensure in the
        // placementHistory the drop of the target will appear earlier then the insert of the target
        // we forcely add a tick to have 2 valid timestamp that we can use to differentiate the 2
        // operations.
        auto now = VectorClock::get(opCtx)->getTime();
        auto nowClusterTime = now.clusterTime();
        auto timeDrop = nowClusterTime.asTimestamp();

        nowClusterTime.addTicks(1);
        auto timeInsert = nowClusterTime.asTimestamp();

        // Retrieve the latest placement information about "FROM".
        auto fromNssShards = getLatestCollectionPlacementInfoFor(opCtx, fromNss, fromUUID);

        auto transactionChain = [&](const txn_api::TransactionClient& txnClient,
                                    ExecutorPtr txnExec) {
            // Remove config.collection entry. Query by 'ns' AND 'uuid' so that the remove can be
            // resolved with an IXSCAN (thanks to the index on '_id') and is idempotent (thanks to
            // the 'uuid') delete TO collection if exists.
            return deleteTrackedCollectionStatement(txnClient, toNss, droppedTargetUUID, 1)
                .thenRunOn(txnExec)
                .then([&](const BatchedCommandResponse& deleteCollResponse) {
                    uassertStatusOK(deleteCollResponse.toStatus());

                    return insertToPlacementHistoryStatement(txnClient,
                                                             toNss,
                                                             droppedTargetUUID,
                                                             timeDrop,
                                                             {} /*shards*/,
                                                             2,
                                                             deleteCollResponse);
                })
                .thenRunOn(txnExec)
                .then([&](const BatchedCommandResponse& response) {
                    uassertStatusOK(response.toStatus());

                    return deleteShardingIndexCatalogMetadataStatement(txnClient,
                                                                       droppedTargetUUID);
                })
                // Delete "FROM" collection
                .thenRunOn(txnExec)
                .then([&](const BatchedCommandResponse& response) {
                    uassertStatusOK(response.toStatus());
                    return deleteTrackedCollectionStatement(txnClient, fromNss, fromUUID, 3);
                })
                .thenRunOn(txnExec)
                .then([&](const BatchedCommandResponse& deleteCollResponse) {
                    uassertStatusOK(deleteCollResponse.toStatus());

                    return insertToPlacementHistoryStatement(txnClient,
                                                             fromNss,
                                                             fromUUID,
                                                             timeDrop,
                                                             {} /*shards*/,
                                                             4,
                                                             deleteCollResponse);
                })
                .thenRunOn(txnExec)
                .then([&](const BatchedCommandResponse& deleteCollResponse) {
                    uassertStatusOK(deleteCollResponse.toStatus());
                    // Use the modified entries to insert collection and placement entries for "TO".
                    return renameTrackedCollectionStatement(
                        txnClient, *optFromCollType, toNss, newTargetCollectionUuid, timeInsert, 5);
                })
                .thenRunOn(txnExec)
                .then([&](const BatchedCommandResponse& upsertCollResponse) {
                    uassertStatusOK(upsertCollResponse.toStatus());

                    return insertToPlacementHistoryStatement(
                        txnClient,
                        toNss,
                        newTargetCollectionUuid.get_value_or(fromUUID),
                        timeInsert,
                        fromNssShards,
                        6,
                        upsertCollResponse);
                })
                // update tags and check it was successful
                .thenRunOn(txnExec)
                .then([&](const BatchedCommandResponse& insertCollResponse) {
                    uassertStatusOK(insertCollResponse.toStatus());

                    return updateZonesStatement(txnClient, fromNss, toNss);
                })
                .thenRunOn(txnExec)
                .then([&](const BatchedCommandResponse& response) {
                    uassertStatusOK(response.toStatus());
                    return updateChunksUuid(txnClient, *optFromCollType, newTargetCollectionUuid);
                })
                .thenRunOn(txnExec)
                .then([&](const BatchedCommandResponse& updateChunksResponse) {
                    uassertStatusOK(updateChunksResponse.toStatus());
                    // Make sure the chunks update query must target unsplittable collections with
                    // one chunk.
                    dassert(updateChunksResponse.getN() <= 1);
                })
                .semi();
        };
        const bool useClusterTransaction = true;
        sharding_ddl_util::runTransactionOnShardingCatalog(
            opCtx, std::move(transactionChain), writeConcern, osi, useClusterTransaction, executor);

        ShardingLogging::get(opCtx)->logChange(
            opCtx,
            str::stream() << logMsg << ": dropped target collection and renamed source collection",
            NamespaceStringUtil::deserialize(
                boost::none, "renameCollection.metadata", SerializationContext::stateDefault()),
            BSON("newCollMetadata" << optFromCollType->toBSON()),
            ShardingCatalogClient::kMajorityWriteConcern,
            Grid::get(opCtx)->shardRegistry()->getConfigShard(),
            Grid::get(opCtx)->catalogClient());
    } else {
        // Case FROM collection is not tracked by the config server: just delete the target
        // collection if it was registered in the CSRS
        auto now = VectorClock::get(opCtx)->getTime();
        auto newTimestamp = now.clusterTime().asTimestamp();

        auto transactionChain = [&](const txn_api::TransactionClient& txnClient,
                                    ExecutorPtr txnExec) {
            return deleteTrackedCollectionStatement(txnClient, toNss, droppedTargetUUID, 1)
                .thenRunOn(txnExec)
                .then([&](const BatchedCommandResponse& deleteCollResponse) {
                    uassertStatusOK(deleteCollResponse.toStatus());
                    return insertToPlacementHistoryStatement(txnClient,
                                                             toNss,
                                                             droppedTargetUUID,
                                                             newTimestamp,
                                                             {},
                                                             2,
                                                             deleteCollResponse);
                })
                .thenRunOn(txnExec)
                .then([&](const BatchedCommandResponse& response) {
                    uassertStatusOK(response.toStatus());

                    return deleteShardingIndexCatalogMetadataStatement(txnClient,
                                                                       droppedTargetUUID);
                })
                .thenRunOn(txnExec)
                .then([&](const BatchedCommandResponse& response) {
                    uassertStatusOK(response.toStatus());

                    return deleteZonesStatement(txnClient, toNss);
                })
                .thenRunOn(txnExec)
                .then([&](const BatchedCommandResponse& response) {
                    uassertStatusOK(response.toStatus());
                })
                .semi();
        };

        const bool useClusterTransaction = true;
        sharding_ddl_util::runTransactionOnShardingCatalog(
            opCtx, std::move(transactionChain), writeConcern, osi, useClusterTransaction, executor);

        ShardingLogging::get(opCtx)->logChange(
            opCtx,
            str::stream() << logMsg << " : dropped target collection.",
            NamespaceStringUtil::deserialize(
                boost::none, "renameCollection.metadata", SerializationContext::stateDefault()),
            BSONObj(),
            ShardingCatalogClient::kMajorityWriteConcern,
            Grid::get(opCtx)->shardRegistry()->getConfigShard(),
            Grid::get(opCtx)->catalogClient());
    }
}

void checkExpectedTargetCollectionOptionsMatch(OperationContext* opCtx,
                                               const NamespaceString targetNss,
                                               const BSONObj& expectedOptions) {
    const auto collectionOptions = [&]() {
        // Collection options can be read from the local shard even if it doesn't own any chunks,
        // because the dbPrimary shard is kept consistent with the data-owning chunks.
        AutoGetCollection coll(opCtx, targetNss, MODE_IS);
        return coll ? coll->getCollectionOptions().toBSON() : BSONObj();
    }();

    uassertStatusOK(
        checkTargetCollectionOptionsMatch(targetNss, expectedOptions, collectionOptions));
}

void checkExpectedTargetIndexesMatch(OperationContext* opCtx,
                                     const NamespaceString targetNss,
                                     const std::vector<BSONObj>& expectedIndexes) {
    sharding::router::CollectionRouter router(opCtx->getServiceContext(), targetNss);
    const auto currentIndexes =
        router.route(opCtx,
                     "checking indexes prerequisites within rename collection coordinator",
                     [&](OperationContext* opCtx, const CollectionRoutingInfo& cri) {
                         const auto response =
                             loadIndexesFromAuthoritativeShard(opCtx, targetNss, cri);
                         if (response.getStatus() == ErrorCodes::NamespaceNotFound) {
                             // Collection does not exist. Consider as no index exist.
                             return std::vector<BSONObj>();
                         }
                         return uassertStatusOK(response).docs;
                     });
    uassertStatusOK(checkTargetCollectionIndexesMatch(
        targetNss,
        std::list<BSONObj>{expectedIndexes.begin(), expectedIndexes.end()},
        std::list<BSONObj>{currentIndexes.begin(), currentIndexes.end()}));
}
}  // namespace

RenameCollectionCoordinator::RenameCollectionCoordinator(ShardingDDLCoordinatorService* service,
                                                         const BSONObj& initialState)
    : RecoverableShardingDDLCoordinator(service, "RenameCollectionCoordinator", initialState),
      _request(_doc.getRenameCollectionRequest()) {}

void RenameCollectionCoordinator::checkIfOptionsConflict(const BSONObj& doc) const {
    const auto otherDoc = RenameCollectionCoordinatorDocument::parse(
        IDLParserContext("RenameCollectionCoordinatorDocument"), doc);

    const auto& selfReq = _request.toBSON();
    const auto& otherReq = otherDoc.getRenameCollectionRequest().toBSON();

    uassert(ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Another rename collection for namespace "
                          << originalNss().toStringForErrorMsg()
                          << " is being executed with different parameters: " << selfReq,
            SimpleBSONObjComparator::kInstance.evaluate(selfReq == otherReq));
}

logv2::DynamicAttributes RenameCollectionCoordinator::getCoordinatorLogAttrs() const {
    return logv2::DynamicAttributes{getBasicCoordinatorAttrs(),
                                    "destinationNamespace"_attr = _request.getTo()};
}

std::set<NamespaceString> RenameCollectionCoordinator::_getAdditionalLocksToAcquire(
    OperationContext* opCtx) {
    return {_request.getTo()};
}

void RenameCollectionCoordinator::appendCommandInfo(BSONObjBuilder* cmdInfoBuilder) const {
    cmdInfoBuilder->appendElements(_request.toBSON());
}

ExecutorFuture<void> RenameCollectionCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then([this, executor = executor, anchor = shared_from_this()]() {
            // Check expected target collection indexes, if necessary.
            // Done only before having advanced into or past the kCheckPreconditions phase. It
            // cannot be done within the kCheckPreconditions phase because that phase takes the
            // critical section on the destination namespace, which makes it impossible to send
            // a versioned command to the participant shards to get the current indexes.
            if (_doc.getPhase() < Phase::kCheckPreconditions &&
                _doc.getRenameCollectionRequest().getExpectedIndexes()) {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                getForwardableOpMetadata().setOn(opCtx);
                checkExpectedTargetIndexesMatch(
                    opCtx,
                    _request.getTo(),
                    *_doc.getRenameCollectionRequest().getExpectedIndexes());
            }
        })
        .then(_buildPhaseHandler(
            Phase::kCheckPreconditions,
            [this, executor = executor, anchor = shared_from_this()] {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                getForwardableOpMetadata().setOn(opCtx);

                const auto& fromNss = nss();
                const auto& toNss = _request.getTo();

                const auto criticalSectionReason =
                    sharding_ddl_util::getCriticalSectionReasonForRename(fromNss, toNss);

                try {
                    uassert(ErrorCodes::InvalidOptions,
                            "Cannot provide an expected collection UUID when renaming between "
                            "databases",
                            fromNss.db_forSharding() == toNss.db_forSharding() ||
                                (!_doc.getExpectedSourceUUID() && !_doc.getExpectedTargetUUID()));

                    {
                        AutoGetCollection coll{
                            opCtx,
                            fromNss,
                            MODE_IS,
                            AutoGetCollection::Options{}
                                .viewMode(auto_get_collection::ViewMode::kViewsPermitted)
                                .expectedUUID(_doc.getExpectedSourceUUID())};

                        uassert(ErrorCodes::CommandNotSupportedOnView,
                                str::stream()
                                    << "Can't rename source collection `"
                                    << fromNss.toStringForErrorMsg() << "` because it is a view.",
                                !CollectionCatalog::get(opCtx)->lookupView(opCtx, fromNss));

                        checkCollectionUUIDMismatch(
                            opCtx, fromNss, *coll, _doc.getExpectedSourceUUID());

                        uassert(ErrorCodes::NamespaceNotFound,
                                str::stream() << "Collection " << fromNss.toStringForErrorMsg()
                                              << " doesn't exist.",
                                coll.getCollection());

                        uassert(ErrorCodes::IllegalOperation,
                                "Cannot rename an encrypted collection",
                                !coll || !coll->getCollectionOptions().encryptedFieldConfig ||
                                    _doc.getAllowEncryptedCollectionRename().value_or(false));
                    }

                    // Make sure the source collection exists
                    const auto optSourceCollType =
                        sharding_ddl_util::getCollectionFromConfigServer(opCtx, fromNss);
                    const auto sourceCollUuid =
                        getCollectionUUID(opCtx, fromNss, optSourceCollType);
                    _doc.setSourceUUID(sourceCollUuid);
                    _doc.setOptTrackedCollInfo(optSourceCollType);

                    const auto optTargetCollType =
                        sharding_ddl_util::getCollectionFromConfigServer(opCtx, toNss);
                    _doc.setTargetUUID(getCollectionUUID(
                        opCtx, toNss, optTargetCollType, /*throwNotFound*/ false));

                    // TODO (SERVER-80704): Get rid of targetIsSharded and optShardedCollInfo fields
                    // once v8.0 branches out
                    _doc.setTargetIsSharded(isCollectionSharded(optTargetCollType));
                    _doc.setOptShardedCollInfo(optSourceCollType);

                    if (fromNss.db_forSharding() != toNss.db_forSharding()) {
                        // Renaming across databases will result in a new UUID that is generated by
                        // the coordinator and will be propagated to the participants.
                        _doc.setNewTargetCollectionUuid(UUID::gen());
                    } else {
                        _doc.setNewTargetCollectionUuid(sourceCollUuid);
                    }

                    if (!optTargetCollType) {
                        // (SERVER-67325) Acquire critical section on the target collection in order
                        // to disallow concurrent local `createCollection`. In case the collection
                        // does not exist, it will be later released by the rename participant. In
                        // the collection exists, the critical section can be released right away as
                        // the participant will re-acquire it when needed.
                        auto criticalSection = ShardingRecoveryService::get(opCtx);
                        criticalSection->acquireRecoverableCriticalSectionBlockWrites(
                            opCtx,
                            toNss,
                            criticalSectionReason,
                            ShardingCatalogClient::kLocalWriteConcern);
                        criticalSection->promoteRecoverableCriticalSectionToBlockAlsoReads(
                            opCtx,
                            toNss,
                            criticalSectionReason,
                            ShardingCatalogClient::kLocalWriteConcern);

                        if (CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx,
                                                                                       toNss)) {
                            // Release the critical section because the untracked target collection
                            // already exists, hence no risk of concurrent `createCollection`
                            criticalSection->releaseRecoverableCriticalSection(
                                opCtx,
                                toNss,
                                criticalSectionReason,
                                WriteConcerns::kLocalWriteConcern);
                        }
                    }

                    sharding_ddl_util::checkRenamePreconditions(
                        opCtx, toNss, optTargetCollType, _doc.getDropTarget());

                    checkDatabaseRestrictions(opCtx, fromNss, optSourceCollType, toNss);

                    checkCatalogConsistencyAcrossShards(
                        opCtx, fromNss, optSourceCollType, toNss, _doc.getDropTarget(), executor);

                    // Check that the target collection is not sharded, if requested.
                    if (_doc.getRenameCollectionRequest().getTargetMustNotBeSharded().get_value_or(
                            false)) {
                        uassert(ErrorCodes::IllegalOperation,
                                str::stream() << "cannot rename to sharded collection '"
                                              << toNss.toStringForErrorMsg() << "'",
                                !_doc.getTargetIsSharded());
                    }

                    // Check expected target collection options, if necessary.
                    if (_doc.getRenameCollectionRequest().getExpectedCollectionOptions()) {
                        checkExpectedTargetCollectionOptionsMatch(
                            opCtx,
                            toNss,
                            *_doc.getRenameCollectionRequest().getExpectedCollectionOptions());
                    }

                    {
                        AutoGetCollection coll{
                            opCtx,
                            toNss,
                            MODE_IS,
                            AutoGetCollection::Options{}
                                .viewMode(auto_get_collection::ViewMode::kViewsPermitted)
                                .expectedUUID(_doc.getExpectedTargetUUID())};

                        uassert(ErrorCodes::NamespaceExists,
                                str::stream() << "a view already exists with that name: "
                                              << toNss.toStringForErrorMsg(),
                                !CollectionCatalog::get(opCtx)->lookupView(opCtx, toNss));

                        uassert(ErrorCodes::IllegalOperation,
                                "Cannot rename to an existing encrypted collection",
                                !coll || !coll->getCollectionOptions().encryptedFieldConfig ||
                                    _doc.getAllowEncryptedCollectionRename().value_or(false));
                    }

                } catch (const DBException&) {
                    auto criticalSection = ShardingRecoveryService::get(opCtx);
                    criticalSection->releaseRecoverableCriticalSection(
                        opCtx,
                        toNss,
                        criticalSectionReason,
                        WriteConcerns::kLocalWriteConcern,
                        false /* throwIfReasonDiffers */);
                    _completeOnError = true;
                    throw;
                }
            }))
        .then(_buildPhaseHandler(
            Phase::kFreezeMigrations,
            [this, executor = executor, anchor = shared_from_this()] {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                getForwardableOpMetadata().setOn(opCtx);

                const auto& fromNss = nss();
                const auto& toNss = _request.getTo();

                _updateNewOptTrackedCollInfoFieldAfterBinaryUpgrade();

                ShardingLogging::get(opCtx)->logChange(
                    opCtx,
                    "renameCollection.start",
                    fromNss,
                    BSON("source" << NamespaceStringUtil::serialize(
                                         fromNss, SerializationContext::stateDefault())
                                  << "destination"
                                  << NamespaceStringUtil::serialize(
                                         toNss, SerializationContext::stateDefault())),
                    ShardingCatalogClient::kMajorityWriteConcern);

                // Block migrations on involved collections.
                try {
                    const auto& osi = getNewSession(opCtx);
                    sharding_ddl_util::stopMigrations(opCtx, fromNss, _doc.getSourceUUID(), osi);
                } catch (ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
                    // stopMigrations is allowed to fail when the source collection is not tracked
                    // by the sharding catalog.
                }

                try {
                    const auto& osi = getNewSession(opCtx);
                    sharding_ddl_util::stopMigrations(opCtx, toNss, _doc.getTargetUUID(), osi);
                } catch (ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
                    // stopMigrations is allowed to fail when the target collection doesn't exist or
                    // is not tracked by the sharding catalog.
                }
            }))
        .then(_buildPhaseHandler(
            Phase::kBlockCrudAndRename,
            [this, token, executor = executor, anchor = shared_from_this()] {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                getForwardableOpMetadata().setOn(opCtx);

                if (!_firstExecution) {
                    _performNoopRetryableWriteOnAllShardsAndConfigsvr(
                        opCtx, getNewSession(opCtx), **executor);
                }

                _updateNewOptTrackedCollInfoFieldAfterBinaryUpgrade();

                const auto& fromNss = nss();

                // On participant shards:
                // - Block CRUD on source and target collection in case at least one of such
                //   collections is currently tracked by the config server
                // - Locally drop the target collection
                // - Locally rename source to target
                ShardsvrRenameCollectionParticipant renameCollParticipantRequest(
                    fromNss, _doc.getSourceUUID().value());
                renameCollParticipantRequest.setDbName(fromNss.dbName());
                renameCollParticipantRequest.setTargetUUID(_doc.getTargetUUID());
                renameCollParticipantRequest.setNewTargetCollectionUuid(
                    _doc.getNewTargetCollectionUuid());
                renameCollParticipantRequest.setRenameCollectionRequest(_request);

                // We need to send the command to all the shards because both movePrimary and
                // moveChunk leave garbage behind for sharded collections. At the same time, the
                // primary shard needs to be last participant to perfom its local rename operation:
                // this will ensure that the op entries generated by the collections being
                // renamed/dropped will be generated at points in time where all shards have a
                // consistent view of the metadata and no concurrent writes are being performed.
                const auto primaryShardId = ShardingState::get(opCtx)->shardId();
                auto participants = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
                participants.erase(
                    std::remove(participants.begin(), participants.end(), primaryShardId),
                    participants.end());

                async_rpc::GenericArgs args;
                async_rpc::AsyncRPCCommandHelpers::appendMajorityWriteConcern(args);
                async_rpc::AsyncRPCCommandHelpers::appendOSI(args, getNewSession(opCtx));
                auto opts = std::make_shared<
                    async_rpc::AsyncRPCOptions<ShardsvrRenameCollectionParticipant>>(
                    **executor, token, renameCollParticipantRequest, args);
                sharding_ddl_util::sendAuthenticatedCommandToShards(opCtx, opts, participants);
                sharding_ddl_util::sendAuthenticatedCommandToShards(opCtx, opts, {primaryShardId});
            }))
        .then(_buildPhaseHandler(
            Phase::kRenameMetadata,
            [this, token, executor = executor, anchor = shared_from_this()] {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                getForwardableOpMetadata().setOn(opCtx);

                _updateNewOptTrackedCollInfoFieldAfterBinaryUpgrade();

                // Remove the query sampling configuration documents for the source and destination
                // collections, if they exist.
                sharding_ddl_util::removeQueryAnalyzerMetadataFromConfig(
                    opCtx,
                    BSON(analyze_shard_key::QueryAnalyzerDocument::kNsFieldName << BSON(
                             "$in" << BSON_ARRAY(
                                 NamespaceStringUtil::serialize(
                                     nss(), SerializationContext::stateDefault())
                                 << NamespaceStringUtil::serialize(
                                        _request.getTo(), SerializationContext::stateDefault())))));

                // For an untracked collection the CSRS server can not verify the targetUUID.
                // Use the session ID + txnNumber to ensure no stale requests get through.
                if (!_firstExecution) {
                    _performNoopRetryableWriteOnAllShardsAndConfigsvr(
                        opCtx, getNewSession(opCtx), **executor);
                }

                {
                    const auto& osi = getNewSession(opCtx);
                    renameIndexMetadataInShards(
                        opCtx, nss(), _request, osi, **executor, &_doc, token);
                }

                // Update the collection metadata after the rename.
                // Renaming the metadata will also resume migrations for the resulting collection.
                {
                    const auto& osi = getNewSession(opCtx);
                    renameCollectionMetadataInTransaction(
                        opCtx,
                        _doc.getOptTrackedCollInfo(),
                        nss(),
                        _request.getTo(),
                        _doc.getTargetUUID(),
                        _doc.getNewTargetCollectionUuid(),
                        ShardingCatalogClient::kMajorityWriteConcern,
                        **executor,
                        osi);
                }

                // Checkpoint the configTime to ensure that, in the case of a stepdown, the new
                // primary will start-up from a configTime that is inclusive of the renamed
                // metadata.
                VectorClockMutable::get(opCtx)->waitForDurableConfigTime().get(opCtx);
            }))
        .then(_buildPhaseHandler(
            Phase::kUnblockCRUD,
            [this, token, executor = executor, anchor = shared_from_this()] {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                getForwardableOpMetadata().setOn(opCtx);

                if (!_firstExecution) {
                    _performNoopRetryableWriteOnAllShardsAndConfigsvr(
                        opCtx, getNewSession(opCtx), **executor);
                }

                _updateNewOptTrackedCollInfoFieldAfterBinaryUpgrade();

                const auto& fromNss = nss();
                // On participant shards:
                // - Unblock CRUD on participants for both source and destination collections
                ShardsvrRenameCollectionUnblockParticipant unblockParticipantRequest(
                    fromNss, _doc.getSourceUUID().value());
                unblockParticipantRequest.setDbName(fromNss.dbName());
                unblockParticipantRequest.setRenameCollectionRequest(_request);
                auto participants = getAllShardsAndConfigServerIds(opCtx);

                async_rpc::GenericArgs args;
                async_rpc::AsyncRPCCommandHelpers::appendMajorityWriteConcern(args);
                async_rpc::AsyncRPCCommandHelpers::appendOSI(args, getNewSession(opCtx));
                auto opts = std::make_shared<
                    async_rpc::AsyncRPCOptions<ShardsvrRenameCollectionUnblockParticipant>>(
                    **executor, token, unblockParticipantRequest, args);
                sharding_ddl_util::sendAuthenticatedCommandToShards(opCtx, opts, participants);

                // Delete chunks belonging to the previous incarnation of the target collection.
                // This is performed after releasing the critical section in order to reduce stalls
                // and performed outside of a transaction to prevent timeout.
                auto targetUUID = _doc.getTargetUUID();
                if (targetUUID) {
                    auto query = BSON("uuid" << *targetUUID);
                    uassertStatusOK(Grid::get(opCtx)->catalogClient()->removeConfigDocuments(
                        opCtx,
                        ChunkType::ConfigNS,
                        query,
                        ShardingCatalogClient::kMajorityWriteConcern));
                }
            }))
        .then(_buildPhaseHandler(Phase::kSetResponse, [this, anchor = shared_from_this()] {
            auto opCtxHolder = cc().makeOperationContext();
            auto* opCtx = opCtxHolder.get();
            getForwardableOpMetadata().setOn(opCtx);

            _updateNewOptTrackedCollInfoFieldAfterBinaryUpgrade();

            // Retrieve the new collection version
            const auto catalog = Grid::get(opCtx)->catalogCache();
            const auto cri = uassertStatusOK(
                catalog->getCollectionRoutingInfoWithRefresh(opCtx, _request.getTo()));
            _response = RenameCollectionResponse(
                cri.cm.hasRoutingTable() ? cri.getCollectionVersion() : ShardVersion::UNSHARDED());

            ShardingLogging::get(opCtx)->logChange(
                opCtx,
                "renameCollection.end",
                nss(),
                BSON("source" << NamespaceStringUtil::serialize(
                                     nss(), SerializationContext::stateDefault())
                              << "destination"
                              << NamespaceStringUtil::serialize(
                                     _request.getTo(), SerializationContext::stateDefault())),
                ShardingCatalogClient::kMajorityWriteConcern);
            LOGV2(5460504, "Collection renamed", logAttrs(nss()));
        }));
}

// TODO (SERVER-80704): Get rid of this method once v8.0 branches out
void RenameCollectionCoordinator::_updateNewOptTrackedCollInfoFieldAfterBinaryUpgrade() {
    // `optTrackedCollInfo` is a new field added on v7.1, so we need to make sure it' set to the
    // proper value in case the rename operation started its execution on a previous binary version.
    if (!_doc.getOptTrackedCollInfo() && _doc.getOptShardedCollInfo()) {
        _doc.setOptTrackedCollInfo(_doc.getOptShardedCollInfo());
    }
}

}  // namespace mongo
