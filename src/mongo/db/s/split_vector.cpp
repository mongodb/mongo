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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/split_vector.h"

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
#include "mongo/logv2/log.h"

namespace mongo {
namespace {

const int kMaxObjectPerChunk{250000};
const int estimatedAdditionalBytesPerItemInBSONArray{2};

BSONObj prettyKey(const BSONObj& keyPattern, const BSONObj& key) {
    return key.replaceFieldNames(keyPattern).clientReadable();
}

}  // namespace

StatusWith<std::vector<BSONObj>> splitVector(OperationContext* opCtx,
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
        AutoGetCollection autoColl(opCtx, nss, MODE_IS);

        Collection* const collection = autoColl.getCollection();
        if (!collection) {
            return {ErrorCodes::NamespaceNotFound, "ns not found"};
        }

        // Allow multiKey based on the invariant that shard keys must be single-valued. Therefore,
        // any multi-key index prefixed by shard key cannot be multikey over the shard key fields.
        const IndexDescriptor* idx =
            collection->getIndexCatalog()->findShardKeyPrefixedIndex(opCtx, keyPattern, false);
        if (idx == nullptr) {
            return {ErrorCodes::IndexNotFound,
                    "couldn't find index over splitting key " +
                        keyPattern.clientReadable().toString()};
        }

        // extend min to get (min, MinKey, MinKey, ....)
        KeyPattern kp(idx->keyPattern());
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

        // We need a maximum size for the chunk.
        if (!maxChunkSizeBytes || maxChunkSizeBytes.get() <= 0) {
            return {ErrorCodes::InvalidOptions, "need to specify the desired max chunk size"};
        }

        // If there's not enough data for more than one chunk, no point continuing.
        if (dataSize < maxChunkSizeBytes.get() || recCount == 0) {
            std::vector<BSONObj> emptyVector;
            return emptyVector;
        }

        LOGV2(22107,
              "Requested split points lookup for chunk {namespace} {minKey} -->> {maxKey}",
              "Requested split points lookup for chunk",
              "namespace"_attr = nss.toString(),
              "minKey"_attr = redact(minKey),
              "maxKey"_attr = redact(maxKey));

        // We'll use the average object size and number of object to find approximately how many
        // keys each chunk should have. We'll split at half the maxChunkSizeBytes or
        // maxChunkObjects, if provided.
        const long long avgRecSize = dataSize / recCount;

        long long keyCount = maxChunkSizeBytes.get() / (2 * avgRecSize);

        if (maxChunkObjects.get() && (maxChunkObjects.get() < keyCount)) {
            LOGV2(22108,
                  "Limiting the number of documents per chunk to {maxChunkObjects} based "
                  "on the maxChunkObjects parameter for split vector command (compared to maximum "
                  "possible: {maxPossibleDocumentsPerChunk})",
                  "Limiting the number of documents per chunk for split vector command based on "
                  "the maxChunksObject parameter",
                  "maxChunkObjects"_attr = maxChunkObjects.get(),
                  "maxPossibleDocumentsPerChunk"_attr = keyCount);
            keyCount = maxChunkObjects.get();
        }

        //
        // Traverse the index and add the keyCount-th key to the result vector. If that key
        // appeared in the vector before, we omit it. The invariant here is that all the
        // instances of a given key value live in the same chunk.
        //

        Timer timer;
        long long currCount = 0;
        long long numChunks = 0;

        auto exec = InternalPlanner::indexScan(opCtx,
                                               collection,
                                               idx,
                                               minKey,
                                               maxKey,
                                               BoundInclusion::kIncludeStartKeyOnly,
                                               PlanExecutor::YIELD_AUTO,
                                               InternalPlanner::FORWARD);

        BSONObj currKey;
        PlanExecutor::ExecState state = exec->getNext(&currKey, nullptr);
        if (PlanExecutor::ADVANCED != state) {
            return {ErrorCodes::OperationFailed,
                    "can't open a cursor to scan the range (desired range is possibly empty)"};
        }

        // Get the final key in the range, and see if it's the same as the first key.
        BSONObj maxKeyInChunk;
        {
            auto exec = InternalPlanner::indexScan(opCtx,
                                                   collection,
                                                   idx,
                                                   maxKey,
                                                   minKey,
                                                   BoundInclusion::kIncludeEndKeyOnly,
                                                   PlanExecutor::YIELD_AUTO,
                                                   InternalPlanner::BACKWARD);

            PlanExecutor::ExecState state = exec->getNext(&maxKeyInChunk, nullptr);
            if (PlanExecutor::ADVANCED != state) {
                return {ErrorCodes::OperationFailed,
                        "can't open a cursor to find final key in range (desired range is possibly "
                        "empty)"};
            }
        }

        if (currKey.woCompare(maxKeyInChunk) == 0) {
            // Range contains only documents with a single key value.  So we cannot possibly find a
            // split point, and there is no need to scan any further.
            LOGV2_WARNING(
                22113,
                "Possible low cardinality key detected in {namespace} - range {minKey} -->> "
                "{maxKey} contains only the key {key}",
                "Possible low cardinality key detected in range. Range contains only a single key.",
                "namespace"_attr = nss.toString(),
                "minKey"_attr = redact(minKey),
                "maxKey"_attr = redact(maxKey),
                "key"_attr = redact(prettyKey(idx->keyPattern(), currKey)));
            std::vector<BSONObj> emptyVector;
            return emptyVector;
        }

        // Use every 'keyCount'-th key as a split point. We add the initial key as a sentinel,
        // to be removed at the end. If a key appears more times than entries allowed on a
        // chunk, we issue a warning and split on the following key.
        auto tooFrequentKeys = SimpleBSONObjComparator::kInstance.makeBSONObjSet();
        splitKeys.push_back(dotted_path_support::extractElementsBasedOnTemplate(
            prettyKey(idx->keyPattern(), currKey.getOwned()), keyPattern));

        while (1) {
            while (PlanExecutor::ADVANCED == state) {
                currCount++;

                if (currCount > keyCount && !force) {
                    currKey = dotted_path_support::extractElementsBasedOnTemplate(
                        prettyKey(idx->keyPattern(), currKey.getOwned()), keyPattern);

                    // Do not use this split key if it is the same used in the previous split
                    // point.
                    if (currKey.woCompare(splitKeys.back()) == 0) {
                        tooFrequentKeys.insert(currKey.getOwned());
                    } else {
                        auto additionalKeySize =
                            currKey.objsize() + estimatedAdditionalBytesPerItemInBSONArray;
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
                                  "namespace"_attr = nss.toString(),
                                  "minKey"_attr = redact(minKey),
                                  "maxKey"_attr = redact(maxKey));
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
                if (maxSplitPoints && maxSplitPoints.get() && (numChunks >= maxSplitPoints.get())) {
                    LOGV2(22111,
                          "Max number of requested split points reached ({numSplitPoints}) before "
                          "the end of chunk {namespace} {minKey} -->> {maxKey}",
                          "Max number of requested split points reached before the end of chunk",
                          "numSplitPoints"_attr = numChunks,
                          "namespace"_attr = nss.toString(),
                          "minKey"_attr = redact(minKey),
                          "maxKey"_attr = redact(maxKey));
                    break;
                }

                state = exec->getNext(&currKey, nullptr);
            }

            if (PlanExecutor::FAILURE == state) {
                return WorkingSetCommon::getMemberObjectStatus(currKey).withContext(
                    "Executor error during splitVector command");
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

            exec = InternalPlanner::indexScan(opCtx,
                                              collection,
                                              idx,
                                              minKey,
                                              maxKey,
                                              BoundInclusion::kIncludeStartKeyOnly,
                                              PlanExecutor::YIELD_AUTO,
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
                          "namespace"_attr = nss.toString(),
                          "key"_attr = redact(prettyKey(idx->keyPattern(), *it)));
        }

        // Remove the sentinel at the beginning before returning
        splitKeys.erase(splitKeys.begin());

        if (timer.millis() > serverGlobalParams.slowMS) {
            LOGV2_WARNING(
                22115,
                "Finding the split vector for {namespace} over {keyPattern} keyCount: {keyCount} "
                "numSplits: {numSplits} lookedAt: {currCount} took {duration}",
                "Finding the split vector completed",
                "namespace"_attr = nss.toString(),
                "keyPattern"_attr = redact(keyPattern),
                "keyCount"_attr = keyCount,
                "numSplits"_attr = splitKeys.size(),
                "currCount"_attr = currCount,
                "duration"_attr = Milliseconds(timer.millis()));
        }
    }

    // Make sure splitKeys is in ascending order
    std::sort(
        splitKeys.begin(), splitKeys.end(), SimpleBSONObjComparator::kInstance.makeLessThan());

    return splitKeys;
}

}  // namespace mongo
