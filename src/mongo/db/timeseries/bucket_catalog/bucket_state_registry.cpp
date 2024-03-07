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

#include "mongo/db/timeseries/bucket_catalog/bucket_state_registry.h"

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <algorithm>
#include <boost/optional.hpp>
#include <cstdint>
#include <fmt/format.h>
#include <utility>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/db/timeseries/bucket_catalog/bucket.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/with_lock.h"

namespace mongo::timeseries::bucket_catalog {

namespace {
void cleanClearRegistry(BucketStateRegistry& registry) {
    // When the count map is empty we can clean the whole clear registry.
    if (registry.bucketsPerEra.begin() == registry.bucketsPerEra.end()) {
        registry.clearedSets.erase(registry.clearedSets.begin(), registry.clearedSets.end());
        return;
    }

    uint64_t smallestEra = registry.bucketsPerEra.begin()->first;
    auto endIt = upper_bound(registry.clearedSets.begin(),
                             registry.clearedSets.end(),
                             smallestEra,
                             [](uint64_t val, auto kv) { return val < kv.first; });

    registry.clearedSets.erase(registry.clearedSets.begin(), endIt);
}

void decrementEraCountHelper(BucketStateRegistry& registry, BucketStateRegistry::Era era) {
    auto it = registry.bucketsPerEra.find(era);
    invariant(it != registry.bucketsPerEra.end());
    if (it->second == 1) {
        registry.bucketsPerEra.erase(it);
        cleanClearRegistry(registry);
    } else {
        --it->second;
    }
}

void incrementEraCountHelper(BucketStateRegistry& registry, BucketStateRegistry::Era era) {
    auto it = registry.bucketsPerEra.find(era);
    if (it == registry.bucketsPerEra.end()) {
        registry.bucketsPerEra[era] = 1;
    } else {
        ++it->second;
    }
}

bool isMemberOfClearedSet(BucketStateRegistry& registry, WithLock lock, Bucket* bucket) {
    for (auto it = registry.clearedSets.lower_bound(bucket->lastChecked + 1);
         it != registry.clearedSets.end();
         ++it) {
        if (std::find(it->second.begin(), it->second.end(), bucket->bucketId.collectionUUID) !=
            it->second.end()) {
            return true;
        }
    }
    if (bucket->lastChecked != registry.currentEra) {
        decrementEraCountHelper(registry, bucket->lastChecked);
        incrementEraCountHelper(registry, registry.currentEra);
        bucket->lastChecked = registry.currentEra;
    }

    return false;
}

void markIndividualBucketCleared(BucketStateRegistry& registry,
                                 WithLock catalogLock,
                                 const BucketId& bucketId) {
    auto it = registry.bucketStates.find(bucketId);
    if (it == registry.bucketStates.end() || holds_alternative<DirectWriteCounter>(it->second) ||
        isBucketStateFrozen(it->second)) {
        return;
    }
    it->second = (isBucketStatePrepared(it->second)) ? BucketState::kPreparedAndCleared
                                                     : BucketState::kCleared;
}
}  // namespace

BucketStateRegistry::BucketStateRegistry(TrackingContext& trackingContext)
    : bucketsPerEra(make_tracked_map<Era, uint64_t>(trackingContext)),
      bucketStates(make_tracked_unordered_map<BucketId,
                                              std::variant<BucketState, DirectWriteCounter>,
                                              BucketHasher>(trackingContext)),
      clearedSets(make_tracked_map<Era, tracked_vector<UUID>>(trackingContext)) {}

BucketStateRegistry::Era getCurrentEra(const BucketStateRegistry& registry) {
    stdx::lock_guard lk{registry.mutex};
    return registry.currentEra;
}

BucketStateRegistry::Era getCurrentEraAndIncrementBucketCount(BucketStateRegistry& registry) {
    stdx::lock_guard lk{registry.mutex};
    incrementEraCountHelper(registry, registry.currentEra);
    return registry.currentEra;
}

void decrementBucketCountForEra(BucketStateRegistry& registry, BucketStateRegistry::Era value) {
    stdx::lock_guard lk{registry.mutex};
    decrementEraCountHelper(registry, value);
}

BucketStateRegistry::Era getBucketCountForEra(BucketStateRegistry& registry,
                                              BucketStateRegistry::Era value) {
    stdx::lock_guard lk{registry.mutex};
    auto it = registry.bucketsPerEra.find(value);
    if (it == registry.bucketsPerEra.end()) {
        return 0;
    } else {
        return it->second;
    }
}

void clearSetOfBuckets(BucketStateRegistry& registry, tracked_vector<UUID> clearedCollectionUUIDs) {
    stdx::lock_guard lk{registry.mutex};
    registry.clearedSets[++registry.currentEra] = std::move(clearedCollectionUUIDs);
}

std::uint64_t getClearedSetsCount(const BucketStateRegistry& registry) {
    stdx::lock_guard lk{registry.mutex};
    return registry.clearedSets.size();
}

boost::optional<std::variant<BucketState, DirectWriteCounter>> getBucketState(
    BucketStateRegistry& registry, Bucket* bucket) {
    stdx::lock_guard catalogLock{registry.mutex};

    // If the bucket has been cleared, we will set the bucket state accordingly to reflect that.
    if (isMemberOfClearedSet(registry, catalogLock, bucket)) {
        markIndividualBucketCleared(registry, catalogLock, bucket->bucketId);
    }

    auto it = registry.bucketStates.find(bucket->bucketId);
    if (it == registry.bucketStates.end()) {
        return boost::none;
    }

    return it->second;
}

boost::optional<std::variant<BucketState, DirectWriteCounter>> getBucketState(
    BucketStateRegistry& registry, const BucketId& bucketId) {
    stdx::lock_guard catalogLock{registry.mutex};

    auto it = registry.bucketStates.find(bucketId);
    if (it == registry.bucketStates.end()) {
        return boost::none;
    }

    return it->second;
}

bool isBucketStateCleared(std::variant<BucketState, DirectWriteCounter>& state) {
    if (auto* bucketState = get_if<BucketState>(&state)) {
        return *bucketState == BucketState::kCleared ||
            *bucketState == BucketState::kPreparedAndCleared;
    }
    return false;
}

bool isBucketStateFrozen(std::variant<BucketState, DirectWriteCounter>& state) {
    if (auto* bucketState = get_if<BucketState>(&state)) {
        return *bucketState == BucketState::kFrozen ||
            *bucketState == BucketState::kPreparedAndFrozen;
    }
    return false;
}

bool isBucketStatePrepared(std::variant<BucketState, DirectWriteCounter>& state) {
    if (auto* bucketState = get_if<BucketState>(&state)) {
        return *bucketState == BucketState::kPrepared ||
            *bucketState == BucketState::kPreparedAndCleared ||
            *bucketState == BucketState::kPreparedAndFrozen;
    }
    return false;
}

bool transientlyConflictsWithReopening(std::variant<BucketState, DirectWriteCounter>& state) {
    return holds_alternative<DirectWriteCounter>(state);
}

bool conflictsWithInsertions(std::variant<BucketState, DirectWriteCounter>& state) {
    return transientlyConflictsWithReopening(state) || isBucketStateCleared(state) ||
        isBucketStateFrozen(state);
}

Status initializeBucketState(BucketStateRegistry& registry,
                             const BucketId& bucketId,
                             Bucket* bucket,
                             boost::optional<BucketStateRegistry::Era> targetEra) {
    stdx::lock_guard catalogLock{registry.mutex};

    // Returns a WriteConflict error if the target Era is older than the registry Era or if the
    // 'bucket' is cleared.
    if (targetEra.has_value() && targetEra < registry.currentEra) {
        return {ErrorCodes::WriteConflict, "Bucket may be stale"};
    } else if (bucket && isMemberOfClearedSet(registry, catalogLock, bucket)) {
        markIndividualBucketCleared(registry, catalogLock, bucketId);
        return {ErrorCodes::WriteConflict, "Bucket may be stale"};
    }

    auto it = registry.bucketStates.find(bucketId);
    if (it == registry.bucketStates.end()) {
        registry.bucketStates.emplace(bucketId, BucketState::kNormal);
        return Status::OK();
    } else if (transientlyConflictsWithReopening(it->second)) {
        // If we are currently performing direct writes on it we cannot initialize the bucket to a
        // normal state.
        return {ErrorCodes::WriteConflict,
                "Bucket initialization failed: conflict with an exisiting bucket"};
    } else if (isBucketStateFrozen(it->second)) {
        return {ErrorCodes::TimeseriesBucketFrozen,
                "Bucket initialization failed: bucket is frozen"};
    }

    invariant(!isBucketStatePrepared(it->second));
    it->second = BucketState::kNormal;

    return Status::OK();
}

StateChangeSuccessful prepareBucketState(BucketStateRegistry& registry,
                                         const BucketId& bucketId,
                                         Bucket* bucket) {
    stdx::lock_guard catalogLock{registry.mutex};

    if (bucket && isMemberOfClearedSet(registry, catalogLock, bucket)) {
        markIndividualBucketCleared(registry, catalogLock, bucketId);
        return StateChangeSuccessful::kNo;
    }

    auto it = registry.bucketStates.find(bucketId);
    invariant(it != registry.bucketStates.end());

    // We cannot update the bucket if it is in a cleared state or has a pending direct write.
    if (conflictsWithInsertions(it->second)) {
        return StateChangeSuccessful::kNo;
    }

    // We cannot prepare an already prepared bucket.
    invariant(!isBucketStatePrepared(it->second));

    it->second = BucketState::kPrepared;
    return StateChangeSuccessful::kYes;
}

StateChangeSuccessful unprepareBucketState(BucketStateRegistry& registry,
                                           const BucketId& bucketId,
                                           Bucket* bucket) {
    stdx::lock_guard catalogLock{registry.mutex};

    if (bucket && isMemberOfClearedSet(registry, catalogLock, bucket)) {
        markIndividualBucketCleared(registry, catalogLock, bucketId);
        return StateChangeSuccessful::kNo;
    }

    auto it = registry.bucketStates.find(bucketId);
    invariant(it != registry.bucketStates.end());
    invariant(holds_alternative<BucketState>(it->second));
    invariant(isBucketStatePrepared(it->second));

    auto bucketState = get<BucketState>(it->second);
    // There is also a chance the state got cleared or frozen, in which case we should keep the
    // state as 'kCleared' or 'kFrozen'.
    if (bucketState == BucketState::kPreparedAndCleared) {
        it->second = BucketState::kCleared;
    } else if (bucketState == BucketState::kPreparedAndFrozen) {
        it->second = BucketState::kFrozen;
    } else {
        it->second = BucketState::kNormal;
    }
    return StateChangeSuccessful::kYes;
}

std::variant<BucketState, DirectWriteCounter> addDirectWrite(
    BucketStateRegistry& registry,
    const BucketId& bucketId,
    ContinueTrackingBucket continueTrackingBucket) {
    stdx::lock_guard catalogLock{registry.mutex};

    auto it = registry.bucketStates.find(bucketId);
    DirectWriteCounter newDirectWriteCount = 1;
    if (it == registry.bucketStates.end()) {
        // If we are initiating a direct write, we need to advance the era. This allows us to
        // synchronize with reopening attempts that do not directly observe a state with direct
        // write counter, but which nevertheless may be trying to reopen a stale bucket.
        ++registry.currentEra;

        // We can perform direct writes on buckets not being tracked by the registry. Tracked by a
        // negative value to signify we must delete the state from the 'registry' when the counter
        // reaches 0.
        newDirectWriteCount *= -1;
        registry.bucketStates.emplace(bucketId, newDirectWriteCount);
        return newDirectWriteCount;
    } else if (auto* directWriteCount = get_if<DirectWriteCounter>(&it->second)) {
        if (*directWriteCount > 0) {
            newDirectWriteCount = *directWriteCount + 1;
        } else {
            newDirectWriteCount = *directWriteCount - 1;
        }
    } else if (isBucketStateFrozen(it->second) || isBucketStatePrepared(it->second)) {
        // Frozen buckets are safe to receive direct writes. Cannot perform direct writes on
        // prepared buckets.
        return it->second;
    }

    // Convert the direct write counter to a negative value so we can interpret it as an untracked
    // state when the counter goes to 0.
    if (continueTrackingBucket == ContinueTrackingBucket::kStop && newDirectWriteCount > 0) {
        newDirectWriteCount *= -1;
    }
    it->second = newDirectWriteCount;
    return it->second;
}

void removeDirectWrite(BucketStateRegistry& registry, const BucketId& bucketId) {
    stdx::lock_guard catalogLock{registry.mutex};

    auto it = registry.bucketStates.find(bucketId);
    invariant(it != registry.bucketStates.end());
    if (isBucketStateFrozen(it->second)) {
        return;
    }
    invariant(holds_alternative<DirectWriteCounter>(it->second));

    bool removingFinalDirectWrite = true;
    auto directWriteCount = get<DirectWriteCounter>(it->second);
    if (directWriteCount == 1) {
        it->second = BucketState::kCleared;
    } else if (directWriteCount == -1) {
        registry.bucketStates.erase(it);
    } else {
        removingFinalDirectWrite = false;
        directWriteCount = (directWriteCount > 0) ? directWriteCount - 1 : directWriteCount + 1;
        it->second = directWriteCount;
    }

    if (removingFinalDirectWrite) {
        // If we are finishing a direct write, we need to advance the era. This allows us to
        // synchronize with reopening attempts that do not directly observe a state with direct
        // write counter, but which nevertheless may be trying to reopen a stale bucket.
        ++registry.currentEra;
    }
}

void clearBucketState(BucketStateRegistry& registry, const BucketId& bucketId) {
    stdx::lock_guard catalogLock{registry.mutex};
    markIndividualBucketCleared(registry, catalogLock, bucketId);
}

void stopTrackingBucketState(BucketStateRegistry& registry, const BucketId& bucketId) {
    stdx::lock_guard catalogLock{registry.mutex};
    auto it = registry.bucketStates.find(bucketId);
    if (it == registry.bucketStates.end()) {
        return;
    }

    if (transientlyConflictsWithReopening(it->second)) {
        // We cannot release the bucket state of pending direct writes.
        auto directWriteCount = get<DirectWriteCounter>(it->second);
        if (directWriteCount > 0) {
            // A negative value signals the immediate removal of the bucket state after the
            // completion of the direct writes.
            directWriteCount *= -1;
        }
        it->second = directWriteCount;
    } else if (isBucketStateFrozen(it->second)) {
        it->second = BucketState::kFrozen;
    } else {
        registry.bucketStates.erase(it);
    }
}

void freezeBucket(BucketStateRegistry& registry, const BucketId& bucketId) {
    stdx::lock_guard catalogLock{registry.mutex};

    auto it = registry.bucketStates.find(bucketId);
    if (it == registry.bucketStates.end()) {
        registry.bucketStates.emplace(bucketId, BucketState::kFrozen);
        return;
    }
    it->second = (isBucketStatePrepared(it->second)) ? BucketState::kPreparedAndFrozen
                                                     : BucketState::kFrozen;
}

void appendStats(const BucketStateRegistry& registry, BSONObjBuilder& base) {
    stdx::lock_guard catalogLock{registry.mutex};

    BSONObjBuilder builder{base.subobjStart("stateManagement")};

    builder.appendNumber("bucketsManaged", static_cast<long long>(registry.bucketStates.size()));
    builder.appendNumber("currentEra", static_cast<long long>(registry.currentEra));
    builder.appendNumber("erasWithRemainingBuckets",
                         static_cast<long long>(registry.bucketsPerEra.size()));
    builder.appendNumber("trackedClearOperations",
                         static_cast<long long>(registry.clearedSets.size()));
}

std::string bucketStateToString(const std::variant<BucketState, DirectWriteCounter>& state) {
    if (auto* directWriteCount = get_if<DirectWriteCounter>(&state)) {
        return fmt::format("{{type: DirectWrite, value: {}}}", *directWriteCount);
    }

    auto bucketState = get<BucketState>(state);
    switch (bucketState) {
        case BucketState::kNormal: {
            return "{{type: BucketState, value: kNormal}}";
        }
        case BucketState::kPrepared: {
            return "{{type: BucketState, value: kPrepared}}";
        }
        case BucketState::kCleared: {
            return "{{type: BucketState, value: kCleared}}";
        }
        case BucketState::kPreparedAndCleared: {
            return "{{type: BucketState, value: kPreparedAndCleared}}";
        }
        default: {
            MONGO_UNREACHABLE;
        }
    }
}

}  // namespace mongo::timeseries::bucket_catalog
