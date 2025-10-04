/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bson_field.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/bson/util/builder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/catalog_cache/routing_information_cache.h"
#include "mongo/db/global_catalog/ddl/shard_util.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/global_catalog/ddl/sharding_util.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/global_catalog/type_collection_gen.h"
#include "mongo/db/global_catalog/type_namespace_placement_gen.h"
#include "mongo/db/global_catalog/type_tags.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/shard_role_api/resource_yielder.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/query/distinct_command_gen.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/sharding_config_server_parameters_gen.h"
#include "mongo/db/sharding_environment/sharding_logging.h"
#include "mongo/db/storage/snapshot_window_options_gen.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/transaction/transaction_api.h"
#include "mongo/db/transaction/transaction_participant_gen.h"
#include "mongo/db/transaction/transaction_participant_resource_yielder.h"
#include "mongo/db/vector_clock/vector_clock.h"
#include "mongo/db/write_concern.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/inline_executor.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/logv2/attribute_storage.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/functional.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/future_util.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

MONGO_FAIL_POINT_DEFINE(overrideHistoryWindowInSecs);

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(migrationCommitVersionError);
MONGO_FAIL_POINT_DEFINE(migrateCommitInvalidChunkQuery);
MONGO_FAIL_POINT_DEFINE(skipExpiringOldChunkHistory);
MONGO_FAIL_POINT_DEFINE(hangMergeAllChunksUntilReachingTimeout);

const WriteConcernOptions kNoWaitWriteConcern(1, WriteConcernOptions::SyncMode::UNSET, Seconds(0));

/**
 * Append min, max and version information from chunk to the buffer for logChange purposes.
 */
void appendShortVersion(BufBuilder* out, const ChunkType& chunk) {
    BSONObjBuilder bb(*out);
    bb.append(ChunkType::min(), chunk.getMin());
    bb.append(ChunkType::max(), chunk.getMax());
    if (chunk.isVersionSet()) {
        chunk.getVersion().serialize(ChunkType::lastmod(), &bb);
    }
    bb.done();
}

/**
 * Check that the chunk still exists and return its metadata.
 */
StatusWith<ChunkType> findChunkContainingRange(OperationContext* opCtx,
                                               Shard* configShard,
                                               const UUID& uuid,
                                               const OID& epoch,
                                               const Timestamp& timestamp,
                                               const ChunkRange& range) {
    const auto chunkQuery = [&]() {
        BSONObjBuilder queryBuilder;
        queryBuilder << ChunkType::collectionUUID << uuid;
        queryBuilder << ChunkType::min(BSON("$lte" << range.getMin()));
        return queryBuilder.obj();
    }();

    // Must use local read concern because we're going to perform subsequent writes.
    auto findResponseWith =
        configShard->exhaustiveFindOnConfig(opCtx,
                                            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                            repl::ReadConcernLevel::kLocalReadConcern,
                                            NamespaceString::kConfigsvrChunksNamespace,
                                            chunkQuery,
                                            BSON(ChunkType::min << -1),
                                            1 /* limit */);

    if (!findResponseWith.isOK()) {
        return findResponseWith.getStatus();
    }

    if (!findResponseWith.getValue().docs.empty()) {
        const auto containingChunk = uassertStatusOK(ChunkType::parseFromConfigBSON(
            findResponseWith.getValue().docs.front(), epoch, timestamp));

        if (containingChunk.getRange().covers(range)) {
            return containingChunk;
        }
    }

    return {ErrorCodes::Error(40165),
            str::stream() << "Could not find a chunk including bounds [" << range.getMin() << ", "
                          << range.getMax()
                          << "). Cannot execute the migration commit with invalid chunks."};
}

BSONObj buildCountChunksInRangeCommand(const UUID& collectionUUID,
                                       const ShardId& shardId,
                                       const ChunkRange& chunkRange) {
    AggregateCommandRequest countRequest(NamespaceString::kConfigsvrChunksNamespace);

    BSONObjBuilder builder;
    builder.append("aggregate",
                   NamespaceStringUtil::serialize(NamespaceString::kConfigsvrChunksNamespace,
                                                  SerializationContext::stateDefault()));

    BSONObjBuilder queryBuilder;
    queryBuilder << ChunkType::collectionUUID << collectionUUID;
    queryBuilder << ChunkType::shard(shardId.toString());
    queryBuilder << ChunkType::min(BSON("$gte" << chunkRange.getMin()));
    queryBuilder << ChunkType::min(BSON("$lt" << chunkRange.getMax()));

    std::vector<BSONObj> pipeline;
    pipeline.push_back(BSON("$match" << queryBuilder.obj()));
    pipeline.push_back(BSON("$count" << ChunkType::collectionUUID.name()));
    countRequest.setPipeline(pipeline);

    return countRequest.toBSON();
}

BSONObj buildCountSingleChunkCommand(const ChunkType& chunk) {
    AggregateCommandRequest countRequest(NamespaceString::kConfigsvrChunksNamespace);

    auto query =
        BSON(ChunkType::min(chunk.getMin())
             << ChunkType::max(chunk.getMax()) << ChunkType::collectionUUID()
             << chunk.getCollectionUUID() << ChunkType::shard() << chunk.getShard().toString());
    std::vector<BSONObj> pipeline;
    pipeline.push_back(BSON("$match" << query));
    pipeline.push_back(BSON("$count" << ChunkType::collectionUUID.name()));
    countRequest.setPipeline(pipeline);

    return countRequest.toBSON();
}

BSONObj buildCountContiguousChunksByBounds(const UUID& collectionUUID,
                                           const std::string& shard,
                                           const std::vector<BSONObj>& boundsForChunks) {
    AggregateCommandRequest countRequest(NamespaceString::kConfigsvrChunksNamespace);

    invariant(boundsForChunks.size() > 1);
    auto minBoundIt = boundsForChunks.begin();
    auto maxBoundIt = minBoundIt + 1;

    BSONArrayBuilder chunkDocArray;
    while (maxBoundIt != boundsForChunks.end()) {
        const auto query = BSON(ChunkType::min(*minBoundIt)
                                << ChunkType::max(*maxBoundIt) << ChunkType::collectionUUID()
                                << collectionUUID << ChunkType::shard() << shard);

        chunkDocArray.append(query);
        ++minBoundIt;
        ++maxBoundIt;
    }

    std::vector<BSONObj> pipeline;
    pipeline.push_back(BSON("$match" << BSON("$or" << chunkDocArray.arr())));
    pipeline.push_back(BSON("$count" << ChunkType::collectionUUID.name()));
    countRequest.setPipeline(pipeline);
    return countRequest.toBSON();
}

/**
 * Returns a chunk different from the one being migrated or 'none' if one doesn't exist.
 */
boost::optional<ChunkType> getControlChunkForMigrate(OperationContext* opCtx,
                                                     Shard* configShard,
                                                     const UUID& uuid,
                                                     const OID& epoch,
                                                     const Timestamp& timestamp,
                                                     const ChunkType& migratedChunk,
                                                     const ShardId& fromShard) {
    BSONObjBuilder queryBuilder;
    queryBuilder << ChunkType::collectionUUID << uuid;
    queryBuilder << ChunkType::shard(fromShard.toString());
    queryBuilder << ChunkType::min(BSON("$ne" << migratedChunk.getMin()));

    auto status =
        configShard->exhaustiveFindOnConfig(opCtx,
                                            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                            repl::ReadConcernLevel::kLocalReadConcern,
                                            NamespaceString::kConfigsvrChunksNamespace,
                                            queryBuilder.obj(),
                                            {},
                                            1);
    auto response = uassertStatusOK(status);
    if (response.docs.empty()) {
        return boost::none;
    }

    return uassertStatusOK(ChunkType::parseFromConfigBSON(response.docs.front(), epoch, timestamp));
}

/**
 * Helper function to find collection and shard placement versions.
 */
StatusWith<ChunkVersion> getMaxChunkVersionFromQueryResponse(
    const CollectionType& coll, const StatusWith<Shard::QueryResponse>& queryResponse) {

    if (!queryResponse.isOK()) {
        return queryResponse.getStatus();
    }

    const auto& chunksVector = queryResponse.getValue().docs;
    if (chunksVector.empty()) {
        return {ErrorCodes::Error(50577),
                str::stream() << "Collection '" << coll.getNss().toStringForErrorMsg()
                              << "' no longer either exists, is sharded, or has chunks"};
    }

    const auto chunk = uassertStatusOK(
        ChunkType::parseFromConfigBSON(chunksVector.front(), coll.getEpoch(), coll.getTimestamp()));

    return chunk.getVersion();
}

/**
 * Helper function to get the collection entry and version for nss. Always uses kLocalReadConcern.
 */
StatusWith<std::pair<CollectionType, ChunkVersion>> getCollectionAndVersion(
    OperationContext* opCtx, Shard* configShard, const NamespaceString& nss) {
    auto findCollResponse = configShard->exhaustiveFindOnConfig(
        opCtx,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        repl::ReadConcernLevel::kLocalReadConcern,
        CollectionType::ConfigNS,
        BSON(CollectionType::kNssFieldName
             << NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault())),
        {},
        1);
    if (!findCollResponse.isOK()) {
        return findCollResponse.getStatus();
    }

    if (findCollResponse.getValue().docs.empty()) {
        return {ErrorCodes::ConflictingOperationInProgress,
                str::stream() << "Sharded collection '" << nss.toStringForErrorMsg()
                              << "' no longer exists"};
    }

    CollectionType coll(findCollResponse.getValue().docs[0]);
    const auto chunksQuery = BSON(ChunkType::collectionUUID << coll.getUuid());
    auto version = uassertStatusOK(getMaxChunkVersionFromQueryResponse(
        coll,
        configShard->exhaustiveFindOnConfig(opCtx,
                                            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                            repl::ReadConcernLevel::kLocalReadConcern,
                                            NamespaceString::kConfigsvrChunksNamespace,
                                            chunksQuery,  // Query all chunks for this namespace.
                                            BSON(ChunkType::lastmod << -1),  // Sort by version.
                                            1))                              // Limit 1.
    );

    return std::pair<CollectionType, ChunkVersion>{std::move(coll), std::move(version)};
}

ChunkVersion getShardPlacementVersion(OperationContext* opCtx,
                                      Shard* configShard,
                                      const CollectionType& coll,
                                      const ShardId& fromShard,
                                      const ChunkVersion& collectionPlacementVersion) {
    const auto chunksQuery =
        BSON(ChunkType::collectionUUID << coll.getUuid() << ChunkType::shard(fromShard.toString()));

    auto swDonorPlacementVersion = getMaxChunkVersionFromQueryResponse(
        coll,
        configShard->exhaustiveFindOnConfig(opCtx,
                                            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                            repl::ReadConcernLevel::kLocalReadConcern,
                                            NamespaceString::kConfigsvrChunksNamespace,
                                            chunksQuery,
                                            BSON(ChunkType::lastmod << -1),  // Sort by version.
                                            1));
    if (!swDonorPlacementVersion.isOK()) {
        if (swDonorPlacementVersion.getStatus().code() == 50577) {
            // The query to find 'nss' chunks belonging to the donor shard didn't return any chunks,
            // meaning the last chunk for fromShard was donated. Gracefully handle the error.
            return ChunkVersion(
                {collectionPlacementVersion.epoch(), collectionPlacementVersion.getTimestamp()},
                {0, 0});
        } else {
            // Bubble up any other error
            uassertStatusOK(swDonorPlacementVersion);
        }
    }
    return swDonorPlacementVersion.getValue();
}

void bumpCollectionMinorVersion(OperationContext* opCtx,
                                Shard* configShard,
                                const NamespaceString& nss,
                                TxnNumber txnNumber) {
    const auto findCollResponse = uassertStatusOK(configShard->exhaustiveFindOnConfig(
        opCtx,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        repl::ReadConcernLevel::kLocalReadConcern,
        CollectionType::ConfigNS,
        BSON(CollectionType::kNssFieldName
             << NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault())),
        {},
        1));
    uassert(
        ErrorCodes::NamespaceNotFound, "Collection does not exist", !findCollResponse.docs.empty());
    const CollectionType coll(findCollResponse.docs[0]);

    // Find the newest chunk
    const auto findChunkResponse = uassertStatusOK(configShard->exhaustiveFindOnConfig(
        opCtx,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        repl::ReadConcernLevel::kLocalReadConcern,
        NamespaceString::kConfigsvrChunksNamespace,
        BSON(ChunkType::collectionUUID << coll.getUuid()) /* query */,
        BSON(ChunkType::lastmod << -1) /* sort */,
        1 /* limit */));

    uassert(ErrorCodes::IncompatibleShardingMetadata,
            str::stream() << "Tried to find max chunk version for collection '"
                          << nss.toStringForErrorMsg() << ", but found no chunks",
            !findChunkResponse.docs.empty());

    const auto newestChunk = uassertStatusOK(ChunkType::parseFromConfigBSON(
        findChunkResponse.docs[0], coll.getEpoch(), coll.getTimestamp()));
    const auto targetVersion = [&]() {
        ChunkVersion version = newestChunk.getVersion();
        version.incMinor();
        return version;
    }();

    // Update the newest chunk to have the new (bumped) version
    BSONObjBuilder updateBuilder;
    BSONObjBuilder updateVersionClause(updateBuilder.subobjStart("$set"));
    updateVersionClause.appendTimestamp(ChunkType::lastmod(), targetVersion.toLong());
    updateVersionClause.doneFast();
    const auto chunkUpdate = updateBuilder.obj();
    const auto request = BatchedCommandRequest::buildUpdateOp(
        NamespaceString::kConfigsvrChunksNamespace,
        BSON(ChunkType::name << newestChunk.getName()),  // query
        chunkUpdate,                                     // update
        false,                                           // upsert
        false                                            // multi
    );

    const auto res = ShardingCatalogManager::get(opCtx)->writeToConfigDocumentInTxn(
        opCtx, NamespaceString::kConfigsvrChunksNamespace, request, txnNumber);

    auto numDocsExpectedModified = 1;
    auto numDocsModified = res.getIntField("n");

    uassert(5511400,
            str::stream() << "Expected to match " << numDocsExpectedModified
                          << " docs, but only matched " << numDocsModified << " for write request "
                          << request.toString(),
            numDocsExpectedModified == numDocsModified);
}

