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

#include "mongo/db/s/auto_split_vector.h"

#include "mongo/base/status_with.h"
#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

/*
 * BSON arrays are serialized as BSON objects with the index of each element as a string key: for
 * example, the array ["a","b","c"] is going to be serialized as {"0":"a","1":"b","2":"c"}. The
 * minimum size for a BSON object is `BSONObj::kMinBSONLength`.
 *
 * Given that the `vector<BSONObj>` returned by `autoSplitVector` can't be greater than 16MB when
 * serialized, pessimistically assume that each key occupies the highest possible number of bytes.
 */
const int estimatedAdditionalBytesPerItemInBSONArray{
    (int)std::to_string(BSONObjMaxUserSize / BSONObj::kMinBSONLength).length()};

constexpr int kMaxSplitPointsToReposition{3};

BSONObj prettyKey(const BSONObj& keyPattern, const BSONObj& key) {
    return key.replaceFieldNames(keyPattern).clientReadable();
}

/*
 * Takes the given min/max BSON objects that are a prefix of the shardKey and return two new BSON
 * object extended to cover the entire shardKey. See KeyPattern::extendRangeBound documentation for
 * some examples.
 */
std::tuple<BSONObj, BSONObj> getMinMaxExtendedBounds(const ShardKeyIndex& shardKeyIdx,
                                                     const BSONObj& min,
                                                     const BSONObj& max) {
    KeyPattern kp(shardKeyIdx.keyPattern());

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

    return {minKey, maxKey};
}

/*
 * Reshuffle fields according to the shard key pattern.
 */
auto orderShardKeyFields(const BSONObj& keyPattern, const BSONObj& key) {
    // Note: It is correct to hydrate the indexKey 'key' with 'keyPattern', because the index key
    // pattern is a prefix of 'keyPattern'.
    return dotted_path_support::extractElementsBasedOnTemplate(key.replaceFieldNames(keyPattern),
                                                               keyPattern);
}

}  // namespace

