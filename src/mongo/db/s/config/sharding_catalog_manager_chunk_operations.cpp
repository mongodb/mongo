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
#include "mongo/db/logical_session_cache.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/distinct_command_gen.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/s/sharding_logging.h"
#include "mongo/db/s/sharding_util.h"
#include "mongo/db/server_options.h"
#include "mongo/db/snapshot_window_options_gen.h"
#include "mongo/db/transaction_api.h"
#include "mongo/db/transaction_participant_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/s/shard_util.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(migrationCommitVersionError);
MONGO_FAIL_POINT_DEFINE(migrateCommitInvalidChunkQuery);
MONGO_FAIL_POINT_DEFINE(skipExpiringOldChunkHistory);

const WriteConcernOptions kNoWaitWriteConcern(1, WriteConcernOptions::SyncMode::UNSET, Seconds(0));

constexpr StringData kCollectionVersionField = "collectionVersion"_sd;

/**
 * Append min, max and version information from chunk to the buffer for logChange purposes.
 */
void appendShortVersion(BufBuilder* out, const ChunkType& chunk) {
    BSONObjBuilder bb(*out);
    bb.append(ChunkType::min(), chunk.getMin());
    bb.append(ChunkType::max(), chunk.getMax());
    if (chunk.isVersionSet()) {
        chunk.getVersion().serializeToBSON(ChunkType::lastmod(), &bb);
    }
    bb.done();
}

/**
 * Check that the chunk still exists and return its metadata.
 */
StatusWith<ChunkType> findChunkContainingRange(OperationContext* opCtx,
                                               const UUID& uuid,
                                               const OID& epoch,
                                               const Timestamp& timestamp,
                                               const BSONObj& min,
                                               const BSONObj& max) {
    const auto chunkQuery = [&]() {
        BSONObjBuilder queryBuilder;
        queryBuilder << ChunkType::collectionUUID << uuid;
        queryBuilder << ChunkType::min(BSON("$lte" << min));
        queryBuilder << ChunkType::max(BSON("$gte" << max));
        return queryBuilder.obj();
    }();

    // Must use local read concern because we're going to perform subsequent writes.
    auto findResponseWith =
        Grid::get(opCtx)->shardRegistry()->getConfigShard()->exhaustiveFindOnConfig(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            repl::ReadConcernLevel::kLocalReadConcern,
            ChunkType::ConfigNS,
            chunkQuery,
            BSONObj(),
            2 /* limit */);

    if (!findResponseWith.isOK()) {
        return findResponseWith.getStatus();
    }

    if (findResponseWith.getValue().docs.size() != 1) {
        return {ErrorCodes::Error(40165),
                str::stream() << "Could not find a chunk including bounds [" << min << ", " << max
                              << "). Cannot execute the migration commit with invalid chunks."};
    }

    return uassertStatusOK(
        ChunkType::parseFromConfigBSON(findResponseWith.getValue().docs.front(), epoch, timestamp));
}

BSONObj buildCountChunksCommand(const std::vector<ChunkType>& chunks) {
    AggregateCommandRequest countRequest(ChunkType::ConfigNS);

    BSONObjBuilder builder;
    builder.append("aggregate", ChunkType::ConfigNS.ns());

    BSONArrayBuilder arrayBuilder;
    for (const auto& chunk : chunks) {
        auto query =
            BSON(ChunkType::min(chunk.getMin())
                 << ChunkType::max(chunk.getMax()) << ChunkType::collectionUUID()
                 << chunk.getCollectionUUID() << ChunkType::shard() << chunk.getShard().toString());
        arrayBuilder.append(query);
    }
    std::vector<BSONObj> pipeline;
    pipeline.push_back(BSON("$match" << BSON("$or" << arrayBuilder.arr())));
    pipeline.push_back(BSON("$count" << ChunkType::collectionUUID.name()));
    countRequest.setPipeline(pipeline);

    return countRequest.toBSON({});
}

/**
 * Returns a chunk different from the one being migrated or 'none' if one doesn't exist.
 */
boost::optional<ChunkType> getControlChunkForMigrate(OperationContext* opCtx,
                                                     const UUID& uuid,
                                                     const OID& epoch,
                                                     const Timestamp& timestamp,
                                                     const ChunkType& migratedChunk,
                                                     const ShardId& fromShard) {
    auto const configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

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
 * Helper function to find collection version and shard version.
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
 * Helper function to get the collection version for nss. Always uses kLocalReadConcern.
 */
StatusWith<ChunkVersion> getCollectionVersion(OperationContext* opCtx, const NamespaceString& nss) {
    auto const configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
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
        return {ErrorCodes::Error(5057701),
                str::stream() << "Collection '" << nss.ns() << "' no longer either exists"};
    }

    const CollectionType coll(findCollResponse.getValue().docs[0]);
    const auto chunksQuery = BSON(ChunkType::collectionUUID << coll.getUuid());
    return getMaxChunkVersionFromQueryResponse(
        coll,
        Grid::get(opCtx)->shardRegistry()->getConfigShard()->exhaustiveFindOnConfig(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            repl::ReadConcernLevel::kLocalReadConcern,
            ChunkType::ConfigNS,
            chunksQuery,                     // Query all chunks for this namespace.
            BSON(ChunkType::lastmod << -1),  // Sort by version.
            1));                             // Limit 1.
}

ChunkVersion getShardVersion(OperationContext* opCtx,
                             const CollectionType& coll,
                             const ShardId& fromShard,
                             const ChunkVersion& collectionVersion) {
    const auto chunksQuery =
        BSON(ChunkType::collectionUUID << coll.getUuid() << ChunkType::shard(fromShard.toString()));

    auto swDonorShardVersion = getMaxChunkVersionFromQueryResponse(
        coll,
        Grid::get(opCtx)->shardRegistry()->getConfigShard()->exhaustiveFindOnConfig(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            repl::ReadConcernLevel::kLocalReadConcern,
            ChunkType::ConfigNS,
            chunksQuery,
            BSON(ChunkType::lastmod << -1),  // Sort by version.
            1));
    if (!swDonorShardVersion.isOK()) {
        if (swDonorShardVersion.getStatus().code() == 50577) {
            // The query to find 'nss' chunks belonging to the donor shard didn't return any chunks,
            // meaning the last chunk for fromShard was donated. Gracefully handle the error.
            return ChunkVersion({collectionVersion.epoch(), collectionVersion.getTimestamp()},
                                {0, 0});
        } else {
            // Bubble up any other error
            uassertStatusOK(swDonorShardVersion);
        }
    }
    return swDonorShardVersion.getValue();
}

