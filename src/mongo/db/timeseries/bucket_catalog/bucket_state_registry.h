/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#pragma once

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <variant>

#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_identifiers.h"
#include "mongo/db/timeseries/timeseries_tracked_types.h"
#include "mongo/db/timeseries/timeseries_tracking_context.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/hierarchical_acquisition.h"
#include "mongo/util/uuid.h"

namespace mongo::timeseries::bucket_catalog {

struct Bucket;

/**
 * Describes if the state within the BucketStateRegistry was successfully changed.
 */
enum class StateChangeSuccessful { kYes, kNo };

/**
 * Controls whether to stop tracking the bucket after finishing all direct writes.
 */
enum class ContinueTrackingBucket { kContinue, kStop };

/**
 * State Transition Chart:
 * {+ = valid transition, INV = invariants, WCE = throws WriteConflictException, nop = no-operation}
 *
 * | Current State      |                           Tranistion State                            |
 * |--------------------|:---------:|:------:|:-------:|:------:|:--------:|:------------------:|
 * |                    | Untracked | Normal | Cleared | Frozen | Prepared | DirectWriteCounter |
 * |--------------------|-----------|--------|---------|--------|----------|--------------------|
 * | Untracked          |    nop    |   +    |   nop   |   +    |   INV    |         +          |
 * | Normal             |     +     |   +    |    +    |   +    |    +     |         +          |
 * | Cleared            |     +     |   +    |    +    |   +    |   nop    |         +          |
 * | Frozen             |    nop    |  nop   |   nop   |  nop   |   nop    |        nop         |
 * | Prepared           |     +     |  INV   |    +    |   +    |   INV    |        nop         |
 * | PreparedAndCleared |     +     |  WCE   |    +    |   +    |   nop    |        WCE         |
 * | PreparedAndFrozen  |    nop    |  WCE   |   nop   |   +    |   nop    |        nop         |
 * | DirectWriteCounter |    nop    |  WCE   |   nop   |   +    |   nop    |         +          |
 *
 * Note: we never explicitly set the 'kPreparedAndCleared' state.
 */
enum class BucketState : uint8_t {
    // Can accept inserts.
    kNormal,
    // Can accept inserts, and has an outstanding prepared commit.
    kPrepared,
    // Cannot accept inserts as the bucket will soon be removed from the registry.
    kCleared,
    // Cannot accept inserts and will continue to not accept inserts.
    kFrozen,
    // Cannot accept inserts, and has an outstanding prepared commit. This
    // state will propogate WriteConflictExceptions to all writers aside from
    // the writer who prepared the commit.
    kPreparedAndCleared,
    // Cannot accept inserts, and has an outstanding prepared commit. This
    // state will propogate WriteConflictExceptions to all writers aside from
    // the writer who prepared the commit.
    kPreparedAndFrozen,
};

/**
 * Writes initiated outside of the BucketCatalog are considered "direct writes" since they are
 * operating directly on the 'system.buckets' collection. We must synchronize these writes with the
 * BucketCatalog to ensure we don't try to insert into a bucket that is currently being written to.
 * We also represent buckets undergoing compression with a DirectWriteCounter.
 *
 * Note: we cannot perform direct writes on prepared buckets and there can be multiple direct writes
 * on the same bucket. Conflicts between multiple simultaneous direct writes are mediated by the
 * storage engine.
 */
using DirectWriteCounter = std::int32_t;

/**
 * A helper struct to hold and synchronize both individual bucket states and global state about the
 * catalog era used to support asynchronous 'clear' operations.
 *
 * Provides thread-safety by taking the 'catalogMutex' for all operations. DO NOT directly access
 * any members without holding this lock.
 */
struct BucketStateRegistry {
    using Era = std::uint64_t;
    using ShouldClearFn = std::function<bool(const UUID&)>;

    mutable Mutex mutex =
        MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(0), "BucketStateRegistry::mutex");

    // Global number tracking the current number of eras that have passed. Incremented each time
    // a bucket is cleared.
    Era currentEra = 0;

    // Mapping of era to counts of how many buckets are associated with that era.
    tracked_map<Era, uint64_t> bucketsPerEra;

    // Bucket state for synchronization with direct writes.
    tracked_unordered_map<BucketId, std::variant<BucketState, DirectWriteCounter>, BucketHasher>
        bucketStates;

    // Registry storing 'clearSetOfBuckets' operations. Maps from era to a list of cleared
    // collection UUIDS.
    tracked_map<Era, tracked_vector<UUID>> clearedSets;

