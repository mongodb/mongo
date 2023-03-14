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

#include "mongo/db/timeseries/bucket_catalog/bucket.h"

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
        if (it->second(bucket->bucketId.ns)) {
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

boost::optional<BucketState> changeBucketStateHelper(
    BucketStateRegistry& registry,
    WithLock lock,
    const BucketId& bucketId,
    const BucketStateRegistry::StateChangeFn& change) {
    auto it = registry.bucketStates.find(bucketId);
    const boost::optional<BucketState> initial =
        (it == registry.bucketStates.end()) ? boost::none : boost::make_optional(it->second);
    const boost::optional<BucketState> target = change(initial, registry.currentEra);

    // If we are initiating or finishing a direct write, we need to advance the era. This allows us
    // to synchronize with reopening attempts that do not directly observe a state with the
    // kPendingDirectWrite flag set, but which nevertheless may be trying to reopen a stale bucket.
    if ((target.has_value() && target.value().isSet(BucketStateFlag::kPendingDirectWrite) &&
         (!initial.has_value() || !initial.value().isSet(BucketStateFlag::kPendingDirectWrite))) ||
        (initial.has_value() && initial.value().isSet(BucketStateFlag::kPendingDirectWrite) &&
         (!target.has_value() || !target.value().isSet(BucketStateFlag::kPendingDirectWrite)))) {
        ++registry.currentEra;
    }

    // If initial and target are not both set, then we are either initializing or erasing the state.
    if (!target.has_value()) {
        if (initial.has_value()) {
            registry.bucketStates.erase(it);
        }
        return boost::none;
    } else if (!initial.has_value()) {
        registry.bucketStates.emplace(bucketId, target.value());
        return target;
    }

    // At this point we can now assume that both initial and target are set.

    // We cannot prepare a bucket that isn't eligible for insertions. We expect to attempt this when
    // we try to prepare a batch on a bucket that's been recently cleared.
    if (!initial.value().isPrepared() && target.value().isPrepared() &&
        initial.value().conflictsWithInsertion()) {
        return initial;
    }

    // We cannot transition from a prepared state to pending compression, as that would indicate a
    // programmer error.
    invariant(!initial.value().isPrepared() ||
              !target.value().isSet(BucketStateFlag::kPendingCompression));

    it->second = target.value();

    return target;
}

boost::optional<BucketState> markIndividualBucketCleared(BucketStateRegistry& registry,
                                                         WithLock lock,
                                                         const BucketId& bucketId) {
    return changeBucketStateHelper(
        registry,
        lock,
        bucketId,
        [](boost::optional<BucketState> input, std::uint64_t) -> boost::optional<BucketState> {
            if (!input.has_value()) {
                return boost::none;
            }
            return input.value().setFlag(BucketStateFlag::kCleared);
        });
}
}  // namespace

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

void clearSetOfBuckets(BucketStateRegistry& registry,
                       BucketStateRegistry::ShouldClearFn&& shouldClear) {
    stdx::lock_guard lk{registry.mutex};
    registry.clearedSets[++registry.currentEra] = std::move(shouldClear);
}

std::uint64_t getClearedSetsCount(const BucketStateRegistry& registry) {
    stdx::lock_guard lk{registry.mutex};
    return registry.clearedSets.size();
}

boost::optional<BucketState> getBucketState(BucketStateRegistry& registry, Bucket* bucket) {
    stdx::lock_guard catalogLock{registry.mutex};
    // If the bucket has been cleared, we will set the bucket state accordingly to reflect that.
    if (isMemberOfClearedSet(registry, catalogLock, bucket)) {
        return markIndividualBucketCleared(registry, catalogLock, bucket->bucketId);
    }
    auto it = registry.bucketStates.find(bucket->bucketId);
    return it != registry.bucketStates.end() ? boost::make_optional(it->second) : boost::none;
}

boost::optional<BucketState> getBucketState(const BucketStateRegistry& registry,
                                            const BucketId& bucketId) {
    stdx::lock_guard catalogLock{registry.mutex};
    auto it = registry.bucketStates.find(bucketId);
    return it != registry.bucketStates.end() ? boost::make_optional(it->second) : boost::none;
}

boost::optional<BucketState> changeBucketState(BucketStateRegistry& registry,
                                               Bucket* bucket,
                                               const BucketStateRegistry::StateChangeFn& change) {
    stdx::lock_guard catalogLock{registry.mutex};
    if (isMemberOfClearedSet(registry, catalogLock, bucket)) {
        return markIndividualBucketCleared(registry, catalogLock, bucket->bucketId);
    }

    return changeBucketStateHelper(registry, catalogLock, bucket->bucketId, change);
}

boost::optional<BucketState> changeBucketState(BucketStateRegistry& registry,
                                               const BucketId& bucketId,
                                               const BucketStateRegistry::StateChangeFn& change) {
    stdx::lock_guard catalogLock{registry.mutex};
    return changeBucketStateHelper(registry, catalogLock, bucketId, change);
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

}  // namespace mongo::timeseries::bucket_catalog