unsigned int getHistoryWindowInSeconds() {
    if (MONGO_unlikely(overrideHistoryWindowInSecs.shouldFail())) {
        int secs;
        overrideHistoryWindowInSecs.execute([&](const BSONObj& data) {
            secs = data["seconds"].numberInt();
            LOGV2(7351500,
                  "overrideHistoryWindowInSecs: using customized history window in seconds",
                  "historyWindowInSeconds"_attr = secs);
        });
        return secs;
    }

    return std::max(minSnapshotHistoryWindowInSeconds.load(),
                    gTransactionLifetimeLimitSeconds.load());
}

void logMergeToChangelog(OperationContext* opCtx,
                         const NamespaceString& nss,
                         const ChunkVersion& prevPlacementVersion,
                         const ChunkVersion& mergedVersion,
                         const ShardId& owningShard,
                         const ChunkRange& chunkRange,
                         const size_t numChunks,
                         std::shared_ptr<Shard> configShard,
                         ShardingCatalogClient* catalogClient,
                         const bool isAutoMerge = false) {
    BSONObjBuilder logDetail;
    prevPlacementVersion.serialize("prevPlacementVersion", &logDetail);
    mergedVersion.serialize("mergedVersion", &logDetail);
    logDetail.append("owningShard", owningShard);
    chunkRange.serialize(&logDetail);
    logDetail.append("numChunks", static_cast<int>(numChunks));
    auto what = isAutoMerge ? "autoMerge" : "merge";

    ShardingLogging::get(opCtx)->logChange(opCtx,
                                           what,
                                           nss,
                                           logDetail.obj(),
                                           WriteConcernOptions(),
                                           std::move(configShard),
                                           catalogClient);
}

void mergeAllChunksOnShardInTransaction(OperationContext* opCtx,
                                        const UUID& collectionUUID,
                                        const ShardId& shardId,
                                        const std::shared_ptr<std::vector<ChunkType>> newChunks) {
    auto updateChunksFn = [collectionUUID, shardId, newChunks](
                              const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
        std::vector<ExecutorFuture<void>> statementsChain;

        for (auto& chunk : *newChunks) {
            // Prepare deletion of existing chunks in the range
            BSONObjBuilder queryBuilder;
            queryBuilder << ChunkType::collectionUUID << collectionUUID;
            queryBuilder << ChunkType::shard(shardId.toString());
            queryBuilder << ChunkType::min(BSON("$gte" << chunk.getMin()));
            queryBuilder << ChunkType::min(BSON("$lt" << chunk.getMax()));

            write_ops::DeleteCommandRequest deleteOp(NamespaceString::kConfigsvrChunksNamespace);
            deleteOp.setDeletes([&] {
                std::vector<write_ops::DeleteOpEntry> deletes;
                write_ops::DeleteOpEntry entry;
                entry.setQ(queryBuilder.obj());
                entry.setMulti(true);
                return std::vector<write_ops::DeleteOpEntry>{entry};
            }());

            // Prepare insertion of new chunks covering the whole range
            write_ops::InsertCommandRequest insertOp(NamespaceString::kConfigsvrChunksNamespace,
                                                     {chunk.toConfigBSON()});

            statementsChain.push_back(txnClient.runCRUDOp(deleteOp, {})
                                          .thenRunOn(txnExec)
                                          .then([](auto removeChunksResponse) {
                                              uassertStatusOK(removeChunksResponse.toStatus());
                                          })
                                          .thenRunOn(txnExec)
                                          .then([&txnClient, insertOp = std::move(insertOp)]() {
                                              return txnClient.runCRUDOp(insertOp, {});
                                          })
                                          .thenRunOn(txnExec)
                                          .then([](auto insertChunkResponse) {
                                              uassertStatusOK(insertChunkResponse.toStatus());
                                          })
                                          .thenRunOn(txnExec));
        }

        return whenAllSucceed(std::move(statementsChain));
    };

    auto executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
    auto inlineExecutor = std::make_shared<executor::InlineExecutor>();

    txn_api::SyncTransactionWithRetries txn(opCtx, executor, nullptr, inlineExecutor);
    txn.run(opCtx, updateChunksFn);
}

/*
 * Check if the placement of the parent collection is impacted by the migrated chunk(s).
 */
bool isPlacementChangedInParentCollection(OperationContext* opCtx,
                                          Shard* configShard,
                                          const ChunkType& migratedChunk,
                                          const std::vector<ChunkType>& splitChunks,
                                          const boost::optional<ChunkType>& controlChunk) {
    // Check if the donor will stop owning data of the parent collection once the migration is
    // committed.
    if (!controlChunk && splitChunks.empty()) {
        return true;
    }

    // Check if the recipient isn't owning data of the parent collection prior to the migration
    // commit.
    const auto query =
        BSON(ChunkType::collectionUUID << migratedChunk.getCollectionUUID() << ChunkType::shard
                                       << migratedChunk.getShard());
    auto findResponse = uassertStatusOK(
        configShard->exhaustiveFindOnConfig(opCtx,
                                            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                            repl::ReadConcernLevel::kLocalReadConcern,
                                            NamespaceString::kConfigsvrChunksNamespace,
                                            query,
                                            BSONObj(),
                                            1 /* limit */));
    if (findResponse.docs.empty()) {
        return true;
    }

    return false;
}

auto doSplitChunk(const txn_api::TransactionClient& txnClient,
                  const ChunkRange& range,
                  const std::string& shardName,
                  const ChunkType& origChunk,
                  const ChunkVersion& collPlacementVersion,
                  const std::vector<BSONObj>& newChunkBounds) {
    auto currentMaxVersion = collPlacementVersion;
    std::vector<ChunkType> newChunks;

    auto startKey = range.getMin();
    OID chunkID;

    auto shouldTakeOriginalChunkID = true;
    write_ops::UpdateCommandRequest updateOp(NamespaceString::kConfigsvrChunksNamespace);
    std::vector<write_ops::UpdateOpEntry> entries;
    entries.reserve(newChunkBounds.size());

    for (const auto& endKey : newChunkBounds) {
        // Verify the split points are all within the chunk
        uassert(ErrorCodes::InvalidOptions,
                str::stream() << "Split key " << endKey << " not contained within chunk "
                              << range.toString(),
                endKey.woCompare(range.getMax()) == 0 || range.containsKey(endKey));

        // Verify the split points came in increasing order
        uassert(ErrorCodes::InvalidOptions,
                str::stream() << "Split keys must be specified in strictly increasing order. Key "
                              << endKey << " was specified after " << startKey << ".",
                endKey.woCompare(startKey) >= 0);

        // Verify that splitPoints are not repeated
        uassert(ErrorCodes::InvalidOptions,
                str::stream() << "Split on lower bound of chunk [" << startKey.toString() << ", "
                              << endKey.toString() << "] is not allowed",
                endKey.woCompare(startKey) != 0);

        // verify that splits don't use disallowed BSON object format
        uassertStatusOK(ShardKeyPattern::checkShardKeyIsValidForMetadataStorage(endKey));

        // splits only update the 'minor' portion of version
        currentMaxVersion.incMinor();

        // First chunk takes ID of the original chunk and all other chunks get new
        // IDs. This occurs because we perform an update operation below (with
        // upsert true). Keeping the original ID ensures we overwrite the old chunk
        // (before the split) without having to perform a delete.
        chunkID = shouldTakeOriginalChunkID ? origChunk.getName() : OID::gen();

        shouldTakeOriginalChunkID = false;

        ChunkType newChunk = origChunk;
        newChunk.setName(chunkID);
        newChunk.setVersion(currentMaxVersion);
        newChunk.setRange({startKey, endKey});
        newChunk.setEstimatedSizeBytes(boost::none);
        newChunk.setJumbo(false);

        // build an update operation against the chunks collection of the config
        // database with upsert true
        write_ops::UpdateOpEntry entry;
        entry.setMulti(false);
        entry.setUpsert(true);
        entry.setQ(BSON(ChunkType::name() << chunkID));
        entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(newChunk.toConfigBSON()));
        entries.push_back(entry);

        // remember this chunk info for logging later
        newChunks.push_back(std::move(newChunk));

        startKey = endKey;
    }
    updateOp.setUpdates(entries);

    auto updateBSONObjSize = updateOp.toBSON().objsize();
    uassert(ErrorCodes::InvalidOptions,
            str::stream() << "Spliting the chunk with too many split points, the "
                             "final BSON operation size "
                          << updateBSONObjSize << " bytes would exceed the maximum BSON size: "
                          << BSONObjMaxInternalSize << " bytes",
            updateBSONObjSize < BSONObjMaxInternalSize);
    uassertStatusOK(txnClient.runCRUDOpSync(updateOp, {}).toStatus());

    LOGV2_DEBUG(6583806, 1, "Split chunk in transaction finished");

    return std::pair{currentMaxVersion, newChunks};
}

// Checks if the requested split already exists. It is possible that the split operation completed,
// but the router did not receive the response. This would result in the router retrying the split
// operation, in which case it is fine for the request to become a no-op.
auto isSplitAlreadyDone(const txn_api::TransactionClient& txnClient,
                        const ChunkRange& range,
                        const std::string& shardName,
                        const ChunkType& origChunk,
                        const std::vector<BSONObj>& newChunkBounds) {
    std::vector<BSONObj> expectedChunksBounds;
    expectedChunksBounds.reserve(newChunkBounds.size() + 1);
    expectedChunksBounds.push_back(range.getMin());
    expectedChunksBounds.insert(
        std::end(expectedChunksBounds), std::begin(newChunkBounds), std::end(newChunkBounds));

    auto countRequest = buildCountContiguousChunksByBounds(
        origChunk.getCollectionUUID(), shardName, expectedChunksBounds);

    const auto expectedChunkCount = expectedChunksBounds.size() - 1;
    auto countResponse =
        txnClient.runCommandSync(NamespaceString::kConfigsvrChunksNamespace.dbName(), countRequest);
    const auto docCount = [&]() {
        auto cursorResponse = uassertStatusOK(CursorResponse::parseFromBSON(countResponse));
        auto firstBatch = cursorResponse.getBatch();
        if (firstBatch.empty()) {
            return 0;
        }

        auto countObj = firstBatch.front();
        return countObj.getIntField(ChunkType::collectionUUID.name());
    }();
    return size_t(docCount) == expectedChunkCount;
}

}  // namespace

void ShardingCatalogManager::bumpMajorVersionOneChunkPerShard(
    OperationContext* opCtx,
    const NamespaceString& nss,
    TxnNumber txnNumber,
    const std::vector<ShardId>& shardIds) {
    const auto [coll, curCollectionPlacementVersion] =
        uassertStatusOK(getCollectionAndVersion(opCtx, _localConfigShard.get(), nss));
    ChunkVersion targetChunkVersion(
        {curCollectionPlacementVersion.epoch(), curCollectionPlacementVersion.getTimestamp()},
        {curCollectionPlacementVersion.majorVersion() + 1, 0});

    for (const auto& shardId : shardIds) {
        BSONObjBuilder updateBuilder;
        BSONObjBuilder updateVersionClause(updateBuilder.subobjStart("$set"));
        updateVersionClause.appendTimestamp(ChunkType::lastmod(), targetChunkVersion.toLong());
        updateVersionClause.doneFast();
        auto chunkUpdate = updateBuilder.obj();

        const auto query = BSON(ChunkType::collectionUUID << coll.getUuid()
                                                          << ChunkType::shard(shardId.toString()));
        auto request =
            BatchedCommandRequest::buildUpdateOp(NamespaceString::kConfigsvrChunksNamespace,
                                                 query,        // query
                                                 chunkUpdate,  // update
                                                 false,        // upsert
                                                 false         // multi
            );

        auto res = ShardingCatalogManager::get(opCtx)->writeToConfigDocumentInTxn(
            opCtx, NamespaceString::kConfigsvrChunksNamespace, request, txnNumber);

        auto numDocsExpectedModified = 1;
        auto numDocsModified = res.getIntField("n");

        uassert(6102800,
                str::stream() << "Expected to match " << numDocsExpectedModified
                              << " docs, but only matched " << numDocsModified
                              << " for write request " << request.toString(),
                numDocsExpectedModified == numDocsModified);

        // There exists a constraint that a chunk version must be unique for a given namespace,
        // so the minor version is incremented for each chunk placed.
        targetChunkVersion.incMinor();
    }
}

