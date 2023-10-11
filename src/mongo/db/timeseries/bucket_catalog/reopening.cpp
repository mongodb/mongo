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

#include "mongo/db/timeseries/bucket_catalog/reopening.h"

#include <cstddef>

#include <absl/container/node_hash_map.h>

#include "mongo/db/timeseries/bucket_catalog/bucket_catalog.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog_internal.h"
#include "mongo/db/timeseries/bucket_catalog/execution_stats.h"
#include "mongo/util/time_support.h"

namespace mongo::timeseries::bucket_catalog {

ReopeningContext::~ReopeningContext() {
    if (!_cleared) {
        clear();
    }
}

ReopeningContext::ReopeningContext(BucketCatalog& catalog,
                                   Stripe& s,
                                   WithLock,
                                   const BucketKey& k,
                                   uint64_t era,
                                   CandidateType&& c)
    : catalogEra{era}, candidate{std::move(c)}, _stripe(&s), _key(k), _cleared(false) {
    invariant(!_stripe->outstandingReopeningRequests.contains(_key));
    _stripe->outstandingReopeningRequests.emplace(
        _key,
        std::make_shared<ReopeningRequest>(
            ExecutionStatsController{internal::getOrInitializeExecutionStats(catalog, _key.ns)}));
}

ReopeningContext::ReopeningContext(ReopeningContext&& other)
    : catalogEra{other.catalogEra},
      candidate{std::move(other.candidate)},
      fetchedBucket{other.fetchedBucket},
      queriedBucket{other.queriedBucket},
      bucketToReopen{std::move(other.bucketToReopen)},
      _stripe(other._stripe),
      _key(std::move(other._key)),
      _cleared(other._cleared) {
    other._cleared = true;
}

ReopeningContext& ReopeningContext::operator=(ReopeningContext&& other) {
    if (this != &other) {
        _stripe = other._stripe;
        _key = other._key;
        _cleared = other._cleared;
        other._cleared = true;
    }
    return *this;
}

void ReopeningContext::clear() {
    stdx::lock_guard stripeLock{_stripe->mutex};
    clear(stripeLock);
}

void ReopeningContext::clear(WithLock) {
    auto it = _stripe->outstandingReopeningRequests.find(_key);
    invariant(it != _stripe->outstandingReopeningRequests.end());

    // Notify any waiters and clean up state.
    it->second->promise.emplaceValue();
    _stripe->outstandingReopeningRequests.erase(it);
    _cleared = true;
}

ArchivedBucket::ArchivedBucket(const BucketId& b, const std::string& t)
    : bucketId{b}, timeField{t} {}

long long marginalMemoryUsageForArchivedBucket(
    const ArchivedBucket& bucket, IncludeMemoryOverheadFromMap includeMemoryOverheadFromMap) {
    return sizeof(Date_t) +        // key in set of archived buckets for meta hash
        sizeof(ArchivedBucket) +   // main data for archived bucket
        bucket.timeField.size() +  // allocated space for timeField string, ignoring SSO
        (includeMemoryOverheadFromMap == IncludeMemoryOverheadFromMap::kInclude
             ? sizeof(std::size_t) +                                    // key in set (meta hash)
                 sizeof(decltype(Stripe::archivedBuckets)::value_type)  // set container
             : 0);
}

ReopeningRequest::ReopeningRequest(ExecutionStatsController&& s) : stats{std::move(s)} {}

void waitForReopeningRequest(ReopeningRequest& request) {
    if (!request.promise.getFuture().isReady()) {
        request.stats.incNumWaits();
    }
    request.promise.getFuture().getNoThrow().ignore();
}

}  // namespace mongo::timeseries::bucket_catalog
