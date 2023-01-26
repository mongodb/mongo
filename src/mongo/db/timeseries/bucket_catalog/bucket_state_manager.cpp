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

#include "mongo/db/timeseries/bucket_catalog/bucket_state_manager.h"

#include "mongo/db/timeseries/bucket_catalog/bucket.h"

namespace mongo::timeseries::bucket_catalog {

BucketStateManager::BucketStateManager(Mutex* m) : _mutex(m), _era(0) {}

uint64_t BucketStateManager::getEra() {
    stdx::lock_guard lk{*_mutex};
    return _era;
}

uint64_t BucketStateManager::getEraAndIncrementCount() {
    stdx::lock_guard lk{*_mutex};
    _incrementEraCountHelper(_era);
    return _era;
}

void BucketStateManager::decrementCountForEra(uint64_t value) {
    stdx::lock_guard lk{*_mutex};
    _decrementEraCountHelper(value);
}

uint64_t BucketStateManager::getCountForEra(uint64_t value) {
    stdx::lock_guard lk{*_mutex};
    auto it = _countMap.find(value);
    if (it == _countMap.end()) {
        return 0;
    } else {
        return it->second;
    }
}

void BucketStateManager::clearSetOfBuckets(ShouldClearFn&& shouldClear) {
    stdx::lock_guard lk{*_mutex};
    _clearRegistry[++_era] = std::move(shouldClear);
}

uint64_t BucketStateManager::getClearOperationsCount() {
    return _clearRegistry.size();
}

void BucketStateManager::_decrementEraCountHelper(uint64_t era) {
    auto it = _countMap.find(era);
    invariant(it != _countMap.end());
    if (it->second == 1) {
        _countMap.erase(it);
        _cleanClearRegistry();
    } else {
        --it->second;
    }
}

void BucketStateManager::_incrementEraCountHelper(uint64_t era) {
    auto it = _countMap.find(era);
    if (it == _countMap.end()) {
        (_countMap)[era] = 1;
    } else {
        ++it->second;
    }
}

bool BucketStateManager::_isMemberOfClearedSet(WithLock catalogLock, Bucket* bucket) {
    for (auto it = _clearRegistry.lower_bound(bucket->lastChecked + 1); it != _clearRegistry.end();
         ++it) {
        if (it->second(bucket->bucketId.ns)) {
            return true;
        }
    }
    if (bucket->lastChecked != _era) {
        _decrementEraCountHelper(bucket->lastChecked);
        _incrementEraCountHelper(_era);
        bucket->lastChecked = _era;
    }

    return false;
}

boost::optional<BucketState> BucketStateManager::_markIndividualBucketCleared(
    WithLock catalogLock, const BucketId& bucketId) {
    return _changeBucketStateHelper(
        catalogLock,
        bucketId,
        [](boost::optional<BucketState> input, std::uint64_t) -> boost::optional<BucketState> {
            if (!input.has_value()) {
                return boost::none;
            }
            return input.value().setFlag(BucketStateFlag::kCleared);
        });
}

boost::optional<BucketState> BucketStateManager::getBucketState(Bucket* bucket) {
    stdx::lock_guard catalogLock{*_mutex};
    // If the bucket has been cleared, we will set the bucket state accordingly to reflect that.
    if (_isMemberOfClearedSet(catalogLock, bucket)) {
        return _markIndividualBucketCleared(catalogLock, bucket->bucketId);
    }
    auto it = _bucketStates.find(bucket->bucketId);
    return it != _bucketStates.end() ? boost::make_optional(it->second) : boost::none;
}

boost::optional<BucketState> BucketStateManager::getBucketState(const BucketId& bucketId) const {
    stdx::lock_guard catalogLock{*_mutex};
    auto it = _bucketStates.find(bucketId);
    return it != _bucketStates.end() ? boost::make_optional(it->second) : boost::none;
}

boost::optional<BucketState> BucketStateManager::changeBucketState(Bucket* bucket,
                                                                   const StateChangeFn& change) {
    stdx::lock_guard catalogLock{*_mutex};
    if (_isMemberOfClearedSet(catalogLock, bucket)) {
        return _markIndividualBucketCleared(catalogLock, bucket->bucketId);
    }

    return _changeBucketStateHelper(catalogLock, bucket->bucketId, change);
}

boost::optional<BucketState> BucketStateManager::changeBucketState(const BucketId& bucketId,
                                                                   const StateChangeFn& change) {
    stdx::lock_guard catalogLock{*_mutex};
    return _changeBucketStateHelper(catalogLock, bucketId, change);
}

void BucketStateManager::appendStats(BSONObjBuilder* base) const {
    stdx::lock_guard catalogLock{*_mutex};

    BSONObjBuilder builder{base->subobjStart("stateManagement")};

    builder.appendNumber("bucketsManaged", static_cast<long long>(_bucketStates.size()));
    builder.appendNumber("currentEra", static_cast<long long>(_era));
    builder.appendNumber("erasWithRemainingBuckets", static_cast<long long>(_countMap.size()));
    builder.appendNumber("trackedClearOperations", static_cast<long long>(_clearRegistry.size()));
}

boost::optional<BucketState> BucketStateManager::_changeBucketStateHelper(
    WithLock catalogLock, const BucketId& bucketId, const StateChangeFn& change) {
    auto it = _bucketStates.find(bucketId);
    const boost::optional<BucketState> initial =
        (it == _bucketStates.end()) ? boost::none : boost::make_optional(it->second);
    const boost::optional<BucketState> target = change(initial, _era);

    // If we are initiating or finishing a direct write, we need to advance the era. This allows us
    // to synchronize with reopening attempts that do not directly observe a state with the
    // kPendingDirectWrite flag set, but which nevertheless may be trying to reopen a stale bucket.
    if ((target.has_value() && target.value().isSet(BucketStateFlag::kPendingDirectWrite) &&
         (!initial.has_value() || !initial.value().isSet(BucketStateFlag::kPendingDirectWrite))) ||
        (initial.has_value() && initial.value().isSet(BucketStateFlag::kPendingDirectWrite) &&
         (!target.has_value() || !target.value().isSet(BucketStateFlag::kPendingDirectWrite)))) {
        ++_era;
    }

    // If initial and target are not both set, then we are either initializing or erasing the state.
    if (!target.has_value()) {
        if (initial.has_value()) {
            _bucketStates.erase(it);
        }
        return boost::none;
    } else if (!initial.has_value()) {
        _bucketStates.emplace(bucketId, target.value());
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

void BucketStateManager::_cleanClearRegistry() {
    // An edge case occurs when the count map is empty. In this case, we can clean the whole clear
    // registry.
    if (_countMap.begin() == _countMap.end()) {
        _clearRegistry.erase(_clearRegistry.begin(), _clearRegistry.end());
        return;
    }

    uint64_t smallestEra = _countMap.begin()->first;
    auto endIt = upper_bound(_clearRegistry.begin(),
                             _clearRegistry.end(),
                             smallestEra,
                             [](uint64_t val, auto kv) { return val < kv.first; });

    _clearRegistry.erase(_clearRegistry.begin(), endIt);
}

}  // namespace mongo::timeseries::bucket_catalog