std::pair<std::vector<BSONObj>, bool> autoSplitVector(OperationContext* opCtx,
                                                      const NamespaceString& nss,
                                                      const BSONObj& keyPattern,
                                                      const BSONObj& min,
                                                      const BSONObj& max,
                                                      long long maxChunkSizeBytes,
                                                      boost::optional<int> limit,
                                                      bool forward) {
    if (limit) {
        uassert(ErrorCodes::InvalidOptions, "autoSplitVector expects a positive limit", *limit > 0);
    }

    std::vector<BSONObj> splitKeys;
    bool reachedMaxBSONSize = false;  // True if the split points vector becomes too big

    int elapsedMillisToFindSplitPoints;

    // Contains each key appearing multiple times and estimated to be able to fill-in a chunk alone
    auto tooFrequentKeys = SimpleBSONObjComparator::kInstance.makeBSONObjSet();

    {
        AutoGetCollection collection(opCtx, nss, MODE_IS);

        uassert(ErrorCodes::NamespaceNotFound,
                str::stream() << "namespace " << nss.toStringForErrorMsg() << " does not exists",
                collection);

        // Get the size estimate for this namespace
        const long long totalLocalCollDocuments = collection->numRecords(opCtx);
        const long long dataSize = collection->dataSize(opCtx);

        // Return empty vector if current estimated data size is less than max chunk size
        if (dataSize < maxChunkSizeBytes || totalLocalCollDocuments == 0) {
            return {};
        }

        // Allow multiKey based on the invariant that shard keys must be single-valued. Therefore,
        // any multi-key index prefixed by shard key cannot be multikey over the shard key fields.
        const auto shardKeyIdx = findShardKeyPrefixedIndex(opCtx,
                                                           *collection,
                                                           keyPattern,
                                                           /*requireSingleKey=*/false);
        uassert(ErrorCodes::IndexNotFound,
                str::stream() << "couldn't find index over splitting key "
                              << keyPattern.clientReadable().toString(),
                shardKeyIdx);

        const auto [minKey, maxKey] = getMinMaxExtendedBounds(*shardKeyIdx, min, max);

        auto getIdxScanner = [&](const BSONObj& minKey,
                                 const BSONObj& maxKey,
                                 BoundInclusion inclusion,
                                 InternalPlanner::Direction direction) {
            return InternalPlanner::shardKeyIndexScan(opCtx,
                                                      &(*collection),
                                                      *shardKeyIdx,
                                                      minKey,
                                                      maxKey,
                                                      inclusion,
                                                      PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                                      direction);
        };

        // Setup the index scanner that will be used to find the split points
        auto idxScanner = forward
            ? getIdxScanner(
                  minKey, maxKey, BoundInclusion::kIncludeStartKeyOnly, InternalPlanner::FORWARD)
            : getIdxScanner(
                  maxKey, minKey, BoundInclusion::kIncludeEndKeyOnly, InternalPlanner::BACKWARD);
        // Get first key belonging to the chunk
        BSONObj firstKeyInOriginalChunk;
        {
            PlanExecutor::ExecState state = idxScanner->getNext(&firstKeyInOriginalChunk, nullptr);
            if (state == PlanExecutor::IS_EOF) {
                // Range is empty
                return {};
            }
        }

        BSONObj lastKeyInChunk;
        {
            auto rangeEndIdxScanner = forward
                ? getIdxScanner(
                      maxKey, minKey, BoundInclusion::kIncludeEndKeyOnly, InternalPlanner::BACKWARD)
                : getIdxScanner(minKey,
                                maxKey,
                                BoundInclusion::kIncludeStartKeyOnly,
                                InternalPlanner::FORWARD);

            PlanExecutor::ExecState state = rangeEndIdxScanner->getNext(&lastKeyInChunk, nullptr);
            if (state == PlanExecutor::IS_EOF) {
                // Range is empty
                return {};
            }
        }

        if (firstKeyInOriginalChunk.woCompare(lastKeyInChunk) == 0) {
            // Range contains only documents with a single key value.  So we cannot possibly find a
            // split point, and there is no need to scan any further.
            LOGV2_WARNING(
                5865001,
                "Possible low cardinality key detected in range. Range contains only a single key.",
                logAttrs(collection.getNss()),
                "minKey"_attr = redact(prettyKey(keyPattern, minKey)),
                "maxKey"_attr = redact(prettyKey(keyPattern, maxKey)),
                "key"_attr = redact(prettyKey(shardKeyIdx->keyPattern(), firstKeyInOriginalChunk)));
            return {};
        }

        LOGV2(6492600,
              "Requested split points lookup for range",
              logAttrs(nss),
              "minKey"_attr = redact(prettyKey(keyPattern, minKey)),
              "maxKey"_attr = redact(prettyKey(keyPattern, maxKey)),
              "direction"_attr = forward ? "forwards" : "backwards");

        // Use the average document size and number of documents to find the approximate number of
        // keys each chunk should contain
        const long long avgDocSize = dataSize / totalLocalCollDocuments;

        // Split at max chunk size
        long long maxDocsPerChunk = maxChunkSizeBytes / avgDocSize;

        BSONObj currentKey;               // Last key seen during the index scan
        long long numScannedKeys = 1;     // firstKeyInOriginalChunk has already been scanned
        std::size_t resultArraySize = 0;  // Approximate size in bytes of the split points array

        // Lambda to check whether the split points vector would exceed BSONObjMaxUserSize in case
        // of additional split key of the specified size.
        auto checkMaxBSONSize = [&resultArraySize](const int additionalKeySize) {
            return resultArraySize + additionalKeySize > BSONObjMaxUserSize;
        };

        // Reference to last split point that needs to be checked in order to avoid adding duplicate
        // split points. Initialized to the min of the first chunk being split.
        auto firstKeyElement = orderShardKeyFields(keyPattern, firstKeyInOriginalChunk);
        auto lastSplitPoint = firstKeyElement;

        Timer timer;  // To measure time elapsed while searching split points

        // Traverse the index and add the maxDocsPerChunk-th key to the result vector
        while (idxScanner->getNext(&currentKey, nullptr) == PlanExecutor::ADVANCED) {
            if (++numScannedKeys >= maxDocsPerChunk) {
                currentKey = orderShardKeyFields(keyPattern, currentKey);

                const auto compareWithPreviousSplitPoint = currentKey.woCompare(lastSplitPoint);
                if (forward) {
                    dassert(compareWithPreviousSplitPoint >= 0,
                            str::stream()
                                << "Found split key smaller than the last one in forwards lookup: "
                                << currentKey << " < " << lastSplitPoint);
                } else {
                    dassert(compareWithPreviousSplitPoint <= 0,
                            str::stream()
                                << "Found split key larger than the last one in backwards lookup: "
                                << currentKey << " > " << lastSplitPoint);
                }
                if (compareWithPreviousSplitPoint == 0) {
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

                if (limit && splitKeys.size() == static_cast<size_t>(*limit) + 1) {
                    // If the user has specified a limit, calculate the first `limit + 1` split
                    // points (avoid creating small chunks)
                    break;
                }

                LOGV2_DEBUG(5865003, 4, "Picked a split key", "key"_attr = redact(currentKey));
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

                auto idxScanner = forward
                    ? getIdxScanner(splitKeys.empty() ? firstKeyElement : splitKeys.back(),
                                    maxKey,
                                    BoundInclusion::kIncludeStartKeyOnly,
                                    InternalPlanner::FORWARD)
                    : getIdxScanner(splitKeys.empty() ? firstKeyElement : splitKeys.back(),
                                    minKey,
                                    BoundInclusion::kIncludeBothStartAndEndKeys,
                                    InternalPlanner::BACKWARD);

                numScannedKeys = 0;

                auto previousSplitPoint = splitKeys.empty() ? firstKeyElement : splitKeys.back();
                while (idxScanner->getNext(&currentKey, nullptr) == PlanExecutor::ADVANCED) {
                    if (++numScannedKeys >= maxDocsPerNewChunk) {
                        currentKey = orderShardKeyFields(keyPattern, currentKey);

                        const auto compareWithPreviousSplitPoint =
                            currentKey.woCompare(previousSplitPoint);

                        if (forward) {
                            dassert(compareWithPreviousSplitPoint >= 0,
                                    str::stream() << "Found split key smaller than the last one in "
                                                     "forwards lookup: "
                                                  << currentKey << " < " << previousSplitPoint);
                        } else {
                            dassert(compareWithPreviousSplitPoint <= 0,
                                    str::stream() << "Found split key larger than the previous one "
                                                     "in backwards lookup: "
                                                  << currentKey << " > " << previousSplitPoint);
                        }
                        if ((forward && compareWithPreviousSplitPoint > 0) ||
                            (!forward && compareWithPreviousSplitPoint < 0)) {
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
            LOGV2(5865002,
                  "Max BSON response size reached for split vector before the end of chunk",
                  logAttrs(nss),
                  "minKey"_attr = redact(prettyKey(shardKeyIdx->keyPattern(), minKey)),
                  "maxKey"_attr = redact(prettyKey(shardKeyIdx->keyPattern(), maxKey)));
        }
    }

    // Emit a warning for each frequent key
    for (const auto& frequentKey : tooFrequentKeys) {
        LOGV2_WARNING(5865004,
                      "Possible low cardinality key detected",
                      logAttrs(nss),
                      "key"_attr = redact(prettyKey(keyPattern, frequentKey)));
    }

    if (elapsedMillisToFindSplitPoints > serverGlobalParams.slowMS.load()) {
        LOGV2_WARNING(5865005,
                      "Finding the auto split vector completed",
                      logAttrs(nss),
                      "keyPattern"_attr = redact(keyPattern),
                      "numSplits"_attr = splitKeys.size(),
                      "duration"_attr = Milliseconds(elapsedMillisToFindSplitPoints));
    }

    if (limit && splitKeys.size() > static_cast<size_t>(*limit)) {
        splitKeys.resize(*limit);
    }

    return std::make_pair(std::move(splitKeys), reachedMaxBSONSize);
}

}  // namespace mongo