ShardingCatalogManager::SplitChunkInTransactionResult
ShardingCatalogManager::_splitChunkInTransaction(OperationContext* opCtx,
                                                 const NamespaceString& nss,
                                                 const ChunkRange& range,
                                                 const std::string& shardName,
                                                 const ChunkType& origChunk,
                                                 const ChunkVersion& collPlacementVersion,
                                                 const std::vector<BSONObj>& splitPoints) {

    ShardingCatalogManager::SplitChunkInTransactionResult splitChunkResult;
    auto updateChunksFn = [&](const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
        ChunkType chunk(origChunk.getCollectionUUID(), range, collPlacementVersion, shardName);

        // Verify that the range matches exactly a single chunk
        auto countRequest = buildCountSingleChunkCommand(chunk);
        auto countResponse = txnClient.runCommandSync(
            NamespaceString::kConfigsvrChunksNamespace.dbName(), countRequest);
        auto cursorResponse = uassertStatusOK(CursorResponse::parseFromBSON(countResponse));
        auto firstBatch = cursorResponse.getBatch();

        std::vector<BSONObj> newChunkBounds{splitPoints};
        newChunkBounds.push_back(range.getMax());

        if (firstBatch.empty()) {
            // Detect if the split already exists (i.e. a retry).
            const auto splitAlreadyDone =
                isSplitAlreadyDone(txnClient, range, shardName, origChunk, newChunkBounds);

            // At this point the split is either already fullfilled or
            // unfullfillable due to preconditions not being met. Anything else in
            // the continuation chain is bypassed by throwing here.
            uassert(ErrorCodes::BadValue,
                    str::stream() << "Could not meet precondition to split chunk, expected "
                                     "chunk with range "
                                  << range.toString() << " in shard " << redact(shardName)
                                  << " but no chunk was found",
                    splitAlreadyDone);

            // Chunks already existed. No need to re-log the chunks.
            splitChunkResult = {collPlacementVersion, {}};
        } else {
            auto countObj = firstBatch.front();
            auto docCount = countObj.getIntField(ChunkType::collectionUUID.name());
            uassert(ErrorCodes::BadValue,
                    str::stream() << "Could not meet precondition to split chunk, expected "
                                     "one chunk with range "
                                  << range.toString() << " in shard " << redact(shardName)
                                  << " but found " << docCount << " chunks",
                    1 == docCount);

            std::tie(splitChunkResult.currentMaxVersion, splitChunkResult.newChunks) = doSplitChunk(
                txnClient, range, shardName, origChunk, collPlacementVersion, newChunkBounds);
        }

        return SemiFuture<void>::makeReady();
    };

    auto executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
    auto inlineExecutor = std::make_shared<executor::InlineExecutor>();

    txn_api::SyncTransactionWithRetries txn(opCtx, executor, nullptr, inlineExecutor);
    txn.run(opCtx, updateChunksFn);

    if (splitChunkResult.newChunks.empty()) {
        // In case the request was already fullfilled, we still need to wait until the original
        // request is majority written. The timestamp is not known, so we use the system's last
        // optime. Otherwise the next RoutingInfo cache refresh from the shard may not see the
        // newest information.
        repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);
    }

    return splitChunkResult;
}

StatusWith<ShardingCatalogManager::ShardAndCollectionPlacementVersions>
ShardingCatalogManager::commitChunkSplit(OperationContext* opCtx,
                                         const NamespaceString& nss,
                                         const OID& requestEpoch,
                                         const boost::optional<Timestamp>& requestTimestamp,
                                         const ChunkRange& range,
                                         const std::vector<BSONObj>& splitPoints,
                                         const std::string& shardName) {

    // Mark opCtx as interruptible to ensure that all reads and writes to the metadata collections
    // under the exclusive _kChunkOpLock happen on the same term.
    opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

    // Take _kChunkOpLock in exclusive mode to prevent concurrent chunk modifications and generate
    // strictly monotonously increasing collection placement versions
    Lock::ExclusiveLock lk(opCtx, _kChunkOpLock);

    // Get collection entry and max chunk version for this namespace.
    auto swCollAndVersion = getCollectionAndVersion(opCtx, _localConfigShard.get(), nss);

    if (!swCollAndVersion.isOK()) {
        return swCollAndVersion.getStatus().withContext(
            str::stream() << "splitChunk cannot split chunk " << range.toString() << ".");
    }

    const auto [coll, version] = std::move(swCollAndVersion.getValue());
    auto collPlacementVersion = version;

    if (coll.getUnsplittable()) {
        return {
            ErrorCodes::NamespaceNotSharded,
            str::stream() << "Can't execute splitChunk on unsharded collection "
                          << nss.toStringForErrorMsg(),
        };
    }

    // Return an error if collection epoch does not match epoch of request.
    if (coll.getEpoch() != requestEpoch ||
        (requestTimestamp && coll.getTimestamp() != requestTimestamp)) {
        uasserted(StaleEpochInfo(nss, ShardVersion{}, ShardVersion{}),
                  str::stream() << "splitChunk cannot split chunk " << range.toString()
                                << ". Epoch of collection '" << nss.toStringForErrorMsg()
                                << "' has changed."
                                << " Current epoch: " << coll.getEpoch()
                                << ", cmd epoch: " << requestEpoch);
    }

    // Find the chunk history.
    const auto origChunkStatus = _findChunkOnConfig(
        opCtx, coll.getUuid(), coll.getEpoch(), coll.getTimestamp(), range.getMin());
    if (!origChunkStatus.isOK()) {
        repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);
        return origChunkStatus.getStatus();
    }

    auto splitChunkResult = _splitChunkInTransaction(opCtx,
                                                     nss,
                                                     range,
                                                     shardName,
                                                     origChunkStatus.getValue(),
                                                     collPlacementVersion,
                                                     splitPoints);

    // Release the _kChunkOpLock to avoid blocking other operations for longer than necessary
    lk.unlock();

    // Log changes
    BSONObjBuilder logDetail;
    {
        BSONObjBuilder b(logDetail.subobjStart("before"));
        b.append(ChunkType::min(), range.getMin());
        b.append(ChunkType::max(), range.getMax());
        collPlacementVersion.serialize(ChunkType::lastmod(), &b);
    }

    if (splitChunkResult.newChunks.size() == 2) {
        appendShortVersion(&logDetail.subobjStart("left"), splitChunkResult.newChunks.at(0));
        appendShortVersion(&logDetail.subobjStart("right"), splitChunkResult.newChunks.at(1));
        logDetail.append("owningShard", shardName);

        ShardingLogging::get(opCtx)->logChange(opCtx,
                                               "split",
                                               nss,
                                               logDetail.obj(),
                                               WriteConcernOptions(),
                                               _localConfigShard,
                                               _localCatalogClient.get());
    } else {
        BSONObj beforeDetailObj = logDetail.obj();
        BSONObj firstDetailObj = beforeDetailObj.getOwned();
        const int newChunksSize = splitChunkResult.newChunks.size();

        for (int i = 0; i < newChunksSize; i++) {
            BSONObjBuilder chunkDetail;
            chunkDetail.appendElements(beforeDetailObj);
            chunkDetail.append("number", i + 1);
            chunkDetail.append("of", newChunksSize);
            appendShortVersion(&chunkDetail.subobjStart("chunk"), splitChunkResult.newChunks.at(i));
            chunkDetail.append("owningShard", shardName);

            const auto status =
                ShardingLogging::get(opCtx)->logChangeChecked(opCtx,
                                                              "multi-split",
                                                              nss,
                                                              chunkDetail.obj(),
                                                              WriteConcernOptions(),
                                                              _localConfigShard,
                                                              _localCatalogClient.get());

            // Stop logging if the last log op failed because the primary stepped down
            if (status.code() == ErrorCodes::InterruptedDueToReplStateChange)
                break;
        }
    }

    return ShardAndCollectionPlacementVersions{
        splitChunkResult.currentMaxVersion /*shardPlacementVersion*/,
        splitChunkResult.currentMaxVersion /*collectionPlacementVersion*/};
}

void ShardingCatalogManager::_mergeChunksInTransaction(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const UUID& collectionUUID,
    const ChunkVersion& mergeVersion,
    const Timestamp& validAfter,
    const ChunkRange& chunkRange,
    const ShardId& shardId,
    std::shared_ptr<std::vector<ChunkType>> chunksToMerge) {
    auto updateChunksFn =
        [chunksToMerge, collectionUUID, mergeVersion, validAfter, chunkRange, shardId](
            const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
            // Check the merge chunk precondition, chunks must not have moved.
            auto countRequest = buildCountChunksInRangeCommand(collectionUUID, shardId, chunkRange);
            return txnClient
                .runCommand(NamespaceString::kConfigsvrChunksNamespace.dbName(), countRequest)
                .thenRunOn(txnExec)
                .then([&txnClient, chunksToMerge, mergeVersion, validAfter](auto commandResponse) {
                    auto countResponse =
                        uassertStatusOK(CursorResponse::parseFromBSON(commandResponse));
                    uint64_t docCount = 0;
                    auto firstBatch = countResponse.getBatch();
                    if (!firstBatch.empty()) {
                        auto countObj = firstBatch.front();
                        docCount = countObj.getIntField(ChunkType::collectionUUID.name());
                    }
                    uassert(ErrorCodes::BadValue,
                            str::stream()
                                << "Could not meet precondition to execute merge, expected "
                                << chunksToMerge->size() << " chunks, but found " << docCount,
                            docCount == chunksToMerge->size());

                    // Construct the new chunk by taking `min` from the first merged chunk and `max`
                    // from the last.
                    write_ops::UpdateCommandRequest updateOp(
                        NamespaceString::kConfigsvrChunksNamespace);
                    updateOp.setUpdates({[&] {
                        write_ops::UpdateOpEntry entry;

                        ChunkType mergedChunk(chunksToMerge->front());
                        entry.setQ(BSON(ChunkType::name(mergedChunk.getName())));
                        mergedChunk.setRange(
                            {chunksToMerge->front().getMin(), chunksToMerge->back().getMax()});

                        // Fill in additional details for sending through transaction.
                        mergedChunk.setVersion(mergeVersion);
                        mergedChunk.setEstimatedSizeBytes(boost::none);

                        mergedChunk.setOnCurrentShardSince(validAfter);
                        mergedChunk.setHistory({ChunkHistory(*mergedChunk.getOnCurrentShardSince(),
                                                             mergedChunk.getShard())});

                        entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(
                            mergedChunk.toConfigBSON()));
                        entry.setMulti(false);

                        return entry;
                    }()});

                    return txnClient.runCRUDOp(updateOp, {});
                })
                .thenRunOn(txnExec)
                .then([&txnClient,
                       collectionUUID,
                       shardId,
                       // Delete the rest of the chunks to be merged. Remember not to delete the
                       // first chunk we're expanding.
                       chunkRangeToDelete =
                           ChunkRange(chunksToMerge->front().getMax(),
                                      chunksToMerge->back().getMax())](auto chunkUpdateResponse) {
                    uassertStatusOK(chunkUpdateResponse.toStatus());

                    BSONObjBuilder queryBuilder;
                    queryBuilder << ChunkType::collectionUUID << collectionUUID;
                    queryBuilder << ChunkType::shard(shardId.toString());
                    queryBuilder << ChunkType::min(BSON("$gte" << chunkRangeToDelete.getMin()));
                    queryBuilder << ChunkType::min(BSON("$lt" << chunkRangeToDelete.getMax()));

                    write_ops::DeleteCommandRequest deleteOp(
                        NamespaceString::kConfigsvrChunksNamespace);
                    deleteOp.setDeletes([&] {
                        std::vector<write_ops::DeleteOpEntry> deletes;
                        write_ops::DeleteOpEntry entry;
                        entry.setQ(queryBuilder.obj());
                        entry.setMulti(true);
                        return std::vector<write_ops::DeleteOpEntry>{entry};
                    }());

                    return txnClient.runCRUDOp(deleteOp, {});
                })
                .thenRunOn(txnExec)
                .then([](auto removeChunkResponse) {
                    uassertStatusOK(removeChunkResponse.toStatus());

                    LOGV2_DEBUG(
                        6583805, 1, "Finished all transaction operations in merge chunk command");
                })
                .semi();
        };

    auto executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
    auto inlineExecutor = std::make_shared<executor::InlineExecutor>();

    txn_api::SyncTransactionWithRetries txn(opCtx, executor, nullptr, inlineExecutor);
    txn.run(opCtx, updateChunksFn);
}

