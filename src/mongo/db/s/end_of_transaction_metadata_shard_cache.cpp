// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/end_of_transaction_metadata_shard_cache.h"

#include "mongo/db/service_context.h"

#include <mutex>
#include <shared_mutex>

namespace mongo {

namespace {
const auto getEndOfTransactionMetadataShardCache =
    ServiceContext::declareDecoration<EndOfTransactionMetadataShardCache>();
}  // namespace

EndOfTransactionMetadataShardCache& EndOfTransactionMetadataShardCache::get(
    ServiceContext* service) {
    return getEndOfTransactionMetadataShardCache(service);
}

Date_t EndOfTransactionMetadataShardCache::_now() const {
    return getEndOfTransactionMetadataShardCache.owner(*this).getPreciseClockSource()->now();
}

bool EndOfTransactionMetadataShardCache::isShardDefinitelyEligible(const ShardId& shardId) const {
    std::shared_lock lock(_mutex);  // NOLINT
    const auto it = _ineligibleUntil.find(shardId);
    if (it == _ineligibleUntil.end()) {
        return true;
    }
    return it->second <= _now();
}

void EndOfTransactionMetadataShardCache::markShardIneligible(const ShardId& shardId) {
    const auto expiry = _now() + kIneligibilityTtl;
    std::lock_guard lock(_mutex);
    _ineligibleUntil.insert_or_assign(shardId, expiry);
}

}  // namespace mongo
