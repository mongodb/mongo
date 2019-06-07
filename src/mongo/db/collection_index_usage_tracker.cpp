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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/base/counter.h"
#include "mongo/db/collection_index_usage_tracker.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {
Counter64 collectionScansCounter;
Counter64 collectionScansNonTailableCounter;

ServerStatusMetricField<Counter64> displayCollectionScans("queryExecutor.collectionScans.total",
                                                          &collectionScansCounter);
ServerStatusMetricField<Counter64> displayCollectionScansNonTailable(
    "queryExecutor.collectionScans.nonTailable", &collectionScansNonTailableCounter);
}

CollectionIndexUsageTracker::CollectionIndexUsageTracker(ClockSource* clockSource)
    : _clockSource(clockSource) {
    invariant(_clockSource);
}

void CollectionIndexUsageTracker::recordIndexAccess(StringData indexName) {
    invariant(!indexName.empty());
    dassert(_indexUsageMap.find(indexName) != _indexUsageMap.end());

    _indexUsageMap[indexName].accesses.fetchAndAdd(1);
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

void CollectionIndexUsageTracker::registerIndex(StringData indexName, const BSONObj& indexKey) {
    invariant(!indexName.empty());
    dassert(_indexUsageMap.find(indexName) == _indexUsageMap.end());

    // Create map entry.
    _indexUsageMap[indexName] = IndexUsageStats(_clockSource->now(), indexKey);
}

void CollectionIndexUsageTracker::unregisterIndex(StringData indexName) {
    invariant(!indexName.empty());

    _indexUsageMap.erase(indexName);
}

CollectionIndexUsageMap CollectionIndexUsageTracker::getUsageStats() const {
    return _indexUsageMap;
}

CollectionIndexUsageTracker::CollectionScanStats
CollectionIndexUsageTracker::getCollectionScanStats() const {
    return {_collectionScans.load(), _collectionScansNonTailable.load()};
}
}  // namespace mongo
