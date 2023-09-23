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

#include "mongo/db/s/split_vector.h"

#include <boost/move/utility_core.hpp>
#include <cstddef>
#include <memory>
#include <set>

#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/index_bounds.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/s/shard_key_index_util.h"
#include "mongo/db/server_options.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/str.h"
#include "mongo/util/timer.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

const int kMaxObjectPerChunk{250000};
const int kEstimatedAdditionalBytesPerItemInBSONArray{2};

BSONObj prettyKey(const BSONObj& keyPattern, const BSONObj& key) {
    return key.replaceFieldNames(keyPattern).clientReadable();
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

std::vector<BSONObj> splitVector(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 const BSONObj& keyPattern,
                                 const BSONObj& min,
                                 const BSONObj& max,
                                 bool force,
                                 boost::optional<long long> maxSplitPoints,
                                 boost::optional<long long> maxChunkObjects,
                                 boost::optional<long long> maxChunkSizeBytes) {
    std::vector<BSONObj> splitKeys;
    std::size_t splitVectorResponseSize = 0;

    // Always have a default value for maxChunkObjects
    if (!maxChunkObjects) {
        maxChunkObjects = kMaxObjectPerChunk;
    }

    {
        AutoGetCollection collection(opCtx, nss, MODE_IS);
        uassert(ErrorCodes::NamespaceNotFound, "ns not found", collection);

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

        // extend min to get (min, MinKey, MinKey, ....)
        KeyPattern kp(shardKeyIdx->keyPattern());
        BSONObj minKey = Helpers::toKeyFormat(kp.extendRangeBound(min, false));
        BSONObj maxKey;
        if (max.isEmpty()) {
            // if max not specified, make it (MaxKey, Maxkey, MaxKey...)
            maxKey = Helpers::toKeyFormat(kp.extendRangeBound(max, true));
        } else {
            // otherwise make it (max,MinKey,MinKey...) so that bound is non-inclusive
            maxKey = Helpers::toKeyFormat(kp.extendRangeBound(max, false));
        }

        // Get the size estimate for this namespace
        const long long recCount = collection->numRecords(opCtx);
        const long long dataSize = collection->dataSize(opCtx);

        // Now that we have the size estimate, go over the remaining parameters and apply any
        // maximum size restrictions specified there.

        // Forcing a split is equivalent to having maxChunkSizeBytes be the size of the current
        // chunk, i.e., the logic below will split that chunk in half

        if (force) {
            maxChunkSizeBytes = dataSize;
        }

        // If the collection is empty, cannot use split with find or bounds option.
        if (!maxChunkSizeBytes || maxChunkSizeBytes.value() <= 0) {
            uasserted(ErrorCodes::InvalidOptions,
                      "cannot use split with find or bounds option on an empty collection");
        }

        // If there's not enough data for more than one chunk, no point continuing.
        if (dataSize < maxChunkSizeBytes.value() || recCount == 0) {
            std::vector<BSONObj> emptyVector;
            return emptyVector;
        }

        LOGV2(22107,
              "Requested split points lookup for chunk {namespace} {minKey} -->> {maxKey}",
              "Requested split points lookup for chunk",
              logAttrs(nss),
              "minKey"_attr = redact(prettyKey(keyPattern, minKey)),
              "maxKey"_attr = redact(prettyKey(keyPattern, maxKey)));

        // We'll use the average object size and number of object to find approximately how many
        // keys each chunk should have. We'll split at half the maxChunkSizeBytes or
        // maxChunkObjects, if provided.
        const long long avgRecSize = dataSize / recCount;

        long long keyCount = maxChunkSizeBytes.value() / (2 * avgRecSize);

        if (maxChunkObjects.value() && (maxChunkObjects.value() < keyCount)) {
            LOGV2(22108,
                  "Limiting the number of documents per chunk to {maxChunkObjects} based "
                  "on the maxChunkObjects parameter for split vector command (compared to maximum "
                  "possible: {maxPossibleDocumentsPerChunk})",
                  "Limiting the number of documents per chunk for split vector command based on "
                  "the maxChunksObject parameter",
                  "maxChunkObjects"_attr = maxChunkObjects.value(),
                  "maxPossibleDocumentsPerChunk"_attr = keyCount);
            keyCount = maxChunkObjects.value();
        }

        //
        // Traverse the index and add the keyCount-th key to the result vector. If that key
        // appeared in the vector before, we omit it. The invariant here is that all the
        // instances of a given key value live in the same chunk.
        //

        Timer timer;
        long long currCount = 0;
        long long numChunks = 0;

        auto exec = InternalPlanner::shardKeyIndexScan(opCtx,
                                                       &collection.getCollection(),
                                                       *shardKeyIdx,
                                                       minKey,
                                                       maxKey,
                                                       BoundInclusion::kIncludeStartKeyOnly,
                                                       PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                                       InternalPlanner::FORWARD);

        BSONObj currKey;
        PlanExecutor::ExecState state = exec->getNext(&currKey, nullptr);
        uassert(ErrorCodes::OperationFailed,
                "can't open a cursor to scan the range (desired range is possibly empty)",
                state == PlanExecutor::ADVANCED);

        // Get the final key in the range, and see if it's the same as the first key.
        BSONObj maxKeyInChunk;
        {
            auto exec = InternalPlanner::shardKeyIndexScan(opCtx,
                                                           &collection.getCollection(),
                                                           *shardKeyIdx,
                                                           maxKey,
                                                           minKey,
                                                           BoundInclusion::kIncludeEndKeyOnly,
                                                           PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                                           InternalPlanner::BACKWARD);

            PlanExecutor::ExecState state = exec->getNext(&maxKeyInChunk, nullptr);
            uassert(
                ErrorCodes::OperationFailed,
                "can't open a cursor to find final key in range (desired range is possibly empty)",
                state == PlanExecutor::ADVANCED);
        }

        if (currKey.woCompare(maxKeyInChunk) == 0) {
            // Range contains only documents with a single key value.  So we cannot possibly find a
            // split point, and there is no need to scan any further.
            LOGV2_WARNING(
                22113,
                "Possible low cardinality key detected in {namespace} - range {minKey} -->> "
                "{maxKey} contains only the key {key}",
                "Possible low cardinality key detected in range. Range contains only a single key.",
                logAttrs(nss),
                "minKey"_attr = redact(prettyKey(shardKeyIdx->keyPattern(), minKey)),
                "maxKey"_attr = redact(prettyKey(shardKeyIdx->keyPattern(), maxKey)),
                "key"_attr = redact(prettyKey(shardKeyIdx->keyPattern(), currKey)));
            std::vector<BSONObj> emptyVector;
            return emptyVector;
        }

        // Use every 'keyCount'-th key as a split point. We add the initial key as a sentinel,
        // to be removed at the end. If a key appears more times than entries allowed on a
        // chunk, we issue a warning and split on the following key.
        auto tooFrequentKeys = SimpleBSONObjComparator::kInstance.makeBSONObjSet();
        splitKeys.push_back(orderShardKeyFields(keyPattern, currKey.getOwned()));

        while (1) {
            while (PlanExecutor::ADVANCED == state) {
                currCount++;

                if (currCount > keyCount && !force) {
                    currKey = orderShardKeyFields(keyPattern, currKey.getOwned());

                    const auto compareWithPreviousSplitPoint = currKey.woCompare(splitKeys.back());
                    dassert(compareWithPreviousSplitPoint >= 0,
                            str::stream() << "Found split key smaller then the previous one: "
                                          << currKey << " < " << splitKeys.back());
                    if (currKey.woCompare(splitKeys.back()) == 0) {
                        // Do not use this split key if it is the same of the previous split point.
                        tooFrequentKeys.insert(currKey.getOwned());
                    } else {
                        auto additionalKeySize =
                            currKey.objsize() + kEstimatedAdditionalBytesPerItemInBSONArray;
                        if (splitVectorResponseSize + additionalKeySize > BSONObjMaxUserSize) {
                            if (splitKeys.empty()) {
                                // Keep trying until we get at least one split point that isn't
                                // above the max object user size.
                                state = exec->getNext(&currKey, nullptr);
                                continue;
                            }

                            LOGV2(22109,
                                  "Max BSON response size reached for split vector before the end "
                                  "of chunk {namespace} {minKey} -->> {maxKey}",
                                  "Max BSON response size reached for split vector before the end "
                                  "of chunk",
                                  logAttrs(nss),
                                  "minKey"_attr =
                                      redact(prettyKey(shardKeyIdx->keyPattern(), minKey)),
                                  "maxKey"_attr =
                                      redact(prettyKey(shardKeyIdx->keyPattern(), maxKey)));
                            break;
                        }

                        splitVectorResponseSize += additionalKeySize;
                        splitKeys.push_back(currKey.getOwned());
                        currCount = 0;
                        numChunks++;
                        LOGV2_DEBUG(22110,
                                    4,
                                    "Picked a split key: {key}",
                                    "Picked a split key",
                                    "key"_attr = redact(currKey));
                    }
                }

                // Stop if we have enough split points.
                if (maxSplitPoints && maxSplitPoints.value() &&
                    (numChunks >= maxSplitPoints.value())) {
                    LOGV2(22111,
                          "Max number of requested split points reached ({numSplitPoints}) before "
                          "the end of chunk {namespace} {minKey} -->> {maxKey}",
                          "Max number of requested split points reached before the end of chunk",
                          "numSplitPoints"_attr = numChunks,
                          logAttrs(nss),
                          "minKey"_attr = redact(prettyKey(shardKeyIdx->keyPattern(), minKey)),
                          "maxKey"_attr = redact(prettyKey(shardKeyIdx->keyPattern(), maxKey)));
                    break;
                }

                state = exec->getNext(&currKey, nullptr);
            }

            if (!force)
                break;

            //
            // If we're forcing a split at the halfway point, then the first pass was just
            // to count the keys, and we still need a second pass.
            //

            force = false;
            keyCount = currCount / 2;
            currCount = 0;
            LOGV2(22112,
                  "splitVector doing another cycle because of force, keyCount now: {keyCount}",
                  "splitVector doing another cycle because of force",
                  "keyCount"_attr = keyCount);

            exec = InternalPlanner::shardKeyIndexScan(opCtx,
                                                      &collection.getCollection(),
                                                      *shardKeyIdx,
                                                      minKey,
                                                      maxKey,
                                                      BoundInclusion::kIncludeStartKeyOnly,
                                                      PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                                      InternalPlanner::FORWARD);

            state = exec->getNext(&currKey, nullptr);
        }

        //
        // Format the result and issue any warnings about the data we gathered while traversing the
        // index
        //

        // Warn for keys that are more numerous than maxChunkSizeBytes allows.
        for (auto it = tooFrequentKeys.cbegin(); it != tooFrequentKeys.cend(); ++it) {
            LOGV2_WARNING(22114,
                          "Possible low cardinality key detected in {namespace} - key is "
                          "{key}",
                          "Possible low cardinality key detected",
                          logAttrs(nss),
                          "key"_attr = redact(prettyKey(shardKeyIdx->keyPattern(), *it)));
        }

        // Remove the sentinel at the beginning before returning
        splitKeys.erase(splitKeys.begin());

        if (timer.millis() > serverGlobalParams.slowMS.load()) {
            LOGV2_INFO(
                22115,
                "Finding the split vector for {namespace} over {keyPattern} keyCount: {keyCount} "
                "numSplits: {numSplits} lookedAt: {currCount} took {duration}",
                "Finding the split vector completed",
                logAttrs(nss),
                "keyPattern"_attr = redact(keyPattern),
                "keyCount"_attr = keyCount,
                "numSplits"_attr = splitKeys.size(),
                "currCount"_attr = currCount,
                "duration"_attr = Milliseconds(timer.millis()));
        }
    }

    return splitKeys;
}

}  // namespace mongo
