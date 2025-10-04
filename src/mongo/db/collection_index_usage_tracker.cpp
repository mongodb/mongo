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

#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"

#include <utility>

#include <absl/container/flat_hash_map.h>
#include <absl/meta/type_traits.h>
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

void CollectionIndexUsageTracker::recordIndexAccess(StringData indexName) const {
    invariant(!indexName.empty());

    auto it = _indexUsageStatsMap.find(indexName);

    // The index is guaranteed to be tracked
    invariant(it != _indexUsageStatsMap.end());

    _aggregatedIndexUsageTracker->onAccess(it->second->features);

    // Increment the index usage atomic counter.
    it->second->accesses.fetchAndAdd(1);
}

void CollectionIndexUsageTracker::recordCollectionScans(unsigned long long collectionScans) const {
    _sharedStats->_collectionScans.fetchAndAdd(collectionScans);
}

void CollectionIndexUsageTracker::recordCollectionScansNonTailable(
    unsigned long long collectionScansNonTailable) const {
    _sharedStats->_collectionScansNonTailable.fetchAndAdd(collectionScansNonTailable);
}

void CollectionIndexUsageTracker::registerIndex(StringData indexName,
                                                const BSONObj& indexKey,
                                                const IndexFeatures& features) {
    invariant(!indexName.empty());

    // Create the map entry.
    auto inserted = _indexUsageStatsMap.try_emplace(
        indexName, make_intrusive<IndexUsageStats>(_clockSource->now(), indexKey, features));
    invariant(inserted.second);

    _aggregatedIndexUsageTracker->onRegister(inserted.first->second->features);
}

void CollectionIndexUsageTracker::unregisterIndex(StringData indexName) {
    invariant(!indexName.empty());

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
