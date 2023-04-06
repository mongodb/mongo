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

#include "mongo/db/s/config/sharding_catalog_manager.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/distinct_command_gen.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/s/sharding_logging.h"
#include "mongo/db/s/sharding_util.h"
#include "mongo/db/server_options.h"
#include "mongo/db/session/logical_session_cache.h"
#include "mongo/db/snapshot_window_options_gen.h"
#include "mongo/db/transaction/transaction_api.h"
#include "mongo/db/transaction/transaction_participant_gen.h"
#include "mongo/db/vector_clock_mutable.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_namespace_placement_gen.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/s/shard_util.h"
#include "mongo/s/sharding_feature_flags_gen.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future_util.h"
#include "mongo/util/str.h"

MONGO_FAIL_POINT_DEFINE(overrideHistoryWindowInSecs);

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(migrationCommitVersionError);
MONGO_FAIL_POINT_DEFINE(migrateCommitInvalidChunkQuery);
MONGO_FAIL_POINT_DEFINE(skipExpiringOldChunkHistory);

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
                                            ChunkType::ConfigNS,
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
    AggregateCommandRequest countRequest(ChunkType::ConfigNS);

    BSONObjBuilder builder;
    builder.append("aggregate", ChunkType::ConfigNS.ns());

    BSONObjBuilder queryBuilder;
    queryBuilder << ChunkType::collectionUUID << collectionUUID;
    queryBuilder << ChunkType::shard(shardId.toString());
    queryBuilder << ChunkType::min(BSON("$gte" << chunkRange.getMin()));
    queryBuilder << ChunkType::min(BSON("$lt" << chunkRange.getMax()));

    std::vector<BSONObj> pipeline;
    pipeline.push_back(BSON("$match" << queryBuilder.obj()));
    pipeline.push_back(BSON("$count" << ChunkType::collectionUUID.name()));
    countRequest.setPipeline(pipeline);

    return countRequest.toBSON({});
}

