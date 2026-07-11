// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/collection_index_usage_tracker.h"

#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"

#include <string_view>
#include <utility>

#include <absl/container/flat_hash_map.h>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

CollectionIndexUsageTracker::CollectionIndexUsageTracker(
    AggregatedIndexUsageTracker* aggregatedIndexUsageTracker, ClockSource* clockSource)
    : _clockSource(clockSource),
      _aggregatedIndexUsageTracker(aggregatedIndexUsageTracker),
      _sharedStats(new CollectionScanStatsStorage()) {
    invariant(_clockSource);
}

void CollectionIndexUsageTracker::recordIndexAccess(std::string_view indexName) const {
    tassert(11122205,
            "CollectionIndexUsageTracker::recordIndexAccess invoked with an empty indexname",
            !indexName.empty());

    auto it = _indexUsageStatsMap.find(indexName);

    // The index is guaranteed to be tracked
    tassert(11122206,
            str::stream() << "Index '" << indexName
                          << "' is not registered in CollectionIndexUsageTracker",
            it != _indexUsageStatsMap.end());

    _aggregatedIndexUsageTracker->onAccess(it->second->features);

    // Increment the index usage atomic counter.
    it->second->accesses.fetchAndAdd(1);
}

void CollectionIndexUsageTracker::recordCollectionScans(unsigned long long collectionScans) const {
    // Skip the atomic RMW when there is nothing to add. For index-only reads this value is always
    // zero, and `fetchAndAdd(0)` still emits a hardware exclusive-store on ARM64, invalidating the
    // cache line across all cores sharing `_sharedStats`.
    if (collectionScans > 0) {
        _sharedStats->_collectionScans.fetchAndAdd(collectionScans);
    }
}

void CollectionIndexUsageTracker::recordCollectionScansNonTailable(
    unsigned long long collectionScansNonTailable) const {
    if (collectionScansNonTailable > 0) {
        _sharedStats->_collectionScansNonTailable.fetchAndAdd(collectionScansNonTailable);
    }
}

void CollectionIndexUsageTracker::registerIndex(std::string_view indexName,
                                                const BSONObj& indexKey,
                                                const IndexFeatures& features) {
    tassert(11122207,
            "CollectionIndexUsageTracker::registerIndex invoked with an empty indexname",
            !indexName.empty());

    // Create the map entry.
    auto inserted = _indexUsageStatsMap.try_emplace(
        indexName, make_intrusive<IndexUsageStats>(_clockSource->now(), indexKey, features));
    tassert(11122208,
            str::stream()
                << "CollectionIndexUsageTracker::registerIndex has already been invoked for index '"
                << indexName << "'",
            inserted.second);

    _aggregatedIndexUsageTracker->onRegister(inserted.first->second->features);
}

void CollectionIndexUsageTracker::unregisterIndex(std::string_view indexName) {
    tassert(11122209,
            "CollectionIndexUsageTracker::unregisterIndex invoked with an empty indexname",
            !indexName.empty());

    auto it = _indexUsageStatsMap.find(indexName);
    // Only finished/ready indexes are tracked and this function may be called for an unfinished
    // index. When that happens there is nothing we need to do.
    if (it == _indexUsageStatsMap.end()) {
        return;
    }

    _aggregatedIndexUsageTracker->onUnregister(it->second->features);

    // Remove the map entry.
    _indexUsageStatsMap.erase(it);
}

void CollectionIndexUsageTracker::recordCollectionIndexUsage(
    long long collectionScans,
    long long collectionScansNonTailable,
    const std::set<std::string>& indexesUsed) const {
    recordCollectionScans(collectionScans);
    recordCollectionScansNonTailable(collectionScansNonTailable);

    // Record indexes used to fulfill query.
    for (auto it = indexesUsed.begin(); it != indexesUsed.end(); ++it) {
        recordIndexAccess(*it);
    }
}

const CollectionIndexUsageTracker::CollectionIndexUsageMap&
CollectionIndexUsageTracker::getUsageStats() const {
    return _indexUsageStatsMap;
}

CollectionIndexUsageTracker::CollectionScanStats
CollectionIndexUsageTracker::getCollectionScanStats() const {
    return {_sharedStats->_collectionScans.load(),
            _sharedStats->_collectionScansNonTailable.load()};
}
}  // namespace mongo