StatusWith<ShardingCatalogManager::ShardAndCollectionPlacementVersions>
ShardingCatalogManager::commitChunksMerge(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          const boost::optional<OID>& epoch,
                                          const boost::optional<Timestamp>& timestamp,
                                          const UUID& requestCollectionUUID,
                                          const ChunkRange& chunkRange,
                                          const ShardId& shardId) {

    // Mark opCtx as interruptible to ensure that all reads and writes to the metadata collections
    // under the exclusive _kChunkOpLock happen on the same term.
    opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

    // Take _kChunkOpLock in exclusive mode to prevent concurrent chunk modifications and generate
    // strictly monotonously increasing collection placement versions
    Lock::ExclusiveLock lk(opCtx, _kChunkOpLock);

    // 1. Retrieve the initial collection placement version info to build up the logging info.
    const auto [coll, collPlacementVersion] =
        uassertStatusOK(getCollectionAndVersion(opCtx, _localConfigShard.get(), nss));
    uassert(StaleEpochInfo(nss, ShardVersion{}, ShardVersion{}),
            "Collection changed",
            (!epoch || collPlacementVersion.epoch() == epoch) &&
                (!timestamp || collPlacementVersion.getTimestamp() == timestamp));

    if (coll.getUuid() != requestCollectionUUID) {
        return {
            ErrorCodes::InvalidUUID,
            str::stream() << "UUID of collection does not match UUID of request. Colletion UUID: "
                          << coll.getUuid() << ", request UUID: " << requestCollectionUUID};
    }

    // 2. Retrieve the list of chunks belonging to the requested shard + key range.
    const auto shardChunksInRangeQuery = [&shardId, &chunkRange, collUuid = coll.getUuid()]() {
        BSONObjBuilder queryBuilder;
        queryBuilder << ChunkType::collectionUUID << collUuid;
        queryBuilder << ChunkType::shard(shardId.toString());
        queryBuilder << ChunkType::min(BSON("$gte" << chunkRange.getMin()));
        queryBuilder << ChunkType::min(BSON("$lt" << chunkRange.getMax()));
        return queryBuilder.obj();
    }();

    const auto shardChunksInRangeResponse =
        uassertStatusOK(_localConfigShard->exhaustiveFindOnConfig(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            repl::ReadConcernLevel::kLocalReadConcern,
            NamespaceString::kConfigsvrChunksNamespace,
            shardChunksInRangeQuery,
            BSON(ChunkType::min << 1),
            boost::none));

    // Check if the chunk(s) have already been merged. If so, return success.
    if (shardChunksInRangeResponse.docs.size() == 1) {
        auto chunk = uassertStatusOK(ChunkType::parseFromConfigBSON(
            shardChunksInRangeResponse.docs.back(), coll.getEpoch(), coll.getTimestamp()));
        uassert(
            ErrorCodes::IllegalOperation,
            str::stream() << "could not merge chunks, shard " << shardId
                          << " does not contain a sequence of chunks that exactly fills the range "
                          << chunkRange.toString(),
            chunk.getRange() == chunkRange);

        const auto currentShardPlacementVersion = getShardPlacementVersion(
            opCtx, _localConfigShard.get(), coll, shardId, collPlacementVersion);

        // Makes sure that the last thing we read in getCollectionAndVersion and
        // getShardPlacementVersion gets majority written before to return from this command,
        // otherwise next RoutingInfo cache refresh from the shard may not see those newest
        // information.
        repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);
        return ShardAndCollectionPlacementVersions{currentShardPlacementVersion,
                                                   collPlacementVersion};
    }

    // 3. Prepare the data for the merge
    //    and ensure that the retrieved list of chunks covers the whole range.

    // The `validAfter` field must always be set. If not existing, it means the chunk
    // always belonged to the same shard, hence it's valid to set `0` as the time at
    // which the chunk started being valid.
    Timestamp validAfter{0};

    auto chunksToMerge = std::make_shared<std::vector<ChunkType>>();
    chunksToMerge->reserve(shardChunksInRangeResponse.docs.size());
    for (const auto& chunkDoc : shardChunksInRangeResponse.docs) {
        auto chunk = uassertStatusOK(
            ChunkType::parseFromConfigBSON(chunkDoc, coll.getEpoch(), coll.getTimestamp()));
        if (chunksToMerge->empty()) {
            uassert(ErrorCodes::IllegalOperation,
                    str::stream()
                        << "could not merge chunks, shard " << shardId
                        << " does not contain a sequence of chunks that exactly fills the range "
                        << chunkRange.toString(),
                    chunk.getMin().woCompare(chunkRange.getMin()) == 0);
        } else {
            uassert(ErrorCodes::IllegalOperation,
                    str::stream()
                        << "could not merge chunks, shard " << shardId
                        << " does not contain a sequence of chunks that exactly fills the range "
                        << chunkRange.toString(),
                    chunk.getMin().woCompare(chunksToMerge->back().getMax()) == 0);
        }

        // Get the `validAfter` field from the most recent chunk placed on the shard
        if (!chunk.getHistory().empty()) {
            const auto& chunkValidAfter = chunk.getHistory().front().getValidAfter();
            if (validAfter < chunkValidAfter) {
                validAfter = chunkValidAfter;
            }
        }

        chunksToMerge->push_back(std::move(chunk));
    }
    uassert(ErrorCodes::IllegalOperation,
            str::stream() << "could not merge chunks, shard " << shardId
                          << " does not contain a sequence of chunks that exactly fills the range "
                          << chunkRange.toString(),
            !chunksToMerge->empty() &&
                chunksToMerge->back().getMax().woCompare(chunkRange.getMax()) == 0);

    ChunkVersion initialVersion = collPlacementVersion;
    ChunkVersion mergeVersion = initialVersion;
    mergeVersion.incMinor();

    // 4. apply the batch of updates to local metadata
    _mergeChunksInTransaction(
        opCtx, nss, coll.getUuid(), mergeVersion, validAfter, chunkRange, shardId, chunksToMerge);

    // 5. release the _kChunkOpLock to avoid blocking other operations for longer than necessary
    lk.unlock();

    // 6. log changes
    logMergeToChangelog(opCtx,
                        nss,
                        initialVersion,
                        mergeVersion,
                        shardId,
                        chunkRange,
                        chunksToMerge->size(),
                        _localConfigShard,
                        _localCatalogClient.get());

    return ShardAndCollectionPlacementVersions{mergeVersion /*shardPlacementVersion*/,
                                               mergeVersion /*collectionPlacementVersion*/};
}

StatusWith<std::pair<ShardingCatalogManager::ShardAndCollectionPlacementVersions, int>>
ShardingCatalogManager::commitMergeAllChunksOnShard(OperationContext* opCtx,
                                                    const NamespaceString& nss,
                                                    const ShardId& shardId,
                                                    int maxNumberOfChunksToMerge,
                                                    int maxTimeProcessingChunksMS) {
    // Mark opCtx as interruptible to ensure that all reads and writes to the metadata collections
    // under the exclusive _kChunkOpLock happen on the same term.
    opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

    // Retry the commit a fixed number of  times before failing: the discovery of chunks to merge
    // happens before acquiring the `_kChunkOpLock` in order not to block  for too long concurrent
    // chunk operations. This implies that other chunk operations for the same collection could
    // potentially commit before acquiring the lock, forcing to repeat the discovey.
    const int MAX_RETRIES = 5;
    int nRetries = 0;

    // Store the 'min' field of the first mergeable chunk found. This is to speed up the processing
    // of the chunks for the succeeding retries.
    boost::optional<BSONObj> firstMergeableChunkMin;

    while (nRetries < MAX_RETRIES) {
        Timer tElapsed;

        if (MONGO_unlikely(hangMergeAllChunksUntilReachingTimeout.shouldFail())) {
            sleepFor(Milliseconds(maxTimeProcessingChunksMS + 1));
        }

        // 1. Retrieve the collection entry and the initial version.
        const auto [coll, originalVersion] =
            uassertStatusOK(getCollectionAndVersion(opCtx, _localConfigShard.get(), nss));

        uassert(ErrorCodes::NamespaceNotSharded,
                str::stream() << "Can't execute mergeChunks on unsharded collection "
                              << nss.toStringForErrorMsg(),
                !coll.getUnsplittable());

        const auto& collUuid = coll.getUuid();
        const auto& keyPattern = coll.getKeyPattern();
        auto newVersion = originalVersion;

        // 2. Retrieve the list of mergeable chunks belonging to the requested shard/collection.
        // A chunk is mergeable when the following conditions are honored:
        // - Non-jumbo
        // - The last migration occurred before the current history window
        DBDirectClient client{opCtx};

        const auto chunksBelongingToShardCursor{client.find(std::invoke([&] {
            FindCommandRequest chunksFindRequest{NamespaceString::kConfigsvrChunksNamespace};
            chunksFindRequest.setFilter(std::invoke([&]() {
                BSONObjBuilder filterBuilder;
                filterBuilder << ChunkType::collectionUUID << collUuid;
                filterBuilder << ChunkType::shard(shardId.toString());
                if (firstMergeableChunkMin) {
                    filterBuilder << ChunkType::min.query("$gte", *firstMergeableChunkMin);
                    firstMergeableChunkMin = boost::none;
                }
                filterBuilder << ChunkType::onCurrentShardSince.lt(
                    getOldestTimestampSupportedForSnapshotHistory(opCtx));
                filterBuilder << ChunkType::jumbo.ne(true);
                return filterBuilder.obj();
            }));
            chunksFindRequest.setSort(BSON(ChunkType::min << 1));
            return chunksFindRequest;
        }))};

        tassert(ErrorCodes::OperationFailed,
                str::stream() << "Failed to establish a cursor for reading "
                              << nss.toStringForErrorMsg() << " from local storage",
                chunksBelongingToShardCursor);

        // 3. Prepare the data for the merge.

        // Track the total and per-range number of merged chunks
        std::pair<int, std::vector<size_t>> numMergedChunks;

        const auto newChunks = std::invoke([&]() -> std::shared_ptr<std::vector<ChunkType>> {
            auto newChunks = std::make_shared<std::vector<ChunkType>>();
            const Timestamp minValidTimestamp = Timestamp(0, 1);

            BSONObj rangeMin, rangeMax;
            Timestamp rangeOnCurrentShardSince = minValidTimestamp;
            int nChunksInRange = 0;

            // Lambda generating the new chunk to be committed if a merge can be issued on the range
            auto processRange = [&]() {
                if (nChunksInRange > 1) {
                    newVersion.incMinor();
                    ChunkType newChunk(
                        collUuid, {rangeMin.getOwned(), rangeMax.copy()}, newVersion, shardId);
                    newChunk.setOnCurrentShardSince(rangeOnCurrentShardSince);
                    newChunk.setHistory({ChunkHistory{rangeOnCurrentShardSince, shardId}});
                    numMergedChunks.first += nChunksInRange;
                    numMergedChunks.second.push_back(nChunksInRange);
                    newChunks->push_back(std::move(newChunk));
                    if (!firstMergeableChunkMin) {
                        firstMergeableChunkMin = newChunks->at(0).getMin().copy();
                    }
                }
                nChunksInRange = 0;
                rangeOnCurrentShardSince = minValidTimestamp;
            };

            const auto zonesCursor{client.find(std::invoke([&nss]() {
                FindCommandRequest zonesFindRequest{TagsType::ConfigNS};
                zonesFindRequest.setSort(BSON(TagsType::min << 1));
                zonesFindRequest.setFilter(BSON(TagsType::ns(
                    NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault()))));
                zonesFindRequest.setProjection(BSON(TagsType::min << 1 << TagsType::max << 1));
                return zonesFindRequest;
            }))};

            // Initialize bounds lower than any zone [(), Minkey) so that it can be later advanced
            boost::optional<ChunkRange> currentZone = ChunkRange(BSONObj(), keyPattern.globalMin());

            auto advanceZoneIfNeeded = [&](const BSONObj& advanceZoneUpToThisBound) {
                // This lambda advances zones taking into account the whole shard key space,
                // also considering the "no-zone" as a zone itself.
                //
                // Example:
                // - Zones set by the user: [1, 10), [20, 30), [30, 40)
                // - Real zones: [Minkey, 1), [1, 10), [10, 20), [20, 30), [30, 40), [40, MaxKey)
                //
                // Returns a bool indicating whether the zone has changed or not.
                bool zoneChanged = false;
                while (currentZone &&
                       advanceZoneUpToThisBound.woCompare(currentZone->getMin()) > 0 &&
                       advanceZoneUpToThisBound.woCompare(currentZone->getMax()) > 0) {
                    zoneChanged = true;
                    if (zonesCursor->more()) {
                        const auto nextZone = zonesCursor->peekFirst();
                        const auto nextZoneMin = keyPattern.extendRangeBound(
                            nextZone.getObjectField(TagsType::min()), false);
                        if (nextZoneMin.woCompare(currentZone->getMax()) > 0) {
                            currentZone = ChunkRange(currentZone->getMax(), nextZoneMin);
                        } else {
                            const auto nextZoneMax = keyPattern.extendRangeBound(
                                nextZone.getObjectField(TagsType::max()), false);
                            currentZone = ChunkRange(nextZoneMin, nextZoneMax);
                            zonesCursor->nextSafe();  // Advance cursor
                        }
                    } else {
                        currentZone = boost::none;
                    }
                }
                return zoneChanged;
            };

            while (chunksBelongingToShardCursor->more()) {
                const auto chunkDoc = chunksBelongingToShardCursor->nextSafe();

                const auto& chunkMin = chunkDoc.getObjectField(ChunkType::min());
                const auto& chunkMax = chunkDoc.getObjectField(ChunkType::max());
                const Timestamp chunkOnCurrentShardSince = [&]() {
                    Timestamp t = minValidTimestamp;
                    bsonExtractTimestampField(chunkDoc, ChunkType::onCurrentShardSince(), &t)
                        .ignore();
                    return t;
                }();

                bool zoneChanged = advanceZoneIfNeeded(chunkMax);
                if (rangeMax.woCompare(chunkMin) != 0 || zoneChanged) {
                    processRange();
                }

                if (nChunksInRange == 0) {
                    rangeMin = chunkMin.getOwned();
                }
                rangeMax = chunkMax.getOwned();

                if (chunkOnCurrentShardSince > rangeOnCurrentShardSince) {
                    rangeOnCurrentShardSince = chunkOnCurrentShardSince;
                }
                nChunksInRange++;

                // Stop looking for additional mergeable chunks if `maxNumberOfChunksToMerge` is
                // reached.
                if (numMergedChunks.first + nChunksInRange >= maxNumberOfChunksToMerge) {
                    break;
                }

                // Stop looking for additional mergeable chunks if the `maxTimeProcessingChunksMS`
                // is exceeded. The main reason of this timeout is to reduce the likelihood of
                // failing on commit because of a concurrent migration.
                //
                // Note that we'll only timeout if we've already found mergeable chunks, otherwise
                // we'll continue looking. Although it'll be more likely to fail on commit due to a
                // concurrent migration, we'll increase the success rate for the next retry due to
                // knowing the `firstMergeableChunkMin`.
                if (!newChunks->empty() && tElapsed.millis() > maxTimeProcessingChunksMS) {
                    break;
                }
            }
            processRange();

            return newChunks;
        });

        // If there is no mergeable chunk for the given shard, return success.
        if (newChunks->empty()) {
            const auto currentShardPlacementVersion = getShardPlacementVersion(
                opCtx, _localConfigShard.get(), coll, shardId, originalVersion);

            // Makes sure that the last thing we read in getCollectionAndVersion and
            // getShardPlacementVersion gets majority written before to return from this command,
            // otherwise next RoutingInfo cache refresh from the shard may not see those newest
            // information.
            repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);

            return std::pair<ShardingCatalogManager::ShardAndCollectionPlacementVersions, int>{
                ShardAndCollectionPlacementVersions{currentShardPlacementVersion, originalVersion},
                0};
        }

        {
            // Take _kChunkOpLock in exclusive mode to prevent concurrent chunk modifications
            Lock::ExclusiveLock lk(opCtx, _kChunkOpLock);

            // Precondition for merges to be safely committed: make sure the current collection
            // placement version fits the one retrieved before acquiring the lock.
            const auto [_, versionRetrievedUnderLock] =
                uassertStatusOK(getCollectionAndVersion(opCtx, _localConfigShard.get(), nss));

            if (originalVersion != versionRetrievedUnderLock) {
                nRetries++;
                continue;
            }

            // 4. Commit the new routing table changes to the sharding catalog.
            mergeAllChunksOnShardInTransaction(opCtx, collUuid, shardId, newChunks);
        }

        // 5. Log changes
        auto prevVersion = originalVersion;
        invariant(numMergedChunks.second.size() == newChunks->size());
        for (auto i = 0U; i < newChunks->size(); ++i) {
            const auto& newChunk = newChunks->at(i);
            logMergeToChangelog(opCtx,
                                nss,
                                prevVersion,
                                newChunk.getVersion(),
                                shardId,
                                newChunk.getRange(),
                                numMergedChunks.second.at(i),
                                _localConfigShard,
                                _localCatalogClient.get(),
                                true /*isAutoMerge*/);

            // we can know the prevVersion since newChunks vector is sorted by version
            prevVersion = newChunk.getVersion();
        }

        return std::pair<ShardingCatalogManager::ShardAndCollectionPlacementVersions, int>{
            ShardAndCollectionPlacementVersions{newVersion /*shardPlacementVersion*/,
                                                newVersion /*collPlacementVersion*/},
            numMergedChunks.first};
    }

    uasserted(ErrorCodes::ConflictingOperationInProgress,
              str::stream() << "Tried to commit the operation " << nRetries
                            << " times before giving up due to concurrent chunk operations "
                               "happening for the same collection");
}

