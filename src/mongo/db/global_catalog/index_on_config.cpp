// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/global_catalog/index_on_config.h"

#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_namespace_placement_gen.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

std::vector<IndexSpec_ForCatalog> getChunkCollectionIndexSpecs() {
    return {
        {BSON(ChunkType::collectionUUID() << 1 << ChunkType::min() << 1), true /* unique */},
        {BSON(ChunkType::collectionUUID() << 1 << ChunkType::shard() << 1 << ChunkType::min() << 1),
         true /* unique */},
        {BSON(ChunkType::collectionUUID() << 1 << ChunkType::lastmod() << 1), true /* unique */},
        {BSON(ChunkType::collectionUUID()
              << 1 << ChunkType::shard() << 1 << ChunkType::onCurrentShardSince() << 1),
         false /* unique */},
    };
}

std::vector<IndexSpec_ForCatalog> getPlacementHistoryCollectionIndexSpecs() {
    return {
        // Create a combined index on 'nss' (sorted ascending) and 'timestamp' (sorted descending).
        {BSON(NamespacePlacementType::kNssFieldName
              << 1 << NamespacePlacementType::kTimestampFieldName << -1),
         true /* unique */},
        // Create another index with 'timestamp' first (sorted descending), then 'nss' (sorted
        // ascending). This is necessary to cover queries to the placement history that are querying
        // by time range. Note that this index does not need to be unique, as the uniqueness of
        // every {timestamp, nss} combination is already ensured by the first index.
        {BSON(NamespacePlacementType::kTimestampFieldName
              << -1 << NamespacePlacementType::kNssFieldName << 1),
         false /* unique */},
    };
}

}  // namespace mongo