    BucketStateRegistry(TrackingContext& trackingContext);
};

BucketStateRegistry::Era getCurrentEra(const BucketStateRegistry& registry);
BucketStateRegistry::Era getCurrentEraAndIncrementBucketCount(BucketStateRegistry& registry);
void decrementBucketCountForEra(BucketStateRegistry& registry, BucketStateRegistry::Era value);
BucketStateRegistry::Era getBucketCountForEra(BucketStateRegistry& registry,
                                              BucketStateRegistry::Era value);

/**
 * Asynchronously clears all buckets belonging to cleared collection UUIDs.
 */
void clearSetOfBuckets(BucketStateRegistry& registry, tracked_vector<UUID> clearedCollectionUUIDs);

/**
 * Returns the number of clear operations currently stored in the clear registry.
 */
std::uint64_t getClearedSetsCount(const BucketStateRegistry& registry);

/**
 * Retrieves the bucket state if it is tracked in the catalog. Modifies the bucket state if
 * the bucket is found to have been cleared.
 */
boost::optional<std::variant<BucketState, DirectWriteCounter>> getBucketState(
    BucketStateRegistry& registry, Bucket* bucket);

/**
 * Retrieves the bucket state if it is tracked in the catalog.
 */
boost::optional<std::variant<BucketState, DirectWriteCounter>> getBucketState(
    BucketStateRegistry& registry, const BucketId& bucketId);

/**
 * Returns true if the state is cleared.
 */
bool isBucketStateCleared(std::variant<BucketState, DirectWriteCounter>& state);

/**
 * Returns true if the state is frozen.
 */
bool isBucketStateFrozen(std::variant<BucketState, DirectWriteCounter>& state);

/**
 * Returns true if the state is prepared.
 */
bool isBucketStatePrepared(std::variant<BucketState, DirectWriteCounter>& state);

/**
 * Returns true if the state transiently conflicts with reopening (aka a direct write).
 */
bool transientlyConflictsWithReopening(std::variant<BucketState, DirectWriteCounter>& state);

/**
 * Returns true if the state conflicts with reopening or is cleared.
 */
bool conflictsWithInsertions(std::variant<BucketState, DirectWriteCounter>& state);

/**
 * Initializes the state of the bucket within the registry to a state of 'kNormal'. If included,
 * checks the registry Era against the 'targetEra' prior to performing the initialization to prevent
 * operating on a potentially stale bucket. Returns WriteConflict if the current bucket state
 * conflicts with reopening.
 *
 * |   Current State    |         Result
 * |--------------------|------------------------
 * | Untracked          | kNormal
 * | Normal             | kNormal
 * | Cleared            | kNormal
 * | Frozen             | kFrozen
 * | Prepared           | TimeseriesBucketFrozen
 * | PreparedAndCleared | WriteConflict
 * | PreparedAndFrozen  | WriteConflict
 * | DirectWriteCounter | WriteConflict
 */
Status initializeBucketState(BucketStateRegistry& registry,
                             const BucketId& bucketId,
                             Bucket* bucket = nullptr,
                             boost::optional<BucketStateRegistry::Era> targetEra = boost::none);

/**
 * Transitions bucket state to 'kPrepared'. If included, checks if the 'bucket' has been marked as
 * cleared prior to performing transition to prevent operating on a potentially stale bucket.
 * Returns enum describing if the state change was successful or not.
 *
 * |   Current State    |  Result
 * |--------------------|-----------
 * | Untracked          | invariants
 * | Normal             | kPrepared
 * | Cleared            |     -
 * | Frozen             |     -
 * | Prepared           | invariants
 * | PreparedAndCleared |     -
 * | PreparedAndFrozen  |     -
 * | DirectWriteCounter |     -
 */
StateChangeSuccessful prepareBucketState(BucketStateRegistry& registry,
                                         const BucketId& bucketId,
                                         Bucket* bucket = nullptr);

/**
 * Detransition bucket state from 'kPrepared' to 'kNormal' (or 'kCleared' if the bucket was cleared
 * while the bucket was in the 'kPrepared' state). If included, checks if the 'bucket' has been
 * marked as cleared prior to performing transition to prevent operating on a potentially stale
 * bucket. Returns enum describing if the state change was successful or not.
 *
 * |   Current State    |   Result
 * |--------------------|-----------
 * | Untracked          | invariants
 * | Normal             | invariants
 * | Cleared            | invariants
 * | Frozen             | invariants
 * | Prepared           | kNormal
 * | PreparedAndCleared | kCleared
 * | PreparedAndFrozen  | kFrozen
 * | DirectWriteCounter | invariants
 */