StatusWith<ShardingCatalogManager::ShardAndCollectionPlacementVersions>
ShardingCatalogManager::commitChunkMigration(OperationContext* opCtx,
                                             const NamespaceString& nss,
                                             const ChunkType& migratedChunk,
                                             const OID& collectionEpoch,
                                             const Timestamp& collectionTimestamp,
                                             const ShardId& fromShard,
                                             const ShardId& toShard) {
    uassertStatusOK(
        ShardKeyPattern::checkShardKeyIsValidForMetadataStorage(migratedChunk.getMin()));
    uassertStatusOK(
        ShardKeyPattern::checkShardKeyIsValidForMetadataStorage(migratedChunk.getMax()));

    // Mark opCtx as interruptible to ensure that all reads and writes to the metadata collections
    // under the exclusive _kChunkOpLock happen on the same term.
    opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

    // Must hold the shard lock until the entire commit finishes to serialize with removeShard.
    Lock::SharedLock shardLock(opCtx, _kShardMembershipLock);

    auto shardResult = uassertStatusOK(_localConfigShard->exhaustiveFindOnConfig(
        opCtx,
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        repl::ReadConcernLevel::kLocalReadConcern,
        NamespaceString::kConfigsvrShardsNamespace,
        BSON(ShardType::name(toShard.toString())),
        {},
        boost::none));
    uassert(ErrorCodes::ShardNotFound,
            str::stream() << "Shard " << toShard << " does not exist",
            !shardResult.docs.empty());

    auto shard = uassertStatusOK(ShardType::fromBSON(shardResult.docs.front()));
    uassert(ErrorCodes::ShardNotFound,
            str::stream() << "Shard " << toShard << " is currently draining",
            !shard.getDraining());

    // Take _kChunkOpLock in exclusive mode to prevent concurrent chunk modifications and generate
    // strictly monotonously increasing collection placement versions
    Lock::ExclusiveLock lk(opCtx, _kChunkOpLock);

    auto findCollResponse = uassertStatusOK(_localConfigShard->exhaustiveFindOnConfig(
        opCtx,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        repl::ReadConcernLevel::kLocalReadConcern,
        CollectionType::ConfigNS,
        BSON(CollectionType::kNssFieldName
             << NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault())),
        {},
        1));
    uassert(ErrorCodes::ConflictingOperationInProgress,
            "Collection does not exist",
            !findCollResponse.docs.empty());

    const CollectionType coll(findCollResponse.docs[0]);
    uassert(ErrorCodes::ConflictingOperationInProgress,
            "Can't execute moveChunk because migrations for this collection are disallowed",
            coll.getAllowMigrations() && coll.getPermitMigrations());

    if (coll.getUnsplittable()) {
        return {
            ErrorCodes::NamespaceNotSharded,
            str::stream() << "Can't execute moveChunk on the unsharded collection "
                          << coll.getNss().toStringForErrorMsg(),
        };
    }
    const auto findChunkQuery = BSON(ChunkType::collectionUUID() << coll.getUuid());

    auto findResponse = uassertStatusOK(_localConfigShard->exhaustiveFindOnConfig(
        opCtx,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        repl::ReadConcernLevel::kLocalReadConcern,
        NamespaceString::kConfigsvrChunksNamespace,
        findChunkQuery,
        BSON(ChunkType::lastmod << -1),
        1));
    uassert(ErrorCodes::IncompatibleShardingMetadata,
            str::stream() << "Tried to find max chunk version for collection '"
                          << nss.toStringForErrorMsg() << ", but found no chunks",
            !findResponse.docs.empty());

    const auto chunk = uassertStatusOK(
        ChunkType::parseFromConfigBSON(findResponse.docs[0], coll.getEpoch(), coll.getTimestamp()));
    const auto& currentCollectionPlacementVersion = chunk.getVersion();

    if (MONGO_unlikely(migrationCommitVersionError.shouldFail())) {
        uasserted(StaleEpochInfo(nss, ShardVersion{}, ShardVersion{}),
                  "Failpoint 'migrationCommitVersionError' generated error");
    }

    // Check that current collection epoch and timestamp still matches the one sent by the shard.
    // This is to spot scenarios in which the collection has been dropped and recreated or had its
    // shard key refined since the migration began.
    if (currentCollectionPlacementVersion.epoch() != collectionEpoch ||
        currentCollectionPlacementVersion.getTimestamp() != collectionTimestamp) {
        uasserted(StaleEpochInfo(nss, ShardVersion{}, ShardVersion{}),
                  str::stream() << "The epoch of collection '" << nss.toStringForErrorMsg()
                                << "' has changed since the migration began. The config server's "
                                   "collection placement version epoch is now '"
                                << currentCollectionPlacementVersion.epoch().toString()
                                << "', but the shard's is " << collectionEpoch.toString()
                                << "'. Aborting migration commit for chunk ("
                                << migratedChunk.getRange().toString() << ").");
    }

    uassert(4683300,
            "Config server rejecting commitChunkMigration request that does not have a "
            "ChunkVersion",
            migratedChunk.isVersionSet() && migratedChunk.getVersion().isSet());

    // Check if range still exists and which shard owns it
    auto swCurrentChunk = findChunkContainingRange(opCtx,
                                                   _localConfigShard.get(),
                                                   coll.getUuid(),
                                                   coll.getEpoch(),
                                                   coll.getTimestamp(),
                                                   migratedChunk.getRange());

    if (!swCurrentChunk.isOK()) {
        return swCurrentChunk.getStatus();
    }

    const auto currentChunk = std::move(swCurrentChunk.getValue());

    if (currentChunk.getShard() == toShard) {
        // The commit was already done successfully
        const auto currentShardPlacementVersion = getShardPlacementVersion(
            opCtx, _localConfigShard.get(), coll, fromShard, currentCollectionPlacementVersion);
        // Makes sure that the last thing we read in findChunkContainingRange,
        // getShardPlacementVersion, and getCollectionAndVersion gets majority written before to
        // return from this command, otherwise next RoutingInfo cache refresh from the shard may not
        // see those newest information.
        repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);
        return ShardAndCollectionPlacementVersions{currentShardPlacementVersion,
                                                   currentCollectionPlacementVersion};
    }

    uassert(4914702,
            str::stream() << "Migrated  chunk " << migratedChunk.toString()
                          << " from ns: " << nss.toStringForErrorMsg() << " not owned by donor "
                          << fromShard << " neither by recipient " << toShard,
            currentChunk.getShard() == fromShard);

    auto compareResult = migratedChunk.getVersion() <=> currentChunk.getVersion();
    if (compareResult == std::partial_ordering::unordered ||
        compareResult == std::partial_ordering::less) {
        return {ErrorCodes::ConflictingOperationInProgress,
                str::stream()
                    << "Rejecting migration request because the version of the requested chunk "
                    << migratedChunk.toConfigBSON() << "(" << migratedChunk.getVersion().toString()
                    << ") is older than the version of the current chunk "
                    << currentChunk.toConfigBSON() << "(" << currentChunk.getVersion().toString()
                    << ") on shard " << fromShard.toString()};
    }

    // Generate the new versions of migratedChunk and controlChunk. Migrating chunk's minor version
    // will be 0.
    uint32_t minVersionIncrement = 0;
    ChunkType newMigratedChunk{currentChunk};
    newMigratedChunk.setRange(migratedChunk.getRange());
    newMigratedChunk.setShard(toShard);
    newMigratedChunk.setVersion(ChunkVersion(
        {currentCollectionPlacementVersion.epoch(),
         currentCollectionPlacementVersion.getTimestamp()},
        {currentCollectionPlacementVersion.majorVersion() + 1, minVersionIncrement++}));

    // Set the commit time of the migration.
    const auto currentTime = VectorClock::get(opCtx)->getTime();
    const auto validAfter = currentTime.clusterTime().asTimestamp();

    // Copy the complete history.
    auto newHistory = currentChunk.getHistory();

    // Drop old history. Keep at least 1 entry so ChunkInfo::getShardIdAt finds valid history for
    // any query younger than the history window.
    if (!MONGO_unlikely(skipExpiringOldChunkHistory.shouldFail())) {
        int entriesDeleted = 0;
        while (newHistory.size() > 1 &&
               newHistory.back().getValidAfter().getSecs() + getHistoryWindowInSeconds() <
                   validAfter.getSecs()) {
            newHistory.pop_back();
            ++entriesDeleted;
        }

        logv2::DynamicAttributes attrs;
        attrs.add("entriesDeleted", entriesDeleted);
        if (!newHistory.empty()) {
            attrs.add("oldestEntryValidAfter", newHistory.back().getValidAfter());
        }

        LOGV2_DEBUG(4778500, 1, "Deleted old chunk history entries", attrs);
    }

    if (!newHistory.empty() && newHistory.front().getValidAfter() >= validAfter) {
        return {ErrorCodes::IncompatibleShardingMetadata,
                str::stream() << "The chunk history for chunk with namespace "
                              << nss.toStringForErrorMsg() << " and min key "
                              << migratedChunk.getMin() << " is corrupted. The last validAfter "
                              << newHistory.back().getValidAfter().toString()
                              << " is greater or equal to the new validAfter "
                              << validAfter.toString()};
    }
    newMigratedChunk.setOnCurrentShardSince(validAfter);
    newHistory.emplace(newHistory.begin(),
                       ChunkHistory(*newMigratedChunk.getOnCurrentShardSince(), toShard));
    newMigratedChunk.setHistory(std::move(newHistory));

    std::vector<ChunkType> newSplitChunks;
    {
        // This scope handles the `moveRange` scenario, potentially create chunks on the sides of
        // the moved range
        const auto& movedChunkMin = newMigratedChunk.getMin();
        const auto& movedChunkMax = newMigratedChunk.getMax();
        const auto& movedChunkVersion = newMigratedChunk.getVersion();

        if (SimpleBSONObjComparator::kInstance.evaluate(movedChunkMin != currentChunk.getMin())) {
            // Left chunk: inherits history and min of the original chunk, max equal to the min of
            // the new moved range. Major version equal to the new chunk's one, min version bumped.
            ChunkType leftSplitChunk = currentChunk;
            leftSplitChunk.setName(OID::gen());
            leftSplitChunk.setRange({leftSplitChunk.getMin(), movedChunkMin});
            leftSplitChunk.setVersion(
                ChunkVersion({movedChunkVersion.epoch(), movedChunkVersion.getTimestamp()},
                             {movedChunkVersion.majorVersion(), minVersionIncrement++}));
            newSplitChunks.emplace_back(std::move(leftSplitChunk));
        }

        if (SimpleBSONObjComparator::kInstance.evaluate(movedChunkMax != currentChunk.getMax())) {
            // Right chunk: min equal to the max of the new moved range, inherits history and min of
            // the original chunk. Major version equal to the new chunk's one, min version bumped.
            ChunkType rightSplitChunk = currentChunk;
            rightSplitChunk.setName(OID::gen());
            rightSplitChunk.setRange({movedChunkMax, rightSplitChunk.getMax()});
            rightSplitChunk.setVersion(
                ChunkVersion({movedChunkVersion.epoch(), movedChunkVersion.getTimestamp()},
                             {movedChunkVersion.majorVersion(), minVersionIncrement++}));
            newSplitChunks.emplace_back(std::move(rightSplitChunk));
        }
    }

    auto controlChunk = getControlChunkForMigrate(opCtx,
                                                  _localConfigShard.get(),
                                                  coll.getUuid(),
                                                  coll.getEpoch(),
                                                  coll.getTimestamp(),
                                                  currentChunk,
                                                  fromShard);
    boost::optional<ChunkType> newControlChunk = boost::none;

    if (controlChunk) {
        // Find the chunk history.
        newControlChunk = uassertStatusOK(_findChunkOnConfig(
            opCtx, coll.getUuid(), coll.getEpoch(), coll.getTimestamp(), controlChunk->getMin()));

        // Setting control chunk's minor version to 1 on the donor shard.
        newControlChunk->setVersion(ChunkVersion(
            {currentCollectionPlacementVersion.epoch(),
             currentCollectionPlacementVersion.getTimestamp()},
            {currentCollectionPlacementVersion.majorVersion() + 1, minVersionIncrement++}));
    }

    _commitChunkMigrationInTransaction(
        opCtx, nss, newMigratedChunk, newSplitChunks, newControlChunk, fromShard);

    ShardAndCollectionPlacementVersions response;
    if (!newControlChunk) {
        // We migrated the last chunk from the donor shard.
        response.collectionPlacementVersion = newMigratedChunk.getVersion();
        response.shardPlacementVersion =
            ChunkVersion({currentCollectionPlacementVersion.epoch(),
                          currentCollectionPlacementVersion.getTimestamp()},
                         {0, 0});
    } else {
        response.collectionPlacementVersion = newControlChunk->getVersion();
        response.shardPlacementVersion = newControlChunk->getVersion();
    }
    return response;
}

