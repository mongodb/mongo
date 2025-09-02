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


#include "mongo/db/global_catalog/ddl/rename_collection_coordinator.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bson_field.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/client.h"
#include "mongo/db/database_name.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/ddl/notify_sharding_event_gen.h"
#include "mongo/db/global_catalog/ddl/sharded_ddl_commands_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_util.h"
#include "mongo/db/global_catalog/ddl/sharding_recovery_service.h"
#include "mongo/db/global_catalog/router_role_api/cluster_commands_helpers.h"
#include "mongo/db/global_catalog/router_role_api/router_role.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/global_catalog/type_collection_gen.h"
#include "mongo/db/global_catalog/type_namespace_placement_gen.h"
#include "mongo/db/global_catalog/type_tags.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/local_catalog/collection_uuid_mismatch.h"
#include "mongo/db/local_catalog/ddl/list_collections_gen.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/rename_collection.h"
#include "mongo/db/local_catalog/shard_role_catalog/participant_block_gen.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/query/distinct_command_gen.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/s/forwardable_operation_metadata.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/sharding_environment/sharding_logging.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/transaction/transaction_api.h"
#include "mongo/db/vector_clock/vector_clock.h"
#include "mongo/db/vector_clock/vector_clock_mutable.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/db/versioning_protocol/shard_version_factory.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/async_rpc.h"
#include "mongo/executor/task_executor.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#include <algorithm>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

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
        const auto toDB = Grid::get(opCtx)->catalogClient()->getDatabase(
            opCtx, toNss.dbName(), repl::ReadConcernLevel::kMajorityReadConcern);

        uassert(ErrorCodes::CommandFailed,
                "Source and destination collections must be on same shard",
                ShardingState::get(opCtx)->shardId() == toDB.getPrimary());
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

// TODO (SERVER-98118): Remove this method (assuming a true condition on each invocation)
// once v9.0 become last-lts.
bool supportsPreciseChangeStreamTargeter(OperationContext* opCtx) {
    return feature_flags::gFeatureFlagChangeStreamPreciseShardTargeting.isEnabled(
        VersionContext::getDecoration(opCtx),
        serverGlobalParams.featureCompatibility.acquireFCVSnapshot());
}

void upsertPlacementHistoryDocStatement(const txn_api::TransactionClient& txnClient,
                                        const NamespaceString& nss,
                                        const boost::optional<UUID>& uuid,
                                        const Timestamp& timeAtPlacementChange,
                                        const std::vector<ShardId>&& shards,
                                        int stmtId) {
    write_ops::UpdateCommandRequest upsertPlacementChangeRequest(
        NamespaceString::kConfigsvrPlacementHistoryNamespace);
    upsertPlacementChangeRequest.setUpdates({[&] {
        NamespacePlacementType placementInfo(nss, timeAtPlacementChange, std::move(shards));
        placementInfo.setUuid(uuid);

        write_ops::UpdateOpEntry entry;
        entry.setQ(BSON(NamespacePlacementType::kNssFieldName
                        << NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault())
                        << NamespacePlacementType::kTimestampFieldName << timeAtPlacementChange));
        entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(placementInfo.toBSON()));
        entry.setUpsert(true);
        entry.setMulti(false);
        return entry;
    }()});

    auto upsertPlacementEntryResponse =
        txnClient.runCRUDOpSync(upsertPlacementChangeRequest, {stmtId});

    uassertStatusOK(upsertPlacementEntryResponse.toStatus());
}

