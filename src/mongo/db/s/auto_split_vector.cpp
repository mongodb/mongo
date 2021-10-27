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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/auto_split_vector.h"

#include "mongo/base/status_with.h"
#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

constexpr int estimatedAdditionalBytesPerItemInBSONArray{2};

constexpr int kMaxSplitPointsToReposition{3};

BSONObj prettyKey(const BSONObj& keyPattern, const BSONObj& key) {
    return key.replaceFieldNames(keyPattern).clientReadable();
}

/*
 * Takes the given min/max BSON objects that are a prefix of the shardKey and return two new BSON
 * object extended to cover the entire shardKey. See KeyPattern::extendRangeBound documentation for
 * some examples.
 */
const std::tuple<BSONObj, BSONObj> getMinMaxExtendedBounds(const IndexDescriptor* shardKeyIdx,
                                                           const BSONObj& min,
                                                           const BSONObj& max) {
    KeyPattern kp(shardKeyIdx->keyPattern());

    // Extend min to get (min, MinKey, MinKey, ....)
    BSONObj minKey = Helpers::toKeyFormat(kp.extendRangeBound(min, false /* upperInclusive */));
    BSONObj maxKey;
    if (max.isEmpty()) {
        // if max not specified, make it (MaxKey, Maxkey, MaxKey...)
        maxKey = Helpers::toKeyFormat(kp.extendRangeBound(max, true /* upperInclusive */));
    } else {
        // otherwise make it (max,MinKey,MinKey...) so that bound is non-inclusive
        maxKey = Helpers::toKeyFormat(kp.extendRangeBound(max, false /* upperInclusive*/));
    }

    return std::tuple<BSONObj, BSONObj>(minKey, maxKey);
}

/*
 * Returns true if the final key in the range is the same as the first key, false otherwise.
 */
bool maxKeyEqualToMinKey(OperationContext* opCtx,
                         const Collection* collection,
                         const IndexDescriptor* shardKeyIdx,
                         const BSONObj& minBound,
                         const BSONObj& maxBound,
                         const BSONObj& minKeyInChunk) {
    BSONObj maxKeyInChunk;
    {
        auto backwardIdxScanner = InternalPlanner::indexScan(opCtx,
                                                             collection,
                                                             shardKeyIdx,
                                                             maxBound,
                                                             minBound,
                                                             BoundInclusion::kIncludeEndKeyOnly,
                                                             PlanExecutor::YIELD_AUTO,
                                                             InternalPlanner::BACKWARD);

        PlanExecutor::ExecState state = backwardIdxScanner->getNext(&maxKeyInChunk, nullptr);
        uassert(ErrorCodes::OperationFailed,
                "can't open a cursor to find final key in range (desired range is possibly empty)",
                state == PlanExecutor::ADVANCED);
    }

    if (minKeyInChunk.woCompare(maxKeyInChunk) == 0) {
        // Range contains only documents with a single key value.  So we cannot possibly find a
        // split point, and there is no need to scan any further.
        warning() << "possible low cardinality key detected in " << collection->ns().toString()
                  << " - range " << redact(prettyKey(shardKeyIdx->keyPattern(), minKeyInChunk))
                  << " -->> " << redact(prettyKey(shardKeyIdx->keyPattern(), maxKeyInChunk))
                  << " contains only the key "
                  << redact(prettyKey(shardKeyIdx->keyPattern(), minKeyInChunk));
        return true;
    }

    return false;
}

/*
 * Reshuffle fields according to the shard key pattern.
 */
auto orderShardKeyFields(const BSONObj& keyPattern, BSONObj& key) {
    return dotted_path_support::extractElementsBasedOnTemplate(
        prettyKey(keyPattern, key.getOwned()), keyPattern);
}

}  // namespace