StatusWith<ChunkType> ShardingCatalogManager::_findChunkOnConfig(OperationContext* opCtx,
                                                                 const UUID& uuid,
                                                                 const OID& epoch,
                                                                 const Timestamp& timestamp,
                                                                 const BSONObj& key) {
    const auto query = BSON(ChunkType::collectionUUID << uuid << ChunkType::min(key));
    auto findResponse = _localConfigShard->exhaustiveFindOnConfig(
        opCtx,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        repl::ReadConcernLevel::kLocalReadConcern,
        NamespaceString::kConfigsvrChunksNamespace,
        query,
        BSONObj(),
        1);

    if (!findResponse.isOK()) {
        return findResponse.getStatus();
    }

    const auto origChunks = std::move(findResponse.getValue().docs);
    if (origChunks.size() != 1) {
        return {ErrorCodes::IncompatibleShardingMetadata,
                str::stream() << "Tried to find the chunk for uuid" << uuid.toString()
                              << " and min key " << key.toString() << ", but found no chunks"};
    }

    return ChunkType::parseFromConfigBSON(origChunks.front(), epoch, timestamp);
}

void ShardingCatalogManager::upgradeChunksHistory(OperationContext* opCtx,
                                                  const NamespaceString& nss,
                                                  bool force,
                                                  const Timestamp& validAfter) {
    const auto shardRegistry = Grid::get(opCtx)->shardRegistry();

    // Mark opCtx as interruptible to ensure that all reads and writes to the metadata collections
    // under the exclusive _kChunkOpLock happen on the same term.
    opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

    // Take _kChunkOpLock in exclusive mode to prevent concurrent chunk splits, merges, and
    // migrations.
    Lock::ExclusiveLock lk(opCtx, _kChunkOpLock);

    const auto [coll, collPlacementVersion] =
        uassertStatusOK(getCollectionAndVersion(opCtx, _localConfigShard.get(), nss));

    if (force) {
        LOGV2(620650,
              "Resetting the 'historyIsAt40' field for all chunks in collection {namespace} in "
              "order to force all chunks' history to get recreated",
              logAttrs(nss));

        BatchedCommandRequest request([collUuid = coll.getUuid()] {
            write_ops::UpdateCommandRequest updateOp(NamespaceString::kConfigsvrChunksNamespace);
            updateOp.setUpdates({[&] {
                write_ops::UpdateOpEntry entry;
                entry.setQ(BSON(ChunkType::collectionUUID() << collUuid));
                entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(
                    BSON("$unset" << BSON(ChunkType::historyIsAt40() << ""))));
                entry.setUpsert(false);
                entry.setMulti(true);
                return entry;
            }()});
            return updateOp;
        }());

        auto response = _localConfigShard->runBatchWriteCommand(
            opCtx,
            Milliseconds(defaultConfigCommandTimeoutMS.load()),
            request,
            ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter(),
            Shard::RetryPolicy::kIdempotent);
        uassertStatusOK(response.toStatus());

        uassert(ErrorCodes::Error(5760502),
                str::stream() << "No chunks found for collection " << nss.toStringForErrorMsg(),
                response.getN() > 0);
    }

    // Find the chunk history
    const auto allChunksVector = [&, collUuid = coll.getUuid()] {
        auto findChunksResponse = uassertStatusOK(_localConfigShard->exhaustiveFindOnConfig(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            repl::ReadConcernLevel::kLocalReadConcern,
            NamespaceString::kConfigsvrChunksNamespace,
            BSON(ChunkType::collectionUUID() << collUuid),
            BSONObj(),
            boost::none));
        uassert(ErrorCodes::Error(5760503),
                str::stream() << "No chunks found for collection " << nss.toStringForErrorMsg(),
                !findChunksResponse.docs.empty());
        return std::move(findChunksResponse.docs);
    }();

    // Bump the major version in order to be guaranteed to trigger refresh on every shard
    ChunkVersion newCollectionPlacementVersion(
        {collPlacementVersion.epoch(), collPlacementVersion.getTimestamp()},
        {collPlacementVersion.majorVersion() + 1, 0});
    std::set<ShardId> shardsOwningChunks;
    for (const auto& chunk : allChunksVector) {
        auto upgradeChunk = uassertStatusOK(ChunkType::parseFromConfigBSON(
            chunk, collPlacementVersion.epoch(), collPlacementVersion.getTimestamp()));
        shardsOwningChunks.emplace(upgradeChunk.getShard());
        bool historyIsAt40 = chunk[ChunkType::historyIsAt40()].booleanSafe();
        if (historyIsAt40) {
            uassert(
                ErrorCodes::Error(5760504),
                str::stream() << "Chunk " << upgradeChunk.getName() << " in collection "
                              << nss.toStringForErrorMsg()
                              << " indicates that it has been upgraded to version 4.0, but is "
                                 "missing the history field. This indicates a corrupted routing "
                                 "table and requires a manual intervention to be fixed.",
                !upgradeChunk.getHistory().empty());
            continue;
        }

        upgradeChunk.setVersion(newCollectionPlacementVersion);
        newCollectionPlacementVersion.incMinor();

        // Construct the fresh history.
        upgradeChunk.setOnCurrentShardSince(validAfter);
        upgradeChunk.setHistory(
            {ChunkHistory{*upgradeChunk.getOnCurrentShardSince(), upgradeChunk.getShard()}});

        // Set the 'historyIsAt40' field so that it gets skipped if the command is re-run
        BSONObjBuilder chunkObjBuilder(upgradeChunk.toConfigBSON());
        chunkObjBuilder.appendBool(ChunkType::historyIsAt40(), true);

        // Run the update
        uassertStatusOK(_localCatalogClient->updateConfigDocument(
            opCtx,
            NamespaceString::kConfigsvrChunksNamespace,
            BSON(ChunkType::name(upgradeChunk.getName())),
            chunkObjBuilder.obj(),
            false,
            ShardingCatalogClient::writeConcernLocalHavingUpstreamWaiter()));
    }

    // Wait for the writes to become majority committed so that the subsequent shard refreshes can
    // see them
    const auto clientOpTime = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
    WriteConcernResult unusedWCResult;
    uassertStatusOK(waitForWriteConcern(
        opCtx, clientOpTime, defaultMajorityWriteConcernDoNotUse(), &unusedWCResult));

    for (const auto& shardId : shardsOwningChunks) {
        auto shard = uassertStatusOK(shardRegistry->getShard(opCtx, shardId));
        uassertStatusOK(Shard::CommandResponse::getEffectiveStatus(shard->runCommand(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            DatabaseName::kAdmin,
            BSON("_flushRoutingTableCacheUpdates"
                 << NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault())),
            Shard::RetryPolicy::kIdempotent)));
    }
}

void ShardingCatalogManager::clearJumboFlag(OperationContext* opCtx,
                                            const NamespaceString& nss,
                                            const OID& collectionEpoch,
                                            const ChunkRange& chunk) {
    // Mark opCtx as interruptible to ensure that all reads and writes to the metadata collections
    // under the exclusive _kChunkOpLock happen on the same term.
    opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

    // Take _kChunkOpLock in exclusive mode to prevent concurrent chunk modifications and generate
    // strictly monotonously increasing collection placement versions
    Lock::ExclusiveLock lk(opCtx, _kChunkOpLock);

    auto findCollResponse = uassertStatusOK(_localConfigShard->exhaustiveFindOnConfig(
        opCtx,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        repl::ReadConcernLevel::kLocalReadConcern,
        CollectionType::ConfigNS,
        BSON(CollectionType::kNssFieldName
             << NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault())),
        {},
        1));
    uassert(ErrorCodes::ConflictingOperationInProgress,
            "Collection does not exist",
            !findCollResponse.docs.empty());
    const CollectionType coll(findCollResponse.docs[0]);

    BSONObj targetChunkQuery =
        BSON(ChunkType::min(chunk.getMin())
             << ChunkType::max(chunk.getMax()) << ChunkType::collectionUUID << coll.getUuid());

    auto targetChunkResult = uassertStatusOK(_localConfigShard->exhaustiveFindOnConfig(
        opCtx,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        repl::ReadConcernLevel::kLocalReadConcern,
        NamespaceString::kConfigsvrChunksNamespace,
        targetChunkQuery,
        {},
        1));

    const auto targetChunkVector = std::move(targetChunkResult.docs);
    uassert(51262,
            str::stream() << "Unable to locate chunk " << chunk.toString()
                          << " from ns: " << nss.toStringForErrorMsg(),
            !targetChunkVector.empty());

    const auto targetChunk = uassertStatusOK(ChunkType::parseFromConfigBSON(
        targetChunkVector.front(), coll.getEpoch(), coll.getTimestamp()));

    if (!targetChunk.getJumbo()) {
        return;
    }

    const auto allChunksQuery = BSON(ChunkType::collectionUUID << coll.getUuid());

    // Must use local read concern because we will perform subsequent writes.
    auto findResponse = uassertStatusOK(_localConfigShard->exhaustiveFindOnConfig(
        opCtx,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        repl::ReadConcernLevel::kLocalReadConcern,
        NamespaceString::kConfigsvrChunksNamespace,
        allChunksQuery,
        BSON(ChunkType::lastmod << -1),
        1));

    const auto chunksVector = std::move(findResponse.docs);
    uassert(ErrorCodes::IncompatibleShardingMetadata,
            str::stream() << "Tried to find max chunk version for collection '"
                          << nss.toStringForErrorMsg() << ", but found no chunks",
            !chunksVector.empty());

    const auto highestVersionChunk = uassertStatusOK(
        ChunkType::parseFromConfigBSON(chunksVector.front(), coll.getEpoch(), coll.getTimestamp()));
    const auto currentCollectionPlacementVersion = highestVersionChunk.getVersion();

    // Check that current collection epoch and timestamp still matches the one sent by the shard.
    // This is to spot scenarios in which the collection have been dropped and recreated or had its
    // shard key refined since the migration began.
    uassert(StaleEpochInfo(nss, ShardVersion{}, ShardVersion{}),
            str::stream() << "The epoch of collection '" << nss.toStringForErrorMsg()
                          << "' has changed since the migration began. The config server's "
                             "collection placement version epoch is now '"
                          << currentCollectionPlacementVersion.epoch().toString()
                          << "', but the shard's is " << collectionEpoch.toString()
                          << "'. Aborting clear jumbo on chunk (" << chunk.toString() << ").",
            currentCollectionPlacementVersion.epoch() == collectionEpoch);

    ChunkVersion newVersion({currentCollectionPlacementVersion.epoch(),
                             currentCollectionPlacementVersion.getTimestamp()},
                            {currentCollectionPlacementVersion.majorVersion() + 1, 0});

    BSONObj chunkQuery(BSON(ChunkType::min(chunk.getMin())
                            << ChunkType::max(chunk.getMax()) << ChunkType::collectionUUID
                            << coll.getUuid()));

    BSONObjBuilder updateBuilder;
    updateBuilder.append("$unset", BSON(ChunkType::jumbo() << ""));

    // Update the newest chunk to have the new (bumped) version
    BSONObjBuilder updateVersionClause(updateBuilder.subobjStart("$set"));
    updateVersionClause.appendTimestamp(ChunkType::lastmod(), newVersion.toLong());
    updateVersionClause.doneFast();

    auto chunkUpdate = updateBuilder.obj();

    auto didUpdate = uassertStatusOK(
        _localCatalogClient->updateConfigDocument(opCtx,
                                                  NamespaceString::kConfigsvrChunksNamespace,
                                                  chunkQuery,
                                                  chunkUpdate,
                                                  false /* upsert */,
                                                  kNoWaitWriteConcern));

    uassert(51263,
            str::stream() << "failed to clear jumbo flag due to " << chunkQuery
                          << " not matching any existing chunks",
            didUpdate);
}