StateChangeSuccessful unprepareBucketState(BucketStateRegistry& registry,
                                           const BucketId& bucketId,
                                           Bucket* bucket = nullptr);

/**
 * Tracks the bucket with a counter which is incremented everytime this function is called and must
 * be followed by a call to 'removeDirectWrite'. We cannot perform transition on prepared buckets.
 * If 'ContinueTrackingBucket' is set to 'kStop', we will erase the bucket from the registry upon
 * finishing all direct writes else the bucket will transition to 'kCleared'.
 *
 * |   Current State    |      Result
 * |--------------------|-----------------
 * | Untracked          | negative count
 * | Normal             | positive count
 * | Cleared            | positive count
 * | Frozen             |       -
 * | Prepared           |       -
 * | PreparedAndCleared |       -
 * | PreparedAndFrozen  |       -
 * | DirectWriteCounter | increments value
 */
std::variant<BucketState, DirectWriteCounter> addDirectWrite(
    BucketStateRegistry& registry,
    const BucketId& bucketId,
    ContinueTrackingBucket continueTrackingBucket = ContinueTrackingBucket::kContinue);

/**
 * Requires the state to be tracked by a counter. The direct write counter can be positive or
 * negative which affects the behavior of the state when the counter reaches 0. When positive, we
 * decrement the counter and transition the state to 'kCleared' when it reaches 0. When negative, we
 * increment the counter and erase the state when we reach 0.
 *
 * |   Current State    |      Result
 * |--------------------|-----------------
 * | Untracked          | invariants
 * | Normal             | invariants
 * | Cleared            | invariants
 * | Frozen             |        -
 * | Prepared           | invariants
 * | PreparedAndCleared | invariants
 * | PreparedAndFrozen  |        -
 * | DirectWriteCounter | decrements value
 */
void removeDirectWrite(BucketStateRegistry& registry, const BucketId& bucketId);

/**
 * Transitions bucket state to 'kCleared' or 'kPreparedAndCleared'. No action is required for:
 * i.   buckets not currently being tracked by the registry
 * ii.  buckets with pending direct writes (since they will either be cleared or removed from the
 *      registry upon finishing)
 *
 * |   Current State    |       Result
 * |--------------------|--------------------
 * | Untracked          |         -
 * | Normal             | kCleared
 * | Cleared            | kCleared
 * | Frozen             | kFrozen
 * | Prepared           | kPreparedAndCleared
 * | PreparedAndCleared | kPreparedAndCleared
 * | PreparedAndFrozen  | kPreparedAndFrozen
 * | DirectWriteCounter |         -
 */
void clearBucketState(BucketStateRegistry& registry, const BucketId& bucketId);

/**
 * Erases the bucket state from the registry. If there are on-going direct writes, erase the state
 * once the writes finish.
 *
 * |   Current State    |      Result
 * |--------------------|----------------
 * | Untracked          |        -
 * | Normal             | erases entry
 * | Cleared            | erases entry
 * | Frozen             |        -
 * | Prepared           | erases entry
 * | PreparedAndCleared | erases entry
 * | PreparedAndFrozen  |        -
 * | DirectWriteCounter | negative value
 */
void stopTrackingBucketState(BucketStateRegistry& registry, const BucketId& bucketId);

/**
 * Freezes the bucket. A frozen bucket cannot become unfrozen or untracked.
 *
 * |   Current State    |       Result
 * |--------------------|--------------------
 * | Untracked          | kFrozen
 * | Normal             | kFrozen
 * | Cleared            | kFrozen
 * | Frozen             | kFrozen
 * | Prepared           | kPreparedAndFrozen
 * | PreparedAndCleared | kPreparedAndFrozen
 * | PreparedAndFrozen  | kPreparedAndFrozen
 * | DirectWriteCounter | kFrozen
 */
void freezeBucket(BucketStateRegistry&, const BucketId&);

/**
 * Appends statistics for observability.
 */
void appendStats(const BucketStateRegistry& registry, BSONObjBuilder& builder);

/**
 * Helper to stringify BucketState.
 */
std::string bucketStateToString(const std::variant<BucketState, DirectWriteCounter>& state);

}  // namespace mongo::timeseries::bucket_catalog