std::vector<ShardId> getCurrentCollPlacement(OperationContext* opCtx, const UUID& collUuid) {
    // Use the content of config.chunks to obtain the placement of the collection being
    // renamed. The request is equivalent to 'configDb.chunks.distinct("shard",
    // {uuid:collectionUuid})'.
    DistinctCommandRequest distinctRequest(NamespaceString::kConfigsvrChunksNamespace);
    distinctRequest.setKey(ChunkType::shard.name());
    distinctRequest.setQuery(BSON(ChunkType::collectionUUID.name() << collUuid));
    distinctRequest.setReadConcern(repl::ReadConcernArgs::kLocal);

    auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    auto reply = uassertStatusOK(
        configShard->runCommand(opCtx,
                                ReadPreferenceSetting(ReadPreference::PrimaryOnly, TagSet{}),
                                DatabaseName::kConfig,
                                distinctRequest.toBSON(),
                                Shard::RetryPolicy::kIdempotent));

    uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(reply));
    std::vector<ShardId> shardIds;
    for (const auto& valueElement : reply.response.getField("values").Array()) {
        shardIds.emplace_back(valueElement.String());
    }
    return shardIds;
}

void persistPlacementChangeForCollectionBeingRenamed(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const UUID& originalUUID,
    const UUID& uuidUponRename,
    const Timestamp& clusterTimeUponRename,
    const std::shared_ptr<executor::TaskExecutor>& executor,
    const OperationSessionInfo& osi) {
    auto shardIds = getCurrentCollPlacement(opCtx, originalUUID);

    auto transactionChain = [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
        constexpr auto stmtId = 1;
        upsertPlacementHistoryDocStatement(
            txnClient, nss, uuidUponRename, clusterTimeUponRename, std::move(shardIds), stmtId);

        return SemiFuture<void>::makeReady();
    };

    sharding_ddl_util::runTransactionOnShardingCatalog(
        opCtx, std::move(transactionChain), defaultMajorityWriteConcernDoNotUse(), osi, executor);
}

// Removes the namespace from config.collections. Query by 'ns' AND 'uuid' so that the operation can
// be resolved with an IXSCAN (thanks to the index on '_id') and is idempotent (thanks to the
// 'uuid').
// Returns whether the deletion was actually executed - or resolved into a no-op.
bool deleteTrackedCollectionStatement(const txn_api::TransactionClient& txnClient,
                                      const NamespaceString& nss,
                                      const boost::optional<UUID>& uuid,
                                      int stmtId) {
    if (!uuid) {
        return false;
    }

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

    const auto deleteResponse = txnClient.runCRUDOpSync(deleteOp, {stmtId});
    uassertStatusOK(deleteResponse.toStatus());
    return deleteResponse.getN() != 0;
}

void renameTrackedCollectionStatement(const txn_api::TransactionClient& txnClient,
                                      const CollectionType& oldCollection,
                                      const NamespaceString& newNss,
                                      const boost::optional<UUID>& newTargetCollectionUuid,
                                      const Timestamp& timeInsertion,
                                      const OID& renamedCollectionEpoch,
                                      int stmtId) {
    auto newCollectionType = oldCollection;
    newCollectionType.setNss(newNss);
    newCollectionType.setTimestamp(timeInsertion);
    newCollectionType.setEpoch(renamedCollectionEpoch);
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

    uassertStatusOK(txnClient.runCRUDOpSync(updateOp, {stmtId} /*stmtIds*/).toStatus());
}