void ShardingCatalogManager::ensureChunkVersionIsGreaterThan(OperationContext* opCtx,
                                                             const UUID& collUuid,
                                                             const BSONObj& minKey,
                                                             const BSONObj& maxKey,
                                                             const ChunkVersion& version) {
    // Mark opCtx as interruptible to ensure that all reads and writes to the metadata collections
    // under the exclusive _kChunkOpLock happen on the same term.
    opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

    // Take _kChunkOpLock in exclusive mode to prevent concurrent chunk modifications and generate
    // strictly monotonously increasing collection placement versions
    Lock::ExclusiveLock lk(opCtx, _kChunkOpLock);

    ScopeGuard earlyReturnBeforeDoingWriteGuard([&] {
        // Ensure waiting for writeConcern of the data read.
        repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);
    });

    CollectionType coll;
    {
        auto findCollResponse = uassertStatusOK(_localConfigShard->exhaustiveFindOnConfig(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            repl::ReadConcernLevel::kLocalReadConcern,
            CollectionType::ConfigNS,
            BSON(CollectionType::kEpochFieldName << version.epoch()),
            {} /* sort */,
            1));

        if (findCollResponse.docs.empty()) {
            LOGV2(5731600,
                  "ensureChunkVersionIsGreaterThan did not find a collection with epoch "
                  "{epoch} epoch; returning success.",
                  "epoch"_attr = version.epoch());
            return;
        }

        coll = CollectionType(findCollResponse.docs[0]);
        dassert(collUuid == coll.getUuid());
    }

    const auto requestedChunkQuery =
        BSON(ChunkType::min(minKey)
             << ChunkType::max(maxKey) << ChunkType::collectionUUID() << collUuid);

    // Get the chunk matching the requested chunk.
    ChunkType matchingChunk;
    {
        const auto matchingChunksVector =
            uassertStatusOK(_localConfigShard->exhaustiveFindOnConfig(
                                opCtx,
                                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                repl::ReadConcernLevel::kLocalReadConcern,
                                NamespaceString::kConfigsvrChunksNamespace,
                                requestedChunkQuery,
                                BSONObj() /* sort */,
                                1 /* limit */))
                .docs;
        if (matchingChunksVector.empty()) {
            // This can happen in a number of cases, such as that the collection has been
            // dropped, its shard key has been refined, the chunk has been split, or the chunk
            // has been merged.
            LOGV2(23884,
                  "ensureChunkVersionIsGreaterThan did not find any matching chunks; returning "
                  "success",
                  "minKey"_attr = minKey,
                  "maxKey"_attr = maxKey,
                  "epoch"_attr = version.epoch());
            return;
        }

        matchingChunk = uassertStatusOK(ChunkType::parseFromConfigBSON(
            matchingChunksVector.front(), coll.getEpoch(), coll.getTimestamp()));

        if ((version <=> matchingChunk.getVersion()) == std::partial_ordering::less) {
            LOGV2(23885,
                  "ensureChunkVersionIsGreaterThan found that the chunk already has a higher "
                  "version; "
                  "returning success",
                  "minKey"_attr = minKey,
                  "maxKey"_attr = maxKey,
                  "epoch"_attr = version.epoch(),
                  "version"_attr = version,
                  "currentChunk"_attr = matchingChunk.toConfigBSON());
            return;
        }
    }

    // Get the chunk with the current collection placement version for this epoch.
    ChunkType highestChunk;
    {
        const auto query = BSON(ChunkType::collectionUUID() << collUuid);
        const auto highestChunksVector =
            uassertStatusOK(_localConfigShard->exhaustiveFindOnConfig(
                                opCtx,
                                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                repl::ReadConcernLevel::kLocalReadConcern,
                                NamespaceString::kConfigsvrChunksNamespace,
                                query,
                                BSON(ChunkType::lastmod << -1) /* sort */,
                                1 /* limit */))
                .docs;
        if (highestChunksVector.empty()) {
            LOGV2(23886,
                  "ensureChunkVersionIsGreaterThan did not find any chunks with a matching epoch "
                  "when "
                  "attempting to find the collection placement version. The collection must have "
                  "been "
                  "dropped "
                  "concurrently or had its shard key refined. Returning success.",
                  "epoch"_attr = version.epoch());
            return;
        }
        highestChunk = uassertStatusOK(ChunkType::parseFromConfigBSON(
            highestChunksVector.front(), coll.getEpoch(), coll.getTimestamp()));
    }

    // Generate a new version for the chunk by incrementing the collection placement version's major
    // version.
    auto newChunk = matchingChunk;
    newChunk.setVersion(ChunkVersion({coll.getEpoch(), coll.getTimestamp()},
                                     {highestChunk.getVersion().majorVersion() + 1, 0}));

    // Update the chunk, if it still exists, to have the bumped version.
    earlyReturnBeforeDoingWriteGuard.dismiss();
    auto didUpdate = uassertStatusOK(
        _localCatalogClient->updateConfigDocument(opCtx,
                                                  NamespaceString::kConfigsvrChunksNamespace,
                                                  requestedChunkQuery,
                                                  newChunk.toConfigBSON(),
                                                  false /* upsert */,
                                                  kNoWaitWriteConcern));
    if (didUpdate) {
        LOGV2(23887,
              "ensureChunkVersionIsGreaterThan bumped the the chunk version",
              "minKey"_attr = minKey,
              "maxKey"_attr = maxKey,
              "epoch"_attr = version.epoch(),
              "newChunk"_attr = newChunk.toConfigBSON());
    } else {
        LOGV2(23888,
              "ensureChunkVersionIsGreaterThan did not find a matching chunk when trying to bump "
              "its "
              "version. The collection must have been dropped concurrently or had its shard key "
              "refined. Returning success.",
              "minKey"_attr = minKey,
              "maxKey"_attr = maxKey,
              "epoch"_attr = version.epoch());
    }
}

void ShardingCatalogManager::bumpCollectionPlacementVersionAndChangeMetadataInTxn(
    OperationContext* opCtx,
    const NamespaceString& nss,
    unique_function<void(OperationContext*, TxnNumber)> changeMetadataFunc) {
    bumpCollectionPlacementVersionAndChangeMetadataInTxn(
        opCtx, nss, std::move(changeMetadataFunc), defaultMajorityWriteConcernDoNotUse());
}

void ShardingCatalogManager::bumpCollectionPlacementVersionAndChangeMetadataInTxn(
    OperationContext* opCtx,
    const NamespaceString& nss,
    unique_function<void(OperationContext*, TxnNumber)> changeMetadataFunc,
    const WriteConcernOptions& writeConcern) {
    bumpMultipleCollectionPlacementVersionsAndChangeMetadataInTxn(
        opCtx, {nss}, std::move(changeMetadataFunc), writeConcern);
}

void ShardingCatalogManager::bumpMultipleCollectionPlacementVersionsAndChangeMetadataInTxn(
    OperationContext* opCtx,
    const std::vector<NamespaceString>& collNames,
    unique_function<void(OperationContext*, TxnNumber)> changeMetadataFunc) {
    bumpMultipleCollectionPlacementVersionsAndChangeMetadataInTxn(
        opCtx, collNames, std::move(changeMetadataFunc), defaultMajorityWriteConcernDoNotUse());
}

void ShardingCatalogManager::bumpMultipleCollectionPlacementVersionsAndChangeMetadataInTxn(
    OperationContext* opCtx,
    const std::vector<NamespaceString>& collNames,
    unique_function<void(OperationContext*, TxnNumber)> changeMetadataFunc,
    const WriteConcernOptions& writeConcern) {

    // Mark opCtx as interruptible to ensure that all reads and writes to the metadata collections
    // under the exclusive _kChunkOpLock happen on the same term.
    opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

    // Take _kChunkOpLock in exclusive mode to prevent concurrent chunk splits, merges, and
    // migrations
    Lock::ExclusiveLock lk(opCtx, _kChunkOpLock);

    withTransaction(
        opCtx,
        NamespaceString::kConfigReshardingOperationsNamespace,
        [&collNames, &changeMetadataFunc, configShard = _localConfigShard.get()](
            OperationContext* opCtx, TxnNumber txnNumber) {
            for (const auto& nss : collNames) {
                bumpCollectionMinorVersion(opCtx, configShard, nss, txnNumber);
            }
            changeMetadataFunc(opCtx, txnNumber);
        },
        writeConcern);
}

void ShardingCatalogManager::splitOrMarkJumbo(OperationContext* opCtx,
                                              const NamespaceString& nss,
                                              const BSONObj& minKey,
                                              boost::optional<int64_t> optMaxChunkSizeBytes) {
    const auto cm = uassertStatusOK(
        RoutingInformationCache::get(opCtx)->getCollectionPlacementInfoWithRefresh(opCtx, nss));
    uassert(ErrorCodes::NamespaceNotSharded,
            str::stream() << "Collection '" << nss.toStringForErrorMsg() << "' is not sharded",
            cm.isSharded());

    auto chunk = cm.findIntersectingChunkWithSimpleCollation(minKey);

    try {
        const auto maxChunkSizeBytes = [&]() -> int64_t {
            if (optMaxChunkSizeBytes.has_value()) {
                return *optMaxChunkSizeBytes;
            }

            auto coll = _localCatalogClient->getCollection(
                opCtx, nss, repl::ReadConcernLevel::kMajorityReadConcern);
            return coll.getMaxChunkSizeBytes().value_or(
                Grid::get(opCtx)->getBalancerConfiguration()->getMaxChunkSizeBytes());
        }();

        // Limit the search to one split point: this code path is reached when a migration fails due
        // to ErrorCodes::ChunkTooBig. In case there is a too frequent shard key, only select the
        // next key in order to split the range in jumbo chunk + remaining range.
        const int limit = 1;
        auto splitPoints =
            uassertStatusOK(shardutil::selectChunkSplitPoints(opCtx,
                                                              chunk.getShardId(),
                                                              nss,
                                                              cm.getShardKeyPattern(),
                                                              chunk.getRange(),
                                                              maxChunkSizeBytes,
                                                              limit));

        if (splitPoints.empty()) {
            LOGV2(21873, "Marking chunk as jumbo", "chunk"_attr = redact(chunk.toString()));
            chunk.markAsJumbo();

            // Take _kChunkOpLock in exclusive mode to prevent concurrent chunk modifications. Note
            // that the operation below doesn't increment the chunk marked as jumbo's version, which
            // means that a subsequent incremental refresh will not see it. However, it is being
            // marked in memory through the call to 'markAsJumbo' above so subsequent balancer
            // iterations will not consider it for migration.
            Lock::ExclusiveLock lk(opCtx, _kChunkOpLock);

            const auto findCollResponse = uassertStatusOK(_localConfigShard->exhaustiveFindOnConfig(
                opCtx,
                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                repl::ReadConcernLevel::kLocalReadConcern,
                CollectionType::ConfigNS,
                BSON(CollectionType::kNssFieldName
                     << NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault())),
                {},
                1));
            uassert(ErrorCodes::ConflictingOperationInProgress,
                    "Collection does not exist",
                    !findCollResponse.docs.empty());
            const CollectionType coll(findCollResponse.docs[0]);

            const auto chunkQuery = BSON(ChunkType::collectionUUID()
                                         << coll.getUuid() << ChunkType::min(chunk.getMin()));

            auto status = _localCatalogClient->updateConfigDocument(
                opCtx,
                NamespaceString::kConfigsvrChunksNamespace,
                chunkQuery,
                BSON("$set" << BSON(ChunkType::jumbo(true))),
                false,
                defaultMajorityWriteConcernDoNotUse());
            if (!status.isOK()) {
                LOGV2(21874,
                      "Couldn't mark chunk as jumbo",
                      "namespace"_attr = redact(toStringForLogging(nss)),
                      "minKey"_attr = redact(chunk.getMin()),
                      "error"_attr = redact(status.getStatus()));
            }

            return;
        }

        // Resize the vector because in multiversion scenarios the `autoSplitVector` command may end
        // up ignoring the `limit` parameter and returning the whole list of split points.
        splitPoints.resize(limit);
        uassertStatusOK(shardutil::splitChunkAtMultiplePoints(opCtx,
                                                              chunk.getShardId(),
                                                              nss,
                                                              cm.getShardKeyPattern(),
                                                              cm.getVersion().epoch(),
                                                              cm.getVersion().getTimestamp(),
                                                              chunk.getRange(),
                                                              splitPoints));
    } catch (const DBException&) {
    }
}