BSONObj buildCountSingleChunkCommand(const ChunkType& chunk) {
    AggregateCommandRequest countRequest(ChunkType::ConfigNS);

    auto query =
        BSON(ChunkType::min(chunk.getMin())
             << ChunkType::max(chunk.getMax()) << ChunkType::collectionUUID()
             << chunk.getCollectionUUID() << ChunkType::shard() << chunk.getShard().toString());
    std::vector<BSONObj> pipeline;
    pipeline.push_back(BSON("$match" << query));
    pipeline.push_back(BSON("$count" << ChunkType::collectionUUID.name()));
    countRequest.setPipeline(pipeline);

    return countRequest.toBSON({});
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
                                            ChunkType::ConfigNS,
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
                str::stream() << "Collection '" << coll.getNss().ns()
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
    auto findCollResponse =
        configShard->exhaustiveFindOnConfig(opCtx,
                                            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                            repl::ReadConcernLevel::kLocalReadConcern,
                                            CollectionType::ConfigNS,
                                            BSON(CollectionType::kNssFieldName << nss.ns()),
                                            {},
                                            1);
    if (!findCollResponse.isOK()) {
        return findCollResponse.getStatus();
    }

    if (findCollResponse.getValue().docs.empty()) {
        return {ErrorCodes::ConflictingOperationInProgress,
                str::stream() << "Sharded collection '" << nss.ns() << "' no longer exists"};
    }

    const CollectionType coll(findCollResponse.getValue().docs[0]);
    const auto chunksQuery = BSON(ChunkType::collectionUUID << coll.getUuid());
    const auto version = uassertStatusOK(getMaxChunkVersionFromQueryResponse(
        coll,
        configShard->exhaustiveFindOnConfig(opCtx,
                                            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                            repl::ReadConcernLevel::kLocalReadConcern,
                                            ChunkType::ConfigNS,
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
                                            ChunkType::ConfigNS,
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
    const auto findCollResponse = uassertStatusOK(
        configShard->exhaustiveFindOnConfig(opCtx,
                                            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                            repl::ReadConcernLevel::kLocalReadConcern,
                                            CollectionType::ConfigNS,
                                            BSON(CollectionType::kNssFieldName << nss.ns()),
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
        ChunkType::ConfigNS,
        BSON(ChunkType::collectionUUID << coll.getUuid()) /* query */,
        BSON(ChunkType::lastmod << -1) /* sort */,
        1 /* limit */));

    uassert(ErrorCodes::IncompatibleShardingMetadata,
            str::stream() << "Tried to find max chunk version for collection '" << nss.ns()
                          << ", but found no chunks",
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
        ChunkType::ConfigNS,
        BSON(ChunkType::name << newestChunk.getName()),  // query
        chunkUpdate,                                     // update
        false,                                           // upsert
        false                                            // multi
    );

    const auto res = ShardingCatalogManager::get(opCtx)->writeToConfigDocumentInTxn(
        opCtx, ChunkType::ConfigNS, request, txnNumber);

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

    // TODO SERVER-73295 review hardcoded 10 seconds minimum history
    return std::max(
        std::max(minSnapshotHistoryWindowInSeconds.load(), gTransactionLifetimeLimitSeconds.load()),
        10);
}

void logMergeToChangelog(OperationContext* opCtx,
                         const NamespaceString& nss,
                         const ChunkVersion& prevPlacementVersion,
                         const ChunkVersion& mergedVersion,
                         const ShardId& owningShard,
                         const ChunkRange& chunkRange,
                         const size_t numChunks,
                         std::shared_ptr<Shard> configShard,
                         ShardingCatalogClient* catalogClient) {
    BSONObjBuilder logDetail;
    prevPlacementVersion.serialize("prevPlacementVersion", &logDetail);
    mergedVersion.serialize("mergedVersion", &logDetail);
    logDetail.append("owningShard", owningShard);
    chunkRange.append(&logDetail);
    logDetail.append("numChunks", static_cast<int>(numChunks));

    ShardingLogging::get(opCtx)->logChange(opCtx,
                                           "merge",
                                           nss.ns(),
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

            write_ops::DeleteCommandRequest deleteOp(ChunkType::ConfigNS);
            deleteOp.setDeletes([&] {
                std::vector<write_ops::DeleteOpEntry> deletes;
                write_ops::DeleteOpEntry entry;
                entry.setQ(queryBuilder.obj());
                entry.setMulti(true);
                return std::vector<write_ops::DeleteOpEntry>{entry};
            }());

            // Prepare insertion of new chunks covering the whole range
            write_ops::InsertCommandRequest insertOp(ChunkType::ConfigNS, {chunk.toConfigBSON()});

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
    auto sleepInlineExecutor = inlineExecutor->getSleepableExecutor(executor);

    txn_api::SyncTransactionWithRetries txn(opCtx, sleepInlineExecutor, nullptr, inlineExecutor);
    txn.run(opCtx, updateChunksFn);
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
        auto request = BatchedCommandRequest::buildUpdateOp(ChunkType::ConfigNS,
                                                            query,        // query
                                                            chunkUpdate,  // update
                                                            false,        // upsert
                                                            false         // multi
        );

        auto res = ShardingCatalogManager::get(opCtx)->writeToConfigDocumentInTxn(
            opCtx, ChunkType::ConfigNS, request, txnNumber);

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
    auto newChunkBounds = std::make_shared<std::vector<BSONObj>>(splitPoints);
    newChunkBounds->push_back(range.getMax());
    // We need to use a shared pointer to prevent an scenario where the operation context is
    // interrupted and the scope containing SyncTransactionWithRetries goes away but the callback is
    // called from the executor thread.
    // TODO SERVER-75189: remove after SERVER-66261 is committed.
    struct SharedBlock {
        SharedBlock(const NamespaceString& nss_,
                    const ChunkRange& range_,
                    const ChunkType& origChunk_,
                    const std::string& shardName_,
                    const ChunkVersion& currentMaxVersion_,
                    std::shared_ptr<std::vector<BSONObj>> newChunkBounds_)
            : nss(nss_),
              range(range_),
              origChunk(origChunk_),
              shardName(shardName_),
              currentMaxVersion(currentMaxVersion_),
              newChunkBounds(newChunkBounds_) {
            newChunks = std::make_shared<std::vector<ChunkType>>();
        }

        NamespaceString nss;
        ChunkRange range;
        ChunkType origChunk;
        std::string shardName;
        ChunkVersion currentMaxVersion;
        std::shared_ptr<std::vector<BSONObj>> newChunkBounds;
        std::shared_ptr<std::vector<ChunkType>> newChunks;
    };
    auto sharedBlock = std::make_shared<SharedBlock>(
        nss, range, origChunk, shardName, collPlacementVersion, newChunkBounds);

    auto updateChunksFn = [sharedBlock](const txn_api::TransactionClient& txnClient,
                                        ExecutorPtr txnExec) {
        ChunkType chunk(sharedBlock->origChunk.getCollectionUUID(),
                        sharedBlock->range,
                        sharedBlock->currentMaxVersion,
                        sharedBlock->shardName);

        // Verify that the range matches exactly a single chunk
        auto countRequest = buildCountSingleChunkCommand(chunk);

        return txnClient.runCommand(ChunkType::ConfigNS.dbName(), countRequest)
            .thenRunOn(txnExec)
            .then([&txnClient, sharedBlock](auto countResponse) {
                auto cursorResponse = uassertStatusOK(CursorResponse::parseFromBSON(countResponse));
                auto firstBatch = cursorResponse.getBatch();
                uassert(ErrorCodes::BadValue,
                        str::stream()
                            << "Could not meet precondition to split chunk, expected "
                               "chunk with range "
                            << sharedBlock->range.toString() << " in shard "
                            << redact(sharedBlock->shardName) << " but no chunk was found",
                        !firstBatch.empty());
                auto countObj = firstBatch.front();
                auto docCount = countObj.getIntField(ChunkType::collectionUUID.name());
                uassert(ErrorCodes::BadValue,
                        str::stream() << "Could not meet precondition to split chunk, expected "
                                         "one chunk with range "
                                      << sharedBlock->range.toString() << " in shard "
                                      << redact(sharedBlock->shardName) << " but found " << docCount
                                      << " chunks",
                        1 == docCount);

                auto startKey = sharedBlock->range.getMin();
                OID chunkID;

                auto shouldTakeOriginalChunkID = true;
                write_ops::UpdateCommandRequest updateOp(ChunkType::ConfigNS);
                std::vector<write_ops::UpdateOpEntry> entries;
                entries.reserve(sharedBlock->newChunkBounds->size());
                for (const auto& endKey : *(sharedBlock->newChunkBounds)) {
                    // Verify the split points are all within the chunk
                    uassert(ErrorCodes::InvalidOptions,
                            str::stream()
                                << "Split key " << endKey << " not contained within chunk "
                                << sharedBlock->range.toString(),
                            endKey.woCompare(sharedBlock->range.getMax()) == 0 ||
                                sharedBlock->range.containsKey(endKey));

                    // Verify the split points came in increasing order
                    uassert(ErrorCodes::InvalidOptions,
                            str::stream()
                                << "Split keys must be specified in strictly increasing order. Key "
                                << endKey << " was specified after " << startKey << ".",
                            endKey.woCompare(startKey) >= 0);

                    // Verify that splitPoints are not repeated
                    uassert(ErrorCodes::InvalidOptions,
                            str::stream()
                                << "Split on lower bound of chunk [" << startKey.toString() << ", "
                                << endKey.toString() << "] is not allowed",
                            endKey.woCompare(startKey) != 0);

                    // verify that splits don't use disallowed BSON object format
                    uassertStatusOK(
                        ShardKeyPattern::checkShardKeyIsValidForMetadataStorage(endKey));

                    // splits only update the 'minor' portion of version
                    sharedBlock->currentMaxVersion.incMinor();

                    // First chunk takes ID of the original chunk and all other chunks get new
                    // IDs. This occurs because we perform an update operation below (with
                    // upsert true). Keeping the original ID ensures we overwrite the old chunk
                    // (before the split) without having to perform a delete.
                    chunkID =
                        shouldTakeOriginalChunkID ? sharedBlock->origChunk.getName() : OID::gen();

                    shouldTakeOriginalChunkID = false;

                    ChunkType newChunk = sharedBlock->origChunk;
                    newChunk.setName(chunkID);
                    newChunk.setVersion(sharedBlock->currentMaxVersion);
                    newChunk.setMin(startKey);
                    newChunk.setMax(endKey);
                    newChunk.setEstimatedSizeBytes(boost::none);
                    newChunk.setJumbo(false);

                    // build an update operation against the chunks collection of the config
                    // database with upsert true
                    write_ops::UpdateOpEntry entry;
                    entry.setMulti(false);
                    entry.setUpsert(true);
                    entry.setQ(BSON(ChunkType::name() << chunkID));
                    entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(
                        newChunk.toConfigBSON()));
                    entries.push_back(entry);

                    // remember this chunk info for logging later
                    sharedBlock->newChunks->push_back(std::move(newChunk));

                    startKey = endKey;
                }
                updateOp.setUpdates(entries);

                auto updateBSONObjSize = updateOp.toBSON({}).objsize();
                uassert(ErrorCodes::InvalidOptions,
                        str::stream()
                            << "Spliting the chunk with too many split points, the "
                               "final BSON operation size "
                            << updateBSONObjSize << " bytes would exceed the maximum BSON size: "
                            << BSONObjMaxInternalSize << " bytes",
                        updateBSONObjSize < BSONObjMaxInternalSize);
                return txnClient.runCRUDOp(updateOp, {});
            })
            .thenRunOn(txnExec)
            .then([](auto updateResponse) {
                uassertStatusOK(updateResponse.toStatus());

                LOGV2_DEBUG(6583806, 1, "Split chunk in transaction finished");
            })
            .semi();
    };

    auto executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
    auto inlineExecutor = std::make_shared<executor::InlineExecutor>();
    auto sleepInlineExecutor = inlineExecutor->getSleepableExecutor(executor);

    txn_api::SyncTransactionWithRetries txn(opCtx, sleepInlineExecutor, nullptr, inlineExecutor);

    // TODO: SERVER-72431 Make split chunk commit idempotent, with that we won't need anymore the
    // transaction precondition and we will be able to remove the try/catch on the transaction run
    try {
        txn.run(opCtx, updateChunksFn);
    } catch (const ExceptionFor<ErrorCodes::BadValue>&) {
        // Makes sure that the last thing we read from config.chunks collection gets majority
        // written before to return from this command, otherwise next RoutingInfo cache refresh from
        // the shard may not see those newest information.
        repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);
        throw;
    }

    return ShardingCatalogManager::SplitChunkInTransactionResult{sharedBlock->currentMaxVersion,
                                                                 sharedBlock->newChunks};
}

StatusWith<ShardingCatalogManager::ShardAndCollectionPlacementVersions>
ShardingCatalogManager::commitChunkSplit(OperationContext* opCtx,
                                         const NamespaceString& nss,
                                         const OID& requestEpoch,
                                         const boost::optional<Timestamp>& requestTimestamp,
                                         const ChunkRange& range,
                                         const std::vector<BSONObj>& splitPoints,
                                         const std::string& shardName,
                                         const bool fromChunkSplitter) {

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

    // Don't allow auto-splitting if the collection is being defragmented
    uassert(ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Can't commit auto-split while `" << nss.ns()
                          << "` is undergoing a defragmentation.",
            !(coll.getDefragmentCollection() && fromChunkSplitter));

    auto collPlacementVersion = version;

    // Return an error if collection epoch does not match epoch of request.
    if (coll.getEpoch() != requestEpoch ||
        (requestTimestamp && coll.getTimestamp() != requestTimestamp)) {
        return {ErrorCodes::StaleEpoch,
                str::stream() << "splitChunk cannot split chunk " << range.toString()
                              << ". Epoch of collection '" << nss.ns() << "' has changed."
                              << " Current epoch: " << coll.getEpoch()
                              << ", cmd epoch: " << requestEpoch};
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

    // log changes
    BSONObjBuilder logDetail;
    {
        BSONObjBuilder b(logDetail.subobjStart("before"));
        b.append(ChunkType::min(), range.getMin());
        b.append(ChunkType::max(), range.getMax());
        collPlacementVersion.serialize(ChunkType::lastmod(), &b);
    }

    if (splitChunkResult.newChunks->size() == 2) {
        appendShortVersion(&logDetail.subobjStart("left"), splitChunkResult.newChunks->at(0));
        appendShortVersion(&logDetail.subobjStart("right"), splitChunkResult.newChunks->at(1));
        logDetail.append("owningShard", shardName);

        ShardingLogging::get(opCtx)->logChange(opCtx,
                                               "split",
                                               nss.ns(),
                                               logDetail.obj(),
                                               WriteConcernOptions(),
                                               _localConfigShard,
                                               _localCatalogClient.get());
    } else {
        BSONObj beforeDetailObj = logDetail.obj();
        BSONObj firstDetailObj = beforeDetailObj.getOwned();
        const int newChunksSize = splitChunkResult.newChunks->size();

        for (int i = 0; i < newChunksSize; i++) {
            BSONObjBuilder chunkDetail;
            chunkDetail.appendElements(beforeDetailObj);
            chunkDetail.append("number", i + 1);
            chunkDetail.append("of", newChunksSize);
            appendShortVersion(&chunkDetail.subobjStart("chunk"),
                               splitChunkResult.newChunks->at(i));
            chunkDetail.append("owningShard", shardName);

            const auto status =
                ShardingLogging::get(opCtx)->logChangeChecked(opCtx,
                                                              "multi-split",
                                                              nss.ns(),
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
            return txnClient.runCommand(ChunkType::ConfigNS.dbName(), countRequest)
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
                    write_ops::UpdateCommandRequest updateOp(ChunkType::ConfigNS);
                    updateOp.setUpdates({[&] {
                        write_ops::UpdateOpEntry entry;

                        ChunkType mergedChunk(chunksToMerge->front());
                        entry.setQ(BSON(ChunkType::name(mergedChunk.getName())));
                        mergedChunk.setMax(chunksToMerge->back().getMax());

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

                    write_ops::DeleteCommandRequest deleteOp(ChunkType::ConfigNS);
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
    auto sleepInlineExecutor = inlineExecutor->getSleepableExecutor(executor);

    txn_api::SyncTransactionWithRetries txn(opCtx, sleepInlineExecutor, nullptr, inlineExecutor);
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
    uassert(ErrorCodes::StaleEpoch,
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
            ChunkType::ConfigNS,
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

    // 5. log changes
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
                                                    int maxNumberOfChunksToMerge) {
    // Mark opCtx as interruptible to ensure that all reads and writes to the metadata collections
    // under the exclusive _kChunkOpLock happen on the same term.
    opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

    // Retry the commit a fixed number of  times before failing: the discovery of chunks to merge
    // happens before acquiring the `_kChunkOpLock` in order not to block  for too long concurrent
    // chunk operations. This implies that other chunk operations for the same collection could
    // potentially commit before acquiring the lock, forcing to repeat the discovey.
    const int MAX_RETRIES = 5;
    int nRetries = 0;

    while (nRetries < MAX_RETRIES) {
        // 1. Retrieve the collection entry and the initial version.
        const auto [coll, originalVersion] =
            uassertStatusOK(getCollectionAndVersion(opCtx, _localConfigShard.get(), nss));
        auto& collUuid = coll.getUuid();
        auto newVersion = originalVersion;

        // 2. Retrieve the list of mergeable chunks belonging to the requested shard/collection.
        // A chunk is mergeable when the following conditions are honored:
        // - Non-jumbo
        // - The last migration occurred before the current history window
        const auto oldestTimestampSupportedForHistory =
            getOldestTimestampSupportedForSnapshotHistory(opCtx);
        const auto chunksBelongingToShard =
            uassertStatusOK(_localConfigShard->exhaustiveFindOnConfig(
                                opCtx,
                                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                repl::ReadConcernLevel::kLocalReadConcern,
                                ChunkType::ConfigNS,
                                BSON(ChunkType::collectionUUID
                                     << collUuid << ChunkType::shard(shardId.toString())
                                     << ChunkType::onCurrentShardSince
                                     << BSON("$lt" << oldestTimestampSupportedForHistory)
                                     << ChunkType::jumbo << BSON("$ne" << true)),
                                BSON(ChunkType::min << 1) /* sort */,
                                boost::none /* limit */))
                .docs;

        // 3. Prepare the data for the merge.

        // Track the total and per-range number of merged chunks
        std::pair<int, std::vector<size_t>> numMergedChunks;

        const auto newChunks = [&]() -> std::shared_ptr<std::vector<ChunkType>> {
            auto newChunks = std::make_shared<std::vector<ChunkType>>();
            const Timestamp minValidTimestamp = Timestamp(0, 1);

            BSONObj rangeMin, rangeMax;
            Timestamp rangeOnCurrentShardSince = minValidTimestamp;
            int nChunksInRange = 0;

            // Lambda generating the new chunk to be committed if a merge can be issued on the range
            auto processRange = [&]() {
                if (nChunksInRange > 1) {
                    newVersion.incMinor();
                    ChunkType newChunk(collUuid, {rangeMin, rangeMax}, newVersion, shardId);
                    newChunk.setOnCurrentShardSince(rangeOnCurrentShardSince);
                    newChunk.setHistory({ChunkHistory{rangeOnCurrentShardSince, shardId}});
                    numMergedChunks.first += nChunksInRange;
                    numMergedChunks.second.push_back(nChunksInRange);
                    newChunks->push_back(std::move(newChunk));
                }
                nChunksInRange = 0;
                rangeOnCurrentShardSince = minValidTimestamp;
            };

            for (const auto& chunkDoc : chunksBelongingToShard) {
                const auto& chunkMin = chunkDoc.getObjectField(ChunkType::min());
                const auto& chunkMax = chunkDoc.getObjectField(ChunkType::max());
                const Timestamp chunkOnCurrentShardSince = [&]() {
                    Timestamp t = minValidTimestamp;
                    bsonExtractTimestampField(chunkDoc, ChunkType::onCurrentShardSince(), &t)
                        .ignore();
                    return t;
                }();

                if (rangeMax.woCompare(chunkMin) != 0) {
                    processRange();
                }

                if (nChunksInRange == 0) {
                    rangeMin = chunkMin;
                }
                rangeMax = chunkMax;

                if (chunkOnCurrentShardSince > rangeOnCurrentShardSince) {
                    rangeOnCurrentShardSince = chunkOnCurrentShardSince;
                }
                nChunksInRange++;

                if (numMergedChunks.first + nChunksInRange == maxNumberOfChunksToMerge) {
                    break;
                }
            }
            processRange();

            return newChunks;
        }();

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
                                _localCatalogClient.get());

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
        BSON(CollectionType::kNssFieldName << nss.ns()),
        {},
        1));
    uassert(ErrorCodes::ConflictingOperationInProgress,
            "Collection does not exist",
            !findCollResponse.docs.empty());

    const CollectionType coll(findCollResponse.docs[0]);
    uassert(ErrorCodes::ConflictingOperationInProgress,
            "Collection is undergoing changes and chunks cannot be moved",
            coll.getAllowMigrations() && coll.getPermitMigrations());

    const auto findChunkQuery = BSON(ChunkType::collectionUUID() << coll.getUuid());

    auto findResponse = uassertStatusOK(_localConfigShard->exhaustiveFindOnConfig(
        opCtx,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        repl::ReadConcernLevel::kLocalReadConcern,
        ChunkType::ConfigNS,
        findChunkQuery,
        BSON(ChunkType::lastmod << -1),
        1));
    uassert(ErrorCodes::IncompatibleShardingMetadata,
            str::stream() << "Tried to find max chunk version for collection '" << nss.ns()
                          << ", but found no chunks",
            !findResponse.docs.empty());

    const auto chunk = uassertStatusOK(
        ChunkType::parseFromConfigBSON(findResponse.docs[0], coll.getEpoch(), coll.getTimestamp()));
    const auto& currentCollectionPlacementVersion = chunk.getVersion();

    if (MONGO_unlikely(migrationCommitVersionError.shouldFail())) {
        uasserted(ErrorCodes::StaleEpoch,
                  "Failpoint 'migrationCommitVersionError' generated error");
    }

    // It is possible for a migration to end up running partly without the protection of the
    // distributed lock if the config primary stepped down since the start of the migration and
    // failed to recover the migration. Check that the collection has not been dropped and recreated
    // or had its shard key refined since the migration began, unbeknown to the shard when the
    // command was sent.
    if (currentCollectionPlacementVersion.epoch() != collectionEpoch ||
        currentCollectionPlacementVersion.getTimestamp() != collectionTimestamp) {
        return {ErrorCodes::StaleEpoch,
                str::stream() << "The epoch of collection '" << nss.ns()
                              << "' has changed since the migration began. The config server's "
                                 "collection placement version epoch is now '"
                              << currentCollectionPlacementVersion.epoch().toString()
                              << "', but the shard's is " << collectionEpoch.toString()
                              << "'. Aborting migration commit for chunk ("
                              << migratedChunk.getRange().toString() << ")."};
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
                          << " from ns: " << nss.ns() << " not owned by donor " << fromShard
                          << " neither by recipient " << toShard,
            currentChunk.getShard() == fromShard);

    if (migratedChunk.getVersion().isNotComparableWith(currentChunk.getVersion()) ||
        migratedChunk.getVersion().isOlderThan(currentChunk.getVersion())) {
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
    std::shared_ptr<ChunkType> newMigratedChunk = std::make_shared<ChunkType>(currentChunk);
    newMigratedChunk->setMin(migratedChunk.getMin());
    newMigratedChunk->setMax(migratedChunk.getMax());
    newMigratedChunk->setShard(toShard);
    newMigratedChunk->setVersion(ChunkVersion(
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
                str::stream() << "The chunk history for chunk with namespace " << nss.ns()
                              << " and min key " << migratedChunk.getMin()
                              << " is corrupted. The last validAfter "
                              << newHistory.back().getValidAfter().toString()
                              << " is greater or equal to the new validAfter "
                              << validAfter.toString()};
    }
    newMigratedChunk->setOnCurrentShardSince(validAfter);
    newHistory.emplace(newHistory.begin(),
                       ChunkHistory(*newMigratedChunk->getOnCurrentShardSince(), toShard));
    newMigratedChunk->setHistory(std::move(newHistory));

    std::shared_ptr<std::vector<ChunkType>> newSplitChunks =
        std::make_shared<std::vector<ChunkType>>();
    {
        // This scope handles the `moveRange` scenario, potentially create chunks on the sides of
        // the moved range
        const auto& movedChunkMin = newMigratedChunk->getMin();
        const auto& movedChunkMax = newMigratedChunk->getMax();
        const auto& movedChunkVersion = newMigratedChunk->getVersion();

        if (SimpleBSONObjComparator::kInstance.evaluate(movedChunkMin != currentChunk.getMin())) {
            // Left chunk: inherits history and min of the original chunk, max equal to the min of
            // the new moved range. Major version equal to the new chunk's one, min version bumped.
            ChunkType leftSplitChunk = currentChunk;
            leftSplitChunk.setName(OID::gen());
            leftSplitChunk.setMax(movedChunkMin);
            leftSplitChunk.setVersion(
                ChunkVersion({movedChunkVersion.epoch(), movedChunkVersion.getTimestamp()},
                             {movedChunkVersion.majorVersion(), minVersionIncrement++}));
            newSplitChunks->emplace_back(std::move(leftSplitChunk));
        }

        if (SimpleBSONObjComparator::kInstance.evaluate(movedChunkMax != currentChunk.getMax())) {
            // Right chunk: min equal to the max of the new moved range, inherits history and min of
            // the original chunk. Major version equal to the new chunk's one, min version bumped.
            ChunkType rightSplitChunk = currentChunk;
            rightSplitChunk.setName(OID::gen());
            rightSplitChunk.setMin(movedChunkMax);
            rightSplitChunk.setVersion(
                ChunkVersion({movedChunkVersion.epoch(), movedChunkVersion.getTimestamp()},
                             {movedChunkVersion.majorVersion(), minVersionIncrement++}));
            newSplitChunks->emplace_back(std::move(rightSplitChunk));
        }
    }

    auto controlChunk = getControlChunkForMigrate(opCtx,
                                                  _localConfigShard.get(),
                                                  coll.getUuid(),
                                                  coll.getEpoch(),
                                                  coll.getTimestamp(),
                                                  currentChunk,
                                                  fromShard);
    std::shared_ptr<ChunkType> newControlChunk = nullptr;

    if (controlChunk) {
        // Find the chunk history.
        auto origControlChunk = uassertStatusOK(_findChunkOnConfig(
            opCtx, coll.getUuid(), coll.getEpoch(), coll.getTimestamp(), controlChunk->getMin()));

        newControlChunk = std::make_shared<ChunkType>(origControlChunk);
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
        response.collectionPlacementVersion = newMigratedChunk->getVersion();
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
        ChunkType::ConfigNS,
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
            write_ops::UpdateCommandRequest updateOp(ChunkType::ConfigNS);
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
        request.setWriteConcern(ShardingCatalogClient::kLocalWriteConcern.toBSON());

        auto response = _localConfigShard->runBatchWriteCommand(
            opCtx, Shard::kDefaultConfigCommandTimeout, request, Shard::RetryPolicy::kIdempotent);
        uassertStatusOK(response.toStatus());

        uassert(ErrorCodes::Error(5760502),
                str::stream() << "No chunks found for collection " << nss.ns(),
                response.getN() > 0);
    }

    // Find the chunk history
    const auto allChunksVector = [&, collUuid = coll.getUuid()] {
        auto findChunksResponse = uassertStatusOK(_localConfigShard->exhaustiveFindOnConfig(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            repl::ReadConcernLevel::kLocalReadConcern,
            ChunkType::ConfigNS,
            BSON(ChunkType::collectionUUID() << collUuid),
            BSONObj(),
            boost::none));
        uassert(ErrorCodes::Error(5760503),
                str::stream() << "No chunks found for collection " << nss.ns(),
                !findChunksResponse.docs.empty());
        return std::move(findChunksResponse.docs);
    }();

    // Bump the major version in order to be guaranteed to trigger refresh on every shard
    ChunkVersion newCollectionPlacementVersion(
        {collPlacementVersion.epoch(), collPlacementVersion.getTimestamp()},
        {collPlacementVersion.majorVersion() + 1, 0});
    std::set<ShardId> changedShardIds;
    for (const auto& chunk : allChunksVector) {
        auto upgradeChunk = uassertStatusOK(ChunkType::parseFromConfigBSON(
            chunk, collPlacementVersion.epoch(), collPlacementVersion.getTimestamp()));
        bool historyIsAt40 = chunk[ChunkType::historyIsAt40()].booleanSafe();
        if (historyIsAt40) {
            uassert(
                ErrorCodes::Error(5760504),
                str::stream() << "Chunk " << upgradeChunk.getName() << " in collection " << nss.ns()
                              << " indicates that it has been upgraded to version 4.0, but is "
                                 "missing the history field. This indicates a corrupted routing "
                                 "table and requires a manual intervention to be fixed.",
                !upgradeChunk.getHistory().empty());
            continue;
        }

        upgradeChunk.setVersion(newCollectionPlacementVersion);
        newCollectionPlacementVersion.incMinor();
        changedShardIds.emplace(upgradeChunk.getShard());

        // Construct the fresh history.
        upgradeChunk.setOnCurrentShardSince(validAfter);
        upgradeChunk.setHistory(
            {ChunkHistory{*upgradeChunk.getOnCurrentShardSince(), upgradeChunk.getShard()}});

        // Set the 'historyIsAt40' field so that it gets skipped if the command is re-run
        BSONObjBuilder chunkObjBuilder(upgradeChunk.toConfigBSON());
        chunkObjBuilder.appendBool(ChunkType::historyIsAt40(), true);

        // Run the update
        uassertStatusOK(
            _localCatalogClient->updateConfigDocument(opCtx,
                                                      ChunkType::ConfigNS,
                                                      BSON(ChunkType::name(upgradeChunk.getName())),
                                                      chunkObjBuilder.obj(),
                                                      false,
                                                      ShardingCatalogClient::kLocalWriteConcern));
    }

    // Wait for the writes to become majority committed so that the subsequent shard refreshes can
    // see them
    const auto clientOpTime = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
    WriteConcernResult unusedWCResult;
    uassertStatusOK(waitForWriteConcern(
        opCtx, clientOpTime, ShardingCatalogClient::kMajorityWriteConcern, &unusedWCResult));

    for (const auto& shardId : changedShardIds) {
        auto shard = uassertStatusOK(shardRegistry->getShard(opCtx, shardId));
        uassertStatusOK(
            Shard::CommandResponse::getEffectiveStatus(shard->runCommandWithFixedRetryAttempts(
                opCtx,
                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                "admin",
                BSON("_flushRoutingTableCacheUpdates" << nss.ns()),
                Shard::RetryPolicy::kIdempotent)));
    }
}

void ShardingCatalogManager::setOnCurrentShardSinceFieldOnChunks(OperationContext* opCtx) {
    {
        // Take _kChunkOpLock in exclusive mode to prevent concurrent chunk modifications
        Lock::ExclusiveLock lk(opCtx, _kChunkOpLock);

        DBDirectClient dbClient(opCtx);

        // 1st match only chunks with non empty history
        BSONObj query = BSON("history.0" << BSON("$exists" << true));

        // 2nd use the $set aggregation stage pipeline to set `onCurrentShardSince` to the same
        // value as the `validAfter` field on the first element of `history` array
        // [
        //    {
        //        $set: {
        //            onCurrentShardSince: {
        //                $getField: { field: "validAfter", input: { $first : "$history" } }
        //        }
        //    }
        //  ]

        BSONObj update =
            BSON("$set" << BSON(
                     ChunkType::onCurrentShardSince()
                     << BSON("$getField"
                             << BSON("field" << ChunkHistoryBase::kValidAfterFieldName << "input"
                                             << BSON("$first" << ("$" + ChunkType::history()))))));

        auto response = dbClient.runCommand([&] {
            write_ops::UpdateCommandRequest updateOp(ChunkType::ConfigNS);

            updateOp.setUpdates({[&] {
                // Sending a vector as an update to make sure we use an aggregation pipeline
                write_ops::UpdateOpEntry entry;
                entry.setQ(query);
                entry.setU(std::vector<BSONObj>{update.getOwned()});
                entry.setMulti(true);
                entry.setUpsert(false);
                return entry;
            }()});
            updateOp.getWriteCommandRequestBase().setOrdered(false);
            return updateOp.serialize({});
        }());

        uassertStatusOK(getStatusFromWriteCommandReply(response->getCommandReply()));
    }

    const auto clientOpTime = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
    WriteConcernResult unusedWCResult;
    uassertStatusOK(waitForWriteConcern(
        opCtx, clientOpTime, ShardingCatalogClient::kMajorityWriteConcern, &unusedWCResult));
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
        BSON(CollectionType::kNssFieldName << nss.ns()),
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
        ChunkType::ConfigNS,
        targetChunkQuery,
        {},
        1));

    const auto targetChunkVector = std::move(targetChunkResult.docs);
    uassert(51262,
            str::stream() << "Unable to locate chunk " << chunk.toString()
                          << " from ns: " << nss.ns(),
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
        ChunkType::ConfigNS,
        allChunksQuery,
        BSON(ChunkType::lastmod << -1),
        1));

    const auto chunksVector = std::move(findResponse.docs);
    uassert(ErrorCodes::IncompatibleShardingMetadata,
            str::stream() << "Tried to find max chunk version for collection '" << nss.ns()
                          << ", but found no chunks",
            !chunksVector.empty());

    const auto highestVersionChunk = uassertStatusOK(
        ChunkType::parseFromConfigBSON(chunksVector.front(), coll.getEpoch(), coll.getTimestamp()));
    const auto currentCollectionPlacementVersion = highestVersionChunk.getVersion();

    // It is possible for a migration to end up running partly without the protection of the
    // distributed lock if the config primary stepped down since the start of the migration and
    // failed to recover the migration. Check that the collection has not been dropped and recreated
    // or had its shard key refined since the migration began, unbeknown to the shard when the
    // command was sent.
    uassert(ErrorCodes::StaleEpoch,
            str::stream() << "The epoch of collection '" << nss.ns()
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

    auto didUpdate =
        uassertStatusOK(_localCatalogClient->updateConfigDocument(opCtx,
                                                                  ChunkType::ConfigNS,
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
                                ChunkType::ConfigNS,
                                requestedChunkQuery,
                                BSONObj() /* sort */,
                                1 /* limit */))
                .docs;
        if (matchingChunksVector.empty()) {
            // This can happen in a number of cases, such as that the collection has been
            // dropped, its shard key has been refined, the chunk has been split, or the chunk
            // has been merged.
            LOGV2(23884,
                  "ensureChunkVersionIsGreaterThan did not find any chunks with minKey {minKey}, "
                  "maxKey {maxKey}, and epoch {epoch}. Returning success.",
                  "ensureChunkVersionIsGreaterThan did not find any matching chunks; returning "
                  "success",
                  "minKey"_attr = minKey,
                  "maxKey"_attr = maxKey,
                  "epoch"_attr = version.epoch());
            return;
        }

        matchingChunk = uassertStatusOK(ChunkType::parseFromConfigBSON(
            matchingChunksVector.front(), coll.getEpoch(), coll.getTimestamp()));

        if (version.isOlderThan(matchingChunk.getVersion())) {
            LOGV2(23885,
                  "ensureChunkVersionIsGreaterThan found that the chunk with minKey {minKey}, "
                  "maxKey "
                  "{maxKey}, and epoch {epoch} already has a higher version than {version}. "
                  "Current "
                  "chunk is {currentChunk}. Returning success.",
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
                                ChunkType::ConfigNS,
                                query,
                                BSON(ChunkType::lastmod << -1) /* sort */,
                                1 /* limit */))
                .docs;
        if (highestChunksVector.empty()) {
            LOGV2(23886,
                  "ensureChunkVersionIsGreaterThan did not find any chunks with epoch {epoch} "
                  "when "
                  "attempting to find the collection placement version. The collection must have "
                  "been "
                  "dropped "
                  "concurrently or had its shard key refined. Returning success.",
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
    auto didUpdate =
        uassertStatusOK(_localCatalogClient->updateConfigDocument(opCtx,
                                                                  ChunkType::ConfigNS,
                                                                  requestedChunkQuery,
                                                                  newChunk.toConfigBSON(),
                                                                  false /* upsert */,
                                                                  kNoWaitWriteConcern));
    if (didUpdate) {
        LOGV2(23887,
              "ensureChunkVersionIsGreaterThan bumped the version of the chunk with minKey "
              "{minKey}, "
              "maxKey {maxKey}, and epoch {epoch}. Chunk is now {newChunk}",
              "ensureChunkVersionIsGreaterThan bumped the the chunk version",
              "minKey"_attr = minKey,
              "maxKey"_attr = maxKey,
              "epoch"_attr = version.epoch(),
              "newChunk"_attr = newChunk.toConfigBSON());
    } else {
        LOGV2(23888,
              "ensureChunkVersionIsGreaterThan did not find a chunk matching minKey {minKey}, "
              "maxKey {maxKey}, and epoch {epoch} when trying to bump its version. The "
              "collection "
              "must have been dropped concurrently or had its shard key refined. Returning "
              "success.",
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
        opCtx, nss, std::move(changeMetadataFunc), ShardingCatalogClient::kMajorityWriteConcern);
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
        opCtx,
        collNames,
        std::move(changeMetadataFunc),
        ShardingCatalogClient::kMajorityWriteConcern);
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
    const auto [cm, _] = uassertStatusOK(
        Grid::get(opCtx)->catalogCache()->getShardedCollectionRoutingInfoWithPlacementRefresh(opCtx,
                                                                                              nss));
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
        auto splitPoints = uassertStatusOK(
            shardutil::selectChunkSplitPoints(opCtx,
                                              chunk.getShardId(),
                                              nss,
                                              cm.getShardKeyPattern(),
                                              ChunkRange(chunk.getMin(), chunk.getMax()),
                                              maxChunkSizeBytes,
                                              limit));

        if (splitPoints.empty()) {
            LOGV2(21873,
                  "Marking chunk {chunk} as jumbo",
                  "Marking chunk as jumbo",
                  "chunk"_attr = redact(chunk.toString()));
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
                BSON(CollectionType::kNssFieldName << nss.ns()),
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
                ChunkType::ConfigNS,
                chunkQuery,
                BSON("$set" << BSON(ChunkType::jumbo(true))),
                false,
                ShardingCatalogClient::kMajorityWriteConcern);
            if (!status.isOK()) {
                LOGV2(21874,
                      "Couldn't mark chunk with namespace {namespace} and min key {minKey} as "
                      "jumbo due to {error}",
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
        uassertStatusOK(
            shardutil::splitChunkAtMultiplePoints(opCtx,
                                                  chunk.getShardId(),
                                                  nss,
                                                  cm.getShardKeyPattern(),
                                                  cm.getVersion().epoch(),
                                                  cm.getVersion().getTimestamp(),
                                                  ChunkRange(chunk.getMin(), chunk.getMax()),
                                                  splitPoints));
    } catch (const DBException&) {
    }
}

void ShardingCatalogManager::setAllowMigrationsAndBumpOneChunk(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const boost::optional<UUID>& collectionUUID,
    bool allowMigrations) {
    std::set<ShardId> cmShardIds;
    {
        // Mark opCtx as interruptible to ensure that all reads and writes to the metadata
        // collections under the exclusive _kChunkOpLock happen on the same term.
        opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

        // Take _kChunkOpLock in exclusive mode to prevent concurrent chunk splits, merges, and
        // migrations
        Lock::ExclusiveLock lk(opCtx, _kChunkOpLock);

        const auto cm =
            uassertStatusOK(Grid::get(opCtx)
                                ->catalogCache()
                                ->getShardedCollectionRoutingInfoWithPlacementRefresh(opCtx, nss))
                .cm;

        uassert(ErrorCodes::InvalidUUID,
                str::stream() << "Collection uuid " << collectionUUID
                              << " in the request does not match the current uuid " << cm.getUUID()
                              << " for ns " << nss,
                !collectionUUID || collectionUUID == cm.getUUID());

        cm.getAllShardIds(&cmShardIds);
        withTransaction(
            opCtx,
            CollectionType::ConfigNS,
            [this, allowMigrations, &nss, &collectionUUID](OperationContext* opCtx,
                                                           TxnNumber txnNumber) {
                // Update the 'allowMigrations' field. An unset 'allowMigrations' field implies
                // 'true'. To ease backwards compatibility we omit 'allowMigrations' instead of
                // setting it explicitly to 'true'.
                const auto update = allowMigrations
                    ? BSON("$unset" << BSON(CollectionType::kAllowMigrationsFieldName << ""))
                    : BSON("$set" << BSON(CollectionType::kAllowMigrationsFieldName << false));

                BSONObj query = BSON(CollectionType::kNssFieldName << nss.ns());
                if (collectionUUID) {
                    query =
                        query.addFields(BSON(CollectionType::kUuidFieldName << *collectionUUID));
                }

                const auto res = writeToConfigDocumentInTxn(
                    opCtx,
                    CollectionType::ConfigNS,
                    BatchedCommandRequest::buildUpdateOp(CollectionType::ConfigNS,
                                                         query,
                                                         update /* update */,
                                                         false /* upsert */,
                                                         false /* multi */),
                    txnNumber);
                const auto numDocsModified = UpdateOp::parseResponse(res).getN();
                uassert(ErrorCodes::ConflictingOperationInProgress,
                        str::stream() << "Expected to match one doc for query " << query
                                      << " but matched " << numDocsModified,
                        numDocsModified == 1);

                bumpCollectionMinorVersion(opCtx, _localConfigShard.get(), nss, txnNumber);
            });

        // From now on migrations are not allowed anymore, so it is not possible that new shards
        // will own chunks for this collection.
    }

    // Trigger a refresh on each shard containing chunks for this collection.
    const auto executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
    // TODO (SERVER-74477): Remove cmShardIds and always send the refresh to all shards.
    if (feature_flags::gAllowMigrationsRefreshToAll.isEnabled(
            serverGlobalParams.featureCompatibility)) {
        const auto allShardIds = Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx);
        sharding_util::tellShardsToRefreshCollection(opCtx, allShardIds, nss, executor);
    } else {
        sharding_util::tellShardsToRefreshCollection(opCtx,
                                                     {std::make_move_iterator(cmShardIds.begin()),
                                                      std::make_move_iterator(cmShardIds.end())},
                                                     nss,
                                                     executor);
    }
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

    auto didUpdate = uassertStatusOK(_localCatalogClient->updateConfigDocument(opCtx,
                                                                               ChunkType::ConfigNS,
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
    request.setWriteConcern(ShardingCatalogClient::kMajorityWriteConcern.toBSON());

    auto response = _localConfigShard->runBatchWriteCommand(
        opCtx, Shard::kDefaultConfigCommandTimeout, request, Shard::RetryPolicy::kIdempotent);

    uassertStatusOK(response.toStatus());
    return response.getN() > 0;
}

void ShardingCatalogManager::_commitChunkMigrationInTransaction(
    OperationContext* opCtx,
    const NamespaceString& nss,
    std::shared_ptr<const ChunkType> migratedChunk,
    std::shared_ptr<const std::vector<ChunkType>> splitChunks,
    std::shared_ptr<ChunkType> controlChunk,
    const ShardId& donorShardId) {
    // Verify the placement info for collectionUUID needs to be updated because the donor is losing
    // its last chunk for the namespace.
    const auto removeDonorFromPlacementHistory = !controlChunk && splitChunks->empty();

    // Verify the placement info for collectionUUID needs to be updated because the recipient is
    // acquiring its first chunk for the namespace.
    const auto addRecipientToPlacementHistory = [&] {
        const auto chunkQuery =
            BSON(ChunkType::collectionUUID << migratedChunk->getCollectionUUID() << ChunkType::shard
                                           << migratedChunk->getShard());
        auto findResponse = uassertStatusOK(_localConfigShard->exhaustiveFindOnConfig(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            repl::ReadConcernLevel::kLocalReadConcern,
            ChunkType::ConfigNS,
            chunkQuery,
            BSONObj(),
            1 /* limit */));
        return findResponse.docs.empty();
    }();

    const auto configChunksUpdateRequest = [&migratedChunk, &splitChunks, &controlChunk] {
        write_ops::UpdateCommandRequest updateOp(ChunkType::ConfigNS);
        std::vector<write_ops::UpdateOpEntry> updateEntries;
        updateEntries.reserve(1 + splitChunks->size() + (controlChunk ? 1 : 0));

        auto buildUpdateOpEntry = [](const ChunkType& chunk,
                                     bool isUpsert) -> write_ops::UpdateOpEntry {
            write_ops::UpdateOpEntry entry;

            entry.setUpsert(isUpsert);
            auto chunkID = MONGO_unlikely(migrateCommitInvalidChunkQuery.shouldFail())
                ? OID::gen()
                : chunk.getName();

            entry.setQ(BSON(ChunkType::name() << chunkID));
            entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(chunk.toConfigBSON()));
            return entry;
        };

        updateEntries.emplace_back(buildUpdateOpEntry(*migratedChunk, false));
        for (const auto& splitChunk : *splitChunks) {
            updateEntries.emplace_back(buildUpdateOpEntry(splitChunk, true));
        }

        if (controlChunk) {
            updateEntries.emplace_back(buildUpdateOpEntry(*controlChunk, false));
        }

        updateOp.setUpdates(updateEntries);
        return updateOp;
    }();

    auto transactionChain =
        [nss,
         collUuid = migratedChunk->getCollectionUUID(),
         donorShardId,
         recipientShardId = migratedChunk->getShard(),
         migrationCommitTime = migratedChunk->getHistory().front().getValidAfter(),
         configChunksUpdateRequest = std::move(configChunksUpdateRequest),
         removeDonorFromPlacementHistory,
         addRecipientToPlacementHistory](const txn_api::TransactionClient& txnClient,
                                         ExecutorPtr txnExec) -> SemiFuture<void> {
        const long long nChunksToUpdate = configChunksUpdateRequest.getUpdates().size();

        auto updateConfigChunksFuture =
            txnClient.runCRUDOp(configChunksUpdateRequest, {})
                .thenRunOn(txnExec)
                .then([nChunksToUpdate](const BatchedCommandResponse& updateResponse) {
                    uassertStatusOK(updateResponse.toStatus());
                    uassert(ErrorCodes::UpdateOperationFailed,
                            str::stream()
                                << "Commit chunk migration in transaction failed: N chunks updated "
                                << updateResponse.getN() << " expected " << nChunksToUpdate,
                            updateResponse.getN() == nChunksToUpdate);
                });

        if (!(removeDonorFromPlacementHistory || addRecipientToPlacementHistory)) {
            // End the transaction here.
            return std::move(updateConfigChunksFuture).semi();
        }

        // The main method to store placement info as part of the transaction, given a valid
        // descriptor.
        auto persistPlacementInfoSubchain = [txnExec,
                                             &txnClient](NamespacePlacementType&& placementInfo) {
            write_ops::InsertCommandRequest insertPlacementEntry(
                NamespaceString::kConfigsvrPlacementHistoryNamespace, {placementInfo.toBSON()});
            return txnClient.runCRUDOp(insertPlacementEntry, {})
                .thenRunOn(txnExec)
                .then([](const BatchedCommandResponse& insertPlacementEntryResponse) {
                    uassertStatusOK(insertPlacementEntryResponse.toStatus());
                })
                .semi();
        };

        // Obtain a valid placement descriptor from config.chunks and then store it as part of the
        // transaction.
        auto generateAndPersistPlacementInfoSubchain =
            [txnExec, &txnClient](const NamespaceString& nss,
                                  const UUID& collUuid,
                                  const Timestamp& migrationCommitTime) {
                // Compose the query - equivalent to
                // 'configDb.chunks.distinct("shard", {uuid:collectionUuid})'
                DistinctCommandRequest distinctRequest(ChunkType::ConfigNS);
                distinctRequest.setKey(ChunkType::shard.name());
                distinctRequest.setQuery(BSON(ChunkType::collectionUUID.name() << collUuid));
                return txnClient.runCommand(DatabaseName::kConfig, distinctRequest.toBSON({}))
                    .thenRunOn(txnExec)
                    .then([=, &txnClient](BSONObj reply) {
                        uassertStatusOK(getStatusFromWriteCommandReply(reply));
                        std::vector<ShardId> shardIds;
                        for (const auto& valueElement : reply.getField("values").Array()) {
                            shardIds.emplace_back(valueElement.String());
                        }

                        NamespacePlacementType placementInfo(
                            nss, migrationCommitTime, std::move(shardIds));
                        placementInfo.setUuid(collUuid);
                        write_ops::InsertCommandRequest insertPlacementEntry(
                            NamespaceString::kConfigsvrPlacementHistoryNamespace,
                            {placementInfo.toBSON()});

                        return txnClient.runCRUDOp(insertPlacementEntry, {});
                    })
                    .thenRunOn(txnExec)
                    .then([](const BatchedCommandResponse& insertPlacementEntryResponse) {
                        uassertStatusOK(insertPlacementEntryResponse.toStatus());
                    })
                    .semi();
            };

        // Extend the transaction to also upsert the placement information that matches the
        // migration commit.
        return std::move(updateConfigChunksFuture)
            .thenRunOn(txnExec)
            .then([&] {
                // Retrieve the previous placement entry - it will be used as a base for the next
                // update.
                FindCommandRequest placementInfoQuery{
                    NamespaceString::kConfigsvrPlacementHistoryNamespace};
                placementInfoQuery.setFilter(BSON(NamespacePlacementType::kNssFieldName
                                                  << nss.toString()
                                                  << NamespacePlacementType::kTimestampFieldName
                                                  << BSON("$lte" << migrationCommitTime)));
                placementInfoQuery.setSort(BSON(NamespacePlacementType::kTimestampFieldName << -1));
                placementInfoQuery.setLimit(1);
                return txnClient.exhaustiveFind(placementInfoQuery);
            })
            .thenRunOn(txnExec)
            .then([&,
                   persistPlacementInfo = std::move(persistPlacementInfoSubchain),
                   generateAndPersistPlacementInfo =
                       std::move(generateAndPersistPlacementInfoSubchain)](
                      const std::vector<BSONObj>& queryResponse) {
                tassert(6892800,
                        str::stream()
                            << "Unexpected number of placement entries retrieved" << nss.toString(),
                        queryResponse.size() <= 1);

                if (queryResponse.size() == 0) {
                    //  Historical placement data may not be available due to an FCV transition -
                    //  invoke the more expensive fallback method.
                    return generateAndPersistPlacementInfo(nss, collUuid, migrationCommitTime);
                }

                // Leverage the most recent placement info to build the new version.
                auto placementInfo = NamespacePlacementType::parse(
                    IDLParserContext("CommitMoveChunk"), queryResponse.front());
                placementInfo.setTimestamp(migrationCommitTime);

                const auto& originalShardList = placementInfo.getShards();
                std::vector<ShardId> updatedShardList;
                updatedShardList.reserve(originalShardList.size() + 1);
                if (addRecipientToPlacementHistory) {
                    updatedShardList.push_back(recipientShardId);
                }

                std::copy_if(std::make_move_iterator(originalShardList.begin()),
                             std::make_move_iterator(originalShardList.end()),
                             std::back_inserter(updatedShardList),
                             [&](const ShardId& shardId) {
                                 if (removeDonorFromPlacementHistory && shardId == donorShardId) {
                                     return false;
                                 }
                                 if (addRecipientToPlacementHistory &&
                                     shardId == recipientShardId) {
                                     // Ensure that the added recipient will only appear once.
                                     return false;
                                 }
                                 return true;
                             });
                placementInfo.setShards(std::move(updatedShardList));

                return persistPlacementInfo(std::move(placementInfo));
            })
            .semi();
    };

    auto executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
    auto inlineExecutor = std::make_shared<executor::InlineExecutor>();
    auto sleepInlineExecutor = inlineExecutor->getSleepableExecutor(executor);

    txn_api::SyncTransactionWithRetries txn(opCtx, sleepInlineExecutor, nullptr, inlineExecutor);

    txn.run(opCtx, transactionChain);
}

Timestamp ShardingCatalogManager::getOldestTimestampSupportedForSnapshotHistory(
    OperationContext* opCtx) {
    const auto currTime = VectorClock::get(opCtx)->getTime();
    auto currTimeSeconds = currTime.clusterTime().asTimestamp().getSecs();
    return Timestamp(currTimeSeconds - getHistoryWindowInSeconds(), 0);
}
}  // namespace mongo