void updateUnsplittableCollChunkStmt(const txn_api::TransactionClient& txnClient,
                                     const CollectionType& oldCollection,
                                     const boost::optional<UUID>& newTargetCollectionUuid) {
    // Skip the statement in case the commit does not involve the cross-DB rename of an unsplittable
    // collection.
    if (!newTargetCollectionUuid.has_value() ||
        newTargetCollectionUuid.get() == oldCollection.getUuid()) {
        return;
    }

    const auto query = BSON(ChunkType::collectionUUID() << oldCollection.getUuid());
    const auto update =
        BSON("$set" << BSON(ChunkType::collectionUUID() << *newTargetCollectionUuid));

    // This query is expected to target unsplittable collections with one chunk.
    // Don't use this for updating a high amount of chunks because the transaction
    // may abort due to hitting the `transactionLifetimeLimitSeconds`.
    BatchedCommandRequest request([&] {
        write_ops::UpdateCommandRequest updateOp(NamespaceString::kConfigsvrChunksNamespace);
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

    const auto response = txnClient.runCRUDOpSync(request, {-1} /*stmtIds*/);
    uassertStatusOK(response.toStatus());
    // Only one chunk should have been updated (unless the transaction is repeated due to a
    // stepdown).
    tassert(
        10488804, "Unexpectedly found collection with more than one chunk", response.getN() <= 1);
}

void updateZonesStatement(const txn_api::TransactionClient& txnClient,
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

    uassertStatusOK(txnClient.runCRUDOpSync(request, {-1} /*stmtIds*/).toStatus());
}

void deleteZonesStatement(const txn_api::TransactionClient& txnClient, const NamespaceString& nss) {

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

    uassertStatusOK(txnClient.runCRUDOpSync(request, {-1}).toStatus());
}

// TODO (SERVER-98118): remove the logTargetPlacementChange parameter (assuming a 'false' value)
// once v9.0 become last-lts.
void renameCollectionMetadataInTransaction(OperationContext* opCtx,
                                           const boost::optional<CollectionType>& optFromCollType,
                                           const NamespaceString& fromNss,
                                           const NamespaceString& toNss,
                                           const boost::optional<UUID>& droppedTargetUUID,
                                           const boost::optional<UUID>& newTargetCollectionUuid,
                                           const Timestamp& commitTime,
                                           const OID& renamedCollectionEpoch,
                                           const std::shared_ptr<executor::TaskExecutor>& executor,
                                           const OperationSessionInfo& osi) {
    const auto isFromCollTracked = optFromCollType.has_value();

    auto transactionChain = [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
        int stmtId = 1;
        if (isFromCollTracked) {
            auto fromUUID = optFromCollType->getUuid();
            // Delete TO collection (if it is also tracked).
            deleteTrackedCollectionStatement(txnClient, toNss, droppedTargetUUID, stmtId++);
            // Log the placement change for TO through an upsert statement
            // (Note: a first copy may have been already committed during the
            // kSetupChangeStreamsPreconditions phase, but then deleted as a consequence of a
            // concurrent resetPlacementHistory command).
            auto shardIds = getCurrentCollPlacement(opCtx, fromUUID);
            if (!shardIds.empty()) {
                upsertPlacementHistoryDocStatement(txnClient,
                                                   toNss,
                                                   newTargetCollectionUuid,
                                                   commitTime,
                                                   std::move(shardIds),
                                                   stmtId++);
            }
            // Delete FROM collection.
            deleteTrackedCollectionStatement(txnClient, fromNss, fromUUID, stmtId++);
            // Persist the entry for the renamed collection
            renameTrackedCollectionStatement(txnClient,
                                             *optFromCollType,
                                             toNss,
                                             newTargetCollectionUuid,
                                             commitTime,
                                             renamedCollectionEpoch,
                                             stmtId++);
            // Log the placement change of FROM.
            upsertPlacementHistoryDocStatement(
                txnClient, fromNss, fromUUID, commitTime, {} /*shards*/, stmtId++);
            // Reassign original zones to the renamed collection entry.
            updateZonesStatement(txnClient, fromNss, toNss);
            // A rename request across databases (only allowed when FROM is unsharded or
            // unsplittable) causes a change of the collection UUID; config.chunks needs to be
            // updated accordingly.
            updateUnsplittableCollChunkStmt(txnClient, *optFromCollType, newTargetCollectionUuid);
        } else {
            // If TO is tracked, remove its routing table and log the placement change.
            // (Note: the placement change may have been already committed during the
            // kSetupChangeStreamsPreconditions phase, but then deleted as a consequence of a
            // concurrent resetPlacementHistory command).
            const auto targetMetadataDeleted =
                deleteTrackedCollectionStatement(txnClient, toNss, droppedTargetUUID, stmtId++);
            if (targetMetadataDeleted) {
                upsertPlacementHistoryDocStatement(txnClient,
                                                   toNss,
                                                   newTargetCollectionUuid,
                                                   commitTime,
                                                   {} /*shards=*/,
                                                   stmtId++);
            }
            deleteZonesStatement(txnClient, toNss);
        }

        return SemiFuture<void>::makeReady();
    };

    sharding_ddl_util::runTransactionOnShardingCatalog(
        opCtx, std::move(transactionChain), defaultMajorityWriteConcernDoNotUse(), osi, executor);

    const std::string logWhatParam = str::stream()
        << toStringForLogging(fromNss) << " to " << toStringForLogging(toNss)
        << " : dropped target collection"
        << (isFromCollTracked ? " and renamed source collection." : ".");
    const auto logDetailParam =
        isFromCollTracked ? BSON("newCollMetadata" << optFromCollType->toBSON()) : BSONObj();

    ShardingLogging::get(opCtx)->logChange(
        opCtx,
        logWhatParam,
        NamespaceStringUtil::deserialize(
            boost::none, "renameCollection.metadata", SerializationContext::stateDefault()),
        logDetailParam,
        defaultMajorityWriteConcernDoNotUse(),
        Grid::get(opCtx)->shardRegistry()->getConfigShard(),
        Grid::get(opCtx)->catalogClient());
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

    const auto currentIndexes = router.routeWithRoutingContext(
        opCtx,
        "checking indexes prerequisites within rename collection coordinator",
        [&](OperationContext* opCtx, RoutingContext& routingCtx) {
            const auto response = loadIndexesFromAuthoritativeShard(opCtx, routingCtx, targetNss);
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
        doc, IDLParserContext("RenameCollectionCoordinatorDocument"));

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
                auto opCtxHolder = makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                checkExpectedTargetIndexesMatch(
                    opCtx,
                    _request.getTo(),
                    *_doc.getRenameCollectionRequest().getExpectedIndexes());
            }
        })
        .then(_buildPhaseHandler(
            Phase::kCheckPreconditions,
            [this, executor = executor, anchor = shared_from_this()](auto* opCtx) {
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
                                !CollectionCatalog::get(opCtx)->lookupView(opCtx, originalNss()));

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

                        // TODO SERVER-89089 adapt this check once we have the guarantee that bucket
                        // collection always have timeseries options
                        uassert(ErrorCodes::IllegalOperation,
                                str::stream() << "Cannot rename timeseries buckets collection '"
                                              << fromNss.toStringForErrorMsg()
                                              << "' to a namespace that is not timeseries buckets '"
                                              << toNss.toStringForErrorMsg() << "'.",
                                !fromNss.isTimeseriesBucketsCollection() ||
                                    !coll->getTimeseriesOptions() ||
                                    toNss.isTimeseriesBucketsCollection());
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
                            ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter());
                        criticalSection->promoteRecoverableCriticalSectionToBlockAlsoReads(
                            opCtx,
                            toNss,
                            criticalSectionReason,
                            ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter());

                        if (CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx,
                                                                                       toNss)) {
                            // Release the critical section because the untracked target collection
                            // already exists, hence no risk of concurrent `createCollection`
                            criticalSection->releaseRecoverableCriticalSection(
                                opCtx,
                                toNss,
                                criticalSectionReason,
                                ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
                                ShardingRecoveryService::NoCustomAction());
                        }
                    }
                    const auto isSourceUnsharded =
                        !optSourceCollType || optSourceCollType->getUnsplittable();
                    sharding_ddl_util::checkRenamePreconditions(
                        opCtx, toNss, optTargetCollType, isSourceUnsharded, _doc.getDropTarget());

                    checkDatabaseRestrictions(opCtx, fromNss, optSourceCollType, toNss);

                    checkCatalogConsistencyAcrossShards(
                        opCtx, fromNss, optSourceCollType, toNss, _doc.getDropTarget(), executor);

                    // Check that the target collection is not sharded, if requested.
                    if (_doc.getRenameCollectionRequest().getTargetMustNotBeSharded().get_value_or(
                            false)) {
                        uassert(ErrorCodes::IllegalOperation,
                                str::stream() << "cannot rename to sharded collection '"
                                              << toNss.toStringForErrorMsg() << "'",
                                !isCollectionSharded(optTargetCollType));
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
                        ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
                        ShardingRecoveryService::NoCustomAction(),
                        false /* throwIfReasonDiffers */);
                    _completeOnError = true;
                    throw;
                }
            }))
        .then(_buildPhaseHandler(
            Phase::kFreezeMigrations,
            [this, executor = executor, anchor = shared_from_this()](auto* opCtx) {
                const auto& fromNss = nss();
                const auto& toNss = _request.getTo();

                ShardingLogging::get(opCtx)->logChange(
                    opCtx,
                    "renameCollection.start",
                    fromNss,
                    BSON("source" << NamespaceStringUtil::serialize(
                                         fromNss, SerializationContext::stateDefault())
                                  << "destination"
                                  << NamespaceStringUtil::serialize(
                                         toNss, SerializationContext::stateDefault())),
                    defaultMajorityWriteConcernDoNotUse());

                // Block migrations on involved collections.
                try {
                    const auto session = getNewSession(opCtx);
                    sharding_ddl_util::stopMigrations(
                        opCtx, fromNss, _doc.getSourceUUID(), session);
                } catch (ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
                    // stopMigrations is allowed to fail when the source collection is not tracked
                    // by the sharding catalog.
                }

                try {
                    const auto session = getNewSession(opCtx);
                    sharding_ddl_util::stopMigrations(opCtx, toNss, _doc.getTargetUUID(), session);
                } catch (ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
                    // stopMigrations is allowed to fail when the target collection doesn't exist or
                    // is not tracked by the sharding catalog.
                }
            }))
        .then(_buildPhaseHandler(
            Phase::kSetupChangeStreamsPreconditions,
            supportsPreciseChangeStreamTargeter /*shouldExecute*/,
            [this, token, executor = executor, anchor = shared_from_this()](auto* opCtx) {
                const auto& fromNss = nss();
                const auto& toNss = _request.getTo();
                const auto reason =
                    sharding_ddl_util::getCriticalSectionReasonForRename(fromNss, toNss);

                auto acquireCriticalSectionOnParticipantsFor = [&](const NamespaceString& nss) {
                    LOGV2_DEBUG(10488800, 2, "Acquiring critical section", "nss"_attr = nss);

                    ShardsvrParticipantBlock blockCRUDOperationsRequest(nss);
                    blockCRUDOperationsRequest.setBlockType(
                        mongo::CriticalSectionBlockTypeEnum::kReadsAndWrites);
                    blockCRUDOperationsRequest.setReason(reason);

                    generic_argument_util::setMajorityWriteConcern(blockCRUDOperationsRequest);
                    generic_argument_util::setOperationSessionInfo(blockCRUDOperationsRequest,
                                                                   getNewSession(opCtx));
                    auto opts =
                        std::make_shared<async_rpc::AsyncRPCOptions<ShardsvrParticipantBlock>>(
                            **executor, token, blockCRUDOperationsRequest);
                    sharding_ddl_util::sendAuthenticatedCommandToShards(
                        opCtx, opts, Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx));

                    LOGV2_DEBUG(10488801, 2, "Acquired critical section", "nss"_attr = nss);
                };

                auto getChangeStreamNotifierShardIdFor =
                    [&](const boost::optional<UUID>& collUUID) {
                        // In case of tracked collection, a data bearing shard needs to generate
                        // events about the upcoming placement change.
                        if (collUUID.has_value()) {
                            auto shardWithChunks =
                                sharding_ddl_util::pickShardOwningCollectionChunks(opCtx,
                                                                                   *collUUID);
                            if (shardWithChunks.has_value()) {
                                return *shardWithChunks;
                            }
                        }

                        // In case the collection is untracked or does not currently exist, change
                        // stream readers are expected to tail the primary shard of the parent DB.
                        return ShardingState::get(opCtx)->shardId();
                    };

                // 1. Block CRUD operations on any node for both namespaces before emitting any
                // pre-post commit notification to change stream readers.
                acquireCriticalSectionOnParticipantsFor(fromNss);
                acquireCriticalSectionOnParticipantsFor(toNss);

                // 2. Define stable values for:
                // - The cluster time at which the commit of this operation will be recorded in the
                //   content of notification events and config.collections/placementHistory
                //   documents;
                // - The new epoch for FROM, once renamed to TO.
                // - The identity of the shard that will notify change stream readers of FROM once
                //   the operation gets committed.
                if (!_doc.getCommitTimeInGlobalCatalog()) {
                    auto newDoc = _doc;

                    auto now = VectorClock::get(opCtx)->getTime();
                    auto commitTime = now.clusterTime().asTimestamp();
                    newDoc.setCommitTimeInGlobalCatalog(commitTime);
                    newDoc.setRenamedCollectionEpoch(OID::gen());
                    auto changeStreamsNotifierForSource =
                        getChangeStreamNotifierShardIdFor(_doc.getSourceUUID().value());
                    LOGV2(10488802,
                          "Defined notifier shard Id for change streams tracking the source nss",
                          "sourceNss"_attr = fromNss,
                          "notifierId"_attr = changeStreamsNotifierForSource);
                    newDoc.setChangeStreamsNotifier(std::move(changeStreamsNotifierForSource));

                    _updateStateDocument(opCtx, std::move(newDoc));
                }

                // 3. Generate placement change metadata (config.placementHistory doc + control
                // event on data bearing shard) for TO before the commit; this will redirect new
                // and existing change stream readers for this collection to the data bearing shards
                // of FROM, where they will be able to observe the "commit" event involving both
                // namespaces.
                const auto session = getNewSession(opCtx);
                persistPlacementChangeForCollectionBeingRenamed(
                    opCtx,
                    toNss,
                    _doc.getSourceUUID().value(),
                    _doc.getNewTargetCollectionUuid().value(),
                    _doc.getCommitTimeInGlobalCatalog().value(),
                    **executor,
                    session);


                const auto changeStreamNotifierForTarget =
                    getChangeStreamNotifierShardIdFor(_doc.getTargetUUID());

                NamespacePlacementChanged notification(toNss, *_doc.getCommitTimeInGlobalCatalog());
                auto buildNewSessionFn = [this](OperationContext* opCtx) {
                    return getNewSession(opCtx);
                };

                LOGV2(10488803,
                      "Defined notifier shard Id for change streams tracking the target nss",
                      "targetNss"_attr = toNss,
                      "notifierId"_attr = changeStreamNotifierForTarget);

                sharding_ddl_util::generatePlacementChangeNotificationOnShard(
                    opCtx,
                    notification,
                    changeStreamNotifierForTarget,
                    buildNewSessionFn,
                    executor,
                    token);
            }))
        .then(_buildPhaseHandler(
            Phase::kBlockCrudAndRename,
            [this, token, executor = executor, anchor = shared_from_this()](auto* opCtx) {
                if (!_firstExecution) {
                    const auto session = getNewSession(opCtx);
                    _performNoopRetryableWriteOnAllShardsAndConfigsvr(opCtx, session, **executor);
                }

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

                const auto opSessionInfo = getNewSession(opCtx);
                generic_argument_util::setMajorityWriteConcern(renameCollParticipantRequest);
                generic_argument_util::setOperationSessionInfo(renameCollParticipantRequest,
                                                               opSessionInfo);

                auto getOtherParticipants = [&](const ShardId& mainParticipant) {
                    auto participants = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
                    participants.erase(
                        std::remove(participants.begin(), participants.end(), mainParticipant),
                        participants.end());
                    return participants;
                };

                auto sendRequestTo = [&](const ShardsvrRenameCollectionParticipant& request,
                                         const std::vector<ShardId>& participants) {
                    auto opts = std::make_shared<
                        async_rpc::AsyncRPCOptions<ShardsvrRenameCollectionParticipant>>(
                        **executor, token, request);
                    sharding_ddl_util::sendAuthenticatedCommandToShards(opCtx, opts, participants);
                };

                // Perform the local commit on each shard. The command is sent out everywhere, since
                // both movePrimary and moveChunk leave garbage behind for sharded collections.
                if (supportsPreciseChangeStreamTargeter(opCtx)) {
                    // Instruct the notifier shard ID to generate user-visible commit events for
                    // change streams...
                    const auto& notifierShardId = _doc.getChangeStreamsNotifier().value();
                    renameCollParticipantRequest.setFromMigrate(false);
                    sendRequestTo(renameCollParticipantRequest, {notifierShardId});

                    // .. and the opposite for the rest of the participants.
                    const auto otherParticipants = getOtherParticipants(notifierShardId);
                    renameCollParticipantRequest.setFromMigrate(true);
                    sendRequestTo(renameCollParticipantRequest, otherParticipants);
                } else {
                    // (Since critical sections may not be taken at this stage), the primary shard
                    // needs to be last participant to perform its local rename operation: this will
                    // ensure that the op entries generated by the collections being renamed/dropped
                    // will be generated at points in time where all shards have a consistent view
                    // of the metadata and no concurrent writes are being performed.
                    const auto primaryShardId = ShardingState::get(opCtx)->shardId();
                    const auto otherParticipants = getOtherParticipants(primaryShardId);

                    sendRequestTo(renameCollParticipantRequest, otherParticipants);
                    sendRequestTo(renameCollParticipantRequest, {primaryShardId});
                }
            }))
        .then(_buildPhaseHandler(
            Phase::kRenameMetadata,
            [this, token, executor = executor, anchor = shared_from_this()](auto* opCtx) {
                // Remove the query sampling configuration documents for the source and destination
                // collections, if they exist.
                {
                    auto getCollUuidsToRemove = [&] {
                        std::vector<UUID> collectionUUIDs;
                        collectionUUIDs.push_back(_doc.getSourceUUID().value());
                        if (auto targetUUID = _doc.getTargetUUID()) {
                            collectionUUIDs.push_back(targetUUID.value());
                        }
                        return collectionUUIDs;
                    };
                    sharding_ddl_util::removeQueryAnalyzerMetadata(opCtx, getCollUuidsToRemove());
                }

                // For an untracked collection the CSRS server can not verify the targetUUID.
                // Use the session ID + txnNumber to ensure no stale requests get through.
                if (!_firstExecution) {
                    _performNoopRetryableWriteOnAllShardsAndConfigsvr(
                        opCtx, getNewSession(opCtx), **executor);
                }

                // Commit the collection and chunks metadata on the global catalog.
                // (This will also have the effect of resuming migrations on the renamed namespace).
                const auto& fromNss = nss();
                const auto& toNss = _request.getTo();
                const auto preciseChangeStreamTargeterEnabled =
                    supportsPreciseChangeStreamTargeter(opCtx);
                const auto commitTime = [&] {
                    if (preciseChangeStreamTargeterEnabled) {
                        tassert(10723700,
                                "The commit time must be already present in the recovery document",
                                _doc.getCommitTimeInGlobalCatalog().has_value());
                        return _doc.getCommitTimeInGlobalCatalog().value();
                    }

                    auto now = VectorClock::get(opCtx)->getTime();
                    return now.clusterTime().asTimestamp();
                }();

                const auto renamedCollectionEpoch = [&] {
                    if (preciseChangeStreamTargeterEnabled) {
                        return _doc.getRenamedCollectionEpoch().value();
                    }

                    return OID::gen();
                }();

                const auto session = getNewSession(opCtx);
                renameCollectionMetadataInTransaction(opCtx,
                                                      _doc.getOptTrackedCollInfo(),
                                                      fromNss,
                                                      toNss,
                                                      _doc.getTargetUUID(),
                                                      _doc.getNewTargetCollectionUuid(),
                                                      commitTime,
                                                      renamedCollectionEpoch,
                                                      **executor,
                                                      session);

                // Generate post-commit placement change event for FROM.
                if (preciseChangeStreamTargeterEnabled) {
                    NamespacePlacementChanged notification(fromNss,
                                                           *_doc.getCommitTimeInGlobalCatalog());
                    auto buildNewSessionFn = [this](OperationContext* opCtx) {
                        return getNewSession(opCtx);
                    };
                    sharding_ddl_util::generatePlacementChangeNotificationOnShard(
                        opCtx,
                        notification,
                        _doc.getChangeStreamsNotifier().value(),
                        buildNewSessionFn,
                        executor,
                        token);
                }

                // Checkpoint the configTime to ensure that, in the case of a stepdown, the new
                // primary will start-up from a configTime that is inclusive of the renamed
                // metadata.
                VectorClockMutable::get(opCtx)->waitForDurableConfigTime().get(opCtx);
            }))
        .then(_buildPhaseHandler(
            Phase::kUnblockCRUD,
            [this, token, executor = executor, anchor = shared_from_this()](auto* opCtx) {
                if (!_firstExecution) {
                    _performNoopRetryableWriteOnAllShardsAndConfigsvr(
                        opCtx, getNewSession(opCtx), **executor);
                }

                const auto& fromNss = nss();
                // On participant shards:
                // - Unblock CRUD on participants for both source and destination collections
                ShardsvrRenameCollectionUnblockParticipant unblockParticipantRequest(
                    fromNss, _doc.getSourceUUID().value());
                unblockParticipantRequest.setDbName(fromNss.dbName());
                unblockParticipantRequest.setRenameCollectionRequest(_request);
                auto participants = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);

                generic_argument_util::setMajorityWriteConcern(unblockParticipantRequest);
                generic_argument_util::setOperationSessionInfo(unblockParticipantRequest,
                                                               getNewSession(opCtx));
                auto opts = std::make_shared<
                    async_rpc::AsyncRPCOptions<ShardsvrRenameCollectionUnblockParticipant>>(
                    **executor, token, unblockParticipantRequest);
                sharding_ddl_util::sendAuthenticatedCommandToShards(opCtx, opts, participants);

                // Delete chunks belonging to the previous incarnation of the target collection.
                // This is performed after releasing the critical section in order to reduce stalls
                // and performed outside of a transaction to prevent timeout.
                auto targetUUID = _doc.getTargetUUID();
                if (targetUUID) {
                    auto query = BSON("uuid" << *targetUUID);
                    uassertStatusOK(Grid::get(opCtx)->catalogClient()->removeConfigDocuments(
                        opCtx,
                        NamespaceString::kConfigsvrChunksNamespace,
                        query,
                        defaultMajorityWriteConcernDoNotUse()));
                }
            }))
        .then(_buildPhaseHandler(
            Phase::kSetResponse, [this, anchor = shared_from_this()](auto* opCtx) {
                // Retrieve the new collection version
                const auto& cm = uassertStatusOK(
                    Grid::get(opCtx)->catalogCache()->getCollectionPlacementInfoWithRefresh(
                        opCtx, _request.getTo()));
                auto placementVersion =
                    cm.hasRoutingTable() ? cm.getVersion() : ChunkVersion::UNSHARDED();
                _response = RenameCollectionResponse(
                    ShardVersionFactory::make(std::move(placementVersion)));

                ShardingLogging::get(opCtx)->logChange(
                    opCtx,
                    "renameCollection.end",
                    nss(),
                    BSON("source" << NamespaceStringUtil::serialize(
                                         nss(), SerializationContext::stateDefault())
                                  << "destination"
                                  << NamespaceStringUtil::serialize(
                                         _request.getTo(), SerializationContext::stateDefault())),
                    defaultMajorityWriteConcernDoNotUse());
                LOGV2(5460504, "Collection renamed", logAttrs(nss()));
            }));
}

}  // namespace mongo
