// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/timeseries/bucket_catalog/reopening.h"

#include "mongo/db/timeseries/bucket_catalog/bucket_catalog.h"
#include "mongo/db/timeseries/bucket_catalog/bucket_catalog_internal.h"
#include "mongo/db/timeseries/bucket_catalog/execution_stats.h"

#include <absl/container/node_hash_map.h>

namespace mongo::timeseries::bucket_catalog {

ReopeningScope::ReopeningScope(BucketCatalog& catalog,
                               Stripe& stripe,
                               WithLock stripeLock,
                               const BucketKey& key,
                               const CandidateType& candidate)
    : _stripe(&stripe), _key(key) {
    if (auto* c = get_if<OID>(&candidate)) {
        _oid = *c;
    }
    // Only one query-based reopening can exist for 'key'.
    invariant(_oid.has_value() || !stripe.outstandingReopeningRequests.contains(_key));

    auto it = stripe.outstandingReopeningRequests.find(_key);
    if (it == stripe.outstandingReopeningRequests.end()) {
        bool inserted = false;
        // Track the memory usage for the bucket keys in this data structure because these buckets
        // are not open yet which means they are not already being tracked.
        std::tie(it, inserted) = stripe.outstandingReopeningRequests.try_emplace(
            key,
            tracking::make_inlined_vector<tracking::shared_ptr<ReopeningRequest>,
                                          Stripe::kInlinedVectorSize>(
                getTrackingContext(catalog.trackingContexts, TrackingScope::kReopeningRequests)));
        invariant(inserted);
    }
    auto& list = it->second;

    list.push_back(tracking::make_shared<ReopeningRequest>(
        getTrackingContext(catalog.trackingContexts, TrackingScope::kReopeningRequests),
        ExecutionStatsController{
            internal::getOrInitializeExecutionStats(catalog, key.collectionUUID)},
        _oid));
}

// When the RAII type is destructed, '_stripe' should be locked.
ReopeningScope::~ReopeningScope() {
    auto keyIt = _stripe->outstandingReopeningRequests.find(_key);
    invariant(keyIt != _stripe->outstandingReopeningRequests.end());
    auto& list = keyIt->second;

    // Only one query-based reopening can exist for 'key'.
    invariant(_oid.has_value() || list.size() == 1);
    auto requestIt = std::find_if(
        list.begin(), list.end(), [&](const std::shared_ptr<ReopeningRequest>& request) {
            return request->oid == _oid;
        });
    invariant(requestIt != list.end());

    // Notify any waiters and clean up state.
    (*requestIt)->promise.emplaceValue();
    list.erase(requestIt);
    if (list.empty()) {
        _stripe->outstandingReopeningRequests.erase(keyIt);
    }
}

ReopeningRequest::ReopeningRequest(ExecutionStatsController&& s, boost::optional<OID> o)
    : stats{std::move(s)}, oid{o} {}

void waitForReopeningRequest(ReopeningRequest& request) {
    if (!request.promise.getFuture().isReady()) {
        request.stats.incNumWaits();
    }
    request.promise.getFuture().getNoThrow().ignore();
}

}  // namespace mongo::timeseries::bucket_catalog
