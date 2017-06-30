/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
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
#include "mongo/db/db_raii.h"
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

const int kMaxObjectPerChunk{250000};

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
                                             boost::optional<long long> maxChunkSize,
                                             boost::optional<long long> maxChunkSizeBytes) {
    std::vector<BSONObj> splitKeys;

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
        IndexDescriptor* idx =
            collection->getIndexCatalog()->findShardKeyPrefixedIndex(opCtx, keyPattern, false);
        if (idx == NULL) {
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

        // Forcing a split is equivalent to having maxChunkSize be the size of the current
        // chunk, i.e., the logic below will split that chunk in half

        if (force) {
            maxChunkSize = dataSize;
        } else if (!maxChunkSize) {
            if (maxChunkSizeBytes) {
                maxChunkSize = maxChunkSizeBytes.get();
            }
        } else {
            maxChunkSize = maxChunkSize.get() * 1 << 20;
        }

        // We need a maximum size for the chunk, unless we're not actually capable of finding any
        // split points.
        if ((!maxChunkSize || maxChunkSize.get() <= 0) && recCount != 0) {
            return {ErrorCodes::InvalidOptions, "need to specify the desired max chunk size"};
        }

        // If there's not enough data for more than one chunk, no point continuing.
        if (dataSize < maxChunkSize.get() || recCount == 0) {
            std::vector<BSONObj> emptyVector;
            return emptyVector;
        }

        log() << "request split points lookup for chunk " << nss.toString() << " " << redact(minKey)
              << " -->> " << redact(maxKey);

        // We'll use the average object size and number of object to find approximately how many
        // keys each chunk should have. We'll split at half the maxChunkSize or maxChunkObjects,
        // if provided.
        const long long avgRecSize = dataSize / recCount;

        long long keyCount = maxChunkSize.get() / (2 * avgRecSize);

        if (maxChunkObjects.get() && (maxChunkObjects.get() < keyCount)) {
            log() << "limiting split vector to " << maxChunkObjects.get() << " (from " << keyCount
                  << ") objects ";
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
        PlanExecutor::ExecState state = exec->getNext(&currKey, NULL);
        if (PlanExecutor::ADVANCED != state) {
            return {ErrorCodes::OperationFailed,
                    "can't open a cursor to scan the range (desired range is possibly empty)"};
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
                        splitKeys.push_back(currKey.getOwned());
                        currCount = 0;
                        numChunks++;
                        LOG(4) << "picked a split key: " << redact(currKey);
                    }
                }

                // Stop if we have enough split points.
                if (maxSplitPoints && maxSplitPoints.get() && (numChunks >= maxSplitPoints.get())) {
                    log() << "max number of requested split points reached (" << numChunks
                          << ") before the end of chunk " << nss.toString() << " " << redact(minKey)
                          << " -->> " << redact(maxKey);
                    break;
                }

                state = exec->getNext(&currKey, NULL);
            }

            if (PlanExecutor::DEAD == state || PlanExecutor::FAILURE == state) {
                return {ErrorCodes::OperationFailed,
                        "Executor error during splitVector command: " +
                            WorkingSetCommon::toStatusString(currKey)};
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
            log() << "splitVector doing another cycle because of force, keyCount now: " << keyCount;

            exec = InternalPlanner::indexScan(opCtx,
                                              collection,
                                              idx,
                                              minKey,
                                              maxKey,
                                              BoundInclusion::kIncludeStartKeyOnly,
                                              PlanExecutor::YIELD_AUTO,
                                              InternalPlanner::FORWARD);

            state = exec->getNext(&currKey, NULL);
        }

        //
        // Format the result and issue any warnings about the data we gathered while traversing the
        // index
        //

        // Warn for keys that are more numerous than maxChunkSize allows.
        for (auto it = tooFrequentKeys.cbegin(); it != tooFrequentKeys.cend(); ++it) {
            warning() << "possible low cardinality key detected in " << nss.toString()
                      << " - key is " << prettyKey(idx->keyPattern(), *it);
        }

        // Remove the sentinel at the beginning before returning
        splitKeys.erase(splitKeys.begin());

        if (timer.millis() > serverGlobalParams.slowMS) {
            warning() << "Finding the split vector for " << nss.toString() << " over "
                      << redact(keyPattern) << " keyCount: " << keyCount
                      << " numSplits: " << splitKeys.size() << " lookedAt: " << currCount
                      << " took " << timer.millis() << "ms";
        }

        // Warning: we are sending back an array of keys but are currently limited to 4MB work of
        // 'result' size. This should be okay for now.
    }

    // Make sure splitKeys is in ascending order
    std::sort(
        splitKeys.begin(), splitKeys.end(), SimpleBSONObjComparator::kInstance.makeLessThan());

    return splitKeys;
}

}  // namespace mongo