std::vector<BSONObj> autoSplitVector(OperationContext* opCtx,
                                     const NamespaceString& nss,
                                     const BSONObj& keyPattern,
                                     const BSONObj& min,
                                     const BSONObj& max,
                                     long long maxChunkSizeBytes) {
    std::vector<BSONObj> splitKeys;

    int elapsedMillisToFindSplitPoints;

    // Contains each key appearing multiple times and estimated to be able to fill-in a chunk alone
    auto tooFrequentKeys = SimpleBSONObjComparator::kInstance.makeBSONObjSet();

    {
        AutoGetCollection autoGetColl(opCtx, nss, MODE_IS);
        auto* collection = autoGetColl.getCollection();

        uassert(ErrorCodes::NamespaceNotFound, "ns not found", collection);

        // Get the size estimate for this namespace
        const long long totalLocalCollDocuments = collection->numRecords(opCtx);
        const long long dataSize = collection->dataSize(opCtx);

        // Return empty vector if current estimated data size is less than max chunk size
        if (dataSize < maxChunkSizeBytes || totalLocalCollDocuments == 0) {
            return {};
        }

        // Allow multiKey based on the invariant that shard keys must be single-valued. Therefore,
        // any multi-key index prefixed by shard key cannot be multikey over the shard key fields.
        auto catalog = collection->getIndexCatalog();
        auto shardKeyIdx =
            catalog->findShardKeyPrefixedIndex(opCtx, keyPattern, false /* requireSingleKey */);
        uassert(ErrorCodes::IndexNotFound,
                str::stream() << "couldn't find index over splitting key "
                              << keyPattern.clientReadable().toString(),
                shardKeyIdx);

        const auto minKeyAndMaxKey = getMinMaxExtendedBounds(shardKeyIdx, min, max);
        const auto minKey = std::get<0>(minKeyAndMaxKey);
        const auto maxKey = std::get<1>(minKeyAndMaxKey);

        // Setup the index scanner that will be used to find the split points
        auto forwardIdxScanner = InternalPlanner::indexScan(opCtx,
                                                            &(*collection),
                                                            shardKeyIdx,
                                                            minKey,
                                                            maxKey,
                                                            BoundInclusion::kIncludeStartKeyOnly,
                                                            PlanExecutor::YIELD_AUTO,
                                                            InternalPlanner::FORWARD);

        // Get minimum key belonging to the chunk
        BSONObj minKeyInOriginalChunk;
        {
            PlanExecutor::ExecState state =
                forwardIdxScanner->getNext(&minKeyInOriginalChunk, nullptr);
            uassert(ErrorCodes::OperationFailed,
                    "can't open a cursor to scan the range (desired range is possibly empty)",
                    state == PlanExecutor::ADVANCED);
        }

        // Return empty vector if chunk's min and max keys are the same.
        if (maxKeyEqualToMinKey(
                opCtx, collection, shardKeyIdx, minKey, maxKey, minKeyInOriginalChunk)) {
            return {};
        }

        log() << "request split points lookup for chunk " << nss.toString() << " " << redact(minKey)
              << " -->> " << redact(maxKey);

        // Use the average document size and number of documents to find the approximate number of
        // keys each chunk should contain
        const long long avgDocSize = dataSize / totalLocalCollDocuments;

        // Split at max chunk size
        long long maxDocsPerChunk = maxChunkSizeBytes / avgDocSize;

        BSONObj currentKey;               // Last key seen during the index scan
        long long numScannedKeys = 1;     // minKeyInOriginalChunk has already been scanned
        std::size_t resultArraySize = 0;  // Approximate size in bytes of the split points array
        bool reachedMaxBSONSize = false;  // True if the split points vector becomes too big

        // Lambda to check whether the split points vector would exceed BSONObjMaxUserSize in case
        // of additional split key of the specified size.
        auto checkMaxBSONSize = [&resultArraySize](const int additionalKeySize) {
            return resultArraySize + additionalKeySize > BSONObjMaxUserSize;
        };

        // Reference to last split point that needs to be checked in order to avoid adding duplicate
        // split points. Initialized to the min of the first chunk being split.
        auto minKeyElement = orderShardKeyFields(keyPattern, minKeyInOriginalChunk);
        auto lastSplitPoint = minKeyElement;

        Timer timer;  // To measure time elapsed while searching split points

        // Traverse the index and add the maxDocsPerChunk-th key to the result vector
        while (forwardIdxScanner->getNext(&currentKey, nullptr) == PlanExecutor::ADVANCED) {
            if (++numScannedKeys >= maxDocsPerChunk) {
                currentKey = orderShardKeyFields(keyPattern, currentKey);

                if (currentKey.woCompare(lastSplitPoint) == 0) {
                    // Do not add again the same split point in case of frequent shard key.
                    tooFrequentKeys.insert(currentKey.getOwned());
                    continue;
                }

                const auto additionalKeySize =
                    currentKey.objsize() + estimatedAdditionalBytesPerItemInBSONArray;
                if (checkMaxBSONSize(additionalKeySize)) {
                    if (splitKeys.empty()) {
                        // Keep trying until finding at least one split point that isn't above
                        // the max object user size. Very improbable corner case: the shard key
                        // size for the chosen split point is exactly 16MB.
                        continue;
                    }
                    reachedMaxBSONSize = true;
                    break;
                }

                resultArraySize += additionalKeySize;
                splitKeys.push_back(currentKey.getOwned());
                lastSplitPoint = splitKeys.back();
                numScannedKeys = 0;

                LOG(4) << "picked a split key: " << redact(currentKey);
            }
        }

        // Avoid creating small chunks by fairly recalculating the last split points if the last
        // chunk would be too small (containing less than `80% maxDocsPerChunk` documents).
        bool lastChunk80PercentFull = numScannedKeys >= maxDocsPerChunk * 0.8;
        if (!lastChunk80PercentFull && !splitKeys.empty() && !reachedMaxBSONSize) {
            // Eventually recalculate the last split points (at most `kMaxSplitPointsToReposition`).
            int nSplitPointsToReposition = splitKeys.size() > kMaxSplitPointsToReposition
                ? kMaxSplitPointsToReposition
                : splitKeys.size();

            // Equivalent to: (nSplitPointsToReposition * maxDocsPerChunk + numScannedKeys) divided
            // by the number of reshuffled chunks (nSplitPointsToReposition + 1).
            const auto maxDocsPerNewChunk = maxDocsPerChunk -
                ((maxDocsPerChunk - numScannedKeys) / (nSplitPointsToReposition + 1));

            if (numScannedKeys < maxDocsPerChunk - maxDocsPerNewChunk) {
                // If the surplus is not too much, simply keep a bigger last chunk.
                // The surplus is considered enough if repositioning the split points would imply
                // generating chunks with a number of documents lower than `67% maxDocsPerChunk`.
                splitKeys.pop_back();
            } else {
                // Fairly recalculate the last `nSplitPointsToReposition` split points.
                splitKeys.erase(splitKeys.end() - nSplitPointsToReposition, splitKeys.end());

                auto forwardIdxScanner =
                    InternalPlanner::indexScan(opCtx,
                                               collection,
                                               shardKeyIdx,
                                               splitKeys.empty() ? minKeyElement : splitKeys.back(),
                                               maxKey,
                                               BoundInclusion::kIncludeStartKeyOnly,
                                               PlanExecutor::YIELD_AUTO,
                                               InternalPlanner::FORWARD);

                numScannedKeys = 0;

                auto previousSplitPoint = splitKeys.empty() ? minKeyElement : splitKeys.back();
                while (forwardIdxScanner->getNext(&currentKey, nullptr) == PlanExecutor::ADVANCED) {
                    if (++numScannedKeys >= maxDocsPerNewChunk) {
                        currentKey = orderShardKeyFields(keyPattern, currentKey);

                        const auto compareWithPreviousSplitPoint =
                            currentKey.woCompare(previousSplitPoint);
                        if (compareWithPreviousSplitPoint > 0) {
                            const auto additionalKeySize =
                                currentKey.objsize() + estimatedAdditionalBytesPerItemInBSONArray;
                            if (checkMaxBSONSize(additionalKeySize)) {
                                reachedMaxBSONSize = true;
                                break;
                            }

                            splitKeys.push_back(currentKey.getOwned());
                            previousSplitPoint = splitKeys.back();
                            numScannedKeys = 0;

                            if (--nSplitPointsToReposition == 0) {
                                break;
                            }
                        } else if (compareWithPreviousSplitPoint == 0) {
                            // Don't add again the same split point in case of frequent shard key.
                            tooFrequentKeys.insert(currentKey.getOwned());
                        }
                    }
                }
            }
        }

        elapsedMillisToFindSplitPoints = timer.millis();

        if (reachedMaxBSONSize) {
            log() << "Max BSON response size reached for split vector before the end of chunk for "
                     "namespace "
                  << nss.toString() << " - range "
                  << redact(prettyKey(shardKeyIdx->keyPattern(), minKey)) << " --> "
                  << redact(prettyKey(shardKeyIdx->keyPattern(), maxKey));
        }
    }

    // Emit a warning for each frequent key
    for (const auto& frequentKey : tooFrequentKeys) {
        warning() << "possible low cardinality key detected in " << nss.toString()
                  << " - key: " << redact(prettyKey(keyPattern, frequentKey));
    }

    if (elapsedMillisToFindSplitPoints > serverGlobalParams.slowMS) {
        warning() << "Finding the auto split vector for " << nss.toString() << " completed over "
                  << redact(keyPattern) << " - numSplits: " << splitKeys.size()
                  << " - duration: " << elapsedMillisToFindSplitPoints << "ms";
    }

    std::sort(
        splitKeys.begin(), splitKeys.end(), SimpleBSONObjComparator::kInstance.makeLessThan());

    return splitKeys;
}

}  // namespace mongo
