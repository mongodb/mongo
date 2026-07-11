// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/scoped_collection_metadata.h"

namespace mongo {

bool ScopedCollectionFilter::isRangeEntirelyOwned(const BSONObj& min,
                                                  const BSONObj& max,
                                                  bool includeMaxBound) const {
    const auto cm = _impl->get().getChunkManager();
    if (!cm->hasRoutingTable() || cm->isUnsplittable())
        // Unsharded collection are always placed in only one shard
        return true;
    std::set<ShardId> shardIds;
    cm->getShardIdsForRange(min, max, &shardIds, nullptr, includeMaxBound);
    return shardIds.size() == 1;
}
}  // namespace mongo