void ShardingCatalogManager::setAllowMigrationsAndBumpOneChunk(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const boost::optional<UUID>& collectionUUID,
    bool allowMigrations) {
    // Mark opCtx as interruptible to ensure that all reads and writes to the metadata
    // collections under the exclusive _kChunkOpLock happen on the same term.
    opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

    // Take _kChunkOpLock in exclusive mode to prevent concurrent chunk splits, merges, and
    // migrations
    Lock::ExclusiveLock lk(opCtx, _kChunkOpLock);

    const auto cm = uassertStatusOK(
        RoutingInformationCache::get(opCtx)->getCollectionPlacementInfoWithRefresh(opCtx, nss));
    uassert(ErrorCodes::NamespaceNotSharded,
            str::stream() << "Collection '" << nss.toStringForErrorMsg() << "' is not sharded",
            cm.isSharded());

    uassert(ErrorCodes::InvalidUUID,
            str::stream() << "Collection uuid " << collectionUUID
                          << " in the request does not match the current uuid " << cm.getUUID()
                          << " for ns " << nss.toStringForErrorMsg(),
            !collectionUUID || collectionUUID == cm.getUUID());

    auto updateCollectionAndChunkFn = [allowMigrations, &nss, &collectionUUID](
                                          const txn_api::TransactionClient& txnClient,
                                          ExecutorPtr txnExec) {
        write_ops::UpdateCommandRequest updateCollOp(CollectionType::ConfigNS);
        updateCollOp.setUpdates([&] {
            write_ops::UpdateOpEntry entry;
            const auto update = allowMigrations
                ? BSON("$unset" << BSON(CollectionType::kAllowMigrationsFieldName << ""))
                : BSON("$set" << BSON(CollectionType::kAllowMigrationsFieldName << false));

            BSONObj query = BSON(CollectionType::kNssFieldName << NamespaceStringUtil::serialize(
                                     nss, SerializationContext::stateDefault()));
            if (collectionUUID) {
                query = query.addFields(BSON(CollectionType::kUuidFieldName << *collectionUUID));
            }
            entry.setQ(query);
            entry.setU(update);
            entry.setMulti(false);
            return std::vector<write_ops::UpdateOpEntry>{entry};
        }());

        auto updateCollResponse = txnClient.runCRUDOpSync(updateCollOp, {0});
        uassertStatusOK(updateCollResponse.toStatus());
        uassert(ErrorCodes::ConflictingOperationInProgress,
                str::stream() << "Expected to match one doc but matched "
                              << updateCollResponse.getN(),
                updateCollResponse.getN() == 1);

        FindCommandRequest collQuery{CollectionType::ConfigNS};
        collQuery.setFilter(BSON(CollectionType::kNssFieldName << NamespaceStringUtil::serialize(
                                     nss, SerializationContext::stateDefault())));
        collQuery.setLimit(1);

        const auto findCollResponse = txnClient.exhaustiveFindSync(collQuery);
        uassert(ErrorCodes::NamespaceNotFound,
                "Collection does not exist",
                findCollResponse.size() == 1);
        const CollectionType coll(findCollResponse[0]);

        // Find the newest chunk
        FindCommandRequest chunkQuery{NamespaceString::kConfigsvrChunksNamespace};
        chunkQuery.setFilter(BSON(ChunkType::collectionUUID << coll.getUuid()));
        chunkQuery.setSort(BSON(ChunkType::lastmod << -1));
        chunkQuery.setLimit(1);
        const auto findChunkResponse = txnClient.exhaustiveFindSync(chunkQuery);

        uassert(ErrorCodes::IncompatibleShardingMetadata,
                str::stream() << "Tried to find max chunk version for collection "
                              << nss.toStringForErrorMsg() << ", but found no chunks",
                findChunkResponse.size() == 1);

        const auto newestChunk = uassertStatusOK(ChunkType::parseFromConfigBSON(
            findChunkResponse[0], coll.getEpoch(), coll.getTimestamp()));
        const auto targetVersion = [&]() {
            ChunkVersion version = newestChunk.getVersion();
            version.incMinor();
            return version;
        }();

        write_ops::UpdateCommandRequest updateChunkOp(NamespaceString::kConfigsvrChunksNamespace);
        BSONObjBuilder updateBuilder;
        BSONObjBuilder updateVersionClause(updateBuilder.subobjStart("$set"));
        updateVersionClause.appendTimestamp(ChunkType::lastmod(), targetVersion.toLong());
        updateVersionClause.doneFast();
        const auto update = updateBuilder.obj();
        updateChunkOp.setUpdates([&] {
            write_ops::UpdateOpEntry entry;
            entry.setQ(BSON(ChunkType::name << newestChunk.getName()));
            entry.setU(update);
            entry.setMulti(false);
            entry.setUpsert(false);
            return std::vector<write_ops::UpdateOpEntry>{entry};
        }());
        auto updateChunkResponse = txnClient.runCRUDOpSync(updateChunkOp, {1});
        uassertStatusOK(updateChunkResponse.toStatus());
        LOGV2_DEBUG(
            7353900, 1, "Finished all transaction operations in setAllowMigrations command");

        return SemiFuture<void>::makeReady();
    };
    auto executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
    auto inlineExecutor = std::make_shared<executor::InlineExecutor>();

    txn_api::SyncTransactionWithRetries txn(opCtx, executor, nullptr, inlineExecutor);

    txn.run(opCtx, updateCollectionAndChunkFn);
    // From now on migrations are not allowed anymore, so it is not possible that new shards
    // will own chunks for this collection.
}

void ShardingCatalogManager::bumpCollectionMinorVersionInTxn(OperationContext* opCtx,
                                                             const NamespaceString& nss,
                                                             TxnNumber txnNumber) const {
    bumpCollectionMinorVersion(opCtx, _localConfigShard.get(), nss, txnNumber);
}

void ShardingCatalogManager::setChunkEstimatedSize(OperationContext* opCtx,
                                                   const ChunkType& chunk,
                                                   long long estimatedDataSizeBytes,
                                                   const WriteConcernOptions& writeConcern) {
    // ensure the unsigned value fits in the signed long long
    uassert(6049442, "Estimated chunk size cannot be negative", estimatedDataSizeBytes >= 0);

    // Take _kChunkOpLock in exclusive mode to prevent concurrent chunk modifications and generate
    // strictly monotonously increasing collection placement versions
    Lock::ExclusiveLock lk(opCtx, _kChunkOpLock);

    const auto chunkQuery = BSON(ChunkType::collectionUUID()
                                 << chunk.getCollectionUUID() << ChunkType::min(chunk.getMin())
                                 << ChunkType::max(chunk.getMax()));
    BSONObjBuilder updateBuilder;
    BSONObjBuilder updateSub(updateBuilder.subobjStart("$set"));
    updateSub.appendNumber(ChunkType::estimatedSizeBytes.name(), estimatedDataSizeBytes);
    updateSub.doneFast();

    auto didUpdate = uassertStatusOK(
        _localCatalogClient->updateConfigDocument(opCtx,
                                                  NamespaceString::kConfigsvrChunksNamespace,
                                                  chunkQuery,
                                                  updateBuilder.done(),
                                                  false /* upsert */,
                                                  writeConcern));
    if (!didUpdate) {
        uasserted(6049401, "Did not update chunk with estimated size");
    }
}

bool ShardingCatalogManager::clearChunkEstimatedSize(OperationContext* opCtx, const UUID& uuid) {
    // Take _kChunkOpLock in exclusive mode to prevent concurrent chunk splits, merges, and
    // migrations
    Lock::ExclusiveLock lk(opCtx, _kChunkOpLock);

    const auto query = BSON(ChunkType::collectionUUID() << uuid);
    const auto update = BSON("$unset" << BSON(ChunkType::estimatedSizeBytes() << ""));
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

    auto response =
        _localConfigShard->runBatchWriteCommand(opCtx,
                                                Milliseconds(defaultConfigCommandTimeoutMS.load()),
                                                request,
                                                defaultMajorityWriteConcernDoNotUse(),
                                                Shard::RetryPolicy::kIdempotent);

    uassertStatusOK(response.toStatus());
    return response.getN() > 0;
}

void ShardingCatalogManager::_commitChunkMigrationInTransaction(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ChunkType& migratedChunk,
    const std::vector<ChunkType>& splitChunks,
    const boost::optional<ChunkType>& controlChunk,
    const ShardId& donorShardId) {
    const auto transactionChain = [&](const txn_api::TransactionClient& txnClient,
                                      ExecutorPtr txnExec) {
        std::vector<int> updateRequestStmtIds;
        int currStmtId = 0;

        // 1. Update the config.chunks collection with new entries.
        auto configChunksUpdateRequest = [&] {
            std::vector<write_ops::UpdateOpEntry> updateEntries;
            updateEntries.reserve(1 + splitChunks.size() + (controlChunk ? 1 : 0));

            auto buildUpdateOpEntry = [](const ChunkType& chunk, bool isUpsert) {
                write_ops::UpdateOpEntry entry;

                entry.setUpsert(isUpsert);
                auto chunkID = MONGO_unlikely(migrateCommitInvalidChunkQuery.shouldFail())
                    ? OID::gen()
                    : chunk.getName();

                entry.setQ(BSON(ChunkType::name() << chunkID));
                entry.setU(
                    write_ops::UpdateModification::parseFromClassicUpdate(chunk.toConfigBSON()));
                return entry;
            };

            updateEntries.emplace_back(buildUpdateOpEntry(migratedChunk, false));
            updateRequestStmtIds.push_back(currStmtId++);
            for (const auto& splitChunk : splitChunks) {
                updateEntries.emplace_back(buildUpdateOpEntry(splitChunk, true));
                updateRequestStmtIds.push_back(currStmtId++);
            }

            if (controlChunk) {
                updateEntries.emplace_back(buildUpdateOpEntry(*controlChunk, false));
                updateRequestStmtIds.push_back(currStmtId++);
            }

            write_ops::UpdateCommandRequest updateOp(NamespaceString::kConfigsvrChunksNamespace);
            updateOp.setUpdates(std::move(updateEntries));
            return updateOp;
        }();

        auto updateConfigChunks =
            txnClient.runCRUDOpSync(configChunksUpdateRequest, updateRequestStmtIds);

        uassertStatusOK(updateConfigChunks.toStatus());
        uassert(ErrorCodes::UpdateOperationFailed,
                str::stream() << "Commit chunk migration in transaction failed: N chunks updated "
                              << updateConfigChunks.getN() << " expected " << currStmtId,
                updateConfigChunks.getN() == currStmtId);

        // 2. If the placement of the parent collection is not impacted by the migrated chunks, end
        // the transaction as there is no need to update the placement history.
        if (!isPlacementChangedInParentCollection(
                opCtx, _localConfigShard.get(), migratedChunk, splitChunks, controlChunk)) {
            return SemiFuture<void>::makeReady();
        }

        // 3. Use the updated content of config.chunks to build the collection placement metadata.
        // The request is equivalent to "configDb.chunks.distinct('shard',{uuid:collUuid})".
        DistinctCommandRequest distinctRequest(NamespaceString::kConfigsvrChunksNamespace);
        distinctRequest.setKey(ChunkType::shard.name());
        distinctRequest.setQuery(
            BSON(ChunkType::collectionUUID.name() << migratedChunk.getCollectionUUID()));

        auto distinctCommandResponse =
            txnClient.runCommandSync(DatabaseName::kConfig, distinctRequest.toBSON());

        uassertStatusOK(getStatusFromWriteCommandReply(distinctCommandResponse));

        // 4. Persist new data to the config.placementHistory collection.
        std::vector<ShardId> shardIds;
        for (const auto& valueElement : distinctCommandResponse.getField("values").Array()) {
            shardIds.emplace_back(valueElement.String());
        }
        NamespacePlacementType placementInfo(
            nss, migratedChunk.getHistory().front().getValidAfter(), std::move(shardIds));
        placementInfo.setUuid(migratedChunk.getCollectionUUID());
        write_ops::InsertCommandRequest insertPlacementEntry(
            NamespaceString::kConfigsvrPlacementHistoryNamespace, {placementInfo.toBSON()});

        auto insertPlacementEntryResponse =
            txnClient.runCRUDOpSync(insertPlacementEntry, {currStmtId++});

        uassertStatusOK(insertPlacementEntryResponse.toStatus());

        return SemiFuture<void>::makeReady();
    };

    auto executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
    auto inlineExecutor = std::make_shared<executor::InlineExecutor>();

    txn_api::SyncTransactionWithRetries txn(opCtx, executor, nullptr, inlineExecutor);

    txn.run(opCtx, transactionChain);
}

Timestamp ShardingCatalogManager::getOldestTimestampSupportedForSnapshotHistory(
    OperationContext* opCtx) {
    const auto currTime = VectorClock::get(opCtx)->getTime();
    auto currTimeSeconds = currTime.clusterTime().asTimestamp().getSecs();
    return Timestamp(currTimeSeconds - getHistoryWindowInSeconds(), 0);
}
}  // namespace mongo
