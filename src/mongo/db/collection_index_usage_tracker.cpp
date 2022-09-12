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


#include "mongo/db/collection_index_usage_tracker.h"

#include <atomic>

#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {
namespace {
CounterMetric collectionScansCounter("queryExecutor.collectionScans.total");
CounterMetric collectionScansNonTailableCounter("queryExecutor.collectionScans.nonTailable");
}  // namespace

CollectionIndexUsageTracker::CollectionIndexUsageTracker(
    AggregatedIndexUsageTracker* aggregatedIndexUsageTracker, ClockSource* clockSource)
    : _indexUsageStatsMap(std::make_shared<CollectionIndexUsageMap>()),
      _clockSource(clockSource),
      _aggregatedIndexUsageTracker(aggregatedIndexUsageTracker) {
    invariant(_clockSource);
}

void CollectionIndexUsageTracker::recordIndexAccess(StringData indexName) {
    invariant(!indexName.empty());

    // The following update after fetching the map can race with the removal of this index entry
    // from the map. However, that race is inconsequential and remains memory safe.
    auto mapSharedPtr = atomic_load(&_indexUsageStatsMap);

    auto it = mapSharedPtr->find(indexName);
    if (it == mapSharedPtr->end()) {
        // We are using an index that has been removed from the catalog, no need to track usage
        return;
    }

    _aggregatedIndexUsageTracker->onAccess(it->second->features);

    // Increment the index usage atomic counter.
    it->second->accesses.fetchAndAdd(1);
}

void CollectionIndexUsageTracker::recordCollectionScans(unsigned long long collectionScans) {
    _collectionScans.fetchAndAdd(collectionScans);
    collectionScansCounter.increment(collectionScans);
}

void CollectionIndexUsageTracker::recordCollectionScansNonTailable(
    unsigned long long collectionScansNonTailable) {
    _collectionScansNonTailable.fetchAndAdd(collectionScansNonTailable);
    collectionScansNonTailableCounter.increment(collectionScansNonTailable);
}

void CollectionIndexUsageTracker::registerIndex(StringData indexName,
                                                const BSONObj& indexKey,
                                                const IndexFeatures& features) {
    invariant(!indexName.empty());

    // Create a copy of the map to modify.
    auto mapSharedPtr = atomic_load(&_indexUsageStatsMap);
    auto mapCopy = std::make_shared<CollectionIndexUsageMap>(*mapSharedPtr);

    dassert(mapCopy->find(indexName) == mapCopy->end());

    // Create the map entry.
    auto inserted = mapCopy->try_emplace(
        indexName, make_intrusive<IndexUsageStats>(_clockSource->now(), indexKey, features));
    invariant(inserted.second);

    _aggregatedIndexUsageTracker->onRegister(inserted.first->second->features);

    // Swap the modified map into place atomically.
    atomic_store(&_indexUsageStatsMap, std::move(mapCopy));
}

void CollectionIndexUsageTracker::unregisterIndex(StringData indexName) {
    invariant(!indexName.empty());

    // Create a copy of the map to modify.
    auto mapSharedPtr = atomic_load(&_indexUsageStatsMap);
    auto mapCopy = std::make_shared<CollectionIndexUsageMap>(*mapSharedPtr);

    auto it = mapCopy->find(indexName);
    if (it != mapCopy->end()) {
        _aggregatedIndexUsageTracker->onUnregister(it->second->features);

        // Remove the map entry.
        mapCopy->erase(it);

        // Swap the modified map into place atomically.
        atomic_store(&_indexUsageStatsMap, std::move(mapCopy));
    }
}

std::shared_ptr<CollectionIndexUsageTracker::CollectionIndexUsageMap>
CollectionIndexUsageTracker::getUsageStats() const {
    return atomic_load(&_indexUsageStatsMap);
}

CollectionIndexUsageTracker::CollectionScanStats
CollectionIndexUsageTracker::getCollectionScanStats() const {
    return {_collectionScans.load(), _collectionScansNonTailable.load()};
}
}  // namespace mongo