void bumpCollectionMinorVersion(OperationContext* opCtx,
                                const NamespaceString& nss,
                                TxnNumber txnNumber) {
    auto const configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

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

std::vector<ShardId> getShardsOwningChunksForCollection(OperationContext* opCtx,
                                                        const NamespaceString& nss) {
    auto const configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    auto findCollResponse = uassertStatusOK(
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

    DistinctCommandRequest distinctCmd(ChunkType::ConfigNS, ChunkType::shard.name());
    distinctCmd.setQuery(BSON(ChunkType::collectionUUID << coll.getUuid()));

    const auto distinctResult = uassertStatusOK(configShard->runCommandWithFixedRetryAttempts(
        opCtx,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        NamespaceString::kConfigDb.toString(),
        distinctCmd.toBSON({}),
        Shard::RetryPolicy::kIdempotent));
    uassertStatusOK(distinctResult.commandStatus);

    const auto valuesElem = distinctResult.response.getField("values");
    std::vector<ShardId> shardIds;
    for (const auto& shard : valuesElem.Array()) {
        shardIds.emplace_back(shard.String());
    }
    uassert(ErrorCodes::IncompatibleShardingMetadata,
            str::stream() << "Tried to find shardIds owning chunks for collection '" << nss.ns()
                          << ", but found none",
            !shardIds.empty());

    return shardIds;
}

}  // namespace

void ShardingCatalogManager::bumpMajorVersionOneChunkPerShard(
    OperationContext* opCtx,
    const NamespaceString& nss,
    TxnNumber txnNumber,
    const std::vector<ShardId>& shardIds) {
    auto curCollectionVersion = uassertStatusOK(getCollectionVersion(opCtx, nss));
    ChunkVersion targetChunkVersion(
        {curCollectionVersion.epoch(), curCollectionVersion.getTimestamp()},
        {curCollectionVersion.majorVersion() + 1, 0});

    auto const configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    auto findCollResponse = uassertStatusOK(
        configShard->exhaustiveFindOnConfig(opCtx,
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
                                                 const ChunkVersion& collVersion,
                                                 const std::vector<BSONObj>& splitPoints) {
    auto newChunkBounds = std::make_shared<std::vector<BSONObj>>(splitPoints);
    newChunkBounds->push_back(range.getMax());
    // We need to use a shared pointer to prevent an scenario where the operation context is
    // interrupted and the scope containing SyncTransactionWithRetries goes away but the callback is
    // called from the executor thread.
    // TODO SERVER-66261: remove after SERVER-66261 is committed.
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
        nss, range, origChunk, shardName, collVersion, newChunkBounds);

    auto updateChunksFn = [sharedBlock](const txn_api::TransactionClient& txnClient,
                                        ExecutorPtr txnExec) {
        ChunkType chunk(sharedBlock->origChunk.getCollectionUUID(),
                        sharedBlock->range,
                        sharedBlock->currentMaxVersion,
                        sharedBlock->shardName);
        std::vector<ChunkType> chunks{chunk};
        auto countRequest = buildCountChunksCommand(chunks);
        return txnClient.runCommand(ChunkType::ConfigNS.db(), countRequest)
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

    txn_api::SyncTransactionWithRetries txn(
        opCtx, Grid::get(opCtx)->getExecutorPool()->getFixedExecutor(), nullptr);

    txn.run(opCtx, updateChunksFn);

    return ShardingCatalogManager::SplitChunkInTransactionResult{sharedBlock->currentMaxVersion,
                                                                 sharedBlock->newChunks};
}

StatusWith<BSONObj> ShardingCatalogManager::commitChunkSplit(
    OperationContext* opCtx,
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
    // strictly monotonously increasing collection versions
    Lock::ExclusiveLock lk(opCtx, opCtx->lockState(), _kChunkOpLock);

    auto const configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    auto findCollResponse = uassertStatusOK(
        configShard->exhaustiveFindOnConfig(opCtx,
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

    // Don't allow auto-splitting if the collection is being defragmented
    uassert(ErrorCodes::ConflictingOperationInProgress,
            str::stream() << "Can't commit auto-split while `" << nss.ns()
                          << "` is undergoing a defragmentation.",
            !(coll.getDefragmentCollection() && fromChunkSplitter));

    // Get the max chunk version for this namespace.
    auto swCollVersion = getCollectionVersion(opCtx, nss);

    if (!swCollVersion.isOK()) {
        return swCollVersion.getStatus().withContext(
            str::stream() << "splitChunk cannot split chunk " << range.toString() << ".");
    }

    auto collVersion = swCollVersion.getValue();

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

    auto splitChunkResult = _splitChunkInTransaction(
        opCtx, nss, range, shardName, origChunkStatus.getValue(), collVersion, splitPoints);

    // log changes
    BSONObjBuilder logDetail;
    {
        BSONObjBuilder b(logDetail.subobjStart("before"));
        b.append(ChunkType::min(), range.getMin());
        b.append(ChunkType::max(), range.getMax());
        collVersion.serializeToBSON(ChunkType::lastmod(), &b);
    }

    if (splitChunkResult.newChunks->size() == 2) {
        appendShortVersion(&logDetail.subobjStart("left"), splitChunkResult.newChunks->at(0));
        appendShortVersion(&logDetail.subobjStart("right"), splitChunkResult.newChunks->at(1));
        logDetail.append("owningShard", shardName);

        ShardingLogging::get(opCtx)->logChange(
            opCtx, "split", nss.ns(), logDetail.obj(), WriteConcernOptions());
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

            const auto status = ShardingLogging::get(opCtx)->logChangeChecked(
                opCtx, "multi-split", nss.ns(), chunkDetail.obj(), WriteConcernOptions());

            // Stop logging if the last log op failed because the primary stepped down
            if (status.code() == ErrorCodes::InterruptedDueToReplStateChange)
                break;
        }
    }

    BSONObjBuilder response;
    splitChunkResult.currentMaxVersion.serializeToBSON(kCollectionVersionField, &response);
    splitChunkResult.currentMaxVersion.serializeToBSON(ChunkVersion::kShardVersionField, &response);
    return response.obj();
}

void ShardingCatalogManager::_mergeChunksInTransaction(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const UUID& collectionUUID,
    const ChunkVersion& initialVersion,
    const ChunkVersion& mergeVersion,
    const boost::optional<Timestamp>& validAfter,
    std::shared_ptr<std::vector<ChunkType>> chunksToMerge) {
    dassert(validAfter);
    auto updateChunksFn = [chunksToMerge, mergeVersion, validAfter](
                              const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
        // Check the merge chunk precondition, chunks must not have moved.
        auto countRequest = buildCountChunksCommand(*chunksToMerge);
        auto countBSONObjSize = countRequest.objsize();
        uassert(
            ErrorCodes::InvalidOptions,
            str::stream() << "Cannot merge such large range, the final BSON count operation size "
                          << countBSONObjSize << " bytes, would exceed the maximum BSON size: "
                          << BSONObjMaxInternalSize << " bytes",
            countBSONObjSize < BSONObjMaxInternalSize);
        return txnClient.runCommand(ChunkType::ConfigNS.db(), countRequest)
            .thenRunOn(txnExec)
            .then([&txnClient, chunksToMerge, mergeVersion, validAfter](auto commandResponse) {
                auto countResponse =
                    uassertStatusOK(CursorResponse::parseFromBSON(commandResponse));
                uint64_t docCount = 0;
                auto firstBatch = countResponse.getBatch();
                if (!firstBatch.empty()) {
                    auto countObj = firstBatch.front();
                    docCount = countObj.getIntField(ChunkType::collectionUUID.name());
                    uassert(
                        ErrorCodes::BadValue, "Unexpected negative document count", docCount >= 0);
                }
                uassert(ErrorCodes::BadValue,
                        str::stream() << "Could not meet precondition to execute merge, expected "
                                      << chunksToMerge->size() << " chunks, but found " << docCount,
                        docCount == chunksToMerge->size());

                // Expand the first chunk into the newly merged chunks.
                write_ops::UpdateCommandRequest updateOp(ChunkType::ConfigNS);
                updateOp.setUpdates({[&] {
                    write_ops::UpdateOpEntry entry;

                    ChunkType mergedChunk(chunksToMerge->front());
                    entry.setQ(BSON(ChunkType::name(mergedChunk.getName())));
                    mergedChunk.setMax(chunksToMerge->back().getMax());

                    // Fill in additional details for sending through transaction.
                    mergedChunk.setVersion(mergeVersion);
                    mergedChunk.setEstimatedSizeBytes(boost::none);

                    mergedChunk.setHistory(
                        {ChunkHistory(validAfter.get(), mergedChunk.getShard())});

                    entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(
                        mergedChunk.toConfigBSON()));
                    entry.setMulti(false);

                    return entry;
                }()});

                return txnClient.runCRUDOp(updateOp, {});
            })
            .thenRunOn(txnExec)
            .then([&txnClient, chunksToMerge](auto chunkUpdateResponse) {
                uassertStatusOK(chunkUpdateResponse.toStatus());

                // Delete the rest of the chunks to be merged. Remember not to delete the first
                // chunk we're expanding.
                write_ops::DeleteCommandRequest deleteOp(ChunkType::ConfigNS);
                deleteOp.setDeletes([&] {
                    std::vector<write_ops::DeleteOpEntry> deletes;
                    for (size_t i = 1; i < chunksToMerge->size(); ++i) {
                        write_ops::DeleteOpEntry entry;
                        entry.setQ(BSON(ChunkType::name(chunksToMerge->at(i).getName())));
                        entry.setMulti(false);
                        deletes.push_back(entry);
                    }
                    return deletes;
                }());

                auto deleteBSONObjSize = deleteOp.toBSON({}).objsize();
                uassert(ErrorCodes::InvalidOptions,
                        str::stream()
                            << "Cannot merge such large range, the final delete request size "
                            << deleteBSONObjSize << " would exceed the maximum BSON size: "
                            << BSONObjMaxInternalSize << " bytes",
                        deleteBSONObjSize < BSONObjMaxInternalSize);
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

    txn_api::SyncTransactionWithRetries txn(
        opCtx, Grid::get(opCtx)->getExecutorPool()->getFixedExecutor(), nullptr);
    txn.run(opCtx, updateChunksFn);
}

StatusWith<BSONObj> ShardingCatalogManager::commitChunksMerge(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const boost::optional<OID>& epoch,
    const boost::optional<Timestamp>& timestamp,
    const UUID& requestCollectionUUID,
    const ChunkRange& chunkRange,
    const ShardId& shardId,
    const boost::optional<Timestamp>& validAfter) {
    if (!validAfter) {
        return {ErrorCodes::IllegalOperation, "chunk operation requires validAfter timestamp"};
    }

    // Mark opCtx as interruptible to ensure that all reads and writes to the metadata collections
    // under the exclusive _kChunkOpLock happen on the same term.
    opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

    // Take _kChunkOpLock in exclusive mode to prevent concurrent chunk modifications and generate
    // strictly monotonously increasing collection versions
    Lock::ExclusiveLock lk(opCtx, opCtx->lockState(), _kChunkOpLock);

    // 1. Retrieve the initial collection version info to build up the logging info.
    auto collVersion = uassertStatusOK(getCollectionVersion(opCtx, nss));
    uassert(ErrorCodes::StaleEpoch,
            "Collection changed",
            (!epoch || collVersion.epoch() == epoch) &&
                (!timestamp || collVersion.getTimestamp() == timestamp));

    // 2. Retrieve the list of chunks belonging to the requested shard + key range.
    const auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    auto findCollResponse = uassertStatusOK(
        configShard->exhaustiveFindOnConfig(opCtx,
                                            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                            repl::ReadConcernLevel::kLocalReadConcern,
                                            CollectionType::ConfigNS,
                                            BSON(CollectionType::kNssFieldName << nss.ns()),
                                            {},
                                            1));
    if (findCollResponse.docs.empty()) {
        return {ErrorCodes::Error(5678601),
                str::stream() << "Collection '" << nss.ns() << "' no longer either exists"};
    }

    const CollectionType coll(findCollResponse.docs[0]);
    if (coll.getUuid() != requestCollectionUUID) {
        return {
            ErrorCodes::InvalidUUID,
            str::stream() << "UUID of collection does not match UUID of request. Colletion UUID: "
                          << coll.getUuid() << ", request UUID: " << requestCollectionUUID};
    }
    const auto shardChunksInRangeQuery = [&]() {
        BSONObjBuilder queryBuilder;
        queryBuilder << ChunkType::collectionUUID << coll.getUuid();
        queryBuilder << ChunkType::shard(shardId.toString());
        queryBuilder << ChunkType::min(BSON("$gte" << chunkRange.getMin()));
        queryBuilder << ChunkType::min(BSON("$lt" << chunkRange.getMax()));
        return queryBuilder.obj();
    }();

    const auto shardChunksInRangeResponse = uassertStatusOK(
        configShard->exhaustiveFindOnConfig(opCtx,
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
        BSONObjBuilder response;
        collVersion.serializeToBSON(kCollectionVersionField, &response);
        const auto currentShardVersion = getShardVersion(opCtx, coll, shardId, collVersion);
        currentShardVersion.serializeToBSON(ChunkVersion::kShardVersionField, &response);
        // Makes sure that the last thing we read in getCollectionVersion and getShardVersion gets
        // majority written before to return from this command, otherwise next RoutingInfo cache
        // refresh from the shard may not see those newest information.
        repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);
        return response.obj();
    }

    // 3. Prepare the data for the merge
    //    and ensure that the retrieved list of chunks covers the whole range.
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
        chunksToMerge->push_back(std::move(chunk));
    }
    uassert(ErrorCodes::IllegalOperation,
            str::stream() << "could not merge chunks, shard " << shardId
                          << " does not contain a sequence of chunks that exactly fills the range "
                          << chunkRange.toString(),
            !chunksToMerge->empty() &&
                chunksToMerge->back().getMax().woCompare(chunkRange.getMax()) == 0);

    ChunkVersion initialVersion = collVersion;
    ChunkVersion mergeVersion = initialVersion;
    mergeVersion.incMinor();

    // 4. apply the batch of updates to local metadata
    _mergeChunksInTransaction(
        opCtx, nss, coll.getUuid(), initialVersion, mergeVersion, validAfter, chunksToMerge);

    // 5. log changes
    BSONObjBuilder logDetail;
    {
        BSONArrayBuilder b(logDetail.subarrayStart("merged"));
        for (const auto& chunkToMerge : *chunksToMerge) {
            b.append(chunkToMerge.toConfigBSON());
        }
    }
    initialVersion.serializeToBSON("prevShardVersion", &logDetail);
    mergeVersion.serializeToBSON("mergedVersion", &logDetail);
    logDetail.append("owningShard", shardId);

    ShardingLogging::get(opCtx)->logChange(
        opCtx, "merge", nss.ns(), logDetail.obj(), WriteConcernOptions());

    BSONObjBuilder response;
    mergeVersion.serializeToBSON(kCollectionVersionField, &response);
    mergeVersion.serializeToBSON(ChunkVersion::kShardVersionField, &response);
    return response.obj();
}

StatusWith<BSONObj> ShardingCatalogManager::commitChunkMigration(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ChunkType& migratedChunk,
    const OID& collectionEpoch,
    const Timestamp& collectionTimestamp,
    const ShardId& fromShard,
    const ShardId& toShard,
    const boost::optional<Timestamp>& validAfter) {

    if (!validAfter) {
        return {ErrorCodes::IllegalOperation, "chunk operation requires validAfter timestamp"};
    }

    // Mark opCtx as interruptible to ensure that all reads and writes to the metadata collections
    // under the exclusive _kChunkOpLock happen on the same term.
    opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

    // Must hold the shard lock until the entire commit finishes to serialize with removeShard.
    Lock::SharedLock shardLock(opCtx->lockState(), _kShardMembershipLock);

    auto const configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    auto shardResult = uassertStatusOK(
        configShard->exhaustiveFindOnConfig(opCtx,
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
    // strictly monotonously increasing collection versions
    Lock::ExclusiveLock lk(opCtx, opCtx->lockState(), _kChunkOpLock);

    auto findCollResponse = uassertStatusOK(
        configShard->exhaustiveFindOnConfig(opCtx,
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

    auto findResponse = uassertStatusOK(
        configShard->exhaustiveFindOnConfig(opCtx,
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
    const auto& currentCollectionVersion = chunk.getVersion();

    if (MONGO_unlikely(migrationCommitVersionError.shouldFail())) {
        uasserted(ErrorCodes::StaleEpoch,
                  "Failpoint 'migrationCommitVersionError' generated error");
    }

    // It is possible for a migration to end up running partly without the protection of the
    // distributed lock if the config primary stepped down since the start of the migration and
    // failed to recover the migration. Check that the collection has not been dropped and recreated
    // or had its shard key refined since the migration began, unbeknown to the shard when the
    // command was sent.
    if (currentCollectionVersion.epoch() != collectionEpoch ||
        currentCollectionVersion.getTimestamp() != collectionTimestamp) {
        return {ErrorCodes::StaleEpoch,
                str::stream() << "The epoch of collection '" << nss.ns()
                              << "' has changed since the migration began. The config server's "
                                 "collection version epoch is now '"
                              << currentCollectionVersion.epoch().toString()
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
                                                   coll.getUuid(),
                                                   coll.getEpoch(),
                                                   coll.getTimestamp(),
                                                   migratedChunk.getMin(),
                                                   migratedChunk.getMax());

    if (!swCurrentChunk.isOK()) {
        return swCurrentChunk.getStatus();
    }

    const auto currentChunk = std::move(swCurrentChunk.getValue());

    if (currentChunk.getShard() == toShard) {
        // The commit was already done successfully
        BSONObjBuilder response;
        currentCollectionVersion.serializeToBSON(kCollectionVersionField, &response);
        const auto currentShardVersion =
            getShardVersion(opCtx, coll, fromShard, currentCollectionVersion);
        currentShardVersion.serializeToBSON(ChunkVersion::kShardVersionField, &response);
        // Makes sure that the last thing we read in findChunkContainingRange, getShardVersion, and
        // getCollectionVersion gets majority written before to return from this command, otherwise
        // next RoutingInfo cache refresh from the shard may not see those newest information.
        repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);
        return response.obj();
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
    newMigratedChunk->setVersion(
        ChunkVersion({currentCollectionVersion.epoch(), currentCollectionVersion.getTimestamp()},
                     {currentCollectionVersion.majorVersion() + 1, minVersionIncrement++}));

    // Copy the complete history.
    auto newHistory = currentChunk.getHistory();
    invariant(validAfter);

    // Drop old history. Keep at least 1 entry so ChunkInfo::getShardIdAt finds valid history for
    // any query younger than the history window.
    if (!MONGO_unlikely(skipExpiringOldChunkHistory.shouldFail())) {
        auto windowInSeconds = std::max(std::max(minSnapshotHistoryWindowInSeconds.load(),
                                                 gTransactionLifetimeLimitSeconds.load()),
                                        10);
        int entriesDeleted = 0;
        while (newHistory.size() > 1 &&
               newHistory.back().getValidAfter().getSecs() + windowInSeconds <
                   validAfter.get().getSecs()) {
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

    if (!newHistory.empty() && newHistory.front().getValidAfter() >= validAfter.get()) {
        return {ErrorCodes::IncompatibleShardingMetadata,
                str::stream() << "The chunk history for chunk with namespace " << nss.ns()
                              << " and min key " << migratedChunk.getMin()
                              << " is corrupted. The last validAfter "
                              << newHistory.back().getValidAfter().toString()
                              << " is greater or equal to the new validAfter "
                              << validAfter.get().toString()};
    }
    newHistory.emplace(newHistory.begin(), ChunkHistory(validAfter.get(), toShard));
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

    auto controlChunk = getControlChunkForMigrate(
        opCtx, coll.getUuid(), coll.getEpoch(), coll.getTimestamp(), currentChunk, fromShard);
    std::shared_ptr<ChunkType> newControlChunk = nullptr;

    if (controlChunk) {
        // Find the chunk history.
        auto origControlChunk = uassertStatusOK(_findChunkOnConfig(
            opCtx, coll.getUuid(), coll.getEpoch(), coll.getTimestamp(), controlChunk->getMin()));

        newControlChunk = std::make_shared<ChunkType>(origControlChunk);
        // Setting control chunk's minor version to 1 on the donor shard.
        newControlChunk->setVersion(ChunkVersion(
            {currentCollectionVersion.epoch(), currentCollectionVersion.getTimestamp()},
            {currentCollectionVersion.majorVersion() + 1, minVersionIncrement++}));
    }

    _commitChunkMigrationInTransaction(
        opCtx, nss, newMigratedChunk, newSplitChunks, newControlChunk);

    BSONObjBuilder response;
    if (!newControlChunk) {
        // We migrated the last chunk from the donor shard.
        newMigratedChunk->getVersion().serializeToBSON(kCollectionVersionField, &response);
        const ChunkVersion donorShardVersion(
            {currentCollectionVersion.epoch(), currentCollectionVersion.getTimestamp()}, {0, 0});
        donorShardVersion.serializeToBSON(ChunkVersion::kShardVersionField, &response);
    } else {
        newControlChunk->getVersion().serializeToBSON(kCollectionVersionField, &response);
        newControlChunk->getVersion().serializeToBSON(ChunkVersion::kShardVersionField, &response);
    }
    return response.obj();
}

StatusWith<ChunkType> ShardingCatalogManager::_findChunkOnConfig(OperationContext* opCtx,
                                                                 const UUID& uuid,
                                                                 const OID& epoch,
                                                                 const Timestamp& timestamp,
                                                                 const BSONObj& key) {
    auto const configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

    const auto query = BSON(ChunkType::collectionUUID << uuid << ChunkType::min(key));
    auto findResponse =
        configShard->exhaustiveFindOnConfig(opCtx,
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
    auto const catalogClient = Grid::get(opCtx)->catalogClient();
    const auto shardRegistry = Grid::get(opCtx)->shardRegistry();

    // Mark opCtx as interruptible to ensure that all reads and writes to the metadata collections
    // under the exclusive _kChunkOpLock happen on the same term.
    opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

    // Take _kChunkOpLock in exclusive mode to prevent concurrent chunk splits, merges, and
    // migrations.
    Lock::ExclusiveLock lk(opCtx->lockState(), _kChunkOpLock);

    auto const configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    const auto coll = [&] {
        auto collDocs = uassertStatusOK(configShard->exhaustiveFindOnConfig(
                                            opCtx,
                                            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                            repl::ReadConcernLevel::kLocalReadConcern,
                                            CollectionType::ConfigNS,
                                            BSON(CollectionType::kNssFieldName << nss.ns()),
                                            {},
                                            1))
                            .docs;
        uassert(ErrorCodes::NamespaceNotFound, "Collection does not exist", !collDocs.empty());

        return CollectionType(collDocs[0].getOwned());
    }();

    if (force) {
        LOGV2(620650,
              "Resetting the 'historyIsAt40' field for all chunks in collection {namespace} in "
              "order to force all chunks' history to get recreated",
              "namespace"_attr = nss.ns());

        BatchedCommandRequest request([&] {
            write_ops::UpdateCommandRequest updateOp(ChunkType::ConfigNS);
            updateOp.setUpdates({[&] {
                write_ops::UpdateOpEntry entry;
                entry.setQ(BSON(ChunkType::collectionUUID() << coll.getUuid()));
                entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(
                    BSON("$unset" << BSON(ChunkType::historyIsAt40() << ""))));
                entry.setUpsert(false);
                entry.setMulti(true);
                return entry;
            }()});
            return updateOp;
        }());
        request.setWriteConcern(ShardingCatalogClient::kLocalWriteConcern.toBSON());

        auto response = configShard->runBatchWriteCommand(
            opCtx, Shard::kDefaultConfigCommandTimeout, request, Shard::RetryPolicy::kIdempotent);
        uassertStatusOK(response.toStatus());

        uassert(ErrorCodes::Error(5760502),
                str::stream() << "No chunks found for collection " << nss.ns(),
                response.getN() > 0);
    }

    // Find the collection version
    const auto collVersion = uassertStatusOK(getCollectionVersion(opCtx, nss));

    // Find the chunk history
    const auto allChunksVector = [&] {
        auto findChunksResponse = uassertStatusOK(
            configShard->exhaustiveFindOnConfig(opCtx,
                                                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                                repl::ReadConcernLevel::kLocalReadConcern,
                                                ChunkType::ConfigNS,
                                                BSON(ChunkType::collectionUUID() << coll.getUuid()),
                                                BSONObj(),
                                                boost::none));
        uassert(ErrorCodes::Error(5760503),
                str::stream() << "No chunks found for collection " << nss.ns(),
                !findChunksResponse.docs.empty());
        return std::move(findChunksResponse.docs);
    }();

    // Bump the major version in order to be guaranteed to trigger refresh on every shard
    ChunkVersion newCollectionVersion({collVersion.epoch(), collVersion.getTimestamp()},
                                      {collVersion.majorVersion() + 1, 0});
    std::set<ShardId> changedShardIds;
    for (const auto& chunk : allChunksVector) {
        auto upgradeChunk = uassertStatusOK(
            ChunkType::parseFromConfigBSON(chunk, collVersion.epoch(), collVersion.getTimestamp()));
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

        upgradeChunk.setVersion(newCollectionVersion);
        newCollectionVersion.incMinor();
        changedShardIds.emplace(upgradeChunk.getShard());

        // Construct the fresh history.
        upgradeChunk.setHistory({ChunkHistory{validAfter, upgradeChunk.getShard()}});

        // Set the 'historyIsAt40' field so that it gets skipped if the command is re-run
        BSONObjBuilder chunkObjBuilder(upgradeChunk.toConfigBSON());
        chunkObjBuilder.appendBool(ChunkType::historyIsAt40(), true);

        // Run the update
        uassertStatusOK(
            catalogClient->updateConfigDocument(opCtx,
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

void ShardingCatalogManager::clearJumboFlag(OperationContext* opCtx,
                                            const NamespaceString& nss,
                                            const OID& collectionEpoch,
                                            const ChunkRange& chunk) {
    // Mark opCtx as interruptible to ensure that all reads and writes to the metadata collections
    // under the exclusive _kChunkOpLock happen on the same term.
    opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

    // Take _kChunkOpLock in exclusive mode to prevent concurrent chunk modifications and generate
    // strictly monotonously increasing collection versions
    Lock::ExclusiveLock lk(opCtx, opCtx->lockState(), _kChunkOpLock);

    auto const configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    auto findCollResponse = uassertStatusOK(
        configShard->exhaustiveFindOnConfig(opCtx,
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

    auto targetChunkResult = uassertStatusOK(
        configShard->exhaustiveFindOnConfig(opCtx,
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
    auto findResponse = uassertStatusOK(
        configShard->exhaustiveFindOnConfig(opCtx,
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
    const auto currentCollectionVersion = highestVersionChunk.getVersion();

    // It is possible for a migration to end up running partly without the protection of the
    // distributed lock if the config primary stepped down since the start of the migration and
    // failed to recover the migration. Check that the collection has not been dropped and recreated
    // or had its shard key refined since the migration began, unbeknown to the shard when the
    // command was sent.
    uassert(ErrorCodes::StaleEpoch,
            str::stream() << "The epoch of collection '" << nss.ns()
                          << "' has changed since the migration began. The config server's "
                             "collection version epoch is now '"
                          << currentCollectionVersion.epoch().toString() << "', but the shard's is "
                          << collectionEpoch.toString() << "'. Aborting clear jumbo on chunk ("
                          << chunk.toString() << ").",
            currentCollectionVersion.epoch() == collectionEpoch);

    ChunkVersion newVersion(
        {currentCollectionVersion.epoch(), currentCollectionVersion.getTimestamp()},
        {currentCollectionVersion.majorVersion() + 1, 0});

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
        Grid::get(opCtx)->catalogClient()->updateConfigDocument(opCtx,
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
    // strictly monotonously increasing collection versions
    Lock::ExclusiveLock lk(opCtx, opCtx->lockState(), _kChunkOpLock);

    ScopeGuard earlyReturnBeforeDoingWriteGuard([&] {
        // Ensure waiting for writeConcern of the data read.
        repl::ReplClientInfo::forClient(opCtx->getClient()).setLastOpToSystemLastOpTime(opCtx);
    });

    const auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

    CollectionType coll;
    {
        auto findCollResponse = uassertStatusOK(configShard->exhaustiveFindOnConfig(
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
            uassertStatusOK(configShard->exhaustiveFindOnConfig(
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

    // Get the chunk with the current collectionVersion for this epoch.
    ChunkType highestChunk;
    {
        const auto query = BSON(ChunkType::collectionUUID() << collUuid);
        const auto highestChunksVector =
            uassertStatusOK(configShard->exhaustiveFindOnConfig(
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
                  "attempting to find the collectionVersion. The collection must have been "
                  "dropped "
                  "concurrently or had its shard key refined. Returning success.",
                  "ensureChunkVersionIsGreaterThan did not find any chunks with a matching epoch "
                  "when "
                  "attempting to find the collectionVersion. The collection must have been "
                  "dropped "
                  "concurrently or had its shard key refined. Returning success.",
                  "epoch"_attr = version.epoch());
            return;
        }
        highestChunk = uassertStatusOK(ChunkType::parseFromConfigBSON(
            highestChunksVector.front(), coll.getEpoch(), coll.getTimestamp()));
    }

    // Generate a new version for the chunk by incrementing the collectionVersion's major
    // version.
    auto newChunk = matchingChunk;
    newChunk.setVersion(ChunkVersion({coll.getEpoch(), coll.getTimestamp()},
                                     {highestChunk.getVersion().majorVersion() + 1, 0}));

    // Update the chunk, if it still exists, to have the bumped version.
    earlyReturnBeforeDoingWriteGuard.dismiss();
    auto didUpdate = uassertStatusOK(
        Grid::get(opCtx)->catalogClient()->updateConfigDocument(opCtx,
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

void ShardingCatalogManager::bumpCollectionVersionAndChangeMetadataInTxn(
    OperationContext* opCtx,
    const NamespaceString& nss,
    unique_function<void(OperationContext*, TxnNumber)> changeMetadataFunc) {
    bumpCollectionVersionAndChangeMetadataInTxn(
        opCtx, nss, std::move(changeMetadataFunc), ShardingCatalogClient::kMajorityWriteConcern);
}

void ShardingCatalogManager::bumpCollectionVersionAndChangeMetadataInTxn(
    OperationContext* opCtx,
    const NamespaceString& nss,
    unique_function<void(OperationContext*, TxnNumber)> changeMetadataFunc,
    const WriteConcernOptions& writeConcern) {
    bumpMultipleCollectionVersionsAndChangeMetadataInTxn(
        opCtx, {nss}, std::move(changeMetadataFunc), writeConcern);
}

void ShardingCatalogManager::bumpMultipleCollectionVersionsAndChangeMetadataInTxn(
    OperationContext* opCtx,
    const std::vector<NamespaceString>& collNames,
    unique_function<void(OperationContext*, TxnNumber)> changeMetadataFunc) {
    bumpMultipleCollectionVersionsAndChangeMetadataInTxn(
        opCtx,
        collNames,
        std::move(changeMetadataFunc),
        ShardingCatalogClient::kMajorityWriteConcern);
}

void ShardingCatalogManager::bumpMultipleCollectionVersionsAndChangeMetadataInTxn(
    OperationContext* opCtx,
    const std::vector<NamespaceString>& collNames,
    unique_function<void(OperationContext*, TxnNumber)> changeMetadataFunc,
    const WriteConcernOptions& writeConcern) {

    // Mark opCtx as interruptible to ensure that all reads and writes to the metadata collections
    // under the exclusive _kChunkOpLock happen on the same term.
    opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

    // Take _kChunkOpLock in exclusive mode to prevent concurrent chunk splits, merges, and
    // migrations
    Lock::ExclusiveLock lk(opCtx, opCtx->lockState(), _kChunkOpLock);

    withTransaction(
        opCtx,
        NamespaceString::kConfigReshardingOperationsNamespace,
        [&collNames, &changeMetadataFunc](OperationContext* opCtx, TxnNumber txnNumber) {
            for (const auto& nss : collNames) {
                bumpCollectionMinorVersion(opCtx, nss, txnNumber);
            }
            changeMetadataFunc(opCtx, txnNumber);
        },
        writeConcern);
}

void ShardingCatalogManager::splitOrMarkJumbo(OperationContext* opCtx,
                                              const NamespaceString& nss,
                                              const BSONObj& minKey) {
    const auto cm = uassertStatusOK(
        Grid::get(opCtx)->catalogCache()->getShardedCollectionRoutingInfoWithRefresh(opCtx, nss));
    auto chunk = cm.findIntersectingChunkWithSimpleCollation(minKey);

    try {
        const auto splitPoints = uassertStatusOK(shardutil::selectChunkSplitPoints(
            opCtx,
            chunk.getShardId(),
            nss,
            cm.getShardKeyPattern(),
            ChunkRange(chunk.getMin(), chunk.getMax()),
            Grid::get(opCtx)->getBalancerConfiguration()->getMaxChunkSizeBytes()));

        if (splitPoints.empty()) {
            LOGV2(21873,
                  "Marking chunk {chunk} as jumbo",
                  "Marking chunk as jumbo",
                  "chunk"_attr = redact(chunk.toString()));
            chunk.markAsJumbo();

            auto const configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();

            // Take _kChunkOpLock in exclusive mode to prevent concurrent chunk modifications. Note
            // that the operation below doesn't increment the chunk marked as jumbo's version, which
            // means that a subsequent incremental refresh will not see it. However, it is being
            // marked in memory through the call to 'markAsJumbo' above so subsequent balancer
            // iterations will not consider it for migration.
            Lock::ExclusiveLock lk(opCtx, opCtx->lockState(), _kChunkOpLock);

            const auto findCollResponse = uassertStatusOK(configShard->exhaustiveFindOnConfig(
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

            auto status = Grid::get(opCtx)->catalogClient()->updateConfigDocument(
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
                      "namespace"_attr = redact(nss.ns()),
                      "minKey"_attr = redact(chunk.getMin()),
                      "error"_attr = redact(status.getStatus()));
            }

            return;
        }

        uassertStatusOK(
            shardutil::splitChunkAtMultiplePoints(opCtx,
                                                  chunk.getShardId(),
                                                  nss,
                                                  cm.getShardKeyPattern(),
                                                  cm.getVersion().epoch(),
                                                  cm.getVersion().getTimestamp(),
                                                  ChunkVersion::IGNORED() /*shardVersion*/,
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
    std::set<ShardId> shardsIds;
    {
        // Mark opCtx as interruptible to ensure that all reads and writes to the metadata
        // collections under the exclusive _kChunkOpLock happen on the same term.
        opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

        // Take _kChunkOpLock in exclusive mode to prevent concurrent chunk splits, merges, and
        // migrations
        Lock::ExclusiveLock lk(opCtx, opCtx->lockState(), _kChunkOpLock);

        const auto cm = uassertStatusOK(
            Grid::get(opCtx)->catalogCache()->getShardedCollectionRoutingInfoWithRefresh(opCtx,
                                                                                         nss));

        uassert(ErrorCodes::InvalidUUID,
                str::stream() << "Collection uuid " << collectionUUID
                              << " in the request does not match the current uuid " << cm.getUUID()
                              << " for ns " << nss,
                !collectionUUID || collectionUUID == cm.getUUID());

        cm.getAllShardIds(&shardsIds);
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

                bumpCollectionMinorVersion(opCtx, nss, txnNumber);
            });

        // From now on migrations are not allowed anymore, so it is not possible that new shards
        // will own chunks for this collection.
    }

    // Trigger a refresh on each shard containing chunks for this collection.
    const auto executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
    sharding_util::tellShardsToRefreshCollection(
        opCtx,
        {std::make_move_iterator(shardsIds.begin()), std::make_move_iterator(shardsIds.end())},
        nss,
        executor);
}

void ShardingCatalogManager::bumpCollectionMinorVersionInTxn(OperationContext* opCtx,
                                                             const NamespaceString& nss,
                                                             TxnNumber txnNumber) const {
    bumpCollectionMinorVersion(opCtx, nss, txnNumber);
}

void ShardingCatalogManager::setChunkEstimatedSize(OperationContext* opCtx,
                                                   const ChunkType& chunk,
                                                   long long estimatedDataSizeBytes,
                                                   const WriteConcernOptions& writeConcern) {
    // ensure the unsigned value fits in the signed long long
    uassert(6049442, "Estimated chunk size cannot be negative", estimatedDataSizeBytes >= 0);

    // Take _kChunkOpLock in exclusive mode to prevent concurrent chunk modifications and generate
    // strictly monotonously increasing collection versions
    Lock::ExclusiveLock lk(opCtx, opCtx->lockState(), _kChunkOpLock);

    const auto chunkQuery = BSON(ChunkType::collectionUUID()
                                 << chunk.getCollectionUUID() << ChunkType::min(chunk.getMin())
                                 << ChunkType::max(chunk.getMax()));
    BSONObjBuilder updateBuilder;
    BSONObjBuilder updateSub(updateBuilder.subobjStart("$set"));
    updateSub.appendNumber(ChunkType::estimatedSizeBytes.name(), estimatedDataSizeBytes);
    updateSub.doneFast();

    auto didUpdate = uassertStatusOK(
        Grid::get(opCtx)->catalogClient()->updateConfigDocument(opCtx,
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
    Lock::ExclusiveLock lk(opCtx, opCtx->lockState(), _kChunkOpLock);

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

    auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
    auto response = configShard->runBatchWriteCommand(
        opCtx, Shard::kDefaultConfigCommandTimeout, request, Shard::RetryPolicy::kIdempotent);

    uassertStatusOK(response.toStatus());
    return response.getN() > 0;
}

void ShardingCatalogManager::_commitChunkMigrationInTransaction(
    OperationContext* opCtx,
    const NamespaceString& nss,
    std::shared_ptr<const ChunkType> migratedChunk,
    std::shared_ptr<const std::vector<ChunkType>> splitChunks,
    std::shared_ptr<ChunkType> controlChunk) {


    auto updateFn = [nss, migratedChunk, splitChunks, controlChunk](
                        const txn_api::TransactionClient& txnClient, ExecutorPtr txnExec) {
        write_ops::UpdateCommandRequest updateOp(ChunkType::ConfigNS);
        std::vector<write_ops::UpdateOpEntry> updateEntries;

        if (controlChunk)
            updateEntries.reserve(splitChunks->size() + 1);  // + migrateChunk
        else
            updateEntries.reserve(splitChunks->size() + 2);  // + migrateChunk + controlChunk


        updateEntries.emplace_back([&] {
            write_ops::UpdateOpEntry entry;
            entry.setUpsert(false);

            auto chunkID = MONGO_unlikely(migrateCommitInvalidChunkQuery.shouldFail())
                ? OID::gen()
                : migratedChunk->getName();

            entry.setQ(BSON(ChunkType::name() << chunkID));

            entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(
                migratedChunk->toConfigBSON()));

            return entry;
        }());

        if (controlChunk) {
            updateEntries.emplace_back([&] {
                write_ops::UpdateOpEntry entry;
                entry.setUpsert(false);

                auto chunkID = MONGO_unlikely(migrateCommitInvalidChunkQuery.shouldFail())
                    ? OID::gen()
                    : controlChunk->getName();

                entry.setQ(BSON(ChunkType::name() << chunkID));

                entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(
                    controlChunk->toConfigBSON()));

                return entry;
            }());
        }


        for (const auto& splitChunk : *splitChunks) {
            updateEntries.emplace_back([&] {
                write_ops::UpdateOpEntry entry;
                entry.setUpsert(true);
                auto chunkID = MONGO_unlikely(migrateCommitInvalidChunkQuery.shouldFail())
                    ? OID::gen()
                    : splitChunk.getName();
                entry.setQ(BSON(ChunkType::name() << chunkID));

                entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(
                    splitChunk.toConfigBSON()));

                return entry;
            }());
        }

        updateOp.setUpdates(updateEntries);

        // Capture expected chunk to update for asserting update succeeded.
        int nChunksToUpdate = controlChunk ? 2 + splitChunks->size() : 1 + splitChunks->size();

        return txnClient.runCRUDOp(updateOp, {})
            .thenRunOn(txnExec)
            .then([nChunksToUpdate](auto updateResponse) {
                uassertStatusOK(updateResponse.toStatus());
                uassert(ErrorCodes::UpdateOperationFailed,
                        str::stream()
                            << "commit chunk migration in transaction failed: N chunks updated "
                            << updateResponse.getN() << " expected " << nChunksToUpdate,
                        updateResponse.getN() == nChunksToUpdate);
                LOGV2_DEBUG(6583601, 1, "commit chunk migration in transaction finished");
            })
            .semi();
    };

    txn_api::SyncTransactionWithRetries txn(
        opCtx, Grid::get(opCtx)->getExecutorPool()->getFixedExecutor(), nullptr);

    txn.run(opCtx, updateFn);
}
}  // namespace mongo
