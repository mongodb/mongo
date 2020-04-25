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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/split_chunk.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/shard_filtering_metadata_refresh.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/split_chunk_request_type.h"

namespace mongo {
namespace {

const ReadPreferenceSetting kPrimaryOnlyReadPreference{ReadPreference::PrimaryOnly};

bool checkIfSingleDoc(OperationContext* opCtx,
                      Collection* collection,
                      const IndexDescriptor* idx,
                      const ChunkType* chunk) {
    KeyPattern kp(idx->keyPattern());
    BSONObj newmin = Helpers::toKeyFormat(kp.extendRangeBound(chunk->getMin(), false));
    BSONObj newmax = Helpers::toKeyFormat(kp.extendRangeBound(chunk->getMax(), true));

    auto exec = InternalPlanner::indexScan(opCtx,
                                           collection,
                                           idx,
                                           newmin,
                                           newmax,
                                           BoundInclusion::kIncludeStartKeyOnly,
                                           PlanExecutor::NO_YIELD);
    // check if exactly one document found
    PlanExecutor::ExecState state;
    BSONObj obj;
    if (PlanExecutor::ADVANCED == (state = exec->getNext(&obj, nullptr))) {
        if (PlanExecutor::IS_EOF == (state = exec->getNext(&obj, nullptr))) {
            return true;
        }
    }

    // Non-yielding collection scans from InternalPlanner will never error.
    invariant(PlanExecutor::ADVANCED == state || PlanExecutor::IS_EOF == state);

    return false;
}

/**
 * Checks the collection's metadata for a successful split on the specified chunkRange using the
 * specified splitKeys. Returns false if the metadata's chunks don't match the new chunk
 * boundaries exactly.
 */
bool checkMetadataForSuccessfulSplitChunk(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          const OID& epoch,
                                          const ChunkRange& chunkRange,
                                          const std::vector<BSONObj>& splitKeys) {
    AutoGetCollection autoColl(opCtx, nss, MODE_IS);
    const auto metadataAfterSplit =
        CollectionShardingRuntime::get(opCtx, nss)->getCurrentMetadataIfKnown();

    uassert(ErrorCodes::StaleEpoch,
            str::stream() << "Collection " << nss.ns() << " changed since split start",
            metadataAfterSplit && metadataAfterSplit->getShardVersion().epoch() == epoch);

    auto newChunkBounds(splitKeys);
    auto startKey = chunkRange.getMin();
    newChunkBounds.push_back(chunkRange.getMax());

    ChunkType nextChunk;
    for (const auto& endKey : newChunkBounds) {
        // Check that all new chunks fit the new chunk boundaries
        if (!metadataAfterSplit->getNextChunk(startKey, &nextChunk) ||
            nextChunk.getMax().woCompare(endKey)) {
            return false;
        }

        startKey = endKey;
    }

    return true;
}

}  // namespace

StatusWith<boost::optional<ChunkRange>> splitChunk(OperationContext* opCtx,
                                                   const NamespaceString& nss,
                                                   const BSONObj& keyPatternObj,
                                                   const ChunkRange& chunkRange,
                                                   const std::vector<BSONObj>& splitKeys,
                                                   const std::string& shardName,
                                                   const OID& expectedCollectionEpoch) {
    const std::string whyMessage(str::stream() << "splitting chunk " << chunkRange.toString()
                                               << " in " << nss.toString());
    auto scopedDistLock = Grid::get(opCtx)->catalogClient()->getDistLockManager()->lock(
        opCtx, nss.ns(), whyMessage, DistLockManager::kDefaultLockTimeout);
    if (!scopedDistLock.isOK()) {
        return scopedDistLock.getStatus().withContext(
            str::stream() << "could not acquire collection lock for " << nss.toString()
                          << " to split chunk " << chunkRange.toString());
    }

    // If the shard key is hashed, then we must make sure that the split points are of supported
    // data types.
    const auto hashedField = ShardKeyPattern::extractHashedField(keyPatternObj);
    if (hashedField) {
        for (BSONObj splitKey : splitKeys) {
            auto hashedSplitElement = splitKey[hashedField.fieldName()];
            if (!ShardKeyPattern::isValidHashedValue(hashedSplitElement)) {
                return {ErrorCodes::CannotSplit,
                        str::stream() << "splitChunk cannot split chunk " << chunkRange.toString()
                                      << ", split point " << hashedSplitElement.toString()
                                      << "Value of type '" << hashedSplitElement.type()
                                      << "' is not allowed for hashed fields"};
            }
        }
    }

    // Commit the split to the config server.
    auto request =
        SplitChunkRequest(nss, shardName, expectedCollectionEpoch, chunkRange, splitKeys);

    auto configCmdObj =
        request.toConfigCommandBSON(ShardingCatalogClient::kMajorityWriteConcern.toBSON());

    auto cmdResponseStatus =
        Grid::get(opCtx)->shardRegistry()->getConfigShard()->runCommandWithFixedRetryAttempts(
            opCtx,
            kPrimaryOnlyReadPreference,
            "admin",
            configCmdObj,
            Shard::RetryPolicy::kIdempotent);

    // If we failed to get any response from the config server at all, despite retries, then we
    // should just go ahead and fail the whole operation.
    if (!cmdResponseStatus.isOK()) {
        return cmdResponseStatus.getStatus();
    }

    // Check commandStatus and writeConcernStatus
    auto commandStatus = cmdResponseStatus.getValue().commandStatus;
    auto writeConcernStatus = cmdResponseStatus.getValue().writeConcernStatus;

    // Send stale epoch if epoch of request did not match epoch of collection
    if (commandStatus == ErrorCodes::StaleEpoch) {
        return commandStatus;
    }

    //
    // If _configsvrCommitChunkSplit returned an error, refresh and look at the metadata to
    // determine if the split actually did happen. This can happen if there's a network error
    // getting the response from the first call to _configsvrCommitChunkSplit, but it actually
    // succeeds, thus the automatic retry fails with a precondition violation, for example.
    //
    if (!commandStatus.isOK() || !writeConcernStatus.isOK()) {
        forceShardFilteringMetadataRefresh(opCtx, nss);

        if (checkMetadataForSuccessfulSplitChunk(
                opCtx, nss, expectedCollectionEpoch, chunkRange, splitKeys)) {
            // Split was committed.
        } else if (!commandStatus.isOK()) {
            return commandStatus;
        } else if (!writeConcernStatus.isOK()) {
            return writeConcernStatus;
        }
    }

    AutoGetCollection autoColl(opCtx, nss, MODE_IS);

    Collection* const collection = autoColl.getCollection();
    if (!collection) {
        LOGV2_WARNING(
            23778,
            "will not perform top-chunk checking since {nss} does not exist after splitting",
            "nss"_attr = nss.toString());
        return boost::optional<ChunkRange>(boost::none);
    }

    // Allow multiKey based on the invariant that shard keys must be single-valued. Therefore,
    // any multi-key index prefixed by shard key cannot be multikey over the shard key fields.
    const IndexDescriptor* idx =
        collection->getIndexCatalog()->findShardKeyPrefixedIndex(opCtx, keyPatternObj, false);
    if (!idx) {
        return boost::optional<ChunkRange>(boost::none);
    }

    auto backChunk = ChunkType();
    backChunk.setMin(splitKeys.back());
    backChunk.setMax(chunkRange.getMax());

    auto frontChunk = ChunkType();
    frontChunk.setMin(chunkRange.getMin());
    frontChunk.setMax(splitKeys.front());

    KeyPattern shardKeyPattern(keyPatternObj);
    if (shardKeyPattern.globalMax().woCompare(backChunk.getMax()) == 0 &&
        checkIfSingleDoc(opCtx, collection, idx, &backChunk)) {
        return boost::optional<ChunkRange>(ChunkRange(backChunk.getMin(), backChunk.getMax()));
    } else if (shardKeyPattern.globalMin().woCompare(frontChunk.getMin()) == 0 &&
               checkIfSingleDoc(opCtx, collection, idx, &frontChunk)) {
        return boost::optional<ChunkRange>(ChunkRange(frontChunk.getMin(), frontChunk.getMax()));
    }

    return boost::optional<ChunkRange>(boost::none);
}

}  // namespace mongo
